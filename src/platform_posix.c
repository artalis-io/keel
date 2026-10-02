/*
 * platform_posix.c: POSIX platform services (implements platform.h).
 *
 * One-platform-per-TU (Makefile PLATFORM_SRC): the POSIX sibling of
 * platform_win.c. See docs/archive/phases/phase6_winsock_design.md §B.3.
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

void kl_plat_random_weak(void *buf, size_t len) {
    struct timespec ts = { 0, 0 };
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    struct timespec rt = { 0, 0 };
    (void)clock_gettime(CLOCK_REALTIME, &rt);
    uint64_t x = ((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec) ^
                 ((uint64_t)rt.tv_nsec << 32) ^ ((uint64_t)getpid() << 16) ^ (uint64_t)(uintptr_t)buf;
    unsigned char *p = buf;
    for (size_t i = 0; i < len; i++) {
        x += 0x9E3779B97F4A7C15ull;                     /* splitmix64 */
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        p[i] = (unsigned char)(z ^ (z >> 31));
    }
}

void kl_plat_random(void *buf, size_t len) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(buf, len);
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        unsigned char *p = buf;
        size_t total = 0;
        while (total < len) {
            kl_ssize_t r = read(fd, p + total, len - total);
            if (r <= 0) break;
            total += (size_t)r;
        }
        close(fd);
        if (total == len) return;
    }
    /* Last resort (effectively never on a real system). */
    kl_plat_random_weak(buf, len);
#endif
}

/* kl_plat_wakeup_* live in platform_wakeup_posix.c: an overridable seam so a
 * foreign stack (lwIP) can swap the wakeup channel without touching this TU. */

int kl_plat_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

long kl_plat_pid(void)
{
    return (long)getpid();
}

int kl_plat_file_pread(int fd, void *buf, size_t count, long long offset)
{
    kl_ssize_t r = pread(fd, buf, count, (off_t)offset);
    return (int)r;
}

void kl_plat_file_close(int fd)
{
    if (fd >= 0)
        close(fd);
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
