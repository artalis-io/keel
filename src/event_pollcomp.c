/*
 * event_pollcomp.c — a portable COMPLETION backend over poll() (PAL Phase 8d-0.5).
 *
 * The completion event axis (completion.h) had one implementation: IOCP
 * (event_iocp.c, Windows-only). That left the platform-independent completion driver
 * (completion_driver.c) — and everything built on it (8b/8c/8d) — validatable only by
 * compile-gate + a bring-your-own Windows runtime. This TU is a SECOND completion
 * backend: it implements the same completion.h contract using poll() over ordinary
 * POSIX sockets, so the completion driver runs — and is tested — on Linux/macOS under
 * `make BACKEND=pollcomp test`.
 *
 * It does double duty: it proves the completion axis is genuinely platform-independent
 * (the driver is reused verbatim, no #ifdef), and it makes that axis CI-testable. Like
 * event_iocp.c it is purely the platform layer — the KlEventLoop lifecycle + the
 * completion.h post/drain mechanics; no connection logic (that stays in
 * completion_driver.c), no Win32.
 *
 * Model: each post_* records a pending op (fd + kind + buffer/target). kl_comp_drain
 * builds a pollfd set from the pending ops, polls once, and for each ready op performs
 * the single syscall it represents (accept / recv / send / sendfile / recvfrom /
 * sendto), synthesising a platform-independent KlCompletionEvent. It is the completion
 * facade over readiness — the mirror image of a readiness backend adapting a completion
 * source to the readiness interface.
 *
 * DUAL ROLE (RC-2): this TU is a PURE runtime provider — static KlEventOps +
 * KlCompletionOps hung off a KlEventProvider (kl_event_provider_pollcomp), with NO
 * kl_event_*_builtin / kl_comp_ops_builtin (which would clash with a default backend's
 * own _builtin when pollcomp is injected into a default libkeel at runtime). Its event
 * ops + completion table are named (non-static, kl_pollcomp_*) and declared in
 * event_pollcomp_internal.h so the compiled-in glue (event_pollcomp_builtin.c, linked
 * only for BACKEND=pollcomp) can wrap them as the _builtin symbols the default-path
 * dispatch expects. See event_lwip.c (the pure-provider template) + event_dispatch.c.
 */
#include <keel/event.h>
#include "event_pollcomp_internal.h"
#include <keel/server.h>
#include <keel/connection.h>
#include "udp_cmsg.h"            /* KL_UDP_RX_CTRL_SIZE, kl_udp_parse_local — pktinfo local addr (POSIX) */
#include "event_caps.h"
#include "socket.h"              /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED + seam */
#include "sockaddr_native.h"     /* KlSockAddr <-> host sockaddr at the seam boundary */
#include "completion.h"          /* the abstract axis this TU implements */
#include "datagram_life.h"       /* stable-liveness token for datagram completion ops (neutral) */

#include <poll.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

typedef enum {
    PC_ACCEPT, PC_READ, PC_WRITE, PC_SENDFILE, PC_DGRAM_RECV, PC_DGRAM_SEND,
    PC_CONNECT   /* outbound connect (LC-0): real connect()+POLLOUT+SO_ERROR */
} PcOpType;

/* One pending async op — polled, then completed by the single syscall it names. */
typedef struct KlPcOp {
    struct KlPcOp *next;
    PcOpType       type;
    KlAllocator   *alloc;
    KlSocketHandle fd;                    /* the descriptor this op polls */
    KlStream      *stream;               /* READ / WRITE / SENDFILE target (raw transport) */
    /* Datagram ops (PC_DGRAM_RECV/_SEND): the transport-neutral stable-liveness token, retained at
     * post so the op NEVER dereferences the transport owner afterwards. The recv buffer + capture flags are
     * COPIED at post (into buf/buflen/dg_pktinfo/dg_gro) so the completion touches only the op. */
    KlDgramLife   *life;
    int            dg_pktinfo;            /* UDP_RECV: capture pktinfo local addr */
    int            dg_gro;                /* UDP_RECV: capture GRO segment size */
    int            dg_tos;                /* UDP_RECV: capture received TOS byte (M6.0a) */
    void          *buf;                   /* READ / UDP_RECV: receive buffer (pinned by `life`) */
    size_t         buflen;                /* READ / UDP_RECV: capacity at post time */
    char          *sendbuf;              /* WRITE/SENDFILE head/UDP_SEND: owned copy */
    size_t         send_total, send_done; /* partial-send tracking */
    int            file_fd;               /* SENDFILE */
    uint64_t       file_count, file_off;  /* SENDFILE: file bytes + progress */
    struct sockaddr_storage dest;         /* UDP_SEND / CONNECT destination */
    socklen_t      dest_len;
    /* _Alignas: the builder makes typed struct cmsghdr accesses into this buffer (cmsghdr alignment
     * required); a plain unsigned char[] is only byte-aligned. */
    _Alignas(struct cmsghdr) unsigned char send_ctrl[KL_UDP_TX_CTRL_SIZE]; /* UDP_SEND source-pin/TOS cmsg */
    size_t         send_ctrllen;          /* 0 = no control → plain sendto */
    union {
        void              *watcher_udata;   /* CONNECT: the client's tagged KlWatcher */
    };
    int            aborted;               /* cancelled (idle timeout) — deliver as error */
} KlPcOp;

/* A registered readiness watch (8e-2). kl_event_add stores these for TAGGED udata
 * (KlWatcher pointers, LSB=1 — the shared watcher convention); connection fds (untagged)
 * are driven by posted ops and need no readiness watch. kl_comp_drain polls them
 * alongside the ops and surfaces ready ones as KL_COMP_WATCHER, which the driver routes
 * back through kl_event_dispatch — so thread-pool wakeups, timers, etc. work here. */
typedef struct KlPcWatch {
    struct KlPcWatch *next;
    KlSocketHandle    fd;
    KlEventMask       mask;
    void             *udata;   /* the tagged KlWatcher pointer */
} KlPcWatch;

typedef struct {
    KlAllocator   *alloc;
    KlPcOp        *ops;                    /* singly-linked pending-op list */
    KlPcWatch     *watches;                /* registered readiness watches (8e-2) */
    KlSocketHandle  listen_fd;
    int             primed;
} KlPcState;

/* ── KlEventLoop lifecycle (provider ops; the glue wraps these as *_builtin) ── */

int kl_pollcomp_ev_init(KlEventLoop *loop) {
    KlPcState *st = kl_malloc(loop->alloc, sizeof(*st));
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = loop->alloc;
    st->listen_fd = KL_INVALID_SOCKET;
    loop->_backend = st;
    loop->fd = -1;
    return 0;
}

/* Connection fds are driven by posted overlapped ops, not readiness — for those (untagged
 * udata) add/mod/del are inert, as on IOCP. But a TAGGED udata is a KlWatcher (thread-pool
 * wakeup, timer, generic FD watcher); register it so kl_comp_drain relays its readiness as
 * a KL_COMP_WATCHER event (8e-2). This keys on the shared LSB watcher tag, not on any
 * pollcomp/IOCP specific — the same convention kl_event_dispatch uses. */
int kl_pollcomp_ev_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    if (!((uintptr_t)udata & 1)) return 0;   /* connection — posted ops, no readiness watch */
    KlPcState *st = loop->_backend;
    for (KlPcWatch *w = st->watches; w; w = w->next)
        if (w->fd == fd) { w->mask = mask; w->udata = udata; return 0; }   /* idempotent */
    KlPcWatch *w = kl_malloc(st->alloc, sizeof(*w));
    if (!w) return -1;
    w->fd = fd;
    w->mask = mask;
    w->udata = udata;
    w->next = st->watches;
    st->watches = w;
    return 0;
}
int kl_pollcomp_ev_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    if (!((uintptr_t)udata & 1)) return 0;
    KlPcState *st = loop->_backend;
    for (KlPcWatch *w = st->watches; w; w = w->next)
        if (w->fd == fd) { w->mask = mask; w->udata = udata; return 0; }
    return kl_pollcomp_ev_add(loop, fd, mask, udata);   /* not yet watched — add it */
}
int kl_pollcomp_ev_del(KlEventLoop *loop, KlSocketHandle fd) {
    KlPcState *st = loop->_backend;
    for (KlPcWatch **link = &st->watches; *link; link = &(*link)->next)
        if ((*link)->fd == fd) {
            KlPcWatch *w = *link;
            *link = w->next;
            kl_free(st->alloc, w, sizeof(*w));
            return 0;
        }
    return 0;   /* not watched (a connection fd) — nothing to do */
}

int kl_pollcomp_ev_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* The server drives a completion loop via kl_comp_drain (io_engine seam), not this
     * readiness call. Defined because the KlEventLoop API requires it. */
    (void)loop; (void)out; (void)max;
    if (timeout_ms > 0) poll(NULL, 0, timeout_ms);
    return 0;
}

void kl_pollcomp_ev_close(KlEventLoop *loop) {
    KlPcState *st = loop->_backend;
    if (!st) return;
    KlPcOp *op = st->ops;
    while (op) {
        KlPcOp *next = op->next;
        /* Release the datagram op's stable-token reference (loop teardown drops a never-reaped op
         * WITHOUT emitting an event) — its final release frees the receive storage. Non-datagram ops
         * have op->life == NULL. */
        if (op->life) kl_dgram_life_release(op->life);
        if (op->sendbuf) kl_free(op->alloc, op->sendbuf, op->send_total ? op->send_total : 1);
        kl_free(op->alloc, op, sizeof(*op));
        op = next;
    }
    KlPcWatch *w = st->watches;
    while (w) {
        KlPcWatch *wnext = w->next;
        kl_free(st->alloc, w, sizeof(*w));
        w = wnext;
    }
    kl_free(st->alloc, st, sizeof(*st));
    loop->_backend = NULL;
    loop->fd = -1;
}

/* A completion loop over native fds. COMPLETION makes the Phase 7 negotiation require
 * an OVERLAPPED provider (kl_socket_provider_pollcomp, below). */
unsigned kl_pollcomp_ev_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD;
}

/* The overlapped provider this completion loop needs (5a) — auto-wired by the server/client
 * when the caller configured none, so the pollcomp backend is a source-compatible drop-in. */
const struct KlSocketProvider *kl_pollcomp_ev_native_provider(const KlEventLoop *loop) {
    (void)loop;
    return kl_socket_provider_pollcomp();
}

/* The overlapped socket provider: reuse the POSIX control-plane ops (close, send used
 * by comp_tls_flush, tcp_nodelay, …) and add the OVERLAPPED capability the negotiation
 * keys on — so completion_driver.c's kl_sock_* calls behave normally while the loop
 * advertises completion. */
const KlSocketProvider *kl_socket_provider_pollcomp(void) {
    static KlSocketProvider prov;
    const KlSocketProvider *posix = kl_socket_provider_posix();
    prov.ops = posix->ops;
    prov.context = posix->context;
    prov.capabilities = posix->capabilities | KL_SOCK_CAP_OVERLAPPED;
    prov.dgram = posix->dgram;   /* UDP config/opts (readiness dgram data-plane unused on this loop) */
    return &prov;
}

/* ── completion.h implementation (poll-driven mechanics) ─────────────── */

static void pc_op_push(KlPcState *st, KlPcOp *op) {
    op->next = st->ops;
    st->ops = op;
}

static void pc_op_free(KlPcOp *op) {
    /* A datagram op that still owns its token reference (dropped WITHOUT emitting an event — e.g. a
     * post-failure unwind or loop teardown) releases it here. Emit paths transfer the ref to the
     * event and NULL op->life first, so this does not double-release. */
    if (op->life)
        kl_dgram_life_release(op->life);
    /* send_total is set to the sendbuf allocation size on every path that allocates one
     * (WRITE/SENDFILE/UDP = total, or 1 when total==0 since the alloc is `total ? total : 1`).
     * Fall back to 1 for a zero-length send, else a sized custom allocator mis-buckets the
     * freed 1-byte block. Receives no longer own a buffer (the caller supplies it). */
    if (op->sendbuf)
        kl_free(op->alloc, op->sendbuf, op->send_total ? op->send_total : 1);
    kl_free(op->alloc, op, sizeof(*op));
}

/* Raw receive: recv up to `cap` bytes into the caller-supplied `buf` on `stream`. No TLS /
 * connection-state knowledge — the HTTP adapter chose the buffer. */
static int pc_comp_post_recv(KlStream *stream, void *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    KlPcState *st = stream->ctx->loop._backend;
    KlPcOp *op = kl_malloc(stream->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->alloc = stream->alloc;
    op->stream = stream;
    op->fd = stream->fd;
    op->type = PC_READ;
    op->buf = buf;
    op->buflen = cap;
    pc_op_push(st, op);
    return 0;
}

static int pc_comp_post_send(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total) {
    KlPcState *st = stream->ctx->loop._backend;
    KlPcOp *op = kl_malloc(stream->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = PC_WRITE;
    op->alloc = stream->alloc;
    op->stream = stream;
    op->fd = stream->fd;
    op->send_total = total;
    op->sendbuf = kl_malloc(stream->alloc, total ? total : 1);
    if (!op->sendbuf) { op->send_total = 0; pc_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        memcpy(op->sendbuf + off, iov[i].base, iov[i].len);
        off += iov[i].len;
    }
    pc_op_push(st, op);
    return 0;
}

static int pc_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    KlStream *stream = &c->stream;   /* post_sendfile stays KlConn-typed (Phase-A audit §8) */
    KlPcState *st = stream->ctx->loop._backend;
    KlPcOp *op = kl_malloc(stream->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = PC_SENDFILE;
    op->alloc = stream->alloc;
    op->stream = stream;
    op->fd = stream->fd;
    op->file_fd = file_fd;
    op->file_count = count;
    op->send_total = head_total;
    op->sendbuf = kl_malloc(stream->alloc, head_total ? head_total : 1);
    if (!op->sendbuf) { op->send_total = 0; pc_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < head_n; i++) {
        memcpy(op->sendbuf + off, head_iov[i].base, head_iov[i].len);
        off += head_iov[i].len;
    }
    pc_op_push(st, op);
    return 0;
}

static int pc_comp_post_accept(struct KlServer *s) {
    if (!s) return -1;
    KlPcState *st = s->ev.loop._backend;
    /* One accept op suffices: on completion the driver refills. Avoid stacking
     * duplicate accept ops on the same listen fd (they would just spin on EAGAIN). */
    for (const KlPcOp *o = st->ops; o; o = o->next)
        if (o->type == PC_ACCEPT) return 0;
    KlPcOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = PC_ACCEPT;
    op->alloc = st->alloc;
    op->fd = st->listen_fd;
    pc_op_push(st, op);
    return 0;
}

static int pc_comp_post_dgram_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *rop) {
    KlPcState *st = ctx->loop._backend;
    KlPcOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;                 /* nothing taken → caller releases its retained token ref */
    memset(op, 0, sizeof(*op));
    op->type = PC_DGRAM_RECV;
    op->alloc = st->alloc;
    op->fd = rop->fd;
    /* The receive buffer + capture flags travel in the descriptor; the op never dereferences a transport.
     * The buffer stays valid because the token ref (transferred below) pins it past the op's lifetime. */
    op->buf        = rop->buf;
    op->buflen     = rop->cap;
    op->dg_pktinfo = (rop->capture & KL_DGRAM_RX_PKTINFO) != 0;
    op->dg_gro     = (rop->capture & KL_DGRAM_RX_GRO)     != 0;
    op->dg_tos     = (rop->capture & KL_DGRAM_RX_TOS)     != 0;
    op->life = rop->life;               /* TRANSFERRED into the op (no retain — the caller retained) */
    pc_op_push(st, op);
    return 0;
}

static int pc_comp_post_dgram_send(struct KlEventCtx *ctx, const KlDgramSendOp *sop) {
    KlPcState *st = ctx->loop._backend;
    KlPcOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;                 /* nothing taken → caller releases its ref */
    memset(op, 0, sizeof(*op));
    op->type = PC_DGRAM_SEND;
    op->alloc = st->alloc;
    op->fd = sop->fd;
    op->send_total = sop->len;
    op->sendbuf = kl_malloc(st->alloc, sop->len ? sop->len : 1);
    if (!op->sendbuf) { op->send_total = 0; pc_op_free(op); return -1; }   /* life unset → caller releases */
    memcpy(op->sendbuf, sop->data, sop->len);   /* COPY payload before accept */
    /* Marshal the neutral dest to a host sockaddr for the send at drain time. */
    if (sop->dest && kl_sockaddr_family(sop->dest) != KL_AF_UNSPEC)
        op->dest_len = kl_sockaddr_to_native(sop->dest, &op->dest);
    /* Source-pin / per-packet TOS control message — COPIED into the op at post (§2.5.1); the drain uses
     * sendmsg with it (the sendto fast path stays for plain sends). */
    if ((sop->src && kl_sockaddr_family(sop->src) != KL_AF_UNSPEC) || sop->tos >= 0) {
        struct sockaddr_storage sss;
        const struct sockaddr *src_sa = NULL;
        if (sop->src && kl_sockaddr_family(sop->src) != KL_AF_UNSPEC && kl_sockaddr_to_native(sop->src, &sss))
            src_sa = (const struct sockaddr *)&sss;
        int fam = kl_udp_send_family((int)op->fd, op->dest_len ? (struct sockaddr *)&op->dest : NULL, src_sa);
        if (fam < 0) { pc_op_free(op); return -1; }   /* undeterminable family → fail the post */
        if (kl_udp_build_control(op->send_ctrl, sizeof(op->send_ctrl), src_sa, sop->tos, fam,
                                 &op->send_ctrllen) != 0) {
            pc_op_free(op); return -1;   /* requested source-pin/TOS could not be built → fail the post */
        }
    }
    op->life = sop->life;               /* TRANSFERRED into the op (no retain) */
    pc_op_push(st, op);
    return 0;
}

/* Cancel the outstanding datagram op(s) of `kind` for `life` (7B-2): mark them aborted so the next
 * drain delivers their terminal completion (which releases the token ref). Idempotent; NO ref release
 * here. Pollcomp has no in-kernel op — "cancel" just flips the abort flag on the tracked op. */
static int pc_comp_cancel_dgram(struct KlEventCtx *ctx, KlDgramLife *life, KlDgramOpKind kind) {
    KlPcState *st = ctx->loop._backend;
    PcOpType want = (kind == KL_DGRAM_OP_SEND) ? PC_DGRAM_SEND : PC_DGRAM_RECV;
    for (KlPcOp *o = st->ops; o; o = o->next)
        if (o->life == life && o->type == want) o->aborted = 1;
    return 0;
}

/* Classify retirement (§4.3): a matching op still tracked in st->ops is PENDING (its cancelled
 * completion has not yet drained + released); none tracked means it physically retired. Pollcomp
 * never quarantines — a portable double where every posted op completes in a drain. */
static KlDgramRetireResult pc_comp_retire_dgram(struct KlEventCtx *ctx, KlDgramLife *life,
                                                KlDgramOpKind kind, int *transport_err) {
    const KlPcState *st = ctx->loop._backend;
    PcOpType want = (kind == KL_DGRAM_OP_SEND) ? PC_DGRAM_SEND : PC_DGRAM_RECV;
    if (transport_err) *transport_err = 0;
    for (const KlPcOp *o = st->ops; o; o = o->next)
        if (o->life == life && o->type == want) return KL_DGRAM_RETIRE_PENDING;
    return KL_DGRAM_RETIRE_RETIRED;
}

/* Post an outbound connect (LC-0): issue the real nonblocking connect() now (the client
 * already made the fd nonblocking), then poll POLLOUT and read SO_ERROR on completion — a
 * genuine, portable connect completion (not a fake readiness relay). On connect() returning
 * 0 (immediate loopback success) the op still completes on the next drain (POLLOUT is
 * immediately ready), keeping the completion single + deferred. The result surfaces as
 * KL_COMP_CONNECT against the client's tagged watcher; the driver routes it to
 * he_on_writable, which re-reads SO_ERROR (0 → win, else fail). */
static int pc_comp_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                                const KlSockAddr *addr, void *watcher_udata) {
    KlPcState *st = ctx->loop._backend;
    KlPcOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = PC_CONNECT;
    op->alloc = st->alloc;
    op->fd = fd;
    op->watcher_udata = watcher_udata;

    int rc = kl_sock_connect(ctx->sockets, fd, addr);
    if (rc < 0 && errno != EINPROGRESS) {
        pc_op_free(op);
        return -1;                 /* hard local failure — caller closes fd + tries next */
    }
    /* rc == 0 (connected immediately) or EINPROGRESS: complete via POLLOUT on the drain. */
    pc_op_push(st, op);
    return 0;
}

static int pc_comp_prime_accepts(struct KlServer *s) {
    if (!s) return -1;
    KlPcState *st = s->ev.loop._backend;
    if (st->primed) return 1;              /* setup already done — report the window (6B-3 2b-ii) */
    st->listen_fd = s->listen_fd;
    st->primed = 1;
    /* Post-driven, window 1: latch the listen fd; the completion KlListener posts the single accept
     * (reserve-before-post backpressure). No accept posted here — the listener arms it. */
    return 1;
}

/* Cancel pending ops on `fd`. Server conn ops (idle-timeout sweep): mark aborted so the next
 * drain delivers an error completion, driving the driver's normal release — we mark rather
 * than remove so the invariant "a conn is released only from a completion" holds. A PC_CONNECT
 * op is different: the async client owns the connect fd + its (detached) watcher and tears both
 * down synchronously (he_close_attempts / he_fail_attempt), so its pending op is DROPPED here
 * (freed, no completion) — surfacing an aborted KL_COMP_CONNECT would dispatch against the
 * just-freed watcher. (LC-0) */
static void pc_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    KlPcState *st = ctx->loop._backend;
    KlPcOp **link = &st->ops;
    while (*link) {
        KlPcOp *o = *link;
        if (o->fd == fd && o->type == PC_CONNECT) {
            *link = o->next;
            pc_op_free(o);
            continue;
        }
        if (o->fd == fd) o->aborted = 1;
        link = &o->next;
    }
}

/* ── drain: poll the pending ops, complete the ready ones ─────────────── */

/* Fill an error completion for a cancelled op. Returns 1 if an event was written
 * (a connection/UDP target exists), 0 for an accept op (no target — just drop it). */
static int pc_emit_abort(KlPcOp *op, KlCompletionEvent *ev) {
    memset(ev, 0, sizeof(*ev));
    ev->ok = 0;
    switch (op->type) {
    case PC_READ:                    ev->kind = KL_COMP_READ;  ev->target = op->stream; return 1;
    case PC_WRITE: case PC_SENDFILE: ev->kind = KL_COMP_WRITE; ev->target = op->stream; return 1;
    /* Datagram: transfer the token reference op → event (released after dispatch); the op no longer
     * owns it, so pc_op_free must not release it. Never touch the transport owner. */
    case PC_DGRAM_RECV:                ev->kind = KL_COMP_DGRAM_RECV; ev->life = op->life; op->life = NULL; return 1;
    case PC_DGRAM_SEND:                ev->kind = KL_COMP_DGRAM_SEND; ev->life = op->life; op->life = NULL; return 1;
    case PC_CONNECT:                 /* never reached: cancelled connect ops are freed in
                                      * pc_comp_cancel, not delivered as aborts. */
                                     return 0;
    case PC_ACCEPT:                  /* a cancelled accept (listener close, 6B-3 2b-ii) — deliver
                                      * ok=0 with no fd so the completion listener retires this
                                      * posted accept (returns its credit). The op never pre-accepts
                                      * a socket (accept() runs in pc_complete), so nothing leaks. */
                                     ev->kind = KL_COMP_ACCEPT; ev->target = NULL; return 1;
    }
    return 0;
}

static void pc_set_blocking(KlSocketHandle fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
}

/* Complete a ready op into `ev`. Returns 1 if `ev` was filled (op done → remove),
 * 0 if the op made partial/no progress and should stay pending, -1 on op failure
 * (ev filled with ok=0 for TCP so the driver tears down; op removed). */
static int pc_complete(KlPcOp *op, KlCompletionEvent *ev) {
    memset(ev, 0, sizeof(*ev));
    switch (op->type) {
    case PC_ACCEPT: {
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        int a = accept(op->fd, (struct sockaddr *)&ss, &slen);
        if (a < 0) return 0;                 /* EAGAIN/spurious — keep the accept op */
        pc_set_blocking(a);
        ev->kind = KL_COMP_ACCEPT;
        ev->target = NULL;   /* ACCEPT: server recovered from ctx at dispatch (6B-3) */
        ev->ok = 1;
        ev->accepted_fd = a;
        if (slen > 0)                            /* native → neutral once, at the seam */
            (void)kl_sockaddr_from_native(&ev->peer, (struct sockaddr *)&ss, slen);
        return 1;
    }
    case PC_READ: {
        ssize_t n = recv(op->fd, op->buf, op->buflen, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        ev->kind = KL_COMP_READ;
        ev->target = op->stream;               /* raw bytes landed in the caller's buffer */
        ev->ok = 1;
        ev->bytes = (n > 0) ? (size_t)n : 0;   /* 0/-1 → peer closed; driver closes */
        return 1;
    }
    case PC_WRITE: {
        while (op->send_done < op->send_total) {
            ssize_t n = send(op->fd, op->sendbuf + op->send_done,
                             op->send_total - op->send_done, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  /* re-poll */
                ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 0;
                return -1;
            }
            op->send_done += (size_t)n;
        }
        ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 1;
        ev->bytes = op->send_total;
        return 1;
    }
    case PC_SENDFILE: {
        while (op->send_done < op->send_total) {          /* head first */
            ssize_t n = send(op->fd, op->sendbuf + op->send_done,
                             op->send_total - op->send_done, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 0;
                return -1;
            }
            op->send_done += (size_t)n;
        }
        while (op->file_off < op->file_count) {           /* then the file bytes */
            uint64_t off = op->file_off;
            ssize_t n = kl_sockdef_sendfile(op->fd, op->file_fd, &off,
                                            (size_t)(op->file_count - op->file_off));
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 0;
                return -1;
            }
            if (n == 0) break;
            op->file_off = off;
        }
        ev->kind = KL_COMP_WRITE; ev->target = op->stream; ev->ok = 1;
        ev->bytes = op->send_total + op->file_count;
        return 1;
    }
    case PC_DGRAM_RECV: {
        struct sockaddr_storage ss;
        unsigned char ctrl[KL_UDP_RX_CTRL_SIZE];
        /* The buffer + flags were COPIED at post; the token ref pins the buffer past this op. */
        struct iovec iov = { .iov_base = op->buf, .iov_len = op->buflen };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = &ss;
        msg.msg_namelen = sizeof(ss);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;                    /* capture pktinfo (local addr) cmsg */
        msg.msg_controllen = sizeof(ctrl);
        ssize_t n;
        do { n = recvmsg(op->fd, &msg, 0); } while (n < 0 && errno == EINTR);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        ev->kind = KL_COMP_DGRAM_RECV;
        ev->life = op->life; op->life = NULL;      /* transfer token ref op → event */
        ev->ok = (n >= 0);
        ev->bytes = (n > 0) ? (size_t)n : 0;
        ev->buf = op->buf;
        if (n >= 0 && msg.msg_namelen > 0)         /* source: native → neutral at the seam */
            (void)kl_sockaddr_from_native(&ev->peer, (struct sockaddr *)&ss,
                                          msg.msg_namelen);
        if (n >= 0 && op->dg_pktinfo) {           /* local (dest) addr via pktinfo cmsg */
            struct sockaddr_storage local_ss;
            socklen_t local_len = kl_udp_parse_local(&msg, &local_ss);
            if (local_len)
                (void)kl_sockaddr_from_native(&ev->local,
                                              (struct sockaddr *)&local_ss, local_len);
        }
        ev->tos = -1;                              /* M6.0a: default "none"; parse only if requested */
        if (n >= 0) {
            if (op->dg_gro)                       /* GRO coalesced segment size */
                ev->gro_seg = kl_udp_parse_gro(&msg);
            if (op->dg_tos)                        /* received TOS/Traffic-Class byte */
                ev->tos = kl_udp_parse_tos(&msg);
            if (msg.msg_flags & MSG_TRUNC)         /* datagram truncated to recv_buf */
                ev->truncated = 1;
        }
        return 1;
    }
    case PC_DGRAM_SEND: {
        ssize_t n;
        if (op->send_ctrllen) {                    /* source-pin / TOS → sendmsg with the copied cmsg */
            struct iovec iov = { .iov_base = op->sendbuf, .iov_len = op->send_total };
            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_name = op->dest_len ? (void *)&op->dest : NULL;
            msg.msg_namelen = op->dest_len;
            msg.msg_iov = &iov; msg.msg_iovlen = 1;
            msg.msg_control = op->send_ctrl; msg.msg_controllen = op->send_ctrllen;
            n = sendmsg(op->fd, &msg, 0);
        } else {
            n = sendto(op->fd, op->sendbuf, op->send_total, 0,
                       op->dest_len ? (struct sockaddr *)&op->dest : NULL, op->dest_len);
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        ev->kind = KL_COMP_DGRAM_SEND;
        ev->life = op->life; op->life = NULL;      /* transfer token ref op → event */
        ev->ok = (n >= 0);
        ev->bytes = (n > 0) ? (size_t)n : 0;
        return 1;
    }
    case PC_CONNECT: {
        /* POLLOUT fired: the nonblocking connect settled. Read SO_ERROR to decide win/fail.
         * Surface KL_COMP_CONNECT against the client's tagged watcher, encoding the result in
         * the delivered mask: KL_EVENT_WRITE = connected, 0 = failed. (The client's connect
         * watcher trusts the delivered result rather than re-reading SO_ERROR, so this is
         * uniform with io_uring where SO_ERROR is not preserved after a failed connect.) */
        int soerr = 0;
        (void)getsockopt(op->fd, SOL_SOCKET, SO_ERROR, &soerr, &(socklen_t){ sizeof(soerr) });
        ev->kind = KL_COMP_CONNECT;
        ev->target = op->watcher_udata;
        ev->ok = (soerr == 0);
        ev->bytes = ev->ok ? (size_t)KL_EVENT_WRITE : 0;
        return 1;
    }
    }
    return 0;
}

static short pc_poll_events(PcOpType t) {
    switch (t) {
    case PC_ACCEPT: case PC_READ: case PC_DGRAM_RECV: return POLLIN;
    default: return POLLOUT;   /* PC_WRITE / PC_SENDFILE / PC_DGRAM_SEND */
    }
}

static short pc_watch_events(KlEventMask mask) {
    short e = 0;
    if (mask & KL_EVENT_READ)  e |= POLLIN;
    if (mask & KL_EVENT_WRITE) e |= POLLOUT;
    return e;
}

static int pc_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    KlPcState *st = ctx->loop._backend;
    int count = 0;

    /* Deliver cancelled ops (kl_comp_cancel) as error completions first, so the driver
     * releases the idle-timed-out connection through its normal completion path. */
    KlPcOp **link = &st->ops;
    while (*link && count < max) {
        KlPcOp *op = *link;
        if (!op->aborted) { link = &op->next; continue; }
        if (pc_emit_abort(op, &out[count])) count++;
        *link = op->next;
        pc_op_free(op);
    }
    if (count >= max) return count;

    /* Count pending ops + registered watches; build one pollfd array (ops then watches). */
    size_t nops = 0, nwatch = 0;
    for (const KlPcOp *o = st->ops; o; o = o->next) nops++;
    for (const KlPcWatch *w = st->watches; w; w = w->next) nwatch++;
    size_t nfds = nops + nwatch;
    if (nfds == 0) {
        if (count == 0 && timeout_ms != 0) poll(NULL, 0, timeout_ms);
        return count;
    }

    struct pollfd *pfds = kl_malloc(st->alloc, nfds * sizeof(*pfds));
    if (!pfds) return -1;
    KlPcOp **oref = NULL;
    if (nops) {
        oref = kl_malloc(st->alloc, nops * sizeof(*oref));
        if (!oref) { kl_free(st->alloc, pfds, nfds * sizeof(*pfds)); return -1; }
    }

    size_t i = 0;
    for (KlPcOp *o = st->ops; o; o = o->next, i++) {
        pfds[i].fd = o->fd;
        pfds[i].events = pc_poll_events(o->type);
        pfds[i].revents = 0;
        oref[i] = o;
    }
    for (const KlPcWatch *w = st->watches; w; w = w->next, i++) {
        pfds[i].fd = w->fd;
        pfds[i].events = pc_watch_events(w->mask);
        pfds[i].revents = 0;
    }

    int pr = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (pr > 0) {
        /* Completed ops (pfds[0..nops)). */
        for (i = 0; i < nops && count < max; i++) {
            KlPcOp *op = oref[i];
            short re = pfds[i].revents;
            if (!re) continue;
            /* POLLERR/POLLHUP without our event still lets the syscall report the
             * error/EOF; pc_complete surfaces ok=0 for TCP so the driver closes. */
            int r = pc_complete(op, &out[count]);
            if (r == 0) continue;                 /* partial/spurious — keep the op */
            count++;                              /* r == 1 (done) or -1 (failed) */
            /* Unlink and free the completed op. */
            KlPcOp **ulink = &st->ops;
            while (*ulink && *ulink != op) ulink = &(*ulink)->next;
            if (*ulink) *ulink = op->next;
            pc_op_free(op);
        }
        /* Ready watches (pfds[nops..nfds)) — level-triggered, persistent. Surface a
         * KL_COMP_WATCHER for each; the driver routes it to kl_event_dispatch (8e-2). The
         * watch list is unmodified during processing, so a parallel re-walk is safe. */
        size_t j = nops;
        for (const KlPcWatch *w = st->watches; w && count < max; w = w->next, j++) {
            short re = pfds[j].revents;
            if (!re) continue;
            KlEventMask ready = 0;
            if (re & (POLLIN | POLLHUP | POLLERR)) ready |= KL_EVENT_READ;
            if (re & POLLOUT) ready |= KL_EVENT_WRITE;
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_WATCHER;
            out[count].target = w->udata;         /* the tagged KlWatcher */
            out[count].bytes = (size_t)ready;      /* carry the ready mask */
            out[count].ok = 1;
            count++;
        }
    }

    kl_free(st->alloc, pfds, nfds * sizeof(*pfds));
    if (oref) kl_free(st->alloc, oref, nops * sizeof(*oref));
    return count;
}

/* Teardown accept-side force-completion (6B-3 2b review). Does NOT reap — the server drives the
 * drain (kl_comp_run) so every dequeued op gets its normal routing. This only marks each posted
 * accept aborted, so the next pc_comp_drain's abort pass (pc_emit_abort) emits a KL_COMP_ACCEPT
 * ok=0 that retires the listener. Synchronous → always succeeds. */
static int pc_shutdown_accepts(struct KlServer *s) {
    KlPcState *st = s->ev.loop._backend;
    for (KlPcOp *o = st->ops; o; o = o->next)
        if (o->type == PC_ACCEPT) o->aborted = 1;
    return 0;
}

/* ── Completion sub-vtable (RC-1) ─────────────────────────────────────────
 * Group this backend's completion primitives so the dispatch (completion_dispatch.c)
 * can reach them through a runtime provider (loop->ops->completion, the injected path)
 * or, on the compiled-in BACKEND=pollcomp path, via the glue's kl_comp_ops_builtin()
 * (event_pollcomp_builtin.c) — which just returns this same table. Named (non-static)
 * so the glue can reference it; declared in event_pollcomp_internal.h. */
const KlCompletionOps kl_pollcomp_completion_ops = {
    pc_comp_drain, pc_comp_prime_accepts, pc_comp_post_recv, pc_comp_post_send,
    pc_comp_post_accept, pc_comp_post_sendfile, pc_comp_cancel,
    pc_comp_post_dgram_recv, pc_comp_post_dgram_send,
    pc_comp_cancel_dgram, pc_comp_retire_dgram, pc_comp_post_connect,
    pc_shutdown_accepts,
};

/* ── Runtime event provider (RC-2) ────────────────────────────────────────
 * The pure-runtime form: KlEventOps carrying the completion sub-vtable via .completion,
 * wrapped in a named KlEventProvider. Handed to KlConfig.event_provider to INJECT the
 * pollcomp completion axis into an otherwise-default (epoll/kqueue) libkeel — the RC-2
 * proof. No kl_event_*_builtin / kl_comp_ops_builtin appear in THIS TU, so it never
 * clashes with the default backend's compiled-in symbols. Mirrors event_lwip.c. */
static const KlEventOps pollcomp_event_ops = {
    .init = kl_pollcomp_ev_init, .add = kl_pollcomp_ev_add, .mod = kl_pollcomp_ev_mod,
    .del = kl_pollcomp_ev_del, .wait = kl_pollcomp_ev_wait, .close = kl_pollcomp_ev_close,
    .caps = kl_pollcomp_ev_caps, .native_provider = kl_pollcomp_ev_native_provider,
    .completion = &kl_pollcomp_completion_ops,   /* carry the completion axis (RC-2) */
};

static const KlEventProvider pollcomp_event_provider = { &pollcomp_event_ops, "pollcomp" };

const KlEventProvider *kl_event_provider_pollcomp(void) { return &pollcomp_event_provider; }
