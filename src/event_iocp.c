/*
 * event_iocp.c — Windows IOCP event backend + completion connection driver
 * (PAL Phase 8, completion axis). Compiled ONLY on Windows under `BACKEND=iocp`
 * (Makefile selection — no #ifdef in shared code), so the completion model never
 * touches the POSIX/readiness TUs or Keel's public API. See docs/phase8_iocp_design.md.
 *
 * The KlEventLoop lifecycle wraps an IOCP port; the driver (kl_io_engine_run_completion,
 * the io_engine seam) posts overlapped AcceptEx/WSARecv/WSASend and, on each
 * completion, drives the connection through the SAME model-blind protocol core the
 * readiness path uses (conn_internal.h) — connection.c never learns the event model.
 *
 * Scope (8a): plaintext TCP, no-request-body (GET/HEAD) HTTP over IOCP —
 * accept → read headers → dispatch → buffered response → keep-alive/close. Request
 * bodies, TLS, UDP and streaming/file responses over IOCP are later increments; a
 * connection that needs a request body is closed rather than mis-served.
 */
#include <keel/event.h>
#include <keel/server.h>
#include <keel/connection.h>
#include "event_caps.h"
#include "io_engine.h"
#include "socket.h"              /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED + kl_sock_* */
#include "internal.h"            /* kl_server_conn_release */
#include "conn_internal.h"       /* kl_conn_dispatch_request / kl_conn_send_complete */
#include "response_internal.h"   /* kl_response_build_iovec */

#include "sockcompat.h"          /* winsock2.h */
#include <windows.h>             /* IOCP */
#include <mswsock.h>             /* AcceptEx / GetAcceptExSockaddrs / SO_UPDATE_ACCEPT_CONTEXT */
#include <string.h>

#define KL_IOCP_MAX_EVENTS     64
#define KL_IOCP_ACCEPT_BACKLOG 8
#define KL_IOCP_ADDR_LEN       (sizeof(struct sockaddr_storage) + 16)

/* Per-loop backend state: the port + the lazily-loaded extension fn pointers +
 * a "started" latch (accepts posted on first tick). Stashed in loop->_backend. */
typedef struct {
    HANDLE                  port;
    LPFN_ACCEPTEX           acceptex;
    LPFN_GETACCEPTEXSOCKADDRS get_sockaddrs;
    int                     started;
    int                     accept_family;   /* AF_INET / AF_INET6 of the listen sock */
    KlAllocator            *alloc;
} KlIocpState;

typedef enum { KL_IOCP_ACCEPT, KL_IOCP_READ, KL_IOCP_WRITE } KlIocpOpType;

/* One in-flight overlapped operation. `ov` MUST be first so a completion's
 * lpOverlapped round-trips back via CONTAINING_RECORD. */
typedef struct {
    OVERLAPPED    ov;
    KlIocpOpType  type;
    KlAllocator  *alloc;
    KlConn       *conn;                        /* READ / WRITE */
    SOCKET        accept_sock;                 /* ACCEPT */
    char          accept_buf[2 * KL_IOCP_ADDR_LEN];  /* ACCEPT: local+remote addr */
    char         *sendbuf;                     /* WRITE: contiguous response copy */
    size_t        send_total, send_done;       /* WRITE: partial-send tracking */
} KlIocpOp;

/* ── KlEventLoop lifecycle over an IOCP port ─────────────────────────── */

int kl_event_init(KlEventLoop *loop) {
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (port == NULL)
        return -1;
    KlIocpState *st = kl_malloc(loop->alloc, sizeof(*st));
    if (!st) { CloseHandle(port); return -1; }
    memset(st, 0, sizeof(*st));
    st->port = port;
    st->alloc = loop->alloc;
    loop->_backend = st;
    loop->fd = -1;
    return 0;
}

int kl_event_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)mask;   /* completion model: no readiness mask — I/O is posted, not armed */
    if (!kl_handle_valid(fd))
        return -1;
    KlIocpState *st = loop->_backend;
    /* Associate the socket with the port; the completion key carries udata (the
     * KlConn*, so a completion maps straight back to its connection). */
    HANDLE h = CreateIoCompletionPort((HANDLE)(uintptr_t)fd, st->port,
                                      (ULONG_PTR)udata, 0);
    return h ? 0 : -1;
}

int kl_event_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    /* No readiness re-arm under completion: the driver re-posts overlapped ops to
     * express interest. A no-op keeps the shared server transition code composing. */
    (void)loop; (void)fd; (void)mask; (void)udata;
    return 0;
}

int kl_event_del(KlEventLoop *loop, KlSocketHandle fd) {
    /* No IOCP de-association API; a socket leaves the port when closed. No-op so
     * shared teardown composes. */
    (void)loop; (void)fd;
    return 0;
}

int kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* A completion loop is driven by kl_io_engine_run_completion (io_engine seam),
     * not this readiness call. Defined because the KlEventLoop API requires it;
     * reports no readiness events. */
    (void)loop; (void)out; (void)max;
    if (timeout_ms > 0)
        Sleep((DWORD)timeout_ms);
    return 0;
}

void kl_event_close(KlEventLoop *loop) {
    KlIocpState *st = loop->_backend;
    if (st) {
        if (st->port) CloseHandle(st->port);
        kl_free(st->alloc, st, sizeof(*st));
        loop->_backend = NULL;
    }
    loop->fd = -1;
}

/* PAL Phase 8: a completion loop over native OS handles (SOCKETs on the port).
 * Advertising COMPLETION makes the Phase 7 negotiation require an OVERLAPPED
 * provider (kl_caps_compatible). */
unsigned kl_event_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD;
}

/* The overlapped socket provider (Phase 8). Control-plane ops fall through to the
 * Winsock kl_sockdef_* defaults (NULL ops); the data plane is driven by the
 * completion loop's overlapped submit path. OVERLAPPED is what negotiation keys on. */
static const KlSocketOps IOCP_OPS = { .name = "iocp" };
static const KlSocketProvider IOCP_PROVIDER = {
    &IOCP_OPS, NULL, KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_OVERLAPPED,
};
const KlSocketProvider *kl_socket_provider_iocp(void) { return &IOCP_PROVIDER; }

/* ── Completion connection driver ────────────────────────────────────── */

static void iocp_op_free(KlIocpOp *op) {
    if (op->sendbuf) kl_free(op->alloc, op->sendbuf, op->send_total);
    kl_free(op->alloc, op, sizeof(*op));
}

/* Post a WSARecv into the connection's read buffer (headers phase). */
static int iocp_post_recv(KlConn *c) {
    size_t space = c->read_cap - c->read_len;
    if (space == 0) {   /* headers overflowed the buffer — give up on this conn */
        return -1;
    }
    KlIocpOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_READ;
    op->alloc = c->alloc;
    op->conn = c;

    WSABUF buf = { (ULONG)space, c->read_buf + c->read_len };
    DWORD flags = 0, received = 0;
    int rc = WSARecv((SOCKET)c->fd, &buf, 1, &received, &flags, &op->ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

/* Serialize the (buffered) response into a contiguous buffer and post WSASend. */
static int iocp_post_send(KlConn *c) {
    char cl_buf[48];
    KlIoVec iov[7];
    size_t total = 0;
    int iovcnt = kl_response_build_iovec(&c->res, iov, 7, cl_buf, sizeof(cl_buf), &total);
    if (iovcnt < 0) return -1;

    KlIocpOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_WRITE;
    op->alloc = c->alloc;
    op->conn = c;
    op->send_total = total;
    op->send_done = 0;

    op->sendbuf = kl_malloc(c->alloc, total ? total : 1);
    if (!op->sendbuf) { op->send_total = 0; iocp_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        memcpy(op->sendbuf + off, iov[i].base, iov[i].len);
        off += iov[i].len;
    }

    WSABUF buf = { (ULONG)total, op->sendbuf };
    DWORD sent = 0;
    int rc = WSASend((SOCKET)c->fd, &buf, 1, &sent, 0, &op->ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

/* Pre-post one AcceptEx: create the accept socket, arm it on the listen socket. */
static int iocp_post_accept(struct KlServer *s, KlIocpState *st) {
    SOCKET a = WSASocketW(st->accept_family, SOCK_STREAM, IPPROTO_TCP,
                          NULL, 0, WSA_FLAG_OVERLAPPED);
    if (a == INVALID_SOCKET) return -1;

    KlIocpOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) { closesocket(a); return -1; }
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_ACCEPT;
    op->alloc = st->alloc;
    op->accept_sock = a;

    DWORD bytes = 0;
    BOOL ok = st->acceptex((SOCKET)s->listen_fd, a, op->accept_buf,
                           0, (DWORD)KL_IOCP_ADDR_LEN, (DWORD)KL_IOCP_ADDR_LEN,
                           &bytes, &op->ov);
    if (!ok && WSAGetLastError() != WSA_IO_PENDING) {
        closesocket(a);
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

static void iocp_close_conn(struct KlServer *s, KlConn *c) {
    kl_server_conn_release(s, c);   /* closes the socket + returns the pool slot */
}

/* Run the model-blind READING core on bytes already in read_buf, then act on the
 * resulting state (send / keep-reading / close). */
static void iocp_drive_reading(struct KlServer *s, KlConn *c) {
    size_t consumed = 0;
    KlParseResult pr = c->parser->parse(c->parser, &c->req,
                                        c->read_buf, c->read_len, &consumed);
    KlConnState st;
    if (pr == KL_PARSE_INCOMPLETE) {
        if (iocp_post_recv(c) < 0) iocp_close_conn(s, c);
        return;
    }
    if (pr == KL_PARSE_ERROR) { iocp_close_conn(s, c); return; }

    if (pr == KL_PARSE_HEADERS_OK)
        st = kl_conn_dispatch_request(c, &s->router,
                                      c->read_buf + consumed, c->read_len - consumed);
    else /* KL_PARSE_OK */
        st = kl_conn_dispatch_request(c, &s->router, NULL, 0);

    if (st == KL_CONN_SENDING) {
        if (iocp_post_send(c) < 0) iocp_close_conn(s, c);
    } else {
        /* READING_BODY (request bodies not yet driven over IOCP), CLOSED,
         * SUSPENDED, or anything else: close rather than mis-serve. */
        iocp_close_conn(s, c);
    }
}

static void iocp_on_accept(struct KlServer *s, KlIocpState *st, KlIocpOp *op) {
    SOCKET a = op->accept_sock;
    /* Inherit the listen socket's properties so the accepted socket works. */
    SOCKET lst = (SOCKET)s->listen_fd;
    setsockopt(a, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&lst, sizeof(lst));

    struct sockaddr *local = NULL, *remote = NULL;
    int llen = 0, rlen = 0;
    st->get_sockaddrs(op->accept_buf, 0, (DWORD)KL_IOCP_ADDR_LEN, (DWORD)KL_IOCP_ADDR_LEN,
                      &local, &llen, &remote, &rlen);

    KlConn *nc = kl_conn_acquire(&s->pool, (KlSocketHandle)a);
    if (!nc) { closesocket(a); goto refill; }   /* pool full — drop (backlog queues) */

    nc->peer_source = KL_PEER_SOCKET;
    if (rlen > 0 && (size_t)rlen <= sizeof(nc->peer_addr)) {
        memcpy(&nc->peer_addr, remote, (size_t)rlen);
        nc->peer_addr_len = (socklen_t)rlen;
    } else {
        nc->peer_addr_len = 0;
    }
    nc->res.alloc = st->alloc;
    (void)kl_sock_set_tcp_nodelay(nc->ctx ? nc->ctx->sockets : NULL, nc->fd, 1);

    if (kl_event_add(&s->ev.loop, nc->fd, KL_EVENT_READ, nc) < 0 ||
        iocp_post_recv(nc) < 0) {
        iocp_close_conn(s, nc);
    }

refill:
    iocp_op_free(op);
    (void)iocp_post_accept(s, st);   /* keep the accept backlog topped up */
}

static void iocp_on_read(struct KlServer *s, KlIocpOp *op, DWORD n) {
    KlConn *c = op->conn;
    iocp_op_free(op);
    if (n == 0) { iocp_close_conn(s, c); return; }   /* peer closed / error */
    c->read_len += (size_t)n;
    iocp_drive_reading(s, c);
}

static void iocp_on_write(struct KlServer *s, KlIocpOp *op, DWORD n) {
    KlConn *c = op->conn;
    op->send_done += (size_t)n;
    if (n > 0 && op->send_done < op->send_total) {
        /* Partial send — re-post the remainder from the same buffer. */
        WSABUF buf = { (ULONG)(op->send_total - op->send_done),
                       op->sendbuf + op->send_done };
        DWORD sent = 0;
        int rc = WSASend((SOCKET)c->fd, &buf, 1, &sent, 0, &op->ov, NULL);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            iocp_op_free(op);
            iocp_close_conn(s, c);
        }
        return;   /* op reused for the remainder */
    }
    iocp_op_free(op);
    if (n == 0) { iocp_close_conn(s, c); return; }

    KlConnState st = kl_conn_send_complete(c);   /* keep-alive reset or close */
    if (st == KL_CONN_READING) {
        if (iocp_post_recv(c) < 0) iocp_close_conn(s, c);
    } else {
        iocp_close_conn(s, c);
    }
}

/* First-tick setup: load the AcceptEx/GetAcceptExSockaddrs extension pointers off
 * the listen socket, learn its address family, and prime the accept backlog. */
static int iocp_start(struct KlServer *s, KlIocpState *st) {
    SOCKET lst = (SOCKET)s->listen_fd;
    GUID g_accept = WSAID_ACCEPTEX;
    GUID g_addrs  = WSAID_GETACCEPTEXSOCKADDRS;
    DWORD b = 0;
    if (WSAIoctl(lst, SIO_GET_EXTENSION_FUNCTION_POINTER, &g_accept, sizeof(g_accept),
                 &st->acceptex, sizeof(st->acceptex), &b, NULL, NULL) == SOCKET_ERROR)
        return -1;
    if (WSAIoctl(lst, SIO_GET_EXTENSION_FUNCTION_POINTER, &g_addrs, sizeof(g_addrs),
                 &st->get_sockaddrs, sizeof(st->get_sockaddrs), &b, NULL, NULL) == SOCKET_ERROR)
        return -1;

    struct sockaddr_storage sa;
    int salen = (int)sizeof(sa);
    st->accept_family = AF_INET;
    if (getsockname(lst, (struct sockaddr *)&sa, &salen) == 0)
        st->accept_family = sa.ss_family;

    for (int i = 0; i < KL_IOCP_ACCEPT_BACKLOG; i++) {
        if (iocp_post_accept(s, st) < 0)
            return (i == 0) ? -1 : 0;   /* at least one posted is enough to serve */
    }
    return 0;
}

int kl_io_engine_run_completion(struct KlServer *s, int timeout_ms) {
    KlIocpState *st = s->ev.loop._backend;
    if (!st->started) {
        if (iocp_start(s, st) < 0)
            return -1;
        st->started = 1;
    }

    OVERLAPPED_ENTRY entries[KL_IOCP_MAX_EVENTS];
    ULONG n = 0;
    BOOL ok = GetQueuedCompletionStatusEx(st->port, entries, KL_IOCP_MAX_EVENTS,
                                          &n, (DWORD)timeout_ms, FALSE);
    if (!ok)
        return 0;   /* WAIT_TIMEOUT (or transient) — nothing to dispatch this tick */

    for (ULONG i = 0; i < n; i++) {
        KlIocpOp *op = CONTAINING_RECORD(entries[i].lpOverlapped, KlIocpOp, ov);
        DWORD bytes = entries[i].dwNumberOfBytesTransferred;
        switch (op->type) {
        case KL_IOCP_ACCEPT: iocp_on_accept(s, st, op);  break;
        case KL_IOCP_READ:   iocp_on_read(s, op, bytes); break;
        case KL_IOCP_WRITE:  iocp_on_write(s, op, bytes); break;
        }
    }
    return 0;
}
