#include <keel/timer.h>
#include <keel/connection.h>  /* kl_monotonic_ms */

/* ── Min-heap helpers ──────────────────────────────────────────────── */

static void heap_swap(KlTimerEntry *a, KlTimerEntry *b) {
    KlTimerEntry tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heap_sift_up(KlTimerEntry *entries, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (entries[idx].deadline_ms >= entries[parent].deadline_ms)
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
        if (left < count &&
            entries[left].deadline_ms < entries[smallest].deadline_ms)
            smallest = left;
        if (right < count &&
            entries[right].deadline_ms < entries[smallest].deadline_ms)
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
    ctx->timers[idx].deadline_ms = kl_monotonic_ms() + delay_ms;
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
    if (rem < (uint64_t)max_ms)
        return (int)rem;

    return max_ms;
}

int kl_timer_fire(KlEventCtx *ctx) {
    if (!ctx || ctx->timer_count == 0)
        return 0;

    uint64_t now = kl_monotonic_ms();
    int fired = 0;

    while (ctx->timer_count > 0 && ctx->timers[0].deadline_ms <= now) {
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

        /* Re-read now — callback may have taken time */
        now = kl_monotonic_ms();
    }

    return fired;
}
