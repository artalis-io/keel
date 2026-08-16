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
#include "event_builtin.h"
#include <keel/server.h>
#include <keel/connection.h>
#include <keel/udp.h>            /* KlUdp — datagram recv over completion (8b-4c) */
#include "event_caps.h"
#include "socket.h"              /* KlSocketProvider + KL_SOCK_CAP_OVERLAPPED */
#include "sockaddr_native.h"     /* KlSockAddr -> Winsock sockaddr for the overlapped UDP send */
#include "completion.h"          /* the abstract axis this TU implements */
#include "datagram_life.h"       /* stable-liveness token for datagram completion ops (neutral) */

#include "sockcompat.h"          /* winsock2.h */
#include <windows.h>             /* IOCP */
#include <mswsock.h>             /* AcceptEx / GetAcceptExSockaddrs / TransmitFile */
#include "udp_cmsg_win.h"        /* WSARecvMsg fetch + pktinfo parse — UDP local addr (shared) */
#include "dgram_recv_classify.h" /* platform-agnostic completed-recv classification (unit-tested) */
#include <io.h>                  /* _get_osfhandle (CRT fd → file HANDLE) */
#include <string.h>
#include <stdlib.h>              /* getenv / strtol — TransmitFile chunk-cap test seam */

#define KL_IOCP_ACCEPT_BACKLOG 8
#define KL_IOCP_ADDR_LEN       (sizeof(struct sockaddr_storage) + 16)
/* TransmitFile's documented maximum bytes per single call (~2 GiB, MSDN). A larger file body
 * is sent as several offset-advancing TransmitFile chunks capped at this size, re-posted from
 * the KL_IOCP_SENDFILE completion (iocp_post_transmitfile_chunk). */
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

/* A tracked in-flight ConnectEx op (LC-0). Tracked so iocp_comp_cancel(fd) can mark the
 * client's connect op aborted before the client frees its (detached) watcher + closes the
 * socket — the aborted completion is then dropped in the drain instead of dispatching
 * against freed memory. (The IOCP backend doesn't otherwise track conn ops; connect is the
 * one op with no KlConn owner.) */
typedef struct KlIocpConnect {
    struct KlIocpConnect *next;
    SOCKET                fd;
    struct KlIocpOp      *op;
} KlIocpConnect;

/* In-flight AcceptEx tracker (6B-3 2b-ii). AcceptEx PRE-creates its accept socket (op->accept_sock)
 * before the op completes; tracking every posted accept lets kl_event_close_builtin() forcibly
 * reclaim the ones a (delayed/failed) cancellation never retired — closing the pre-created socket
 * and freeing the op — so teardown never leaks an accept socket or op. Mirrors KlIocpConnect. */
typedef struct KlIocpAccept {
    struct KlIocpAccept *next;
    struct KlIocpOp     *op;
} KlIocpAccept;

typedef struct {
    HANDLE                    port;
    LPFN_ACCEPTEX             acceptex;
    LPFN_GETACCEPTEXSOCKADDRS get_sockaddrs;
    int                       started;
    int                       accept_family;
    SOCKET                    listen_fd;   /* stored at prime — drain is ctx-scoped */
    KlIocpWatch              *watches;     /* registered readiness watches (8e-2c) */
    KlIocpConnect            *connects;    /* in-flight ConnectEx ops (LC-0) */
    KlIocpAccept             *accepts;     /* in-flight AcceptEx ops (6B-3 2b-ii) */
    struct KlIocpOp          *ops;         /* global registry of ALL outstanding ops (2b review) */
    int                       quiescing;   /* teardown: drain must not re-post any op (2b review) */
    KlAllocator              *alloc;
} KlIocpState;

typedef enum {
    KL_IOCP_ACCEPT, KL_IOCP_READ, KL_IOCP_WRITE, KL_IOCP_SENDFILE,
    KL_IOCP_DGRAM_RECV, KL_IOCP_DGRAM_SEND,
    KL_IOCP_WATCHER,                           /* WSARecv on a KlWatcher socket (8e-2c) */
    KL_IOCP_CONNECT                            /* ConnectEx outbound connect (LC-0) */
} KlIocpOpType;

/* One in-flight overlapped op. `ov` MUST be first (CONTAINING_RECORD round-trip). */
typedef struct KlIocpOp {
    OVERLAPPED    ov;
    KlIocpOpType  type;
    KlAllocator  *alloc;
    KlStream     *stream;                      /* READ / WRITE / SENDFILE target (raw transport) */
    /* Datagram ops (DGRAM_RECV/_SEND): the transport-neutral stable-liveness token, retained at post
     * so the op NEVER dereferences KlUdpTransport afterwards (the owner may be freed while this op is in
     * flight). The recv buffer + capacity + pktinfo flag are COPIED at post (buf/buflen/dg_pktinfo) so
     * the completion touches only the op; op_sock already carries the socket (CancelIoEx / GetOverlappedResult). */
    KlDgramLife  *life;
    void         *buf;                         /* UDP_RECV: receive buffer (pinned by `life`) */
    size_t        buflen;                      /* UDP_RECV: capacity at post time */
    int           dg_pktinfo;                  /* UDP_RECV: capture pktinfo local addr */
    SOCKET        accept_sock;                 /* ACCEPT */
    char          accept_buf[2 * KL_IOCP_ADDR_LEN];  /* ACCEPT: local+remote addr */
    struct sockaddr_storage src;               /* UDP_RECV: source addr (WSARecvMsg name) */
    int           src_len;                     /* UDP_RECV: source addr length */
    WSAMSG        umsg;                         /* UDP_RECV: WSARecvMsg header (name + buf + control) */
    WSABUF        ubuf;                         /* UDP_RECV: single recv iovec into dg->recv_buf */
    char          uctrl[KL_UDP_WIN_RX_CMSG_SPACE]; /* UDP_RECV: pktinfo control buffer (local addr) */
    int           via_recvmsg;                 /* UDP_RECV: 1 = WSARecvMsg (has control), 0 = WSARecvFrom */
    char         *sendbuf;                     /* WRITE/SENDFILE: contiguous response (head) copy */
    size_t        send_total, send_done;       /* WRITE: partial-send tracking; SENDFILE: head len */
    HANDLE        file_h;                       /* SENDFILE: file handle for chunked re-posts */
    uint64_t      file_total, file_done;        /* SENDFILE: total file bytes / bytes transmitted */
    uint64_t      file_chunk;                   /* SENDFILE: bytes in the in-flight TransmitFile */
    union {
        void              *watcher_udata;   /* WATCHER/CONNECT: the tagged KlWatcher pointer */
    };
    int           watcher_removed;             /* WATCHER: kl_event_del'd — free, don't re-post */
    /* Global outstanding-op registry (6B-3 2b review): EVERY posted op is linked here so teardown
     * (kl_event_close_builtin) can cancel + dequeue every kernel-owned OVERLAPPED before freeing it
     * — including the otherwise-untracked READ/WRITE/SENDFILE/UDP ops. op_sock is the socket the
     * overlapped I/O runs on (CancelIoEx target). g_link points at the pointer that points to this
     * op (O(1) unlink). Registered by iocp_op_register at post; unlinked by iocp_op_free. */
    SOCKET             op_sock;
    struct KlIocpOp   *g_next;
    struct KlIocpOp  **g_link;
} KlIocpOp;

/* ── KlEventLoop lifecycle over an IOCP port ─────────────────────────── */

int kl_event_init_builtin(KlEventLoop *loop) {
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
static void iocp_op_register(KlIocpState *st, KlIocpOp *op, SOCKET op_sock);   /* 2b review */
/* Cancel + dequeue every tracked outstanding overlapped op before free (6B-3 2b review); below. */
static void iocp_quiesce_port_for_close(KlIocpState *st);

/* Post (or re-post) the persistent WSARecv for a watcher socket (8e-2c). */
static int iocp_watch_post(KlIocpOp *op) {
    WSABUF b = { (ULONG)sizeof(op->accept_buf), op->accept_buf };
    DWORD flags = 0, recvd = 0;
    memset(&op->ov, 0, sizeof(op->ov));
    int rc = WSARecv(op->accept_sock, &b, 1, &recvd, &flags, &op->ov, NULL);
    return (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) ? -1 : 0;
}

int kl_event_add_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
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
        iocp_op_register(st, op, (SOCKET)fd);
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

int kl_event_mod_builtin(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)mask;   /* no readiness re-arm; a watcher just updates its tag */
    if (!((uintptr_t)udata & 1)) return 0;
    KlIocpState *st = loop->_backend;
    for (KlIocpWatch *w = st->watches; w; w = w->next)
        if (w->fd == (SOCKET)fd) { w->op->watcher_udata = udata; return 0; }
    return 0;
}

int kl_event_del_builtin(KlEventLoop *loop, KlSocketHandle fd) {
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

int kl_event_wait_builtin(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* Driven by the completion tick (completion_driver.c) via kl_comp_drain, not
     * this readiness call. Defined because the KlEventLoop API requires it. */
    (void)loop; (void)out; (void)max;
    if (timeout_ms > 0)
        Sleep((DWORD)timeout_ms);
    return 0;
}

void kl_event_close_builtin(KlEventLoop *loop) {
    KlIocpState *st = loop->_backend;
    if (st) {
        /* Every tracked op (watch / connect / accept) may still own a kernel OVERLAPPED. Cancel +
         * DEQUEUE each before freeing it — freeing an OVERLAPPED the kernel is still writing is a
         * use-after-free (6B-3 2b review). iocp_quiesce_port_for_close drains the port until the
         * tracked lists empty (cancelled I/O always completes → terminating). */
        st->quiescing = 1;
        iocp_quiesce_port_for_close(st);
        /* Anything still tracked here means the drain hit a fatal port error (catastrophic only):
         * free our tracker nodes but intentionally LEAK each op record — its OVERLAPPED was never
         * dequeued, so it may still be kernel-owned. closesocket reclaims an accept fd. Memory-safe
         * over leak-free on that path. */
        KlIocpWatch *w = st->watches;
        while (w) { KlIocpWatch *n = w->next; kl_free(st->alloc, w, sizeof(*w)); w = n; }
        KlIocpConnect *tr = st->connects;
        while (tr) { KlIocpConnect *n = tr->next; kl_free(st->alloc, tr, sizeof(*tr)); tr = n; }
        KlIocpAccept *at = st->accepts;
        while (at) { KlIocpAccept *n = at->next; closesocket(at->op->accept_sock);
                     kl_free(st->alloc, at, sizeof(*at)); at = n; }
        st->watches = NULL; st->connects = NULL; st->accepts = NULL;
        if (st->port) CloseHandle(st->port);
        kl_free(st->alloc, st, sizeof(*st));
        loop->_backend = NULL;
    }
    loop->fd = -1;
}

/* A completion loop over native OS handles (SOCKETs on the port). COMPLETION makes
 * the Phase 7 negotiation require an OVERLAPPED provider (kl_caps_compatible). */
unsigned kl_event_caps_builtin(const KlEventLoop *loop) {
    (void)loop;
    return KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD;
}

/* The overlapped provider this completion loop needs (5a) — auto-wired by the server/client
 * when the caller configured none, so the IOCP backend is a source-compatible drop-in. */
const struct KlSocketProvider *kl_event_native_provider_builtin(const KlEventLoop *loop) {
    (void)loop;
    return kl_socket_provider_iocp();
}

/* The overlapped socket provider: Winsock control-plane defaults (NULL ops) + the
 * OVERLAPPED capability the negotiation keys on. */
static const KlSocketOps IOCP_OPS = { .name = "iocp" };
/* The Winsock datagram ops (socket_dgram_win.c) — reused for UDP config/opts on the
 * completion loop; the readiness dgram send/recv are unused here (comp path drives). */
extern const struct KlDatagramOps kl_socket_winsock_dgram_ops;
static const KlSocketProvider IOCP_PROVIDER = {
    &IOCP_OPS, NULL, KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_OVERLAPPED,
    &kl_socket_winsock_dgram_ops,
};
const KlSocketProvider *kl_socket_provider_iocp(void) { return &IOCP_PROVIDER; }

/* ── completion.h implementation (the overlapped mechanics) ──────────── */

/* ── TEMPORARY IOCP teardown/registry diagnostics (KL_IOCP_TEARDOWN_TRACE) ─────────────
 * Compiled out by default; enabled ONLY for the IOCP smoke CI step to capture runtime evidence
 * of the smoke-udp teardown hang (which does not reproduce off real Windows/IOCP). This changes
 * NO production wait or ownership rule: with the flag OFF, the quiesce still waits INFINITE and
 * nothing is logged. With the flag ON, the quiesce bounds its wait so it DUMPS the outstanding
 * registry and aborts (fails the smoke, non-zero exit) instead of hanging silently — a diagnostic,
 * never a fix. Remove (or leave compiled out) once the lifecycle bug is understood + corrected. */
#ifdef KL_IOCP_TEARDOWN_TRACE
#include <stdio.h>
#include <stdlib.h>   /* abort — the diagnostic stall handler */
#define KLIT(...) do { fprintf(stderr, "[IOCPTRACE] " __VA_ARGS__); fflush(stderr); } while (0)
static const char *klit_type(int t) {
    switch (t) {
    case KL_IOCP_ACCEPT: return "ACCEPT"; case KL_IOCP_READ: return "READ";
    case KL_IOCP_WRITE: return "WRITE"; case KL_IOCP_SENDFILE: return "SENDFILE";
    case KL_IOCP_WATCHER: return "WATCHER"; case KL_IOCP_CONNECT: return "CONNECT";
    case KL_IOCP_DGRAM_RECV: return "DGRAM_RECV"; case KL_IOCP_DGRAM_SEND: return "DGRAM_SEND";
    default: return "?";
    }
}
static void klit_dump_ops(KlIocpState *st) {
    int n = 0;
    for (KlIocpOp *o = st->ops; o; o = o->g_next) {
        KLIT("  reg[%d] op=%p type=%s sock=%llu life=%p ov=%p\n", n++, (void *)o, klit_type(o->type),
             (unsigned long long)o->op_sock, (void *)o->life, (void *)&o->ov);
    }
    KLIT("  reg: %d outstanding op(s)\n", n);
}
#define KLIT_DUMP(st) klit_dump_ops(st)
#else
#define KLIT(...)     do { } while (0)
#define KLIT_DUMP(st) do { (void)(st); } while (0)
#endif

/* Register a freshly-allocated + zeroed op in the global outstanding-op registry (6B-3 2b review),
 * recording the socket its overlapped I/O runs on (for CancelIoEx at teardown). Call once, right
 * after memset, at every post site. O(1). */
static void iocp_op_register(KlIocpState *st, KlIocpOp *op, SOCKET op_sock) {
    op->op_sock = op_sock;
    op->g_next  = st->ops;
    op->g_link  = &st->ops;
    if (st->ops) st->ops->g_link = &op->g_next;
    st->ops = op;
    KLIT("register op=%p type=%s sock=%llu\n", (void *)op, klit_type(op->type),
         (unsigned long long)op_sock);
}

static void iocp_op_free(KlIocpOp *op) {
    KLIT("free op=%p type=%s sock=%llu life=%p\n", (void *)op, klit_type(op->type),
         (unsigned long long)op->op_sock, (void *)op->life);
    if (op->g_link) {                          /* unlink from the global registry (if registered) */
        *op->g_link = op->g_next;
        if (op->g_next) op->g_next->g_link = op->g_link;
        op->g_link = NULL;
    }
    /* A datagram op that still owns its token reference (dropped WITHOUT emitting an event — post-
     * failure unwind, or loop teardown where iocp_quiesce_port_for_close / the drain's quiescing
     * branch cancel + free the op) releases it here. Its final release frees the receive storage. The
     * drain transfers the ref to the event and NULLs op->life first, so an emitted op does not
     * double-release. Non-datagram ops carry op->life == NULL. */
    if (op->life) kl_dgram_life_release(op->life);
    /* send_total is the sendbuf allocation size for WRITE/SENDFILE (the send total, or 1 when
     * total==0 — the alloc is `total ? total : 1`). Fall back to 1 for a zero-length send so a
     * sized custom allocator frees the right bucket. Receives no longer own a buffer. */
    if (op->sendbuf) kl_free(op->alloc, op->sendbuf, op->send_total ? op->send_total : 1);
    kl_free(op->alloc, op, sizeof(*op));
}

/* Raw receive: WSARecv up to `cap` bytes into the caller-supplied `buf` on `stream`. No TLS /
 * connection-state knowledge — the HTTP adapter chose the buffer. */
static int iocp_comp_post_recv(KlStream *stream, void *buf_in, size_t cap) {
    if (!buf_in || cap == 0) return -1;
    if (cap > (size_t)0xFFFFFFFFu) cap = 0xFFFFFFFFu;   /* clamp to WSABUF ULONG len */
    KlIocpOp *op = kl_malloc(stream->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->alloc = stream->alloc;
    op->stream = stream;
    op->type = KL_IOCP_READ;
    iocp_op_register(stream->ctx->loop._backend, op, (SOCKET)stream->fd);

    WSABUF buf;
    buf.len = (ULONG)cap;
    buf.buf = (char *)buf_in;

    DWORD flags = 0, received = 0;
    int rc = WSARecv((SOCKET)stream->fd, &buf, 1, &received, &flags, &op->ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

static int iocp_comp_post_send(KlStream *stream, const KlIoVec *iov, int iovcnt, size_t total) {
    KlIocpOp *op = kl_malloc(stream->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_WRITE;
    op->alloc = stream->alloc;
    op->stream = stream;
    op->send_total = total;
    iocp_op_register(stream->ctx->loop._backend, op, (SOCKET)stream->fd);

    op->sendbuf = kl_malloc(stream->alloc, total ? total : 1);
    if (!op->sendbuf) { op->send_total = 0; iocp_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        memcpy(op->sendbuf + off, iov[i].base, iov[i].len);
        off += iov[i].len;
    }

    WSABUF buf = { (ULONG)total, op->sendbuf };
    DWORD sent = 0;
    int rc = WSASend((SOCKET)stream->fd, &buf, 1, &sent, 0, &op->ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

static int iocp_comp_post_accept(struct KlServer *s) {
    if (!s) return -1;
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
    /* op_sock = the LISTEN socket: AcceptEx's overlapped I/O runs there (CancelIoEx target). */
    iocp_op_register(st, op, (SOCKET)s->listen_fd);

    /* Track the op BEFORE posting so a synchronous completion (or teardown close) can always find
     * it. On a hard post failure we untrack + free below. */
    KlIocpAccept *tr = kl_malloc(st->alloc, sizeof(*tr));
    if (!tr) { closesocket(a); iocp_op_free(op); return -1; }
    tr->op = op; tr->next = st->accepts; st->accepts = tr;

    DWORD bytes = 0;
    BOOL ok = st->acceptex((SOCKET)s->listen_fd, a, op->accept_buf,
                           0, (DWORD)KL_IOCP_ADDR_LEN, (DWORD)KL_IOCP_ADDR_LEN,
                           &bytes, &op->ov);
    if (!ok && WSAGetLastError() != WSA_IO_PENDING) {
        st->accepts = tr->next;                 /* unlink (it is the head we just pushed) */
        kl_free(st->alloc, tr, sizeof(*tr));
        closesocket(a);
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

/* Unlink the AcceptEx tracker for `op` (on completion). Returns whether it was found + freed. */
static void iocp_accept_untrack(KlIocpState *st, const KlIocpOp *op) {
    for (KlIocpAccept **link = &st->accepts; *link; link = &(*link)->next)
        if ((*link)->op == op) {
            KlIocpAccept *tr = *link;
            *link = tr->next;
            kl_free(st->alloc, tr, sizeof(*tr));
            return;
        }
}

/* TransmitFile's per-call byte cap. Defaults to the documented ~2 GiB maximum; a test may
 * lower it via KEEL_IOCP_TF_CHUNK to exercise the chunked (offset-advancing) path on a small
 * file without a >2 GiB fixture. Read + cached once (single-threaded loop; same value across
 * servers in a process). */
static DWORD iocp_tf_chunk_cap(void) {
    static long cap = 0;
    if (cap == 0) {
        const char *e = getenv("KEEL_IOCP_TF_CHUNK");
        long v = e ? strtol(e, NULL, 10) : 0;
        cap = (v > 0 && (unsigned long)v <= KL_IOCP_TRANSMITFILE_MAX)
                  ? v : (long)KL_IOCP_TRANSMITFILE_MAX;
    }
    return (DWORD)cap;
}

/* Post one chunk of a (possibly multi-chunk) TransmitFile: file bytes
 * [file_done, file_done+chunk) at the OVERLAPPED file offset, with the response head
 * prepended only on the first chunk. Re-armed by the KL_IOCP_SENDFILE completion until the
 * whole file is out, so the driver still sees ONE KL_COMP_WRITE (head+file as a unit) and
 * memory stays bounded (one op, ≤1 in flight). Returns 0 on posted/pending, -1 on error. */
static int iocp_post_transmitfile_chunk(KlIocpOp *op) {
    uint64_t remaining = op->file_total - op->file_done;
    DWORD    cap = iocp_tf_chunk_cap();
    DWORD    chunk = (remaining < (uint64_t)cap) ? (DWORD)remaining : cap;
    op->file_chunk = chunk;

    memset(&op->ov, 0, sizeof(op->ov));   /* fresh op state; carry the file offset in it */
    op->ov.Offset     = (DWORD)(op->file_done & 0xFFFFFFFFu);
    op->ov.OffsetHigh = (DWORD)(op->file_done >> 32);

    TRANSMIT_FILE_BUFFERS tb;
    memset(&tb, 0, sizeof(tb));
    if (op->file_done == 0 && op->send_total > 0) {   /* head rides only the first chunk */
        tb.Head = op->sendbuf;
        tb.HeadLength = (DWORD)op->send_total;
    }
    BOOL ok = TransmitFile((SOCKET)op->stream->fd, op->file_h, chunk, 0, &op->ov, &tb, 0);
    if (!ok && WSAGetLastError() != WSA_IO_PENDING)
        return -1;
    return 0;
}

static int iocp_comp_post_sendfile(KlConn *c, const KlIoVec *head_iov, int head_n,
                          size_t head_total, int file_fd, uint64_t count) {
    KlStream *stream = &c->stream;   /* post_sendfile stays KlConn-typed (Phase-A audit §8) */
    HANDLE hfile = (HANDLE)_get_osfhandle(file_fd);
    if (hfile == INVALID_HANDLE_VALUE) return -1;

    KlIocpOp *op = kl_malloc(stream->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_SENDFILE;
    op->alloc = stream->alloc;
    op->stream = stream;
    op->file_h = hfile;
    op->file_total = count;
    op->file_done = 0;
    iocp_op_register(stream->ctx->loop._backend, op, (SOCKET)stream->fd);

    /* Copy the response head — TransmitFile's head buffer must outlive the op. */
    op->send_total = head_total;
    op->sendbuf = kl_malloc(stream->alloc, head_total ? head_total : 1);
    if (!op->sendbuf) { op->send_total = 0; iocp_op_free(op); return -1; }
    size_t off = 0;
    for (int i = 0; i < head_n; i++) {
        memcpy(op->sendbuf + off, head_iov[i].base, head_iov[i].len);
        off += head_iov[i].len;
    }

    /* A file larger than TransmitFile's ~2 GiB per-call cap is sent as several
     * offset-advancing chunks, each re-posted from the KL_IOCP_SENDFILE completion (the
     * head rides only the first). Post the first chunk here. */
    if (iocp_post_transmitfile_chunk(op) < 0) { iocp_op_free(op); return -1; }
    return 0;
}

/* Post one overlapped UDP receive on the completion loop (8b-4c). Prefers WSARecvMsg
 * (WSAID_WSARECVMSG) so an IP_PKTINFO control message yields the datagram's local
 * (destination) address (KlCompletionEvent.local, used by kl_udp_send_to_from reply-from) —
 * the Winsock analogue of the io_uring/pollcomp recvmsg path, sharing the fetch + parse with
 * the readiness Windows recv (udp_cmsg_win.h). Falls back to WSARecvFrom (source address
 * only, local left 0) if the extension is unavailable. Either way the completion surfaces a
 * KL_COMP_DGRAM_RECV event. */
static int iocp_comp_post_dgram_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *rop) {
    KlIocpState *st = ctx->loop._backend;
    KlIocpOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;                 /* nothing taken → caller releases its retained token ref */
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_DGRAM_RECV;
    op->alloc = st->alloc;
    op->src_len = (int)sizeof(op->src);
    /* The receive buffer + capture flags travel in the descriptor; the op never dereferences a transport.
     * The buffer stays valid because the token ref (transferred below) pins it past the op's lifetime. */
    op->buf        = rop->buf;
    op->buflen     = rop->cap;
    op->dg_pktinfo = (rop->capture & KL_DGRAM_RX_PKTINFO) != 0;
    iocp_op_register(st, op, (SOCKET)rop->fd);

    LPFN_WSARECVMSG recvmsg = kl_udp_win_get_recvmsg((SOCKET)rop->fd);
    if (recvmsg) {
        op->via_recvmsg = 1;
        op->ubuf.len = (ULONG)op->buflen;
        op->ubuf.buf = (char *)op->buf;
        op->umsg.name = (SOCKADDR *)&op->src;
        op->umsg.namelen = (INT)sizeof(op->src);
        op->umsg.lpBuffers = &op->ubuf;
        op->umsg.dwBufferCount = 1;
        op->umsg.Control.len = (ULONG)sizeof(op->uctrl);
        op->umsg.Control.buf = op->uctrl;
        op->umsg.dwFlags = 0;
        DWORD recvd = 0;
        int rc = recvmsg((SOCKET)rop->fd, &op->umsg, &recvd, &op->ov, NULL);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            iocp_op_free(op);                  /* life unset → no release */
            return -1;
        }
        op->life = rop->life;  /* TRANSFERRED into the op (no retain) */
        KLIT("post_dgram_recv (recvmsg) op=%p sock=%llu life=%p\n", (void *)op,
             (unsigned long long)op->op_sock, (void *)op->life);
        return 0;
    }

    /* Fallback: no WSARecvMsg extension — source address only. */
    WSABUF buf = { (ULONG)op->buflen, (char *)op->buf };
    DWORD flags = 0, recvd = 0;
    int rc = WSARecvFrom((SOCKET)rop->fd, &buf, 1, &recvd, &flags,
                         (struct sockaddr *)&op->src, &op->src_len, &op->ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_op_free(op);                      /* life unset → no release */
        return -1;
    }
    op->life = rop->life;      /* TRANSFERRED into the op (no retain) */
    KLIT("post_dgram_recv (recvfrom) op=%p sock=%llu life=%p\n", (void *)op,
         (unsigned long long)op->op_sock, (void *)op->life);
    return 0;
}

/* Post one overlapped WSASendTo for a UDP socket (8b-4d). Copies the datagram + its
 * destination into the op (owned until completion); surfaces KL_COMP_DGRAM_SEND. */
static int iocp_comp_post_dgram_send(struct KlEventCtx *ctx, const KlDgramSendOp *sop) {
    KlIocpState *st = ctx->loop._backend;
    KlIocpOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;                 /* nothing taken → caller releases its ref */
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_DGRAM_SEND;
    op->alloc = st->alloc;
    op->send_total = sop->len;
    iocp_op_register(st, op, (SOCKET)sop->fd);

    op->sendbuf = kl_malloc(st->alloc, sop->len ? sop->len : 1);
    if (!op->sendbuf) { op->send_total = 0; iocp_op_free(op); return -1; }  /* life unset → caller releases */
    memcpy(op->sendbuf, sop->data, sop->len);   /* COPY payload before accept */
    /* Marshal the neutral dest to a Winsock sockaddr for the overlapped WSASendTo. */
    if (sop->dest && kl_sockaddr_family(sop->dest) != KL_AF_UNSPEC)
        op->src_len = (int)kl_sockaddr_to_native(sop->dest, &op->src);

    WSABUF buf = { (ULONG)sop->len, op->sendbuf };
    DWORD sent = 0;
    int rc = WSASendTo((SOCKET)sop->fd, &buf, 1, &sent, 0,
                       (struct sockaddr *)&op->src, op->src_len, &op->ov, NULL);
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        iocp_op_free(op);                      /* life unset → caller releases */
        return -1;
    }
    op->life = sop->life;               /* TRANSFERRED into the op (no retain) */
    KLIT("post_dgram_send op=%p sock=%llu life=%p len=%zu\n", (void *)op,
         (unsigned long long)op->op_sock, (void *)op->life, (size_t)op->send_total);
    return 0;
}

/* Cancel the outstanding datagram op(s) of `kind` for `life` (7B-2): CancelIoEx the matching
 * overlapped(s) — the forced ERROR_OPERATION_ABORTED completion drains + releases the token ref (the
 * same mechanism the per-fd cancel relies on). Idempotent; NO ref release here. */
static int iocp_comp_cancel_dgram(struct KlEventCtx *ctx, KlDgramLife *life, KlDgramOpKind kind) {
    KlIocpState *st = ctx->loop._backend;
    KlIocpOpType want = (kind == KL_DGRAM_OP_SEND) ? KL_IOCP_DGRAM_SEND : KL_IOCP_DGRAM_RECV;
    for (KlIocpOp *o = st->ops; o; o = o->g_next)
        if (o->life == life && o->type == want)
            CancelIoEx((HANDLE)(uintptr_t)o->op_sock, &o->ov);
    return 0;
}

/* Classify retirement (§4.3): a matching op still in the global registry is PENDING (its aborted
 * completion has not yet drained + released); none tracked means it physically retired. IOCP never
 * quarantines — a posted overlapped always yields a completion the drain reaps. */
static KlDgramRetireResult iocp_comp_retire_dgram(struct KlEventCtx *ctx, KlDgramLife *life,
                                                  KlDgramOpKind kind, int *transport_err) {
    KlIocpState *st = ctx->loop._backend;
    KlIocpOpType want = (kind == KL_DGRAM_OP_SEND) ? KL_IOCP_DGRAM_SEND : KL_IOCP_DGRAM_RECV;
    if (transport_err) *transport_err = 0;
    for (const KlIocpOp *o = st->ops; o; o = o->g_next)
        if (o->life == life && o->type == want) return KL_DGRAM_RETIRE_PENDING;
    return KL_DGRAM_RETIRE_RETIRED;
}

/* Fetch the ConnectEx extension pointer for `s` (per-socket WSAIoctl, as MSDN requires). */
static LPFN_CONNECTEX iocp_get_connectex(SOCKET s) {
    LPFN_CONNECTEX fn = NULL;
    GUID guid = WSAID_CONNECTEX;
    DWORD b = 0;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                 &fn, sizeof(fn), &b, NULL, NULL) == SOCKET_ERROR)
        return NULL;
    return fn;
}

/* Post an outbound connect via ConnectEx (LC-0). ConnectEx requires: (1) the socket bound
 * (to the wildcard address of the dest's family), (2) the socket associated with the IOCP
 * port so its completion is delivered, and (3) SO_UPDATE_CONNECT_CONTEXT set after success
 * (done in the drain). The dest is marshalled into the op's `src` storage; the completion
 * surfaces KL_COMP_CONNECT against the client's tagged watcher (he_on_writable re-checks
 * SO_ERROR, which is valid after SO_UPDATE_CONNECT_CONTEXT). Returns 0 posted, -1 on failure. */
static int iocp_comp_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                                  const KlSockAddr *addr, void *watcher_udata) {
    KlIocpState *st = ctx->loop._backend;
    SOCKET s = (SOCKET)fd;

    struct sockaddr_storage dst;
    int dstlen = (addr && kl_sockaddr_family(addr) != KL_AF_UNSPEC)
                     ? (int)kl_sockaddr_to_native(addr, &dst) : 0;
    if (dstlen == 0) return -1;

    /* ConnectEx requires an explicitly-bound socket. Bind to the wildcard addr of the same
     * family (port 0 = ephemeral). */
    struct sockaddr_storage any;
    memset(&any, 0, sizeof(any));
    any.ss_family = dst.ss_family;
    int anylen = (dst.ss_family == AF_INET6)
                     ? (int)sizeof(struct sockaddr_in6) : (int)sizeof(struct sockaddr_in);
    if (bind(s, (struct sockaddr *)&any, anylen) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEINVAL)   /* WSAEINVAL = already bound — fine */
        return -1;

    LPFN_CONNECTEX connectex = iocp_get_connectex(s);
    if (!connectex) return -1;

    /* Associate the connecting socket with the port (untagged key is fine — dispatch is by
     * OVERLAPPED/CONTAINING_RECORD, not key; use the watcher for symmetry). */
    if (!CreateIoCompletionPort((HANDLE)(uintptr_t)s, st->port,
                                (ULONG_PTR)watcher_udata, 0))
        return -1;

    KlIocpOp *op = kl_malloc(st->alloc, sizeof(*op));
    if (!op) return -1;
    memset(op, 0, sizeof(*op));
    op->type = KL_IOCP_CONNECT;
    op->alloc = st->alloc;
    op->accept_sock = s;                 /* the connecting socket */
    op->watcher_udata = watcher_udata;
    memcpy(&op->src, &dst, (size_t)dstlen);
    op->src_len = dstlen;
    iocp_op_register(st, op, s);         /* ConnectEx overlapped runs on the connecting socket */

    /* Track the op so a later iocp_comp_cancel(fd) can mark it aborted. */
    KlIocpConnect *tr = kl_malloc(st->alloc, sizeof(*tr));
    if (!tr) { iocp_op_free(op); return -1; }
    tr->fd = s;
    tr->op = op;
    tr->next = st->connects;
    st->connects = tr;

    DWORD sent = 0;
    BOOL ok = connectex(s, (struct sockaddr *)&op->src, dstlen, NULL, 0, &sent, &op->ov);
    if (!ok && WSAGetLastError() != WSA_IO_PENDING) {
        st->connects = tr->next;
        kl_free(st->alloc, tr, sizeof(*tr));
        iocp_op_free(op);
        return -1;
    }
    return 0;
}

/* Unlink + free the connect tracker for `op` (if present). Returns whether it was aborted. */
static int iocp_connect_untrack(KlIocpState *st, const KlIocpOp *op) {
    int aborted = 0;
    for (KlIocpConnect **link = &st->connects; *link; link = &(*link)->next)
        if ((*link)->op == op) {
            KlIocpConnect *tr = *link;
            *link = tr->next;
            aborted = op->watcher_removed;   /* set by iocp_comp_cancel */
            kl_free(st->alloc, tr, sizeof(*tr));
            break;
        }
    return aborted;
}

/* First-drain setup: load the extension fn pointers off the listen socket, learn
 * its address family, and store it. Idempotent — latches on st->started so the server
 * may call it every tick. Server-scoped (needs the listen socket); keeps kl_comp_drain
 * ctx-scoped/server-agnostic. Post-driven (6B-3 2b-ii): returns the AcceptEx window
 * (KL_IOCP_ACCEPT_BACKLOG) and posts NOTHING — the completion KlListener posts that many
 * AcceptEx ops, one reserved pool credit each, and replenishes one per accept completion. */
static int iocp_comp_prime_accepts(struct KlServer *s) {
    if (!s) return -1;
    KlIocpState *st = s->ev.loop._backend;
    if (st->started)
        return KL_IOCP_ACCEPT_BACKLOG;

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
    return KL_IOCP_ACCEPT_BACKLOG;   /* window; the listener posts the AcceptEx ops (6B-3 2b-ii) */
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
static void iocp_comp_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    KlIocpState *st = ctx->loop._backend;
    /* A tracked ConnectEx op on this fd (LC-0): mark it aborted so its (CancelIoEx-forced)
     * completion is DROPPED in the drain — the client frees the detached watcher + closes the
     * socket right after this, so the completion must not dispatch against freed memory. */
    for (KlIocpConnect *tr = st->connects; tr; tr = tr->next)
        if (tr->fd == (SOCKET)fd) tr->op->watcher_removed = 1;
    CancelIoEx((HANDLE)(uintptr_t)fd, NULL);
}

/* Source (peer) of a completed datagram receive: WSARecvMsg fills umsg.namelen, WSARecvFrom fills
 * src_len. Filled by the kernel even for a ZERO-BYTE datagram, so parse it independent of byte count. */
static void iocp_dgram_parse_source(KlIocpOp *op, KlCompletionEvent *ev) {
    int slen = op->via_recvmsg ? (int)op->umsg.namelen : op->src_len;
    if (slen > 0)
        (void)kl_sockaddr_from_native(&ev->peer, (struct sockaddr *)&op->src, (socklen_t)slen);
}
/* Local (destination) address from the pktinfo control message — WSARecvMsg path only. */
static void iocp_dgram_parse_local(KlIocpOp *op, KlCompletionEvent *ev) {
    if (op->via_recvmsg && op->dg_pktinfo) {
        struct sockaddr_storage local_ss;
        socklen_t local_len = kl_udp_win_parse_local(&op->umsg, &local_ss);
        if (local_len)
            (void)kl_sockaddr_from_native(&ev->local, (struct sockaddr *)&local_ss, local_len);
    }
}

static int iocp_comp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
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
            /* Whether this AcceptEx actually succeeded — a cancelled one (CancelIoEx on the listen
             * socket at listener close, 6B-3 2b-ii) completes here with an error status. On failure,
             * close the PRE-created accept socket (AcceptEx allocates it up front — no leak) and
             * deliver ok=0 so the completion listener retires this posted accept (returns its
             * credit). The overlapped is associated with the listen socket, so query it there. */
            iocp_accept_untrack(st, op);   /* completing — no longer close's responsibility */
            DWORD xfer = 0, flags = 0;
            if (!WSAGetOverlappedResult(lst, &op->ov, &xfer, FALSE, &flags)) {
                closesocket(op->accept_sock);   /* pre-created socket never handed off — close it */
                memset(&out[count], 0, sizeof(out[count]));
                out[count].kind = KL_COMP_ACCEPT;
                out[count].target = NULL;
                out[count].ok = 0;
                count++;
                iocp_op_free(op);
                continue;
            }
            setsockopt(op->accept_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                       (char *)&lst, sizeof(lst));
            struct sockaddr *local = NULL, *remote = NULL;
            int llen = 0, rlen = 0;
            st->get_sockaddrs(op->accept_buf, 0, (DWORD)KL_IOCP_ADDR_LEN,
                              (DWORD)KL_IOCP_ADDR_LEN, &local, &llen, &remote, &rlen);
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_ACCEPT;
            out[count].target = NULL;   /* ACCEPT: server recovered from ctx at dispatch (6B-3) */
            out[count].ok = 1;
            out[count].accepted_fd = (KlSocketHandle)op->accept_sock;
            if (rlen > 0 && remote)              /* native → neutral once, at the seam */
                (void)kl_sockaddr_from_native(&out[count].peer, remote,
                                              (socklen_t)rlen);
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_READ) {
            /* Raw bytes landed in the caller's buffer. For a TLS conn that buffer is the
             * HTTP adapter's per-conn ciphertext scratch; the adapter (comp_on_read) feeds
             * it to the engine. The backend has no TLS knowledge. */
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_READ;
            out[count].target = op->stream;
            out[count].bytes = bytes;
            out[count].ok = 1;
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_WRITE) {
            op->send_done += bytes;
            if (st->quiescing) { iocp_op_free(op); continue; }   /* teardown: no re-post (2b review) */
            if (bytes > 0 && op->send_done < op->send_total) {
                /* Partial send — re-post the remainder; do NOT surface an event
                 * until the whole response is out (the driver sees full writes). */
                WSABUF buf = { (ULONG)(op->send_total - op->send_done),
                               op->sendbuf + op->send_done };
                DWORD sent = 0;
                int rc = WSASend((SOCKET)op->stream->fd, &buf, 1, &sent, 0, &op->ov, NULL);
                if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                    memset(&out[count], 0, sizeof(out[count]));
                    out[count].kind = KL_COMP_WRITE;
                    out[count].target = op->stream;
                    out[count].ok = 0;
                    count++;
                    iocp_op_free(op);
                }
                continue;
            }
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_WRITE;
            out[count].target = op->stream;
            out[count].bytes = bytes;
            out[count].ok = (bytes > 0);
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_DGRAM_RECV) {
            /* Teardown: a recv cancelled by iocp_quiesce_port_for_close completes here (ABORTED).
             * Drop it — the teardown reaper discards non-accept events anyway — and avoid touching a
             * socket that is about to be closed. */
            if (st->quiescing) { iocp_op_free(op); continue; }

            /* The buffer + flags were COPIED at post; the token ref pins the buffer past this op.
             * Transfer the token ref op → event (released after dispatch); NULL op->life so
             * iocp_op_free does not double-release. Never dereference KlUdpTransport. */
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_DGRAM_RECV;
            out[count].life = op->life; op->life = NULL;
            out[count].buf = op->buf;

            /* Classify the completed overlapped recv EXPLICITLY via the actual operation result (do
             * NOT assume success by byte count): a zero-length datagram is a real receive; a truncated
             * one is a captured-prefix success; a cancelled/failed one delivers nothing. The decision
             * itself is platform-agnostic + unit-tested (dgram_recv_classify.h); here we supply the
             * Winsock result and then parse the metadata it says is valid. */
            DWORD xfer = 0, wflags = 0;
            BOOL wok = WSAGetOverlappedResult(op->op_sock, &op->ov, &xfer, FALSE, &wflags);
            unsigned mflags = op->via_recvmsg ? op->umsg.dwFlags : wflags;
            KlDgramRecvClass cl = kl_dgram_recv_classify(wok, wok ? 0 : WSAGetLastError(), WSAEMSGSIZE,
                                                         (size_t)xfer, mflags, MSG_TRUNC,
                                                         op->buflen);
            out[count].ok = cl.ok;
            if (cl.ok) {
                out[count].bytes = (DWORD)cl.bytes;
                out[count].truncated = cl.truncated;
                if (cl.parse_source) iocp_dgram_parse_source(op, &out[count]);
                if (cl.parse_local)  iocp_dgram_parse_local(op, &out[count]);
            }
            count++;
            iocp_op_free(op);
        } else if (op->type == KL_IOCP_DGRAM_SEND) {
            /* Whole datagram send done — surface the original len so the driver
             * releases exactly the reserved outstanding bytes (success or not). Transfer the token
             * ref op → event (released after dispatch); NULL op->life so iocp_op_free does not
             * double-release. */
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_DGRAM_SEND;
            out[count].life = op->life; op->life = NULL;
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
            /* Teardown (6B-3 2b review): do NOT re-post — a fresh WSARecv would leave this op
             * kernel-owned when kl_event_close_builtin frees it (use-after-free). This completion
             * dequeued the op, so it is no longer kernel-owned: unlink it from st->watches and free
             * it now. Not surfaced (the teardown reaper drops non-accept events anyway). */
            if (st->quiescing) {
                for (KlIocpWatch **l = &st->watches; *l; l = &(*l)->next)
                    if ((*l)->op == op) {
                        KlIocpWatch *w = *l; *l = w->next; kl_free(st->alloc, w, sizeof(*w)); break;
                    }
                iocp_op_free(op);
                continue;
            }
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_WATCHER;
            out[count].target = op->watcher_udata;
            out[count].bytes = (size_t)KL_EVENT_READ;
            out[count].ok = 1;
            count++;
            if (iocp_watch_post(op) < 0)
                op->watcher_removed = 1;   /* re-post failed — freed at kl_event_close */
        } else if (op->type == KL_IOCP_CONNECT) {
            /* ConnectEx finished (LC-0). Untrack it; if the client aborted the attempt
             * (watcher_removed, set by iocp_comp_cancel) drop it silently — the detached
             * watcher is gone. Otherwise apply SO_UPDATE_CONNECT_CONTEXT (required after
             * ConnectEx so getsockopt/shutdown work) and surface KL_COMP_CONNECT against the
             * client's tagged watcher; he_on_writable re-checks SO_ERROR. A non-successful
             * overlapped completion (ok field) also reports via SO_ERROR to the client. */
            int aborted = iocp_connect_untrack(st, op);
            if (aborted) { iocp_op_free(op); continue; }
            SOCKET cs = op->accept_sock;
            /* ConnectEx signals failure via the overlapped completion status (bytes==0 +
             * error); on success apply SO_UPDATE_CONNECT_CONTEXT. Decide the result from the
             * OVERLAPPED status (GetOverlappedResult) — SO_ERROR is checked as a backstop.
             * Encode it in the delivered mask (KL_EVENT_WRITE = connected, 0 = failed); the
             * client's connect watcher trusts the mask, uniform with io_uring. */
            DWORD xfer = 0, flags = 0;
            BOOL cok = WSAGetOverlappedResult(cs, &op->ov, &xfer, FALSE, &flags);
            int connected = cok ? 1 : 0;
            if (connected) {
                setsockopt(cs, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
                int soerr = 0, slen = (int)sizeof(soerr);
                if (getsockopt(cs, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen) == 0 && soerr != 0)
                    connected = 0;
            }
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_CONNECT;
            out[count].target = op->watcher_udata;
            out[count].ok = connected;
            out[count].bytes = connected ? (size_t)KL_EVENT_WRITE : 0;
            count++;
            iocp_op_free(op);
        } else { /* KL_IOCP_SENDFILE — one chunk of a (possibly multi-chunk) TransmitFile. */
            op->file_done += op->file_chunk;
            if (st->quiescing) { iocp_op_free(op); continue; }   /* teardown: no re-post (2b review) */
            if (bytes > 0 && op->file_done < op->file_total) {
                /* More file to send — re-post the next offset-advancing chunk (file-only);
                 * do NOT surface until the whole head+file is out (the driver sees full
                 * writes). On a re-post error, surface a failed write so the conn closes. */
                if (iocp_post_transmitfile_chunk(op) < 0) {
                    memset(&out[count], 0, sizeof(out[count]));
                    out[count].kind = KL_COMP_WRITE;
                    out[count].target = op->stream;
                    out[count].ok = 0;
                    count++;
                    iocp_op_free(op);
                }
                continue;
            }
            /* Whole file out (or a failed/short completion) — surface the completed write. */
            memset(&out[count], 0, sizeof(out[count]));
            out[count].kind = KL_COMP_WRITE;
            out[count].target = op->stream;
            out[count].bytes = bytes;
            out[count].ok = (bytes > 0);
            count++;
            iocp_op_free(op);
        }
    }
    return count;
}

/* Cancel + DEQUEUE every tracked outstanding overlapped op, then free it — so kl_event_close_builtin
 * never frees an OVERLAPPED the kernel still owns (6B-3 2b review). CancelIoEx forces each pending
 * watcher/connect I/O to complete; the listen socket is already closed so AcceptEx are completing
 * too. Then drain the port until the tracked lists empty, freeing each op AFTER its completion is
 * dequeued. Untracked conn read/write completions that happen to arrive are freed too (dequeued →
 * safe). Terminating: cancelled/closed I/O always completes → posts → is dequeued. On a port error
 * it returns with lists non-empty; the caller then frees the tracker nodes but leaks those op
 * records (never freeing an un-dequeued OVERLAPPED). No dispatch/callbacks — pure teardown. */
static void iocp_quiesce_port_for_close(KlIocpState *st) {
    KLIT("quiesce ENTER\n"); KLIT_DUMP(st);
    /* Cancel EVERY outstanding op (the global registry covers conn READ/WRITE/SENDFILE + UDP too,
     * not just watch/connect/accept) on its owning socket, so all their completions post. The
     * conn/udp sockets are still open at this point (kl_conn_pool_free runs after), so CancelIoEx
     * reaches them; the listen socket is already closed (its AcceptEx are completing regardless). */
    for (KlIocpOp *op = st->ops; op; op = op->g_next) {
        BOOL cr = CancelIoEx((HANDLE)(uintptr_t)op->op_sock, &op->ov);
        (void)cr;   /* used only by the trace below (no-op when the flag is off) */
        KLIT("cancel op=%p type=%s sock=%llu -> %d gle=%lu\n", (void *)op, klit_type(op->type),
             (unsigned long long)op->op_sock, (int)cr, (unsigned long)GetLastError());
    }
    /* Dequeue every completion before freeing its op — no OVERLAPPED freed while kernel-owned.
     * Terminating: cancelled/closed I/O always completes → posts. Per-type teardown cleanup
     * (free the tracker node, close a pre-created accept socket) then free the op (which unlinks it
     * from the registry). No dispatch/callbacks. */
    while (st->ops) {
        OVERLAPPED_ENTRY entries[KL_IOCP_ACCEPT_BACKLOG];
        ULONG got = 0;
        DWORD wait_ms = INFINITE;   /* PRODUCTION: block until every op's completion is reaped */
#ifdef KL_IOCP_TEARDOWN_TRACE
        /* DIAGNOSTIC ONLY: bound the wait so a never-arriving completion DUMPS the registry and
         * aborts (fails the smoke) instead of hanging silently. NOT a production behaviour change. */
        static int klit_stalls = 0;
        wait_ms = 3000;
        KLIT("quiesce: about to wait (%lu ms); remaining registry:\n", (unsigned long)wait_ms);
        klit_dump_ops(st);
#endif
        BOOL wok = GetQueuedCompletionStatusEx(st->port, entries,
                                         (ULONG)(sizeof entries / sizeof entries[0]),
                                         &got, wait_ms, FALSE);
        if (!wok) {
#ifdef KL_IOCP_TEARDOWN_TRACE
            DWORD gle = GetLastError();
            if (gle == WAIT_TIMEOUT) {
                KLIT("quiesce: WAIT_TIMEOUT with st->ops non-empty (stall %d)\n", ++klit_stalls);
                klit_dump_ops(st);
                if (klit_stalls >= 2) { KLIT("quiesce STUCK — no completion for the above op(s); aborting for evidence\n"); abort(); }
                continue;   /* retry the (bounded) wait once more before aborting */
            }
            KLIT("quiesce: GQCS failed gle=%lu (fatal)\n", (unsigned long)gle);
#endif
            return;   /* fatal port error — caller frees trackers + leaks the un-dequeued ops */
        }
        for (ULONG i = 0; i < got; i++) {
            KlIocpOp *op = CONTAINING_RECORD(entries[i].lpOverlapped, KlIocpOp, ov);
            KLIT("dequeued op=%p type=%s sock=%llu bytes=%lu\n", (void *)op, klit_type(op->type),
                 (unsigned long long)op->op_sock, (unsigned long)entries[i].dwNumberOfBytesTransferred);
            if (op->type == KL_IOCP_WATCHER) {
                for (KlIocpWatch **l = &st->watches; *l; l = &(*l)->next)
                    if ((*l)->op == op) {
                        KlIocpWatch *w = *l; *l = w->next; kl_free(st->alloc, w, sizeof(*w)); break;
                    }
            } else if (op->type == KL_IOCP_CONNECT) {
                iocp_connect_untrack(st, op);
            } else if (op->type == KL_IOCP_ACCEPT) {
                iocp_accept_untrack(st, op);
                closesocket(op->accept_sock);   /* aborted → never handed off */
            }
            iocp_op_free(op);                   /* unlinks from the global registry */
        }
    }
}

/* Teardown accept-side force-completion (6B-3 2b review). Does NOT reap. The server closed the
 * listen socket BEFORE calling this, and closesocket cancels every pending overlapped AcceptEx on
 * it — so all posted accepts are already completing (their completions will post to the port). The
 * server then drives the reap through the NORMAL path (kl_comp_run → iocp_comp_drain), which
 * DEQUEUES every completion (accept OR read/write/udp/watcher) before freeing it (no OVERLAPPED is
 * freed while the kernel may still be writing → no use-after-free) and routes each ordinarily (no
 * completion lost); accept completions untrack + retire the listener. Nothing more to force here. */
static int iocp_shutdown_accepts(struct KlServer *s) {
    KlIocpState *st = s->ev.loop._backend;
    st->quiescing = 1;   /* from now, iocp_comp_drain frees fired watchers instead of re-posting */
    return 0;
}

/* ── Completion sub-vtable (RC-1) ─────────────────────────────────────────
 * Group this backend's completion primitives so the dispatch (completion_dispatch.c)
 * reaches them on the compiled-in path (kl_comp_ops_builtin) or through a runtime
 * provider (loop->ops->completion). No behavior change — same funcs, one hop away. */
static const KlCompletionOps iocp_completion_ops = {
    iocp_comp_drain, iocp_comp_prime_accepts, iocp_comp_post_recv, iocp_comp_post_send,
    iocp_comp_post_accept, iocp_comp_post_sendfile, iocp_comp_cancel,
    iocp_comp_post_dgram_recv, iocp_comp_post_dgram_send,
    iocp_comp_cancel_dgram, iocp_comp_retire_dgram, iocp_comp_post_connect,
    iocp_shutdown_accepts,
};

const KlCompletionOps *kl_comp_ops_builtin(void) { return &iocp_completion_ops; }
