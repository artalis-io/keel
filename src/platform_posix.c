/*
 * platform_posix.c: POSIX platform services (implements platform.h).
 *
 * One-platform-per-TU (Makefile PLATFORM_SRC): the POSIX sibling of
 * platform_win.c. See docs/archive/phases/phase6_winsock_design.md §B.3.
 */

#include "platform.h"

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>

/* getrandom(2): Linux 3.17+, declared by glibc 2.25+ and musl 1.1.20+. Without the header the
 * entropy fill uses /dev/urandom alone. */
#if defined(__linux__) && !defined(__COSMOPOLITAN__) && defined(__has_include)
#  if __has_include(<sys/random.h>)
#    include <sys/random.h>
#    define KL_HAVE_GETRANDOM 1
#  endif
#endif

uint64_t kl_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int kl_plat_open_read(const char *path) {
    return open(path, O_RDONLY | O_CLOEXEC);
}

int kl_plat_random(void *buf, size_t len) {
    if (len == 0) return 0;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(buf, len);                           /* cannot fail */
    return 0;
#else
    unsigned char *p = buf;
    size_t total = 0;
#ifdef KL_HAVE_GETRANDOM
    /* getrandom(2) first: it needs no descriptor, so it still works when the process has run out
     * of them or /dev is absent (a chroot, a minimal container). A signal can cut a large read
     * short or interrupt it, so loop. */
    while (total < len) {
        ssize_t r = getrandom(p + total, len - total, 0);
        if (r > 0) { total += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        break;                                          /* ENOSYS (kernel < 3.17): use the device */
    }
    if (total == len) return 0;
#endif
    int fd;
    do { fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
    if (fd >= 0) {
        while (total < len) {
            kl_ssize_t r = read(fd, p + total, len - total);
            if (r > 0) { total += (size_t)r; continue; }
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        close(fd);
        if (total == len) return 0;
    }
    /* No OS entropy: fail rather than fill the buffer with something guessable. */
    memset(buf, 0, len);
    return -1;
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
