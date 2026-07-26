/*
 * socket_winsock.c — the Winsock socket provider (implements the socket.h seam).
 *
 * The Windows sibling of socket_posix.c: one provider per TU, compiled only on
 * Windows (Makefile OS=windows branch), so it uses Winsock directly with no
 * internal #ifdef. Defines the platform-default socket ops (kl_sockdef_*) the
 * neutral socket.h seam calls, plus the Winsock KlSocketProvider. See
 * docs/phase6_winsock_design.md §B.1.
 *
 * A SOCKET is pointer-width (UINT_PTR); it round-trips through KlSocketHandle
 * (intptr_t). INVALID_SOCKET == (SOCKET)(~0) == -1 reinterpreted, so it maps to
 * KL_INVALID_SOCKET. Winsock ops return 0 / SOCKET_ERROR(-1), matching the
 * seam's POSIX 0/-1 contract. Windows has no SIGPIPE (no MSG_NOSIGNAL needed)
 * and blocking Winsock calls don't return WSAEINTR (no EINTR loop needed).
 */

#include "socket.h"

#include <windows.h>
#include <mswsock.h>   /* TransmitFile prototype (linked via -lws2_32 / mswsock) */
#include <io.h>        /* _read, _lseeki64, _get_osfhandle */
#include <limits.h>
#include <errno.h>

#define KL_WSK_SENDFILE_BUF 8192

/* Winsock must be started before ANY socket call. The provider factory does it
 * too (refcounted), but the default path — kl_sockdef_* via a NULL provider —
 * calls socket()/connect() directly, so without this an unconfigured process
 * would get WSANOTINITIALISED. A library constructor initializes it once at
 * load (socket_winsock.o is always linked in on Windows); WSAStartup is
 * refcounted, so this composes with the factory's own WSAStartup/WSACleanup. */
__attribute__((constructor))
static void kl_winsock_global_init(void) {
    WSADATA wsa;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
}
__attribute__((destructor))
static void kl_winsock_global_fini(void) {
    WSACleanup();
}

static int clamp_int(size_t n) {
    return n > (size_t)INT_MAX ? INT_MAX : (int)n;
}

/* Translate the last Winsock error into the CRT errno space. Winsock reports
 * failures via WSAGetLastError() and never touches the CRT errno, but the
 * seam's callers (server accept loop, client connect, connection read/write)
 * all branch on POSIX errno — EAGAIN/EWOULDBLOCK, EINPROGRESS, EINTR — exactly
 * as they do on POSIX. Without this, every non-blocking would-block/in-progress
 * check on Windows sees errno==0 and misfires (the accept loop spins logging
 * "accept: No error"; a pending connect looks like a hard failure). Reading
 * WSAGetLastError() does not clear it, so callers that additionally feed it to
 * kl_sock_errno_to_error() are unaffected. Shared (declared in sockcompat.h) so
 * the Winsock event backend (event_wsapoll.c) and kl_plat_poll1 translate the
 * same way — the server's `errno == EINTR` retry check depends on it. */
void kl_wsa_set_errno(void) {
    switch (WSAGetLastError()) {
        /* NB: on MinGW EAGAIN (11) != EWOULDBLOCK (140), unlike POSIX where they
         * are equal. Callers that test would-block must OR both codes (they do);
         * code testing EAGAIN alone would misfire on Windows. */
        case WSAEWOULDBLOCK:   errno = EWOULDBLOCK;   break;
        case WSAEINPROGRESS:
        case WSAEALREADY:      errno = EINPROGRESS;   break;
        case WSAEINTR:         errno = EINTR;         break;
        case WSAECONNRESET:    errno = ECONNRESET;    break;
        case WSAECONNABORTED:  errno = ECONNABORTED;  break;
        case WSAECONNREFUSED:  errno = ECONNREFUSED;  break;
        case WSAETIMEDOUT:     errno = ETIMEDOUT;     break;
        case WSAENOTCONN:      errno = ENOTCONN;      break;
        case WSAESHUTDOWN:     errno = EPIPE;         break;
        case WSAEHOSTUNREACH:  errno = EHOSTUNREACH;  break;
        case WSAENETUNREACH:   errno = ENETUNREACH;   break;
        case WSAENETDOWN:      errno = ENETDOWN;      break;
        case WSAENETRESET:     errno = ENETRESET;     break;
        case WSAEADDRINUSE:    errno = EADDRINUSE;    break;
        case WSAEADDRNOTAVAIL: errno = EADDRNOTAVAIL; break;
        case WSAEACCES:        errno = EACCES;        break;
        case WSAEMFILE:        errno = EMFILE;        break;
        case WSAENOBUFS:       errno = ENOBUFS;       break;
        case WSAEMSGSIZE:      errno = EMSGSIZE;      break;
        case WSAEINVAL:        errno = EINVAL;        break;
        case WSAEFAULT:        errno = EFAULT;        break;
        default:               errno = EIO;           break;
    }
}

/* ── Platform default socket ops (Winsock) ─────────────────────────────── */

int kl_sockdef_set_nonblocking(KlSocketHandle fd) {
    u_long mode = 1;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &mode) != 0) { kl_wsa_set_errno(); return -1; }
    return 0;
}

int kl_sockdef_set_blocking(KlSocketHandle fd) {
    u_long mode = 0;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &mode) != 0) { kl_wsa_set_errno(); return -1; }
    return 0;
}

void kl_sockdef_set_cloexec(KlSocketHandle fd) {
    /* Clear inherit so the socket isn't leaked into child processes — the
     * Windows analog of FD_CLOEXEC. Best-effort. */
    (void)SetHandleInformation((HANDLE)(SOCKET)fd, HANDLE_FLAG_INHERIT, 0);
}

void kl_sockdef_set_nosigpipe(KlSocketHandle fd) {
    (void)fd;   /* no SIGPIPE on Windows */
}

int kl_sockdef_set_reuseaddr(KlSocketHandle fd, int on) {
    return setsockopt((SOCKET)fd, SOL_SOCKET, SO_REUSEADDR,
                      (const char *)&on, sizeof(on)) == 0 ? 0 : -1;
}
int kl_sockdef_set_reuseport(KlSocketHandle fd, int on) {
    (void)fd; (void)on;
    return -1;   /* no SO_REUSEPORT on Windows — best-effort, caller ignores */
}
int kl_sockdef_set_ipv6only(KlSocketHandle fd, int on) {
    return setsockopt((SOCKET)fd, IPPROTO_IPV6, IPV6_V6ONLY,
                      (const char *)&on, sizeof(on)) == 0 ? 0 : -1;
}
int kl_sockdef_set_tcp_nodelay(KlSocketHandle fd, int on) {
    return setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY,
                      (const char *)&on, sizeof(on)) == 0 ? 0 : -1;
}
int kl_sockdef_set_cork(KlSocketHandle fd, int on) {
    (void)fd; (void)on;
    return -1;   /* no TCP_CORK/NOPUSH on Winsock; TransmitFile coalesces itself */
}

KlSocketHandle kl_sockdef_socket(int domain, int type, int protocol) {
    SOCKET s = socket(domain, type, protocol);
    if (s == INVALID_SOCKET) kl_wsa_set_errno();
    return (KlSocketHandle)s;
}
int kl_sockdef_connect(KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    if (connect((SOCKET)fd, a, l) == SOCKET_ERROR) {
        /* A non-blocking connect that can't finish immediately reports
         * WSAEWOULDBLOCK on Winsock, but POSIX callers expect EINPROGRESS. */
        if (WSAGetLastError() == WSAEWOULDBLOCK) errno = EINPROGRESS;
        else kl_wsa_set_errno();
        return -1;
    }
    return 0;
}
int kl_sockdef_bind(KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    if (bind((SOCKET)fd, a, l) == SOCKET_ERROR) { kl_wsa_set_errno(); return -1; }
    return 0;
}
int kl_sockdef_listen(KlSocketHandle fd, int backlog) {
    if (listen((SOCKET)fd, backlog) == SOCKET_ERROR) { kl_wsa_set_errno(); return -1; }
    return 0;
}
KlSocketHandle kl_sockdef_accept(KlSocketHandle fd, struct sockaddr *a, socklen_t *l) {
    SOCKET c = accept((SOCKET)fd, a, l);
    if (c == INVALID_SOCKET) kl_wsa_set_errno();
    return (KlSocketHandle)c;
}
int kl_sockdef_close(KlSocketHandle fd) {
    return closesocket((SOCKET)fd);
}
int kl_sockdef_get_local_addr(KlSocketHandle fd, struct sockaddr *a, socklen_t *l) {
    if (getsockname((SOCKET)fd, a, l) != 0) { kl_wsa_set_errno(); return -1; }
    return 0;
}
int kl_sockdef_get_so_error(KlSocketHandle fd, int *out_err) {
    int err = 0;
    int len = sizeof(err);   /* Winsock getsockopt optlen is int*, optval char* */
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0) {
        kl_wsa_set_errno();
        return -1;
    }
    *out_err = err;   /* WSA* error code — callers only test zero vs non-zero */
    return 0;
}

ssize_t kl_sockdef_send(KlSocketHandle fd, const void *buf, size_t len) {
    int r = send((SOCKET)fd, (const char *)buf, clamp_int(len), 0);
    if (r == SOCKET_ERROR) { kl_wsa_set_errno(); return -1; }
    return r;
}
ssize_t kl_sockdef_recv(KlSocketHandle fd, void *buf, size_t len) {
    int r = recv((SOCKET)fd, (char *)buf, clamp_int(len), 0);
    if (r == SOCKET_ERROR) { kl_wsa_set_errno(); return -1; }
    return r;
}
ssize_t kl_sockdef_recv_peek(KlSocketHandle fd, void *buf, size_t len) {
    int r = recv((SOCKET)fd, (char *)buf, clamp_int(len), MSG_PEEK);
    if (r == SOCKET_ERROR) { kl_wsa_set_errno(); return -1; }
    return r;
}

/* Vectored write via WSASend. KlIoVec (base,len) -> WSABUF (len,buf) — WSABUF
 * stays inside this provider TU. */
ssize_t kl_sockdef_writev(KlSocketHandle fd, const KlIoVec *iov, int iovcnt) {
    if (iovcnt <= 0)
        return 0;
    WSABUF stackbufs[KL_SOCK_IOV_MAX];
    WSABUF *bufs = stackbufs;
    if (iovcnt > (int)(sizeof(stackbufs) / sizeof(stackbufs[0]))) {
        /* Only the response header+body vectors reach here (<= a few, capped at
         * iov[7] by response.c). Fail loud rather than silently dropping the
         * overflow vectors, which would truncate/corrupt the payload. */
        errno = EINVAL;
        return -1;
    }
    for (int i = 0; i < iovcnt; i++) {
        bufs[i].len = (ULONG)iov[i].len;
        bufs[i].buf = (CHAR *)iov[i].base;
    }
    DWORD sent = 0;
    int rc = WSASend((SOCKET)fd, bufs, (DWORD)iovcnt, &sent, 0, NULL, NULL);
    if (rc == SOCKET_ERROR) {
        kl_wsa_set_errno();
        return -1;
    }
    return (ssize_t)sent;
}

/* No TransmitFile yet (an offset-aware OVERLAPPED optimization for 6b); a
 * pread+send loop is correct and cross-compiles. Sends one chunk per call from
 * *offset, advancing it — same one-chunk-per-tick shape as the POSIX fallback. */
ssize_t kl_sockdef_sendfile(KlSocketHandle out_fd, int in_fd, uint64_t *offset, size_t count) {
    char buf[KL_WSK_SENDFILE_BUF];
    size_t to_read = count < sizeof(buf) ? count : sizeof(buf);
    if (_lseeki64(in_fd, (long long)*offset, SEEK_SET) < 0)
        return -1;
    int nr = _read(in_fd, buf, clamp_int(to_read));
    if (nr <= 0)
        return nr;
    const char *p = buf;
    int remaining = nr;
    while (remaining > 0) {
        int nw = send((SOCKET)out_fd, p, remaining, 0);
        if (nw == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                int wrote = nr - remaining;
                *offset += wrote;
                if (wrote > 0) return wrote;
                errno = EWOULDBLOCK;
                return -1;
            }
            kl_wsa_set_errno();
            return -1;
        }
        p += nw;
        remaining -= nw;
    }
    *offset += nr;
    return nr;
}

/* ── Winsock provider — thin adapters over the kl_sockdef_* defaults ────── */

static int wsk_set_nonblocking(void *ctx, KlSocketHandle fd) {
    (void)ctx; return kl_sockdef_set_nonblocking(fd);
}
static int wsk_set_blocking(void *ctx, KlSocketHandle fd) {
    (void)ctx; return kl_sockdef_set_blocking(fd);
}
static void wsk_set_cloexec(void *ctx, KlSocketHandle fd) {
    (void)ctx; kl_sockdef_set_cloexec(fd);
}
static void wsk_set_nosigpipe(void *ctx, KlSocketHandle fd) {
    (void)ctx; kl_sockdef_set_nosigpipe(fd);
}
static int wsk_set_reuseaddr(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_reuseaddr(fd, on);
}
static int wsk_set_reuseport(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_reuseport(fd, on);
}
static int wsk_set_ipv6only(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_ipv6only(fd, on);
}
static int wsk_set_tcp_nodelay(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_tcp_nodelay(fd, on);
}
static int wsk_set_cork(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_cork(fd, on);
}
static KlSocketHandle wsk_socket(void *ctx, int domain, int type, int protocol) {
    (void)ctx; return kl_sockdef_socket(domain, type, protocol);
}
static int wsk_connect(void *ctx, KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return kl_sockdef_connect(fd, a, l);
}
static int wsk_bind(void *ctx, KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return kl_sockdef_bind(fd, a, l);
}
static int wsk_listen(void *ctx, KlSocketHandle fd, int backlog) {
    (void)ctx; return kl_sockdef_listen(fd, backlog);
}
static KlSocketHandle wsk_accept(void *ctx, KlSocketHandle fd, struct sockaddr *a, socklen_t *l) {
    (void)ctx; return kl_sockdef_accept(fd, a, l);
}
static int wsk_close(void *ctx, KlSocketHandle fd) {
    (void)ctx; return kl_sockdef_close(fd);
}
static int wsk_get_local_addr(void *ctx, KlSocketHandle fd, struct sockaddr *a, socklen_t *l) {
    (void)ctx; return kl_sockdef_get_local_addr(fd, a, l);
}
static int wsk_get_so_error(void *ctx, KlSocketHandle fd, int *out_err) {
    (void)ctx; return kl_sockdef_get_so_error(fd, out_err);
}
static ssize_t wsk_send(void *ctx, KlSocketHandle fd, const void *buf, size_t len) {
    (void)ctx; return kl_sockdef_send(fd, buf, len);
}
static ssize_t wsk_recv(void *ctx, KlSocketHandle fd, void *buf, size_t len) {
    (void)ctx; return kl_sockdef_recv(fd, buf, len);
}
static ssize_t wsk_recv_peek(void *ctx, KlSocketHandle fd, void *buf, size_t len) {
    (void)ctx; return kl_sockdef_recv_peek(fd, buf, len);
}
static ssize_t wsk_writev(void *ctx, KlSocketHandle fd, const KlIoVec *iov, int iovcnt) {
    (void)ctx; return kl_sockdef_writev(fd, iov, iovcnt);
}
static ssize_t wsk_sendfile(void *ctx, KlSocketHandle out_fd, int in_fd, uint64_t *offset, size_t count) {
    (void)ctx; return kl_sockdef_sendfile(out_fd, in_fd, offset, count);
}
static void wsk_destroy(void *ctx) {
    (void)ctx; WSACleanup();   /* balance the WSAStartup in the factory */
}

static const KlSocketOps WINSOCK_OPS = {
    .set_nonblocking = wsk_set_nonblocking,
    .set_blocking    = wsk_set_blocking,
    .set_cloexec     = wsk_set_cloexec,
    .set_nosigpipe   = wsk_set_nosigpipe,
    .set_reuseaddr   = wsk_set_reuseaddr,
    .set_reuseport   = wsk_set_reuseport,
    .set_ipv6only    = wsk_set_ipv6only,
    .set_tcp_nodelay = wsk_set_tcp_nodelay,
    .set_cork        = wsk_set_cork,
    .socket          = wsk_socket,
    .connect         = wsk_connect,
    .bind            = wsk_bind,
    .listen          = wsk_listen,
    .accept          = wsk_accept,
    .close           = wsk_close,
    .get_local_addr  = wsk_get_local_addr,
    .get_so_error    = wsk_get_so_error,
    .send            = wsk_send,
    .recv            = wsk_recv,
    .recv_peek       = wsk_recv_peek,
    .writev          = wsk_writev,
    .sendfile        = wsk_sendfile,
    .destroy         = wsk_destroy,
    .name            = "winsock",
};

static const KlSocketProvider WINSOCK_PROVIDER = {
    &WINSOCK_OPS, NULL,
    KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_WRITEV | KL_SOCK_CAP_SENDFILE,
};

const KlSocketProvider *kl_socket_provider_winsock(void) {
    WSADATA wsa;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);   /* refcounted; WSACleanup on destroy */
    return &WINSOCK_PROVIDER;
}

/* ── Error taxonomy (Winsock) ───────────────────────────────────────────── */

KlError kl_sock_errno_to_error(int err) {
    switch (err) {
        case WSAETIMEDOUT:
            return KL_ERR_TIMEOUT;
        case WSAECONNREFUSED:
        case WSAECONNABORTED:
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
        case WSAENETDOWN:
        case WSAEHOSTDOWN:
            return KL_ERR_CONNECT;
        case WSAEADDRINUSE:
        case WSAEADDRNOTAVAIL:
        case WSAEACCES:
            return KL_ERR_BIND;
        case WSAEMFILE:
        case WSAENOBUFS:
            return KL_ERR_ALLOC;
        case WSAEOPNOTSUPP:
        case WSAEAFNOSUPPORT:
        case WSAEPROTONOSUPPORT:
        case WSAESOCKTNOSUPPORT:
        case WSAEINVAL:
        case WSAENOTSOCK:
        case WSAEFAULT:
            return KL_ERR_INVALID_ARG;
        case WSAEWOULDBLOCK:
        case WSAEINPROGRESS:
        case WSAEINTR:
        case WSAECONNRESET:
        case WSAENOTCONN:
        case WSAESHUTDOWN:
        default:
            return KL_ERR_IO;
    }
}
