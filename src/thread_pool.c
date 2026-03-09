#include <keel/thread_pool.h>
#include <keel/async.h>
#include <keel/server.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#ifdef __cosmopolitan__
#define KL_TP_DEFAULT_WORKERS 2
#else
#include <errno.h>
#endif

#define KL_TP_DEFAULT_CAPACITY 64

struct KlThreadPool {
    KlServer *server;
    KlAllocator *alloc;

    /* Pipe for cross-thread wakeup */
    int pipe_rd;
    int pipe_wr;

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
    pthread_mutex_t mutex;
    pthread_cond_t work_avail;
    int shutdown;

    /* Workers */
    pthread_t *threads;
    int num_workers;
};

/* ── Worker thread ────────────────────────────────────────────────── */

static void *worker_thread(void *arg)
{
    KlThreadPool *pool = arg;

    pthread_mutex_lock(&pool->mutex);
    while (!pool->shutdown) {
        while (pool->work_count == 0 && !pool->shutdown)
            pthread_cond_wait(&pool->work_avail, &pool->mutex);

        if (pool->shutdown)
            break;

        KlWorkItem item = pool->work_queue[pool->work_head];
        pool->work_head = (pool->work_head + 1) % pool->work_cap;
        pool->work_count--;
        pthread_mutex_unlock(&pool->mutex);

        /* Execute blocking work — no lock held */
        item.work_fn(item.user_data);

        /* Push to done queue */
        pthread_mutex_lock(&pool->mutex);
        pool->done_queue[pool->done_tail] = item;
        pool->done_tail = (pool->done_tail + 1) % pool->done_cap;
        pool->done_count++;
        pthread_mutex_unlock(&pool->mutex);

        /* Signal event loop */
        char c = 1;
        ssize_t wr = write(pool->pipe_wr, &c, 1);
        (void)wr;

        pthread_mutex_lock(&pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);
    return NULL;
}

/* ── Pipe watcher callback (runs on event loop thread) ────────────── */

static void thread_pool_on_pipe(int fd, KlEventMask ready, void *user_data)
{
    (void)ready;
    KlThreadPool *pool = user_data;

    /* Drain pipe — exact count doesn't matter */
    char buf[64];
    ssize_t rd = read(fd, buf, sizeof(buf));
    (void)rd;

    pthread_mutex_lock(&pool->mutex);
    while (pool->done_count > 0) {
        KlWorkItem item = pool->done_queue[pool->done_head];
        pool->done_head = (pool->done_head + 1) % pool->done_cap;
        pool->done_count--;
        pool->inflight--;
        pthread_mutex_unlock(&pool->mutex);

        item.done_fn(item.user_data);

        pthread_mutex_lock(&pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);
}

/* ── Public API ───────────────────────────────────────────────────── */

KlThreadPool *kl_thread_pool_create(KlServer *s, const KlThreadPoolConfig *cfg)
{
    if (!s) return NULL;

    KlAllocator *alloc = (cfg && cfg->alloc) ? cfg->alloc : &s->alloc_storage;
    int num_workers = (cfg && cfg->num_workers > 0) ? cfg->num_workers : 0;
    int queue_cap = (cfg && cfg->queue_capacity > 0) ? cfg->queue_capacity
                                                     : KL_TP_DEFAULT_CAPACITY;

    /* Auto-detect worker count */
    if (num_workers == 0) {
#ifdef KL_TP_DEFAULT_WORKERS
        num_workers = KL_TP_DEFAULT_WORKERS;
#else
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        num_workers = (n > 0) ? (int)n : 1;
#endif
    }

    int done_cap = queue_cap + num_workers;

    KlThreadPool *pool = kl_malloc(alloc, sizeof(KlThreadPool));
    if (!pool) return NULL;
    memset(pool, 0, sizeof(*pool));

    pool->server = s;
    pool->alloc = alloc;
    pool->pipe_rd = -1;
    pool->pipe_wr = -1;
    pool->work_cap = queue_cap;
    pool->done_cap = done_cap;
    pool->num_workers = num_workers;

    /* Allocate queues */
    pool->work_queue = kl_malloc(alloc, (size_t)queue_cap * sizeof(KlWorkItem));
    if (!pool->work_queue) goto fail_pool;

    pool->done_queue = kl_malloc(alloc, (size_t)done_cap * sizeof(KlWorkItem));
    if (!pool->done_queue) goto fail_work;

    /* Create pipe */
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) goto fail_done;
    pool->pipe_rd = pipe_fds[0];
    pool->pipe_wr = pipe_fds[1];

    /* Set read end non-blocking */
    int flags = fcntl(pool->pipe_rd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(pool->pipe_rd, F_SETFL, flags | O_NONBLOCK);

    /* Init synchronization */
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) goto fail_pipe;
    if (pthread_cond_init(&pool->work_avail, NULL) != 0) goto fail_mutex;

    /* Register pipe watcher with event loop */
    if (kl_watcher_add(s, pool->pipe_rd, KL_EVENT_READ,
                       thread_pool_on_pipe, pool) < 0)
        goto fail_cond;

    /* Allocate thread array */
    pool->threads = kl_malloc(alloc, (size_t)num_workers * sizeof(pthread_t));
    if (!pool->threads) goto fail_watcher;

    /* Spawn worker threads */
    int started = 0;
    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0)
            break;
        started++;
    }

    if (started == 0) goto fail_threads;
    pool->num_workers = started;

    return pool;

fail_threads:
    kl_free(alloc, pool->threads, (size_t)num_workers * sizeof(pthread_t));
fail_watcher:
    kl_watcher_del(s, pool->pipe_rd);
fail_cond:
    pthread_cond_destroy(&pool->work_avail);
fail_mutex:
    pthread_mutex_destroy(&pool->mutex);
fail_pipe:
    close(pool->pipe_rd);
    close(pool->pipe_wr);
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

    pthread_mutex_lock(&pool->mutex);

    if (pool->inflight >= pool->done_cap) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    pool->work_queue[pool->work_tail] = *item;
    pool->work_tail = (pool->work_tail + 1) % pool->work_cap;
    pool->work_count++;
    pool->inflight++;

    pthread_cond_signal(&pool->work_avail);
    pthread_mutex_unlock(&pool->mutex);

    return 0;
}

void kl_thread_pool_free(KlThreadPool *pool)
{
    if (!pool) return;

    /* Signal shutdown */
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_avail);
    pthread_mutex_unlock(&pool->mutex);

    /* Join all workers */
    for (int i = 0; i < pool->num_workers; i++)
        pthread_join(pool->threads[i], NULL);

    /* Remove pipe watcher from event loop */
    kl_watcher_del(pool->server, pool->pipe_rd);

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

    /* Close pipe */
    close(pool->pipe_rd);
    close(pool->pipe_wr);

    /* Destroy synchronization */
    pthread_cond_destroy(&pool->work_avail);
    pthread_mutex_destroy(&pool->mutex);

    /* Free allocations */
    KlAllocator *alloc = pool->alloc;
    kl_free(alloc, pool->threads, (size_t)pool->num_workers * sizeof(pthread_t));
    kl_free(alloc, pool->done_queue, (size_t)pool->done_cap * sizeof(KlWorkItem));
    kl_free(alloc, pool->work_queue, (size_t)pool->work_cap * sizeof(KlWorkItem));
    kl_free(alloc, pool, sizeof(KlThreadPool));
}
