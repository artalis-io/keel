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
 * "It's a completion BACKEND, not a runtime drop-in" (the CRUCIAL finding): unlike
 * event_lwip.c (a runtime-injectable readiness provider that rides a STOCK libkeel),
 * this backend requires a libkeel BUILT for the completion axis — completion_driver.c
 * linked in, the io_engine.c stub NOT — because the connection state machine that runs
 * over completions lives in the driver, selected at build time (Makefile BACKEND).
 *
 * Division of labour (mirrors event_pollcomp.c — the SIMPLEST completion backend):
 *   - THIS TU defines the completion.h/io_engine.h BACKEND primitives:
 *       kl_comp_drain / kl_comp_prime_accepts / kl_comp_post_{recv,send,accept,sendfile}
 *       / kl_comp_cancel / kl_comp_post_udp_{recv,send}
 *   - completion_driver.c defines the GENERIC tick + seam entry points on top of them:
 *       kl_comp_run / kl_io_engine_run_completion / kl_io_engine_resume_completion /
 *       kl_io_engine_post_read
 *
 * P9-2 SCOPE: listen/accept + recv/send completion — a raw-backed KlServer answers one
 * request over loopback. The socket-provider primitives (tcp_new/bind/listen/close) now
 * operate on real tcp_pcb handles via the glue; the tcp_accept/tcp_recv/tcp_sent callbacks
 * surface KL_COMP_ACCEPT/READ/WRITE.
 *
 * P9-3 SCOPE (this stage): full-payload send + backpressure + file send. kl_comp_post_send
 * no longer bails on responses larger than a small cap — it concatenates the response iovec
 * and hands it to the glue's send-pump (kl_lwr_send_begin), which buffers the whole payload
 * and pumps it across many tcp_write/tcp_sent rounds honouring tcp_sndbuf (ERR_MEM =
 * backpressure, resumed on tcp_sent). kl_comp_post_sendfile is implemented via
 * kl_lwr_sendfile_begin (pread the file after the head into the same owned buffer, reuse the
 * pump). Either way the driver sees a SINGLE fully-completed KL_COMP_WRITE. close-cancel-
 * lifetime is P9-4; KlEventLoop.fd assessment is P9-5.
 *
 * Header-clash seam discipline (P9-1, preserved): this TU includes NO lwIP header — lwIP's
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

/* Max concurrently recv-armed conns tracked by the backend. A conn is armed by
 * kl_comp_post_recv and disarmed when its READ completion is surfaced (single in-flight
 * recv per conn, matching the completion contract). Sized for the P9-2 loopback roundtrip
 * plus slack. */
#define KL_LWR_MAX_ARMED  8

/* Stack buffer size for one drain's worth of glue records (bounded by the caller's `max`,
 * which completion_driver.c caps at KL_COMP_MAX_EVENTS = 64). */
#define KL_LWR_MAX_DRAIN  64

/* Per-loop backend state stashed in loop->_backend. */
typedef struct {
    KlAllocator     *alloc;
    void            *loopif;                    /* opaque lwIP loop netif (kl_lwr_lwip_up) */
    struct KlServer *server;                    /* accept target (set at prime) */
    int              primed;                    /* accept backlog primed (idempotent latch) */
    KlConn          *armed[KL_LWR_MAX_ARMED];   /* conns with a pending recv posted */
    int              armed_count;
} KlLwrState;

/* ── recv-arming bookkeeping ───────────────────────────────────────────────────
 * lwIP recv is passive: the tcp_recv callback stages bytes + enqueues a READ record in the
 * glue whenever data arrives. But the completion contract is pull-shaped — the driver posts
 * one recv (kl_comp_post_recv) and expects exactly one READ. So the backend gates: a READ
 * record is surfaced to the driver only when the owning conn is recv-armed, and arming is
 * consumed on delivery. (The driver posts the recv synchronously in comp_on_accept, before
 * any data can arrive — a tick is required — so arming always precedes the first READ.) */
static int lwr_is_armed(KlLwrState *st, const KlConn *c) {
    for (int i = 0; i < st->armed_count; i++)
        if (st->armed[i] == c) return 1;
    return 0;
}
static void lwr_arm(KlLwrState *st, KlConn *c) {
    if (lwr_is_armed(st, c)) return;
    if (st->armed_count < KL_LWR_MAX_ARMED)
        st->armed[st->armed_count++] = c;
}
static void lwr_disarm(KlLwrState *st, const KlConn *c) {
    for (int i = 0; i < st->armed_count; i++)
        if (st->armed[i] == c) {
            st->armed[i] = st->armed[--st->armed_count];
            return;
        }
}

/* ── KlEventLoop lifecycle ─────────────────────────────────────────────
 * Named *_builtin because a lwipraw-built libkeel selects this TU as its compile-time
 * backend (Makefile BACKEND=lwipraw); event_dispatch.c calls these on the NULL-ops
 * (default) path. The runtime provider below wraps the SAME functions. */

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

/* Connection I/O is driven by posted completions (recv/send via the tcp_* callbacks), not
 * readiness watches, so add/mod/del are inert — matching the IOCP model. (A tagged-udata
 * watcher relay for thread-pool wakeup / timers, as in event_pollcomp.c, is a later stage;
 * P9-2's single-request loopback roundtrip needs none.) */
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

/* ── Runtime KlEventProvider packaging (parallel to event_lwip.c) ────────────── */
static const KlEventOps lwip_raw_event_ops = {
    kl_event_init_builtin, kl_event_add_builtin, kl_event_mod_builtin,
    kl_event_del_builtin, kl_event_wait_builtin, kl_event_close_builtin,
    kl_event_caps_builtin, kl_event_native_provider_builtin,
};
static const KlEventProvider lwip_raw_event_provider = { &lwip_raw_event_ops, "lwip-raw" };

const KlEventProvider *kl_event_provider_lwip_raw(void) { return &lwip_raw_event_provider; }

/* ── Overlapped socket provider (KlSocketHandle == tcp_pcb *) ───────────────────
 * The socket lifecycle (tcp_new/bind/listen/close as socket/bind/listen/close;
 * get_local_addr for the bound-port readback) rides real tcp_pcb handles via the glue.
 * Data-plane I/O (recv/send) goes through the completion post path, not these ops — so
 * send/recv/accept/connect stay stubbed. Control-plane no-ops (set_nonblocking etc.)
 * succeed because a raw pcb needs no fcntl-style setup. The handle is a `struct tcp_pcb *`
 * cast to KlSocketHandle (intptr_t). */
static int lwr_sock_noop_fd(void *c, KlSocketHandle fd) { (void)c; (void)fd; return 0; }
static void lwr_sock_void_fd(void *c, KlSocketHandle fd) { (void)c; (void)fd; }
static int lwr_sock_opt(void *c, KlSocketHandle fd, int on) { (void)c; (void)fd; (void)on; return 0; }

static KlSocketHandle lwr_sock_socket(void *c, int d, int t, int p) {
    (void)c; (void)d; (void)t; (void)p;
    void *pcb = kl_lwr_tcp_new();
    return pcb ? (KlSocketHandle)pcb : KL_INVALID_SOCKET;
}

/* bind: marshal the neutral KlSockAddr's IPv4 bytes + port to the glue (which builds an
 * lwIP ip_addr_t). IPv6 is out of scope for P9-2 (loopback IPv4). */
static int lwr_sock_bind(void *c, KlSocketHandle fd, const KlSockAddr *a) {
    (void)c;
    if (!a || kl_sockaddr_family(a) != KL_AF_INET) return -1;
    return kl_lwr_tcp_bind((void *)fd, a->u.ip, kl_sockaddr_port(a));
}

static int lwr_sock_connect(void *c, KlSocketHandle fd, const KlSockAddr *a) {
    (void)c; (void)fd; (void)a;
    return -1;   /* server-side only for P9-2; the test client uses the glue directly */
}

/* listen: tcp_listen relocates the pcb (frees the passed-in one, returns a smaller LISTEN
 * pcb) and the glue arms the accept callback + tracks the relocated handle. server.c still
 * holds the freed handle in s->listen_fd; kl_comp_prime_accepts adopts the relocated one
 * (kl_lwr_listen_pcb) so close() later targets a live pcb, not the dangling original. */
static int lwr_sock_listen(void *c, KlSocketHandle fd, int backlog) {
    (void)c; (void)backlog;
    return kl_lwr_tcp_listen((void *)fd) ? 0 : -1;
}

static KlSocketHandle lwr_sock_accept(void *c, KlSocketHandle fd, KlSockAddr *peer) {
    (void)c; (void)fd; (void)peer;
    return KL_INVALID_SOCKET;   /* accept arrives via the tcp_accept callback → drain */
}

static int lwr_sock_close(void *c, KlSocketHandle fd) {
    (void)c;
    if (kl_handle_valid(fd)) kl_lwr_tcp_close((void *)fd);
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

int kl_comp_prime_accepts(struct KlServer *s) {
    KlLwrState *st = s->ev.loop._backend;
    if (st->primed) return 0;
    st->server = s;
    st->primed = 1;
    /* tcp_listen relocated the listen pcb (freeing the one server.c bound); adopt the live
     * handle so s->listen_fd is valid for the eventual close (kl_server_close_listener). */
    void *lp = kl_lwr_listen_pcb();
    if (lp) s->listen_fd = (KlSocketHandle)lp;
    /* The accept callback was armed by tcp_listen; accepts enqueue KL_LWR_ACCEPT records the
     * drain surfaces. Nothing more to post (passive raw accept). */
    return 0;
}

int kl_comp_post_accept(struct KlServer *s) {
    /* Passive raw accept: the tcp_accept callback keeps the backlog filled on its own —
     * no per-accept op to re-post (unlike pollcomp's one accept op). Idempotent no-op. */
    (void)s;
    return 0;
}

/* Post one async receive: raw recv is passive (the tcp_recv callback stages bytes + enqueues
 * a READ record), so "posting" just associates the owning conn with its pcb (first call) and
 * arms it — the READ surfaces via drain when data (or peer-close) arrives. */
int kl_comp_post_recv(KlConn *c) {
    KlLwrState *st = lwr_state(c);
    if (!kl_handle_valid(c->fd)) return -1;
    kl_lwr_set_owner((void *)c->fd, c);   /* recv/sent callbacks tag records with this conn */
    lwr_arm(st, c);
    return 0;
}

/* Post one async send of the (already-serialized) response iovec (P9-3). Concatenate the
 * iovec into a heap staging buffer and hand it to the glue's send-pump (kl_lwr_send_begin),
 * which copies it into a per-pcb owned buffer and pumps it across as many tcp_write/tcp_sent
 * rounds as tcp_sndbuf backpressure requires. The driver sees a SINGLE KL_COMP_WRITE (via
 * drain) only once the WHOLE payload is acknowledged — arbitrary `total` works, bounded only
 * by KL_LWR_POST_SEND_MAX (a sanity cap; the glue additionally bounds by heap availability).
 *
 * Removes the P9-2 small-response bailout: no fixed stack buffer, no "must fit tcp_sndbuf"
 * limit. A 64 KB response now spans many segments + hits sndbuf-full (ERR_MEM) backpressure,
 * resumed on tcp_sent — and still completes as one WRITE. */
#define KL_LWR_POST_SEND_MAX (16u * 1024u * 1024u)   /* 16 MiB response sanity cap */
int kl_comp_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    if (!kl_handle_valid(c->fd)) return -1;
    if (total > KL_LWR_POST_SEND_MAX) return -1;
    /* Concatenate the iovec into one contiguous staging buffer; the glue copies it into its
     * own per-pcb send buffer (kl_lwr_send_begin), so this staging buffer is freed here. */
    unsigned char *buf = kl_malloc(c->alloc, total ? total : 1);
    if (!buf) return -1;
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (off + iov[i].len > total) { kl_free(c->alloc, buf, total ? total : 1); return -1; }
        memcpy(buf + off, iov[i].base, iov[i].len);
        off += iov[i].len;
    }
    int rc = kl_lwr_send_begin((void *)c->fd, buf, off);
    kl_free(c->alloc, buf, total ? total : 1);
    return rc;
}

/* Post one async file send (P9-3): the serialized response head (`head_iov`) followed by
 * `count` bytes from file_fd. The glue preads the file into its per-pcb send buffer after
 * the head and reuses the send-pump (approach (a) — see lwip_raw_glue.h). One KL_COMP_WRITE
 * follows when the whole head+file is acked. fd OWNERSHIP: the glue only READS file_fd; the
 * response layer closes res->file_fd (kl_response_reset/free) — we do NOT close it here. */
int kl_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    if (!kl_handle_valid(c->fd)) return -1;
    if (count > KL_LWR_POST_SEND_MAX || head_total > KL_LWR_POST_SEND_MAX) return -1;
    /* Concatenate the head into a staging buffer; the glue copies head + file into its own
     * per-pcb send buffer, so this staging buffer is freed here. */
    unsigned char *head = kl_malloc(c->alloc, head_total ? head_total : 1);
    if (!head) return -1;
    size_t off = 0;
    for (int i = 0; i < head_n; i++) {
        if (off + head_iov[i].len > head_total) {
            kl_free(c->alloc, head, head_total ? head_total : 1); return -1;
        }
        memcpy(head + off, head_iov[i].base, head_iov[i].len);
        off += head_iov[i].len;
    }
    int rc = kl_lwr_sendfile_begin((void *)c->fd, head, off, file_fd, count);
    kl_free(c->alloc, head, head_total ? head_total : 1);
    return rc;
}

void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    (void)ctx;
    /* Release any in-flight send buffer for this conn (P9-4 will add the full
     * tcp_abort/close-with-outstanding lifetime; freeing the owned send copy here already
     * prevents a leak if a conn is cancelled mid-send). */
    if (kl_handle_valid(fd)) kl_lwr_send_release((void *)fd);
}

int kl_comp_post_udp_recv(struct KlUdp *udp) { (void)udp; return -1; }   /* raw UDP: future */
int kl_comp_post_udp_send(struct KlUdp *udp, const void *data, size_t len,
                          const KlSockAddr *dest) {
    (void)udp; (void)data; (void)len; (void)dest; return -1;
}

/* ── drain: one lwIP tick, then translate glue records into completion events ── */
int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    KlLwrState *st = ctx->loop._backend;

    /* One lwIP mainloop tick (docs §P9-1): sys_check_timeouts() (retransmit, TCP fast/slow,
     * TIME-WAIT) + netif_poll(loopif) (drain the loopback TX queue so raw callbacks fire).
     * The tcp_accept/tcp_recv/tcp_sent callbacks run inline here and enqueue glue records. */
    kl_lwr_lwip_tick(st ? st->loopif : NULL);

    /* Pull the finished raw-API records and translate each into a KlCompletionEvent. */
    KlLwrRecord recs[KL_LWR_MAX_DRAIN];
    int rmax = max < KL_LWR_MAX_DRAIN ? max : KL_LWR_MAX_DRAIN;
    int nr = kl_lwr_drain(recs, rmax);

    int count = 0;
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
             * which the driver handles by zeroing the conn's peer_addr. For P9-2 (loopback,
             * peer address unused by the handler) we leave it 0 rather than pulling a host
             * socket header in to synthesize a sockaddr_in. P9-4 can fill it if needed. */
            ev->peer_len = 0;
            count++;
            continue;
        }

        if (r->kind == KL_LWR_WRITE) {
            KlConn *c = r->owner;
            if (!c) continue;
            ev->kind = KL_COMP_WRITE;
            ev->target = c;
            ev->ok = r->ok;
            ev->bytes = r->nbytes;
            count++;
            continue;
        }
    }

    /* Surface READs for armed conns whose staging has data (or whose peer closed). Raw recv
     * is passive — bytes accumulate in the glue's per-pcb staging independent of when the
     * driver posts a recv — so a READ is delivered here once the conn is armed
     * (kl_comp_post_recv), consuming the arming. This also handles the fast-client race where
     * data arrived in the same tick as the accept, before the recv was posted: the READ is
     * simply picked up on the next drain once comp_on_accept has posted it. */
    for (int i = 0; i < st->armed_count && count < max; ) {
        KlConn *c = st->armed[i];
        int has_data = 0, closed = 0;
        kl_lwr_conn_status((void *)c->fd, &has_data, &closed);
        if (!has_data && !closed) { i++; continue; }   /* nothing yet — keep armed */

        KlCompletionEvent *ev = &out[count];
        memset(ev, 0, sizeof(*ev));
        ev->kind = KL_COMP_READ;
        ev->target = c;
        if (has_data) {
            size_t space = c->read_cap - c->read_len;
            size_t got = kl_lwr_take_staged((void *)c->fd, c->read_buf + c->read_len, space);
            ev->ok = 1;
            ev->bytes = got;
        } else {                       /* closed with no pending data — zero-length READ */
            ev->ok = 0;
            ev->bytes = 0;
        }
        lwr_disarm(st, c);             /* consumed the recv (swaps armed[i] with the last) */
        count++;
        /* do not i++: armed[i] now holds a different conn (disarm swapped) — re-check it */
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
