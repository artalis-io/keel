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

#endif /* KEEL_SRC_PLATFORM_H */
