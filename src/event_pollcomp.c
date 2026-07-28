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
 * facade over readiness — the mirror image of event_iouring.c adapting completion to
 * the readiness interface.
 */
#include <keel/event.h>
#include <keel/server.h>
#include <keel/connection.h>
#include <keel/udp.h>            /* KlUdp — datagram recv/send over completion */
#include <keel/tls.h>            /* KlTls feed_input — deliver received ciphertext */
#include "event_caps.h"
#include "socket.h"              /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED + seam */
#include "completion.h"          /* the abstract axis this TU implements */

#include <poll.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

#define KL_PC_CIPHER_SIZE (17u * 1024u)   /* one TLS record + slack (mirrors IOCP) */

typedef enum {
    PC_ACCEPT, PC_READ, PC_TLS_RECV, PC_WRITE, PC_SENDFILE, PC_UDP_RECV, PC_UDP_SEND
} PcOpType;

/* One pending async op — polled, then completed by the single syscall it names. */
typedef struct KlPcOp {
    struct KlPcOp *next;
    PcOpType       type;
    KlAllocator   *alloc;
    KlSocketHandle fd;                    /* the descriptor this op polls */
    KlConn        *conn;                  /* READ / TLS_RECV / WRITE / SENDFILE */
    KlUdp         *udp;                   /* UDP_RECV / UDP_SEND */
    void          *buf;                   /* READ: capture of read_buf+read_len */
    size_t         buflen;                /* READ: space captured at post time */
    char          *sendbuf;              /* WRITE/SENDFILE head/UDP_SEND: owned copy */
    size_t         send_total, send_done; /* partial-send tracking */
    int            file_fd;               /* SENDFILE */
    uint64_t       file_count, file_off;  /* SENDFILE: file bytes + progress */
    struct sockaddr_storage dest;         /* UDP_SEND destination */
    socklen_t      dest_len;
    int            aborted;               /* cancelled (idle timeout) — deliver as error */
} KlPcOp;

typedef struct {
    KlAllocator   *alloc;
    KlPcOp        *ops;                    /* singly-linked pending-op list */
    struct KlServer *server;              /* accept target (set at prime) */
    KlSocketHandle  listen_fd;
    int             primed;
} KlPcState;

/* ── KlEventLoop lifecycle ───────────────────────────────────────────── */

int kl_event_init(KlEventLoop *loop) {
    KlPcState *st = kl_malloc(loop->alloc, sizeof(*st));
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = loop->alloc;
    st->listen_fd = KL_INVALID_SOCKET;
    loop->_backend = st;
    loop->fd = -1;
    return 0;
}

/* Completion model: no readiness arming — I/O is posted, not watched. The fd is used
 * directly at drain time, so add/mod/del are inert (as on IOCP). */
int kl_event_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop; (void)fd; (void)mask; (void)udata;
    return 0;
}
int kl_event_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop; (void)fd; (void)mask; (void)udata;
    return 0;
}
int kl_event_del(KlEventLoop *loop, KlSocketHandle fd) {
    (void)loop; (void)fd;
    return 0;
}

int kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* The server drives a completion loop via kl_comp_drain (io_engine seam), not this
     * readiness call. Defined because the KlEventLoop API requires it. */
    (void)loop; (void)out; (void)max;
    if (timeout_ms > 0) poll(NULL, 0, timeout_ms);
    return 0;
}

void kl_event_close(KlEventLoop *loop) {
    KlPcState *st = loop->_backend;
    if (!st) return;
    KlPcOp *op = st->ops;
    while (op) {
        KlPcOp *next = op->next;
        if (op->sendbuf) kl_free(op->alloc, op->sendbuf, op->send_total ? op->send_total
                                                                        : KL_PC_CIPHER_SIZE);
        kl_free(op->alloc, op, sizeof(*op));
        op = next;
    }
    kl_free(st->alloc, st, sizeof(*st));
    loop->_backend = NULL;
    loop->fd = -1;
}

/* A completion loop over native fds. COMPLETION makes the Phase 7 negotiation require
 * an OVERLAPPED provider (kl_socket_provider_pollcomp, below). */
unsigned kl_event_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD;
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
    return &prov;
}

/* ── completion.h implementation (poll-driven mechanics) ─────────────── */

static void pc_op_push(KlPcState *st, KlPcOp *op) {
    op->next = st->ops;
    st->ops = op;
}

static void pc_op_free(KlPcOp *op) {
    if (op->sendbuf)
        kl_free(op->alloc, op->sendbuf,
                op->send_total ? op->send_total : KL_PC_CIPHER_SIZE);
    kl_free(op->alloc, op, sizeof(*op));
}

static KlPcState *pc_state(KlConn *c) { return c->ctx->loop._backend; }

int kl_comp_post_recv(KlConn *c) {
    KlPcState *st = pc_state(c);
    KlPcOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->alloc = c->alloc;
    op->conn = c;
    op->fd = c->fd;

    if (c->tls) {
        /* TLS: read ciphertext into a transient buffer; read_buf holds plaintext.
         * kl_comp_drain feeds this to the engine (feed_input) before completing. */
        op->type = PC_TLS_RECV;
        op->send_total = KL_PC_CIPHER_SIZE;   /* reuse send_total as the free size */
        op->sendbuf = kl_malloc(c->alloc, KL_PC_CIPHER_SIZE);
        if (!op->sendbuf) { op->send_total = 0; pc_op_free(op); return -1; }
        op->buf = op->sendbuf;
        op->buflen = KL_PC_CIPHER_SIZE;
    } else {
        size_t space = c->read_cap - c->read_len;
        if (space == 0) { pc_op_free(op); return -1; }   /* headers overflowed */
        op->type = PC_READ;
        op->buf = c->read_buf + c->read_len;
        op->buflen = space;
    }
    pc_op_push(st, op);
    return 0;
}

int kl_comp_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    KlPcState *st = pc_state(c);
    KlPcOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = PC_WRITE;
    op->alloc = c->alloc;
    op->conn = c;
    op->fd = c->fd;
    op->send_total = total;
    op->sendbuf = kl_malloc(c->alloc, total ? total : 1);
    if (!op->sendbuf) { op->send_total = 0; pc_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        memcpy(op->sendbuf + off, iov[i].base, iov[i].len);
        off += iov[i].len;
    }
    pc_op_push(st, op);
    return 0;
}

int kl_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    KlPcState *st = pc_state(c);
    KlPcOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = PC_SENDFILE;
    op->alloc = c->alloc;
    op->conn = c;
    op->fd = c->fd;
    op->file_fd = file_fd;
    op->file_count = count;
    op->send_total = head_total;
    op->sendbuf = kl_malloc(c->alloc, head_total ? head_total : 1);
    if (!op->sendbuf) { op->send_total = 0; pc_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < head_n; i++) {
        memcpy(op->sendbuf + off, head_iov[i].base, head_iov[i].len);
        off += head_iov[i].len;
    }
    pc_op_push(st, op);
    return 0;
}

int kl_comp_post_accept(struct KlServer *s) {
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

int kl_comp_post_udp_recv(struct KlUdp *udp) {
    KlPcState *st = udp->ctx->loop._backend;
    KlPcOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = PC_UDP_RECV;
    op->alloc = st->alloc;
    op->udp = udp;
    op->fd = udp->fd;
    pc_op_push(st, op);
    return 0;
}

int kl_comp_post_udp_send(struct KlUdp *udp, const void *data, size_t len,
                          const struct sockaddr *dest, int dest_len) {
    KlPcState *st = udp->ctx->loop._backend;
    KlPcOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = PC_UDP_SEND;
    op->alloc = st->alloc;
    op->udp = udp;
    op->fd = udp->fd;
    op->send_total = len;
    op->sendbuf = kl_malloc(st->alloc, len ? len : 1);
    if (!op->sendbuf) { op->send_total = 0; pc_op_free(op); return -1; }
    memcpy(op->sendbuf, data, len);
    if (dest_len > 0 && (size_t)dest_len <= sizeof(op->dest)) {
        memcpy(&op->dest, dest, (size_t)dest_len);
        op->dest_len = (socklen_t)dest_len;
    }
    pc_op_push(st, op);
    return 0;
}

int kl_comp_prime_accepts(struct KlServer *s) {
    KlPcState *st = s->ev.loop._backend;
    if (st->primed) return 0;
    st->listen_fd = s->listen_fd;
    st->server = s;
    st->primed = 1;
    return kl_comp_post_accept(s);
}

/* Cancel pending ops on `fd` (idle-timeout sweep): mark them aborted so the next drain
 * delivers an error completion, driving the driver's normal release. We mark rather than
 * remove so the invariant "a conn is released only from a completion" holds — the driver
 * expects the op it's waiting on to complete before the conn slot is reused. */
void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    KlPcState *st = ctx->loop._backend;
    for (KlPcOp *o = st->ops; o; o = o->next)
        if (o->fd == fd) o->aborted = 1;
}

/* ── drain: poll the pending ops, complete the ready ones ─────────────── */

/* Fill an error completion for a cancelled op. Returns 1 if an event was written
 * (a connection/UDP target exists), 0 for an accept op (no target — just drop it). */
static int pc_emit_abort(const KlPcOp *op, KlCompletionEvent *ev) {
    memset(ev, 0, sizeof(*ev));
    ev->ok = 0;
    switch (op->type) {
    case PC_READ: case PC_TLS_RECV:  ev->kind = KL_COMP_READ;  ev->target = op->conn; return 1;
    case PC_WRITE: case PC_SENDFILE: ev->kind = KL_COMP_WRITE; ev->target = op->conn; return 1;
    case PC_UDP_RECV:                ev->kind = KL_COMP_UDP_RECV; ev->target = op->udp; return 1;
    case PC_UDP_SEND:                ev->kind = KL_COMP_UDP_SEND; ev->target = op->udp; return 1;
    case PC_ACCEPT:                  return 0;
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
        ev->ok = 1;
        ev->accepted_fd = a;
        if (slen > 0 && (size_t)slen <= sizeof(ev->peer)) {
            memcpy(&ev->peer, &ss, (size_t)slen);
            ev->peer_len = slen;
        }
        return 1;
    }
    case PC_READ:
    case PC_TLS_RECV: {
        ssize_t n = recv(op->fd, op->buf, op->buflen, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        ev->kind = KL_COMP_READ;
        ev->target = op->conn;
        ev->ok = 1;
        ev->bytes = (n > 0) ? (size_t)n : 0;   /* 0/-1 → peer closed; driver closes */
        if (op->type == PC_TLS_RECV && n > 0 && op->conn->tls->feed_input)
            op->conn->tls->feed_input(op->conn->tls, op->buf, (size_t)n);
        return 1;
    }
    case PC_WRITE: {
        while (op->send_done < op->send_total) {
            ssize_t n = send(op->fd, op->sendbuf + op->send_done,
                             op->send_total - op->send_done, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  /* re-poll */
                ev->kind = KL_COMP_WRITE; ev->target = op->conn; ev->ok = 0;
                return -1;
            }
            op->send_done += (size_t)n;
        }
        ev->kind = KL_COMP_WRITE; ev->target = op->conn; ev->ok = 1;
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
                ev->kind = KL_COMP_WRITE; ev->target = op->conn; ev->ok = 0;
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
                ev->kind = KL_COMP_WRITE; ev->target = op->conn; ev->ok = 0;
                return -1;
            }
            if (n == 0) break;
            op->file_off = off;
        }
        ev->kind = KL_COMP_WRITE; ev->target = op->conn; ev->ok = 1;
        ev->bytes = op->send_total + op->file_count;
        return 1;
    }
    case PC_UDP_RECV: {
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        ssize_t n = recvfrom(op->fd, op->udp->recv_buf, op->udp->recv_buf_size, 0,
                             (struct sockaddr *)&ss, &slen);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        ev->kind = KL_COMP_UDP_RECV;
        ev->target = op->udp;
        ev->ok = (n >= 0);
        ev->bytes = (n > 0) ? (size_t)n : 0;
        ev->buf = op->udp->recv_buf;
        if (n >= 0 && slen > 0 && (size_t)slen <= sizeof(ev->peer)) {
            memcpy(&ev->peer, &ss, (size_t)slen);
            ev->peer_len = slen;
        }
        return 1;
    }
    case PC_UDP_SEND: {
        ssize_t n = sendto(op->fd, op->sendbuf, op->send_total, 0,
                           op->dest_len ? (struct sockaddr *)&op->dest : NULL,
                           op->dest_len);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        ev->kind = KL_COMP_UDP_SEND;
        ev->target = op->udp;
        ev->ok = (n >= 0);
        ev->bytes = (n > 0) ? (size_t)n : 0;
        return 1;
    }
    }
    return 0;
}

static short pc_poll_events(PcOpType t) {
    switch (t) {
    case PC_ACCEPT: case PC_READ: case PC_TLS_RECV: case PC_UDP_RECV: return POLLIN;
    default: return POLLOUT;   /* PC_WRITE / PC_SENDFILE / PC_UDP_SEND */
    }
}

int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
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

    /* Count pending ops, then build a parallel pollfd array. */
    size_t nops = 0;
    for (KlPcOp *o = st->ops; o; o = o->next) nops++;
    if (nops == 0) {
        if (count == 0 && timeout_ms != 0) poll(NULL, 0, timeout_ms);
        return count;
    }

    struct pollfd *pfds = kl_malloc(st->alloc, nops * sizeof(*pfds));
    if (!pfds) return -1;
    KlPcOp **oref = kl_malloc(st->alloc, nops * sizeof(*oref));
    if (!oref) { kl_free(st->alloc, pfds, nops * sizeof(*pfds)); return -1; }

    size_t i = 0;
    for (KlPcOp *o = st->ops; o; o = o->next, i++) {
        pfds[i].fd = o->fd;
        pfds[i].events = pc_poll_events(o->type);
        pfds[i].revents = 0;
        oref[i] = o;
    }

    int pr = poll(pfds, (nfds_t)nops, timeout_ms);
    if (pr > 0) {
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
    }

    kl_free(st->alloc, pfds, nops * sizeof(*pfds));
    kl_free(st->alloc, oref, nops * sizeof(*oref));
    return count;
}
