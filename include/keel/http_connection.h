#ifndef KEEL_HTTP_CONNECTION_H
#define KEEL_HTTP_CONNECTION_H

#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/http1_chunked.h>
#include <keel/file_io.h>
#include <keel/http_request.h>
#include <keel/http_response.h>
#include <keel/http1_parser.h>
#include <keel/http_router.h>
#include <stddef.h>
#include <stdint.h>
#include <keel/sockaddr.h>   /* KlSockAddr peer_addr */
#include <keel/drain.h>      /* KlDrain wq (write queue, embedded in KlStream) */
#include <keel/stream.h>         /* KlStream contract (STABLE transport API) */
#include <keel/stream_detail.h>  /* struct KlStream layout: KlHttpConn embeds it (opt-in detail) */
#include <keel/listener.h>       /* KlSlotLease (admission credit handed off at accept, step 6B) */
#ifdef __cplusplus
extern "C" {
#endif


/** @brief Default read buffer size (bytes). */
#define KL_HTTP_CONN_READ_BUF_SIZE 8192

/** @brief peer_source values. @{ */
#define KL_HTTP_PEER_SOCKET 0   /**< Address came from the accepted socket */
#define KL_HTTP_PEER_PROXY  1   /**< Address came from a trusted PROXY header */
/** @} */

typedef struct KlTls KlTls;
typedef struct KlWsServerConn KlWsServerConn;
typedef struct KlHttp2ServerConn KlHttp2ServerConn;
typedef struct KlHttp2ServerConfig KlHttp2ServerConfig;

/* The raw-transport subset of a connection is the STABLE public KlStream (function + ownership
 * contract in <keel/stream.h>, opt-in/unstable layout in <keel/stream_detail.h>). KlHttpConn embeds it
 * below via the detail header. External code must use accessors (e.g. kl_http_conn_peer_addr()), not
 * `conn->stream.*`; the embedded layout is INTERNAL/UNSTABLE even though the API is stable. */

typedef enum {
    KL_HTTP_CONN_PROXY_HEADER,    /**< Reading a PROXY protocol header (pre-TLS) */
    KL_HTTP_CONN_TLS_HANDSHAKE,   /**< TLS handshake in progress */
    KL_HTTP_CONN_READING,         /**< Reading request headers */
    KL_HTTP_CONN_READING_BODY,    /**< Reading request body */
    KL_HTTP_CONN_PROCESSING,      /**< Processing request */
    KL_HTTP_CONN_SENDING,         /**< Sending response */
    KL_HTTP_CONN_WEBSOCKET,       /**< WebSocket connection (upgraded) */
    KL_HTTP_CONN_HTTP2,           /**< HTTP/2 connection (upgraded) */
    KL_HTTP_CONN_SUSPENDED,       /**< Suspended for async operation */
    KL_HTTP_CONN_CLOSED           /**< Connection closed */
} KlHttpConnState;

typedef struct KlHttpConn {
    KlStream stream;            /**< Raw-transport subset (fd, alloc, ctx, peer_addr,
                                     read_buf/len/cap, read_paused).
                                     INTERNAL/UNSTABLE, not a stable field-level API. */

    KlHttpConnState state;          /**< Connection state */

    uint8_t peer_source;                /**< KL_HTTP_PEER_SOCKET | KL_HTTP_PEER_PROXY */

    size_t max_header_size;     /**< Max header size (from KlHttpServerConfig) */

    KlHttpRequest req;              /**< Current request */
    KlHttpResponse res;             /**< Current response */
    KlHttp1Parser *parser;           /**< HTTP parser */

    size_t hdr_sent;            /**< Header bytes sent */

    KlHttpRoute *route;             /**< Matched route (set after HEADERS_OK) */
    KlHttpParam params[KL_HTTP_ROUTER_MAX_PARAMS]; /**< Extracted route parameters */
    int num_params;             /**< Number of route parameters */
    int route_result;           /**< Router match result (200/404/405) */

    uint64_t last_active_ms;    /**< Monotonic clock, updated on every I/O */
    uint64_t request_start_ms;  /**< Stamped at processing start for access log */
    uint64_t body_start_ms;     /**< Stamped when entering READING_BODY */
    KlHttp1ChunkedDecoder chunked_dec; /**< Chunked decoder (reused per-request) */

    KlTls *tls;                 /**< TLS session (NULL for plaintext) */
    int tls_want;               /**< KL_EVENT_READ or KL_EVENT_WRITE during handshake */
    char *comp_cipher;          /**< Completion-mode TLS ciphertext scratch: driver-owned,
                                     stable until the async recv completes. Preallocated at server
                                     init for TLS + completion slots (never in the event-loop hot
                                     path); NULL otherwise. Freed in pool free.
                                     INTERNAL/UNSTABLE. */
    size_t comp_cipher_cap;     /**< Allocated size of comp_cipher (0 if unallocated), so
                                     alloc/free need no completion-internal size macro. */

    KlWsServerConn *ws;         /**< WebSocket state (NULL until upgrade) */

    KlHttp2ServerConn *h2;         /**< HTTP/2 state (NULL until upgrade) */
    KlHttp2ServerConfig *h2_config; /**< HTTP/2 config (set once at pool init) */
    KlHttpRouter *router;           /**< Back-pointer to server router */
    size_t max_body_size;       /**< Discard-path body limit (from KlHttpServerConfig) */

    struct KlAsyncOp *async_op; /**< Active async op (non-NULL when SUSPENDED) */
    uint64_t suspend_start_ms;  /**< Monotonic time when suspended */

    KlFileIO *file_io;          /**< Async file I/O (set once at pool init) */
    int file_io_phase;          /**< FILE_IO_IDLE/READING/WRITING/CANCELLING */
    size_t file_io_len;         /**< Bytes from last async read */
    size_t file_io_sent;        /**< Bytes written to socket so far */

    void (*access_log)(const KlHttpRequest *req, int status,
                       size_t body_bytes, double duration_ms,
                       void *user_data); /**< Access log callback (set once at pool init) */
    void *access_log_data;      /**< Opaque data for access_log callback */

    KlSlotLease slot_lease;     /**< Admission credit handed off by KlListener at accept (step 6B);
                                     consumed once on close AFTER the KlHttpConn returns to the pool. */

    struct KlHttpConn *next_free;   /**< Free list linkage */
} KlHttpConn;

typedef struct {
    KlHttpConn *conns;          /**< Connection slot array */
    int capacity;           /**< Maximum connection slots */
    int active_count;       /**< Number of in-use slots (physical KlHttpConn ownership) */
    int free_credits;       /**< Admission rights reserved before posting accepts (step 6B). The
                                 invariant free_credits + reserved_accepts + active_count == capacity
                                 holds; reserve() takes a credit, the lease returns it on close. */
    KlHttpConn *free_list;      /**< Free slot linked list */
    KlAllocator *alloc;     /**< Allocator for pool memory */
} KlHttpConnPool;

/**
 * @brief Initialize a pre-allocated connection pool.
 * @param pool     Pool to initialize.
 * @param capacity Maximum concurrent connections.
 * @param alloc    Allocator for pool memory.
 * @return 0 on success, -1 on failure.
 */
int     kl_http_conn_pool_init(KlHttpConnPool *pool, int capacity, KlAllocator *alloc);

/** @brief Reserve one admission credit before posting/arming an accept (step 6B). Returns 1 if a
 *  credit was taken (free_credits--), 0 if none available (→ listener backpressure), -1 on a NULL
 *  pool (fail closed). Only the listener-backed READINESS admission path uses free_credits; the
 *  completion accept path currently bypasses it (it calls kl_http_conn_acquire directly). */
int     kl_http_conn_pool_reserve(KlHttpConnPool *pool);

/** @brief Return one admission credit to the pool (free_credits++, capped at capacity). Called via
 *  the KlSlotLease on connection close, or when a reserved-but-unused accept is dropped. No-op on a
 *  NULL pool; an over-return (broken lease accounting) trips an assert in debug/test builds. */
void    kl_http_conn_pool_return_credit(KlHttpConnPool *pool);

/** @brief Acquire a connection slot from the pool. Returns NULL if full. */
KlHttpConn *kl_http_conn_acquire(KlHttpConnPool *pool, KlSocketHandle fd);

/** @brief Release a connection back to the pool (closes fd). */
void    kl_http_conn_release(KlHttpConnPool *pool, KlHttpConn *c);

/** @brief Free all pool resources. */
void    kl_http_conn_pool_free(KlHttpConnPool *pool);

/**
 * @brief Process TLS handshake on a connection.
 * @return New connection state.
 */
KlHttpConnState kl_http_conn_on_handshake(KlHttpConn *c);

/**
 * @brief Read/parse a PROXY protocol header (state KL_HTTP_CONN_PROXY_HEADER).
 *
 * Uses MSG_PEEK to inspect the leading bytes without consuming the following
 * TLS/HTTP stream; on a valid header it consumes exactly the header bytes and
 * overwrites conn->stream.peer_addr with the real client address.
 *
 * @return 1 = done (proceed to TLS/read), 0 = need more bytes, -1 = close.
 */
int kl_http_conn_read_proxy_header(KlHttpConn *c);

/**
 * @brief Ingest a PROXY header from bytes already in read_buf (completion path).
 *
 * The completion counterpart of kl_http_conn_read_proxy_header(): the header bytes arrived via an
 * overlapped recv into read_buf[0..len] rather than a socket peek. On a real header, sets
 * peer_addr from it (KL_HTTP_PEER_PROXY).
 *
 * @param c   Connection (parses read_buf[0..len]).
 * @param len Bytes available in read_buf.
 * @return Header bytes to consume (0 = not a PROXY header, proceed), -1 = malformed/close,
 *         -2 = need more bytes.
 */
int kl_http_conn_ingest_proxy(KlHttpConn *c, size_t len);

/**
 * @brief Process readable data on a connection (parse headers/body, invoke handler).
 * @return New connection state.
 */
KlHttpConnState kl_http_conn_on_readable(KlHttpConn *c, KlHttpRouter *router);

/**
 * @brief Process writable event (send response data).
 * @return New connection state.
 */
KlHttpConnState kl_http_conn_on_writable(KlHttpConn *c);

/**
 * @brief Handle async file I/O completion (called from server tick loop).
 * @param c         Connection that submitted the read.
 * @param result    Bytes read/transferred (positive) or error (negative/zero).
 * @param zero_copy 1 if data was spliced directly to socket (no WRITING phase).
 * @return New connection state.
 */
KlHttpConnState kl_http_conn_on_file_complete(KlHttpConn *c, kl_ssize_t result, int zero_copy);


/**
 * @brief Peer (client) address for a connection: stable accessor.
 *
 * Returns a pointer to the connection's peer address (family KL_AF_UNSPEC when
 * unavailable). Prefer this over reaching into `conn->stream.*`, which is
 * internal/unstable.
 */
const KlSockAddr *kl_http_conn_peer_addr(const KlHttpConn *c);

#ifdef __cplusplus
}
#endif

#endif
