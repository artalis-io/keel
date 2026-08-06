/*
 * event_efi.c — completion-axis KlEventProvider over EFI_TCP4 tokens (U-3, F-8).
 * See event_efi.h for the design. This is the direct real-firmware analogue of the
 * mock completion loop in tests/freestanding_harness.c: connect rides the completion
 * axis (KL_COMP_CONNECT), send/recv ride the U-2 socket provider's SYNC ops driven by
 * an emulated-readiness watcher relay (KL_COMP_WATCHER). No libc, no errno.
 */

#include "event_efi.h"
#include "socket_efi_tcp4.h"          /* kl_uefi_socket_provider + async-connect prims */
#include "platform_uefi.h"            /* kl_uefi_after_ebs (F3 boot-services guard) */

#include <keel/event.h>
#include <keel/sockaddr.h>
#include "../../src/socket.h"          /* KlSocketProvider, KlSocketHandle, kl_handle_valid */
#include "../../src/completion.h"      /* KlCompletionOps, KlCompletionEvent, KL_COMP_* */

#include <stdint.h>
#include <stddef.h>

/* Bounds match the client's needs: it drives ONE connection (one connect op, a
 * handful of live watches). Small fixed arrays — no allocation in the loop. */
#define KL_EFI_MAX_CONN_OPS 8
#define KL_EFI_MAX_WATCHES  8

/* A queued outbound connect (post_connect). Its terminal result is surfaced as a
 * KL_COMP_CONNECT on a later drain (once the Connect token fires), then retired. The
 * captured generation is the stale guard: a completion for a conn that was closed
 * (generation bumped) or whose memory was reused (magic cleared) is dropped. */
typedef struct {
    int             in_use;
    KlSocketHandle  fd;              /* the KlUefiConn handle */
    uint64_t        generation;     /* captured at post time (kl_uefi_conn_generation_h) */
    void           *watcher_udata;  /* the tagged KlWatcher the client registered */
    int             failed;         /* Configure/Connect-post failed → deliver ok=0 */
} EfiConnOp;

/* A registered tagged watch (add/mod). drain relays its armed mask as a
 * KL_COMP_WATCHER so the client retries the SYNC send/recv on the U-2 provider. */
typedef struct {
    int             in_use;
    KlSocketHandle  fd;
    KlEventMask     mask;
    void           *udata;          /* tagged KlWatcher pointer (LSB=1) */
} EfiWatch;

typedef struct {
    EFI_BOOT_SERVICES *bs;
    EFI_HANDLE         image;
    int                created;
    EfiConnOp          conns[KL_EFI_MAX_CONN_OPS];
    EfiWatch           watches[KL_EFI_MAX_WATCHES];
} EfiLoop;

static EfiLoop g_efi;

/* ── KlEventOps (readiness face — only tagged watchers register a relay) ────────── */

static int el_init(KlEventLoop *loop) {
    (void)loop;
    /* Preserve bs/image/created (set by kl_uefi_event_provider); clear per-run state. */
    for (int i = 0; i < KL_EFI_MAX_CONN_OPS; i++) g_efi.conns[i].in_use = 0;
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++) g_efi.watches[i].in_use = 0;
    return 0;
}

static int el_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop;
    if (!((uintptr_t)udata & 1)) return 0;   /* untagged (connection) fd — op-driven */
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (g_efi.watches[i].in_use && g_efi.watches[i].udata == udata) {
            g_efi.watches[i].mask = mask; g_efi.watches[i].fd = fd; return 0;
        }
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (!g_efi.watches[i].in_use) {
            g_efi.watches[i].in_use = 1;
            g_efi.watches[i].fd = fd;
            g_efi.watches[i].mask = mask;
            g_efi.watches[i].udata = udata;
            return 0;
        }
    return -1;
}

static int el_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    if (!((uintptr_t)udata & 1)) return 0;
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (g_efi.watches[i].in_use && g_efi.watches[i].udata == udata) {
            g_efi.watches[i].mask = mask; g_efi.watches[i].fd = fd; return 0;
        }
    return el_add(loop, fd, mask, udata);
}

static int el_del(KlEventLoop *loop, KlSocketHandle fd) {
    (void)loop;
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (g_efi.watches[i].in_use && g_efi.watches[i].fd == fd)
            g_efi.watches[i].in_use = 0;
    return 0;
}

static int el_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* Completion loops drive via kl_comp_run/drain, not this readiness wait. */
    (void)loop; (void)out; (void)max; (void)timeout_ms; return 0;
}
static void el_close(KlEventLoop *loop) { (void)loop; }
static unsigned el_caps(const KlEventLoop *loop) { (void)loop; return KL_EVENT_CAP_COMPLETION; }

static const struct KlSocketProvider *el_native_provider(const KlEventLoop *loop) {
    (void)loop;
    /* Lazily create the U-2 socket provider over the stashed bs/image (single
     * instance — returns the same provider once created). */
    return kl_uefi_socket_provider(g_efi.bs, g_efi.image);
}

/* ── KlCompletionOps ────────────────────────────────────────────────────────────── */

static int el_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                           const KlSockAddr *addr, void *watcher_udata) {
    (void)ctx;
    EfiConnOp *op = NULL;
    for (int i = 0; i < KL_EFI_MAX_CONN_OPS; i++)
        if (!g_efi.conns[i].in_use) { op = &g_efi.conns[i]; break; }
    if (!op) return -1;   /* no slot */

    op->in_use = 1;
    op->fd = fd;
    op->generation = (uint64_t)kl_uefi_conn_generation_h(fd);
    op->watcher_udata = watcher_udata;
    op->failed = 0;

    /* Configure (active/DHCP/remote) then issue the Connect token — both non-blocking
     * beyond the bounded DHCP settle. Any failure is recorded and delivered as an
     * ok=0 KL_COMP_CONNECT on the next drain (the completion model always reports the
     * connect result through the loop, never as a post-time error). */
    if (kl_uefi_socket_configure(fd, addr) != 0 || kl_uefi_socket_connect_post(fd) != 0)
        op->failed = 1;
    return 0;
}

static void el_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    (void)ctx;
    /* Drop any queued connect op on this fd so a stale KL_COMP_CONNECT cannot fire at
     * the (about-to-be-freed) watcher. The socket close's generation bump is the
     * second line of defense (drain re-checks kl_uefi_conn_valid_h). */
    for (int i = 0; i < KL_EFI_MAX_CONN_OPS; i++)
        if (g_efi.conns[i].in_use && g_efi.conns[i].fd == fd)
            g_efi.conns[i].in_use = 0;
    /* Also drop any watches on the fd (the client is done with it). */
    for (int i = 0; i < KL_EFI_MAX_WATCHES; i++)
        if (g_efi.watches[i].in_use && g_efi.watches[i].fd == fd)
            g_efi.watches[i].in_use = 0;
}

/* drain: surface completed Connect tokens (stale-guarded) as KL_COMP_CONNECT, then
 * relay each armed watch as a KL_COMP_WATCHER (level-triggered — the client re-arms
 * while it still needs to send/recv). If nothing fired, Stall briefly so the firmware
 * keeps ticking without a 100%-CPU spin. */
static int el_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    (void)ctx; (void)timeout_ms;
    /* F3: once boot services are gone, the EFI_TCP4 Poll/CheckEvent/connect_poll paths
     * are all invalid — stop driving the loop (fail-closed, no firmware calls). */
    if (kl_uefi_after_ebs()) return 0;
    int count = 0;

    /* Connect completions — one terminal edge each, then retire the op. */
    for (int i = 0; i < KL_EFI_MAX_CONN_OPS && count < max; i++) {
        EfiConnOp *op = &g_efi.conns[i];
        if (!op->in_use) continue;

        int ok = 0;
        if (op->failed) {
            ok = 0;                                    /* Configure/post failed at post time */
        } else if (!kl_uefi_conn_valid_h(op->fd, op->generation)) {
            op->in_use = 0; continue;                  /* stale (closed / reused) — drop */
        } else {
            int connected = 0;
            int r = kl_uefi_socket_connect_poll(op->fd, &connected);
            if (r == 0) continue;                      /* still pending — keep pumping */
            ok = (r == 1 && connected);                /* r<0 → ok=0 */
        }

        for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
        out[count].kind   = KL_COMP_CONNECT;
        out[count].target = op->watcher_udata;
        out[count].bytes  = ok ? (size_t)KL_EVENT_WRITE : 0;
        out[count].ok     = ok;
        count++;
        op->in_use = 0;   /* retire: exactly-once terminal result */
    }

    /* Drain-driven readiness relay (U-6): for each armed watch, emit a
     * KL_COMP_WATCHER ONLY when it is actually ready. WRITE stays always-ready —
     * send is a short synchronous Transmit. READ is relayed only when the
     * provider's non-blocking recv can return something now
     * (kl_uefi_socket_recv_ready posts/polls a Receive without pumping) — no more
     * busy-relay that spun a blocking recv, and the loop is never stalled for a
     * whole recv (timers fire, other ops interleave). */
    for (int i = 0; i < KL_EFI_MAX_WATCHES && count < max; i++) {
        if (!g_efi.watches[i].in_use) continue;
        KlEventMask mask = g_efi.watches[i].mask;
        int ready = 0;
        if (mask & KL_EVENT_WRITE) ready |= KL_EVENT_WRITE;
        if ((mask & KL_EVENT_READ) && kl_uefi_socket_recv_ready(g_efi.watches[i].fd))
            ready |= KL_EVENT_READ;
        if (ready == 0) continue;   /* nothing ready → don't relay (no spin) */
        for (size_t b = 0; b < sizeof(*out); b++) ((unsigned char *)&out[count])[b] = 0;
        out[count].kind   = KL_COMP_WATCHER;
        out[count].target = g_efi.watches[i].udata;
        out[count].bytes  = (size_t)ready;
        out[count].ok     = 1;
        count++;
    }

    if (count == 0 && g_efi.bs)
        g_efi.bs->Stall(1000);   /* 1 ms — idle tick while a connect settles / no work */
    return count;
}

static const KlCompletionOps EFI_COMP_OPS = {
    .drain = el_drain,
    .post_connect = el_post_connect,
    .cancel = el_cancel,
    /* prime_accepts/post_recv/post_send/post_accept/post_sendfile/post_udp_* = NULL:
     * unreached by the client-only completion path. */
};

static const KlEventOps EFI_EVENT_OPS = {
    .init = el_init, .add = el_add, .mod = el_mod, .del = el_del,
    .wait = el_wait, .close = el_close, .caps = el_caps,
    .native_provider = el_native_provider,
    .completion = &EFI_COMP_OPS,
};

static const KlEventProvider EFI_EVENT_PROVIDER = { &EFI_EVENT_OPS, "efi-tcp4-completion" };

const KlEventProvider *kl_uefi_event_provider(EFI_BOOT_SERVICES *bs, EFI_HANDLE image) {
    if (g_efi.created) return &EFI_EVENT_PROVIDER;
    if (!bs) return NULL;
    for (size_t b = 0; b < sizeof(g_efi); b++) ((unsigned char *)&g_efi)[b] = 0;
    g_efi.bs = bs;
    g_efi.image = image;
    g_efi.created = 1;
    return &EFI_EVENT_PROVIDER;
}

void kl_uefi_event_provider_reset(void) {
    for (size_t b = 0; b < sizeof(g_efi); b++) ((unsigned char *)&g_efi)[b] = 0;
}
