#ifndef KEEL_SRC_PLATFORM_H
#define KEEL_SRC_PLATFORM_H

/*
 * platform.h: internal platform-services interface.
 *
 * The narrow, logic-free declarations for the handful of services that are
 * genuinely OS-specific and not socket-shaped: monotonic clock, secure random,
 * thread-pool wakeup. Each is DEFINED per-OS in platform_posix.c / platform_win.c
 * (one selected by the Makefile PLATFORM_SRC branch): the same one-platform-per-
 * TU pattern as event_epoll.c/event_wsapoll.c and socket_posix.c/socket_winsock.c.
 * See docs/archive/phases/phase6_winsock_design.md §B.0/§B.3.
 *
 * Deliberately several narrow functions rather than one KlPlatformOps god-object
 * (per docs/archive/designs/pal_transformation_design.md §6).
 *
 * INTERNAL header: not installed.
 */

#include <stdint.h>
#include <stddef.h>

#include "keel/handle.h"

/* Monotonic clock in milliseconds. POSIX: clock_gettime(CLOCK_MONOTONIC).
 * Windows: QueryPerformanceCounter. Also declared in keel/http_connection.h (public,
 * unchanged); the identical redeclaration here lets the per-OS TU avoid dragging
 * in the not-yet-Windows-ready http_connection.h. */
#include <keel/clock.h>   /* kl_monotonic_ms: generic substrate clock */

/* Fill @buf with @len secure-random bytes (best-effort: always fills the whole
 * buffer, degrading to a non-cryptographic last resort if the OS RNG is somehow
 * unavailable). POSIX: arc4random / /dev/urandom. Windows: BCryptGenRandom.
 * Callers use it for defense-in-depth (WS mask keys) and off-path spoof
 * resistance (DNS txn-id / 0x20 / cookies), not as a hard security boundary. */
void kl_plat_random(void *buf, size_t len);

/* Cross-thread event-loop wakeup channel (a self-pipe). A worker thread writes a
 * byte to .wr to wake the event loop, which watches .rd. .rd is non-blocking and
 * is the handle to register with the event loop (kl_watcher_add); it coalesces:
 * the byte count carries no meaning, only "something is ready".
 *
 * POSIX: pipe(2). Windows: a connected loopback TCP socket pair, because WSAPoll
 * can only watch sockets, not pipe HANDLEs (see docs/archive/phases/phase6_winsock_design.md
 * §B.3). This is the one platform seam the thread pool needs. */
typedef struct {
    KlSocketHandle rd;
    KlSocketHandle wr;
} KlPlatWakeup;

/* Open the wakeup pair, setting .rd non-blocking. On success fills *w and
 * returns 0; on failure sets both ends to KL_INVALID_SOCKET and returns -1. */
int  kl_plat_wakeup_open(KlPlatWakeup *w);

/* Write one wakeup byte to w->wr. Safe from any thread; short/failed writes are
 * ignored (the read end coalesces, so a lost byte only ever costs a wakeup that
 * a concurrent one already delivered). */
void kl_plat_wakeup_signal(const KlPlatWakeup *w);

/* Drain pending wakeup bytes from @rd (the .rd handle). Event-loop thread only. */
void kl_plat_wakeup_drain(KlSocketHandle rd);

/* Close both ends and set them to KL_INVALID_SOCKET. */
void kl_plat_wakeup_close(KlPlatWakeup *w);

/* Number of online logical CPUs (>= 1), for default thread-pool sizing. POSIX:
 * sysconf(_SC_NPROCESSORS_ONLN). Windows: GetSystemInfo. Returns 1 if unknown. */
int kl_plat_cpu_count(void);

/* Positioned read from a file descriptor at @offset, up to @count bytes into
 * @buf. Returns bytes read (0 = EOF), or -1 on error. POSIX: pread (offset
 * unchanged). Windows: _lseeki64 + _read (advances the fd offset, fine for the
 * sequential, per-response file sends this serves). @fd is a CRT file
 * descriptor; @count is bounded by the caller's buffer (fits in int). */
int kl_plat_file_pread(int fd, void *buf, size_t count, long long offset);

/* Close a file-body descriptor opened for a KL_HTTP_BODY_FILE response. A platform
 * seam (not a raw close()) so http_response.c stays freestanding: POSIX/Windows close
 * the CRT fd; a freestanding build (no filesystem) supplies its own: typically a
 * no-op. Ignores a negative fd. */
void kl_plat_file_close(int fd);

/* Single-socket readiness wait (the blocking poll(&pfd,1,timeout) idiom used by
 * the sync client). @events is a mask of KL_POLL_IN / KL_POLL_OUT. Returns >0 if
 * ready (requested event or an error/hangup event), 0 on timeout, -1 on failure.
 * POSIX: poll(2). Windows: WSAPoll. */
#define KL_POLL_IN   0x1
#define KL_POLL_OUT  0x2
int kl_plat_poll1(KlSocketHandle fd, int events, int timeout_ms);

#endif /* KEEL_SRC_PLATFORM_H */
