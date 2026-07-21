/*
 * platform_posix.c — POSIX platform services (implements platform.h).
 *
 * One-platform-per-TU (Makefile PLATFORM_SRC): the POSIX sibling of
 * platform_win.c. See docs/phase6_winsock_design.md §B.3.
 */

#include "platform.h"

#include <time.h>

uint64_t kl_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
