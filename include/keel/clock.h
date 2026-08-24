#ifndef KEEL_CLOCK_H
#define KEEL_CLOCK_H

#include <stdint.h>

/**
 * @file clock.h
 * @brief Generic monotonic clock: substrate primitive.
 *
 * A process-monotonic millisecond clock used for timeouts, idle sweeps, and
 * keep-alive accounting across every layer (event loop, timers, resolver cache,
 * HTTP connection state machine). It is transport/runtime substrate, not tied to
 * any protocol. The definition lives in the platform TUs (platform_posix.c /
 * platform_win.c).
 */

/** @brief Monotonic clock in milliseconds (for timeout tracking). */
uint64_t kl_monotonic_ms(void);

#endif /* KEEL_CLOCK_H */
