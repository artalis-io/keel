/*
 * platform_win.c — Windows platform services (implements platform.h).
 *
 * One-platform-per-TU (Makefile PLATFORM_SRC), compiled only on Windows. See
 * docs/phase6_winsock_design.md §B.3.
 */

#include "platform.h"

#include "sockcompat.h"   /* winsock2.h before windows.h (avoids winsock.h v1 clash) */
#include <windows.h>
#include <bcrypt.h>
#include <io.h>        /* _read / _lseeki64 (CRT file descriptors) */
#include <limits.h>
#include <string.h>

uint64_t kl_monotonic_ms(void) {
    LARGE_INTEGER freq, ctr;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&ctr) ||
        freq.QuadPart == 0)
        return (uint64_t)GetTickCount64();   /* fallback: also monotonic ms */
    /* Overflow-safe QPC-ticks -> ms (avoid ctr*1000 overflowing after ~decades). */
    uint64_t q    = (uint64_t)freq.QuadPart;
    uint64_t sec  = (uint64_t)ctr.QuadPart / q;
    uint64_t rem  = (uint64_t)ctr.QuadPart % q;
    return sec * 1000 + (rem * 1000) / q;
}

void kl_plat_random(void *buf, size_t len) {
    /* BCRYPT_USE_SYSTEM_PREFERRED_RNG: no algorithm handle needed. */
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)   /* STATUS_SUCCESS */
        return;
    /* Last resort — non-cryptographic, never leaves the buffer undefined. */
    unsigned char *p = buf;
    for (size_t i = 0; i < len; i++)
        p[i] = (unsigned char)((uintptr_t)&p[i] ^ (i * 131u));
}

/* kl_plat_wakeup_* moved to platform_wakeup_win.c — an overridable seam mirroring
 * platform_wakeup_posix.c. */

int kl_plat_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

int kl_plat_file_pread(int fd, void *buf, size_t count, long long offset)
{
    if (_lseeki64(fd, offset, SEEK_SET) < 0)
        return -1;
    unsigned n = count > (size_t)INT_MAX ? (unsigned)INT_MAX : (unsigned)count;
    return _read(fd, buf, n);
}

int kl_plat_poll1(KlSocketHandle fd, int events, int timeout_ms)
{
    WSAPOLLFD pfd;
    pfd.fd = (SOCKET)fd;
    pfd.events = (SHORT)(((events & KL_POLL_IN) ? POLLRDNORM : 0) |
                         ((events & KL_POLL_OUT) ? POLLWRNORM : 0));
    pfd.revents = 0;
    int r = WSAPoll(&pfd, 1, timeout_ms);
    if (r == SOCKET_ERROR) { kl_wsa_set_errno(); return -1; }   /* errno for the caller */
    return r;
}
