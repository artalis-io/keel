#include "utest.h"
#include <keel/keel.h>
#include <keel/thread_pool.h>
#include <keel/async.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>

/* ── Helpers (same pattern as test_async.c) ───────────────────────── */

static void dispatch_events(KlEvent *events, int n) {
    for (int i = 0; i < n; i++) {
        uintptr_t tag = (uintptr_t)events[i].udata;
        if (tag & 1) {
            KlWatcher *w = (KlWatcher *)(tag & ~(uintptr_t)1);
            w->on_ready(w->fd, events[i].ready, w->user_data);
        }
    }
}

static void init_test_server(KlServer *s) {
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;
    s->alloc_storage = kl_allocator_default();
    s->loop.alloc = &s->alloc_storage;
}

static void cleanup_test_server(KlServer *s) {
    while (s->watchers) {
        KlWatcher *w = s->watchers;
        s->watchers = w->next;
        kl_event_del(&s->loop, w->fd);
        kl_free(&s->alloc_storage, w, sizeof(KlWatcher));
    }
    kl_event_close(&s->loop);
}

/* Pump the event loop until done_count reaches target or timeout */
static void pump_until(KlServer *s, atomic_int *done_count, int target, int timeout_ms) {
    int elapsed = 0;
    while (atomic_load(done_count) < target && elapsed < timeout_ms) {
        KlEvent events[16];
        int n = kl_event_wait(&s->loop, events, 16, 10);
        if (n > 0) dispatch_events(events, n);
        elapsed += 10;
    }
}

/* ── Test: create and free with no work ───────────────────────────── */

UTEST(thread_pool, create_and_free) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    KlThreadPoolConfig cfg = {.num_workers = 2, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
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
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    atomic_store(&single_work_done, 0);
    atomic_store(&single_done_called, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
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
    usleep(50000); /* 50ms — hold the slot */
}
static void noop_done_fn(void *ud) { (void)ud; }

UTEST(thread_pool, submit_fills_queue) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    /* 1 worker, capacity 4 → done_cap = 5, so inflight limit = 5 */
    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 4};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
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

/* ── Test: work_fn runs on different thread ───────────────────────── */

static pthread_t worker_tid;
static atomic_int worker_tid_set;

static void capture_tid_work(void *ud) {
    (void)ud;
    worker_tid = pthread_self();
    atomic_fetch_add(&worker_tid_set, 1);
}

UTEST(thread_pool, work_executes_on_worker) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    atomic_store(&worker_tid_set, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
    ASSERT_TRUE(pool != NULL);

    atomic_int done_count = 0;
    KlWorkItem item = {
        .work_fn = capture_tid_work,
        .done_fn = single_done_fn,  /* reuse — increments single_done_called */
        .user_data = NULL,
    };

    atomic_store(&single_done_called, 0);
    ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);
    pump_until(&s, &single_done_called, 1, 2000);
    (void)done_count;

    ASSERT_EQ(atomic_load(&worker_tid_set), 1);
    ASSERT_TRUE(!pthread_equal(worker_tid, pthread_self()));

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: done_fn runs on main thread ────────────────────────────── */

static pthread_t done_tid;
static atomic_int done_tid_set;

static void capture_done_tid(void *ud) {
    (void)ud;
    done_tid = pthread_self();
    atomic_fetch_add(&done_tid_set, 1);
}

UTEST(thread_pool, done_runs_on_main) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    atomic_store(&done_tid_set, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
    ASSERT_TRUE(pool != NULL);

    KlWorkItem item = {
        .work_fn = single_work_fn,
        .done_fn = capture_done_tid,
        .user_data = NULL,
    };
    ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);
    pump_until(&s, &done_tid_set, 1, 2000);

    ASSERT_EQ(atomic_load(&done_tid_set), 1);
    ASSERT_TRUE(pthread_equal(done_tid, pthread_self()));

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
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    atomic_store(&fifo_idx, 0);
    atomic_store(&single_done_called, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = FIFO_COUNT};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
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
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    atomic_store(&multi_done_count, 0);

    KlThreadPoolConfig cfg = {.num_workers = 4, .queue_capacity = 32};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
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
    usleep(200000); /* 200ms — hold worker busy */
}

UTEST(thread_pool, shutdown_cancels_pending) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    atomic_store(&cancel_count, 0);

    /* 1 worker, capacity 8 — first item blocks the worker */
    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
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
    usleep(10000);

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

    /* Free immediately — pending items should get cancel_fn called */
    kl_thread_pool_free(pool);

    /* At least some of the 4 pending items should have been cancelled */
    ASSERT_TRUE(atomic_load(&cancel_count) > 0);

    cleanup_test_server(&s);
}

/* ── Test: shutdown waits for running work ─────────────────────────── */

static atomic_int running_completed;

static void slow_complete_work(void *ud) {
    (void)ud;
    usleep(50000); /* 50ms */
    atomic_fetch_add(&running_completed, 1);
}

UTEST(thread_pool, shutdown_waits_for_running) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    atomic_store(&running_completed, 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
    ASSERT_TRUE(pool != NULL);

    KlWorkItem item = {
        .work_fn = slow_complete_work,
        .done_fn = noop_done_fn,
        .user_data = NULL,
    };
    ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);

    /* Let worker pick it up */
    usleep(10000);

    /* Free — should block until the running item completes */
    kl_thread_pool_free(pool);

    ASSERT_EQ(atomic_load(&running_completed), 1);

    cleanup_test_server(&s);
}

/* ── Test: null cancel_fn doesn't crash ───────────────────────────── */

UTEST(thread_pool, null_cancel_fn) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    KlThreadPoolConfig cfg = {.num_workers = 1, .queue_capacity = 8};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
    ASSERT_TRUE(pool != NULL);

    /* Block the worker */
    KlWorkItem blocker = {
        .work_fn = blocking_work_fn,
        .done_fn = noop_done_fn,
        .cancel_fn = NULL,
        .user_data = NULL,
    };
    ASSERT_EQ(kl_thread_pool_submit(pool, &blocker), 0);
    usleep(10000);

    /* Submit with NULL cancel_fn — should not crash on shutdown */
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

/* ── Test: stress — 1000 items ────────────────────────────────────── */

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
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    atomic_store(&stress_done_count, 0);

    KlThreadPoolConfig cfg = {.num_workers = 4, .queue_capacity = 64};
    KlThreadPool *pool = kl_thread_pool_create(&s, &cfg);
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
            /* Queue full — pump event loop to drain done items */
            KlEvent events[16];
            int n = kl_event_wait(&s.loop, events, 16, 10);
            if (n > 0) dispatch_events(events, n);
        }
    }

    pump_until(&s, &stress_done_count, total, 10000);
    ASSERT_EQ(atomic_load(&stress_done_count), total);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

/* ── Test: default config (NULL) ──────────────────────────────────── */

UTEST(thread_pool, default_config) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_init(&s.loop), 0);

    /* NULL config = all defaults */
    KlThreadPool *pool = kl_thread_pool_create(&s, NULL);
    ASSERT_TRUE(pool != NULL);

    kl_thread_pool_free(pool);
    cleanup_test_server(&s);
}

UTEST_MAIN();
