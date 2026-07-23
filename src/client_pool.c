/*
 * client_pool.c — HTTP client connection pool
 *
 * Flat array of KlClientPoolEntry with linear scan. Pool sizes are
 * small (32-128 max), so linear scan is cache-friendly and simpler
 * than a hash map.
 */

#include <keel/client_pool.h>
#include <keel/connection.h>  /* kl_monotonic_ms */
#include <keel/timer.h>
#include "socket.h"

#include <errno.h>
#include <string.h>

/* Socket provider for pooled fds (NULL -> POSIX default close). */
static inline const KlSocketProvider *cpool_sp(const KlClientPool *pool) {
    return (pool && pool->ev_ctx) ? pool->ev_ctx->sockets : NULL;
}

/* ── Entry helpers ───────────────────────────────────────────────── */

static int entry_matches(const KlClientPoolEntry *e,
                          const char *host, int port, int is_tls,
                          const char *proxy_host, int proxy_port)
{
    if (!kl_handle_valid(e->fd) || e->port != port || e->is_tls != is_tls ||
        strcmp(e->host, host) != 0)
        return 0;

    /* Proxy key comparison */
    if (proxy_host)
        return e->proxy_port == proxy_port &&
               strcmp(e->proxy_host, proxy_host) == 0;

    /* Direct: match only direct entries */
    return e->proxy_host[0] == '\0';
}

static void entry_close(KlClientPoolEntry *e)
{
    if (e->tls) {
        e->tls->shutdown(e->tls, e->fd);
        e->tls->destroy(e->tls);
        e->tls = NULL;
    }
    if (kl_handle_valid(e->fd)) {
        kl_sock_close(cpool_sp(e->pool), e->fd);
        e->fd = KL_INVALID_SOCKET;
    }
}

/* ── Idle timer callback ─────────────────────────────────────────── */

static void idle_timer_cb(void *user_data)
{
    KlClientPoolEntry *e = user_data;
    KlClientPool *pool = e->pool;

    /* Entry may have been acquired/evicted since timer was scheduled */
    if (kl_handle_valid(e->fd)) {
        entry_close(e);
        e->timer_id = -1;
        pool->active--;
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

int kl_cpool_init(KlClientPool *pool, const KlClientPoolConfig *cfg,
                   KlAllocator *alloc, KlEventCtx *ev_ctx)
{
    if (!pool || !alloc)
        return -1;

    memset(pool, 0, sizeof(*pool));

    int cap = (cfg && cfg->capacity > 0) ? cfg->capacity
                                          : KL_CPOOL_DEFAULT_CAPACITY;
    int mph = (cfg && cfg->max_per_host > 0) ? cfg->max_per_host
                                               : KL_CPOOL_DEFAULT_MAX_PER_HOST;
    uint64_t idle = (cfg && cfg->idle_ms > 0) ? cfg->idle_ms
                                                : KL_CPOOL_DEFAULT_IDLE_MS;

    /* Overflow guard */
    if ((size_t)cap > SIZE_MAX / sizeof(KlClientPoolEntry)) {
        pool->last_error = KL_ERR_OVERFLOW;
        return -1;
    }

    KlClientPoolEntry *entries = kl_malloc(alloc,
                                            (size_t)cap * sizeof(KlClientPoolEntry));
    if (!entries) {
        pool->last_error = KL_ERR_ALLOC;
        return -1;
    }

    /* Mark all slots free. The pool back-pointer is set here (not just on
     * insert) so entry_close -> cpool_sp(e->pool) is always valid, even for a
     * slot that never held a connection. */
    for (int i = 0; i < cap; i++) {
        entries[i].fd = KL_INVALID_SOCKET;
        entries[i].tls = NULL;
        entries[i].timer_id = -1;
        entries[i].host[0] = '\0';
        entries[i].proxy_host[0] = '\0';
        entries[i].proxy_port = 0;
        entries[i].pool = pool;
    }

    pool->entries = entries;
    pool->capacity = cap;
    pool->active = 0;
    pool->max_per_host = mph;
    pool->idle_ms = idle;
    pool->alloc = alloc;
    pool->ev_ctx = ev_ctx;
    pool->last_error = KL_ERR_NONE;

    return 0;
}

void kl_cpool_free(KlClientPool *pool)
{
    if (!pool || !pool->entries)
        return;

    for (int i = 0; i < pool->capacity; i++) {
        if (kl_handle_valid(pool->entries[i].fd)) {
            /* Cancel idle timer if ev_ctx is available */
            if (pool->ev_ctx && pool->entries[i].timer_id >= 0)
                kl_timer_cancel(pool->ev_ctx, pool->entries[i].timer_id);
            entry_close(&pool->entries[i]);
        }
    }

    kl_free(pool->alloc, pool->entries,
            (size_t)pool->capacity * sizeof(KlClientPoolEntry));
    pool->entries = NULL;
    pool->active = 0;
}

/* ── Acquire (test-on-borrow) ────────────────────────────────────── */

int kl_cpool_acquire(KlClientPool *pool, const char *host, int port,
                      int is_tls, const char *proxy_host, int proxy_port,
                      KlClientPoolConn *conn)
{
    if (!pool || !host || !conn)
        return -1;

    for (int i = 0; i < pool->capacity; i++) {
        KlClientPoolEntry *e = &pool->entries[i];
        if (!entry_matches(e, host, port, is_tls, proxy_host, proxy_port))
            continue;

        /* Test-on-borrow: peek for a peer close. Ensure the fd is non-blocking
         * first (it already is in production — the async client sets it) so the
         * MSG_PEEK returns EAGAIN on an idle connection instead of blocking. This
         * is the portable replacement for the old per-call MSG_DONTWAIT, which
         * Winsock lacks. */
        (void)kl_sock_set_nonblocking(cpool_sp(pool), e->fd);
        char peek;
        ssize_t r = kl_sock_recv_peek(cpool_sp(pool), e->fd, &peek, 1);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            /* Stale connection — close and skip */
            if (pool->ev_ctx && e->timer_id >= 0)
                kl_timer_cancel(pool->ev_ctx, e->timer_id);
            entry_close(e);
            e->timer_id = -1;
            pool->active--;
            continue;
        }

        /* Hit — cancel idle timer, populate conn, clear slot */
        if (pool->ev_ctx && e->timer_id >= 0)
            kl_timer_cancel(pool->ev_ctx, e->timer_id);

        conn->fd = e->fd;
        conn->tls = e->tls;
        conn->reused = 1;
        conn->_entry = NULL;

        e->fd = KL_INVALID_SOCKET;
        e->tls = NULL;
        e->timer_id = -1;
        e->host[0] = '\0';
        pool->active--;

        return 0;  /* hit */
    }

    return 1;  /* miss */
}

/* ── Release ─────────────────────────────────────────────────────── */

int kl_cpool_release(KlClientPool *pool, KlClientPoolConn *conn,
                      const char *host, int port, int is_tls,
                      const char *proxy_host, int proxy_port)
{
    if (!pool || !conn || !host || !kl_handle_valid(conn->fd))
        return -1;

    size_t hlen = strlen(host);
    if (hlen >= KL_CLIENT_HOSTNAME_MAX) {
        pool->last_error = KL_ERR_INVALID_ARG;
        kl_cpool_discard(pool, conn);
        return -1;
    }

    if (proxy_host && strlen(proxy_host) >= KL_CLIENT_HOSTNAME_MAX) {
        pool->last_error = KL_ERR_INVALID_ARG;
        kl_cpool_discard(pool, conn);
        return -1;
    }

    /* Enforce max_per_host — evict oldest for this host if at limit */
    int host_count = 0;
    int oldest_idx = -1;
    uint64_t oldest_time = UINT64_MAX;

    for (int i = 0; i < pool->capacity; i++) {
        const KlClientPoolEntry *e = &pool->entries[i];
        if (entry_matches(e, host, port, is_tls, proxy_host, proxy_port)) {
            host_count++;
            if (e->idle_since_ms < oldest_time) {
                oldest_time = e->idle_since_ms;
                oldest_idx = i;
            }
        }
    }

    if (host_count >= pool->max_per_host && oldest_idx >= 0) {
        KlClientPoolEntry *e = &pool->entries[oldest_idx];
        if (pool->ev_ctx && e->timer_id >= 0)
            kl_timer_cancel(pool->ev_ctx, e->timer_id);
        entry_close(e);
        e->timer_id = -1;
        e->host[0] = '\0';
        pool->active--;
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < pool->capacity; i++) {
        if (!kl_handle_valid(pool->entries[i].fd)) {
            slot = i;
            break;
        }
    }

    /* Pool full — evict globally oldest idle connection */
    if (slot < 0) {
        oldest_idx = -1;
        oldest_time = UINT64_MAX;
        for (int i = 0; i < pool->capacity; i++) {
            const KlClientPoolEntry *e = &pool->entries[i];
            if (kl_handle_valid(e->fd) && e->idle_since_ms < oldest_time) {
                oldest_time = e->idle_since_ms;
                oldest_idx = i;
            }
        }
        if (oldest_idx >= 0) {
            KlClientPoolEntry *e = &pool->entries[oldest_idx];
            if (pool->ev_ctx && e->timer_id >= 0)
                kl_timer_cancel(pool->ev_ctx, e->timer_id);
            entry_close(e);
            e->timer_id = -1;
            pool->active--;
            slot = oldest_idx;
        }
    }

    if (slot < 0) {
        /* Should not happen — pool is fully occupied with no evictable entry */
        kl_cpool_discard(pool, conn);
        return -1;
    }

    /* Populate slot */
    KlClientPoolEntry *e = &pool->entries[slot];
    memcpy(e->host, host, hlen + 1);
    e->port = port;
    e->is_tls = is_tls;
    if (proxy_host) {
        size_t plen = strlen(proxy_host);
        memcpy(e->proxy_host, proxy_host, plen + 1);
        e->proxy_port = proxy_port;
    } else {
        e->proxy_host[0] = '\0';
        e->proxy_port = 0;
    }
    e->fd = conn->fd;
    e->tls = conn->tls;
    e->idle_since_ms = kl_monotonic_ms();
    e->timer_id = -1;

    /* Schedule idle timer if ev_ctx is available */
    if (pool->ev_ctx) {
        e->pool = pool;
        e->timer_id = kl_timer_add(pool->ev_ctx, pool->idle_ms,
                                    idle_timer_cb, e);
    }

    pool->active++;

    /* Clear the conn handle */
    conn->fd = KL_INVALID_SOCKET;
    conn->tls = NULL;
    conn->reused = 0;
    conn->_entry = NULL;

    return 0;
}

/* ── Discard ─────────────────────────────────────────────────────── */

/* cppcheck-suppress constParameterPointer ; kept non-const for API symmetry
   with the rest of the kl_cpool_* family (which do mutate pool) */
void kl_cpool_discard(KlClientPool *pool, KlClientPoolConn *conn)
{
    if (!conn)
        return;

    if (conn->tls) {
        if (kl_handle_valid(conn->fd))
            conn->tls->shutdown(conn->tls, conn->fd);
        conn->tls->destroy(conn->tls);
        conn->tls = NULL;
    }
    if (kl_handle_valid(conn->fd)) {
        kl_sock_close(cpool_sp(pool), conn->fd);
        conn->fd = KL_INVALID_SOCKET;
    }
    conn->reused = 0;
    conn->_entry = NULL;
}

/* ── Maintenance ─────────────────────────────────────────────────── */

int kl_cpool_evict_expired(KlClientPool *pool)
{
    if (!pool || !pool->entries)
        return 0;

    uint64_t now = kl_monotonic_ms();
    int evicted = 0;

    for (int i = 0; i < pool->capacity; i++) {
        KlClientPoolEntry *e = &pool->entries[i];
        if (!kl_handle_valid(e->fd))
            continue;

        /* Defensive: `now >= e->idle_since_ms` should always hold
         * because `kl_monotonic_ms` is monotonic, but explicit guard
         * means a hypothetical clock-skew event (kernel bug, CLOCK_*
         * choice change, suspended-VM resume bugs) can't underflow
         * unsigned subtraction and evict every live entry. */
        if (now >= e->idle_since_ms &&
            now - e->idle_since_ms >= pool->idle_ms) {
            if (pool->ev_ctx && e->timer_id >= 0)
                kl_timer_cancel(pool->ev_ctx, e->timer_id);
            entry_close(e);
            e->timer_id = -1;
            e->host[0] = '\0';
            pool->active--;
            evicted++;
        }
    }

    return evicted;
}

int kl_cpool_idle_count(const KlClientPool *pool)
{
    if (!pool)
        return 0;
    return pool->active;
}

int kl_cpool_host_count(const KlClientPool *pool, const char *host,
                         int port, int is_tls,
                         const char *proxy_host, int proxy_port)
{
    if (!pool || !host)
        return 0;

    int count = 0;
    for (int i = 0; i < pool->capacity; i++) {
        const KlClientPoolEntry *e = &pool->entries[i];
        if (entry_matches(e, host, port, is_tls, proxy_host, proxy_port))
            count++;
    }
    return count;
}
