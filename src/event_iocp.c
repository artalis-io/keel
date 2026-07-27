/*
 * event_iocp.c — IOCP: the Windows *implementation* of the completion event axis
 * (PAL Phase 8). Compiled ONLY on Windows under `BACKEND=iocp` (Makefile selection).
 *
 * This TU is now purely the platform layer: the KlEventLoop lifecycle over an IOCP
 * port, and the completion.h backend contract — post overlapped AcceptEx/WSARecv/
 * WSASend and drain GetQueuedCompletionStatusEx into platform-independent
 * KlCompletionEvent's. The connection-driving logic lives in the
 * platform-independent completion_driver.c; the Win32/OVERLAPPED/WSA* mechanics
 * live only here, so a future io_uring-completion / POSIX-AIO backend implements the
 * same completion.h and reuses the driver unchanged. See docs/phase8b_iocp_breadth_design.md.
 */
#include <keel/event.h>
#include <keel/server.h>
#include <keel/connection.h>
#include "event_caps.h"
#include "socket.h"              /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED */
#include "completion.h"          /* the abstract axis this TU implements */

#include "sockcompat.h"          /* winsock2.h */
#include <windows.h>             /* IOCP */
#include <mswsock.h>             /* AcceptEx / GetAcceptExSockaddrs / SO_UPDATE_ACCEPT_CONTEXT */
#include <string.h>

#define KL_IOCP_ACCEPT_BACKLOG 8
#define KL_IOCP_ADDR_LEN       (sizeof(struct sockaddr_storage) + 16)

typedef struct {
    HANDLE                    port;
    LPFN_ACCEPTEX             acceptex;
    LPFN_GETACCEPTEXSOCKADDRS get_sockaddrs;
    int                       started;
    int                       accept_family;
    KlAllocator              *alloc;
} KlIocpState;

typedef enum { KL_IOCP_ACCEPT, KL_IOCP_READ, KL_IOCP_WRITE } KlIocpOpType;

/* One in-flight overlapped op. `ov` MUST be first (CONTAINING_RECORD round-trip). */
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
    HANDLE h = CreateIoCompletionPort((HANDLE)(uintptr_t)fd, st->port,
                                      (ULONG_PTR)udata, 0);
    return h ? 0 : -1;
}

int kl_event_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop; (void)fd; (void)mask; (void)udata;   /* no readiness re-arm */
    return 0;
}

int kl_event_del(KlEventLoop *loop, KlSocketHandle fd) {
    (void)loop; (void)fd;   /* a socket leaves the port when closed */
    return 0;
}

int kl_event_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* Driven by the completion tick (completion_driver.c) via kl_comp_drain, not
     * this readiness call. Defined because the KlEventLoop API requires it. */
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

/* A completion loop over native OS handles (SOCKETs on the port). COMPLETION makes
 * the Phase 7 negotiation require an OVERLAPPED provider (kl_caps_compatible). */
unsigned kl_event_caps(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD;
}

/* The overlapped socket provider: Winsock control-plane defaults (NULL ops) + the
 * OVERLAPPED capability the negotiation keys on. */
static const KlSocketOps IOCP_OPS = { .name = "iocp" };
static const KlSocketProvider IOCP_PROVIDER = {
    &IOCP_OPS, NULL, KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_OVERLAPPED,
};
const KlSocketProvider *kl_socket_provider_iocp(void) { return &IOCP_PROVIDER; }

/* ── completion.h implementation (the overlapped mechanics) ──────────── */

static void iocp_op_free(KlIocpOp *op) {
    if (op->sendbuf) kl_free(op->alloc, op->sendbuf, op->send_total);
    kl_free(op->alloc, op, sizeof(*op));
}

static KlIocpState *state_of(KlConn *c) {
    return c->ctx->loop._backend;   /* c->ctx == &server->ev */
}

int kl_comp_post_recv(KlConn *c) {
    (void)state_of(c);   /* the socket is already associated with the port */
    size_t space = c->read_cap - c->read_len;
    if (space == 0) return -1;   /* headers overflowed the buffer */

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

int kl_comp_post_send(KlConn *c, const KlIoVec *iov, int iovcnt, size_t total) {
    KlIocpOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_WRITE;
    op->alloc = c->alloc;
    op->conn = c;
    op->send_total = total;

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

int kl_comp_post_accept(struct KlServer *s) {
    KlIocpState *st = s->ev.loop._backend;
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

/* First-drain setup: load the extension fn pointers off the listen socket, learn
 * its address family, prime the accept backlog. */
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
        if (kl_comp_post_accept(s) < 0)
            return (i == 0) ? -1 : 0;
    }
    return 0;
}

int kl_comp_drain(struct KlServer *s, KlCompletionEvent *out, int max, int timeout_ms) {
    KlIocpState *st = s->ev.loop._backend;
    if (!st->started) {
        if (iocp_start(s, st) < 0) return -1;
        st->started = 1;
    }

    OVERLAPPED_ENTRY entries[64];
    ULONG got = 0;
    ULONG want = (max < 64) ? (ULONG)max : 64;
    BOOL ok = GetQueuedCompletionStatusEx(st->port, entries, want, &got,
                                          (DWORD)timeout_ms, FALSE);
    if (!ok)
        return 0;   /* WAIT_TIMEOUT — nothing this tick */

    int count = 0;
    for (ULONG i = 0; i < got && count < max; i++) {
        KlIocpOp *op = CONTAINING_RECORD(entries[i].lpOverlapped, KlIocpOp, ov);
        DWORD bytes = entries[i].dwNumberOfBytesTransferred;

        if (op->type == KL_IOCP_ACCEPT) {
            SOCKET lst = (SOCKET)s->listen_fd;
            setsockopt(op->accept_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                       (char *)&lst, sizeof(lst));
            struct sockaddr *local = NULL, *remote = NULL;
            int llen = 0, rlen = 0;
            st->get_sockaddrs(op->accept_buf, 0, (DWORD)KL_IOCP_ADDR_LEN,
                              (DWORD)KL_IOCP_ADDR_LEN, &local, &llen, &remote, &rlen);
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_ACCEPT;
            out[count].ok = 1;
            out[count].accepted_fd = (KlSocketHandle)op->accept_sock;
            if (rlen > 0 && (size_t)rlen <= sizeof(out[count].peer)) {
                memcpy(&out[count].peer, remote, (size_t)rlen);
                out[count].peer_len = (socklen_t)rlen;
            }
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_READ) {
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_READ;
            out[count].conn = op->conn;
            out[count].bytes = bytes;
            out[count].ok = 1;
            count++;
            iocp_op_free(op);
        } else { /* KL_IOCP_WRITE */
            op->send_done += bytes;
            if (bytes > 0 && op->send_done < op->send_total) {
                /* Partial send — re-post the remainder; do NOT surface an event
                 * until the whole response is out (the driver sees full writes). */
                WSABUF buf = { (ULONG)(op->send_total - op->send_done),
                               op->sendbuf + op->send_done };
                DWORD sent = 0;
                int rc = WSASend((SOCKET)op->conn->fd, &buf, 1, &sent, 0, &op->ov, NULL);
                if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                    memset(&out[count], 0, sizeof(out[count]));
                    out[count].kind = KL_COMP_WRITE;
                    out[count].conn = op->conn;
                    out[count].ok = 0;
                    count++;
                    iocp_op_free(op);
                }
                continue;
            }
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_WRITE;
            out[count].conn = op->conn;
            out[count].bytes = bytes;
            out[count].ok = (bytes > 0);
            count++;
            iocp_op_free(op);
        }
    }
    return count;
}
