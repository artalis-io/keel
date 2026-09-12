#include <keel/thread_pool.h>
#include <keel/event_ctx.h>
#include "platform.h"
#include "allocator_validate.h"   /* kl_allocator_ops_valid: valid-allocator gate */
#include "thread.h"   /* the library's only threading primitives; no pthreads above this */
#include <string.h>
#include <limits.h>
#include <stdint.h>

#ifdef __cosmopolitan__
#define KL_TP_DEFAULT_WORKERS 2
#else
#include <errno.h>
#endif

#define KL_TP_DEFAULT_CAPACITY 64

struct KlThreadPool {
    KlEventCtx *ev_ctx;
    KlAllocator *alloc;

    /* Self-pipe / loopback pair for cross-thread wakeup (platform.h) */
    KlPlatWakeup wakeup;

    /* Work queue: producer = event loop, consumers = workers */
    KlWorkItem *work_queue;
    int work_cap;
    int work_head;
    int work_tail;
    int work_count;

    /* Done queue: producers = workers, consumer = event loop */
    KlWorkItem *done_queue;
    int done_cap;
    int done_head;
    int done_tail;
    int done_count;

    /* Inflight tracking */
    int inflight;

    /* Synchronization */
    KlMutex mutex;
    KlCond work_avail;
    int shutdown;

    /* Workers */
    KlThread *threads;
    int num_workers;
};

/* ── Worker thread ────────────────────────────────────────────────── */

static void worker_thread(void *arg)
{
    KlThreadPool *pool = arg;

    kl_mutex_lock(&pool->mutex);
    while (!pool->shutdown) {
        while (pool->work_count == 0 && !pool->shutdown)
            kl_cond_wait(&pool->work_avail, &pool->mutex);

        if (pool->shutdown)
            break;

        KlWorkItem item = pool->work_queue[pool->work_head];
        pool->work_head = (pool->work_head + 1) % pool->work_cap;
        pool->work_count--;
        kl_mutex_unlock(&pool->mutex);

        /* Execute blocking work; no lock held */
        item.work_fn(item.user_data);

        /* Push to done queue */
        kl_mutex_lock(&pool->mutex);
        pool->done_queue[pool->done_tail] = item;
        pool->done_tail = (pool->done_tail + 1) % pool->done_cap;
        pool->done_count++;
        kl_mutex_unlock(&pool->mutex);

        /* Signal event loop */
        kl_plat_wakeup_signal(&pool->wakeup);

        kl_mutex_lock(&pool->mutex);
    }
    kl_mutex_unlock(&pool->mutex);
}

/* ── Pipe watcher callback (runs on event loop thread) ────────────── */

static void thread_pool_on_pipe(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    (void)ready;
    KlThreadPool *pool = user_data;

    /* Drain wakeup channel; exact count doesn't matter */
    kl_plat_wakeup_drain(fd);

    kl_mutex_lock(&pool->mutex);
    while (pool->done_count > 0) {
        KlWorkItem item = pool->done_queue[pool->done_head];
        pool->done_head = (pool->done_head + 1) % pool->done_cap;
        pool->done_count--;
        pool->inflight--;
        kl_mutex_unlock(&pool->mutex);

        item.done_fn(item.user_data);

        kl_mutex_lock(&pool->mutex);
    }
    kl_mutex_unlock(&pool->mutex);
}

/* ── Public API ───────────────────────────────────────────────────── */

KlThreadPool *kl_thread_pool_create(KlEventCtx *ctx, const KlThreadPoolConfig *cfg)
{
    if (!ctx) return NULL;

    KlAllocator *alloc = (cfg && cfg->alloc) ? cfg->alloc : ctx->alloc;
    /* Validate the RESOLVED allocator (cfg->alloc, or the event context's when NULL);
     * a malformed non-NULL allocator is rejected, not replaced by the default. */
    if (!kl_allocator_ops_valid(alloc)) return NULL;
    int num_workers = (cfg && cfg->num_workers > 0) ? cfg->num_workers : 0;
    int queue_cap = (cfg && cfg->queue_capacity > 0) ? cfg->queue_capacity
                                                     : KL_TP_DEFAULT_CAPACITY;

    /* Auto-detect worker count */
    if (num_workers == 0) {
#ifdef KL_TP_DEFAULT_WORKERS
        num_workers = KL_TP_DEFAULT_WORKERS;
#else
        num_workers = kl_plat_cpu_count();
#endif
    }

    if (queue_cap > INT_MAX / 2 || num_workers > INT_MAX / 2)
        return NULL;
    int done_cap = queue_cap + num_workers;

    if ((size_t)queue_cap > SIZE_MAX / sizeof(KlWorkItem)) return NULL;
    if ((size_t)done_cap > SIZE_MAX / sizeof(KlWorkItem)) return NULL;
    if ((size_t)num_workers > SIZE_MAX / sizeof(KlThread)) return NULL;

    KlThreadPool *pool = kl_malloc(alloc, sizeof(KlThreadPool));
    if (!pool) {
        ctx->last_error = KL_ERR_ALLOC;
        return NULL;
    }
    memset(pool, 0, sizeof(*pool));

    pool->ev_ctx = ctx;
    pool->alloc = alloc;
    pool->wakeup.rd = KL_INVALID_SOCKET;
    pool->wakeup.wr = KL_INVALID_SOCKET;
    pool->work_cap = queue_cap;
    pool->done_cap = done_cap;
    pool->num_workers = num_workers;

    /* Allocate queues */
    pool->work_queue = kl_malloc(alloc, (size_t)queue_cap * sizeof(KlWorkItem));
    if (!pool->work_queue) { ctx->last_error = KL_ERR_ALLOC; goto fail_pool; }

    pool->done_queue = kl_malloc(alloc, (size_t)done_cap * sizeof(KlWorkItem));
    if (!pool->done_queue) { ctx->last_error = KL_ERR_ALLOC; goto fail_work; }

    /* Create cross-thread wakeup channel (read end already non-blocking) */
    if (kl_plat_wakeup_open(&pool->wakeup) < 0) { ctx->last_error = KL_ERR_PIPE; goto fail_done; }

    /* Init synchronization */
    if (kl_mutex_init(&pool->mutex) != 0) { ctx->last_error = KL_ERR_THREAD; goto fail_pipe; }
    if (kl_cond_init(&pool->work_avail) != 0) { ctx->last_error = KL_ERR_THREAD; goto fail_mutex; }

    /* Register wakeup watcher with event loop */
    if (kl_watcher_add(ctx, pool->wakeup.rd, KL_EVENT_READ,
                       thread_pool_on_pipe, pool) < 0) {
        ctx->last_error = KL_ERR_EVENT_ADD;
        goto fail_cond;
    }

    /* Allocate thread array */
    pool->threads = kl_malloc(alloc, (size_t)num_workers * sizeof(KlThread));
    if (!pool->threads) { ctx->last_error = KL_ERR_ALLOC; goto fail_watcher; }

    /* Spawn worker threads */
    int started = 0;
    for (int i = 0; i < num_workers; i++) {
        if (kl_thread_create(&pool->threads[i], worker_thread, pool) != 0)
            break;
        started++;
    }

    if (started == 0) { ctx->last_error = KL_ERR_THREAD; goto fail_threads; }
    pool->num_workers = started;

    return pool;

fail_threads:
    kl_free(alloc, pool->threads, (size_t)num_workers * sizeof(KlThread));
fail_watcher:
    kl_watcher_del(ctx, pool->wakeup.rd);
fail_cond:
    kl_cond_destroy(&pool->work_avail);
fail_mutex:
    kl_mutex_destroy(&pool->mutex);
fail_pipe:
    kl_plat_wakeup_close(&pool->wakeup);
fail_done:
    kl_free(alloc, pool->done_queue, (size_t)done_cap * sizeof(KlWorkItem));
fail_work:
    kl_free(alloc, pool->work_queue, (size_t)queue_cap * sizeof(KlWorkItem));
fail_pool:
    kl_free(alloc, pool, sizeof(KlThreadPool));
    return NULL;
}

int kl_thread_pool_submit(KlThreadPool *pool, const KlWorkItem *item)
{
    if (!pool || !item || !item->work_fn || !item->done_fn) return -1;

    kl_mutex_lock(&pool->mutex);

    if (pool->inflight >= pool->done_cap) {
        kl_mutex_unlock(&pool->mutex);
        return -1;
    }

    pool->work_queue[pool->work_tail] = *item;
    pool->work_tail = (pool->work_tail + 1) % pool->work_cap;
    pool->work_count++;
    pool->inflight++;

    kl_cond_signal(&pool->work_avail);
    kl_mutex_unlock(&pool->mutex);

    return 0;
}

void kl_thread_pool_free(KlThreadPool *pool)
{
    if (!pool) return;

    /* Signal shutdown */
    kl_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    kl_cond_broadcast(&pool->work_avail);
    kl_mutex_unlock(&pool->mutex);

    /* Join all workers */
    for (int i = 0; i < pool->num_workers; i++)
        kl_thread_join(&pool->threads[i]);

    /* Remove wakeup watcher from event loop */
    kl_watcher_del(pool->ev_ctx, pool->wakeup.rd);

    /* Drain remaining done queue items → call done_fn */
    while (pool->done_count > 0) {
        KlWorkItem item = pool->done_queue[pool->done_head];
        pool->done_head = (pool->done_head + 1) % pool->done_cap;
        pool->done_count--;
        item.done_fn(item.user_data);
    }

    /* Drain remaining work queue items → call cancel_fn */
    while (pool->work_count > 0) {
        KlWorkItem item = pool->work_queue[pool->work_head];
        pool->work_head = (pool->work_head + 1) % pool->work_cap;
        pool->work_count--;
        if (item.cancel_fn)
            item.cancel_fn(item.user_data);
    }

    /* Close wakeup channel */
    kl_plat_wakeup_close(&pool->wakeup);

    /* Destroy synchronization */
    kl_cond_destroy(&pool->work_avail);
    kl_mutex_destroy(&pool->mutex);

    /* Free allocations */
    KlAllocator *alloc = pool->alloc;
    kl_free(alloc, pool->threads, (size_t)pool->num_workers * sizeof(KlThread));
    kl_free(alloc, pool->done_queue, (size_t)pool->done_cap * sizeof(KlWorkItem));
    kl_free(alloc, pool->work_queue, (size_t)pool->work_cap * sizeof(KlWorkItem));
    kl_free(alloc, pool, sizeof(KlThreadPool));
}
