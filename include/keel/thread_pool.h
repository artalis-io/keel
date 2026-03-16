#ifndef KEEL_THREAD_POOL_H
#define KEEL_THREAD_POOL_H

#include <keel/allocator.h>

typedef struct KlEventCtx KlEventCtx;

/** @brief Work function — runs on a worker thread. */
typedef void (*KlWorkFn)(void *user_data);
/** @brief Done function — runs on the event loop thread after work completes. */
typedef void (*KlWorkDoneFn)(void *user_data);
/** @brief Cancel function — runs on shutdown if work item never started. */
typedef void (*KlWorkCancelFn)(void *user_data);

/**
 * @brief Work item — passed by pointer to kl_thread_pool_submit.
 * Copied into the internal queue (caller can stack-allocate).
 */
typedef struct KlWorkItem {
    KlWorkFn work_fn;           /**< Runs on worker thread */
    KlWorkDoneFn done_fn;       /**< Runs on event loop thread */
    KlWorkCancelFn cancel_fn;   /**< Runs on shutdown if item never started (may be NULL) */
    void *user_data;            /**< Opaque — passed to all three callbacks */
} KlWorkItem;

/**
 * @brief Thread pool configuration.
 * All fields have sensible defaults when set to 0/NULL.
 */
typedef struct KlThreadPoolConfig {
    int num_workers;          /**< 0 = auto-detect via sysconf(_SC_NPROCESSORS_ONLN) */
    int queue_capacity;       /**< Max items in work queue (0 = 64) */
    KlAllocator *alloc;       /**< NULL = ctx's allocator */
} KlThreadPoolConfig;

typedef struct KlThreadPool KlThreadPool;

/**
 * @brief Create pool, start worker threads, register pipe watcher with ctx.
 * @param ctx  Event context (for event loop + allocator).
 * @param cfg  Configuration (NULL = all defaults).
 * @return Pool on success, NULL on failure.
 */
KlThreadPool *kl_thread_pool_create(KlEventCtx *ctx, const KlThreadPoolConfig *cfg);

/**
 * @brief Submit work. Copies *item into the internal queue.
 * @param pool Thread pool.
 * @param item Work item (copied — caller can stack-allocate).
 * @return 0 on success, -1 if queue full (backpressure).
 */
int kl_thread_pool_submit(KlThreadPool *pool, const KlWorkItem *item);

/**
 * @brief Drain queue, join threads, remove watcher, free.
 */
void kl_thread_pool_free(KlThreadPool *pool);

#endif
