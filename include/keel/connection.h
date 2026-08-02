#ifndef KEEL_CONNECTION_H
#define KEEL_CONNECTION_H

#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/chunked.h>
#include <keel/file_io.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/parser.h>
#include <keel/router.h>
#include <stddef.h>
#include <stdint.h>
#include <keel/sockaddr.h>   /* KlSockAddr peer_addr */

/** @brief Default read buffer size (bytes). */
#define KL_READ_BUF_SIZE 8192

/** @brief peer_source values. @{ */
#define KL_PEER_SOCKET 0   /**< Address came from the accepted socket */
#define KL_PEER_PROXY  1   /**< Address came from a trusted PROXY header */
/** @} */

typedef struct KlTls KlTls;
typedef struct KlWsServerConn KlWsServerConn;
typedef struct KlH2ServerConn KlH2ServerConn;
typedef struct KlH2ServerConfig KlH2ServerConfig;
/* Back-pointer to the owning event context (for the internal socket provider,
 * ctx->sockets). Opaque forward decl keeps the provider seam out of this
 * header. */
struct KlEventCtx;

typedef enum {
    KL_CONN_PROXY_HEADER,    /**< Reading a PROXY protocol header (pre-TLS) */
    KL_CONN_TLS_HANDSHAKE,   /**< TLS handshake in progress */
    KL_CONN_READING,         /**< Reading request headers */
    KL_CONN_READING_BODY,    /**< Reading request body */
    KL_CONN_PROCESSING,      /**< Processing request */
    KL_CONN_SENDING,         /**< Sending response */
    KL_CONN_WEBSOCKET,       /**< WebSocket connection (upgraded) */
    KL_CONN_HTTP2,           /**< HTTP/2 connection (upgraded) */
    KL_CONN_SUSPENDED,       /**< Suspended for async operation */
    KL_CONN_CLOSED           /**< Connection closed */
} KlConnState;

typedef struct KlConn {
    KlSocketHandle fd;                     /**< Socket file descriptor */
    KlConnState state;          /**< Connection state */
    KlAllocator *alloc;         /**< Allocator (set once on pool init) */

    KlSockAddr peer_addr;               /**< Client address (captured at accept);
                                         *   family KL_AF_UNSPEC = unavailable */
    uint8_t peer_source;                /**< KL_PEER_SOCKET | KL_PEER_PROXY */

    char *read_buf;             /**< Read buffer */
    size_t read_len;            /**< Bytes in read buffer */
    size_t read_cap;            /**< Read buffer capacity */
    size_t max_header_size;     /**< Max header size (from KlConfig) */
    int read_paused;            /**< Read-side flow control: 1 = body reads paused
                                     (kl_request_pause_body). Readiness drops READ interest;
                                     completion stops posting the next recv. */

    KlRequest req;              /**< Current request */
    KlResponse res;             /**< Current response */
    KlParser *parser;           /**< HTTP parser */

    size_t hdr_sent;            /**< Header bytes sent */

    KlRoute *route;             /**< Matched route (set after HEADERS_OK) */
    KlParam params[KL_MAX_PARAMS]; /**< Extracted route parameters */
    int num_params;             /**< Number of route parameters */
    int route_result;           /**< Router match result (200/404/405) */

    uint64_t last_active_ms;    /**< Monotonic clock, updated on every I/O */
    uint64_t request_start_ms;  /**< Stamped at processing start for access log */
    uint64_t body_start_ms;     /**< Stamped when entering READING_BODY */
    KlChunkedDecoder chunked_dec; /**< Chunked decoder (reused per-request) */

    KlTls *tls;                 /**< TLS session (NULL for plaintext) */
    int tls_want;               /**< KL_EVENT_READ or KL_EVENT_WRITE during handshake */

    KlWsServerConn *ws;         /**< WebSocket state (NULL until upgrade) */

    KlH2ServerConn *h2;         /**< HTTP/2 state (NULL until upgrade) */
    KlH2ServerConfig *h2_config; /**< HTTP/2 config (set once at pool init) */
    KlRouter *router;           /**< Back-pointer to server router */
    struct KlEventCtx *ctx;     /**< Back-pointer to event ctx (set once at pool init;
                                     hot path reads ctx->sockets for the provider) */
    size_t max_body_size;       /**< Discard-path body limit (from KlConfig) */

    struct KlAsyncOp *async_op; /**< Active async op (non-NULL when SUSPENDED) */
    uint64_t suspend_start_ms;  /**< Monotonic time when suspended */

    KlFileIO *file_io;          /**< Async file I/O (set once at pool init) */
    int file_io_phase;          /**< FILE_IO_IDLE/READING/WRITING/CANCELLING */
    size_t file_io_len;         /**< Bytes from last async read */
    size_t file_io_sent;        /**< Bytes written to socket so far */

    void (*access_log)(const KlRequest *req, int status,
                       size_t body_bytes, double duration_ms,
                       void *user_data); /**< Access log callback (set once at pool init) */
    void *access_log_data;      /**< Opaque data for access_log callback */

    struct KlConn *next_free;   /**< Free list linkage */
} KlConn;

typedef struct {
    KlConn *conns;          /**< Connection slot array */
    int capacity;           /**< Maximum connection slots */
    int active_count;       /**< Number of in-use slots */
    KlConn *free_list;      /**< Free slot linked list */
    KlAllocator *alloc;     /**< Allocator for pool memory */
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
KlConn *kl_conn_acquire(KlConnPool *pool, KlSocketHandle fd);

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
 * @brief Read/parse a PROXY protocol header (state KL_CONN_PROXY_HEADER).
 *
 * Uses MSG_PEEK to inspect the leading bytes without consuming the following
 * TLS/HTTP stream; on a valid header it consumes exactly the header bytes and
 * overwrites conn->peer_addr with the real client address.
 *
 * @return 1 = done (proceed to TLS/read), 0 = need more bytes, -1 = close.
 */
int kl_conn_read_proxy_header(KlConn *c);

/**
 * @brief Ingest a PROXY header from bytes already in read_buf (completion path).
 *
 * The completion counterpart of kl_conn_read_proxy_header(): the header bytes arrived via an
 * overlapped recv into read_buf[0..len] rather than a socket peek. On a real header, sets
 * peer_addr from it (KL_PEER_PROXY).
 *
 * @param c   Connection (parses read_buf[0..len]).
 * @param len Bytes available in read_buf.
 * @return Header bytes to consume (0 = not a PROXY header, proceed), -1 = malformed/close,
 *         -2 = need more bytes.
 */
int kl_conn_ingest_proxy(KlConn *c, size_t len);

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

/**
 * @brief Handle async file I/O completion (called from server tick loop).
 * @param c         Connection that submitted the read.
 * @param result    Bytes read/transferred (positive) or error (negative/zero).
 * @param zero_copy 1 if data was spliced directly to socket (no WRITING phase).
 * @return New connection state.
 */
KlConnState kl_conn_on_file_complete(KlConn *c, ssize_t result, int zero_copy);

/** @brief Monotonic clock in milliseconds (for timeout tracking). */
uint64_t kl_monotonic_ms(void);

#endif
