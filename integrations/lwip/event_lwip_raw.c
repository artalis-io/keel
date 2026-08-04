/*
 * event_lwip_raw.c — Phase 9 lwIP raw-API COMPLETION event backend.
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
 * RUNTIME PROVIDER over a STOCK libkeel (RC-3): originally this was authored as a
 * compiled-in completion BACKEND (BACKEND=lwipraw). Since RC-1 made the completion axis
 * runtime-dispatchable (completion_dispatch.c honours loop->ops->completion) and RC-2
 * proved the injection path on pollcomp, this TU is now a PURE runtime provider — exactly
 * like the readiness event_lwip.c: static ops grouped in a KlEventProvider, NO
 * kl_event_*_builtin / kl_comp_ops_builtin symbols. It links cleanly next to a STOCK
 * libkeel (whose default backend already defines those _builtin symbols) and is installed
 * at runtime via kl_event_provider_lwip_raw() (KlConfig.event_provider /
 * kl_event_ctx_init_ex). The always-linked driver+dispatch in the stock lib reach this
 * backend's primitives through loop->ops->completion. BACKEND=lwipraw is retired.
 *
 * Division of labour (mirrors event_pollcomp.c — the SIMPLEST completion backend):
 *   - THIS TU defines the completion.h/io_engine.h BACKEND primitives:
 *       kl_comp_drain / kl_comp_prime_accepts / kl_comp_post_{recv,send,accept,sendfile}
 *       / kl_comp_cancel / kl_comp_post_udp_{recv,send}
 *   - completion_driver.c defines the GENERIC tick + seam entry points on top of them:
 *       kl_comp_run / kl_io_engine_run_completion / kl_io_engine_resume_completion /
 *       kl_io_engine_post_read
 *
 * CAPABILITIES (server-only, IPv4 TCP): listen/accept + recv/send/sendfile completion — a
 * raw-backed KlServer serves HTTP over the loopback netif. The socket-provider primitives
 * (tcp_new/bind/listen/close) operate on real tcp_pcb handles via the glue; the
 * tcp_accept/tcp_recv/tcp_sent callbacks surface KL_COMP_ACCEPT/READ/WRITE. Unsupported
 * operations fail EARLY and CLEARLY (see the socket ops below): outbound connect returns
 * -1/ENOTSUP (server-only), non-IPv4 bind returns -1 (IPv4-only loopif), and no datagram ops
 * are provided (.dgram == NULL, so kl_udp_init on this provider fails). See the Supported /
 * Unsupported matrix in keel_lwip_raw.h.
 *
 * SEND: BOUNDED, zero-allocation send + backpressure + file send. kl_comp_post_send translates
 * the response iovec into the seam-neutral KlLwrIoVec and hands it to the glue, which stores a
 * bounded COPY of the iov ARRAY (data referenced in place — small transient segments snapshotted)
 * and pumps it through a fixed PREALLOCATED per-conn TX window (KL_LWR_TX_WIN) across many
 * tcp_write/tcp_sent rounds honouring tcp_sndbuf (ERR_MEM = backpressure, resumed on tcp_sent).
 * kl_comp_post_sendfile passes the head the same way and the glue preads the file body
 * chunk-by-chunk into that same window — never reading the file into memory whole. Transmit
 * memory is bounded by conn_cap * KL_LWR_TX_WIN, INDEPENDENT of response/file size, so response
 * size is unbounded-by-design. Either way the driver sees a SINGLE fully-completed KL_COMP_WRITE.
 * The NO_SYS=1 raw loop has no pollable OS fd, so KlEventLoop.fd is -1 (backend-private; no
 * shared code reads it).
 *
 * Header-clash seam discipline (preserved): this TU includes NO lwIP header — lwIP's
 * def.h redefines htons/ntohs and its arch.h typedefs ssize_t, which clash with the host
 * <sys/socket.h>/<netinet/in.h> that the KEEL socket seam below pulls in. All lwIP contact
 * goes through lwip_raw_glue.h (opaque void* pcb/netif + neutral KlLwrRecord), whose
 * implementation (lwip_raw_glue.c) is the lwIP-only TU.
 *
 * BYO lwIP (LWIP_DIR), NO_SYS=1 build (lwipopts_raw.h). See integrations/lwip/Makefile.
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/event.h>
#include <keel/event_ctx.h>    /* KlEventCtx — kl_comp_drain reaches loop._backend */
#include <keel/connection.h>   /* KlConn — read_buf / fd / ctx */
#include <keel/server.h>       /* KlServer — listen_fd (prime accepts) */
#include <keel/allocator.h>    /* kl_malloc / kl_free */
#include <keel/sockaddr.h>     /* KlSockAddr marshalling at the seam boundary */
#include "keel_lwip_raw.h"     /* kl_event_provider_lwip_raw / kl_socket_provider_lwip_raw */
#include "lwip_raw_glue.h"     /* the lwIP seam (no lwIP types) */
#include "socket.h"            /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED (src/) */
#include "completion.h"        /* the abstract completion axis this TU implements (src/) */
#include "io_engine.h"         /* kl_comp_post_udp_* decls (forward-declares struct KlUdp) */

#include <string.h>
#include <time.h>
#include <errno.h>       /* ENOTSUP / EOPNOTSUPP for the fail-early connect */

/* One lwIP tick's bounded idle wait when no completions are ready, in nanoseconds.
 * The mainloop must keep turning to drive sys_check_timeouts()/netif_poll() even with
 * nothing pending, so a caller timeout is honoured as a coarse sleep (there is no fd to
 * block on in NO_SYS=1). 1 ms matches the spike's tick granularity. */
#define KL_LWR_TICK_SLEEP_NS  1000000L
#define KL_LWR_NS_PER_MS      1000000L

/* Stack buffer size for one drain's worth of glue records (bounded by the caller's `max`,
 * which completion_driver.c caps at KL_COMP_MAX_EVENTS = 64). */
#define KL_LWR_MAX_DRAIN  64

/* Per-loop backend state stashed in loop->_backend. All connection state — including the
 * recv-arm flag and the per-conn pending completions — lives in the glue's per-context/
 * per-conn slots (fix #2: ONE authoritative capacity, conn_cap == max_connections). The
 * backend no longer carries a separate armed table. `lwrctx` is the opaque KlLwrCtx the glue
 * owns; the backend threads it through every glue call so no lwIP state is global here. */
typedef struct {
    KlAllocator     *alloc;
    void            *lwrctx;   /* opaque KlLwrCtx (glue-owned; carries loopif + conn slots) */
    void            *loopif;   /* cached loop netif (kl_lwr_ctx_loopif) for the tick */
    struct KlServer *server;   /* accept target (set at prime) */
    int              primed;   /* accept backlog primed (idempotent latch) */
} KlLwrState;

/* ── recv-arming ───────────────────────────────────────────────────────────────
 * lwIP recv is passive: the tcp_recv callback retains received pbufs in the owning conn's slot
 * whenever data arrives. But the completion contract is pull-shaped — the driver posts one recv
 * (kl_comp_post_recv) and expects exactly one READ. So the backend gates: a READ is surfaced to
 * the driver only when the owning conn is recv-armed (a per-conn slot flag, kl_lwr_conn_arm),
 * and arming is consumed on delivery (kl_lwr_conn_disarm). Because the arm flag lives in the
 * slot (fix #2), its capacity IS the slot capacity — no second, smaller limit. The drain
 * enumerates armed-and-ready conns by asking the glue (kl_lwr_next_readable), so the backend
 * needs no conn list of its own and stays lwIP-free. */

/* ── KlEventLoop lifecycle ─────────────────────────────────────────────
 * Pure-provider form (RC-3): these are static lwr_ev_* ops referenced only by the
 * KlEventOps literal below (which the KlEventProvider hangs off). No kl_event_*_builtin
 * surface — the backend is reached at runtime via loop->ops, never the compiled-in
 * default path. Mirrors event_lwip.c. */

/* Default per-conn slot capacity when the loop is created without a server (a standalone
 * KlEventCtx tick). A server sizes it AUTHORITATIVELY to KlConfig.max_connections at prime
 * (kl_lwr_ctx_ensure_cap). Matches KEEL's default max_connections so a default server needs no
 * regrow. */
#define KL_LWR_DEFAULT_CONN_CAP 256

static int lwr_ev_init(KlEventLoop *loop) {
    KlLwrState *st = kl_malloc(loop->alloc, sizeof(*st));
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = loop->alloc;
    /* Create the glue's per-context state (fix #5: no global lwIP state in the backend). This
     * brings up NO_SYS=1 lwIP + the loopback netif and allocates the conn-slot table through
     * KlAllocator. Rejects a 2nd simultaneous ctx (NO_SYS=1 single-stack invariant). */
    st->lwrctx = kl_lwr_ctx_create(loop->alloc, KL_LWR_DEFAULT_CONN_CAP);
    if (!st->lwrctx) { kl_free(loop->alloc, st, sizeof(*st)); return -1; }
    st->loopif = kl_lwr_ctx_loopif(st->lwrctx);
    loop->_backend = st;
    loop->fd = -1;   /* NO_SYS=1 raw loop has no pollable fd (backend-private; no shared code reads it) */
    return 0;
}

/* Connection I/O is driven by posted completions (recv/send via the tcp_* callbacks), not
 * readiness watches, so add/mod/del are inert — matching the IOCP model. The server path this
 * backend supports (accept/recv/send/sendfile over the completion driver) needs no readiness
 * watch registration; timers ride the KlEventCtx timer heap the drain checks each tick. */
static int lwr_ev_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop; (void)fd; (void)mask; (void)udata;
    return 0;
}
static int lwr_ev_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop; (void)fd; (void)mask; (void)udata;
    return 0;
}
static int lwr_ev_del(KlEventLoop *loop, KlSocketHandle fd) {
    (void)loop; (void)fd;
    return 0;
}

/* A completion loop drives via kl_comp_drain (the io_engine seam), not this readiness
 * wait — defined only because the KlEventLoop API requires it (same as event_pollcomp).
 * A NO_SYS=1 raw loop cannot block on an fd, so a positive timeout is a coarse sleep. */
static int lwr_ev_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    (void)loop; (void)out; (void)max;
    if (timeout_ms > 0) {
        struct timespec sl = { timeout_ms / 1000, (long)(timeout_ms % 1000) * KL_LWR_NS_PER_MS };
        nanosleep(&sl, NULL);
    }
    return 0;
}

static void lwr_ev_close(KlEventLoop *loop) {
    KlLwrState *st = loop->_backend;
    if (!st) return;
    /* Destroy the glue ctx: frees every per-conn rx pbuf chain + send buffer, closes the
     * listener, and CLEARS the single-active-ctx guard so a subsequent (sequential) ctx can be
     * created. The lwIP core (loop netif + timer wheel) persists process-globally (no
     * lwip_deinit in NO_SYS=1) but carries no per-ctx state. */
    kl_lwr_ctx_destroy(st->lwrctx);
    kl_free(st->alloc, st, sizeof(*st));
    loop->_backend = NULL;
    loop->fd = -1;
}

/* Completion loop over lwIP-raw. COMPLETION makes the Phase 7 negotiation
 * (kl_caps_compatible) require an OVERLAPPED socket provider — kl_socket_provider_lwip_raw
 * below. NOT native-fd: the raw loop has no OS descriptor to poll. */
static unsigned lwr_ev_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION;
}

static const struct KlSocketProvider *lwr_ev_native_provider(const KlEventLoop *loop) {
    (void)loop;
    return kl_socket_provider_lwip_raw();
}

/* ── Runtime KlEventProvider packaging (parallel to event_lwip.c) ────────────── */
/* The completion sub-vtable is defined below (after the completion primitives) — forward-
 * declare it so the KlEventOps literal can carry it (RC-1): a runtime-injected lwip-raw
 * provider then advertises the completion axis too, which RC-2 exercises. */
static const KlCompletionOps lwip_raw_completion_ops;
static const KlEventOps lwip_raw_event_ops = {
    .init = lwr_ev_init, .add = lwr_ev_add, .mod = lwr_ev_mod,
    .del = lwr_ev_del, .wait = lwr_ev_wait, .close = lwr_ev_close,
    .caps = lwr_ev_caps, .native_provider = lwr_ev_native_provider,
    .completion = &lwip_raw_completion_ops,
};
static const KlEventProvider lwip_raw_event_provider = { &lwip_raw_event_ops, "lwip-raw" };

const KlEventProvider *kl_event_provider_lwip_raw(void) { return &lwip_raw_event_provider; }

/* ── Overlapped socket provider (KlSocketHandle == tcp_pcb *) ───────────────────
 * SERVER-ONLY, IPv4-only. The socket lifecycle (tcp_new/bind/listen/close as
 * socket/bind/listen/close; get_local_addr for the bound-port readback) rides real tcp_pcb
 * handles via the glue. Data-plane I/O (recv/send) goes through the completion post path, not
 * these ops — so send/recv are no-op errors. Two ops fail EARLY + CLEARLY because this backend
 * cannot support them: `connect` (outbound client — server-only, returns -1/ENOTSUP) and a
 * non-IPv4 `bind` (the loopif is IPv4, returns -1). `accept` returns KL_INVALID_SOCKET because
 * accepts arrive via the tcp_accept callback → drain, not this pull op. Control-plane no-ops
 * (set_nonblocking etc.) succeed because a raw pcb needs no fcntl-style setup. The handle is a
 * `struct tcp_pcb *` cast to KlSocketHandle (intptr_t). */
static int lwr_sock_noop_fd(void *c, KlSocketHandle fd) { (void)c; (void)fd; return 0; }
static void lwr_sock_void_fd(void *c, KlSocketHandle fd) { (void)c; (void)fd; }
static int lwr_sock_opt(void *c, KlSocketHandle fd, int on) { (void)c; (void)fd; (void)on; return 0; }

static KlSocketHandle lwr_sock_socket(void *c, int d, int t, int p) {
    (void)c; (void)d; (void)t; (void)p;
    void *pcb = kl_lwr_tcp_new();
    return pcb ? (KlSocketHandle)pcb : KL_INVALID_SOCKET;
}

/* bind: marshal the neutral KlSockAddr's IPv4 bytes + port to the glue (which builds an
 * lwIP ip_addr_t). IPv4-ONLY: a non-IPv4 (e.g. IPv6) address is REJECTED with -1 — the raw
 * backend's loopback netif is IPv4, so there is no IPv6 to bind. This is the explicit
 * unsupported-family fail-early gate. */
static int lwr_sock_bind(void *c, KlSocketHandle fd, const KlSockAddr *a) {
    (void)c;
    if (!a || kl_sockaddr_family(a) != KL_AF_INET) return -1;   /* IPv6 unsupported */
    return kl_lwr_tcp_bind((void *)fd, a->u.ip, kl_sockaddr_port(a));
}

/* connect: UNSUPPORTED. lwip-raw is a SERVER-ONLY backend — no outbound connect. Fail EARLY
 * and CLEARLY (errno = ENOTSUP + return -1) so a KlClient on this provider aborts deterministically
 * at connect rather than silently hanging. For an lwIP CLIENT, use the readiness socket-API lwIP
 * integration (kl_socket_provider_lwip / kl_event_provider_lwip in keel_lwip.h). */
static int lwr_sock_connect(void *c, KlSocketHandle fd, const KlSockAddr *a) {
    (void)c; (void)fd; (void)a;
#ifdef ENOTSUP
    errno = ENOTSUP;
#else
    errno = EOPNOTSUPP;   /* nearest portable equivalent */
#endif
    return -1;
}

/* listen: tcp_listen relocates the pcb (frees the passed-in one, returns a smaller LISTEN
 * pcb) and the glue arms the accept callback + tracks the relocated handle. server.c still
 * holds the freed handle in s->listen_fd; kl_comp_prime_accepts adopts the relocated one
 * (kl_lwr_listen_pcb) so close() later targets a live pcb, not the dangling original. */
static int lwr_sock_listen(void *c, KlSocketHandle fd, int backlog) {
    (void)c; (void)backlog;
    return kl_lwr_tcp_listen(kl_lwr_active_ctx(), (void *)fd) ? 0 : -1;
}

static KlSocketHandle lwr_sock_accept(void *c, KlSocketHandle fd, KlSockAddr *peer) {
    (void)c; (void)fd; (void)peer;
    return KL_INVALID_SOCKET;   /* accept arrives via the tcp_accept callback → drain */
}

static int lwr_sock_close(void *c, KlSocketHandle fd) {
    (void)c;
    if (kl_handle_valid(fd)) kl_lwr_tcp_close(kl_lwr_active_ctx(), (void *)fd);
    return 0;
}

static int lwr_sock_getaddr(void *c, KlSocketHandle fd, KlSockAddr *out) {
    (void)c;
    if (!out || !kl_handle_valid(fd)) return -1;
    uint16_t port = kl_lwr_tcp_local_port((void *)fd);
    /* Loopback IPv4 — the address family/port is all server.c reads (bound_port). */
    static const uint8_t lo[4] = { 127, 0, 0, 1 };
    return kl_sockaddr_from_ipv4(out, lo, port);
}

static int lwr_sock_soerror(void *c, KlSocketHandle fd, int *out_err) {
    (void)c; (void)fd; if (out_err) *out_err = 0; return 0;
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
    .socket = lwr_sock_socket, .connect = lwr_sock_connect, .bind = lwr_sock_bind,
    .listen = lwr_sock_listen, .accept = lwr_sock_accept, .close = lwr_sock_close,
    .get_local_addr = lwr_sock_getaddr, .get_so_error = lwr_sock_soerror,
    .send = lwr_sock_io_const, .recv = lwr_sock_io, .recv_peek = lwr_sock_io,
    .writev = NULL, .sendfile = NULL, .destroy = NULL,
    .name = "lwip-raw",
};

static const KlSocketProvider lwip_raw_provider = {
    &lwip_raw_sock_ops, NULL, KL_SOCK_CAP_OVERLAPPED, NULL,
};

const KlSocketProvider *kl_socket_provider_lwip_raw(void) { return &lwip_raw_provider; }

/* ── completion.h backend primitives ─────────────────────────────────────────── */

static KlLwrState *lwr_state(KlConn *c) { return c->ctx->loop._backend; }

static int lwr_comp_prime_accepts(struct KlServer *s) {
    KlLwrState *st = s->ev.loop._backend;
    if (st->primed) return 0;
    st->server = s;
    /* fix #2: unify capacity on the AUTHORITATIVE Keel limit. Size the glue's per-conn slot
     * table to max_connections (the same value that sizes s->pool) so arm/slot/accept capacity
     * are ONE number — no second, smaller limit. Grown here (before any accept); a default
     * server needs no regrow (both default to KL_DEFAULT_MAX_CONNS). Fails init with a clear
     * error if the slot table can't be sized to the requested capacity. */
    int cap = s->config.max_connections;
    if (cap > 0 && kl_lwr_ctx_ensure_cap(st->lwrctx, cap) < 0) return -1;
    st->primed = 1;
    /* tcp_listen relocated the listen pcb (freeing the one server.c bound); adopt the live
     * handle so s->listen_fd is valid for the eventual close (kl_server_close_listener). */
    void *lp = kl_lwr_listen_pcb(st->lwrctx);
    if (lp) s->listen_fd = (KlSocketHandle)lp;
    /* The accept callback was armed by tcp_listen; accepts surface KL_LWR_ACCEPT via the drain's
     * per-slot scan. Nothing more to post (passive raw accept). */
    return 0;
}

static int lwr_comp_post_accept(struct KlServer *s) {
    /* Passive raw accept: the tcp_accept callback keeps the backlog filled on its own —
     * no per-accept op to re-post (unlike pollcomp's one accept op). Idempotent no-op. */
    (void)s;
    return 0;
}

/* Post one async receive: raw recv is passive (the tcp_recv callback retains received pbufs in
 * the conn's slot), so "posting" associates the owning conn with its pcb (first call) and arms
 * the slot — the READ surfaces via drain when data (or peer-close) arrives. fix #2: arming is a
 * per-conn slot flag; kl_lwr_conn_arm returns non-zero if the pcb has NO slot (a conn that was
 * accepted must always have a slot — if not, the driver closes it rather than leaving it
 * accepted-but-unable-to-receive). */
static int lwr_comp_post_recv(KlConn *c) {
    KlLwrState *st = lwr_state(c);
    if (!kl_handle_valid(c->fd)) return -1;
    kl_lwr_set_owner(st->lwrctx, (void *)c->fd, c);   /* recv/sent/err callbacks tag this conn */
    if (kl_lwr_conn_arm(st->lwrctx, (void *)c->fd) != 0) return -1;
    return 0;
}

/* Post one async send of the (already-serialized) response iovec. NO allocation, NO
 * whole-payload copy: translate the driver's KlIoVec array into the seam-neutral KlLwrIoVec and
 * hand it to the glue, which stores a BOUNDED copy of the iov ARRAY and pumps the referenced-in-
 * place segments through a fixed preallocated per-conn TX window across many tcp_write/tcp_sent
 * rounds (tcp_sndbuf backpressure). The driver sees a SINGLE KL_COMP_WRITE only once the WHOLE
 * payload is acknowledged — response size is UNBOUNDED-by-design (bounded transmit memory; the
 * old 16 MiB KL_LWR_POST_SEND_MAX cap is gone).
 *
 * The iov DATA is referenced in place and MUST stay valid until the KL_COMP_WRITE. That holds:
 * comp_send_response (completion_driver.c) builds the iovec from the live c->res segments
 * (res->hdr_buf, res->body) + static literals + a small stack Content-Length scratch (`cl_buf`);
 * the driver cannot reset c->res until comp_on_write. The glue snapshots any segment <= 256 B
 * (which covers cl_buf) into a preallocated head buffer, so even the transient scratch is safe.
 *
 * The translation array is a fixed KL_LWR_MAX_SEND_IOV stack buffer (the driver caps the iovec
 * at 7); no heap, no per-send allocation. */
#define KL_LWR_MAX_SEND_IOV 8
static int lwr_comp_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    (void)total;
    if (!kl_handle_valid(c->fd)) return -1;
    if (iovcnt < 0 || iovcnt > KL_LWR_MAX_SEND_IOV) return -1;
    KlLwrIoVec lv[KL_LWR_MAX_SEND_IOV];
    for (int i = 0; i < iovcnt; i++) { lv[i].base = iov[i].base; lv[i].len = iov[i].len; }
    return kl_lwr_send_begin(lwr_state(c)->lwrctx, (void *)c->fd, lv, iovcnt);
}

/* Post one async file send: the serialized response head (`head_iov`) followed by
 * `count` bytes pread from file_fd. NO allocation, NO whole-file read: the head iov is passed
 * seam-neutrally (referenced in place / snapshotted like a buffered send) and the glue preads the
 * file body chunk-by-chunk into the same preallocated TX window (constant memory regardless of
 * file size). One KL_COMP_WRITE follows when the whole head+file is acked; a file read error
 * surfaces a FAILED terminal (ok=0) so the driver closes. fd OWNERSHIP: the glue only READS
 * file_fd; the response layer closes res->file_fd (kl_response_reset/free) — not here. */
static int lwr_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    (void)head_total;
    if (!kl_handle_valid(c->fd)) return -1;
    if (head_n < 0 || head_n > KL_LWR_MAX_SEND_IOV) return -1;
    KlLwrIoVec lv[KL_LWR_MAX_SEND_IOV];
    for (int i = 0; i < head_n; i++) { lv[i].base = head_iov[i].base; lv[i].len = head_iov[i].len; }
    return kl_lwr_sendfile_begin(lwr_state(c)->lwrctx, (void *)c->fd, lv, head_n, file_fd, count);
}

/* Cancel pending ops on `fd` (idle-timeout sweep). Semantics mirror event_pollcomp.c: the
 * conn is released ONLY through a completion event, so we do NOT free the KlConn here. Instead
 * we abort the underlying pcb (kl_lwr_tcp_abort: frees its owned send buffer + slot by owner,
 * detaches callbacks so lwIP's internal tcp_err isn't re-entered, RSTs the peer, and marks the
 * slot `closed`). The abort marks the slot closed, so the NEXT kl_comp_drain surfaces a single
 * terminal zero-length READ for this (armed) conn → the driver runs comp_close exactly once,
 * releasing the KlConn through its normal completion path. Idempotent + safe if the conn
 * already closed (kl_lwr_tcp_abort is a no-op on a dead/free slot). */
static void lwr_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    KlLwrState *st = ctx ? ctx->loop._backend : NULL;
    if (st && kl_handle_valid(fd)) kl_lwr_tcp_abort(st->lwrctx, (void *)fd);
}

/* UDP/datagram is UNSUPPORTED on lwip-raw: the paired socket provider advertises no
 * KL_SOCK_CAP_DATAGRAM and its .dgram is NULL, so kl_udp_init on this provider fails before any
 * post — these completion primitives return -1 as a belt-and-braces fail-early. For UDP over lwIP
 * (KlUdp / udp_server / the built-in DNS resolver) use the readiness socket-API integration
 * (kl_socket_provider_lwip), which folds the datagram data-plane onto the provider. */
static int lwr_comp_post_udp_recv(struct KlUdp *udp) { (void)udp; return -1; }
static int lwr_comp_post_udp_send(struct KlUdp *udp, const void *data, size_t len,
                          const KlSockAddr *dest) {
    (void)udp; (void)data; (void)len; (void)dest; return -1;
}

/* ── drain: one lwIP tick, then translate per-slot pending state into completion events ──
 * fix #4: there is no global completion ring. First tick lwIP (so the tcp_* callbacks update
 * per-slot state), then (a) drain per-slot ACCEPT/WRITE/terminal completions via kl_lwr_drain,
 * and (b) surface READs by scanning armed-and-ready slots via kl_lwr_next_readable. Both are
 * bounded by conn_cap, so nothing can overflow / be silently dropped, and a terminal (a
 * per-slot flag) is always deliverable. */
static int lwr_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    KlLwrState *st = ctx->loop._backend;

    /* One lwIP mainloop tick: sys_check_timeouts() (retransmit, TCP fast/slow,
     * TIME-WAIT) + netif_poll(loopif) (drain the loopback TX queue so raw callbacks fire).
     * The tcp_accept/tcp_recv/tcp_sent callbacks run inline here and update per-slot state. */
    kl_lwr_lwip_tick(st ? st->loopif : NULL);

    int count = 0;

    /* (a) ACCEPT / WRITE / terminal completions from the per-slot scan. */
    KlLwrRecord recs[KL_LWR_MAX_DRAIN];
    int rmax = max < KL_LWR_MAX_DRAIN ? max : KL_LWR_MAX_DRAIN;
    int nr = kl_lwr_drain(st->lwrctx, recs, rmax);
    for (int i = 0; i < nr && count < max; i++) {
        KlLwrRecord *r = &recs[i];
        KlCompletionEvent *ev = &out[count];
        memset(ev, 0, sizeof(*ev));

        if (r->kind == KL_LWR_ACCEPT) {
            ev->kind = KL_COMP_ACCEPT;
            ev->ok = 1;
            ev->accepted_fd = (KlSocketHandle)r->accepted;
            /* completion_driver.c's comp_on_accept marshals ev->peer (a host struct sockaddr)
             * via kl_sockaddr_from_native, keyed by ev->peer_len. peer_len 0 = "unavailable",
             * which the driver handles by zeroing the conn's peer_addr. For loopback (peer
             * address unused by the handler) we leave it 0 rather than pulling a host socket
             * header in to synthesize a sockaddr_in. */
            ev->peer_len = 0;
            count++;
            continue;
        }

        /* KL_LWR_WRITE — a completed send (ok=1) OR a terminal close (ok=0). */
        KlConn *c = r->owner;
        if (!c) continue;
        ev->kind = KL_COMP_WRITE;
        ev->target = c;
        ev->ok = r->ok;
        ev->bytes = r->nbytes;
        /* Exactly-one-close: a terminal (ok=0) completion — surfaced by the glue on
         * tcp_err / kl_comp_cancel / close-with-outstanding — drives the driver to release this
         * conn. Disarm it NOW so the armed-READ scan below (and future drains) never touch the
         * about-to-be-released KlConn (no dangling armed slot aliasing a freed/reused conn). */
        if (!r->ok) kl_lwr_conn_disarm(st->lwrctx, (void *)c->fd);
        count++;
    }

    /* (b) READs for armed conns whose retained rx queue has data (or whose peer closed). Raw
     * recv is passive — bytes accumulate in the glue's per-conn slot independent of when the
     * driver posts a recv — so a READ is delivered here once the conn is armed (kl_comp_post_
     * recv), consuming the arming. Also handles the fast-client race where data arrived in the
     * same tick as the accept, before the recv was posted: picked up on the next drain once
     * comp_on_accept has posted it. */
    int cursor = 0;
    void *owner = NULL, *pcb = NULL;
    int closed = 0;
    while (count < max && kl_lwr_next_readable(st->lwrctx, &cursor, &owner, &pcb, &closed)) {
        KlConn *c = owner;
        KlCompletionEvent *ev = &out[count];
        memset(ev, 0, sizeof(*ev));
        ev->kind = KL_COMP_READ;
        ev->target = c;
        if (!closed) {
            size_t space = c->read_cap - c->read_len;
            size_t got = kl_lwr_take_staged(st->lwrctx, pcb, c->read_buf + c->read_len, space);
            ev->ok = 1;
            ev->bytes = got;
        } else {                       /* closed with no pending data — zero-length READ */
            ev->ok = 0;
            ev->bytes = 0;
            /* Exactly-one-close: mark the terminal READ consumed so a re-check can never surface
             * a second terminal event (which would drive a double comp_close). The driver's
             * comp_on_read turns ok=0 into comp_close → conn release → sock.close →
             * kl_lwr_tcp_close, freeing the slot (or, for a dead/tcp_err slot, just the slot —
             * no UAF on the freed pcb). */
            kl_lwr_mark_terminated(st->lwrctx, pcb);
        }
        kl_lwr_conn_disarm(st->lwrctx, pcb);   /* consumed the recv */
        count++;
    }

    /* No fd to block on in NO_SYS=1; if nothing completed, honour a positive timeout as a
     * bounded sleep so a caller's run loop doesn't busy-spin. When events fired, return
     * immediately so the loop processes them promptly. */
    if (count == 0 && timeout_ms != 0) {
        long ns = (timeout_ms > 0)
                    ? (long)(timeout_ms % 1000) * KL_LWR_NS_PER_MS
                    : KL_LWR_TICK_SLEEP_NS;
        time_t sec = (timeout_ms > 0) ? timeout_ms / 1000 : 0;
        struct timespec sl = { sec, ns };
        nanosleep(&sl, NULL);
    }

    return count;
}

/* ── Completion sub-vtable (RC-1/RC-3) ─────────────────────────────────────
 * This backend's completion primitives, grouped so the dispatch (completion_dispatch.c)
 * reaches them exclusively through the runtime lwip-raw provider
 * (loop->ops->completion — the only way this backend runs, RC-3). Forward-declared above
 * so the KlEventOps literal carries it. Static: there is NO kl_comp_ops_builtin here — a
 * pure runtime provider never binds the compiled-in default path, so it links cleanly
 * next to a stock libkeel (whose readiness kl_comp_ops_builtin stub owns that symbol). */
static const KlCompletionOps lwip_raw_completion_ops = {
    lwr_comp_drain, lwr_comp_prime_accepts, lwr_comp_post_recv, lwr_comp_post_send,
    lwr_comp_post_accept, lwr_comp_post_sendfile, lwr_comp_cancel,
    lwr_comp_post_udp_recv, lwr_comp_post_udp_send,
};
