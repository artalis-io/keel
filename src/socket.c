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

static int posix_set_nonblocking(void *ctx, int fd) {
    (void)ctx; return kl_posix_set_nonblocking(fd);
}
static void posix_set_cloexec(void *ctx, int fd) {
    (void)ctx; kl_posix_set_cloexec(fd);
}
static void posix_set_nosigpipe(void *ctx, int fd) {
    (void)ctx; kl_posix_set_nosigpipe(fd);
}
static int posix_socket(void *ctx, int domain, int type, int protocol) {
    (void)ctx; return socket(domain, type, protocol);
}
static int posix_connect(void *ctx, int fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return connect(fd, a, l);
}
static int posix_bind(void *ctx, int fd, const struct sockaddr *a, socklen_t l) {
    (void)ctx; return bind(fd, a, l);
}
static int posix_listen(void *ctx, int fd, int backlog) {
    (void)ctx; return listen(fd, backlog);
}
static int posix_accept(void *ctx, int fd, struct sockaddr *a, socklen_t *l) {
    (void)ctx; return accept(fd, a, l);
}
static int posix_close(void *ctx, int fd) {
    (void)ctx; return close(fd);
}
static ssize_t posix_send(void *ctx, int fd, const void *buf, size_t len) {
    (void)ctx; return kl_sock_send(NULL, fd, buf, len);   /* inline POSIX path */
}
static ssize_t posix_recv(void *ctx, int fd, void *buf, size_t len) {
    (void)ctx; return kl_sock_recv(NULL, fd, buf, len);
}

static const KlSocketOps POSIX_OPS = {
    .set_nonblocking = posix_set_nonblocking,
    .set_cloexec     = posix_set_cloexec,
    .set_nosigpipe   = posix_set_nosigpipe,
    .socket          = posix_socket,
    .connect         = posix_connect,
    .bind            = posix_bind,
    .listen          = posix_listen,
    .accept          = posix_accept,
    .close           = posix_close,
    .send            = posix_send,
    .recv            = posix_recv,
    .name            = "posix",
};

static const KlSocketProvider POSIX_PROVIDER = {
    &POSIX_OPS, NULL, KL_SOCK_CAP_NATIVE_FD,
};

const KlSocketProvider *kl_socket_provider_posix(void) {
    return &POSIX_PROVIDER;
}
