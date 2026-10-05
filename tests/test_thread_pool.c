#include "utest.h"
#include <keel/keel.h>
#include <keel/thread_pool.h>
#include <keel/async.h>
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include "net_compat.h"        /* kl_test_thread_id */
#if !defined(_MSC_VER)
#include <unistd.h>
#endif   /* MSVC has no <unistd.h>; the harness helpers cover it */
#include <string.h>
#include <stdatomic.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

static void init_test_server(KlHttpServer *s) {
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;
    s->alloc_storage = kl_allocator_default();
    s->ev.alloc = &s->alloc_storage;
}

static void cleanup_test_server(KlHttpServer *s) {
    kl_event_ctx_free(&s->ev);
}

/* Pump the event loop until done_count reaches target or timeout */
static void pump_until(KlHttpServer *s, atomic_int *done_count, int target, int timeout_ms) {
    int elapsed = 0;
    while (atomic_load(done_count) < target && elapsed < timeout_ms) {
        kl_event_ctx_run(&s->ev, 16, 10);
        elapsed += 10;
    }
}

/* ── Test: create and free with no work ───────────────────────────── */

UTEST(thread_pool, create_and_free) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    KlThreadPoolConfig cfg = {.num_workers = 2, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: submit single item ─────────────────────────────────────── */

static atomic_int single_work_done;
static atomic_int single_done_called;

static void single_work_fn(void *ud) {
    (void)ud;
    atomic_fetch_add(&single_work_done, 1);
}
static void single_done_fn(void *ud) {
    (void)ud;
    atomic_fetch_add(&single_done_called, 1);
}

UTEST(thread_pool, submit_single) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    atomic_store(&single_work_done, 0);
    atomic_store(&single_done_called, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    KlWorkItem item = {
        .work_fn = single_work_fn,
        .done_fn = single_done_fn,
        .user_data = NULL,
    };
    ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);

    pump_until(&s, &single_done_called, 1, 2000);

    ASSERT_EQ(atomic_load(&single_work_done), 1);
    ASSERT_EQ(atomic_load(&single_done_called), 1);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: queue full returns -1 ──────────────────────────────────── */

static void slow_work_fn(void *ud) {
    (void)ud;
    kl_test_sleep_ms(50); /* 50ms: hold the slot */
}
static void noop_done_fn(void *ud) { (void)ud; }

UTEST(thread_pool, submit_fills_queue) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    /* 1 worker, capacity 4 → done_cap = 5, so inflight limit = 5 */
    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 4};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    KlWorkItem item = {
        .work_fn = slow_work_fn,
        .done_fn = noop_done_fn,
        .user_data = NULL,
    };

    /* Fill up to done_cap (4 + 1 = 5) */
    int submitted = 0;
    for (int i = 0; i < 10; i++) {
        if (kl_thread_pool_submit(pool, &item) == 0)
            submitted++;
        else
            break;
    }
    ASSERT_EQ(submitted, 5);

    /* Next submit should fail */
    ASSERT_EQ(kl_thread_pool_submit(pool, &item), -1);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: a burst before the workers wake runs every item exactly once ── */

/* Admission allows queue_capacity + num_workers items in flight. If the workers have not woken to
 * dequeue yet, all of them sit in the work queue at once, so the queue must hold that many: a ring
 * sized to queue_capacity alone wraps over items not yet taken, losing some and running others twice. */
#define BURST_WORKERS 8
#define BURST_QUEUE   1
#define BURST_MAX     (BURST_WORKERS + BURST_QUEUE)
#define BURST_ROUNDS  200

static atomic_int burst_runs[BURST_MAX];
static atomic_int burst_dones[BURST_MAX];
static atomic_int burst_done_total;

static void burst_work_fn(void *ud) { atomic_fetch_add(&burst_runs[(size_t)ud], 1); }
static void burst_done_fn(void *ud) {
    atomic_fetch_add(&burst_dones[(size_t)ud], 1);
    atomic_fetch_add(&burst_done_total, 1);
}

UTEST(thread_pool, burst_before_workers_wake_runs_each_item_once) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    KlThreadPoolConfig cfg = {.num_workers = BURST_WORKERS, .queue_capacity = BURST_QUEUE};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    int bad_rounds = 0;
    for (int round = 0; round < BURST_ROUNDS; round++) {
        for (int i = 0; i < BURST_MAX; i++) {
            atomic_store(&burst_runs[i], 0);
            atomic_store(&burst_dones[i], 0);
        }
        atomic_store(&burst_done_total, 0);

        int accepted = 0;
        for (size_t i = 0; i < BURST_MAX; i++) {
            KlWorkItem item = {.work_fn = burst_work_fn, .done_fn = burst_done_fn,
                               .user_data = (void *)i};
            if (kl_thread_pool_submit(pool, &item) == 0) accepted++;
        }
        pump_until(&s, &burst_done_total, accepted, 2000);
        kl_event_ctx_run(&s.ev, 16, 20);   /* let a doubled item show up as an extra completion */

        for (int i = 0; i < accepted; i++)
            if (atomic_load(&burst_runs[i]) != 1 || atomic_load(&burst_dones[i]) != 1) {
                bad_rounds++;
                break;
            }
        if (bad_rounds) break;
    }

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
    ASSERT_EQ(bad_rounds, 0);
}

/* ── Test: work_fn runs on different thread ───────────────────────── */

static KlTestThreadId worker_tid;
static atomic_int worker_tid_set;

static void capture_tid_work(void *ud) {
    (void)ud;
    worker_tid = kl_test_thread_id();
    atomic_fetch_add(&worker_tid_set, 1);
}

UTEST(thread_pool, work_executes_on_worker) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    atomic_store(&worker_tid_set, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    atomic_int done_count = 0;
    KlWorkItem item = {
        .work_fn = capture_tid_work,
        .done_fn = single_done_fn,  /* reuse: increments single_done_called */
        .user_data = NULL,
    };

    atomic_store(&single_done_called, 0);
    ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);
    pump_until(&s, &single_done_called, 1, 2000);
    (void)done_count;

    ASSERT_EQ(atomic_load(&worker_tid_set), 1);
    ASSERT_NE(worker_tid, kl_test_thread_id());   /* work_fn ran on a worker, not here */

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: done_fn runs on main thread ────────────────────────────── */

static KlTestThreadId done_tid;
static atomic_int done_tid_set;

static void capture_done_tid(void *ud) {
    (void)ud;
    done_tid = kl_test_thread_id();
    atomic_fetch_add(&done_tid_set, 1);
}

UTEST(thread_pool, done_runs_on_main) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    atomic_store(&done_tid_set, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    KlWorkItem item = {
        .work_fn = single_work_fn,
        .done_fn = capture_done_tid,
        .user_data = NULL,
    };
    ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);
    pump_until(&s, &done_tid_set, 1, 2000);

    ASSERT_EQ(atomic_load(&done_tid_set), 1);
    ASSERT_EQ(done_tid, kl_test_thread_id());     /* done_fn ran on the event-loop thread */

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: FIFO ordering with single worker ───────────────────────── */

#define FIFO_COUNT 16
static int fifo_order[FIFO_COUNT];
static atomic_int fifo_idx;

static void fifo_work_fn(void *ud) {
    int id = *(int *)ud;
    int idx = atomic_fetch_add(&fifo_idx, 1);
    fifo_order[idx] = id;
}

UTEST(thread_pool, ordering_fifo) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    atomic_store(&fifo_idx, 0);
    atomic_store(&single_done_called, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = FIFO_COUNT};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    int ids[FIFO_COUNT];
    for (int i = 0; i < FIFO_COUNT; i++) {
        ids[i] = i;
        KlWorkItem item = {
            .work_fn = fifo_work_fn,
            .done_fn = single_done_fn,
            .user_data = &ids[i],
        };
        ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);
    }

    pump_until(&s, &single_done_called, FIFO_COUNT, 5000);

    ASSERT_EQ(atomic_load(&fifo_idx), FIFO_COUNT);
    for (int i = 0; i < FIFO_COUNT; i++)
        ASSERT_EQ(fifo_order[i], i);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: multiple workers complete all items ────────────────────── */

static atomic_int multi_done_count;

static void multi_done_fn(void *ud) {
    (void)ud;
    atomic_fetch_add(&multi_done_count, 1);
}

UTEST(thread_pool, multiple_workers) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    atomic_store(&multi_done_count, 0);

    KlThreadPoolConfig cfg = {.num_workers = 4, .queue_capacity = 32};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    for (int i = 0; i < 32; i++) {
        KlWorkItem item = {
            .work_fn = single_work_fn,
            .done_fn = multi_done_fn,
            .user_data = NULL,
        };
        ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);
    }

    pump_until(&s, &multi_done_count, 32, 5000);
    ASSERT_EQ(atomic_load(&multi_done_count), 32);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: shutdown cancels pending items ─────────────────────────── */

static atomic_int cancel_count;

static void cancel_fn(void *ud) {
    (void)ud;
    atomic_fetch_add(&cancel_count, 1);
}

static void blocking_work_fn(void *ud) {
    (void)ud;
    kl_test_sleep_ms(200); /* 200ms: hold worker busy */
}

UTEST(thread_pool, shutdown_cancels_pending) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    atomic_store(&cancel_count, 0);

    /* 1 worker, capacity 8: first item blocks the worker */
    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    /* First item blocks the worker for 200ms */
    KlWorkItem blocker = {
        .work_fn = blocking_work_fn,
        .done_fn = noop_done_fn,
        .cancel_fn = cancel_fn,
        .user_data = NULL,
    };
    ASSERT_EQ(kl_thread_pool_submit(pool, &blocker), 0);

    /* Let worker pick up the blocker */
    kl_test_sleep_ms(10);

    /* Submit more items that will be pending when we free */
    for (int i = 0; i < 4; i++) {
        KlWorkItem item = {
            .work_fn = single_work_fn,
            .done_fn = noop_done_fn,
            .cancel_fn = cancel_fn,
            .user_data = NULL,
        };
        ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);
    }

    /* Free immediately: pending items should get cancel_fn called */
    kl_thread_pool_free(pool);

    /* At least some of the 4 pending items should have been cancelled */
    ASSERT_TRUE(atomic_load(&cancel_count) > 0);

    cleanup_test_server(&s);
}

/* ── Test: shutdown waits for running work ─────────────────────────── */

static atomic_int running_completed;

static void slow_complete_work(void *ud) {
    (void)ud;
    kl_test_sleep_ms(50); /* 50ms */
    atomic_fetch_add(&running_completed, 1);
}

UTEST(thread_pool, shutdown_waits_for_running) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    atomic_store(&running_completed, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    KlWorkItem item = {
        .work_fn = slow_complete_work,
        .done_fn = noop_done_fn,
        .user_data = NULL,
    };
    ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);

    /* Let worker pick it up */
    kl_test_sleep_ms(10);

    /* Free: should block until the running item completes */
    kl_thread_pool_free(pool);

    ASSERT_EQ(atomic_load(&running_completed), 1);

    cleanup_test_server(&s);
}

/* ── Test: null cancel_fn doesn't crash ───────────────────────────── */

UTEST(thread_pool, null_cancel_fn) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    /* Block the worker */
    KlWorkItem blocker = {
        .work_fn = blocking_work_fn,
        .done_fn = noop_done_fn,
        .cancel_fn = NULL,
        .user_data = NULL,
    };
    ASSERT_EQ(kl_thread_pool_submit(pool, &blocker), 0);
    kl_test_sleep_ms(10);

    /* Submit with NULL cancel_fn: should not crash on shutdown */
    KlWorkItem item = {
        .work_fn = single_work_fn,
        .done_fn = noop_done_fn,
        .cancel_fn = NULL,
        .user_data = NULL,
    };
    ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: stress: 1000 items ────────────────────────────────────── */

static atomic_int stress_done_count;

static void stress_work_fn(void *ud) {
    (void)ud;
    /* Trivial work */
}

static void stress_done_fn(void *ud) {
    (void)ud;
    atomic_fetch_add(&stress_done_count, 1);
}

UTEST(thread_pool, stress_many_items) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    atomic_store(&stress_done_count, 0);

    KlThreadPoolConfig cfg = {.num_workers = 4, .queue_capacity = 64};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &cfg);
    ASSERT_TRUE(pool != NULL);

    int total = 1000;
    int submitted = 0;
    while (submitted < total) {
        KlWorkItem item = {
            .work_fn = stress_work_fn,
            .done_fn = stress_done_fn,
            .user_data = NULL,
        };
        if (kl_thread_pool_submit(pool, &item) == 0) {
            submitted++;
        } else {
            /* Queue full: pump event loop to drain done items */
            kl_event_ctx_run(&s.ev, 16, 10);
        }
    }

    pump_until(&s, &stress_done_count, total, 10000);
    ASSERT_EQ(atomic_load(&stress_done_count), total);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: default config (NULL) ──────────────────────────────────── */

UTEST(thread_pool, default_config) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    /* NULL config = all defaults */
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, NULL);
    ASSERT_TRUE(pool != NULL);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

typedef struct {
    KlThreadPool *pool;
    int calls;
    int refused;
} FreeFromDone;

static void free_from_done(void *ud) {
    FreeFromDone *state = ud;
    state->calls++;
    kl_thread_pool_free(state->pool);
    /* Repeated free requests from teardown callbacks must not recursively destroy the pool. */
    kl_thread_pool_free(state->pool);
    KlWorkItem item = { .work_fn = single_work_fn, .done_fn = noop_done_fn };
    if (kl_thread_pool_submit(state->pool, &item) < 0) state->refused++;
}

UTEST(thread_pool, destruction_from_done_is_deferred_and_drains_remaining_work) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(0, kl_event_ctx_init(&ev, &alloc));
    KlThreadPoolConfig cfg = { .num_workers = 1, .queue_capacity = 8 };
    FreeFromDone state = {0};
    state.pool = kl_thread_pool_create(&ev, &cfg);
    ASSERT_TRUE(state.pool != NULL);
    KlWorkItem item = { .work_fn = single_work_fn, .done_fn = free_from_done,
                       .cancel_fn = free_from_done, .user_data = &state };
    for (int i = 0; i < 4; i++) ASSERT_EQ(0, kl_thread_pool_submit(state.pool, &item));
    for (int i = 0; i < 200 && state.calls == 0; i++) kl_event_ctx_run(&ev, 16, 10);
    int calls = state.calls;
    int refused = state.refused;
    kl_event_ctx_free(&ev);
    ASSERT_EQ(4, calls);
    ASSERT_EQ(4, refused);
}

UTEST_MAIN();
