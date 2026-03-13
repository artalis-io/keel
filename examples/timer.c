/*
 * timer.c — One-shot + repeating timers, cancellation
 *
 * Concepts: kl_timer_add, kl_timer_cancel, kl_timer_next_timeout,
 * kl_timer_fire, KlEventCtx standalone (no server).
 *
 * Demonstrates:
 *   - One-shot timer that fires after 500ms
 *   - Repeating timer that re-adds itself every 200ms (5 iterations)
 *   - Cancellation of a pending timer
 *
 * Build:  make examples
 * Run:    ./examples/timer
 */

#include <keel/keel.h>
#include <stdio.h>

/* ── Repeating timer ───────────────────────────────────────────────── */

typedef struct {
    KlEventCtx *ctx;
    int remaining;
    int count;
} RepeatCtx;

static void repeat_cb(void *user_data) {
    RepeatCtx *r = user_data;
    r->count++;
    printf("  tick %d\n", r->count);
    r->remaining--;
    if (r->remaining > 0)
        kl_timer_add(r->ctx, 200, repeat_cb, r);
}

/* ── One-shot timer ────────────────────────────────────────────────── */

static int oneshot_fired = 0;

static void oneshot_cb(void *user_data) {
    (void)user_data;
    oneshot_fired = 1;
    printf("  one-shot fired!\n");
}

/* ── Cancelled timer (should NOT fire) ─────────────────────────────── */

static int cancel_fired = 0;

static void cancel_cb(void *user_data) {
    (void)user_data;
    cancel_fired = 1;
    printf("  ERROR: cancelled timer fired!\n");
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init(&ctx, &alloc) < 0) {
        fprintf(stderr, "event_ctx_init failed\n");
        return 1;
    }

    /* 1. One-shot timer at 500ms */
    printf("scheduling one-shot timer (500ms)...\n");
    kl_timer_add(&ctx, 500, oneshot_cb, NULL);

    /* 2. Repeating timer every 200ms, 5 times */
    printf("scheduling repeating timer (200ms x 5)...\n");
    RepeatCtx rctx = { .ctx = &ctx, .remaining = 5, .count = 0 };
    kl_timer_add(&ctx, 200, repeat_cb, &rctx);

    /* 3. Timer to cancel */
    printf("scheduling + cancelling a 300ms timer...\n");
    int64_t cancel_id = kl_timer_add(&ctx, 300, cancel_cb, NULL);
    if (kl_timer_cancel(&ctx, cancel_id) == 0)
        printf("  cancelled successfully\n");

    /* Run event loop until all timers have fired.
     * The repeating timer fires 5 times at 200ms intervals = ~1000ms.
     * The one-shot fires at 500ms. Total runtime ~1200ms. */
    int iterations = 0;
    while (rctx.remaining > 0 || !oneshot_fired) {
        kl_event_ctx_run(&ctx, 16, 1000);
        if (++iterations > 20) break;  /* safety limit */
    }

    printf("done: one-shot=%s, ticks=%d, cancel=%s\n",
           oneshot_fired ? "yes" : "no",
           rctx.count,
           cancel_fired ? "LEAKED" : "ok");

    kl_event_ctx_free(&ctx);
    return 0;
}
