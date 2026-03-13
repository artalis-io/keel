#include "utest.h"
#include <keel/keel.h>
#include <unistd.h>

/* ── Callback helpers ────────────────────────────────────────────── */

static int cb_called;
static void *cb_got_data;

static void counting_cb(void *user_data) {
    cb_called++;
    cb_got_data = user_data;
}

/* Track fire order */
static int order_log[8];
static int order_idx;

static void order_cb_1(void *user_data) { (void)user_data; order_log[order_idx++] = 1; }
static void order_cb_2(void *user_data) { (void)user_data; order_log[order_idx++] = 2; }
static void order_cb_3(void *user_data) { (void)user_data; order_log[order_idx++] = 3; }

/* ── Tests ───────────────────────────────────────────────────────── */

UTEST(timer, add_returns_valid_id) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);

    int64_t id = kl_timer_add(&ctx, 1000, counting_cb, NULL);
    ASSERT_TRUE(id >= 0);

    /* Second add returns a different ID */
    int64_t id2 = kl_timer_add(&ctx, 1000, counting_cb, NULL);
    ASSERT_TRUE(id2 >= 0);
    ASSERT_NE(id, id2);

    kl_event_ctx_free(&ctx);
}

UTEST(timer, fire_after_delay) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);
    cb_called = 0;
    cb_got_data = NULL;
    int marker = 42;

    kl_timer_add(&ctx, 10, counting_cb, &marker);
    ASSERT_EQ(cb_called, 0);

    /* Sleep past the deadline */
    usleep(20000);  /* 20ms */

    int fired = kl_timer_fire(&ctx);
    ASSERT_EQ(fired, 1);
    ASSERT_EQ(cb_called, 1);
    ASSERT_EQ(cb_got_data, &marker);

    kl_event_ctx_free(&ctx);
}

UTEST(timer, fire_order) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);
    order_idx = 0;

    /* Add in reverse order: 30ms, 20ms, 10ms */
    kl_timer_add(&ctx, 30, order_cb_3, NULL);
    kl_timer_add(&ctx, 20, order_cb_2, NULL);
    kl_timer_add(&ctx, 10, order_cb_1, NULL);

    /* Wait for all to expire */
    usleep(50000);  /* 50ms */

    int fired = kl_timer_fire(&ctx);
    ASSERT_EQ(fired, 3);

    /* Should fire in deadline order: 1, 2, 3 */
    ASSERT_EQ(order_log[0], 1);
    ASSERT_EQ(order_log[1], 2);
    ASSERT_EQ(order_log[2], 3);

    kl_event_ctx_free(&ctx);
}

UTEST(timer, cancel_prevents_fire) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);
    cb_called = 0;

    int64_t id = kl_timer_add(&ctx, 10, counting_cb, NULL);
    ASSERT_EQ(kl_timer_cancel(&ctx, id), 0);

    usleep(20000);
    int fired = kl_timer_fire(&ctx);
    ASSERT_EQ(fired, 0);
    ASSERT_EQ(cb_called, 0);

    kl_event_ctx_free(&ctx);
}

UTEST(timer, cancel_invalid_returns_minus1) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);

    /* Cancel a bogus ID */
    ASSERT_EQ(kl_timer_cancel(&ctx, 9999), -1);

    /* Cancel on empty heap */
    ASSERT_EQ(kl_timer_cancel(&ctx, 0), -1);

    kl_event_ctx_free(&ctx);
}

UTEST(timer, next_timeout_no_timers) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);

    /* No timers: returns max_ms unchanged */
    ASSERT_EQ(kl_timer_next_timeout(&ctx, 500), 500);

    kl_event_ctx_free(&ctx);
}

UTEST(timer, next_timeout_clamps) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);

    /* Add a timer 50ms from now */
    kl_timer_add(&ctx, 50, counting_cb, NULL);

    /* With max_ms=1000, should clamp to ~50ms (give or take) */
    int t = kl_timer_next_timeout(&ctx, 1000);
    ASSERT_TRUE(t <= 55);
    ASSERT_TRUE(t >= 0);

    kl_event_ctx_free(&ctx);
}

UTEST(timer, next_timeout_overdue) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);

    /* Add a timer 1ms from now, then wait for it to expire */
    kl_timer_add(&ctx, 1, counting_cb, NULL);
    usleep(10000);  /* 10ms */

    /* Timer is overdue: should return 0 */
    ASSERT_EQ(kl_timer_next_timeout(&ctx, 1000), 0);

    /* Clean up: fire it */
    kl_timer_fire(&ctx);
    kl_event_ctx_free(&ctx);
}

UTEST(timer, zero_delay_fires_immediately) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);
    cb_called = 0;

    kl_timer_add(&ctx, 0, counting_cb, NULL);

    /* Should fire without any sleep */
    int fired = kl_timer_fire(&ctx);
    ASSERT_EQ(fired, 1);
    ASSERT_EQ(cb_called, 1);

    kl_event_ctx_free(&ctx);
}

/* ── Add from within callback ────────────────────────────────────── */

typedef struct {
    KlEventCtx *ctx;
    int refires;
} RefireCtx;

static void refire_cb(void *user_data) {
    RefireCtx *r = user_data;
    r->refires++;
    if (r->refires < 3)
        kl_timer_add(r->ctx, 0, refire_cb, r);
}

UTEST(timer, add_from_callback) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &alloc), 0);

    RefireCtx rctx = { .ctx = &ctx, .refires = 0 };
    kl_timer_add(&ctx, 0, refire_cb, &rctx);

    /* First fire: triggers refire_cb which adds another 0ms timer */
    int total = 0;
    for (int i = 0; i < 5; i++)
        total += kl_timer_fire(&ctx);

    /* Should have fired 3 times total (initial + 2 re-adds) */
    ASSERT_EQ(rctx.refires, 3);
    ASSERT_EQ(total, 3);

    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
