/**
 * @file http_client_pool.h
 * @brief HTTP client connection pool.
 *
 * Caches idle TCP+TLS connections keyed by (host, port, is_tls),
 * enabling HTTP keep-alive reuse across requests. Opt-in: pass a
 * KlHttpClientPool to the pooled request variants.
 *
 * Pool sizes are small (32-128), so entries use a flat array with
 * linear scan: cache-friendly and simpler than a hash map.
 */

#ifndef KEEL_HTTP_CLIENT_POOL_H
#define KEEL_HTTP_CLIENT_POOL_H

#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/http_client.h>
#include <keel/error.h>
#include <keel/event_ctx.h>
#include <keel/tls.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif


/* ── Defaults ────────────────────────────────────────────────────── */

/** @brief Default pool capacity. */
#define KL_HTTP_CLIENT_POOL_DEFAULT_CAPACITY      32
/** @brief Default max idle connections per host. */
#define KL_HTTP_CLIENT_POOL_DEFAULT_MAX_PER_HOST   4
/** @brief Default idle timeout (ms). */
#define KL_HTTP_CLIENT_POOL_DEFAULT_IDLE_MS    60000  /**< 60 seconds */

/* ── Types ───────────────────────────────────────────────────────── */

typedef struct KlHttpClientPool KlHttpClientPool;

/*
 * Append-only config (see docs/contracts/compatibility.md): callers
 * zero-initialize and recompile per major version; every member is optional and
 * its zero/NULL value selects the built-in default. New members are appended
 * after idle_ms.
 */
typedef struct {
    int         capacity;       /**< Total pool slots (0 = default 32) */
    int         max_per_host;   /**< Max idle per (host,port,tls) (0 = default 4) */
    uint64_t    idle_ms;        /**< Idle timeout ms (0 = default 60s) */
} KlHttpClientPoolConfig;

/**
 * @brief Acquired connection handle.
 *
 * Populated by kl_http_client_pool_acquire on hit. Pass back to kl_http_client_pool_release
 * or kl_http_client_pool_discard when done.
 */
typedef struct {
    KlSocketHandle fd;       /**< TCP socket */
    KlTls *tls;      /**< TLS session (NULL for plaintext) */
    int    reused;   /**< 1 if from pool, 0 if fresh */
    void  *_entry;   /**< Internal bookkeeping -- do not touch */
} KlHttpClientPoolConn;

/**
 * @brief Pool entry: an opaque, internal slot.
 *
 * The layout is private to the client-pool implementation
 * (src/protocols/http/http_client_pool_internal.h); KlHttpClientPool holds it by pointer, so only
 * this forward declaration is public.
 */
typedef struct KlHttpClientPoolEntry KlHttpClientPoolEntry;

struct KlHttpClientPool {
    KlHttpClientPoolEntry *entries;  /**< slot array (visible-for-allocation only) */
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
 * @param alloc  Allocator (borrowed, must outlive pool).
 * @param ev_ctx Event context for idle timers (NULL = manual eviction).
 * @return 0 on success, -1 on error.
 */
int  kl_http_client_pool_init(KlHttpClientPool *pool, const KlHttpClientPoolConfig *cfg,
                   KlAllocator *alloc, KlEventCtx *ev_ctx);

/**
 * @brief Free all pool resources (closes all idle connections).
 */
void kl_http_client_pool_free(KlHttpClientPool *pool);

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
int  kl_http_client_pool_acquire(KlHttpClientPool *pool, const char *host, int port,
                      int is_tls, const char *proxy_host, int proxy_port,
                      KlHttpClientPoolConn *conn);

/**
 * @brief Return a connection to the pool for reuse.
 * @return 0 on success, -1 on error (connection discarded).
 */
int  kl_http_client_pool_release(KlHttpClientPool *pool, KlHttpClientPoolConn *conn,
                      const char *host, int port, int is_tls,
                      const char *proxy_host, int proxy_port);

/**
 * @brief Close and discard a connection (not returned to pool).
 */
void kl_http_client_pool_discard(KlHttpClientPool *pool, KlHttpClientPoolConn *conn);

/* ── Maintenance ─────────────────────────────────────────────────── */

/**
 * @brief Evict expired idle connections (for sync-only pools without timers).
 * @return Number of connections evicted.
 */
int  kl_http_client_pool_evict_expired(KlHttpClientPool *pool);

/**
 * @brief Count of idle connections in the pool.
 */
int  kl_http_client_pool_idle_count(const KlHttpClientPool *pool);

/**
 * @brief Count idle connections for a specific host tuple.
 */
int  kl_http_client_pool_host_count(const KlHttpClientPool *pool, const char *host,
                         int port, int is_tls,
                         const char *proxy_host, int proxy_port);

/* ── Pooled client request variants ──────────────────────────────── */

/**
 * @brief Synchronous HTTP request with connection pooling.
 *
 * Same as kl_http_client_request, but reuses connections from the pool.
 * The pool must have been initialized with kl_http_client_pool_init.
 */
int kl_http_client_request_pooled(KlHttpClientPool *pool,
                              KlAllocator *alloc, const KlHttpClientConfig *cfg,
                              const char *method, const char *url,
                              const KlHttpClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              KlHttpClientResponse *resp);

/**
 * @brief Asynchronous HTTP request with connection pooling.
 *
 * Same as kl_http_client_start, but reuses connections from the pool.
 * On completion, the connection is returned to the pool unless the
 * server sent Connection: close.
 */
KlHttpClient *kl_http_client_start_pooled(KlHttpClientPool *pool,
                                   KlEventCtx *ev_ctx, KlAllocator *alloc,
                                   const KlHttpClientConfig *cfg,
                                   const char *method, const char *url,
                                   const KlHttpClientHeader *headers, int num_headers,
                                   const char *body, size_t body_len,
                                   KlHttpClientDoneFn on_done, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_HTTP_CLIENT_POOL_H */
