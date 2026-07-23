/*
 * socket_posix.c — the POSIX socket provider (implements the socket.h seam).
 *
 * One provider per TU: sibling-to-be of socket_winsock.c / socket_lwip.c, the
 * analog of event_epoll.c/event_kqueue.c implementing the event.h interface.
 * socket.h is the platform-neutral seam (vtable + dispatchers); this file is the
 * POSIX implementation of it.
 *
 * Defines the platform-default socket ops (kl_sockdef_*) the neutral socket.h
 * seam calls when there's no provider op, plus the POSIX provider (thin adapters
 * over those defaults). No raw syscall lives in socket.h — this TU owns them for
 * POSIX. See docs/pal_transformation_design.md and docs/phase6_winsock_design.md
 * §B.0.
 */

#include "socket.h"

#include <fcntl.h>
#include <unistd.h>
#include <netinet/tcp.h>   /* IPPROTO_TCP / TCP_NODELAY */
#if defined(__linux__)
  #include <sys/sendfile.h>
#endif

#define KL_SENDFILE_BUF 8192   /* pread+write fallback chunk (no-sendfile hosts) */

/* ── Platform default socket ops (POSIX) — the kl_sockdef_* the seam calls ── */

int kl_sockdef_set_nonblocking(KlSocketHandle fd) {
    int flags = fcntl((int)fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl((int)fd, F_SETFL, flags | O_NONBLOCK);
}

void kl_sockdef_set_cloexec(KlSocketHandle fd) {
    int flags = fcntl((int)fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl((int)fd, F_SETFD, flags | FD_CLOEXEC);
}

void kl_sockdef_set_nosigpipe(KlSocketHandle fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    (void)setsockopt((int)fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

int kl_sockdef_set_reuseaddr(KlSocketHandle fd, int on) {
    return setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
}
int kl_sockdef_set_reuseport(KlSocketHandle fd, int on) {
#ifdef SO_REUSEPORT
    return setsockopt((int)fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#else
    (void)fd; (void)on;
    return -1;   /* not supported — best-effort, caller ignores */
#endif
}
int kl_sockdef_set_ipv6only(KlSocketHandle fd, int on) {
#ifdef IPV6_V6ONLY
    return setsockopt((int)fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
#else
    (void)fd; (void)on;
    return -1;
#endif
}
int kl_sockdef_set_tcp_nodelay(KlSocketHandle fd, int on) {
    return setsockopt((int)fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}
int kl_sockdef_set_cork(KlSocketHandle fd, int on) {
#if defined(TCP_CORK)
    return setsockopt((int)fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
#elif defined(TCP_NOPUSH)
    return setsockopt((int)fd, IPPROTO_TCP, TCP_NOPUSH, &on, sizeof(on));
#else
    (void)fd; (void)on;
    return -1;   /* no cork primitive — best-effort, caller ignores */
#endif
}

KlSocketHandle kl_sockdef_socket(int domain, int type, int protocol) {
    return (KlSocketHandle)socket(domain, type, protocol);
}
int kl_sockdef_connect(KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    return connect((int)fd, a, l);
}
int kl_sockdef_bind(KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    return bind((int)fd, a, l);
}
int kl_sockdef_listen(KlSocketHandle fd, int backlog) {
    return listen((int)fd, backlog);
}
KlSocketHandle kl_sockdef_accept(KlSocketHandle fd, struct sockaddr *a, socklen_t *l) {
    return (KlSocketHandle)accept((int)fd, a, l);
}
int kl_sockdef_close(KlSocketHandle fd) {
    return close((int)fd);
}
int kl_sockdef_get_local_addr(KlSocketHandle fd, struct sockaddr *a, socklen_t *l) {
    return getsockname((int)fd, a, l);
}
int kl_sockdef_get_so_error(KlSocketHandle fd, int *out_err) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
        return -1;
    *out_err = err;
    return 0;
}

ssize_t kl_sockdef_send(KlSocketHandle fd, const void *buf, size_t len) {
    ssize_t r;
#ifdef MSG_NOSIGNAL
    do { r = send((int)fd, buf, len, MSG_NOSIGNAL); } while (r < 0 && errno == EINTR);
#else
    do { r = send((int)fd, buf, len, 0); } while (r < 0 && errno == EINTR);
#endif
    return r;
}
ssize_t kl_sockdef_recv(KlSocketHandle fd, void *buf, size_t len) {
    ssize_t r;
    do { r = recv((int)fd, buf, len, 0); } while (r < 0 && errno == EINTR);
    return r;
}
ssize_t kl_sockdef_recv_peek(KlSocketHandle fd, void *buf, size_t len) {
    ssize_t r;
    do { r = recv((int)fd, buf, len, MSG_PEEK); } while (r < 0 && errno == EINTR);
    return r;
}
ssize_t kl_sockdef_writev(KlSocketHandle fd, const struct iovec *iov, int iovcnt) {
    return writev((int)fd, iov, iovcnt);
}

ssize_t kl_sockdef_sendfile(KlSocketHandle out_fd, int in_fd, off_t *offset, size_t count) {
#if defined(__linux__)
    return sendfile((int)out_fd, in_fd, offset, count);
#elif defined(__APPLE__)
    off_t len = (off_t)count;
    int r = sendfile(in_fd, (int)out_fd, *offset, &len, NULL, 0);
    if (r < 0 && errno != EAGAIN) return -1;
    *offset += len;
    return (ssize_t)len;
#else
    char buf[KL_SENDFILE_BUF];
    size_t to_read = count < sizeof(buf) ? count : sizeof(buf);
    ssize_t nr = pread(in_fd, buf, to_read, *offset);
    if (nr <= 0) return nr;
    const char *p = buf;
    size_t remaining = (size_t)nr;
    while (remaining > 0) {
        ssize_t nw = write((int)out_fd, p, remaining);
        if (nw < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ssize_t wrote = (ssize_t)((size_t)nr - remaining);
                *offset += wrote;
                return wrote > 0 ? wrote : -1;
            }
            return -1;
        }
        p += nw;
        remaining -= (size_t)nw;
    }
    *offset += nr;
    return nr;
#endif
}

/* ── POSIX provider — thin adapters over the kl_sockdef_* defaults ──────── */

static int psx_set_nonblocking(void *ctx, KlSocketHandle fd) {
    (void)ctx; return kl_sockdef_set_nonblocking(fd);
}
static void psx_set_cloexec(void *ctx, KlSocketHandle fd) {
    (void)ctx; kl_sockdef_set_cloexec(fd);
}
static void psx_set_nosigpipe(void *ctx, KlSocketHandle fd) {
    (void)ctx; kl_sockdef_set_nosigpipe(fd);
}
static int psx_set_reuseaddr(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_reuseaddr(fd, on);
}
static int psx_set_reuseport(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_reuseport(fd, on);
}
static int psx_set_ipv6only(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_ipv6only(fd, on);
}
static int psx_set_tcp_nodelay(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_tcp_nodelay(fd, on);
}
static int psx_set_cork(void *ctx, KlSocketHandle fd, int on) {
    (void)ctx; return kl_sockdef_set_cork(fd, on);
}
static KlSocketHandle psx_socket(void *ctx, int domain, int type, int protocol) {
    (void)ctx; return kl_sockdef_socket(domain, type, protocol);
}
static int psx_connect(void *ctx, KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return kl_sockdef_connect(fd, a, l);
}
static int psx_bind(void *ctx, KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return kl_sockdef_bind(fd, a, l);
}
static int psx_listen(void *ctx, KlSocketHandle fd, int backlog) {
    (void)ctx; return kl_sockdef_listen(fd, backlog);
}
static KlSocketHandle psx_accept(void *ctx, KlSocketHandle fd, struct sockaddr *a, socklen_t *l) {
    (void)ctx; return kl_sockdef_accept(fd, a, l);
}
static int psx_close(void *ctx, KlSocketHandle fd) {
    (void)ctx; return kl_sockdef_close(fd);
}
static int psx_get_local_addr(void *ctx, KlSocketHandle fd, struct sockaddr *a, socklen_t *l) {
    (void)ctx; return kl_sockdef_get_local_addr(fd, a, l);
}
static int psx_get_so_error(void *ctx, KlSocketHandle fd, int *out_err) {
    (void)ctx; return kl_sockdef_get_so_error(fd, out_err);
}
static ssize_t psx_send(void *ctx, KlSocketHandle fd, const void *buf, size_t len) {
    (void)ctx; return kl_sockdef_send(fd, buf, len);
}
static ssize_t psx_recv(void *ctx, KlSocketHandle fd, void *buf, size_t len) {
    (void)ctx; return kl_sockdef_recv(fd, buf, len);
}
static ssize_t psx_recv_peek(void *ctx, KlSocketHandle fd, void *buf, size_t len) {
    (void)ctx; return kl_sockdef_recv_peek(fd, buf, len);
}
static ssize_t psx_writev(void *ctx, KlSocketHandle fd, const struct iovec *iov, int iovcnt) {
    (void)ctx; return kl_sockdef_writev(fd, iov, iovcnt);
}
static ssize_t psx_sendfile(void *ctx, KlSocketHandle out_fd, int in_fd, off_t *offset, size_t count) {
    (void)ctx; return kl_sockdef_sendfile(out_fd, in_fd, offset, count);
}

static const KlSocketOps POSIX_OPS = {
    .set_nonblocking = psx_set_nonblocking,
    .set_cloexec     = psx_set_cloexec,
    .set_nosigpipe   = psx_set_nosigpipe,
    .set_reuseaddr   = psx_set_reuseaddr,
    .set_reuseport   = psx_set_reuseport,
    .set_ipv6only    = psx_set_ipv6only,
    .set_tcp_nodelay = psx_set_tcp_nodelay,
    .set_cork        = psx_set_cork,
    .socket          = psx_socket,
    .connect         = psx_connect,
    .bind            = psx_bind,
    .listen          = psx_listen,
    .accept          = psx_accept,
    .close           = psx_close,
    .get_local_addr  = psx_get_local_addr,
    .get_so_error    = psx_get_so_error,
    .send            = psx_send,
    .recv            = psx_recv,
    .recv_peek       = psx_recv_peek,
    .writev          = psx_writev,
    .sendfile        = psx_sendfile,
    .name            = "posix",
};

static const KlSocketProvider POSIX_PROVIDER = {
    &POSIX_OPS, NULL,
    KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_WRITEV | KL_SOCK_CAP_SENDFILE,
};

const KlSocketProvider *kl_socket_provider_posix(void) {
    return &POSIX_PROVIDER;
}

/* ── Error taxonomy ─────────────────────────────────────────────────────── */

KlError kl_sock_errno_to_error(int err) {
    switch (err) {
        case ETIMEDOUT:
            return KL_ERR_TIMEOUT;
        case ECONNREFUSED:
        case ECONNABORTED:
        case ENETUNREACH:
        case EHOSTUNREACH:
        case ENETDOWN:
#ifdef EHOSTDOWN
        case EHOSTDOWN:
#endif
            return KL_ERR_CONNECT;   /* refused / unreachable — connect-class */
        case EADDRINUSE:
        case EADDRNOTAVAIL:
        case EACCES:
            return KL_ERR_BIND;      /* address-in-use / not-available / denied */
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
        case ENOMEM:
            return KL_ERR_ALLOC;     /* resource exhaustion */
        case EOPNOTSUPP:
        case EAFNOSUPPORT:
        case EPROTONOSUPPORT:
#ifdef ESOCKTNOSUPPORT
        case ESOCKTNOSUPPORT:
#endif
        case EINVAL:
        case EBADF:
        case EFAULT:
            return KL_ERR_INVALID_ARG;   /* unsupported / invalid state */
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EINPROGRESS:
        case EINTR:
        case ECONNRESET:
        case EPIPE:
        case ENOTCONN:
#ifdef ESHUTDOWN
        case ESHUTDOWN:
#endif
        default:
            return KL_ERR_IO;        /* transient / reset / generic I/O */
    }
}
