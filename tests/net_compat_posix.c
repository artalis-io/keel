/*
 * net_compat_posix.c: POSIX implementation of the test network helpers.
 *
 * Sibling of net_compat_win.c, selected by the Makefile (never both). Thin
 * wrappers over the POSIX socket calls, so ported tests behave identically to
 * their pre-port form on POSIX. See tests/net_compat.h.
 */
#include "net_compat.h"
#include <errno.h>
#include <time.h>      /* nanosleep, struct timespec */
#include <pthread.h>   /* pthread_self: harness thread identity only */
#include "../src/socket.h"   /* kl_socket_provider_* */

int kl_test_closesock(KlSocketHandle fd) {
    return close(fd);
}

int kl_test_set_nonblock(KlSocketHandle fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Each helper retries a call that a signal or task_work interrupted (EINTR). A test thread that has
 * driven an io_uring loop (a client, or kl_http_server_free reaping its accepts) can be interrupted
 * that way after the loop is gone; a blocking read on a socket with SO_RCVTIMEO then returns EINTR
 * rather than restarting, and the test misread it as the peer failing. */
long kl_test_sockwrite(KlSocketHandle fd, const void *buf, size_t len) {
    long r;
    do r = (long)write(fd, buf, len); while (r < 0 && errno == EINTR);
    return r;
}

long kl_test_sockread(KlSocketHandle fd, void *buf, size_t len) {
    long r;
    do r = (long)read(fd, buf, len); while (r < 0 && errno == EINTR);
    return r;
}

static long long test_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int kl_test_poll1(KlSocketHandle fd, int for_write, int timeout_ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = (short)(for_write ? POLLOUT : POLLIN);
    long long end = timeout_ms >= 0 ? test_now_ms() + timeout_ms : 0;
    for (;;) {
        p.revents = 0;
        int r = poll(&p, 1, timeout_ms);
        if (r >= 0 || errno != EINTR) return r;
        if (timeout_ms >= 0) {                      /* keep the caller's deadline */
            long long left = end - test_now_ms();
            timeout_ms = left > 0 ? (int)left : 0;
        }
    }
}

int kl_test_set_rcvtimeo(KlSocketHandle fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int kl_test_socketpair(int sv[2]) {
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
}

/* See net_compat.h: the platform's built-in socket provider, named per platform. */
const void *kl_test_builtin_provider(void) {
    return (const void *)kl_socket_provider_posix();
}

/* See net_compat.h: millisecond sleep. */
void kl_test_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

/* See net_compat.h: calling thread identity. */
KlTestThreadId kl_test_thread_id(void) {
    /* pthread_t is not required to be integral, but on every platform Keel builds for it is a
     * pointer or an integer, and this value is only ever compared with another from this run. */
    return (KlTestThreadId)(uintptr_t)pthread_self();
}
