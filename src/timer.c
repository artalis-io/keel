#include <keel/timer.h>
#include <keel/clock.h>            /* kl_monotonic_ms */
#include "event_ctx_internal.h"   /* KlTimerEntry layout (opaque on the public surface) */
#include <limits.h>                /* INT_MAX */
#include <stdint.h>

/* ── Min-heap helpers ──────────────────────────────────────────────── */

/* Heap order: earlier deadline first, and on equal deadlines the older timer (smaller id) first, so
 * timers due at the same millisecond fire in the order they were added, and a timer added during a
 * kl_timer_fire never sorts ahead of an older one with the same deadline. */
static int heap_before(const KlTimerEntry *a, const KlTimerEntry *b) {
    if (a->deadline_ms != b->deadline_ms) return a->deadline_ms < b->deadline_ms;
    return a->id < b->id;
}

static void heap_swap(KlTimerEntry *a, KlTimerEntry *b) {
    KlTimerEntry tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heap_sift_up(KlTimerEntry *entries, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (!heap_before(&entries[idx], &entries[parent]))
            break;
        heap_swap(&entries[idx], &entries[parent]);
        idx = parent;
    }
}

static void heap_sift_down(KlTimerEntry *entries, int count, int idx) {
    while (1) {
        int smallest = idx;
        int left  = 2 * idx + 1;
        int right = 2 * idx + 2;
        if (left < count && heap_before(&entries[left], &entries[smallest]))
            smallest = left;
        if (right < count && heap_before(&entries[right], &entries[smallest]))
            smallest = right;
        if (smallest == idx)
            break;
        heap_swap(&entries[idx], &entries[smallest]);
        idx = smallest;
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

#define KL_TIMER_INIT_CAP 8

int64_t kl_timer_add(KlEventCtx *ctx, uint64_t delay_ms,
                     KlTimerFn cb, void *user_data) {
    if (!ctx || !cb) return -1;

    /* Grow heap array if needed */
    if (ctx->timer_count >= ctx->timer_cap) {
        int new_cap = ctx->timer_cap == 0 ? KL_TIMER_INIT_CAP
                                          : ctx->timer_cap * 2;
        /* Overflow guard */
        if (new_cap < ctx->timer_cap ||
            (size_t)new_cap > SIZE_MAX / sizeof(KlTimerEntry))
            return -1;
        size_t old_size = (size_t)ctx->timer_cap * sizeof(KlTimerEntry);
        size_t new_size = (size_t)new_cap * sizeof(KlTimerEntry);
        KlTimerEntry *new_arr = kl_realloc(ctx->alloc, ctx->timers,
                                           old_size, new_size);
        if (!new_arr) return -1;
        ctx->timers = new_arr;
        ctx->timer_cap = new_cap;
    }

    int64_t id = ctx->timer_next_id++;
    int idx = ctx->timer_count++;
    /* Saturate: a delay meaning "never" must not wrap the deadline into the past. */
    uint64_t now = kl_monotonic_ms();
    ctx->timers[idx].deadline_ms = (delay_ms > UINT64_MAX - now) ? UINT64_MAX : now + delay_ms;
    ctx->timers[idx].cb = cb;
    ctx->timers[idx].user_data = user_data;
    ctx->timers[idx].id = id;
    heap_sift_up(ctx->timers, idx);

    return id;
}

int kl_timer_cancel(KlEventCtx *ctx, int64_t timer_id) {
    if (!ctx) return -1;

    /* Linear scan for the ID */
    int idx = -1;
    for (int i = 0; i < ctx->timer_count; i++) {
        if (ctx->timers[i].id == timer_id) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;

    /* Swap with last and shrink */
    ctx->timer_count--;
    if (idx < ctx->timer_count) {
        ctx->timers[idx] = ctx->timers[ctx->timer_count];
        /* Fix heap: try both directions */
        heap_sift_up(ctx->timers, idx);
        heap_sift_down(ctx->timers, ctx->timer_count, idx);
    }

    return 0;
}

int kl_timer_next_timeout(KlEventCtx *ctx, int max_ms) {
    if (!ctx || ctx->timer_count == 0)
        return max_ms;

    uint64_t now = kl_monotonic_ms();
    uint64_t deadline = ctx->timers[0].deadline_ms;

    if (now >= deadline)
        return 0;

    uint64_t rem = deadline - now;
    if (max_ms < 0)                          /* no cap: still an int, so INT_MAX at most */
        return rem > (uint64_t)INT_MAX ? INT_MAX : (int)rem;
    if (rem < (uint64_t)max_ms)
        return (int)rem;

    return max_ms;
}

int kl_timer_fire(KlEventCtx *ctx) {
    if (!ctx || ctx->timer_count == 0)
        return 0;

    uint64_t now = kl_monotonic_ms();
    int fired = 0;
    /* Fire only timers that existed on entry. A timer a callback adds gets an id at or above this
     * watermark and waits for the next kl_timer_fire: a 0 ms timer re-added from its own callback
     * was otherwise due again at once (deadline <= now), so one call fired it without end and a
     * retry loop through a 0 ms deferred timer starved all I/O. Ids grow monotonically and the heap
     * breaks deadline ties by id, so the first new timer at the top means every older due timer has
     * fired (an older one with a later deadline that comes due meanwhile fires next call). */
    const int64_t watermark = ctx->timer_next_id;

    while (ctx->timer_count > 0 && ctx->timers[0].deadline_ms <= now &&
           ctx->timers[0].id < watermark) {
        /* Pop the min entry */
        KlTimerFn cb = ctx->timers[0].cb;
        void *ud = ctx->timers[0].user_data;

        ctx->timer_count--;
        if (ctx->timer_count > 0) {
            ctx->timers[0] = ctx->timers[ctx->timer_count];
            heap_sift_down(ctx->timers, ctx->timer_count, 0);
        }

        cb(ud);
        fired++;

        /* Re-read now; callback may have taken time */
        now = kl_monotonic_ms();
    }

    return fired;
}
