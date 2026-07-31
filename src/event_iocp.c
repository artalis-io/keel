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
#include <keel/udp.h>            /* KlUdp — datagram recv over completion (8b-4c) */
#include <keel/tls.h>            /* KlTls feed_input — deliver received ciphertext (8b-5b) */
#include "event_caps.h"
#include "socket.h"              /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED */
#include "completion.h"          /* the abstract axis this TU implements */

#include "sockcompat.h"          /* winsock2.h */
#include <windows.h>             /* IOCP */
#include <mswsock.h>             /* AcceptEx / GetAcceptExSockaddrs / TransmitFile */
#include <io.h>                  /* _get_osfhandle (CRT fd → file HANDLE) */
#include <string.h>

#define KL_IOCP_ACCEPT_BACKLOG 8
#define KL_IOCP_ADDR_LEN       (sizeof(struct sockaddr_storage) + 16)
/* TransmitFile's documented maximum bytes per single call (~2 GiB, MSDN). A larger file
 * body would need chunked, offset-advancing TransmitFile re-posts (not yet implemented). */
#define KL_IOCP_TRANSMITFILE_MAX 0x7FFFFFFEu

struct KlIocpOp;
/* A registered readiness watch (8e-2c). The completion port cannot readiness-watch an
 * arbitrary fd, but the KlWatcher fds KEEL uses (the thread-pool wakeup) are loopback
 * sockets, so we post a persistent overlapped WSARecv: the wakeup write completes it,
 * surfacing a KL_COMP_WATCHER. Tracked so kl_event_del can cancel it. */
typedef struct KlIocpWatch {
    struct KlIocpWatch *next;
    SOCKET              fd;
    struct KlIocpOp    *op;   /* the pending WSARecv op */
} KlIocpWatch;

typedef struct {
    HANDLE                    port;
    LPFN_ACCEPTEX             acceptex;
    LPFN_GETACCEPTEXSOCKADDRS get_sockaddrs;
    int                       started;
    int                       accept_family;
    SOCKET                    listen_fd;   /* stored at prime — drain is ctx-scoped */
    KlIocpWatch              *watches;     /* registered readiness watches (8e-2c) */
    KlAllocator              *alloc;
} KlIocpState;

typedef enum {
    KL_IOCP_ACCEPT, KL_IOCP_READ, KL_IOCP_WRITE, KL_IOCP_SENDFILE,
    KL_IOCP_UDP_RECV, KL_IOCP_UDP_SEND,
    KL_IOCP_TLS_RECV,                          /* WSARecv ciphertext for a TLS conn (8b-5b) */
    KL_IOCP_WATCHER                            /* WSARecv on a KlWatcher socket (8e-2c) */
} KlIocpOpType;

/* One overlapped TLS ciphertext read: a whole TLS record (~16 KiB max) plus a
 * little header/MAC slack. The engine's input ring reassembles across recvs, so
 * this only bounds one WSARecv, not a record. */
#define KL_IOCP_CIPHER_SIZE (17u * 1024u)

/* One in-flight overlapped op. `ov` MUST be first (CONTAINING_RECORD round-trip). */
typedef struct KlIocpOp {
    OVERLAPPED    ov;
    KlIocpOpType  type;
    KlAllocator  *alloc;
    KlConn       *conn;                        /* READ / WRITE */
    KlUdp        *udp;                          /* UDP_RECV */
    SOCKET        accept_sock;                 /* ACCEPT */
    char          accept_buf[2 * KL_IOCP_ADDR_LEN];  /* ACCEPT: local+remote addr */
    struct sockaddr_storage src;               /* UDP_RECV: source addr (WSARecvFrom) */
    int           src_len;                     /* UDP_RECV: source addr length */
    char         *sendbuf;                     /* WRITE: contiguous response copy */
    size_t        send_total, send_done;       /* WRITE: partial-send tracking */
    void         *watcher_udata;               /* WATCHER: the tagged KlWatcher pointer */
    int           watcher_removed;             /* WATCHER: kl_event_del'd — free, don't re-post */
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

static void iocp_op_free(KlIocpOp *op);

/* Post (or re-post) the persistent WSARecv for a watcher socket (8e-2c). */
static int iocp_watch_post(KlIocpOp *op) {
    WSABUF b = { (ULONG)sizeof(op->accept_buf), op->accept_buf };
    DWORD flags = 0, recvd = 0;
    memset(&op->ov, 0, sizeof(op->ov));
    int rc = WSARecv(op->accept_sock, &b, 1, &recvd, &flags, &op->ov, NULL);
    return (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) ? -1 : 0;
}

int kl_event_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)mask;   /* completion model: no readiness mask — I/O is posted, not armed */
    if (!kl_handle_valid(fd))
        return -1;
    KlIocpState *st = loop->_backend;

    /* A TAGGED udata is a KlWatcher (thread-pool wakeup etc.). Associate its socket with
     * the port and post a persistent WSARecv, so the wakeup write surfaces a completion
     * the driver relays as KL_COMP_WATCHER (8e-2c). Untagged = a connection: its posted
     * overlapped ops complete on the port; no watch op. Keys on the shared LSB watcher
     * tag, not on any IOCP specific. */
    if ((uintptr_t)udata & 1) {
        for (KlIocpWatch *w = st->watches; w; w = w->next)
            if (w->fd == (SOCKET)fd) { w->op->watcher_udata = udata; return 0; }  /* idempotent */
        if (!CreateIoCompletionPort((HANDLE)(uintptr_t)fd, st->port, (ULONG_PTR)udata, 0))
            return -1;
        KlIocpOp *op = kl_malloc(st->alloc, sizeof(*op));
        if (!op) return -1;
        memset(op, 0, sizeof(*op));
        op->type = KL_IOCP_WATCHER;
        op->alloc = st->alloc;
        op->accept_sock = (SOCKET)fd;
        op->watcher_udata = udata;
        if (iocp_watch_post(op) < 0) { iocp_op_free(op); return -1; }
        KlIocpWatch *w = kl_malloc(st->alloc, sizeof(*w));
        if (!w) { op->watcher_removed = 1; CancelIoEx((HANDLE)(uintptr_t)fd, &op->ov); return -1; }
        w->fd = (SOCKET)fd;
        w->op = op;
        w->next = st->watches;
        st->watches = w;
        return 0;
    }

    HANDLE h = CreateIoCompletionPort((HANDLE)(uintptr_t)fd, st->port,
                                      (ULONG_PTR)udata, 0);
    return h ? 0 : -1;
}

int kl_event_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)mask;   /* no readiness re-arm; a watcher just updates its tag */
    if (!((uintptr_t)udata & 1)) return 0;
    KlIocpState *st = loop->_backend;
    for (KlIocpWatch *w = st->watches; w; w = w->next)
        if (w->fd == (SOCKET)fd) { w->op->watcher_udata = udata; return 0; }
    return 0;
}

int kl_event_del(KlEventLoop *loop, KlSocketHandle fd) {
    KlIocpState *st = loop->_backend;
    for (KlIocpWatch **link = &st->watches; *link; link = &(*link)->next)
        if ((*link)->fd == (SOCKET)fd) {
            KlIocpWatch *w = *link;
            *link = w->next;
            w->op->watcher_removed = 1;   /* on the (aborted) completion: free, don't re-post */
            CancelIoEx((HANDLE)(uintptr_t)fd, &w->op->ov);
            kl_free(st->alloc, w, sizeof(*w));
            return 0;
        }
    return 0;   /* a connection socket leaves the port when closed */
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
        KlIocpWatch *w = st->watches;   /* free watch ops (shutdown; the loop is stopped) */
        while (w) {
            KlIocpWatch *n = w->next;
            iocp_op_free(w->op);
            kl_free(st->alloc, w, sizeof(*w));
            w = n;
        }
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

/* The overlapped provider this completion loop needs (5a) — auto-wired by the server/client
 * when the caller configured none, so the IOCP backend is a source-compatible drop-in. */
const struct KlSocketProvider *kl_event_native_provider(const KlEventLoop *loop) {
    (void)loop;
    return kl_socket_provider_iocp();
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

    KlIocpOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->alloc = c->alloc;
    op->conn = c;

    WSABUF buf;
    if (c->tls) {
        /* TLS: read raw ciphertext into a transient op buffer (kept in sendbuf so
         * iocp_op_free reclaims it). read_buf holds *decrypted plaintext*, which
         * accumulates across records for headers — it must not be clobbered by an
         * inbound ciphertext read. kl_comp_drain feeds this buffer to the engine
         * (tls->feed_input) before freeing the op. */
        op->type = KL_IOCP_TLS_RECV;
        op->send_total = KL_IOCP_CIPHER_SIZE;
        op->sendbuf = kl_malloc(c->alloc, KL_IOCP_CIPHER_SIZE);
        if (!op->sendbuf) { op->send_total = 0; iocp_op_free(op); return -1; }
        buf.len = (ULONG)KL_IOCP_CIPHER_SIZE;
        buf.buf = op->sendbuf;
    } else {
        size_t space = c->read_cap - c->read_len;
        if (space == 0) { iocp_op_free(op); return -1; }   /* headers overflowed */
        op->type = KL_IOCP_READ;
        buf.len = (ULONG)space;
        buf.buf = c->read_buf + c->read_len;
    }

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

int kl_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    HANDLE hfile = (HANDLE)_get_osfhandle(file_fd);
    if (hfile == INVALID_HANDLE_VALUE) return -1;

    KlIocpOp *op = kl_malloc(c->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_SENDFILE;
    op->alloc = c->alloc;
    op->conn = c;

    /* Copy the response head — TransmitFile's head buffer must outlive the op. */
    op->send_total = head_total;
    op->sendbuf = kl_malloc(c->alloc, head_total ? head_total : 1);
    if (!op->sendbuf) { op->send_total = 0; iocp_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < head_n; i++) {
        memcpy(op->sendbuf + off, head_iov[i].base, head_iov[i].len);
        off += head_iov[i].len;
    }

    /* TransmitFile's byte count is a DWORD and its documented per-call maximum is
     * ~2 GiB (TF_MAX). A larger file body needs chunked TransmitFile (offset-advancing
     * re-posts across several completions) — not yet implemented. Rather than silently
     * truncate via the (DWORD)count cast (or fail opaquely inside TransmitFile), reject
     * it cleanly here so the caller closes the connection instead of sending wrong bytes. */
    if (count > KL_IOCP_TRANSMITFILE_MAX) { iocp_op_free(op); return -1; }

    TRANSMIT_FILE_BUFFERS tb;
    memset(&tb, 0, sizeof(tb));
    tb.Head = op->sendbuf;
    tb.HeadLength = (DWORD)head_total;

    /* File offset 0 via the OVERLAPPED (op->ov is zeroed → Offset 0). One overlapped
     * op sends head + `count` file bytes; completion arrives as KL_IOCP_SENDFILE. */
    BOOL ok = TransmitFile((SOCKET)c->fd, hfile, (DWORD)count, 0, &op->ov, &tb, 0);
    if (!ok && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

/* Post one overlapped WSARecvFrom for a UDP socket on the completion loop (8b-4c).
 * Receives into the socket's own recv buffer and captures the source address; the
 * completion surfaces a KL_COMP_UDP_RECV event.
 *
 * NOTE: this captures the SOURCE address only. The datagram's local (destination)
 * address (KlCompletionEvent.local, used by kl_udp_send_to_from reply-from) needs a
 * pktinfo control message, which WSARecvFrom does not deliver — that requires the
 * WSARecvMsg extension (WSAID_WSARECVMSG) with an IP_PKTINFO control buffer, the
 * Winsock analogue of the io_uring/pollcomp recvmsg path. Until then IOCP leaves
 * ev->local_len = 0 (the event is memset), so the driver delivers a NULL local —
 * the same graceful degradation as pktinfo being disabled. Follow-up. */
int kl_comp_post_udp_recv(KlUdp *udp) {
    KlIocpState *st = udp->ctx->loop._backend;
    KlIocpOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_UDP_RECV;
    op->alloc = st->alloc;
    op->udp = udp;
    op->src_len = (int)sizeof(op->src);

    WSABUF buf = { (ULONG)udp->recv_buf_size, (char *)udp->recv_buf };
    DWORD flags = 0, recvd = 0;
    int rc = WSARecvFrom((SOCKET)udp->fd, &buf, 1, &recvd, &flags,
                         (struct sockaddr *)&op->src, &op->src_len, &op->ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

/* Post one overlapped WSASendTo for a UDP socket (8b-4d). Copies the datagram + its
 * destination into the op (owned until completion); surfaces KL_COMP_UDP_SEND. */
int kl_comp_post_udp_send(KlUdp *udp, const void *data, size_t len,
                          const struct sockaddr *dest, int dest_len) {
    KlIocpState *st = udp->ctx->loop._backend;
    KlIocpOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_UDP_SEND;
    op->alloc = st->alloc;
    op->udp = udp;
    op->send_total = len;

    op->sendbuf = kl_malloc(st->alloc, len ? len : 1);
    if (!op->sendbuf) { op->send_total = 0; iocp_op_free(op); return -1; }
    memcpy(op->sendbuf, data, len);
    if (dest_len > 0 && (size_t)dest_len <= sizeof(op->src)) {
        memcpy(&op->src, dest, (size_t)dest_len);
        op->src_len = dest_len;
    }

    WSABUF buf = { (ULONG)len, op->sendbuf };
    DWORD sent = 0;
    int rc = WSASendTo((SOCKET)udp->fd, &buf, 1, &sent, 0,
                       (struct sockaddr *)&op->src, op->src_len, &op->ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

/* First-drain setup: load the extension fn pointers off the listen socket, learn
 * its address family, store it, and prime the accept backlog. Idempotent — latches
 * on st->started so the server may call it every tick. Server-scoped (needs the
 * listen socket); keeps kl_comp_drain ctx-scoped/server-agnostic. */
int kl_comp_prime_accepts(struct KlServer *s) {
    KlIocpState *st = s->ev.loop._backend;
    if (st->started)
        return 0;

    SOCKET lst = (SOCKET)s->listen_fd;
    st->listen_fd = lst;
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

    st->started = 1;
    for (int i = 0; i < KL_IOCP_ACCEPT_BACKLOG; i++) {
        if (kl_comp_post_accept(s) < 0)
            return (i == 0) ? -1 : 0;
    }
    return 0;
}

/* Cancel outstanding overlapped ops on `fd` (idle-timeout sweep). CancelIoEx aborts the
 * pending WSARecv/WSASend; each aborted op still completes via GetQueuedCompletionStatus
 * with an error, so the driver releases the connection through its normal completion path
 * (no dangling op, no double release).
 *
 * The NULL OVERLAPPED cancels ALL outstanding I/O the loop thread issued on `fd` — which is
 * exactly what we want when tearing a connection down (recv and, if any, an in-flight send).
 * Each cancelled op posts its own completion, and the driver releases the conn from the last
 * one; the ≤N-completions-then-release accounting lives in the driver, so cancelling all of a
 * dying fd's ops here is safe. Single-loop-thread, so no cross-thread cancel race. */
void kl_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    (void)ctx;
    CancelIoEx((HANDLE)(uintptr_t)fd, NULL);
}

int kl_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    KlIocpState *st = ctx->loop._backend;

    OVERLAPPED_ENTRY entries[64];
    ULONG got = 0;
    ULONG want = (max < 64) ? (ULONG)max : 64;
    BOOL ok = GetQueuedCompletionStatusEx(st->port, entries, want, &got,
                                          (DWORD)timeout_ms, FALSE);
    if (!ok) {
        /* Distinguish an idle tick from a fatal loop error. WAIT_TIMEOUT is the normal
         * "no completions this tick" case → 0. Anything else (e.g. ERROR_ABANDONED_WAIT_0
         * or ERROR_INVALID_HANDLE if the port is closed during shutdown) is a real error →
         * -1, so the run loop stops instead of silently spinning on a dead port. */
        return (GetLastError() == WAIT_TIMEOUT) ? 0 : -1;
    }

    int count = 0;
    for (ULONG i = 0; i < got && count < max; i++) {
        KlIocpOp *op = CONTAINING_RECORD(entries[i].lpOverlapped, KlIocpOp, ov);
        DWORD bytes = entries[i].dwNumberOfBytesTransferred;

        if (op->type == KL_IOCP_ACCEPT) {
            SOCKET lst = st->listen_fd;
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
            out[count].target = op->conn;
            out[count].bytes = bytes;
            out[count].ok = 1;
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_TLS_RECV) {
            /* Hand the received ciphertext to the TLS engine via the public
             * feed_input op (backend-agnostic — no mbedTLS knowledge here), then
             * surface a plain READ event. The driver owns all TLS protocol logic
             * (handshake / decrypt / encrypt); the buffer is consumed here so the
             * op — which carries it in sendbuf — can be freed immediately. bytes==0
             * means the peer closed: surface it so the driver tears down. */
            KlConn *c = op->conn;
            if (bytes > 0 && c->tls->feed_input)
                c->tls->feed_input(c->tls, op->sendbuf, (size_t)bytes);
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_READ;
            out[count].target = c;
            out[count].bytes = bytes;
            out[count].ok = 1;
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_WRITE) {
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
                    out[count].target = op->conn;
                    out[count].ok = 0;
                    count++;
                    iocp_op_free(op);
                }
                continue;
            }
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_WRITE;
            out[count].target = op->conn;
            out[count].bytes = bytes;
            out[count].ok = (bytes > 0);
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_UDP_RECV) {
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_UDP_RECV;
            out[count].target = op->udp;
            out[count].bytes = bytes;
            out[count].ok = 1;
            out[count].buf = op->udp->recv_buf;
            if (op->src_len > 0 && (size_t)op->src_len <= sizeof(out[count].peer)) {
                memcpy(&out[count].peer, &op->src, (size_t)op->src_len);
                out[count].peer_len = (socklen_t)op->src_len;
            }
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_UDP_SEND) {
            /* Whole datagram send done — surface the original len so the driver
             * releases exactly the reserved outstanding bytes (success or not). */
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_UDP_SEND;
            out[count].target = op->udp;
            out[count].bytes = op->send_total;
            out[count].ok = (bytes > 0);
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_WATCHER) {
            /* A watcher socket became readable (wakeup write). Surface KL_COMP_WATCHER for
             * the driver to route to kl_event_dispatch, then re-post the WSARecv to keep
             * watching. If kl_event_del'd it (watcher_removed), this is the aborted
             * completion — just free the op. (8e-2c) */
            if (op->watcher_removed) { iocp_op_free(op); continue; }
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_WATCHER;
            out[count].target = op->watcher_udata;
            out[count].bytes = (size_t)KL_EVENT_READ;
            out[count].ok = 1;
            count++;
            if (iocp_watch_post(op) < 0)
                op->watcher_removed = 1;   /* re-post failed — freed at kl_event_close */
        } else { /* KL_IOCP_SENDFILE — TransmitFile completes as a unit (head+file);
                  * surface it as a completed write. */
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_WRITE;
            out[count].target = op->conn;
            out[count].bytes = bytes;
            out[count].ok = (bytes > 0);
            count++;
            iocp_op_free(op);
        }
    }
    return count;
}
