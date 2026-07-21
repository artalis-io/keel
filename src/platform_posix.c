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
