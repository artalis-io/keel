/*
 * socket.c — internal POSIX socket seam (PAL Phase 1).
 *
 * The one production provider behind src/socket.h. Setup helpers live here
 * (out of line — they run at connect/accept time, not on the hot path); the
 * send/recv fast paths are inline in the header. See
 * docs/pal_transformation_design.md.
 */

#include "socket.h"

#include <fcntl.h>

int kl_sock_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void kl_sock_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void kl_sock_set_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}
