/*
 * socket.c — internal socket seam: POSIX implementations + POSIX provider.
 *
 * The setup helpers run at connect/accept time (out of line here); the
 * send/recv fast paths are inline in the header. The POSIX provider wraps these
 * so a decorator/mock can wrap or replace the built-in stack. See
 * docs/pal_transformation_design.md.
 */

#include "socket.h"

#include <fcntl.h>

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

/* ── POSIX provider ─────────────────────────────────────────────────────── */

static int psx_set_nonblocking(void *ctx, int fd) {
    (void)ctx; return kl_posix_set_nonblocking(fd);
}
static void psx_set_cloexec(void *ctx, int fd) {
    (void)ctx; kl_posix_set_cloexec(fd);
}
static void psx_set_nosigpipe(void *ctx, int fd) {
    (void)ctx; kl_posix_set_nosigpipe(fd);
}
static int psx_socket(void *ctx, int domain, int type, int protocol) {
    (void)ctx; return socket(domain, type, protocol);
}
static int psx_connect(void *ctx, int fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return connect(fd, a, l);
}
static int psx_bind(void *ctx, int fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return bind(fd, a, l);
}
static int psx_listen(void *ctx, int fd, int backlog) {
    (void)ctx; return listen(fd, backlog);
}
static int psx_accept(void *ctx, int fd, struct sockaddr *a, socklen_t *l) {
    (void)ctx; return accept(fd, a, l);
}
static int psx_close(void *ctx, int fd) {
    (void)ctx; return close(fd);
}
static ssize_t psx_send(void *ctx, int fd, const void *buf, size_t len) {
    (void)ctx; return kl_sock_send(NULL, fd, buf, len);   /* inline POSIX path */
}
static ssize_t psx_recv(void *ctx, int fd, void *buf, size_t len) {
    (void)ctx; return kl_sock_recv(NULL, fd, buf, len);
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
    .name            = "posix",
};

static const KlSocketProvider POSIX_PROVIDER = {
    &POSIX_OPS, NULL, KL_SOCK_CAP_NATIVE_FD,
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
