#ifndef KEEL_SRC_PLATFORM_H
#define KEEL_SRC_PLATFORM_H

/*
 * platform.h — internal platform-services interface.
 *
 * The narrow, logic-free declarations for the handful of services that are
 * genuinely OS-specific and not socket-shaped: monotonic clock, secure random,
 * thread-pool wakeup. Each is DEFINED per-OS in platform_posix.c / platform_win.c
 * (one selected by the Makefile PLATFORM_SRC branch) — the same one-platform-per-
 * TU pattern as event_epoll.c/event_wsapoll.c and socket_posix.c/socket_winsock.c.
 * See docs/phase6_winsock_design.md §B.0/§B.3.
 *
 * Deliberately several narrow functions rather than one KlPlatformOps god-object
 * (per docs/pal_transformation_design.md §6).
 *
 * INTERNAL header — not installed.
 */

#include <stdint.h>
#include <stddef.h>

#include "keel/handle.h"

/* Monotonic clock in milliseconds. POSIX: clock_gettime(CLOCK_MONOTONIC).
 * Windows: QueryPerformanceCounter. Also declared in keel/connection.h (public,
 * unchanged); the identical redeclaration here lets the per-OS TU avoid dragging
 * in the not-yet-Windows-ready connection.h. */
uint64_t kl_monotonic_ms(void);

/* Fill @buf with @len secure-random bytes (best-effort — always fills the whole
 * buffer, degrading to a non-cryptographic last resort if the OS RNG is somehow
 * unavailable). POSIX: arc4random / /dev/urandom. Windows: BCryptGenRandom.
 * Callers use it for defense-in-depth (WS mask keys) and off-path spoof
 * resistance (DNS txn-id / 0x20 / cookies), not as a hard security boundary. */
void kl_plat_random(void *buf, size_t len);

/* Cross-thread event-loop wakeup channel (a self-pipe). A worker thread writes a
 * byte to .wr to wake the event loop, which watches .rd. .rd is non-blocking and
 * is the handle to register with the event loop (kl_watcher_add); it coalesces —
 * the byte count carries no meaning, only "something is ready".
 *
 * POSIX: pipe(2). Windows: a connected loopback TCP socket pair, because WSAPoll
 * can only watch sockets, not pipe HANDLEs (see docs/phase6_winsock_design.md
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

#endif /* KEEL_SRC_PLATFORM_H */
