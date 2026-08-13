/*
 * mock_efi_test.c — HOST failure-path test harness for the EFI network provider (F7b).
 *
 * The load-bearing verification for the F8 hardening: QEMU happy-path runs never
 * exercise the EFI completion-token LIFETIME failure paths (timeouts, aborts, EBS), so
 * this harness fakes the firmware in memory and scripts those paths.
 *
 * It provides a scriptable fake EFI_BOOT_SERVICES + EFI_TCP4_PROTOCOL +
 * EFI_UDP4_PROTOCOL + EFI_SERVICE_BINDING_PROTOCOL, then drives the REAL provider TUs
 * (socket_efi_tcp4.c, dns_uefi.c, event_efi.c — compiled host-side against these mock
 * headers) and asserts the F1/F2/F3/F6 fixes hold: every token reaches exactly one
 * terminal state (complete OR cancel-and-drained), close() reconciles outstanding
 * tokens, post-EBS every entry refuses without a firmware call, and the stale-guard
 * reads stable slot storage (no UAF).
 *
 * Built + run host-side under ASan+UBSan by build_mock_efi_test.sh. The token-lifetime
 * invariant is checked structurally: the mock tracks, per token, whether it is
 * outstanding, and asserts NO token is left outstanding at teardown.
 *
 * Exit 0 = all pass; non-zero = a failure was printed.
 */

#include "socket_efi_tcp4.h"
#include "socket_efi_udp4.h"              /* the 6.4b datagram provider under test */
#include "../../spikes/uefi/efi_udp4.h"   /* EFI_UDP4_* types for the UDP mock */
#include "dns_uefi.h"
#include "event_efi.h"
#include "civil_time.h"                   /* cert validity-time conversion (U-8) */
#include "wallclock_uefi.h"               /* EFI_TIME decode + unspecified-TZ policy (U-8) */
#include "clock_snapshot.h"               /* TLS-platform-lifetime snapshot clock gate (U-8) */

#include <keel/sockaddr.h>
#include <keel/udp.h>                /* KlUdpConfig + KlDatagram layout (6.4b) */
#include <keel/allocator.h>          /* KlAllocator (KlDgramLife tests) */
#include "../../src/datagram_life.h" /* KlDgramLife create/retain/release/mark_dead (6.4b-3b) */
#include "../../src/socket.h"        /* KlSocketProvider, KlSocketOps, kl_handle_valid */
#include "../../src/completion.h"    /* KlCompletionOps, KlCompletionEvent, KL_COMP_* */
#include <keel/event.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* connect_now is exported by socket_efi_tcp4.c but not in the public header. */
int kl_uefi_socket_connect_now(KlSocketHandle fd);
/* Reach the completion ops behind the opaque KlEventOps.completion (const void*). */
#define COMP(ep) ((const KlCompletionOps *)(ep)->ops->completion)

/* ── test bookkeeping ───────────────────────────────────────────────────────── */
static int g_fail = 0;
static const char *g_case = "?";
#define T_CASE(name) do { g_case = (name); printf("\n=== %s ===\n", (name)); } while (0)
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { printf("  FAIL [%s]: %s\n", g_case, (msg)); g_fail = 1;}\
    else         { printf("  ok:   %s\n", (msg)); }                        \
} while (0)

/* ── controllable EBS flag (the F3 fail-closed guard reads this) ────────────────
 * socket_efi_tcp4.c / dns_uefi.c / event_efi.c reference kl_uefi_after_ebs(); its real
 * definition lives in platform_uefi.c (not linked here). The harness owns it so we can
 * script the post-EBS path. */
static int g_after_ebs = 0;
int kl_uefi_after_ebs(void);
int kl_uefi_after_ebs(void) { return g_after_ebs; }

#include <stddef.h>
/* kl_plat_random + kl_monotonic_ms (src/platform.h) are provided by libkeel.a's host platform
 * TU — pulled in once clock_snapshot.c references kl_monotonic_ms. We do NOT stub them here
 * (that would duplicate the symbols); the DNS query id being host-random is fine (the DNS mock
 * tests exercise timeout/quarantine, not a successful id-matched parse). */

/* ── controllable kl_uefi_wallclock (U-8) ───────────────────────────────────────
 * clock_snapshot.c calls the REAL kl_uefi_wallclock (platform_uefi.c, GetTime) — not linked
 * here — so the harness provides a scriptable one to drive the snapshot gate: a successful
 * clock (g_wc_fail=0 → returns g_wc_val) or an untrustworthy one (g_wc_fail=1 → -1). This lets
 * the mock exercise snapshot-succeeds and snapshot-refuses end-to-end. */
static int     g_wc_fail = 0;
static int64_t g_wc_val  = 1780272000LL;   /* 2026-06-01T00:00:00Z */
int kl_uefi_wallclock(int64_t *out_unix);
int kl_uefi_wallclock(int64_t *out_unix) {
    if (g_wc_fail) return -1;
    if (out_unix) *out_unix = g_wc_val;
    return 0;
}

/* clock_snapshot.c refuses to capture unless the platform is READY (monotonic timer armed) —
 * kl_uefi_platform_ready() lives in platform_uefi.c (not linked here), so the harness provides a
 * scriptable one to drive the "timer failed -> snapshot must fail" case. Default: ready. */
static int g_plat_ready = 1;
int kl_uefi_platform_ready(void);
int kl_uefi_platform_ready(void) { return g_plat_ready; }

/* ── mock event objects ─────────────────────────────────────────────────────────
 * An EFI_EVENT in the mock is a pointer to MockEvent. CheckEvent(EFI_SUCCESS) iff
 * `signaled`. The token layer sets `signaled` by scripting; Cancel() signals with
 * EFI_ABORTED as the firmware would. */
typedef struct {
    int signaled;
    int closed;       /* CloseEvent called */
    int alive;        /* CreateEvent'd, not yet closed */
} MockEvent;

#define MAX_EVENTS 64
static MockEvent g_events[MAX_EVENTS];
static int       g_event_count;

/* ── scriptable token behavior ──────────────────────────────────────────────────
 * For each protocol op (Connect/Transmit/Receive/Close for TCP; Transmit/Receive for
 * UDP) we can script whether submitting the token immediately completes (signals its
 * event with a chosen status) or HANGS (leaves the event unsignaled → the provider's
 * pump times out → pump_or_cancel must Cancel+drain). */
typedef enum { TOK_COMPLETE_OK, TOK_HANG } TokMode;

static TokMode  g_tcp_connect_mode  = TOK_COMPLETE_OK;
static TokMode  g_tcp_transmit_mode = TOK_COMPLETE_OK;
static TokMode  g_tcp_receive_mode  = TOK_COMPLETE_OK;
static TokMode  g_tcp_close_mode    = TOK_COMPLETE_OK;
static TokMode  g_udp_transmit_mode = TOK_COMPLETE_OK;
static TokMode  g_udp_receive_mode  = TOK_COMPLETE_OK;

/* Adversarial scripting for the release-blocker paths:
 *  - g_cancel_signals=0 models a firmware whose Cancel does NOT retire the token
 *    (the cancel-drain then FAILS → the provider must QUARANTINE, never free).
 *  - g_tcp_poll_calls counts Poll() so a test can assert close() does NOT spin on a
 *    token whose terminal signal was already consumed (finding 2).
 *  - g_tcp_rx_datalen_override (>0) forces an impossible received DataLength (finding 4). */
static int      g_cancel_signals = 1;
static int      g_tcp_poll_calls = 0;
static UINT32   g_tcp_rx_datalen_override = 0;

/* ── firmware-call counters + token-lifetime tracking ──────────────────────────── */
static int g_tcp_cancel_calls;       /* Cancel(This, token != NULL) */
static int g_tcp_cancel_all_calls;   /* Cancel(This, NULL) */
static int g_udp_cancel_calls;       /* Cancel(This, token != NULL) */
static int g_udp_cancel_all_calls;   /* Cancel(This, NULL) — udp close cancels all at once */
static int g_destroy_child_calls;
static int g_close_proto_calls;
static int g_free_pool_calls;
static int g_configure_null_calls;   /* Configure(This, NULL) — teardown reset */
static int g_recycle_signals;        /* SignalEvent on an RxData RecycleSignal */

/* Any boot-service or protocol call bumps this — the EBS tests assert it stays 0. */
static int g_firmware_calls;
#define FW() (g_firmware_calls++)

/* Per-token outstanding tracker: a token whose event was created but never reached a
 * terminal (signaled) state is "outstanding". The token-lifetime rule says NONE may be
 * outstanding once the provider returns / closes. We approximate this by tracking each
 * submitted token's event and asserting it is signaled at the assertion points. */
typedef struct {
    void *token;   /* the completion-token pointer submitted */
    MockEvent *ev; /* its event */
    int outstanding;
} TokRec;
#define MAX_TOKS 64
static TokRec g_toks[MAX_TOKS];
static int    g_tok_count;

static void tok_submit(void *token, MockEvent *ev) {
    /* update if already tracked (tokens are reused across ops) */
    for (int i = 0; i < g_tok_count; i++)
        if (g_toks[i].token == token) { g_toks[i].ev = ev; g_toks[i].outstanding = 1; return; }
    if (g_tok_count < MAX_TOKS) {
        g_toks[g_tok_count].token = token;
        g_toks[g_tok_count].ev = ev;
        g_toks[g_tok_count].outstanding = 1;
        g_tok_count++;
    }
}
static void tok_terminal(void *token) {
    for (int i = 0; i < g_tok_count; i++)
        if (g_toks[i].token == token) { g_toks[i].outstanding = 0; return; }
}
static int outstanding_count(void) {
    int n = 0;
    for (int i = 0; i < g_tok_count; i++) if (g_toks[i].outstanding) n++;
    return n;
}
static void tok_reset(void) { g_tok_count = 0; }

/* ── S-5 server accept: armed Accept (listen) tokens + inbound-connection delivery ──
 * m_tcp_Accept ARMS a listen token (records it, leaves it pending) — a connection
 * "arrives" only when a test calls mock_deliver_connection(), which signals one armed
 * token with a fresh distinct child handle (mirroring the real async accept). */
#define MAX_ACCEPT_TOKS 8
static EFI_TCP4_LISTEN_TOKEN *g_accept_toks[MAX_ACCEPT_TOKS];
static int g_accept_tok_count;
static int g_accept_child_seq;                 /* fresh child-handle generator */
static int g_accept_child_objs[MAX_ACCEPT_TOKS + 8];  /* backing storage for child EFI_HANDLEs */

static void accept_reset(void) {
    for (int i = 0; i < MAX_ACCEPT_TOKS; i++) g_accept_toks[i] = NULL;
    g_accept_tok_count = 0;
    g_accept_child_seq = 0;
}

/* Simulate one inbound connection: signal the first armed-but-undelivered Accept
 * token with a fresh child handle. Returns 1 if a connection was delivered. */
static int mock_deliver_connection(void) {
    for (int i = 0; i < g_accept_tok_count; i++) {
        EFI_TCP4_LISTEN_TOKEN *lt = g_accept_toks[i];
        if (!lt) continue;
        MockEvent *e = (MockEvent *)lt->CompletionToken.Event;
        if (!e || e->signaled) continue;   /* already delivered / not yet consumed */
        lt->NewChildHandle = (EFI_HANDLE)&g_accept_child_objs[g_accept_child_seq++];
        lt->CompletionToken.Status = EFI_SUCCESS;
        e->signaled = 1;
        tok_terminal(&lt->CompletionToken);
        return 1;
    }
    return 0;
}

/* ── EFIAPI is empty on the host (non-x86 or gated); use plain funcs ───────────── */

/* ── mock boot services ─────────────────────────────────────────────────────────
 * We need one EFI_SERVICE_BINDING_PROTOCOL per (TCP4, UDP4) and one child EFI_TCP4/
 * UDP4 protocol vtable each. LocateHandleBuffer returns a 1-handle array; HandleProtocol
 * returns the matching SB. */

static EFI_BOOT_SERVICES        g_bs;
static EFI_SERVICE_BINDING_PROTOCOL g_tcp_sb;
static EFI_SERVICE_BINDING_PROTOCOL g_udp_sb;
static EFI_TCP4_PROTOCOL         g_tcp;
static EFI_UDP4_PROTOCOL         g_udp;

/* fake handles */
static int g_tcp_sb_handle_obj;   /* address used as an EFI_HANDLE */
static int g_udp_sb_handle_obj;
static int g_tcp_child_obj;
static int g_udp_child_obj;

/* GUID compare */
static int guid_eq(EFI_GUID *a, EFI_GUID *b) {
    return memcmp(a, b, sizeof(EFI_GUID)) == 0;
}
static EFI_GUID G_TCP_SB = EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID;
static EFI_GUID G_TCP    = EFI_TCP4_PROTOCOL_GUID;
static EFI_GUID G_UDP_SB = EFI_UDP4_SERVICE_BINDING_PROTOCOL_GUID;
static EFI_GUID G_UDP    = EFI_UDP4_PROTOCOL_GUID;

/* --- boot services ops --- */
static EFI_STATUS EFIAPI m_AllocatePool(UINTN t, UINTN sz, VOID **buf) {
    (void)t; FW(); *buf = malloc(sz ? sz : 1); return *buf ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI m_FreePool(VOID *buf) { FW(); g_free_pool_calls++; free(buf); return EFI_SUCCESS; }

static EFI_STATUS EFIAPI m_CreateEvent(UINT32 type, UINTN tpl, VOID *nf, VOID *nc, EFI_EVENT *ev) {
    (void)type; (void)tpl; (void)nf; (void)nc; FW();
    if (g_event_count >= MAX_EVENTS) return EFI_OUT_OF_RESOURCES;
    MockEvent *e = &g_events[g_event_count++];
    e->signaled = 0; e->closed = 0; e->alive = 1;
    *ev = (EFI_EVENT)e;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_CloseEvent(EFI_EVENT ev) {
    FW();
    MockEvent *e = (MockEvent *)ev;
    if (e) { e->closed = 1; e->alive = 0; }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_CheckEvent(EFI_EVENT ev) {
    FW();
    MockEvent *e = (MockEvent *)ev;
    /* Real EFI CheckEvent CONSUMES a signaled event (returns SUCCESS once, then
     * de-signals). Modelling this faithfully is what lets the harness catch a caller
     * that CheckEvents a token whose terminal signal was ALREADY consumed (finding 2:
     * close draining a connect token that a prior CheckEvent already retired). */
    if (e && e->signaled) { e->signaled = 0; return EFI_SUCCESS; }
    return EFI_NOT_READY;
}
static EFI_STATUS EFIAPI m_SignalEvent(EFI_EVENT ev) {
    FW();
    MockEvent *e = (MockEvent *)ev;
    if (e) { e->signaled = 1; g_recycle_signals++; }   /* only RxData recycle uses this */
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_Stall(UINTN us) { (void)us; FW(); return EFI_SUCCESS; }

static EFI_STATUS EFIAPI m_HandleProtocol(EFI_HANDLE h, EFI_GUID *g, VOID **iface) {
    (void)h; FW();
    if (guid_eq(g, &G_TCP_SB)) { *iface = &g_tcp_sb; return EFI_SUCCESS; }
    if (guid_eq(g, &G_UDP_SB)) { *iface = &g_udp_sb; return EFI_SUCCESS; }
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI m_LocateHandleBuffer(EFI_LOCATE_SEARCH_TYPE t, EFI_GUID *g,
                                              VOID *key, UINTN *n, EFI_HANDLE **buf) {
    (void)t; (void)key; FW();
    EFI_HANDLE *arr = malloc(sizeof(EFI_HANDLE));
    if (guid_eq(g, &G_TCP_SB))      arr[0] = (EFI_HANDLE)&g_tcp_sb_handle_obj;
    else if (guid_eq(g, &G_UDP_SB)) arr[0] = (EFI_HANDLE)&g_udp_sb_handle_obj;
    else { free(arr); *n = 0; return EFI_NOT_FOUND; }
    *n = 1; *buf = arr;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_OpenProtocol(EFI_HANDLE h, EFI_GUID *g, VOID **iface,
                                        EFI_HANDLE a, EFI_HANDLE c, UINT32 attr) {
    (void)h; (void)a; (void)c; (void)attr; FW();
    if (guid_eq(g, &G_TCP)) { *iface = &g_tcp; return EFI_SUCCESS; }
    if (guid_eq(g, &G_UDP)) { *iface = &g_udp; return EFI_SUCCESS; }
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI m_CloseProtocol(EFI_HANDLE h, EFI_GUID *g, EFI_HANDLE a, EFI_HANDLE c) {
    (void)h; (void)g; (void)a; (void)c; FW(); g_close_proto_calls++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_LocateProtocol(EFI_GUID *g, VOID *reg, VOID **iface) {
    (void)g; (void)reg; FW(); *iface = NULL; return EFI_NOT_FOUND;
}

/* --- service binding --- */
static EFI_STATUS EFIAPI m_tcp_CreateChild(EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE *ch) {
    (void)This; FW(); *ch = (EFI_HANDLE)&g_tcp_child_obj; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_tcp_DestroyChild(EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE ch) {
    (void)This; (void)ch; FW(); g_destroy_child_calls++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_udp_CreateChild(EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE *ch) {
    (void)This; FW(); *ch = (EFI_HANDLE)&g_udp_child_obj; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_udp_DestroyChild(EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE ch) {
    (void)This; (void)ch; FW(); g_destroy_child_calls++; return EFI_SUCCESS;
}

/* --- TCP4 protocol --- */
static EFI_STATUS EFIAPI m_tcp_GetModeData(EFI_TCP4_PROTOCOL *This, VOID *s, EFI_TCP4_CONFIG_DATA *cd,
                                           VOID *ip, VOID *mnp, VOID *snp) {
    (void)This; (void)s; (void)ip; (void)mnp; (void)snp; FW();
    if (cd) {
        memset(cd, 0, sizeof(*cd));
        /* Fake accepted-peer so accept()'s GetModeData peer extraction yields a real
         * neutral KlSockAddr (10.0.0.9:5555). */
        cd->AccessPoint.RemoteAddress.Addr[0] = 10;
        cd->AccessPoint.RemoteAddress.Addr[3] = 9;
        cd->AccessPoint.RemotePort = 5555;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_tcp_Configure(EFI_TCP4_PROTOCOL *This, EFI_TCP4_CONFIG_DATA *cd) {
    (void)This; FW();
    if (cd == NULL) g_configure_null_calls++;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_tcp_Routes(EFI_TCP4_PROTOCOL *This, BOOLEAN d, EFI_IPv4_ADDRESS *a,
                                      EFI_IPv4_ADDRESS *b, EFI_IPv4_ADDRESS *c) {
    (void)This; (void)d; (void)a; (void)b; (void)c; FW(); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_tcp_Connect(EFI_TCP4_PROTOCOL *This, EFI_TCP4_CONNECTION_TOKEN *t) {
    (void)This; FW();
    MockEvent *e = (MockEvent *)t->CompletionToken.Event;
    tok_submit(&t->CompletionToken, e);
    if (g_tcp_connect_mode == TOK_COMPLETE_OK) {
        t->CompletionToken.Status = EFI_SUCCESS; e->signaled = 1; tok_terminal(&t->CompletionToken);
    }
    return EFI_SUCCESS;   /* submitted */
}
static EFI_STATUS EFIAPI m_tcp_Accept(EFI_TCP4_PROTOCOL *This, VOID *t) {
    (void)This; FW();
    EFI_TCP4_LISTEN_TOKEN *lt = (EFI_TCP4_LISTEN_TOKEN *)t;
    MockEvent *e = (MockEvent *)lt->CompletionToken.Event;
    lt->NewChildHandle = NULL;
    tok_submit(&lt->CompletionToken, e);   /* armed, pending — delivery is explicit */
    for (int i = 0; i < g_accept_tok_count; i++)
        if (g_accept_toks[i] == lt) return EFI_SUCCESS;   /* dedup re-arms of the same token */
    if (g_accept_tok_count < MAX_ACCEPT_TOKS) g_accept_toks[g_accept_tok_count++] = lt;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_tcp_Transmit(EFI_TCP4_PROTOCOL *This, EFI_TCP4_IO_TOKEN *t) {
    (void)This; FW();
    MockEvent *e = (MockEvent *)t->CompletionToken.Event;
    tok_submit(&t->CompletionToken, e);
    if (g_tcp_transmit_mode == TOK_COMPLETE_OK) {
        t->CompletionToken.Status = EFI_SUCCESS; e->signaled = 1; tok_terminal(&t->CompletionToken);
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_tcp_Receive(EFI_TCP4_PROTOCOL *This, EFI_TCP4_IO_TOKEN *t) {
    (void)This; FW();
    MockEvent *e = (MockEvent *)t->CompletionToken.Event;
    tok_submit(&t->CompletionToken, e);
    if (g_tcp_receive_mode == TOK_COMPLETE_OK) {
        /* deliver 4 bytes into the posted RxData buffer */
        EFI_TCP4_RECEIVE_DATA *rx = t->Packet.RxData;
        if (rx && rx->FragmentTable[0].FragmentBuffer) {
            unsigned char *b = rx->FragmentTable[0].FragmentBuffer;
            b[0]='O'; b[1]='K'; b[2]='!'; b[3]='\n';
            /* finding 4: an impossible (over-capacity) DataLength must be rejected, not
             * trusted into rx_len (which would let rx_consume read past rx_buf). */
            rx->DataLength = g_tcp_rx_datalen_override ? g_tcp_rx_datalen_override : 4;
        }
        t->CompletionToken.Status = EFI_SUCCESS; e->signaled = 1; tok_terminal(&t->CompletionToken);
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_tcp_Close(EFI_TCP4_PROTOCOL *This, EFI_TCP4_CLOSE_TOKEN *t) {
    (void)This; FW();
    MockEvent *e = (MockEvent *)t->CompletionToken.Event;
    tok_submit(&t->CompletionToken, e);
    if (g_tcp_close_mode == TOK_COMPLETE_OK) {
        t->CompletionToken.Status = EFI_SUCCESS; e->signaled = 1; tok_terminal(&t->CompletionToken);
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_tcp_Cancel(EFI_TCP4_PROTOCOL *This, EFI_TCP4_COMPLETION_TOKEN *t) {
    (void)This; FW();
    if (t == NULL) {
        g_tcp_cancel_all_calls++;
        if (!g_cancel_signals) return EFI_SUCCESS;   /* firmware can't cancel — leaves tokens live */
        for (int i = 0; i < g_tok_count; i++)
            if (g_toks[i].outstanding && g_toks[i].ev) {
                g_toks[i].ev->signaled = 1;          /* firmware signals the event */
                g_toks[i].outstanding = 0;
            }
        return EFI_SUCCESS;
    }
    g_tcp_cancel_calls++;
    if (!g_cancel_signals) return EFI_NOT_FOUND;     /* token not retired → drain will fail */
    MockEvent *e = (MockEvent *)t->Event;
    if (e) e->signaled = 1;
    t->Status = EFI_ABORTED;
    tok_terminal(t);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_tcp_Poll(EFI_TCP4_PROTOCOL *This) { (void)This; FW(); g_tcp_poll_calls++; return EFI_SUCCESS; }

/* --- UDP4 protocol (6.4b: scriptable session + config for the socket_efi_udp4 tests) --- */
/* Scriptable Configure status (non-NULL cd), a non-NULL Configure counter (single-Configure
 * proof), and captured Rx-session + Tx-session so the provider tests can assert src/dest. */
static EFI_STATUS g_udp_configure_status = EFI_SUCCESS;
static int        g_udp_configure_calls  = 0;   /* non-NULL Configure (station/ephemeral) */
static uint8_t    g_udp_rx_src[4] = { 10, 0, 2, 3 };
static UINT16     g_udp_rx_src_port = 53;
static uint8_t    g_udp_rx_dst[4] = { 10, 0, 2, 15 };
static UINT16     g_udp_rx_dst_port = 49152;
static int        g_udp_tx_calls = 0;
static uint8_t    g_udp_tx_dst[4]; static UINT16 g_udp_tx_dport;
static uint8_t    g_udp_tx_src[4]; static UINT16 g_udp_tx_sport;
static UINT32     g_udp_tx_len;

static EFI_STATUS EFIAPI m_udp_GetModeData(EFI_UDP4_PROTOCOL *This, EFI_UDP4_CONFIG_DATA *cd,
                                           VOID *ip, VOID *mnp, VOID *snp) {
    (void)This; (void)ip; (void)mnp; (void)snp; FW();
    if (cd) {   /* fake a resolved ephemeral station so get_local_addr yields a real addr */
        memset(cd, 0, sizeof(*cd));
        cd->StationAddress.Addr[0] = 10; cd->StationAddress.Addr[2] = 2; cd->StationAddress.Addr[3] = 15;
        cd->StationPort = 49152;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_udp_Configure(EFI_UDP4_PROTOCOL *This, EFI_UDP4_CONFIG_DATA *cd) {
    (void)This; FW();
    if (cd == NULL) { g_configure_null_calls++; return EFI_SUCCESS; }
    g_udp_configure_calls++;
    return g_udp_configure_status;   /* scriptable: force a Configure failure to test fail-close */
}
static EFI_STATUS EFIAPI m_udp_Groups(EFI_UDP4_PROTOCOL *This, BOOLEAN j, EFI_IPv4_ADDRESS *m) {
    (void)This; (void)j; (void)m; FW(); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_udp_Routes(EFI_UDP4_PROTOCOL *This, BOOLEAN d, EFI_IPv4_ADDRESS *a,
                                      EFI_IPv4_ADDRESS *b, EFI_IPv4_ADDRESS *c) {
    (void)This; (void)d; (void)a; (void)b; (void)c; FW(); return EFI_SUCCESS;
}
/* firmware-owned RxData for the receive-OK path */
static unsigned char g_udp_resp[512];
static EFI_UDP4_RECEIVE_DATA g_udp_rxdata;
static MockEvent g_udp_recycle_ev;
/* The last UDP token the firmware ACCEPTED but did not complete (a TOK_HANG). The test uses
 * it to model a DELAYED firmware write into the token AFTER kl_uefi_dns_resolve() returns —
 * the exact condition that catches a stack-local token (write into a returned frame =
 * ASan stack-use-after-return) vs the stable g_dns_op storage (safe). */
static EFI_UDP4_COMPLETION_TOKEN *g_udp_hung_tok;
static EFI_STATUS EFIAPI m_udp_Transmit(EFI_UDP4_PROTOCOL *This, EFI_UDP4_COMPLETION_TOKEN *t) {
    (void)This; FW();
    g_udp_tx_calls++;
    /* Capture the session the provider built (must be fully built BEFORE this call — the
     * source-pin test asserts the SourceAddress landed here, proving no post-submit mutation). */
    EFI_UDP4_TRANSMIT_DATA *tx = t->Packet.TxData;
    if (tx) {
        g_udp_tx_len = tx->DataLength;
        if (tx->UdpSessionData) {
            for (int i = 0; i < 4; i++) g_udp_tx_dst[i] = tx->UdpSessionData->DestinationAddress.Addr[i];
            g_udp_tx_dport = tx->UdpSessionData->DestinationPort;
            for (int i = 0; i < 4; i++) g_udp_tx_src[i] = tx->UdpSessionData->SourceAddress.Addr[i];
            g_udp_tx_sport = tx->UdpSessionData->SourcePort;
        }
    }
    MockEvent *e = (MockEvent *)t->Event;
    tok_submit(t, e);
    if (g_udp_transmit_mode == TOK_COMPLETE_OK) {
        t->Status = EFI_SUCCESS; if (e) e->signaled = 1; tok_terminal(t);
    } else {
        g_udp_hung_tok = t;   /* firmware keeps this token address (will "write late") */
    }
    return EFI_SUCCESS;
}
static size_t g_udp_resp_len;    /* built by the caller before Receive */
static EFI_STATUS EFIAPI m_udp_Receive(EFI_UDP4_PROTOCOL *This, EFI_UDP4_COMPLETION_TOKEN *t) {
    (void)This; FW();
    MockEvent *e = (MockEvent *)t->Event;
    tok_submit(t, e);
    if (g_udp_receive_mode == TOK_COMPLETE_OK) {
        memset(&g_udp_rxdata, 0, sizeof(g_udp_rxdata));
        g_udp_rxdata.RecycleSignal = (EFI_EVENT)&g_udp_recycle_ev;
        g_udp_recycle_ev.signaled = 0;
        /* Per-datagram source + local (dest) — the provider marshals these into the completion
         * event's peer/local. The source is the resolver's anti-spoof discriminator. */
        for (int i = 0; i < 4; i++) g_udp_rxdata.UdpSession.SourceAddress.Addr[i] = g_udp_rx_src[i];
        g_udp_rxdata.UdpSession.SourcePort = g_udp_rx_src_port;
        for (int i = 0; i < 4; i++) g_udp_rxdata.UdpSession.DestinationAddress.Addr[i] = g_udp_rx_dst[i];
        g_udp_rxdata.UdpSession.DestinationPort = g_udp_rx_dst_port;
        g_udp_rxdata.DataLength = (UINT32)g_udp_resp_len;
        g_udp_rxdata.FragmentCount = 1;
        g_udp_rxdata.FragmentTable[0].FragmentLength = (UINT32)g_udp_resp_len;
        g_udp_rxdata.FragmentTable[0].FragmentBuffer = g_udp_resp;
        t->Packet.RxData = &g_udp_rxdata;
        t->Status = EFI_SUCCESS; if (e) e->signaled = 1; tok_terminal(t);
    } else {
        g_udp_hung_tok = t;   /* firmware keeps this token address (will "write late") */
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_udp_Cancel(EFI_UDP4_PROTOCOL *This, EFI_UDP4_COMPLETION_TOKEN *t) {
    (void)This; FW();
    if (t == NULL) {                               /* Cancel(NULL) — cancel all (udp close) */
        g_udp_cancel_all_calls++;
        if (!g_cancel_signals) return EFI_SUCCESS;   /* firmware can't cancel → tokens stay live */
        for (int i = 0; i < g_tok_count; i++)
            if (g_toks[i].outstanding && g_toks[i].ev) {
                g_toks[i].ev->signaled = 1;
                g_toks[i].outstanding = 0;
            }
        return EFI_SUCCESS;
    }
    g_udp_cancel_calls++;
    if (!g_cancel_signals) return EFI_NOT_FOUND;   /* token not retired → drain fails */
    MockEvent *e = (MockEvent *)t->Event;
    if (e) e->signaled = 1;
    t->Status = EFI_ABORTED;
    tok_terminal(t);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_udp_Poll(EFI_UDP4_PROTOCOL *This) { (void)This; FW(); return EFI_SUCCESS; }

/* ── wire up the vtables ────────────────────────────────────────────────────────── */
static void mock_init(void) {
    memset(&g_bs, 0, sizeof(g_bs));
    g_bs.AllocatePool = m_AllocatePool;
    g_bs.FreePool = m_FreePool;
    g_bs.CreateEvent = (EFI_STATUS (EFIAPI *)(UINT32, UINTN, VOID *, VOID *, EFI_EVENT *))m_CreateEvent;
    g_bs.CloseEvent = m_CloseEvent;
    g_bs.CheckEvent = m_CheckEvent;
    g_bs.SignalEvent = m_SignalEvent;
    g_bs.Stall = m_Stall;
    g_bs.HandleProtocol = m_HandleProtocol;
    g_bs.LocateHandleBuffer = m_LocateHandleBuffer;
    g_bs.OpenProtocol = m_OpenProtocol;
    g_bs.CloseProtocol = m_CloseProtocol;
    g_bs.LocateProtocol = m_LocateProtocol;

    g_tcp_sb.CreateChild = m_tcp_CreateChild;
    g_tcp_sb.DestroyChild = m_tcp_DestroyChild;
    g_udp_sb.CreateChild = m_udp_CreateChild;
    g_udp_sb.DestroyChild = m_udp_DestroyChild;

    g_tcp.GetModeData = m_tcp_GetModeData;
    g_tcp.Configure = m_tcp_Configure;
    g_tcp.Routes = m_tcp_Routes;
    g_tcp.Connect = m_tcp_Connect;
    g_tcp.Accept = m_tcp_Accept;
    g_tcp.Transmit = m_tcp_Transmit;
    g_tcp.Receive = m_tcp_Receive;
    g_tcp.Close = m_tcp_Close;
    g_tcp.Cancel = m_tcp_Cancel;
    g_tcp.Poll = m_tcp_Poll;

    g_udp.GetModeData = m_udp_GetModeData;
    g_udp.Configure = m_udp_Configure;
    g_udp.Groups = m_udp_Groups;
    g_udp.Routes = m_udp_Routes;
    g_udp.Transmit = m_udp_Transmit;
    g_udp.Receive = m_udp_Receive;
    g_udp.Cancel = m_udp_Cancel;
    g_udp.Poll = m_udp_Poll;
}

static void reset_counters(void) {
    g_tcp_cancel_calls = g_tcp_cancel_all_calls = g_udp_cancel_calls = g_udp_cancel_all_calls = 0;
    g_destroy_child_calls = g_close_proto_calls = g_free_pool_calls = 0;
    g_configure_null_calls = g_recycle_signals = g_firmware_calls = 0;
    g_cancel_signals = 1; g_tcp_poll_calls = 0; g_tcp_rx_datalen_override = 0;
    g_udp_transmit_mode = g_udp_receive_mode = TOK_COMPLETE_OK;
    g_udp_configure_status = EFI_SUCCESS; g_udp_configure_calls = 0;
    g_udp_tx_calls = 0; g_udp_tx_len = 0; g_udp_hung_tok = NULL;
    tok_reset();
    accept_reset();
    g_event_count = 0;
    for (int i = 0; i < MAX_EVENTS; i++) memset(&g_events[i], 0, sizeof(g_events[i]));
}

/* ── tracking allocator + on_final counter for the KlDgramLife tests (6.4b-3b) ──────────
 * A KlAllocator over malloc that RECORDS live blocks, so a QUARANTINE test — which intentionally
 * leaks a KlDgramLife forever (on_final must NEVER run) — can reclaim that heap block at test end
 * WITHOUT running on_final (talloc_free_all frees it directly), keeping LSan clean while still
 * proving retention. Delivered/stale tests release normally (on_final frees the life via t_free). */
#define TALLOC_MAX 32
static struct { void *p[TALLOC_MAX]; int n; } g_talloc;
static void *t_malloc(void *c, size_t n) {
    (void)c; void *p = malloc(n ? n : 1);
    if (p && g_talloc.n < TALLOC_MAX) g_talloc.p[g_talloc.n++] = p;
    return p;
}
static void *t_realloc(void *c, void *p, size_t o, size_t n) {
    (void)c; (void)o; void *q = realloc(p, n ? n : 1);
    for (int i = 0; i < g_talloc.n; i++) if (g_talloc.p[i] == p) { g_talloc.p[i] = q; return q; }
    if (q && g_talloc.n < TALLOC_MAX) g_talloc.p[g_talloc.n++] = q;
    return q;
}
static void t_free(void *c, void *p, size_t n) {
    (void)c; (void)n; if (!p) return;
    for (int i = 0; i < g_talloc.n; i++) if (g_talloc.p[i] == p) { g_talloc.p[i] = g_talloc.p[--g_talloc.n]; break; }
    free(p);
}
static KlAllocator g_ta = { .malloc = t_malloc, .realloc = t_realloc, .free = t_free, .ctx = NULL };
static void talloc_reset(void) { g_talloc.n = 0; }
static void talloc_free_all(void) { for (int i = 0; i < g_talloc.n; i++) free(g_talloc.p[i]); g_talloc.n = 0; }

static int g_on_final_ran;
static void mock_on_final(void *ctx) { (void)ctx; g_on_final_ran++; }

/* Fresh UDP datagram provider each test (re-inits the file-scope ctx over the fake bs). The
 * static slot pool is reclaimed as tests close their sockets; a quarantine test leaks one slot
 * by design, so the udp cases run before the pool is stressed. */
static void fresh_udp(void) {
    kl_uefi_udp_provider_reset();
    kl_uefi_udp_provider_init(&g_bs, (EFI_HANDLE)0x1);
}

/* Fresh provider each test (clears the slot pool + ctx). */
static const KlSocketProvider *fresh_provider(void) {
    kl_uefi_socket_provider_reset();
    return kl_uefi_socket_provider(&g_bs, (EFI_HANDLE)0x1);
}

/* Build a numeric-literal KlSockAddr (10.0.2.2:80). */
static void mk_addr(KlSockAddr *a) {
    uint8_t ip[4] = { 10, 0, 2, 2 };
    kl_sockaddr_from_ipv4(a, ip, 80);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 1 — connect timeout → close: Connect hangs → the connect helper Cancels+drains
 * → then close() completes with no outstanding tokens.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_connect_timeout_close(void) {
    T_CASE("connect timeout -> close (F1 connect cancel+drain, F2 close)");
    reset_counters();
    g_tcp_connect_mode  = TOK_HANG;   /* Connect will not complete */
    g_tcp_close_mode    = TOK_COMPLETE_OK;
    const KlSocketProvider *p = fresh_provider();
    CHECK(p != NULL, "provider created");
    KlSocketHandle fd = p->ops->socket(p->context, 2 /*AF_INET*/, 1 /*STREAM*/, 0);
    CHECK(kl_handle_valid(fd), "socket() claimed a slot");

    KlSockAddr a; mk_addr(&a);
    int r = kl_uefi_socket_configure(fd, &a);
    CHECK(r == 0, "configure ok");
    /* connect_now issues Connect + pumps; it should Cancel+drain on the hang. */
    r = kl_uefi_socket_connect_now(fd);
    CHECK(r == -1, "connect_now returns -1 on hang (timed out)");
    CHECK(g_tcp_cancel_calls >= 1, "F1: Cancel(token) called on the connect timeout");
    CHECK(outstanding_count() == 0, "F1: no token outstanding after connect cancel+drain");

    /* close() reconciles + tears down. */
    p->ops->close(p->context, fd);
    CHECK(outstanding_count() == 0, "F2: no token outstanding after close");
    CHECK(g_destroy_child_calls == 1, "close: DestroyChild called");
    CHECK(g_configure_null_calls == 1, "close: Configure(NULL) reset called");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 2 — transmit timeout: Transmit hangs → efi_sock_send returns -1 AND the mock
 * saw a Cancel + drained the tx token (no token left referencing the caller buffer).
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_transmit_timeout(void) {
    T_CASE("transmit timeout (F1 tx cancel+drain)");
    reset_counters();
    g_tcp_connect_mode  = TOK_COMPLETE_OK;
    g_tcp_transmit_mode = TOK_HANG;
    g_tcp_close_mode    = TOK_COMPLETE_OK;
    const KlSocketProvider *p = fresh_provider();
    KlSocketHandle fd = p->ops->socket(p->context, 2, 1, 0);
    KlSockAddr a; mk_addr(&a);
    kl_uefi_socket_configure(fd, &a);
    int r = kl_uefi_socket_connect_now(fd);
    CHECK(r == 0, "connect_now ok (connect completes)");

    char buf[16] = "hello world";
    kl_ssize_t n = p->ops->send(p->context, fd, buf, 11);
    CHECK(n == -1, "send returns -1 on transmit hang");
    CHECK(g_tcp_cancel_calls >= 1, "F1: Cancel(token) called on the transmit timeout");
    CHECK(outstanding_count() == 0, "F1: no token outstanding after tx cancel+drain");

    /* conn was marked dead by the un-confirmable path? Here cancel-drain confirms, so
     * dead should be false — but the send still failed. close() must still be clean. */
    p->ops->close(p->context, fd);
    CHECK(outstanding_count() == 0, "no token outstanding after close");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 3 — receive pending during close: post a Receive (rx_posted), leave it hanging
 * → close() Cancels+drains it before DestroyChild/free.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_receive_pending_close(void) {
    T_CASE("receive pending during close (F2 rx cancel+drain)");
    reset_counters();
    g_tcp_connect_mode = TOK_COMPLETE_OK;
    g_tcp_receive_mode = TOK_HANG;      /* Receive posts but never completes */
    g_tcp_close_mode   = TOK_COMPLETE_OK;
    const KlSocketProvider *p = fresh_provider();
    KlSocketHandle fd = p->ops->socket(p->context, 2, 1, 0);
    KlSockAddr a; mk_addr(&a);
    kl_uefi_socket_configure(fd, &a);
    kl_uefi_socket_connect_now(fd);

    /* recv_ready posts a Receive that hangs → rx_posted stays 1. */
    int ready = kl_uefi_socket_recv_ready(fd);
    CHECK(ready == 0, "recv_ready: 0 (Receive posted, still pending)");
    CHECK(outstanding_count() == 1, "one Receive token outstanding pre-close");

    p->ops->close(p->context, fd);
    CHECK(g_tcp_cancel_all_calls == 1, "F2: Cancel(NULL) called in close");
    CHECK(outstanding_count() == 0, "F2: Receive token drained before DestroyChild/free");
    CHECK(g_destroy_child_calls == 1, "close: DestroyChild called after drain");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 4 — DNS transmit & receive timeout: same for the UDP4 path.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_dns_transmit_timeout(void) {
    T_CASE("DNS transmit timeout (F1 UDP tx cancel+drain)");
    reset_counters();
    g_udp_transmit_mode = TOK_HANG;
    g_udp_receive_mode  = TOK_COMPLETE_OK;
    kl_uefi_dns_init(&g_bs, (EFI_HANDLE)0x1);
    KlSockAddr out;
    int r = kl_uefi_dns_resolve("example.com", 80, &out);
    CHECK(r == -1, "dns_resolve -1 on transmit hang");
    CHECK(g_udp_cancel_calls >= 1, "F1: UDP Cancel(token) on tx timeout");
    CHECK(outstanding_count() == 0, "F1: no UDP token outstanding after tx cancel+drain");
    CHECK(g_destroy_child_calls == 1, "dns teardown: DestroyChild called");
}

static void t_dns_receive_timeout(void) {
    T_CASE("DNS receive timeout (F1 UDP rx cancel+drain)");
    reset_counters();
    g_udp_transmit_mode = TOK_COMPLETE_OK;
    g_udp_receive_mode  = TOK_HANG;
    kl_uefi_dns_init(&g_bs, (EFI_HANDLE)0x1);
    KlSockAddr out;
    int r = kl_uefi_dns_resolve("example.com", 80, &out);
    CHECK(r == -1, "dns_resolve -1 on receive hang");
    CHECK(g_udp_cancel_calls >= 1, "F1: UDP Cancel(token) on rx timeout");
    CHECK(outstanding_count() == 0, "F1: no UDP token outstanding after rx cancel+drain");
    CHECK(g_destroy_child_calls == 1, "dns teardown: DestroyChild called");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 5 — cancel racing natural completion: a token that completes naturally is NOT
 * cancelled (exactly one terminal), and the OK DNS path recycles RxData exactly once.
 * ───────────────────────────────────────────────────────────────────────────── */
static void build_dns_a_response(uint16_t id) {
    /* Minimal valid A-record response for the query id the resolver will use. The
     * resolver uses a random id, so we can't match it — instead test the happy send/recv
     * lifetime (no double-terminal) via counters rather than a parsed answer. */
    (void)id;
    memset(g_udp_resp, 0, sizeof(g_udp_resp));
    g_udp_resp_len = 12;   /* header only — parse will fail (no answer), that's fine */
}
static void t_cancel_racing_completion(void) {
    T_CASE("cancel racing natural completion (exactly-one terminal + single recycle)");
    reset_counters();
    g_udp_transmit_mode = TOK_COMPLETE_OK;
    g_udp_receive_mode  = TOK_COMPLETE_OK;   /* both complete naturally, no cancel */
    build_dns_a_response(0);
    kl_uefi_dns_init(&g_bs, (EFI_HANDLE)0x1);
    KlSockAddr out;
    int r = kl_uefi_dns_resolve("example.com", 80, &out);
    (void)r;   /* parse fails (12-byte header, no answer) → -1, but lifetime is clean */
    CHECK(g_udp_cancel_calls == 0, "no Cancel when both tokens complete naturally");
    CHECK(outstanding_count() == 0, "exactly-one terminal: no token left outstanding");
    CHECK(g_recycle_signals == 1, "RxData RecycleSignal signaled exactly once");
    CHECK(g_destroy_child_calls == 1, "teardown: DestroyChild once");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 6 — close-token timeout: graceful Close hangs → close() still tears down
 * (Cancel+drain the close token, DestroyChild) — no hang, no leak.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_close_token_timeout(void) {
    T_CASE("close-token timeout (F2 pump_or_cancel on close token)");
    reset_counters();
    g_tcp_connect_mode = TOK_COMPLETE_OK;
    g_tcp_close_mode   = TOK_HANG;      /* the graceful Close token never completes */
    const KlSocketProvider *p = fresh_provider();
    KlSocketHandle fd = p->ops->socket(p->context, 2, 1, 0);
    KlSockAddr a; mk_addr(&a);
    kl_uefi_socket_configure(fd, &a);
    kl_uefi_socket_connect_now(fd);

    p->ops->close(p->context, fd);
    CHECK(g_tcp_cancel_calls >= 1, "F2: Cancel(token) on the hung close token");
    CHECK(outstanding_count() == 0, "F2: close token drained (no outstanding)");
    CHECK(g_destroy_child_calls == 1, "close still tears down: DestroyChild");
    CHECK(g_configure_null_calls == 1, "close still resets: Configure(NULL)");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 7 — calls after simulated EBS: send/recv/recv_ready/connect/drain/dns all refuse
 * WITHOUT any firmware call; close() frees without touching firmware.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_after_ebs_refuses(void) {
    T_CASE("calls after simulated EBS (F3 fail-closed, zero firmware calls)");
    reset_counters();
    g_tcp_connect_mode = TOK_COMPLETE_OK;
    const KlSocketProvider *p = fresh_provider();
    KlSocketHandle fd = p->ops->socket(p->context, 2, 1, 0);
    KlSockAddr a; mk_addr(&a);
    kl_uefi_socket_configure(fd, &a);
    kl_uefi_socket_connect_now(fd);

    /* Now firmware "leaves boot services". */
    g_after_ebs = 1;
    int fw_before = g_firmware_calls;

    char buf[8] = "x";
    CHECK(p->ops->send(p->context, fd, buf, 1) == -1, "send refuses post-EBS");
    CHECK(p->ops->recv(p->context, fd, buf, 1) == -1, "recv refuses post-EBS");
    CHECK(kl_uefi_socket_recv_ready(fd) == 0, "recv_ready 0 post-EBS");
    CHECK(p->ops->connect(p->context, fd, &a) == -1, "connect refuses post-EBS");
    CHECK(kl_uefi_socket_connect_poll(fd, NULL) == -1, "connect_poll refuses post-EBS");
    CHECK(kl_uefi_socket_connect_now(fd) == -1, "connect_now refuses post-EBS");

    /* event loop drain must not call firmware post-EBS. */
    const KlEventProvider *ep = kl_uefi_event_provider(&g_bs, (EFI_HANDLE)0x1);
    CHECK(ep != NULL, "event provider created");
    KlCompletionEvent evs[4];
    int dn = COMP(ep)->drain(NULL, evs, 4, 0);
    CHECK(dn == 0, "el_drain returns 0 post-EBS");

    /* DNS must refuse up front. */
    kl_uefi_dns_init(&g_bs, (EFI_HANDLE)0x1);
    KlSockAddr out;
    CHECK(kl_uefi_dns_resolve("example.com", 80, &out) == -1, "dns_resolve refuses post-EBS");

    CHECK(g_firmware_calls == fw_before, "F3: ZERO firmware calls across all post-EBS entries");

    /* close() post-EBS: frees the slot without touching firmware. */
    int fw_before_close = g_firmware_calls;
    p->ops->close(p->context, fd);
    CHECK(g_firmware_calls == fw_before_close, "F3: close() touches no firmware post-EBS");
    CHECK(g_destroy_child_calls == 0, "F3: no DestroyChild post-EBS");

    g_after_ebs = 0;   /* restore for subsequent tests */
    kl_uefi_event_provider_reset();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 8 — entropy fail-closed (F4). Links the REAL mbedtls_hardware_poll from the
 * mbedTLS-free entropy_uefi.c (split out precisely so this runs without the mbedTLS
 * adapter's libc-clashing residuals). No MOCK_WITH_MBEDTLS gate — always runs.
 * ───────────────────────────────────────────────────────────────────────────── */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
/* entropy_uefi.c references kl_uefi_have_entropy; stub it to report NO entropy so we
 * exercise the fail-closed (default) / insecure-fallback (macro) branch. */
int kl_uefi_have_entropy(void);
int kl_uefi_have_entropy(void) { return 0; }
static void t_entropy_fail_closed(void) {
    T_CASE("entropy unavailable in the reusable adapter (F4)");
    unsigned char buf[16];
    size_t olen = 123;
    int r = mbedtls_hardware_poll(NULL, buf, sizeof(buf), &olen);
#ifdef KL_UEFI_INSECURE_TEST_ENTROPY
    CHECK(r == 0 && olen == sizeof(buf), "with macro: fills weak bytes (r=0, olen=len)");
#else
    CHECK(r != 0 && olen == 0, "without macro: FAILS CLOSED (r!=0, olen=0)");
#endif
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST — cert validity-time conversion + fail-closed clock policy (U-8). The
 * error-prone civil-date math + the fail-closed floor back mbedTLS's notBefore/
 * notAfter enforcement; the QEMU RTC is always valid, so the bad-clock rejection
 * is only ever exercised here.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_civil_time(void) {
    T_CASE("cert validity-time: civil<->unix + fail-closed clock (U-8)");
    /* known UTC vectors */
    KlCivil a = {2026, 8, 6, 12, 0, 0};
    CHECK(kl_civil_to_unix(&a) == 1786017600LL, "civil->unix 2026-08-06T12:00:00Z");
    KlCivil e = {1970, 1, 1, 0, 0, 0};
    CHECK(kl_civil_to_unix(&e) == 0, "civil->unix epoch");
    /* round-trip over a wide range (leap years, pre-epoch) */
    int rt_ok = 1;
    for (int64_t t = -2000000000LL; t < 4000000000LL; t += 1500007) {
        KlCivil c; kl_unix_to_civil(t, &c);
        if (kl_civil_to_unix(&c) != t) { rt_ok = 0; break; }
    }
    CHECK(rt_ok, "unix->civil->unix round-trips exactly");

    /* fail-closed policy: year below the floor is rejected (untrustworthy RTC). */
    int64_t out = 12345;
    KlCivil stuck = {2000, 1, 1, 0, 0, 0};   /* an unset RTC */
    CHECK(kl_wallclock_from_fields(&stuck, 0, 0, 2025, &out) == -1 && out == 12345,
          "U-8: year < floor -> FAIL CLOSED (out untouched)");
    KlCivil good = {2026, 6, 1, 0, 0, 0};
    CHECK(kl_wallclock_from_fields(&good, 0, 0, 2025, &out) == 0 && out == 1780272000LL,
          "U-8: valid year -> UTC seconds");
    /* timezone normalisation: UEFI §8.3 UTC = LocalTime + TimeZone. PST = +480. */
    int64_t utc_pst = 0;
    KlCivil local = {2026, 6, 1, 0, 0, 0};
    kl_wallclock_from_fields(&local, 480, 1, 2025, &utc_pst);     /* PST tz=+480 */
    CHECK(utc_pst == 1780272000LL + 480 * 60,
          "U-8: PST (+480) normalises UTC = local + TimeZone");
    int64_t utc_east = 0;
    kl_wallclock_from_fields(&local, -120, 1, 2025, &utc_east);   /* UTC+2 zone tz=-120 */
    CHECK(utc_east == 1780272000LL - 120 * 60,
          "U-8: -120 normalises UTC = local + TimeZone");

    /* EFI_TIME field validation (malformed firmware must be REJECTED, not normalised). */
    KlCivil v = {2026, 6, 15, 12, 30, 30};
    CHECK(kl_civil_valid(&v, 0, 0, 0, 0) == 0, "U-8: well-formed EFI_TIME accepted");
    KlCivil m0  = {2026, 0, 1, 0, 0, 0};   CHECK(kl_civil_valid(&m0,  0,0,0,0) == -1, "U-8: month 0 rejected");
    KlCivil m13 = {2026,13, 1, 0, 0, 0};   CHECK(kl_civil_valid(&m13, 0,0,0,0) == -1, "U-8: month 13 rejected");
    KlCivil f30 = {2026, 2,30, 0, 0, 0};   CHECK(kl_civil_valid(&f30, 0,0,0,0) == -1, "U-8: Feb 30 rejected");
    KlCivil f29n= {2023, 2,29, 0, 0, 0};   CHECK(kl_civil_valid(&f29n,0,0,0,0) == -1, "U-8: Feb 29 (non-leap) rejected");
    KlCivil f29l= {2024, 2,29, 0, 0, 0};   CHECK(kl_civil_valid(&f29l,0,0,0,0) ==  0, "U-8: Feb 29 (leap) accepted");
    KlCivil h25 = {2026, 6, 1,25, 0, 0};   CHECK(kl_civil_valid(&h25, 0,0,0,0) == -1, "U-8: hour 25 rejected");
    KlCivil s60 = {2026, 6, 1, 0, 0,60};   CHECK(kl_civil_valid(&s60, 0,0,0,0) == -1, "U-8: second 60 rejected");
    CHECK(kl_civil_valid(&v, 1000000000u, 0, 0, 0) == -1, "U-8: nanosecond > 1e9 rejected");
    CHECK(kl_civil_valid(&v, 0, 1441, 1, 0) == -1, "U-8: TimeZone > 1440 rejected");
    CHECK(kl_civil_valid(&v, 0, -1441, 1, 0) == -1, "U-8: TimeZone < -1440 rejected");
    CHECK(kl_civil_valid(&v, 0, 0, 0, 0x04) == -1, "U-8: reserved Daylight bit rejected");
    CHECK(kl_civil_valid(&v, 0, 0, 0, 0x03) == 0,  "U-8: Daylight ADJUST|IN accepted");
}

/* Build an EFI_TIME for the decode tests. */
static EFI_TIME mk_efi_time(UINT16 y, UINT8 mo, UINT8 d, UINT8 h, UINT8 mi, UINT8 s,
                            INT16 tz, UINT8 dl) {
    EFI_TIME t; memset(&t, 0, sizeof(t));
    t.Year = y; t.Month = mo; t.Day = d; t.Hour = h; t.Minute = mi; t.Second = s;
    t.TimeZone = tz; t.Daylight = dl;
    return t;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST — EFI_TIME decode + the unspecified-timezone POLICY (U-8, finding 1). The mock is
 * built WITHOUT KL_UEFI_ASSUME_UNSPECIFIED_UTC, i.e. the production default: an unspecified
 * zone (local time, unknown offset) must be REJECTED unless the app declares an offset.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_wallclock_decode(void) {
    T_CASE("EFI_TIME decode + unspecified-TZ policy (U-8)");
    int64_t out = 0;
    /* GetTime failed -> fail-closed regardless of the (garbage) EFI_TIME. */
    EFI_TIME any = mk_efi_time(2026, 6, 1, 0, 0, 0, 0, 0);
    CHECK(kl_uefi_wallclock_from_efi(0, &any, &out) == -1, "U-8: GetTime failure -> -1");
    /* Explicit UTC zone (TimeZone=0, specified) -> convert. */
    EFI_TIME utc = mk_efi_time(2026, 6, 1, 0, 0, 0, 0, 0);
    CHECK(kl_uefi_wallclock_from_efi(1, &utc, &out) == 0 && out == 1780272000LL,
          "U-8: explicit UTC zone converts");
    /* Unspecified zone, no configured offset -> REJECT (finding 1, production default). */
    EFI_TIME unspec = mk_efi_time(2026, 6, 1, 0, 0, 0, (INT16)EFI_UNSPECIFIED_TIMEZONE, 0);
    CHECK(kl_uefi_wallclock_from_efi(1, &unspec, &out) == -1,
          "U-8: unspecified TZ REJECTED by default (local time, unknown offset)");
    /* App declares the local offset (PST +480) -> unspecified now resolves: UTC = local + 480. */
    kl_uefi_set_unspecified_tz(480);
    CHECK(kl_uefi_wallclock_from_efi(1, &unspec, &out) == 0 && out == 1780272000LL + 480 * 60,
          "U-8: configured unspecified-TZ offset applied");
    kl_uefi_clear_unspecified_tz();
    CHECK(kl_uefi_wallclock_from_efi(1, &unspec, &out) == -1, "U-8: cleared -> rejects again");
    /* Malformed fields via the decode path are rejected. */
    EFI_TIME bad = mk_efi_time(2026, 13, 1, 0, 0, 0, 0, 0);
    CHECK(kl_uefi_wallclock_from_efi(1, &bad, &out) == -1, "U-8: malformed EFI_TIME -> -1");
    /* Defensive NULL guards (reusable API): NULL t or NULL out -> -1, never a deref/false OK. */
    CHECK(kl_uefi_wallclock_from_efi(1, NULL, &out) == -1, "U-8: NULL EFI_TIME -> -1");
    CHECK(kl_uefi_wallclock_from_efi(1, &utc, NULL) == -1, "U-8: NULL out_unix -> -1");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST — the TLS-lifetime snapshot clock (U-8, finding 2). mbedtls_time reads ONE SNAPSHOT
 * (captured once inside platform init, advanced by the monotonic clock), never GetTime — so a
 * GetTime that fails AFTER capture cannot reopen the epoch-0 loophole, and concurrent sessions
 * share one forward-moving basis. Driven via the scriptable kl_uefi_wallclock stub above.
 * ───────────────────────────────────────────────────────────────────────────── */
/* The platform-readiness gate on the cert snapshot (U-8): a not-ready platform (frozen monotonic
 * timer) must make the snapshot REFUSE even with a good wall clock — else cert time freezes and
 * accepts expired certs. Its own scenario so the count reflects it. */
static void t_platform_ready_gate(void) {
    T_CASE("platform-readiness gate on the cert snapshot (U-8)");
    kl_uefi_clock_snapshot_reset();
    g_wc_fail = 0;         /* wall clock is fine ... */
    g_plat_ready = 0;      /* ... but the platform (monotonic timer) is NOT ready */
    CHECK(kl_uefi_clock_snapshot() == -1, "U-8: platform-not-ready (frozen timer) -> snapshot REFUSED");
    CHECK(kl_uefi_mbedtls_time(NULL) == 0, "U-8: no snapshot after refusal -> epoch 0");
    g_plat_ready = 1;      /* restore for later tests */
}

static void t_clock_snapshot(void) {
    T_CASE("TLS-lifetime snapshot clock gate (U-8)");
    g_plat_ready = 1;
    kl_uefi_clock_snapshot_reset();
    /* No snapshot yet -> fail-closed epoch. */
    CHECK(kl_uefi_mbedtls_time(NULL) == 0, "U-8: no snapshot -> mbedtls_time == epoch 0");
    /* Trustworthy clock -> snapshot succeeds; mbedtls_time ~ snapshot (advanced by monotonic). */
    g_wc_fail = 0; g_wc_val = 1780272000LL;
    CHECK(kl_uefi_clock_snapshot() == 0, "U-8: snapshot succeeds with a trustworthy clock");
    long long now = kl_uefi_mbedtls_time(NULL);
    CHECK(now >= 1780272000LL && now <= 1780272000LL + 5,
          "U-8: mbedtls_time reads the snapshot (monotonic-advanced), not GetTime");
    /* A later GetTime FAILURE is irrelevant: the callback still uses the snapshot, never GetTime. */
    g_wc_fail = 1;
    CHECK(kl_uefi_mbedtls_time(NULL) >= 1780272000LL,
          "U-8: post-snapshot GetTime failure does NOT reopen the epoch-0 loophole");
    /* A REFRESH that fails invalidates the snapshot -> session must refuse (fail-closed). */
    CHECK(kl_uefi_clock_snapshot() == -1, "U-8: snapshot refresh refuses when clock untrustworthy");
    CHECK(kl_uefi_mbedtls_time(NULL) == 0, "U-8: failed refresh invalidates snapshot -> epoch 0");
    g_wc_fail = 0;   /* restore */
    kl_uefi_clock_snapshot_reset();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 9 — stale-guard no-UAF: create a conn, queue a connect op in event_efi with its
 * (handle, gen), close() the conn (slot freed + gen bumped), then el_drain — assert the
 * stale op is dropped and NOTHING dereferences freed memory (F6: stable slot pool).
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_stale_guard_no_uaf(void) {
    T_CASE("stale-guard no-UAF (F6 stable slot pool)");
    reset_counters();
    g_tcp_connect_mode = TOK_HANG;   /* keep the connect pending so the op stays queued */
    g_tcp_close_mode   = TOK_COMPLETE_OK;
    kl_uefi_event_provider_reset();
    const KlEventProvider *ep = kl_uefi_event_provider(&g_bs, (EFI_HANDLE)0x1);
    const KlSocketProvider *p = fresh_provider();
    KlSocketHandle fd = p->ops->socket(p->context, 2, 1, 0);
    CHECK(kl_handle_valid(fd), "socket claimed");
    unsigned long long gen = kl_uefi_conn_generation_h(fd);
    CHECK((gen & 1) == 1, "live conn generation is odd");

    KlSockAddr a; mk_addr(&a);
    /* Queue a connect op (post_connect issues Configure + Connect-post; Connect hangs). */
    int watcher_tag = 0;
    void *tagged = (void *)((uintptr_t)&watcher_tag | 1);
    int r = COMP(ep)->post_connect(NULL, fd, &a, tagged);
    CHECK(r == 0, "post_connect queued the op");

    /* Now close the conn — slot freed, generation bumped even. The queued op holds the
     * OLD (handle, gen). With F6 the slot storage is stable, so validity check is safe. */
    p->ops->close(p->context, fd);
    unsigned long long gen2 = kl_uefi_conn_generation_h(fd);   /* fd now dead → 0 */
    CHECK(gen2 == 0, "closed conn: generation_h reports 0 (slot dead)");
    CHECK(!kl_uefi_conn_valid_h(fd, gen), "stale (handle, old gen) rejected — no UAF");

    /* Drain: the stale connect op must be dropped, not delivered / dereferenced. */
    KlCompletionEvent evs[4];
    int dn = COMP(ep)->drain(NULL, evs, 4, 0);
    /* The op was for a now-closed conn → dropped → no KL_COMP_CONNECT emitted. */
    int emitted_connect = 0;
    for (int i = 0; i < dn; i++) if (evs[i].kind == KL_COMP_CONNECT) emitted_connect = 1;
    CHECK(emitted_connect == 0, "F6: stale connect op dropped (no completion emitted)");

    kl_uefi_event_provider_reset();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 10 (F1 release-blocker) — CANCEL FAILS: a Transmit hangs AND the firmware's
 * Cancel does not retire the token. The provider must QUARANTINE the slot: never
 * CloseEvent/DestroyChild (the firmware still owns the token + slot tx_buf), never
 * reclaim the slot for a new socket. ASan proves no freed storage is referenced.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_cancel_fails_quarantine(void) {
    T_CASE("cancel-drain FAILS -> quarantine, never free/reuse (F1)");
    reset_counters();
    g_tcp_connect_mode  = TOK_COMPLETE_OK;
    g_tcp_transmit_mode = TOK_HANG;
    g_cancel_signals    = 0;             /* Cancel is a no-op — the token stays live */
    const KlSocketProvider *p = fresh_provider();
    KlSocketHandle fd = p->ops->socket(p->context, 2, 1, 0);
    KlSockAddr a; mk_addr(&a);
    kl_uefi_socket_configure(fd, &a);
    kl_uefi_socket_connect_now(fd);

    char buf[16] = "data";
    kl_ssize_t n = p->ops->send(p->context, fd, buf, 4);
    CHECK(n == -1, "send fails on transmit hang + failed cancel");
    CHECK(g_tcp_cancel_calls >= 1, "Cancel(token) was attempted");

    /* close() must NOT DestroyChild / CloseEvent a quarantined conn (firmware owns it). */
    p->ops->close(p->context, fd);
    CHECK(g_destroy_child_calls == 0, "F1: quarantined conn NOT destroyed (child leaked, safe)");

    /* The quarantined slot must NOT be reclaimed — a new socket gets a DIFFERENT slot. */
    KlSocketHandle fd2 = p->ops->socket(p->context, 2, 1, 0);
    CHECK(kl_handle_valid(fd2), "a fresh slot is still available");
    CHECK(fd2 != fd, "F1: quarantined slot is NOT reused for the new socket");
    g_cancel_signals = 1;   /* restore */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 11 (F2 release-blocker) — close must NOT spin on an ALREADY-CONSUMED connect
 * token. A completed connect retired its token (event consumed by CheckEvent); close()
 * must skip it (conn_posted cleared) rather than pump ~forever on an event that will
 * never fire again.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_close_no_spin_on_consumed_connect(void) {
    T_CASE("close does NOT spin on a consumed connect token (F2 token-state)");
    reset_counters();
    g_tcp_connect_mode = TOK_COMPLETE_OK;   /* connect completes → token consumed */
    g_tcp_close_mode   = TOK_COMPLETE_OK;
    const KlSocketProvider *p = fresh_provider();
    KlSocketHandle fd = p->ops->socket(p->context, 2, 1, 0);
    KlSockAddr a; mk_addr(&a);
    kl_uefi_socket_configure(fd, &a);
    int r = kl_uefi_socket_connect_now(fd);
    CHECK(r == 0, "connect completed");

    g_tcp_poll_calls = 0;                   /* measure Poll() during close only */
    p->ops->close(p->context, fd);
    /* With a proxy (configured && !connected) close would pump the consumed connect
     * token to the spin cap (thousands of Poll). With token-state it drains nothing
     * stale. Allow a small budget for the graceful-close token drain. */
    CHECK(g_tcp_poll_calls < 50, "F2: close did not spin on the consumed connect token");
    CHECK(g_destroy_child_calls == 1, "close tore down cleanly");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 12 (F4 release-blocker) — an impossible firmware-reported receive DataLength
 * must be rejected, not trusted into rx_len (which would read past rx_buf).
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_bad_rx_length(void) {
    T_CASE("impossible received DataLength rejected (F4)");
    reset_counters();
    g_tcp_connect_mode = TOK_COMPLETE_OK;
    g_tcp_receive_mode = TOK_COMPLETE_OK;
    g_tcp_rx_datalen_override = 8192u + 4096u;   /* > KL_EFI_RXBUF */
    const KlSocketProvider *p = fresh_provider();
    KlSocketHandle fd = p->ops->socket(p->context, 2, 1, 0);
    KlSockAddr a; mk_addr(&a);
    kl_uefi_socket_configure(fd, &a);
    kl_uefi_socket_connect_now(fd);

    int ready = kl_uefi_socket_recv_ready(fd);
    CHECK(ready == 1, "recv_ready 1 (latched a result — an error, not bogus data)");
    char rx[64];
    kl_ssize_t n = p->ops->recv(p->context, fd, rx, sizeof(rx));
    CHECK(n == -1, "F4: recv returns -1 (fatal) on impossible DataLength, no OOB read");
    g_tcp_rx_datalen_override = 0;
    p->ops->close(p->context, fd);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * TEST 13 (F1/UDP release-blocker) — a DNS Transmit hangs AND Cancel does not retire
 * the token: the resolver must QUARANTINE (leak the child/events/static tx+qbuf, retain
 * until EBS) and fail-close every subsequent resolve — never DestroyChild a live token.
 * Runs LAST: it sets the sticky g_dns_quarantined.
 * ───────────────────────────────────────────────────────────────────────────── */
static void t_dns_cancel_fails_quarantine(void) {
    T_CASE("DNS cancel-drain FAILS -> quarantine + fail-close (F1/UDP)");
    reset_counters();
    g_udp_transmit_mode = TOK_HANG;
    g_cancel_signals    = 0;             /* UDP Cancel is a no-op — token stays live */
    kl_uefi_dns_init(&g_bs, (EFI_HANDLE)0x1);
    KlSockAddr out;
    g_udp_hung_tok = NULL;
    int r = kl_uefi_dns_resolve("example.com", 80, &out);
    CHECK(r == -1, "dns_resolve -1 on tx hang + failed cancel");
    CHECK(g_destroy_child_calls == 0, "F1/UDP: child NOT destroyed while token live (leaked, safe)");

    /* THE bug this guards: the live token the firmware still owns must NOT be stack-local.
     * Model a DELAYED firmware write into it AFTER dns_resolve() has returned (the firmware
     * eventually completes the "hung" Transmit, writing Status + Packet into the token). If
     * the token lived on dns_resolve's stack, this write hits a returned frame — ASan
     * stack-use-after-return. With the g_dns_op fix it lands in stable static storage. */
    CHECK(g_udp_hung_tok != NULL, "F1/UDP: firmware retained the live token address");
    if (g_udp_hung_tok) {
        g_udp_hung_tok->Status = EFI_ABORTED;      /* late write — must be safe */
        g_udp_hung_tok->Packet.RxData = NULL;
        CHECK(g_udp_hung_tok->Status == EFI_ABORTED,
              "F1/UDP: late firmware write lands in STABLE token storage (no stack-UAR)");
    }

    /* A subsequent resolve must fail-close (quarantined). */
    int r2 = kl_uefi_dns_resolve("example.com", 80, &out);
    CHECK(r2 == -1, "F1/UDP: subsequent resolve fails-closed after quarantine");
    g_cancel_signals = 1;
}

/* ═════════════════════════════════════════════════════════════════════════════
 * S-5 — server accept-lifetime scenarios (the runtime verification of S-2 + S-3).
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Basic passive open: bind + listen arm the Accept pool; accept is would-block until
 * a connection is delivered, then returns a child conn with the neutral peer; the
 * child is an ordinary conn (recv works); listener close drains the pool cleanly. */
static void t_accept_basic(void) {
    T_CASE("accept: bind/listen/accept -> child conn + neutral peer (S-2/S-3)");
    reset_counters();
    g_tcp_receive_mode = TOK_COMPLETE_OK; g_tcp_close_mode = TOK_COMPLETE_OK;
    const KlSocketProvider *p = fresh_provider();
    KlSockAddr ba; mk_addr(&ba);
    KlSocketHandle lfd = p->ops->socket(p->context, 2, 1, 0);
    CHECK(kl_handle_valid(lfd), "listener socket claimed a slot");
    CHECK(p->ops->bind(p->context, lfd, &ba) == 0, "bind ok");
    CHECK(p->ops->listen(p->context, lfd, 2) == 0, "listen ok (creates Accept-token pool)");
    CHECK(kl_uefi_socket_accept_arm(lfd, 2) == 2, "accept_arm(2) arms 2 tokens (capacity-gated)");
    CHECK(g_accept_tok_count >= 1, "Accept-token pool armed");

    KlSockAddr peer;
    KlSocketHandle a = p->ops->accept(p->context, lfd, &peer);
    CHECK(!kl_handle_valid(a), "accept would-block before a connection arrives");

    CHECK(mock_deliver_connection() == 1, "one inbound connection delivered");
    a = p->ops->accept(p->context, lfd, &peer);
    CHECK(kl_handle_valid(a), "accept returns the child conn");
    CHECK(kl_sockaddr_family(&peer) == KL_AF_INET, "peer is a neutral IPv4 KlSockAddr");

    char buf[8];
    kl_ssize_t n = p->ops->recv(p->context, a, buf, sizeof buf);
    CHECK(n >= 0, "recv on the accepted child does not error (it is a real conn)");

    p->ops->close(p->context, a);
    p->ops->close(p->context, lfd);
    CHECK(outstanding_count() == 0, "no token outstanding after child + listener close");
}

/* Closing a listener with N outstanding Accept tokens drains them all (Cancel signals
 * them) — no leak, events closed, listener child destroyed. */
static void t_accept_listener_close_drains(void) {
    T_CASE("accept: close listener with N outstanding Accept tokens -> all drained");
    reset_counters();
    g_cancel_signals = 1;   /* firmware Cancel retires the tokens */
    const KlSocketProvider *p = fresh_provider();
    KlSockAddr ba; mk_addr(&ba);
    KlSocketHandle lfd = p->ops->socket(p->context, 2, 1, 0);
    CHECK(p->ops->listen(p->context, lfd, 4) == 0, "listen ok (Accept-token pool created)");
    CHECK(kl_uefi_socket_accept_arm(lfd, 4) == 4, "accept_arm(4) arms 4 Accept tokens");
    CHECK(outstanding_count() >= 4, "4 Accept tokens outstanding");
    p->ops->close(p->context, lfd);
    CHECK(outstanding_count() == 0, "all Accept tokens drained on listener close");
    CHECK(g_destroy_child_calls == 1, "listener child destroyed (clean teardown)");
}

/* THE load-bearing test: an Accept-token cancel FAILS -> the listener is quarantined,
 * NEVER DestroyChild'd (the firmware may still write those tokens via its tcp). */
static void t_accept_cancel_fail_quarantine(void) {
    T_CASE("accept: Accept-token cancel FAILS -> quarantine listener (F1)");
    reset_counters();
    g_cancel_signals = 0;   /* firmware cannot retire the Accept tokens */
    const KlSocketProvider *p = fresh_provider();
    KlSockAddr ba; mk_addr(&ba);
    KlSocketHandle lfd = p->ops->socket(p->context, 2, 1, 0);
    CHECK(p->ops->listen(p->context, lfd, 4) == 0, "listen ok (Accept-token pool created)");
    CHECK(kl_uefi_socket_accept_arm(lfd, 4) == 4, "accept_arm(4) arms Accept tokens");
    int before = g_destroy_child_calls;
    p->ops->close(p->context, lfd);   /* cancel fails -> can't drain -> quarantine */
    CHECK(g_tcp_cancel_all_calls >= 1, "F1: Cancel(NULL) attempted on the Accept pool");
    CHECK(g_destroy_child_calls == before,
          "F1: listener child NOT destroyed while Accept tokens may be live (quarantined)");
    CHECK(outstanding_count() > 0, "F1: the un-retired Accept tokens are leaked, not freed");
    /* fail-closed: a new socket must still work (the quarantined slot is not reused). */
    KlSocketHandle s2 = p->ops->socket(p->context, 2, 1, 0);
    CHECK(kl_handle_valid(s2), "a fresh socket still available after quarantine");
    p->ops->close(p->context, s2);   /* the quarantined listener stays leaked (intentional) */
}

/* An accepted child with a receive outstanding closes cleanly (cancel+drain) — the
 * child reuses efi_sock_close's per-conn token discipline verbatim. */
static void t_accepted_child_close_recv(void) {
    T_CASE("accept: accepted-child close-while-recv-outstanding (cancel+drain)");
    reset_counters();
    g_tcp_receive_mode = TOK_HANG;   /* the child's Receive will not complete */
    g_tcp_close_mode = TOK_COMPLETE_OK;
    const KlSocketProvider *p = fresh_provider();
    KlSockAddr ba; mk_addr(&ba);
    KlSocketHandle lfd = p->ops->socket(p->context, 2, 1, 0);
    p->ops->listen(p->context, lfd, 2);
    kl_uefi_socket_accept_arm(lfd, 2);   /* listen no longer auto-arms — arm explicitly */
    mock_deliver_connection();
    KlSockAddr peer;
    KlSocketHandle a = p->ops->accept(p->context, lfd, &peer);
    CHECK(kl_handle_valid(a), "accepted a child");
    char buf[8];
    (void)p->ops->recv(p->context, a, buf, sizeof buf);   /* posts a Receive that hangs */
    p->ops->close(p->context, a);
    CHECK(outstanding_count() == 0, "child recv token cancel+drained on close");
    CHECK(g_destroy_child_calls >= 1, "accepted child destroyed on close");
    p->ops->close(p->context, lfd);
}

/* Post-ExitBootServices: bind/listen/accept all refuse, touching no boot service. */
static void t_accept_post_ebs_refuse(void) {
    T_CASE("accept: post-EBS bind/listen/accept refuse (fail-closed)");
    reset_counters();
    const KlSocketProvider *p = fresh_provider();
    KlSockAddr ba; mk_addr(&ba);
    KlSocketHandle lfd = p->ops->socket(p->context, 2, 1, 0);
    CHECK(kl_handle_valid(lfd), "socket before EBS");
    g_after_ebs = 1;
    CHECK(p->ops->bind(p->context, lfd, &ba) == -1, "bind refuses post-EBS");
    CHECK(p->ops->listen(p->context, lfd, 2) == -1, "listen refuses post-EBS");
    KlSockAddr peer;
    CHECK(!kl_handle_valid(p->ops->accept(p->context, lfd, &peer)), "accept refuses post-EBS");
    g_after_ebs = 0;
    p->ops->close(p->context, lfd);   /* reclaim the slot now that EBS is cleared */
}

/* Pool-full: with every conn slot occupied, an accepted child cannot be adopted —
 * accept returns would-block and the orphan child is destroyed (no accept-into-leak). */
static void t_accept_pool_full(void) {
    T_CASE("accept: conn-pool full -> child not orphaned (backpressure floor)");
    reset_counters();
    const KlSocketProvider *p = fresh_provider();
    KlSockAddr ba; mk_addr(&ba);
    KlSocketHandle fds[64]; int nf = 0;
    KlSocketHandle lfd = p->ops->socket(p->context, 2, 1, 0);
    fds[nf++] = lfd;
    p->ops->listen(p->context, lfd, 2);
    kl_uefi_socket_accept_arm(lfd, 2);   /* arm 2 tokens (backpressure floor: adoption still fails when full) */
    /* Occupy every remaining slot with plain sockets. */
    for (int i = 0; i < 64; i++) {
        KlSocketHandle s = p->ops->socket(p->context, 2, 1, 0);
        if (!kl_handle_valid(s)) break;   /* pool exhausted */
        fds[nf++] = s;
    }
    mock_deliver_connection();
    int before = g_destroy_child_calls;
    KlSockAddr peer;
    KlSocketHandle a = p->ops->accept(p->context, lfd, &peer);
    CHECK(!kl_handle_valid(a), "accept fails when the conn pool is full");
    CHECK(g_destroy_child_calls > before, "the un-adoptable child was destroyed, not orphaned");
    for (int i = 0; i < nf; i++) p->ops->close(p->context, fds[i]);   /* reclaim the pool */
}

/* THE backpressure test (S-7 review): listen arms NO Accept tokens; kl_uefi_socket_accept_arm
 * bounds the armed-token count to the free-capacity `want` the event layer passes. With only
 * `want` tokens armed, only `want` connections can be accepted into firmware children — the
 * rest wait in the TCP backlog (never orphaned in a child Keel can't service). This is real
 * backpressure (Goal 8), not just the "unadoptable child destroyed" cleanup floor. */
static void t_accept_backpressure(void) {
    T_CASE("accept: capacity-gated arming bounds armed tokens (Goal 8 backpressure)");
    reset_counters();
    g_tcp_receive_mode = TOK_COMPLETE_OK; g_tcp_close_mode = TOK_COMPLETE_OK;
    const KlSocketProvider *p = fresh_provider();
    KlSockAddr ba; mk_addr(&ba);
    KlSocketHandle lfd = p->ops->socket(p->context, 2, 1, 0);
    p->ops->bind(p->context, lfd, &ba);
    CHECK(p->ops->listen(p->context, lfd, 4) == 0, "listen ok (backlog 4)");
    CHECK(g_accept_tok_count == 0, "listen arms ZERO tokens (capacity-gated — the fix)");
    CHECK(outstanding_count() == 0, "no Accept tokens outstanding after listen");

    /* Free capacity = 2 -> exactly 2 tokens armed, never the full backlog of 4. */
    CHECK(kl_uefi_socket_accept_arm(lfd, 2) == 2, "accept_arm(2) arms exactly 2");
    CHECK(kl_uefi_socket_accept_arm(lfd, 2) == 2, "accept_arm(2) is idempotent (still 2)");
    CHECK(outstanding_count() == 2, "only 2 Accept tokens outstanding (not 4) — bounded by capacity");

    /* Three connections arrive; only the 2 armed tokens can take one each. */
    CHECK(mock_deliver_connection() == 1, "conn 1 taken (armed token)");
    CHECK(mock_deliver_connection() == 1, "conn 2 taken (armed token)");
    CHECK(mock_deliver_connection() == 0, "conn 3 NOT taken — no armed token (backpressure holds)");

    KlSockAddr peer;
    KlSocketHandle a1 = p->ops->accept(p->context, lfd, &peer);
    KlSocketHandle a2 = p->ops->accept(p->context, lfd, &peer);
    CHECK(kl_handle_valid(a1) && kl_handle_valid(a2), "both armed connections accepted");
    CHECK(!kl_handle_valid(p->ops->accept(p->context, lfd, &peer)),
          "no 3rd child — the excess connection was never accepted into firmware");

    p->ops->close(p->context, a1);
    p->ops->close(p->context, a2);
    p->ops->close(p->context, lfd);
}

/* Stale generation: a closed accepted-child slot bumps its generation, so a captured
 * {handle,gen} is rejected — no stale completion can touch a reused slot. */
static void t_accept_stale_generation(void) {
    T_CASE("accept: stale generation on a reused accepted-child slot");
    reset_counters();
    g_tcp_close_mode = TOK_COMPLETE_OK;
    const KlSocketProvider *p = fresh_provider();
    KlSockAddr ba; mk_addr(&ba);
    KlSocketHandle lfd = p->ops->socket(p->context, 2, 1, 0);
    p->ops->listen(p->context, lfd, 2);
    kl_uefi_socket_accept_arm(lfd, 2);   /* listen no longer auto-arms — arm explicitly */
    mock_deliver_connection();
    KlSockAddr peer;
    KlSocketHandle a = p->ops->accept(p->context, lfd, &peer);
    CHECK(kl_handle_valid(a), "accepted a child");
    unsigned long long gen = kl_uefi_conn_generation_h(a);
    CHECK(kl_uefi_conn_valid_h(a, gen) == 1, "child valid at its live generation");
    p->ops->close(p->context, a);
    CHECK(kl_uefi_conn_valid_h(a, gen) == 0, "stale {handle,gen} rejected after close");
    p->ops->close(p->context, lfd);
}

/* ══════════════════════════════════════════════════════════════════════════════════
 * 6.4b — EFI_UDP4 datagram provider (socket_efi_udp4.c) state-machine tests
 * ══════════════════════════════════════════════════════════════════════════════════ */
static void mk_ipv4(KlSockAddr *a, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint16_t port) {
    uint8_t ip[4] = { b0, b1, b2, b3 };
    kl_sockaddr_from_ipv4(a, ip, port);
}
/* Extract IPv4 bytes+port from a neutral KlSockAddr (via the exported EFI marshaller). */
static int addr_is(const KlSockAddr *a, uint8_t b0, uint8_t b3, uint16_t port) {
    EFI_IPv4_ADDRESS ip; UINT16 p = 0;
    return kl_efi_sockaddr_to_ipv4(a, &ip, &p) == 0 &&
           ip.Addr[0] == b0 && ip.Addr[3] == b3 && p == port;
}
#define AF_INET_ 2
#define SOCK_STREAM_ 1
#define SOCK_DGRAM_  2

/* Normal receive: post one Receive, poll → payload + source + local + RecycleSignal, then
 * RE-ARM (serial receive) and receive a second datagram. */
static void t_udp_recv_normal(void) {
    T_CASE("udp: normal receive (payload + src + local + recycle + serial re-arm)");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    CHECK(kl_handle_valid(fd), "udp socket created");
    CHECK(kl_efi_is_udp_handle(fd), "handle is UDP-tagged");
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops();
    KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);   /* unbound DHCP-ephemeral configure */
    CHECK(g_udp_configure_calls == 1, "unbound configure() Configured once");
    unsigned long long gen = kl_uefi_udp_generation_h(fd);   /* the op identity captured at post */

    memcpy(g_udp_resp, "hello", 5); g_udp_resp_len = 5; g_udp_receive_mode = TOK_COMPLETE_OK;
    CHECK(kl_uefi_udp_post_recv(fd) == 0, "post_recv posts a Receive token");
    CHECK(kl_uefi_udp_recv_posted(fd, gen), "a receive is outstanding");
    unsigned char buf[64]; KlSockAddr src, local; int trunc = 9; size_t nb = 0; int ok = 0;
    int rc = kl_uefi_udp_poll_recv(fd, gen, buf, sizeof(buf), &src, &local, &trunc, &nb, &ok);
    CHECK(rc == KL_UEFI_UDP_OP_DELIVERED, "poll_recv reports DELIVERED");
    CHECK(ok == 1 && nb == 5 && memcmp(buf, "hello", 5) == 0, "payload copied out");
    CHECK(trunc == 0, "not truncated");
    CHECK(addr_is(&src, 10, 3, 53), "source == 10.0.2.3:53 (anti-spoof discriminator)");
    CHECK(addr_is(&local, 10, 15, 49152), "local (dest) == 10.0.2.15:49152");
    CHECK(g_recycle_signals == 1, "RxData RecycleSignal fired exactly once");
    CHECK(!kl_uefi_udp_recv_posted(fd, gen), "no receive outstanding after delivery");

    memcpy(g_udp_resp, "two", 3); g_udp_resp_len = 3;
    CHECK(kl_uefi_udp_post_recv(fd) == 0, "serial re-arm: post_recv again");
    ok = 0; nb = 0;
    rc = kl_uefi_udp_poll_recv(fd, gen, buf, sizeof(buf), &src, &local, &trunc, &nb, &ok);
    CHECK(rc == KL_UEFI_UDP_OP_DELIVERED && ok == 1 && nb == 3 && memcmp(buf, "two", 3) == 0,
          "second datagram delivered");
    CHECK(g_recycle_signals == 2, "second RecycleSignal");
    kl_uefi_udp_close(fd);
    CHECK(kl_uefi_udp_provider_live_count() == 0, "no live udp slots after close");
}

/* Receive truncation: a datagram larger than the caller buffer delivers the prefix + TRUNCATED. */
static void t_udp_recv_truncate(void) {
    T_CASE("udp: over-capacity datagram truncates to the caller buffer");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    unsigned long long gen = kl_uefi_udp_generation_h(fd);
    memset(g_udp_resp, 'A', 20); g_udp_resp_len = 20; g_udp_receive_mode = TOK_COMPLETE_OK;
    CHECK(kl_uefi_udp_post_recv(fd) == 0, "post_recv");
    unsigned char buf[8]; KlSockAddr src, local; int trunc = 0; size_t nb = 0; int ok = 0;
    int rc = kl_uefi_udp_poll_recv(fd, gen, buf, sizeof(buf), &src, &local, &trunc, &nb, &ok);
    CHECK(rc == KL_UEFI_UDP_OP_DELIVERED && ok == 1, "delivered");
    CHECK(nb == 8 && trunc == 1, "prefix filled the 8-byte buffer + TRUNCATED flag set");
    kl_uefi_udp_close(fd);
}

/* Normal transmit: post one Transmit, poll → ok + bytes; the dest landed in the session. */
static void t_udp_send_normal(void) {
    T_CASE("udp: normal transmit (dest in session, completes ok)");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    unsigned long long gen = kl_uefi_udp_generation_h(fd);
    g_udp_transmit_mode = TOK_COMPLETE_OK;
    KlSockAddr dest; mk_ipv4(&dest, 10, 0, 2, 3, 53);
    CHECK(kl_uefi_udp_post_send(fd, "q", 1, &dest) == 0, "post_send posts a Transmit");
    size_t nb = 0; int ok = 0;
    int rc = kl_uefi_udp_poll_send(fd, gen, &nb, &ok);
    CHECK(rc == KL_UEFI_UDP_OP_DELIVERED && ok == 1 && nb == 1, "poll_send DELIVERED, 1 byte");
    CHECK(g_udp_tx_calls == 1 && g_udp_tx_dst[0] == 10 && g_udp_tx_dst[3] == 3 && g_udp_tx_dport == 53,
          "destination captured in the Tx session");
    kl_uefi_udp_close(fd);
}

/* Confirmed cancellation: a hung Receive is Cancel+drained (firmware CAN cancel) → retired,
 * then close tears the child down cleanly (not quarantined). */
static void t_udp_cancel_confirmed(void) {
    T_CASE("udp: confirmed cancel retires the token → clean close");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    unsigned long long gen = kl_uefi_udp_generation_h(fd);
    g_udp_receive_mode = TOK_HANG;
    CHECK(kl_uefi_udp_post_recv(fd) == 0, "post_recv (hangs — no completion)");
    g_cancel_signals = 1;
    CHECK(kl_uefi_udp_cancel_recv(fd, gen) == KL_UEFI_UDP_OP_RETIRED, "cancel_recv confirms retirement");
    CHECK(g_udp_cancel_calls >= 1, "Cancel(token) was called");
    CHECK(!kl_uefi_udp_recv_posted(fd, gen), "no receive outstanding after confirmed cancel");
    kl_uefi_udp_close(fd);
    CHECK(g_destroy_child_calls >= 1, "clean teardown (DestroyChild) after confirmed cancel");
    CHECK(kl_uefi_udp_provider_quarantined_count() == 0, "not quarantined");
}

/* Unconfirmed quarantine: a hung Receive whose Cancel canNOT be confirmed at close →
 * QUARANTINE (child NOT destroyed, leaked to EBS), and a fresh socket is still available. */
/* RECEIVE quarantine: an unconfirmed cancel at close leaks the slot AND — critically — a later
 * op query for the quarantined op reports QUARANTINED (not STALE_RETIRED), so the event layer will
 * RETAIN the B.6 life ref rather than release it. */
static void t_udp_quarantine_unconfirmed(void) {
    T_CASE("udp: unconfirmed Rx cancel at close → quarantine (leak) + QUARANTINED op result");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    unsigned long long gen = kl_uefi_udp_generation_h(fd);
    g_udp_receive_mode = TOK_HANG;
    CHECK(kl_uefi_udp_post_recv(fd) == 0, "post_recv (hangs)");
    g_cancel_signals = 0;   /* firmware CANNOT cancel → drain fails → quarantine */
    kl_uefi_udp_close(fd);
    CHECK(g_udp_cancel_all_calls >= 1, "close attempted Cancel(NULL)");
    CHECK(g_destroy_child_calls == 0, "quarantine: child NOT destroyed (leaked to EBS)");
    CHECK(kl_uefi_udp_provider_quarantined_count() == 1, "one quarantined slot");
    CHECK(kl_uefi_udp_provider_live_count() == 0, "quarantined slot is not live");
    /* The op-result distinction (review-High #2): the quarantined op must NOT look like a clean
     * retirement — poll/cancel/state all report QUARANTINED so the event layer retains life. */
    unsigned char buf[16]; KlSockAddr s, l; int tr = 0; size_t nb = 0; int ok = 0;
    CHECK(kl_uefi_udp_op_state(fd, gen) == KL_UEFI_UDP_OP_QUARANTINED, "op_state == QUARANTINED (retain life)");
    CHECK(kl_uefi_udp_poll_recv(fd, gen, buf, sizeof(buf), &s, &l, &tr, &nb, &ok) == KL_UEFI_UDP_OP_QUARANTINED,
          "poll_recv on the quarantined op == QUARANTINED (not STALE_RETIRED)");
    CHECK(kl_uefi_udp_cancel_recv(fd, gen) == KL_UEFI_UDP_OP_QUARANTINED,
          "cancel_recv on the quarantined op == QUARANTINED (not RETIRED)");
    KlSocketHandle fd2 = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    CHECK(kl_handle_valid(fd2), "a fresh udp socket still available after quarantine");
    kl_uefi_udp_close(fd2);
}

/* TRANSMIT quarantine: same distinction for the Tx op — an unconfirmed Tx cancel at close reports
 * QUARANTINED (never RETIRED), so the event layer retains the send op's life ref. */
static void t_udp_quarantine_tx(void) {
    T_CASE("udp: unconfirmed Tx cancel at close → quarantine + QUARANTINED op result");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    unsigned long long gen = kl_uefi_udp_generation_h(fd);
    int q_before = kl_uefi_udp_provider_quarantined_count();   /* prior udp tests may have leaked slots */
    g_udp_transmit_mode = TOK_HANG;
    KlSockAddr dest; mk_ipv4(&dest, 10, 0, 2, 3, 53);
    CHECK(kl_uefi_udp_post_send(fd, "q", 1, &dest) == 0, "post_send (hangs)");
    g_cancel_signals = 0;
    kl_uefi_udp_close(fd);
    CHECK(g_destroy_child_calls == 0, "quarantine: child NOT destroyed");
    CHECK(kl_uefi_udp_provider_quarantined_count() == q_before + 1, "this Tx op quarantined one more slot");
    size_t nb = 0; int ok = 0;
    CHECK(kl_uefi_udp_op_state(fd, gen) == KL_UEFI_UDP_OP_QUARANTINED, "op_state == QUARANTINED");
    CHECK(kl_uefi_udp_poll_send(fd, gen, &nb, &ok) == KL_UEFI_UDP_OP_QUARANTINED,
          "poll_send on the quarantined Tx op == QUARANTINED");
    CHECK(kl_uefi_udp_cancel_send(fd, gen) == KL_UEFI_UDP_OP_QUARANTINED,
          "cancel_send on the quarantined Tx op == QUARANTINED (not RETIRED)");
    KlSocketHandle fd2 = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    CHECK(kl_handle_valid(fd2), "a fresh udp socket still available after Tx quarantine");
    kl_uefi_udp_close(fd2);
}

/* SYNCHRONOUS dgram->send quarantine (review-High): a sync send whose Transmit times out AND whose
 * cancel is unconfirmed must go through the SAME centralized quarantine transition as close — bumping
 * the generation exactly once so {fd, captured_gen} classifies as QUARANTINED (not STALE_RETIRED),
 * retaining the child/events/token (no DestroyChild/CloseEvent), and never reusing the slot. */
static void t_udp_sync_send_quarantine(void) {
    T_CASE("udp: sync dgram->send timeout + unconfirmed cancel → centralized quarantine (gen bumped once)");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    unsigned long long gen = kl_uefi_udp_generation_h(fd);
    int q_before = kl_uefi_udp_provider_quarantined_count();
    g_udp_transmit_mode = TOK_HANG;   /* Transmit posts but never completes */
    g_cancel_signals    = 0;          /* and the cancel-drain cannot confirm → quarantine */
    KlSockAddr dest; mk_ipv4(&dest, 10, 0, 2, 3, 53);
    CHECK(ops->send(NULL, fd, "x", 1, &dest, NULL, -1) < 0, "sync send fails on transmit timeout");
    CHECK(g_destroy_child_calls == 0, "quarantine: no DestroyChild (child retained)");
    CHECK(g_close_proto_calls == 0, "quarantine: no CloseProtocol (child retained)");
    CHECK(kl_uefi_udp_provider_quarantined_count() == q_before + 1, "this sync send quarantined one slot");
    CHECK(kl_uefi_udp_provider_live_count() == 0, "quarantined slot is not live (never reusable)");
    /* The load-bearing invariant: {fd, captured_gen} → QUARANTINED (generation bumped exactly once). */
    CHECK(kl_uefi_udp_op_state(fd, gen) == KL_UEFI_UDP_OP_QUARANTINED,
          "op {fd, captured_gen} == QUARANTINED (NOT STALE_RETIRED — generation bumped once)");
    KlSocketHandle fd2 = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    CHECK(kl_handle_valid(fd2) && fd2 != fd, "a fresh socket uses a DIFFERENT slot (quarantined one leaked)");
    kl_uefi_udp_close(fd2);
}

/* ── event_efi datagram completion wiring (6.4b-3b) — B.6 KlDgramLife lifetime ─────────────
 * These drive event_efi's post_dgram_recv/_send + drain against a REAL KlDgramLife over a REAL
 * EFI_UDP4 socket (fake firmware), proving the frozen op-result → life contract:
 *   DELIVERED     → the event carries the transferred ref; after dispatch releases it AND the owner
 *                   ref is dropped, on_final runs (release on delivery + confirmed retirement);
 *   STALE_RETIRED → drain releases the op ref → on_final runs;
 *   QUARANTINED   → drain RETAINS the ref forever → on_final NEVER runs (Rx and Tx). */
static KlSocketHandle dgl_socket(void) {
    fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops();
    KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    return fd;
}

static void t_dgram_life_delivered_recv(void) {
    T_CASE("dgram life: DELIVERED transfers the ref → dispatch+owner release run on_final");
    reset_counters(); talloc_reset(); g_on_final_ran = 0;
    kl_uefi_event_provider_reset();
    const KlEventProvider *ep = kl_uefi_event_provider(&g_bs, (EFI_HANDLE)0x1);
    KlSocketHandle fd = dgl_socket();
    int owner = 0;
    KlDgramLife *life = kl_dgram_life_create(&g_ta, &owner, mock_on_final, NULL);   /* refcount 1 (owner) */
    CHECK(life != NULL, "KlDgramLife created (owner ref)");
    unsigned char rbuf[64];
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    dg.fd = fd; dg.recv_buf = rbuf; dg.recv_buf_size = sizeof(rbuf); dg.rx_life = life;
    memcpy(g_udp_resp, "abc", 3); g_udp_resp_len = 3; g_udp_receive_mode = TOK_COMPLETE_OK;
    CHECK(COMP(ep)->post_dgram_recv(&dg) == 0, "post_dgram_recv (op ref retained → 2)");
    KlCompletionEvent evs[4];
    int dn = COMP(ep)->drain(NULL, evs, 4, 0);
    int idx = -1; for (int i = 0; i < dn; i++) if (evs[i].kind == KL_COMP_DGRAM_RECV) idx = i;
    CHECK(idx >= 0, "drain emitted KL_COMP_DGRAM_RECV");
    CHECK(idx >= 0 && evs[idx].life == life, "event carries the TRANSFERRED life ref");
    CHECK(idx >= 0 && evs[idx].ok == 1 && evs[idx].bytes == 3, "delivered payload (3 bytes)");
    CHECK(g_on_final_ran == 0, "on_final NOT run yet (event + owner refs outstanding)");
    if (idx >= 0) kl_dgram_life_release(evs[idx].life);   /* dispatch releases after delivery */
    CHECK(g_on_final_ran == 0, "still not run (owner ref remains)");
    kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);   /* owner drop (kl_udp_free) */
    CHECK(g_on_final_ran == 1, "on_final RAN once event + owner refs released (confirmed retirement)");
    kl_uefi_udp_close(fd); kl_uefi_event_provider_reset(); talloc_free_all();
}

static void t_dgram_life_stale_release_recv(void) {
    T_CASE("dgram life: STALE_RETIRED (clean close) releases the op ref → on_final runs");
    reset_counters(); talloc_reset(); g_on_final_ran = 0;
    kl_uefi_event_provider_reset();
    const KlEventProvider *ep = kl_uefi_event_provider(&g_bs, (EFI_HANDLE)0x1);
    KlSocketHandle fd = dgl_socket();
    int owner = 0;
    KlDgramLife *life = kl_dgram_life_create(&g_ta, &owner, mock_on_final, NULL);
    unsigned char rbuf[64];
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    dg.fd = fd; dg.recv_buf = rbuf; dg.recv_buf_size = sizeof(rbuf); dg.rx_life = life;
    g_udp_receive_mode = TOK_HANG;   /* posted, never completes — reaped cleanly at close */
    CHECK(COMP(ep)->post_dgram_recv(&dg) == 0, "post_dgram_recv (→ 2)");
    g_cancel_signals = 1;
    kl_uefi_udp_close(fd);   /* clean close reaps the token, bumps the generation */
    kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);   /* owner drop → refcount 1 (op) */
    CHECK(g_on_final_ran == 0, "on_final not run yet (op ref remains after owner drop)");
    KlCompletionEvent evs[4];
    int dn = COMP(ep)->drain(NULL, evs, 4, 0);
    int emitted = 0; for (int i = 0; i < dn; i++) if (evs[i].kind == KL_COMP_DGRAM_RECV) emitted = 1;
    CHECK(emitted == 0, "no event on a stale (cleanly-retired) op");
    CHECK(g_on_final_ran == 1, "STALE_RETIRED released the op ref → on_final RAN");
    kl_uefi_event_provider_reset(); talloc_free_all();
}

static void t_dgram_life_quarantine_recv(void) {
    T_CASE("dgram life: QUARANTINED Rx RETAINS the ref forever → on_final NEVER runs");
    reset_counters(); talloc_reset(); g_on_final_ran = 0;
    kl_uefi_event_provider_reset();
    const KlEventProvider *ep = kl_uefi_event_provider(&g_bs, (EFI_HANDLE)0x1);
    KlSocketHandle fd = dgl_socket();
    int owner = 0;
    KlDgramLife *life = kl_dgram_life_create(&g_ta, &owner, mock_on_final, NULL);
    unsigned char rbuf[64];
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    dg.fd = fd; dg.recv_buf = rbuf; dg.recv_buf_size = sizeof(rbuf); dg.rx_life = life;
    g_udp_receive_mode = TOK_HANG;
    CHECK(COMP(ep)->post_dgram_recv(&dg) == 0, "post_dgram_recv (→ 2)");
    g_cancel_signals = 0;
    kl_uefi_udp_close(fd);   /* unconfirmed → quarantine */
    kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);   /* owner drop → refcount 1 (op) */
    KlCompletionEvent evs[4];
    (void)COMP(ep)->drain(NULL, evs, 4, 0);   /* QUARANTINED → RETAIN (no release) */
    CHECK(g_on_final_ran == 0, "QUARANTINED Rx: op ref RETAINED → on_final NEVER runs");
    talloc_free_all();   /* reclaim the intentionally-leaked life WITHOUT release (LSan-clean) */
    CHECK(g_on_final_ran == 0, "on_final still never ran (retention proven)");
    kl_uefi_event_provider_reset();
}

static void t_dgram_life_quarantine_send(void) {
    T_CASE("dgram life: QUARANTINED Tx RETAINS the ref forever → on_final NEVER runs");
    reset_counters(); talloc_reset(); g_on_final_ran = 0;
    kl_uefi_event_provider_reset();
    const KlEventProvider *ep = kl_uefi_event_provider(&g_bs, (EFI_HANDLE)0x1);
    KlSocketHandle fd = dgl_socket();
    int owner = 0;
    KlDgramLife *life = kl_dgram_life_create(&g_ta, &owner, mock_on_final, NULL);
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    dg.fd = fd; dg.rx_life = life;
    g_udp_transmit_mode = TOK_HANG;
    KlSockAddr dest; mk_ipv4(&dest, 10, 0, 2, 3, 53);
    CHECK(COMP(ep)->post_dgram_send(&dg, "x", 1, &dest) == 0, "post_dgram_send (→ 2)");
    g_cancel_signals = 0;
    kl_uefi_udp_close(fd);   /* unconfirmed → quarantine (Tx) */
    kl_dgram_life_mark_dead(life); kl_dgram_life_release(life);   /* owner drop → refcount 1 (op) */
    KlCompletionEvent evs[4];
    (void)COMP(ep)->drain(NULL, evs, 4, 0);
    CHECK(g_on_final_ran == 0, "QUARANTINED Tx: op ref RETAINED → on_final NEVER runs");
    talloc_free_all();
    CHECK(g_on_final_ran == 0, "on_final still never ran (retention proven)");
    kl_uefi_event_provider_reset();
}

/* poll_recv no-copy mode: a LIVE signalled receive polled with copy_into==NULL recycles the
 * firmware RxData without copying (a drain-without-deliver). (This is NOT the stale path — the
 * op is live; stale is exercised by t_udp_stale_generation_drop below.) */
static void t_udp_poll_recv_null_copy(void) {
    T_CASE("udp: poll_recv NULL-copy mode recycles a live signalled token without copying");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    unsigned long long gen = kl_uefi_udp_generation_h(fd);
    memcpy(g_udp_resp, "data", 4); g_udp_resp_len = 4; g_udp_receive_mode = TOK_COMPLETE_OK;
    CHECK(kl_uefi_udp_post_recv(fd) == 0, "post_recv");
    int trunc = 9; size_t nb = 7; int ok = 1;
    int rc = kl_uefi_udp_poll_recv(fd, gen, NULL, 0, NULL, NULL, &trunc, &nb, &ok);
    CHECK(rc == KL_UEFI_UDP_OP_DELIVERED, "poll_recv reports DELIVERED (live op)");
    CHECK(ok == 0 && nb == 0, "no delivery (copy_into==NULL)");
    CHECK(g_recycle_signals == 1, "firmware RxData still recycled (no leak)");
    kl_uefi_udp_close(fd);
}

/* REAL stale-generation drop (review-High/Medium): post a receive, close the slot (reaping the
 * token), then REUSE the same slot with a NEW socket (same handle value, new generation) that posts
 * its OWN receive. Polling the OLD op {fd, old-gen} must be a terminal-DROP that does NOT touch/
 * consume the reused slot's token — proving the completion op is selected by captured generation
 * over stable storage, never by udp_of(fd). The new op then delivers its own datagram correctly. */
static void t_udp_stale_generation_drop(void) {
    T_CASE("udp: stale-generation poll drops without touching the reused slot's token");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    unsigned long long oldgen = kl_uefi_udp_generation_h(fd);
    memcpy(g_udp_resp, "old", 3); g_udp_resp_len = 3; g_udp_receive_mode = TOK_COMPLETE_OK;
    CHECK(kl_uefi_udp_post_recv(fd) == 0, "post_recv on the original op");
    kl_uefi_udp_close(fd);   /* reaps + recycles the original token (Cancel+drain) before reuse */
    CHECK(g_recycle_signals == 1, "close reaped+recycled the original signalled token");

    KlSocketHandle fd2 = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    CHECK(fd2 == fd, "the slot was REUSED (same handle value)");
    unsigned long long newgen = kl_uefi_udp_generation_h(fd2);
    CHECK(newgen != oldgen, "the reused slot has a DIFFERENT generation");
    (void)ops->configure(NULL, fd2, AF_INET_, &cfg);
    memcpy(g_udp_resp, "new", 3); g_udp_resp_len = 3;
    CHECK(kl_uefi_udp_post_recv(fd2) == 0, "post_recv on the reused slot (new op)");

    int recyc_before = g_recycle_signals;
    unsigned char buf[16]; KlSockAddr s, l; int tr = 0; size_t nb = 9; int ok = 1;
    int rc = kl_uefi_udp_poll_recv(fd, oldgen, buf, sizeof(buf), &s, &l, &tr, &nb, &ok);
    CHECK(rc == KL_UEFI_UDP_OP_STALE_RETIRED, "stale (cleanly-retired) poll → STALE_RETIRED (release life)");
    CHECK(ok == 0 && nb == 0, "no delivery on the stale op");
    CHECK(g_recycle_signals == recyc_before, "did NOT touch/recycle the reused slot's token");
    CHECK(kl_uefi_udp_op_state(fd, oldgen) == KL_UEFI_UDP_OP_STALE_RETIRED, "op_state == STALE_RETIRED");
    CHECK(kl_uefi_udp_recv_posted(fd2, newgen), "the NEW op is still outstanding (untouched)");

    ok = 0; nb = 0;
    rc = kl_uefi_udp_poll_recv(fd2, newgen, buf, sizeof(buf), &s, &l, &tr, &nb, &ok);
    CHECK(rc == KL_UEFI_UDP_OP_DELIVERED && ok == 1 && nb == 3 && memcmp(buf, "new", 3) == 0,
          "new op delivers its own datagram");
    kl_uefi_udp_close(fd2);
}

/* Handle discrimination: a TCP handle is not a UDP slot and vice-versa; a cross-provider
 * close is a no-op (never tears down the other transport's child). */
static void t_udp_cross_transport_reject(void) {
    T_CASE("udp: cross-transport handle rejection (two independent tagged pools)");
    reset_counters();
    const KlSocketProvider *tcp = fresh_provider();
    fresh_udp();
    KlSocketHandle tcpfd = tcp->ops->socket(tcp->context, AF_INET_, SOCK_STREAM_, 0);
    KlSocketHandle udpfd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    CHECK(kl_handle_valid(tcpfd) && kl_handle_valid(udpfd), "both sockets created");
    CHECK(!kl_efi_is_udp_handle(tcpfd), "TCP handle is NOT UDP-tagged");
    CHECK(kl_efi_is_udp_handle(udpfd), "UDP handle IS UDP-tagged");
    CHECK(kl_uefi_udp_generation_h(tcpfd) == 0, "udp_generation of a TCP handle = 0 (rejected)");
    CHECK(!kl_uefi_udp_valid_h(tcpfd, 1), "udp_valid_h rejects a TCP handle");
    CHECK(kl_uefi_conn_generation_h(udpfd) == 0, "tcp_generation of a UDP handle = 0 (rejected)");
    CHECK(!kl_uefi_conn_valid_h(udpfd, 1), "tcp conn_valid_h rejects a UDP handle");
    int destroy_before = g_destroy_child_calls;
    kl_uefi_udp_close(tcpfd);   /* udp close of a TCP handle → no-op */
    CHECK(g_destroy_child_calls == destroy_before, "udp close of a TCP handle tears down nothing");
    tcp->ops->close(tcp->context, tcpfd);
    kl_uefi_udp_close(udpfd);
}

/* Unified provider (6.4b-3a): ONE KlSocketProvider serves SOCK_STREAM (EFI_TCP4) AND SOCK_DGRAM
 * (EFI_UDP4). socket() routes by type; the datagram data-plane is on .dgram; close routes by tag. */
static void t_udp_unified_provider(void) {
    T_CASE("udp: unified provider routes SOCK_DGRAM → EFI_UDP4 (.dgram + tag-routed close)");
    reset_counters();
    const KlSocketProvider *p = fresh_provider();   /* now also inits the datagram side */
    CHECK(p != NULL, "provider created");
    CHECK((p->capabilities & KL_SOCK_CAP_DATAGRAM) != 0, "provider advertises KL_SOCK_CAP_DATAGRAM");
    CHECK(p->dgram != NULL && p->dgram->configure != NULL, "provider exposes the .dgram data-plane");

    KlSocketHandle ufd = p->ops->socket(p->context, AF_INET_, SOCK_DGRAM_, 0);
    KlSocketHandle tfd = p->ops->socket(p->context, AF_INET_, SOCK_STREAM_, 0);
    CHECK(kl_handle_valid(ufd) && kl_efi_is_udp_handle(ufd), "SOCK_DGRAM → a UDP-tagged handle");
    CHECK(kl_handle_valid(tfd) && !kl_efi_is_udp_handle(tfd), "SOCK_STREAM → a stream handle (untagged)");

    /* A datagram round-trips through the unified provider's .dgram + completion primitives. */
    KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)p->dgram->configure(p->context, ufd, AF_INET_, &cfg);
    unsigned long long gen = kl_uefi_udp_generation_h(ufd);
    memcpy(g_udp_resp, "ok", 2); g_udp_resp_len = 2; g_udp_receive_mode = TOK_COMPLETE_OK;
    CHECK(kl_uefi_udp_post_recv(ufd) == 0, "post_recv on the unified-provider datagram socket");
    unsigned char buf[8]; KlSockAddr s, l; int tr = 0; size_t nb = 0; int ok = 0;
    CHECK(kl_uefi_udp_poll_recv(ufd, gen, buf, sizeof(buf), &s, &l, &tr, &nb, &ok) == KL_UEFI_UDP_OP_DELIVERED
          && nb == 2, "datagram delivered via the unified provider");

    /* Provider close on the UDP handle routes to the datagram teardown (DestroyChild). */
    int destroy_before = g_destroy_child_calls;
    CHECK(p->ops->close(p->context, ufd) == 0, "provider close on a UDP handle");
    CHECK(g_destroy_child_calls == destroy_before + 1, "datagram close tore down the EFI_UDP4 child");
    p->ops->close(p->context, tfd);
}

/* Configuration: an explicit bind_addr is configured EXACTLY ONCE (configure() defers, bind()
 * does the single Configure) — no EFI_ALREADY_STARTED double-Configure. */
static void t_udp_bind_single_configure(void) {
    T_CASE("udp: explicit bind Configures exactly once (deferred from configure)");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops();
    KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg)); cfg.bind_addr = "10.0.2.15"; cfg.bind_port = 5300;
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    CHECK(g_udp_configure_calls == 0, "configure() DEFERS the bind_addr case (no Configure yet)");
    KlSockAddr b; mk_ipv4(&b, 10, 0, 2, 15, 5300);
    CHECK(kl_uefi_udp_bind(fd, &b) == 0, "bind configures the station");
    CHECK(g_udp_configure_calls == 1, "exactly ONE Configure for a bound socket (no double)");
    KlSockAddr la;
    CHECK(kl_uefi_udp_get_local_addr(fd, &la) == 0 && addr_is(&la, 10, 15, 49152),
          "get_local_addr yields the station via GetModeData");
    kl_uefi_udp_close(fd);
}

/* Configuration failure: a failing EFI_UDP4.Configure fail-closes the slot (full teardown) so
 * every subsequent op fails — no "init succeeds with an unusable socket". */
static void t_udp_configure_failclose(void) {
    T_CASE("udp: Configure failure fail-closes the slot (next op fails)");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops();
    g_udp_configure_status = EFI_INVALID_PARAMETER;   /* force Configure to fail */
    KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));     /* unbound → configures here */
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    CHECK(g_destroy_child_calls >= 1, "fail-close tore down the child (DestroyChild)");
    CHECK(kl_uefi_udp_post_recv(fd) == -1, "a subsequent op fails on the fail-closed slot");
    CHECK(kl_uefi_udp_provider_live_count() == 0, "slot not live after fail-close");
}

/* dgram->send provide-or-reject: an explicit per-datagram TOS is rejected (send nothing); a
 * valid source pin lands in the session BEFORE submit; an invalid source is rejected. */
static void t_udp_send_tos_and_srcpin(void) {
    T_CASE("udp: dgram->send TOS-reject + source-pin (valid lands pre-submit, invalid rejected)");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    g_udp_transmit_mode = TOK_COMPLETE_OK;
    KlSockAddr dest; mk_ipv4(&dest, 10, 0, 2, 3, 53);
    CHECK(ops->send(NULL, fd, "x", 1, &dest, NULL, 0) < 0, "explicit per-datagram TOS rejected");
    CHECK(g_udp_tx_calls == 0, "no Transmit on TOS reject (send nothing)");
    KlSockAddr src; mk_ipv4(&src, 10, 0, 2, 99, 1234);
    CHECK(ops->send(NULL, fd, "x", 1, &dest, &src, -1) == 1, "source-pinned send accepted");
    CHECK(g_udp_tx_calls == 1 && g_udp_tx_src[0] == 10 && g_udp_tx_src[3] == 99 && g_udp_tx_sport == 1234,
          "SourceAddress landed in the session BEFORE submit (no post-submit mutation)");
    KlSockAddr bad; memset(&bad, 0, sizeof(bad));   /* UNSPEC family — not IPv4 */
    CHECK(ops->send(NULL, fd, "x", 1, &dest, &bad, -1) < 0, "invalid (non-IPv4) source rejected");
    CHECK(g_udp_tx_calls == 1, "no additional Transmit on invalid-source reject");
    kl_uefi_udp_close(fd);
}

/* Post-EBS fail-closed: after ExitBootServices, every datagram op refuses + touches no firmware. */
static void t_udp_after_ebs_refuses(void) {
    T_CASE("udp: post-ExitBootServices ops are fail-closed (no firmware calls)");
    reset_counters(); fresh_udp();
    KlSocketHandle fd = kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0);
    const KlDatagramOps *ops = kl_uefi_udp_dgram_ops(); KlUdpConfig cfg; memset(&cfg, 0, sizeof(cfg));
    (void)ops->configure(NULL, fd, AF_INET_, &cfg);
    g_after_ebs = 1;
    int fw_before = g_firmware_calls;
    CHECK(kl_uefi_udp_socket(AF_INET_, SOCK_DGRAM_, 0) == KL_INVALID_SOCKET, "socket refused post-EBS");
    CHECK(kl_uefi_udp_post_recv(fd) == -1, "post_recv refused post-EBS");
    CHECK(g_firmware_calls == fw_before, "no firmware call after EBS");
    g_after_ebs = 0;
    kl_uefi_udp_close(fd);
}

int main(void) {
    printf("=== mock-EFI failure-path harness (F7b) ===\n");
    mock_init();

    /* 6.4b EFI_UDP4 datagram provider — before the pool-leaking quarantine cases below. */
    t_udp_recv_normal();
    t_udp_recv_truncate();
    t_udp_send_normal();
    t_udp_cancel_confirmed();
    t_udp_poll_recv_null_copy();
    t_udp_stale_generation_drop();
    t_udp_cross_transport_reject();
    t_udp_unified_provider();
    t_udp_bind_single_configure();
    t_udp_configure_failclose();
    t_udp_send_tos_and_srcpin();
    t_udp_after_ebs_refuses();
    t_dgram_life_delivered_recv();    /* 6.4b-3b event_efi wiring — clean (no slot leak) */
    t_dgram_life_stale_release_recv();
    t_udp_quarantine_unconfirmed();   /* leaks one udp slot by design — runs last of the udp set */
    t_udp_quarantine_tx();            /* leaks one udp slot by design */
    t_udp_sync_send_quarantine();     /* leaks one udp slot by design */
    t_dgram_life_quarantine_recv();   /* leaks one udp slot + retains a life by design */
    t_dgram_life_quarantine_send();   /* leaks one udp slot + retains a life by design */

    t_connect_timeout_close();
    t_transmit_timeout();
    t_receive_pending_close();
    t_dns_transmit_timeout();
    t_dns_receive_timeout();
    t_cancel_racing_completion();
    t_close_token_timeout();
    t_after_ebs_refuses();
    t_entropy_fail_closed();           /* links entropy_uefi.c (mbedTLS-free) */
    t_civil_time();                    /* cert validity-time conversion + fail-closed (U-8) */
    t_wallclock_decode();              /* EFI_TIME decode + unspecified-TZ policy (U-8) */
    t_platform_ready_gate();           /* frozen-monotonic-timer gate on the snapshot (U-8) */
    t_clock_snapshot();                /* TLS-platform-lifetime snapshot clock gate (U-8) */
    t_stale_guard_no_uaf();
    t_cancel_fails_quarantine();
    t_close_no_spin_on_consumed_connect();
    t_bad_rx_length();
    /* S-5: server accept-lifetime (verifies S-2 bind/listen/accept + S-3 post_accept).
     * The cancel-fail quarantine case permanently leaks its listener slot (by design),
     * so it runs after the pool-sensitive cases (each of which reclaims its slots). */
    t_accept_basic();
    t_accept_listener_close_drains();
    t_accepted_child_close_recv();
    t_accept_post_ebs_refuse();
    t_accept_pool_full();
    t_accept_backpressure();
    t_accept_stale_generation();
    t_accept_cancel_fail_quarantine();   /* last: intentional permanent slot leak */
    t_dns_cancel_fails_quarantine();   /* last: sets the sticky DNS quarantine */

    printf(g_fail ? "\nmock-EFI harness: FAIL\n" : "\nmock-EFI harness: PASS\n");
    return g_fail;
}
