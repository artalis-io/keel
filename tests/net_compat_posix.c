/*
 * net_compat_posix.c: POSIX implementation of the test network helpers.
 *
 * Sibling of net_compat_win.c, selected by the Makefile (never both). Thin
 * wrappers over the POSIX socket calls, so ported tests behave identically to
 * their pre-port form on POSIX. See tests/net_compat.h.
 */
#include "net_compat.h"

int kl_test_closesock(int fd) {
    return close(fd);
}

int kl_test_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

long kl_test_sockwrite(int fd, const void *buf, size_t len) {
    return (long)write(fd, buf, len);
}

long kl_test_sockread(int fd, void *buf, size_t len) {
    return (long)read(fd, buf, len);
}

int kl_test_poll1(int fd, int for_write, int timeout_ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = (short)(for_write ? POLLOUT : POLLIN);
    p.revents = 0;
    return poll(&p, 1, timeout_ms);
}

int kl_test_set_rcvtimeo(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int kl_test_socketpair(int sv[2]) {
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
}
