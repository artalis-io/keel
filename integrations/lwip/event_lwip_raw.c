/*
 * event_lwip_raw.c — Phase 9 lwIP raw-API COMPLETION event backend (P9-1 skeleton).
 *
 * This is the completion-model counterpart of the shipped *readiness* lwIP provider
 * (event_lwip.c, which polls lwIP's sockets layer via lwip_poll). It drives lwIP's
 * native, lightest interface: the raw `tcp_*` callback API in NO_SYS=1 mode — no
 * tcpip thread, no sockets/netconn. See docs/phase9_lwip_raw_design.md.
 *
 * The crux (design doc §"NO_SYS=1"): lwIP's raw callbacks are not thread-safe and
 * must run on a single bare mainloop. KEEL's KlEventCtx run loop IS that mainloop:
 * each completion tick (kl_comp_drain) calls sys_check_timeouts() (retransmit /
 * TIME-WAIT / etc.) + netif_poll(loopif) (drains queued loopback/TX pbufs), and the
 * raw tcp_* callbacks fire inline on that one thread — so no locking is needed and
 * they map cleanly onto the completion driver (completion_driver.c).
 *
 * "It's a completion BACKEND, not a runtime drop-in" (the CRUCIAL finding): unlike
 * event_lwip.c (a runtime-injectable readiness provider that rides a STOCK libkeel),
 * this backend requires a libkeel BUILT for the completion axis — completion_driver.c
 * linked in, the io_engine.c stub NOT — because the connection state machine that runs
 * over completions lives in the driver, selected at build time (Makefile BACKEND).
 * So this TU is compiled INTO a lwipraw libkeel (Makefile BACKEND=lwipraw): it defines
 * both the compile-time backend entry points (kl_event_*_builtin) AND the runtime
 * KlEventProvider (kl_event_provider_lwip_raw), which are the same implementation — a
 * lwipraw libkeel's compiled-in default backend IS lwIP-raw, and the ctx also accepts
 * the named provider so callers can init explicitly.
 *
 * Division of labour (mirrors event_pollcomp.c — the SIMPLEST completion backend):
 *   - THIS TU defines the completion.h/io_engine.h BACKEND primitives:
 *       kl_comp_drain / kl_comp_prime_accepts / kl_comp_post_{recv,send,accept,sendfile}
 *       / kl_comp_cancel / kl_comp_post_udp_{recv,send}
 *   - completion_driver.c defines the GENERIC tick + seam entry points on top of them:
 *       kl_comp_run / kl_io_engine_run_completion / kl_io_engine_resume_completion /
 *       kl_io_engine_post_read
 *     — do NOT redefine those here.
 *
 * P9-1 SCOPE: skeleton + loop drive only. accept/recv/send/close (mapping tcp_accept/
 * tcp_recv/tcp_write/tcp_sent callbacks to KL_COMP_ACCEPT/READ/WRITE) land in P9-2..P9-5.
 * Here kl_comp_drain does ONE lwIP tick and returns 0 events; the post_* primitives are
 * stubs that link but do nothing yet (each flagged "P9-2+").
 *
 * BYO lwIP (LWIP_DIR), NO_SYS=1 build (lwipopts_raw.h). See integrations/lwip/Makefile.
 *
 * SPDX-License-Identifier: MIT
 */

/* Keel internal completion-backend contract (a completion backend is inherently a
 * consumer of these internal seams, exactly as event_pollcomp.c / event_iocp.c are).
 * NOTE: this TU deliberately includes NO lwIP header — lwIP's def.h redefines htons/ntohs
 * and its arch.h typedefs ssize_t, which clash with the host <sys/socket.h>/<netinet/in.h>
 * that the KEEL socket seam below pulls in. All lwIP contact goes through lwip_raw_glue.h
 * (opaque void* netif), whose implementation (lwip_raw_glue.c) is the lwIP-only TU. */
#include <keel/event.h>
#include <keel/event_ctx.h>    /* KlEventCtx — kl_comp_drain reaches loop._backend */
#include <keel/allocator.h>    /* kl_malloc / kl_free */
#include "keel_lwip_raw.h"     /* kl_event_provider_lwip_raw / kl_socket_provider_lwip_raw */
#include "lwip_raw_glue.h"     /* kl_lwr_lwip_up / kl_lwr_lwip_tick (lwIP seam, no lwIP types) */
#include "event_builtin.h"     /* kl_event_*_builtin entry points (src/) */
#include "event_caps.h"        /* KL_EVENT_CAP_* + kl_event_native_provider_builtin (src/) */
#include "socket.h"            /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED (src/) */
#include "completion.h"        /* the abstract completion axis this TU implements (src/) */
#include "io_engine.h"         /* kl_comp_post_udp_* decls (forward-declares struct KlUdp) */

#include <string.h>
#include <time.h>

/* One lwIP tick's bounded idle wait when no completions are ready, in nanoseconds.
 * The mainloop must keep turning to drive sys_check_timeouts()/netif_poll() even with
 * nothing pending, so a caller timeout is honoured as a coarse sleep (there is no fd to
 * block on in NO_SYS=1). 1 ms matches the spike's tick granularity. */
#define KL_LWR_TICK_SLEEP_NS  1000000L
#define KL_LWR_NS_PER_MS      1000000L

/* Per-loop backend state stashed in loop->_backend. In P9-1 it holds only the opaque
 * loopback netif we tick each drain; P9-2+ adds the pending-op list + accept target. */
typedef struct {
    KlAllocator     *alloc;
    void            *loopif;     /* opaque lwIP loop netif (kl_lwr_lwip_up) */
    struct KlServer *server;     /* accept target (set at prime) — P9-2+ */
    int              primed;     /* accept backlog primed — P9-2+ */
} KlLwrState;

/* ── KlEventLoop lifecycle ─────────────────────────────────────────────
 * Named *_builtin because a lwipraw-built libkeel selects this TU as its compile-time
 * backend (Makefile BACKEND=lwipraw); event_dispatch.c calls these on the NULL-ops
 * (default) path. The runtime provider below wraps the SAME functions, so an explicit
 * kl_event_provider_lwip_raw() init and the default init behave identically. */

int kl_event_init_builtin(KlEventLoop *loop) {
    KlLwrState *st = kl_malloc(loop->alloc, sizeof(*st));
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = loop->alloc;
    st->loopif = kl_lwr_lwip_up();   /* NO_SYS=1 lwip_init + attach loopback netif */
    if (!st->loopif) { kl_free(loop->alloc, st, sizeof(*st)); return -1; }
    loop->_backend = st;
    loop->fd = -1;   /* NO_SYS=1 raw loop has no pollable fd (axis-audit F1, P9-5) */
    return 0;
}

/* P9-1 has no watchers: connection I/O is driven by posted overlapped ops (P9-2+),
 * not readiness watches, so add/mod/del are inert here — matching the IOCP model. A
 * tagged-udata watcher relay (thread-pool wakeup / timers, as in event_pollcomp.c's
 * KL_COMP_WATCHER path) is a later stage; P9-1 needs none. */
int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop; (void)fd; (void)mask; (void)udata;
    return 0;
}
int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop; (void)fd; (void)mask; (void)udata;
    return 0;
}
int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
    (void)loop; (void)fd;
    return 0;
}

/* A completion loop drives via kl_comp_drain (the io_engine seam), not this readiness
 * wait — defined only because the KlEventLoop API requires it (same as event_pollcomp).
 * A NO_SYS=1 raw loop cannot block on an fd, so a positive timeout is a coarse sleep. */
int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    (void)loop; (void)out; (void)max;
    if (timeout_ms > 0) {
        struct timespec sl = { timeout_ms / 1000, (long)(timeout_ms % 1000) * KL_LWR_NS_PER_MS };
        nanosleep(&sl, NULL);
    }
    return 0;
}

void kl_event_close_builtin(KlEventLoop *loop) {
    KlLwrState *st = loop->_backend;
    if (!st) return;
    /* lwIP itself is a process-global singleton (no lwip_deinit in NO_SYS=1); we free
     * only our per-loop state. The loop netif persists for a subsequent ctx. */
    kl_free(st->alloc, st, sizeof(*st));
    loop->_backend = NULL;
    loop->fd = -1;
}

/* Completion loop over lwIP-raw. COMPLETION makes the Phase 7 negotiation
 * (kl_caps_compatible) require an OVERLAPPED socket provider — kl_socket_provider_lwip_raw
 * below. NOT native-fd: the raw loop has no OS descriptor to poll (F1/P9-5). */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION;
}

const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return kl_socket_provider_lwip_raw();
}

/* ── Runtime KlEventProvider packaging (parallel to event_lwip.c) ──────────────
 * So a caller can init the ctx explicitly with this backend:
 *   kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw());
 * The ops just forward to the *_builtin functions above (event_dispatch.c routes
 * through loop->ops when a provider is installed). Same implementation either way. */
static const KlEventOps lwip_raw_event_ops = {
    kl_event_init_builtin, kl_event_add_builtin, kl_event_mod_builtin,
    kl_event_del_builtin, kl_event_wait_builtin, kl_event_close_builtin,
    kl_event_caps_builtin, kl_event_native_provider_builtin,
};
static const KlEventProvider lwip_raw_event_provider = { &lwip_raw_event_ops, "lwip-raw" };

const KlEventProvider *kl_event_provider_lwip_raw(void) { return &lwip_raw_event_provider; }

/* ── Overlapped socket provider (KlSocketHandle == tcp_pcb *) ───────────────────
 * P9-1: minimal stubs that carry the OVERLAPPED capability so
 * kl_event_ctx_sockets_compatible() passes for a completion loop (kl_caps_compatible
 * keys on KL_SOCK_CAP_OVERLAPPED). The real raw-PCB ops (tcp_new/bind/listen/close as
 * the socket lifecycle; recv/send driven by the completion path, not these) land in
 * P9-2..P9-5. The handle will be a `struct tcp_pcb *` cast to KlSocketHandle (intptr_t;
 * handle.h already anticipates "lwIP raw tcp_pcb *"). None of these are reached in P9-1
 * (the tick test posts no ops and creates no sockets). */
static int lwr_sock_noop_fd(void *c, KlSocketHandle fd) { (void)c; (void)fd; return 0; }
static void lwr_sock_void_fd(void *c, KlSocketHandle fd) { (void)c; (void)fd; }
static int lwr_sock_opt(void *c, KlSocketHandle fd, int on) { (void)c; (void)fd; (void)on; return -1; }
static KlSocketHandle lwr_sock_socket(void *c, int d, int t, int p) {
    (void)c; (void)d; (void)t; (void)p;
    return KL_INVALID_SOCKET;   /* P9-2+: tcp_new() → (KlSocketHandle)pcb */
}
static int lwr_sock_addr(void *c, KlSocketHandle fd, const KlSockAddr *a) {
    (void)c; (void)fd; (void)a; return -1;   /* P9-2+: tcp_bind / tcp_connect */
}
static int lwr_sock_listen(void *c, KlSocketHandle fd, int backlog) {
    (void)c; (void)fd; (void)backlog; return -1;   /* P9-2+: tcp_listen */
}
static KlSocketHandle lwr_sock_accept(void *c, KlSocketHandle fd, KlSockAddr *peer) {
    (void)c; (void)fd; (void)peer;
    return KL_INVALID_SOCKET;   /* P9-2+: accept arrives via the tcp_accept callback */
}
static int lwr_sock_close(void *c, KlSocketHandle fd) {
    (void)c; (void)fd; return -1;   /* P9-4+: tcp_close(pcb) */
}
static int lwr_sock_getaddr(void *c, KlSocketHandle fd, KlSockAddr *out) {
    (void)c; (void)fd; (void)out; return -1;   /* P9-2+: pcb->local_ip/local_port */
}
static int lwr_sock_soerror(void *c, KlSocketHandle fd, int *out_err) {
    (void)c; (void)fd; if (out_err) *out_err = 0; return -1;
}
static kl_ssize_t lwr_sock_io(void *c, KlSocketHandle fd, void *b, size_t n) {
    (void)c; (void)fd; (void)b; (void)n; return -1;   /* I/O rides the completion path */
}
static kl_ssize_t lwr_sock_io_const(void *c, KlSocketHandle fd, const void *b, size_t n) {
    (void)c; (void)fd; (void)b; (void)n; return -1;
}

static const KlSocketOps lwip_raw_sock_ops = {
    .set_nonblocking = lwr_sock_noop_fd, .set_blocking = lwr_sock_noop_fd,
    .set_cloexec = lwr_sock_void_fd, .set_nosigpipe = lwr_sock_void_fd,
    .set_reuseaddr = lwr_sock_opt, .set_reuseport = lwr_sock_opt,
    .set_ipv6only = lwr_sock_opt, .set_tcp_nodelay = lwr_sock_opt,
    .set_cork = lwr_sock_opt,
    .socket = lwr_sock_socket, .connect = lwr_sock_addr, .bind = lwr_sock_addr,
    .listen = lwr_sock_listen, .accept = lwr_sock_accept, .close = lwr_sock_close,
    .get_local_addr = lwr_sock_getaddr, .get_so_error = lwr_sock_soerror,
    .send = lwr_sock_io_const, .recv = lwr_sock_io, .recv_peek = lwr_sock_io,
    .writev = NULL, .sendfile = NULL, .destroy = NULL,
    .name = "lwip-raw",
};

/* OVERLAPPED (not native-fd): the raw loop drives I/O through the completion post
 * path, exactly like the pollcomp/iocp/iouring overlapped providers. */
static const KlSocketProvider lwip_raw_provider = {
    &lwip_raw_sock_ops, NULL, KL_SOCK_CAP_OVERLAPPED, NULL,
};

const KlSocketProvider *kl_socket_provider_lwip_raw(void) { return &lwip_raw_provider; }

/* ── completion.h backend primitives ───────────────────────────────────────────
 * P9-1: kl_comp_drain does ONE lwIP mainloop tick and reports 0 events. The post_*
 * primitives link but do nothing yet — the tick test posts none. P9-2 wires the
 * tcp_accept/tcp_recv callbacks into KL_COMP_ACCEPT/READ events here. */

int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    (void)out; (void)max;
    KlLwrState *st = ctx->loop._backend;

    /* One lwIP mainloop tick — the whole point of P9-1 (docs §P9-1): sys_check_timeouts()
     * (lwIP timers: retransmit, TCP fast/slow, TIME-WAIT) + netif_poll(loopif) (drain the
     * loopback TX queue into RX so raw callbacks fire). Both go through the lwIP-only glue
     * TU so this backend TU stays free of lwIP's clashing headers. */
    kl_lwr_lwip_tick(st ? st->loopif : NULL);

    /* No fd to block on in NO_SYS=1; honour a positive timeout as a bounded sleep so a
     * caller's kl_event_ctx_run(&ctx, n, timeout_ms) doesn't busy-spin. timeout_ms == 0
     * is a non-blocking poll (return immediately); -1 (infinite) sleeps one tick and
     * returns so the loop re-ticks (a raw loop must keep turning to make progress). */
    if (timeout_ms != 0) {
        long ns = (timeout_ms > 0)
                    ? (long)(timeout_ms % 1000) * KL_LWR_NS_PER_MS
                    : KL_LWR_TICK_SLEEP_NS;
        time_t sec = (timeout_ms > 0) ? timeout_ms / 1000 : 0;
        struct timespec sl = { sec, ns };
        nanosleep(&sl, NULL);
    }

    /* P9-2+: report accept/recv/send completions collected from the tcp_* callbacks. */
    return 0;
}

int kl_comp_prime_accepts(struct KlServer *s) {
    /* P9-2: tcp_new/tcp_bind/tcp_listen + tcp_accept(cb) on the server's listen addr,
     * then post the first accept. P9-1 drives no server (the tick test runs the loop
     * standalone), so this simply latches and no-ops. */
    (void)s;
    return 0;
}

int kl_comp_post_recv(KlConn *c) { (void)c; return -1; }   /* P9-2: tcp_recv callback → KL_COMP_READ */
int kl_comp_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    (void)c; (void)iov; (void)iovcnt; (void)total; return -1;   /* P9-3: tcp_write + tcp_output */
}
int kl_comp_post_accept(struct KlServer *s) { (void)s; return 0; }   /* P9-2: refill accept backlog */
int kl_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    (void)c; (void)head_iov; (void)head_n; (void)head_total; (void)file_fd; (void)count;
    return -1;   /* P9-3: head via tcp_write, file bytes chunked (no lwIP zero-copy file send) */
}
void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    (void)ctx; (void)fd;   /* P9-4: abort the conn's pending op (tcp_abort / mark aborted) */
}
int kl_comp_post_udp_recv(struct KlUdp *udp) { (void)udp; return -1; }   /* raw UDP: future */
int kl_comp_post_udp_send(struct KlUdp *udp, const void *data, size_t len,
                          const KlSockAddr *dest) {
    (void)udp; (void)data; (void)len; (void)dest; return -1;
}
