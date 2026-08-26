#ifndef KEEL_TIMER_H
#define KEEL_TIMER_H

#include <keel/event_ctx.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Timer callback function.
 * @param user_data Opaque pointer passed to kl_timer_add.
 */
typedef void (*KlTimerFn)(void *user_data);

/**
 * @brief Timer heap entry (stored in KlEventCtx.timers array).
 */
struct KlTimerEntry {
    uint64_t  deadline_ms;   /**< Absolute monotonic deadline. */
    KlTimerFn cb;            /**< Callback invoked on expiry. */
    void     *user_data;     /**< Opaque pointer passed to cb. */
    int64_t   id;            /**< Monotonic timer ID. */
};

/**
 * @brief Schedule a one-shot timer.
 *
 * The callback fires after delay_ms milliseconds on the event loop thread.
 * Timers are one-shot: the callback can re-add itself via kl_timer_add
 * if repeating behavior is needed.
 *
 * Safe to call from within a timer callback.
 *
 * @param ctx      Event context (owns the timer heap).
 * @param delay_ms Delay in milliseconds (0 = fire on next kl_timer_fire).
 * @param cb       Callback invoked when the timer expires.
 * @param user_data Opaque pointer passed to cb.
 * @return Timer ID >= 0 on success, -1 on error.
 */
int64_t kl_timer_add(KlEventCtx *ctx, uint64_t delay_ms,
                     KlTimerFn cb, void *user_data);

/**
 * @brief Cancel a pending timer.
 *
 * Safe to call from within a timer callback. No-op if the timer has
 * already fired (IDs are never reused).
 *
 * @param ctx      Event context.
 * @param timer_id ID returned by kl_timer_add.
 * @return 0 if cancelled, -1 if not found.
 */
int kl_timer_cancel(KlEventCtx *ctx, int64_t timer_id);

/**
 * @brief Compute the timeout for the next kl_event_wait call.
 *
 * Returns the smaller of max_ms and the time until the next timer fires.
 * Returns 0 if a timer is already overdue. Returns max_ms if no timers
 * are pending.
 *
 * @param ctx    Event context.
 * @param max_ms Maximum timeout (e.g. KL_POLL_TIMEOUT_MS).
 * @return Clamped timeout in milliseconds.
 */
int kl_timer_next_timeout(KlEventCtx *ctx, int max_ms);

/**
 * @brief Fire all expired timers.
 *
 * Pops and invokes all timers whose deadline <= kl_monotonic_ms().
 * Safe for callbacks to add or cancel timers.
 *
 * @param ctx Event context.
 * @return Number of timers fired.
 */
int kl_timer_fire(KlEventCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
