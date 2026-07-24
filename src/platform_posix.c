/*
 * platform_posix.c — POSIX platform services (implements platform.h).
 *
 * One-platform-per-TU (Makefile PLATFORM_SRC): the POSIX sibling of
 * platform_win.c. See docs/phase6_winsock_design.md §B.3.
 */

#include "platform.h"

#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>

uint64_t kl_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void kl_plat_random(void *buf, size_t len) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(buf, len);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        unsigned char *p = buf;
        size_t total = 0;
        while (total < len) {
            ssize_t r = read(fd, p + total, len - total);
            if (r <= 0) break;
            total += (size_t)r;
        }
        close(fd);
        if (total == len) return;
    }
    /* Last resort (effectively never on a real system): a non-cryptographic
     * fill that still varies per byte, so the buffer is never left undefined. */
    unsigned char *p = buf;
    for (size_t i = 0; i < len; i++)
        p[i] = (unsigned char)((uintptr_t)&p[i] ^ (i * 131u));
#endif
}

int kl_plat_wakeup_open(KlPlatWakeup *w)
{
    w->rd = w->wr = KL_INVALID_SOCKET;

    int fds[2];
    if (pipe(fds) < 0)
        return -1;

    /* Read end non-blocking: the drain must never stall the event loop. */
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    w->rd = fds[0];
    w->wr = fds[1];
    return 0;
}

void kl_plat_wakeup_signal(const KlPlatWakeup *w)
{
    char c = 1;
    ssize_t wr = write((int)w->wr, &c, 1);
    (void)wr;
}

void kl_plat_wakeup_drain(KlSocketHandle rd)
{
    /* Single non-blocking read is enough: the done queue is drained under the
     * mutex regardless of how many bytes we consume here, and any residue
     * re-fires the level-triggered watcher harmlessly. */
    char buf[64];
    ssize_t rc = read((int)rd, buf, sizeof(buf));
    (void)rc;
}

void kl_plat_wakeup_close(KlPlatWakeup *w)
{
    if (kl_handle_valid(w->rd)) close((int)w->rd);
    if (kl_handle_valid(w->wr)) close((int)w->wr);
    w->rd = w->wr = KL_INVALID_SOCKET;
}

int kl_plat_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

int kl_plat_file_pread(int fd, void *buf, size_t count, long long offset)
{
    ssize_t r = pread(fd, buf, count, (off_t)offset);
    return (int)r;
}

int kl_plat_poll1(KlSocketHandle fd, int events, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd = (int)fd;
    pfd.events = (short)(((events & KL_POLL_IN) ? POLLIN : 0) |
                         ((events & KL_POLL_OUT) ? POLLOUT : 0));
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms);
}
