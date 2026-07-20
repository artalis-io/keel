/*
 * socket_posix.c — the POSIX socket provider (implements the socket.h seam).
 *
 * One provider per TU: sibling-to-be of socket_winsock.c / socket_lwip.c, the
 * analog of event_epoll.c/event_kqueue.c implementing the event.h interface.
 * socket.h is the platform-neutral seam (vtable + dispatchers); this file is the
 * POSIX implementation of it.
 *
 * The setup helpers run at connect/accept time (out of line here); the
 * send/recv fast paths are inline in the header. The POSIX provider wraps these
 * so a decorator/mock can wrap or replace the built-in stack. See
 * docs/pal_transformation_design.md and docs/phase6_winsock_design.md §B.0.
 */

#include "socket.h"

#include <fcntl.h>
#if defined(__linux__)
  #include <sys/sendfile.h>
#endif

#define KL_SENDFILE_BUF 8192   /* pread+write fallback chunk (no-sendfile hosts) */

int kl_posix_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void kl_posix_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void kl_posix_set_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

ssize_t kl_posix_sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
#if defined(__linux__)
    return sendfile(out_fd, in_fd, offset, count);
#elif defined(__APPLE__)
    off_t len = (off_t)count;
    int r = sendfile(in_fd, out_fd, *offset, &len, NULL, 0);
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
        ssize_t nw = write(out_fd, p, remaining);
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

/* ── POSIX provider ─────────────────────────────────────────────────────── */

static int psx_set_nonblocking(void *ctx, KlSocketHandle fd) {
    (void)ctx; return kl_posix_set_nonblocking((int)fd);
}
static void psx_set_cloexec(void *ctx, KlSocketHandle fd) {
    (void)ctx; kl_posix_set_cloexec((int)fd);
}
static void psx_set_nosigpipe(void *ctx, KlSocketHandle fd) {
    (void)ctx; kl_posix_set_nosigpipe((int)fd);
}
static KlSocketHandle psx_socket(void *ctx, int domain, int type, int protocol) {
    (void)ctx; return (KlSocketHandle)socket(domain, type, protocol);
}
static int psx_connect(void *ctx, KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return connect((int)fd, a, l);
}
static int psx_bind(void *ctx, KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return bind((int)fd, a, l);
}
static int psx_listen(void *ctx, KlSocketHandle fd, int backlog) {
    (void)ctx; return listen((int)fd, backlog);
}
static KlSocketHandle psx_accept(void *ctx, KlSocketHandle fd, struct sockaddr *a, socklen_t *l) {
    (void)ctx; return (KlSocketHandle)accept((int)fd, a, l);
}
static int psx_close(void *ctx, KlSocketHandle fd) {
    (void)ctx; return close((int)fd);
}
static ssize_t psx_send(void *ctx, KlSocketHandle fd, const void *buf, size_t len) {
    (void)ctx; return kl_sock_send(NULL, fd, buf, len);   /* inline POSIX path */
}
static ssize_t psx_recv(void *ctx, KlSocketHandle fd, void *buf, size_t len) {
    (void)ctx; return kl_sock_recv(NULL, fd, buf, len);
}
static ssize_t psx_writev(void *ctx, KlSocketHandle fd, const struct iovec *iov, int iovcnt) {
    (void)ctx; return writev((int)fd, iov, iovcnt);
}
static ssize_t psx_sendfile(void *ctx, KlSocketHandle out_fd, int in_fd, off_t *offset, size_t count) {
    (void)ctx; return kl_posix_sendfile((int)out_fd, in_fd, offset, count);
}

static const KlSocketOps POSIX_OPS = {
    .set_nonblocking = psx_set_nonblocking,
    .set_cloexec     = psx_set_cloexec,
    .set_nosigpipe   = psx_set_nosigpipe,
    .socket          = psx_socket,
    .connect         = psx_connect,
    .bind            = psx_bind,
    .listen          = psx_listen,
    .accept          = psx_accept,
    .close           = psx_close,
    .send            = psx_send,
    .recv            = psx_recv,
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
