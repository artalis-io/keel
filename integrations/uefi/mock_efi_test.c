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
#include "../../spikes/uefi/efi_udp4.h"   /* EFI_UDP4_* types for the UDP mock */
#include "dns_uefi.h"
#include "event_efi.h"

#include <keel/sockaddr.h>
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

/* dns_uefi.c draws a query id via kl_plat_random (src/platform.h); libkeel.a does not
 * define the freestanding platform hook, so the harness provides a deterministic one. */
#include <stddef.h>
void kl_plat_random(void *buf, size_t len);
void kl_plat_random(void *buf, size_t len) { memset(buf, 0x5a, len); }

/* dns_uefi.c draws a query id via kl_plat_random (src/platform.h) — libkeel.a defines
 * it; nothing to stub. */

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
static int g_udp_cancel_calls;
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
    if (cd) memset(cd, 0, sizeof(*cd));
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
static EFI_STATUS EFIAPI m_tcp_Accept(EFI_TCP4_PROTOCOL *This, VOID *t) { (void)This; (void)t; FW(); return EFI_UNSUPPORTED; }
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

/* --- UDP4 protocol --- */
static EFI_STATUS EFIAPI m_udp_GetModeData(EFI_UDP4_PROTOCOL *This, EFI_UDP4_CONFIG_DATA *cd,
                                           VOID *ip, VOID *mnp, VOID *snp) {
    (void)This; (void)cd; (void)ip; (void)mnp; (void)snp; FW(); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI m_udp_Configure(EFI_UDP4_PROTOCOL *This, EFI_UDP4_CONFIG_DATA *cd) {
    (void)This; FW(); if (cd == NULL) g_configure_null_calls++; return EFI_SUCCESS;
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
    (void)This; FW(); g_udp_cancel_calls++;
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
    g_tcp_cancel_calls = g_tcp_cancel_all_calls = g_udp_cancel_calls = 0;
    g_destroy_child_calls = g_close_proto_calls = g_free_pool_calls = 0;
    g_configure_null_calls = g_recycle_signals = g_firmware_calls = 0;
    g_cancel_signals = 1; g_tcp_poll_calls = 0; g_tcp_rx_datalen_override = 0;
    tok_reset();
    g_event_count = 0;
    for (int i = 0; i < MAX_EVENTS; i++) memset(&g_events[i], 0, sizeof(g_events[i]));
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

int main(void) {
    printf("=== mock-EFI failure-path harness (F7b) ===\n");
    mock_init();

    t_connect_timeout_close();
    t_transmit_timeout();
    t_receive_pending_close();
    t_dns_transmit_timeout();
    t_dns_receive_timeout();
    t_cancel_racing_completion();
    t_close_token_timeout();
    t_after_ebs_refuses();
    t_entropy_fail_closed();           /* links entropy_uefi.c (mbedTLS-free) */
    t_stale_guard_no_uaf();
    t_cancel_fails_quarantine();
    t_close_no_spin_on_consumed_connect();
    t_bad_rx_length();
    t_dns_cancel_fails_quarantine();   /* last: sets the sticky DNS quarantine */

    printf(g_fail ? "\nmock-EFI harness: FAIL\n" : "\nmock-EFI harness: PASS\n");
    return g_fail;
}
