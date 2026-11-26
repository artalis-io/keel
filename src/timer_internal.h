/*
 * timer_internal.h: INTERNAL timer helpers for substrate modules (not part of the public surface).
 */
#ifndef KEEL_SRC_TIMER_INTERNAL_H
#define KEEL_SRC_TIMER_INTERNAL_H

#include <keel/timer.h>
#include <stdint.h>

/* As kl_timer_cancel, but cancels the entry only if its id, callback AND user_data all match. A
 * holder of a possibly stale id (its ctx freed and re-initialised, ids restarting at 0) then cannot
 * cancel another owner's timer. 0 if cancelled, -1 if no matching entry. */
int kl_timer_cancel_match(KlEventCtx *ctx, int64_t timer_id, KlTimerFn cb, const void *user_data);

#endif /* KEEL_SRC_TIMER_INTERNAL_H */
