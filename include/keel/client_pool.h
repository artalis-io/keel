/**
 * @file client_pool.h
 * @brief HTTP client connection pool.
 *
 * Caches idle TCP+TLS connections keyed by (host, port, is_tls),
 * enabling HTTP keep-alive reuse across requests. Opt-in: pass a
 * KlClientPool to the pooled request variants.
 *
 * Pool sizes are small (32-128), so entries use a flat array with
 * linear scan — cache-friendly and simpler than a hash map.
 */

#ifndef KEEL_CLIENT_POOL_H
#define KEEL_CLIENT_POOL_H

#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/client.h>
#include <keel/error.h>
#include <keel/event_ctx.h>
#include <keel/tls.h>
#include <stdint.h>

/* ── Defaults ────────────────────────────────────────────────────── */

/** @brief Default pool capacity. */
#define KL_CPOOL_DEFAULT_CAPACITY      32
/** @brief Default max idle connections per host. */
#define KL_CPOOL_DEFAULT_MAX_PER_HOST   4
/** @brief Default idle timeout (ms). */
#define KL_CPOOL_DEFAULT_IDLE_MS    60000  /**< 60 seconds */

/* ── Types ───────────────────────────────────────────────────────── */

typedef struct KlClientPool KlClientPool;

typedef struct {
    int         capacity;       /**< Total pool slots (0 = default 32) */
    int         max_per_host;   /**< Max idle per (host,port,tls) (0 = default 4) */
    uint64_t    idle_ms;        /**< Idle timeout ms (0 = default 60s) */
} KlClientPoolConfig;

/**
 * @brief Acquired connection handle.
 *
 * Populated by kl_cpool_acquire on hit. Pass back to kl_cpool_release
 * or kl_cpool_discard when done.
 */
typedef struct {
    KlSocketHandle fd;       /**< TCP socket */
    KlTls *tls;      /**< TLS session (NULL for plaintext) */
    int    reused;   /**< 1 if from pool, 0 if fresh */
    void  *_entry;   /**< Internal bookkeeping -- do not touch */
} KlClientPoolConn;

/**
 * @brief Pool entry (internal, stored in flat array).
 */
typedef struct KlClientPoolEntry {
    char     host[KL_CLIENT_HOSTNAME_MAX]; /**< NUL-terminated key */
    int      port;
    int      is_tls;
    char     proxy_host[KL_CLIENT_HOSTNAME_MAX]; /**< "" = direct connection */
    int      proxy_port;                          /**< 0 = direct connection */
    KlSocketHandle fd;            /**< -1 = free slot */
    KlTls   *tls;
    uint64_t idle_since_ms; /**< kl_monotonic_ms() when returned */
    int64_t  timer_id;      /**< idle timer (-1 = none) */
    struct KlClientPool *pool; /**< back-pointer for timer callback */
} KlClientPoolEntry;

struct KlClientPool {
    KlClientPoolEntry *entries;
    int       capacity;
    int       active;       /**< idle connections in pool */
    int       max_per_host;
    uint64_t  idle_ms;
    KlAllocator *alloc;
    KlEventCtx  *ev_ctx;   /**< NULL for sync-only (manual eviction) */
    KlError   last_error;
};

/* ── Pool lifecycle ──────────────────────────────────────────────── */

/**
 * @brief Initialize a connection pool.
 * @param pool   Pool to initialize (caller-owned storage).
 * @param cfg    Config (NULL for defaults).
 * @param alloc  Allocator (borrowed — must outlive pool).
 * @param ev_ctx Event context for idle timers (NULL = manual eviction).
 * @return 0 on success, -1 on error.
 */
int  kl_cpool_init(KlClientPool *pool, const KlClientPoolConfig *cfg,
                   KlAllocator *alloc, KlEventCtx *ev_ctx);

/**
 * @brief Free all pool resources (closes all idle connections).
 */
void kl_cpool_free(KlClientPool *pool);

/* ── Acquire / release ───────────────────────────────────────────── */

/**
 * @brief Try to acquire an idle connection from the pool.
 * @param pool       Connection pool.
 * @param host       Target hostname.
 * @param port       Target port.
 * @param is_tls     1 for TLS, 0 for plaintext.
 * @param proxy_host Proxy hostname (NULL = direct connection).
 * @param proxy_port Proxy port (0 = direct connection).
 * @param conn       Output: populated on hit.
 * @return 0 = hit (conn populated), 1 = miss, -1 = error.
 */
int  kl_cpool_acquire(KlClientPool *pool, const char *host, int port,
                      int is_tls, const char *proxy_host, int proxy_port,
                      KlClientPoolConn *conn);

/**
 * @brief Return a connection to the pool for reuse.
 * @return 0 on success, -1 on error (connection discarded).
 */
int  kl_cpool_release(KlClientPool *pool, KlClientPoolConn *conn,
                      const char *host, int port, int is_tls,
                      const char *proxy_host, int proxy_port);

/**
 * @brief Close and discard a connection (not returned to pool).
 */
void kl_cpool_discard(KlClientPool *pool, KlClientPoolConn *conn);

/* ── Maintenance ─────────────────────────────────────────────────── */

/**
 * @brief Evict expired idle connections (for sync-only pools without timers).
 * @return Number of connections evicted.
 */
int  kl_cpool_evict_expired(KlClientPool *pool);

/**
 * @brief Count of idle connections in the pool.
 */
int  kl_cpool_idle_count(const KlClientPool *pool);

/**
 * @brief Count idle connections for a specific host tuple.
 */
int  kl_cpool_host_count(const KlClientPool *pool, const char *host,
                         int port, int is_tls,
                         const char *proxy_host, int proxy_port);

/* ── Pooled client request variants ──────────────────────────────── */

/**
 * @brief Synchronous HTTP request with connection pooling.
 *
 * Same as kl_client_request, but reuses connections from the pool.
 * The pool must have been initialized with kl_cpool_init.
 */
int kl_client_request_pooled(KlClientPool *pool,
                              KlAllocator *alloc, const KlClientConfig *cfg,
                              const char *method, const char *url,
                              const KlClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              KlClientResponse *resp);

/**
 * @brief Asynchronous HTTP request with connection pooling.
 *
 * Same as kl_client_start, but reuses connections from the pool.
 * On completion, the connection is returned to the pool unless the
 * server sent Connection: close.
 */
KlClient *kl_client_start_pooled(KlClientPool *pool,
                                   KlEventCtx *ev_ctx, KlAllocator *alloc,
                                   const KlClientConfig *cfg,
                                   const char *method, const char *url,
                                   const KlClientHeader *headers, int num_headers,
                                   const char *body, size_t body_len,
                                   KlClientDoneFn on_done, void *user_data);

#endif /* KEEL_CLIENT_POOL_H */
