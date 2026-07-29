/*
 * event_iouring_comp.c — a COMPLETION-native io_uring backend (PAL Phase 8f).
 *
 * The completion event axis (completion.h) is natively a completion engine's home:
 * post an async op, drain finished ops. io_uring is a completion engine by construction
 * — submit SQEs, reap CQEs — so it is the truest fit for the axis, more than IOCP
 * (per-op OVERLAPPED bookkeeping) or the pollcomp facade (completion faked over poll()).
 *
 * This is the THIRD implementation of the same completion.h contract, after event_iocp.c
 * (Windows) and event_pollcomp.c (portable poll facade). It reuses the platform-independent
 * completion driver (completion_driver.c) VERBATIM — no #ifdef, no io_uring symbol leaks
 * into the driver — a third proof the axis is genuinely implementation-independent, and
 * the first PRODUCTION (not test-facade) completion backend that is CI-testable on Linux.
 *
 * It coexists with event_iouring.c, which drives the SAME engine but ADAPTED to the
 * readiness interface (io_uring as a poll-multiplexer: prep_poll_add, loop.fd=-1,
 * advertises KL_EVENT_CAP_READINESS). That backend's own comment noted a
 * KL_EVENT_CAP_COMPLETION variant "is Phase 8"; this TU is it. The Makefile selects one
 * via BACKEND=iouring (readiness) vs BACKEND=iouringcomp (this, completion). Nothing is
 * removed.
 *
 * Model: each kl_comp_post_* prepares an SQE (recv/send/accept/…) with the op pointer as
 * user_data and queues it; kl_comp_drain submits the batch and waits, then for each CQE
 * recovers the op, maps cqe->res to a platform-independent KlCompletionEvent, and (for a
 * short write) re-prepares the remainder rather than completing. The kernel performs the
 * I/O into the op's buffers — the driver sees only high-level, completed events. A
 * readiness FD watch (kl_event_add with a tagged KlWatcher udata — thread-pool wakeup,
 * timer) is armed as a single-shot IORING_OP_POLL_ADD and surfaced as KL_COMP_WATCHER
 * (8e-2), re-armed on each fire; how a completion backend watches readiness fds alongside
 * its completions is its own business, per the abstract contract.
 *
 * Kernel-feature note: single-shot accept + single-shot poll (re-armed) are used so the
 * backend runs on older kernels (multishot accept 5.19+, multishot poll 5.13+); prep_recv/
 * send/accept/cancel are 5.5–5.6+. Zero-copy file send (splice) is a Phase-8f-2 refinement
 * — the file body is currently pread into the send buffer. See
 * docs/phase8f_iouring_completion_design.md.
 */
#include <keel/event.h>
#include <keel/server.h>
#include <keel/connection.h>
#include <keel/udp.h>            /* KlUdp — datagram recv/send over completion */
#include <keel/tls.h>            /* KlTls feed_input — deliver received ciphertext */
#include "event_caps.h"
#include "socket.h"              /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED + seam */
#include "completion.h"          /* the abstract axis this TU implements */
#include "platform.h"            /* kl_plat_file_pread — file body into the send buffer */

#include <liburing.h>
#include <poll.h>                /* POLLIN/POLLOUT — poll-add mask + CQE revents */
#include <sys/socket.h>          /* struct msghdr — UDP recvmsg/sendmsg */
#include <sys/uio.h>             /* struct iovec */
#include <string.h>
#include <errno.h>
#include <stdint.h>              /* SIZE_MAX — sendfile buffer overflow guard */
#include <time.h>

#define KL_IOU_RING_SIZE  2048                /* SQ/CQ depth: 256 conns × a few ops + slack */
#define KL_IOU_CIPHER_SIZE (17u * 1024u)      /* one TLS record + slack (mirrors IOCP/pollcomp) */

/* Op kind. The FIRST field of both KlIouOp and KlIouWatch is this enum, so a CQE's
 * user_data can be discriminated by reading *(IouOpType*). IOU_WATCH tags a poll-add. */
typedef enum {
    IOU_ACCEPT, IOU_READ, IOU_TLS_RECV, IOU_WRITE,
    IOU_UDP_RECV, IOU_UDP_SEND, IOU_WATCH
} IouOpType;

/* One in-flight async op — its buffers stay valid until the CQE arrives. */
typedef struct KlIouOp {
    IouOpType      type;                  /* MUST be first (CQE discriminator) */
    struct KlIouOp *next;
    KlAllocator   *alloc;
    KlSocketHandle fd;
    KlConn        *conn;                  /* READ / TLS_RECV / WRITE */
    KlUdp         *udp;                   /* UDP_RECV / UDP_SEND */
    void          *buf;                   /* READ: recv target (read_buf slice / cipher) */
    size_t         buflen;
    char          *sendbuf;               /* WRITE/UDP_SEND: owned copy; TLS_RECV: cipher */
    size_t         sendcap;               /* allocated size of sendbuf (for the sized free) */
    size_t         send_total, send_done; /* partial-send tracking (<= sendcap) */
    struct msghdr  msgh;                  /* UDP: recvmsg/sendmsg header */
    struct iovec   msgiov;                /* UDP: single iovec */
    struct sockaddr_storage peer;         /* ACCEPT peer / UDP addr (msg_name) */
    socklen_t      peer_len;
    int            aborted;               /* cancelled (idle timeout) — deliver as error */
} KlIouOp;

/* A registered readiness watch (8e-2). Armed as a single-shot POLL_ADD; re-armed on each
 * fire. Its FIRST field is IOU_WATCH so a poll CQE's user_data is recognised. */
typedef struct KlIouWatch {
    IouOpType         type;               /* == IOU_WATCH (MUST be first) */
    struct KlIouWatch *next;
    KlSocketHandle     fd;
    KlEventMask        mask;
    void              *udata;             /* the tagged KlWatcher pointer */
    int                armed;             /* a POLL_ADD is outstanding for this watch */
    int                removed;           /* kl_event_del pending — free on its CQE */
} KlIouWatch;

typedef struct {
    struct io_uring ring;
    KlAllocator    *alloc;
    KlIouOp        *ops;                   /* in-flight op list (for cancel + cleanup) */
    KlIouWatch     *watches;               /* registered readiness watches (8e-2) */
    KlSocketHandle  listen_fd;
    int             accept_pending;        /* one accept op outstanding at a time */
    int             primed;
} KlIouState;

/* ── SQE helper: flush the SQ if it is full, then retry once. ─────────── */
static struct io_uring_sqe *iou_sqe(KlIouState *st) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&st->ring);
    if (!sqe) {
        io_uring_submit(&st->ring);        /* drain queued SQEs to free ring slots */
        sqe = io_uring_get_sqe(&st->ring);
    }
    return sqe;
}

static unsigned iou_poll_mask(KlEventMask mask) {
    unsigned e = 0;
    if (mask & KL_EVENT_READ)  e |= POLLIN;
    if (mask & KL_EVENT_WRITE) e |= POLLOUT;
    return e;
}

/* ── KlEventLoop lifecycle ───────────────────────────────────────────── */

int kl_event_init(KlEventLoop *loop) {
    KlIouState *st = kl_malloc(loop->alloc, sizeof(*st));
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = loop->alloc;
    st->listen_fd = KL_INVALID_SOCKET;
    if (io_uring_queue_init(KL_IOU_RING_SIZE, &st->ring, 0) < 0) {
        kl_free(loop->alloc, st, sizeof(*st));
        return -1;
    }
    loop->_backend = st;
    loop->fd = -1;
    return 0;
}

/* Connection fds are driven by posted ops, not readiness — for those (untagged udata)
 * add/mod/del are inert, as on IOCP/pollcomp. A TAGGED udata is a KlWatcher (thread-pool
 * wakeup, timer, generic FD watcher); register it and arm a single-shot POLL_ADD so
 * kl_comp_drain relays its readiness as a KL_COMP_WATCHER (8e-2). Keys on the shared LSB
 * watcher tag, not on anything io_uring-specific. */
/* Arm a single-shot POLL_ADD for the watch. Exactly one poll is outstanding per watch:
 * skip if one already is (a mask change while armed keeps the old poll — acceptable, our
 * completion watchers use a constant READ mask). The re-arm after each fire comes from
 * kl_watcher_rearm → kl_event_mod (kl_event_dispatch calls it), not from the drain. */
static int iou_arm_watch(KlIouState *st, KlIouWatch *w) {
    if (w->armed) return 0;
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) return -1;
    io_uring_prep_poll_add(sqe, w->fd, iou_poll_mask(w->mask));
    io_uring_sqe_set_data(sqe, w);
    w->armed = 1;
    return 0;
}

int kl_event_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    if (!((uintptr_t)udata & 1)) return 0;   /* connection — posted ops, no readiness watch */
    KlIouState *st = loop->_backend;
    for (KlIouWatch *w = st->watches; w; w = w->next)
        if (w->fd == fd) { w->mask = mask; w->udata = udata; return 0; }   /* idempotent */
    KlIouWatch *w = kl_malloc(st->alloc, sizeof(*w));
    if (!w) return -1;
    memset(w, 0, sizeof(*w));
    w->type = IOU_WATCH;
    w->fd = fd;
    w->mask = mask;
    w->udata = udata;
    w->next = st->watches;
    st->watches = w;
    if (iou_arm_watch(st, w) < 0) { st->watches = w->next; kl_free(st->alloc, w, sizeof(*w)); return -1; }
    return 0;
}

int kl_event_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    if (!((uintptr_t)udata & 1)) return 0;
    KlIouState *st = loop->_backend;
    for (KlIouWatch *w = st->watches; w; w = w->next)
        if (w->fd == fd) {
            w->mask = mask; w->udata = udata;
            /* The in-flight poll is single-shot with the old mask; re-arm with the new
             * one. The old poll's CQE (if it fires) just re-arms again — harmless. */
            return iou_arm_watch(st, w);
        }
    return kl_event_add(loop, fd, mask, udata);   /* not yet watched — add it */
}

int kl_event_del(KlEventLoop *loop, KlSocketHandle fd) {
    KlIouState *st = loop->_backend;
    for (KlIouWatch *w = st->watches; w; w = w->next)
        if (w->fd == fd && !w->removed) {
            w->removed = 1;
            for (KlIouWatch **link = &st->watches; *link; link = &(*link)->next)
                if (*link == w) { *link = w->next; break; }   /* unlink — re-add is fresh */
            w->next = NULL;
            if (w->armed) {
                /* A POLL_ADD is in the kernel — cancel it; the poll's –ECANCELED CQE
                 * (data = w) frees the watch. Freeing here would leave the kernel writing
                 * a CQE against freed memory. */
                struct io_uring_sqe *sqe = iou_sqe(st);
                if (sqe) {
                    io_uring_prep_poll_remove(sqe, (__u64)(uintptr_t)w);
                    io_uring_sqe_set_data(sqe, NULL);   /* sentinel — ignore this CQE */
                }
            } else {
                kl_free(st->alloc, w, sizeof(*w));      /* no poll outstanding — free now */
            }
            return 0;
        }
    return 0;   /* not watched (a connection fd) — nothing to do */
}

int kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* The server drives a completion loop via kl_comp_drain (io_engine seam), not this
     * readiness call. Defined because the KlEventLoop API requires it. */
    (void)loop; (void)out; (void)max;
    if (timeout_ms > 0) {
        struct timespec ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void iou_op_free(KlIouOp *op) {
    if (op->sendbuf) kl_free(op->alloc, op->sendbuf, op->sendcap);
    kl_free(op->alloc, op, sizeof(*op));
}

void kl_event_close(KlEventLoop *loop) {
    KlIouState *st = loop->_backend;
    if (!st) return;
    io_uring_queue_exit(&st->ring);        /* kernel drops in-flight ops; free our records */
    KlIouOp *op = st->ops;
    while (op) { KlIouOp *n = op->next; iou_op_free(op); op = n; }
    KlIouWatch *w = st->watches;
    while (w) { KlIouWatch *n = w->next; kl_free(st->alloc, w, sizeof(*w)); w = n; }
    kl_free(st->alloc, st, sizeof(*st));
    loop->_backend = NULL;
    loop->fd = -1;
}

/* A completion loop over native fds — COMPLETION makes the Phase 7 negotiation require an
 * OVERLAPPED provider (kl_socket_provider_iouringcomp). This is the distinction from
 * event_iouring.c, which advertises READINESS. */
unsigned kl_event_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD;
}

/* The overlapped socket provider: reuse the POSIX control-plane ops (close, the direct
 * send comp_tls_flush uses, tcp_nodelay, …) and add the OVERLAPPED capability the
 * negotiation keys on — so completion_driver.c's kl_sock_* calls behave normally while the
 * loop advertises completion. io_uring uses real fds + the POSIX control plane. */
const KlSocketProvider *kl_socket_provider_iouringcomp(void) {
    static KlSocketProvider prov;
    const KlSocketProvider *posix = kl_socket_provider_posix();
    prov.ops = posix->ops;
    prov.context = posix->context;
    prov.capabilities = posix->capabilities | KL_SOCK_CAP_OVERLAPPED;
    return &prov;
}

/* ── completion.h implementation (io_uring SQE/CQE mechanics) ─────────── */

static void iou_op_push(KlIouState *st, KlIouOp *op) {
    op->next = st->ops;
    st->ops = op;
}

static void iou_op_unlink(KlIouState *st, KlIouOp *op) {
    for (KlIouOp **link = &st->ops; *link; link = &(*link)->next)
        if (*link == op) { *link = op->next; return; }
}

static KlIouState *iou_state(KlConn *c) { return c->ctx->loop._backend; }

int kl_comp_post_recv(KlConn *c) {
    KlIouState *st = iou_state(c);
    KlIouOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->alloc = c->alloc;
    op->conn = c;
    op->fd = c->fd;

    if (c->tls) {
        /* TLS: recv ciphertext into a transient buffer; read_buf holds plaintext.
         * kl_comp_drain feeds this to the engine (feed_input) before completing. */
        op->type = IOU_TLS_RECV;
        op->sendbuf = kl_malloc(c->alloc, KL_IOU_CIPHER_SIZE);
        if (!op->sendbuf) { iou_op_free(op); return -1; }
        op->sendcap = KL_IOU_CIPHER_SIZE;
        op->buf = op->sendbuf;
        op->buflen = KL_IOU_CIPHER_SIZE;
    } else {
        size_t space = c->read_cap - c->read_len;
        if (space == 0) { iou_op_free(op); return -1; }   /* headers overflowed */
        op->type = IOU_READ;
        op->buf = c->read_buf + c->read_len;
        op->buflen = space;
    }

    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { iou_op_free(op); return -1; }
    io_uring_prep_recv(sqe, op->fd, op->buf, op->buflen, 0);
    io_uring_sqe_set_data(sqe, op);
    iou_op_push(st, op);
    return 0;
}

/* Prepare a send SQE for the unsent tail of a WRITE op. */
static void iou_prep_send_tail(KlIouState *st, KlIouOp *op) {
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { op->aborted = 1; return; }   /* no slot — surface as error next drain */
    io_uring_prep_send(sqe, op->fd, op->sendbuf + op->send_done,
                       op->send_total - op->send_done, 0);
    io_uring_sqe_set_data(sqe, op);
}

int kl_comp_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    KlIouState *st = iou_state(c);
    KlIouOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = IOU_WRITE;
    op->alloc = c->alloc;
    op->conn = c;
    op->fd = c->fd;
    op->send_total = total;
    op->sendcap = total ? total : 1;
    op->sendbuf = kl_malloc(c->alloc, op->sendcap);
    if (!op->sendbuf) { op->sendcap = 0; iou_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        memcpy(op->sendbuf + off, iov[i].base, iov[i].len);
        off += iov[i].len;
    }
    iou_op_push(st, op);
    iou_prep_send_tail(st, op);
    return 0;
}

/* Post a response-head + file-body send. Phase-8f-1: the file bytes are pread into the
 * send buffer after the head, so this becomes a plain WRITE (partial sends handled by the
 * drain re-prep). Zero-copy splice is the 8f-2 refinement — the response owns file_fd and
 * closes it (response.c), so pread here is ownership-neutral. */
int kl_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    KlIouState *st = iou_state(c);
    if (count > (uint64_t)(SIZE_MAX / 2) || head_total > SIZE_MAX / 2 ||
        head_total + (size_t)count > SIZE_MAX / 2)
        return -1;                                   /* overflow guard */
    KlIouOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = IOU_WRITE;
    op->alloc = c->alloc;
    op->conn = c;
    op->fd = c->fd;
    op->send_total = head_total + (size_t)count;
    op->sendcap = op->send_total ? op->send_total : 1;
    op->sendbuf = kl_malloc(c->alloc, op->sendcap);
    if (!op->sendbuf) { op->sendcap = 0; iou_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < head_n; i++) {
        memcpy(op->sendbuf + off, head_iov[i].base, head_iov[i].len);
        off += head_iov[i].len;
    }
    /* Read the whole file body into the buffer (offset 0). */
    size_t got = 0;
    while (got < (size_t)count) {
        ssize_t nr = kl_plat_file_pread(file_fd, op->sendbuf + head_total + got,
                                        (size_t)count - got, (long long)got);
        if (nr < 0) { if (errno == EINTR) continue; iou_op_free(op); return -1; }
        if (nr == 0) break;                          /* short file — send what we have */
        got += (size_t)nr;
    }
    op->send_total = head_total + got;
    iou_op_push(st, op);
    iou_prep_send_tail(st, op);
    return 0;
}

int kl_comp_post_accept(struct KlServer *s) {
    KlIouState *st = s->ev.loop._backend;
    if (st->accept_pending) return 0;                /* one accept outstanding is enough */
    KlIouOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = IOU_ACCEPT;
    op->alloc = st->alloc;
    op->fd = st->listen_fd;
    op->peer_len = sizeof(op->peer);
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { iou_op_free(op); return -1; }
    io_uring_prep_accept(sqe, op->fd, (struct sockaddr *)&op->peer, &op->peer_len, 0);
    io_uring_sqe_set_data(sqe, op);
    iou_op_push(st, op);
    st->accept_pending = 1;
    return 0;
}

int kl_comp_post_udp_recv(struct KlUdp *udp) {
    KlIouState *st = udp->ctx->loop._backend;
    KlIouOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = IOU_UDP_RECV;
    op->alloc = st->alloc;
    op->udp = udp;
    op->fd = udp->fd;
    op->peer_len = sizeof(op->peer);
    op->msgiov.iov_base = udp->recv_buf;
    op->msgiov.iov_len = udp->recv_buf_size;
    op->msgh.msg_name = &op->peer;
    op->msgh.msg_namelen = sizeof(op->peer);
    op->msgh.msg_iov = &op->msgiov;
    op->msgh.msg_iovlen = 1;
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { iou_op_free(op); return -1; }
    io_uring_prep_recvmsg(sqe, op->fd, &op->msgh, 0);
    io_uring_sqe_set_data(sqe, op);
    iou_op_push(st, op);
    return 0;
}

int kl_comp_post_udp_send(struct KlUdp *udp, const void *data, size_t len,
                          const struct sockaddr *dest, int dest_len) {
    KlIouState *st = udp->ctx->loop._backend;
    KlIouOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = IOU_UDP_SEND;
    op->alloc = st->alloc;
    op->udp = udp;
    op->fd = udp->fd;
    op->send_total = len;
    op->sendcap = len ? len : 1;
    op->sendbuf = kl_malloc(st->alloc, op->sendcap);
    if (!op->sendbuf) { op->sendcap = 0; iou_op_free(op); return -1; }
    memcpy(op->sendbuf, data, len);
    if (dest_len > 0 && (size_t)dest_len <= sizeof(op->peer)) {
        memcpy(&op->peer, dest, (size_t)dest_len);
        op->peer_len = (socklen_t)dest_len;
        op->msgh.msg_name = &op->peer;
        op->msgh.msg_namelen = op->peer_len;
    }
    op->msgiov.iov_base = op->sendbuf;
    op->msgiov.iov_len = len;
    op->msgh.msg_iov = &op->msgiov;
    op->msgh.msg_iovlen = 1;
    struct io_uring_sqe *sqe = iou_sqe(st);
    if (!sqe) { iou_op_free(op); return -1; }
    io_uring_prep_sendmsg(sqe, op->fd, &op->msgh, 0);
    io_uring_sqe_set_data(sqe, op);
    iou_op_push(st, op);
    return 0;
}

int kl_comp_prime_accepts(struct KlServer *s) {
    KlIouState *st = s->ev.loop._backend;
    if (st->primed) return 0;
    st->listen_fd = s->listen_fd;
    st->primed = 1;
    return kl_comp_post_accept(s);
}

/* Cancel pending ops on `fd` (idle-timeout sweep): mark aborted + prep_cancel so the
 * in-flight op completes promptly (–ECANCELED) and the driver releases the connection
 * through its normal completion path. Marking (not freeing) preserves the invariant that
 * a conn is released only from a completion — the op the driver waits on still completes,
 * and the kernel is done with its buffer before we free it. */
void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    KlIouState *st = ctx->loop._backend;
    for (KlIouOp *o = st->ops; o; o = o->next)
        if (o->fd == fd && !o->aborted) {
            o->aborted = 1;
            struct io_uring_sqe *sqe = iou_sqe(st);
            if (sqe) {
                io_uring_prep_cancel(sqe, o, 0);
                io_uring_sqe_set_data(sqe, NULL);   /* sentinel — ignore the cancel CQE */
            }
        }
}

/* ── drain: submit the batch, reap CQEs, complete/re-prep ─────────────── */

/* Fill a completion event from a finished op + its cqe result. Returns 1 if `ev` was
 * written (op done → remove/free), 0 if the op made partial progress and was re-prepared
 * (keep it in-flight, no event). */
static int iou_complete(KlIouState *st, KlIouOp *op, int res, KlCompletionEvent *ev) {
    memset(ev, 0, sizeof(*ev));
    switch (op->type) {
    case IOU_ACCEPT:
        st->accept_pending = 0;
        ev->kind = KL_COMP_ACCEPT;
        if (op->aborted || res < 0) { ev->ok = 0; return 1; }   /* driver just refills */
        ev->ok = 1;
        ev->accepted_fd = res;
        if (op->peer_len > 0 && (size_t)op->peer_len <= sizeof(ev->peer)) {
            memcpy(&ev->peer, &op->peer, (size_t)op->peer_len);
            ev->peer_len = op->peer_len;
        }
        return 1;

    case IOU_READ:
    case IOU_TLS_RECV:
        ev->kind = KL_COMP_READ;
        ev->target = op->conn;
        if (op->aborted || res <= 0) { ev->ok = 0; return 1; }  /* EOF/error/abort → close */
        ev->ok = 1;
        ev->bytes = (size_t)res;
        if (op->type == IOU_TLS_RECV && op->conn->tls->feed_input)
            op->conn->tls->feed_input(op->conn->tls, op->buf, (size_t)res);
        return 1;

    case IOU_WRITE:
        if (op->aborted || res < 0) {
            ev->kind = KL_COMP_WRITE; ev->target = op->conn; ev->ok = 0;
            return 1;
        }
        op->send_done += (size_t)res;
        if (op->send_done < op->send_total) {         /* short write — send the tail */
            iou_prep_send_tail(st, op);
            return 0;                                 /* still in flight, no event */
        }
        ev->kind = KL_COMP_WRITE; ev->target = op->conn; ev->ok = 1;
        ev->bytes = op->send_total;
        return 1;

    case IOU_UDP_RECV:
        ev->kind = KL_COMP_UDP_RECV;
        ev->target = op->udp;
        ev->ok = (res >= 0);
        ev->bytes = (res > 0) ? (size_t)res : 0;
        ev->buf = op->udp->recv_buf;
        if (res >= 0 && op->msgh.msg_namelen > 0 &&
            (size_t)op->msgh.msg_namelen <= sizeof(ev->peer)) {
            memcpy(&ev->peer, &op->peer, (size_t)op->msgh.msg_namelen);
            ev->peer_len = op->msgh.msg_namelen;
        }
        return 1;

    case IOU_UDP_SEND:
        ev->kind = KL_COMP_UDP_SEND;
        ev->target = op->udp;
        ev->ok = (res >= 0);
        ev->bytes = (res > 0) ? (size_t)res : 0;
        return 1;

    case IOU_WATCH:                                   /* handled in the drain loop */
        return 0;
    }
    return 0;
}

int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    KlIouState *st = ctx->loop._backend;

    struct __kernel_timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL;
        tsp = &ts;
    }

    /* Submit the queued SQEs and wait for at least one completion (or the timeout). */
    struct io_uring_cqe *cqe = NULL;
    int r = io_uring_submit_and_wait_timeout(&st->ring, &cqe, 1, tsp, NULL);
    if (r < 0 && r != -ETIME && r != -EINTR)
        return -1;

    int count = 0;
    unsigned seen = 0, head;
    io_uring_for_each_cqe(&st->ring, head, cqe) {
        if (count >= max) break;                      /* leave the rest for the next drain */
        uint64_t ud = io_uring_cqe_get_data64(cqe);
        int res = cqe->res;
        seen++;
        /* Skip sentinels: our cancel / poll_remove CQEs (user_data 0), and the internal
         * IORING_OP_TIMEOUT that submit_and_wait_timeout injects (LIBURING_UDATA_TIMEOUT
         * == (uint64_t)-1) — dereferencing either as an op would crash. */
        if (ud == 0 || ud == (uint64_t)-1) continue;
        void *data = (void *)(uintptr_t)ud;

        if (*(IouOpType *)data == IOU_WATCH) {
            KlIouWatch *w = data;
            w->armed = 0;                             /* this single-shot poll is consumed */
            if (w->removed) { kl_free(st->alloc, w, sizeof(*w)); continue; }  /* del completed */
            if (res > 0) {                            /* poll fired — surface a watcher event */
                KlEventMask ready = 0;
                if (res & (POLLIN | POLLHUP | POLLERR)) ready |= KL_EVENT_READ;
                if (res & POLLOUT) ready |= KL_EVENT_WRITE;
                memset(&out[count], 0, sizeof(out[count]));
                out[count].kind = KL_COMP_WATCHER;
                out[count].target = w->udata;         /* the tagged KlWatcher */
                out[count].bytes = (size_t)ready;      /* carry the ready mask */
                out[count].ok = 1;
                count++;
            }
            /* No re-arm here: kl_event_dispatch → kl_watcher_rearm → kl_event_mod re-arms
             * the single-shot poll after the callback runs (the one-shot-backend contract). */
            continue;
        }

        KlIouOp *op = data;
        if (iou_complete(st, op, res, &out[count])) {  /* op finished → emit + free */
            count++;
            iou_op_unlink(st, op);
            iou_op_free(op);
        }
        /* else: partial write re-prepared — op stays in-flight, no event */
    }
    io_uring_cq_advance(&st->ring, seen);
    return count;
}
