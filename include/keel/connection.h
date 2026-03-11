#ifndef KEEL_CONNECTION_H
#define KEEL_CONNECTION_H

#include <keel/allocator.h>
#include <keel/chunked.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/parser.h>
#include <keel/router.h>
#include <stddef.h>
#include <stdint.h>

#define KL_READ_BUF_SIZE 8192

typedef struct KlTls KlTls;
typedef struct KlWsServerConn KlWsServerConn;
typedef struct KlH2ServerConn KlH2ServerConn;
typedef struct KlH2ServerConfig KlH2ServerConfig;

typedef enum {
    KL_CONN_TLS_HANDSHAKE,   /* TLS handshake in progress */
    KL_CONN_READING,
    KL_CONN_READING_BODY,
    KL_CONN_PROCESSING,
    KL_CONN_SENDING,
    KL_CONN_WEBSOCKET,      /* WebSocket connection (upgraded) */
    KL_CONN_HTTP2,          /* HTTP/2 connection (upgraded) */
    KL_CONN_SUSPENDED,      /* Suspended for async operation */
    KL_CONN_CLOSED
} KlConnState;

typedef struct KlConn {
    int fd;
    KlConnState state;
    KlAllocator *alloc;     /* set once on pool init, never NULL */

    char read_buf[KL_READ_BUF_SIZE];
    size_t read_len;

    KlRequest req;
    KlResponse res;
    KlParser *parser;

    size_t hdr_sent;

    /* Routing (set after HEADERS_OK, used in PROCESSING) */
    KlRoute *route;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int route_result;

    uint64_t last_active_ms;   /* monotonic clock, updated on every I/O */
    uint64_t request_start_ms; /* stamped at processing start for access log */
    uint64_t body_start_ms;    /* stamped when entering READING_BODY */
    KlChunkedDecoder chunked_dec;  /* reused per-request, no allocation */

    /* TLS session (NULL for plaintext connections) */
    KlTls *tls;
    int tls_want;               /* KL_EVENT_READ or KL_EVENT_WRITE during handshake */

    /* WebSocket state (NULL until upgrade, heap-allocated) */
    KlWsServerConn *ws;

    /* HTTP/2 state (NULL until upgrade, heap-allocated) */
    KlH2ServerConn *h2;
    KlH2ServerConfig *h2_config;   /* set once at pool init, NULL if disabled */
    KlRouter *router;        /* back-pointer to server router */
    size_t max_body_size;    /* discard-path body limit (from KlConfig) */

    /* Async state (non-NULL when SUSPENDED) */
    struct KlAsyncOp *async_op;
    uint64_t suspend_start_ms;

    /* Access logging (set once at pool init, never changes) */
    void (*access_log)(const KlRequest *req, int status,
                       size_t body_bytes, double duration_ms,
                       void *user_data);
    void *access_log_data;

    /* Pool linkage */
    struct KlConn *next_free;
} KlConn;

typedef struct {
    KlConn *conns;
    int capacity;
    int active_count;       /* number of in-use slots */
    KlConn *free_list;
    KlAllocator *alloc;
} KlConnPool;

/**
 * @brief Initialize a pre-allocated connection pool.
 * @param pool     Pool to initialize.
 * @param capacity Maximum concurrent connections.
 * @param alloc    Allocator for pool memory.
 * @return 0 on success, -1 on failure.
 */
int     kl_conn_pool_init(KlConnPool *pool, int capacity, KlAllocator *alloc);

/** @brief Acquire a connection slot from the pool. Returns NULL if full. */
KlConn *kl_conn_acquire(KlConnPool *pool, int fd);

/** @brief Release a connection back to the pool (closes fd). */
void    kl_conn_release(KlConnPool *pool, KlConn *c);

/** @brief Free all pool resources. */
void    kl_conn_pool_free(KlConnPool *pool);

/**
 * @brief Process TLS handshake on a connection.
 * @return New connection state.
 */
KlConnState kl_conn_on_handshake(KlConn *c);

/**
 * @brief Process readable data on a connection (parse headers/body, invoke handler).
 * @return New connection state.
 */
KlConnState kl_conn_on_readable(KlConn *c, KlRouter *router);

/**
 * @brief Process writable event (send response data).
 * @return New connection state.
 */
KlConnState kl_conn_on_writable(KlConn *c);

/** @brief Monotonic clock in milliseconds (for timeout tracking). */
uint64_t kl_monotonic_ms(void);

#endif
