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

/* Windows has no pipe(2) that WSAPoll can watch, so the wakeup channel is a
 * connected loopback TCP pair. Winsock must already be started (the socket
 * provider's WSAStartup, ref-counted); a standalone thread pool on Windows is a
 * Phase 6b concern. */
static int win_wakeup_pair(SOCKET sv[2])
{
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKET client   = INVALID_SOCKET;
    SOCKET server   = INVALID_SOCKET;
    if (listener == INVALID_SOCKET)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;   /* ephemeral */

    int addrlen = (int)sizeof(addr);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;
    if (listen(listener, 1) != 0) goto fail;
    if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) goto fail;

    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) goto fail;
    if (connect(client, (struct sockaddr *)&addr, addrlen) != 0) goto fail;

    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET) goto fail;

    closesocket(listener);
    sv[0] = server;   /* read end  (event loop watches this) */
    sv[1] = client;   /* write end (workers signal this)     */
    return 0;

fail:
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (client   != INVALID_SOCKET) closesocket(client);
    if (server   != INVALID_SOCKET) closesocket(server);
    return -1;
}

int kl_plat_wakeup_open(KlPlatWakeup *w)
{
    w->rd = w->wr = KL_INVALID_SOCKET;

    SOCKET sv[2];
    if (win_wakeup_pair(sv) != 0)
        return -1;

    u_long nonblocking = 1;
    (void)ioctlsocket(sv[0], FIONBIO, &nonblocking);   /* read end non-blocking */

    w->rd = (KlSocketHandle)sv[0];
    w->wr = (KlSocketHandle)sv[1];
    return 0;
}

void kl_plat_wakeup_signal(const KlPlatWakeup *w)
{
    char c = 1;
    (void)send((SOCKET)w->wr, &c, 1, 0);
}

void kl_plat_wakeup_drain(KlSocketHandle rd)
{
    char buf[64];
    (void)recv((SOCKET)rd, buf, (int)sizeof(buf), 0);
}

void kl_plat_wakeup_close(KlPlatWakeup *w)
{
    if (kl_handle_valid(w->rd)) closesocket((SOCKET)w->rd);
    if (kl_handle_valid(w->wr)) closesocket((SOCKET)w->wr);
    w->rd = w->wr = KL_INVALID_SOCKET;
}

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
