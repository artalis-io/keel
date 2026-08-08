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
#include <keel/drain.h>      /* KlDrain wq (Phase-B write queue, embedded in KlStream) */

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

/**
 * @brief Raw-transport subset of a connection (Phase A structural extraction).
 *
 * INTERNAL / UNSTABLE: these fields were formerly top-level `KlConn` members and
 * are NOT a stable public API — they may move or change without notice. External
 * code must use accessors (e.g. kl_conn_peer_addr()), not `conn->stream.*`.
 *
 * This carries RAW transport state only. TLS-wrapper orchestration (`tls`,
 * `tls_want`) and HTTP policy deliberately stay on `KlConn`; a generic TLS
 * wrapper (`KlTlsStream`) arrives in a later phase.
 */
typedef struct KlStream {
    KlSocketHandle fd;          /**< Socket handle */
    KlAllocator *alloc;         /**< Allocator (set once on pool init) */
    struct KlEventCtx *ctx;     /**< Back-pointer to event ctx (set once at pool init;
                                     hot path reads ctx->sockets for the provider) */

    KlSockAddr peer_addr;       /**< Client address (captured at accept);
                                     family KL_AF_UNSPEC = unavailable */

    char *read_buf;             /**< Read buffer */
    size_t read_len;            /**< Bytes in read buffer */
    size_t read_cap;            /**< Read buffer capacity */
    int read_paused;            /**< Read-side flow control: 1 = body reads paused
                                     (kl_request_pause_body). Readiness drops READ interest;
                                     completion stops posting the next recv. */

    /* ── Phase-B write machinery (internal; API in src/stream_write.h) ────────────────
     * The bounded, preallocated write queue + completion send-in-flight tracking that the
     * generic KlStream write path uses. Dormant (zero) until kl_stream_write_init(); the raw
     * transport writer/submit hooks are supplied by the (interim) HTTP/TLS adapter — no TLS
     * state lives here. Not yet routed by the HTTP response path; INTERNAL/UNSTABLE. */
    KlDrain            wq;          /**< Bounded write queue (reserved capacity at init) */
    int                wq_inited;   /**< 1 once kl_stream_write_init preallocated wq */
    int                wq_err;      /**< Sticky write-side error (submission/completion failure) */
    /* Completion-mode send tracking: exactly one async send may be in flight at a time; a
     * successful submission does NOT permit another until its WRITE completion (ordering).
     * The submit hook is an inline function pointer (no named public typedef while the
     * machinery is internal — the internal name is in src/stream_write.h). */
    int              (*submit_fn)(void *ctx, const char *data, size_t len); /**< NULL = readiness */
    void              *submit_ctx;  /**< submit hook context */
    int                submit_copying; /**< configured ownership policy: 1 = backend copies on submit */
    int                send_inflight;  /**< a submit is outstanding — do not post another */
    size_t             inflight_len;   /**< bytes handed to the in-flight submit */
    int                inflight_copying; /**< ownership policy CAPTURED for the in-flight op */

    /* ── Phase-B strict read pause/resume machinery (internal; API in src/stream_read.h) ──
     * Mediates the raw receive path: a receive lands in the stable read_buf; while paused the
     * completed receive is HELD (undelivered) until resume, which delivers it exactly once
     * before re-arming. Dormant until kl_stream_read_init(); read hooks (deliver/arm/disarm)
     * are adapter-supplied (inline pointers, internal names in src/stream_read.h). No HTTP
     * routing yet; INTERNAL/UNSTABLE. read_paused (above) is the pause flag. */
    void             (*read_deliver)(void *ctx, const char *buf, size_t len, int ok); /**< deliver hook */
    int              (*read_arm)(void *ctx);   /**< arm/post the next receive: 0 ok, -1 fail */
    void             (*read_disarm)(void *ctx);/**< readiness: drop READ interest; completion: no-op */
    void              *read_ctx;               /**< shared context for the read hooks */
    int                read_completion_mode;   /**< 1 = completion (a posted recv completes → held) */
    int                read_inited;            /**< 1 once kl_stream_read_init installed the hooks */
    int                recv_inflight;          /**< a receive is armed/posted */
    int                recv_held;              /**< a completed receive is held (undelivered) */
    size_t             held_len;               /**< held byte count (<= read_cap) */
    int                held_ok;                /**< held terminal flag: 1 = data, 0 = EOF/error */
    int                read_closed;            /**< read side closed (terminal delivered / cancelled) */
    /* Arm trampoline (iterative, bounds stack under synchronous completion): `arming` marks that
     * we are inside read_arm(); a re-arm requested during that window sets `rearm_pending` for the
     * trampoline loop instead of recursing; `completed_inline` records that on_recv retired the
     * current arm synchronously, so read_arm's later return can't retroactively fail it. */
    int                arming;
    int                rearm_pending;
    int                completed_inline;
} KlStream;

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
    KlStream stream;            /**< Raw-transport subset (fd, alloc, ctx, peer_addr,
                                     read_buf/len/cap, read_paused). Phase A extraction —
                                     INTERNAL/UNSTABLE, not a stable field-level API. */

    KlConnState state;          /**< Connection state */

    uint8_t peer_source;                /**< KL_PEER_SOCKET | KL_PEER_PROXY */

    size_t max_header_size;     /**< Max header size (from KlConfig) */

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
    char *comp_cipher;          /**< Completion-mode TLS ciphertext scratch: driver-owned,
                                     stable until the async recv completes. Preallocated at server
                                     init for TLS + completion slots (never in the event-loop hot
                                     path); NULL otherwise. Interim Phase-A TLS orchestration
                                     (→ KlTlsStream in Phase C). Freed in pool free.
                                     INTERNAL/UNSTABLE. */
    size_t comp_cipher_cap;     /**< Allocated size of comp_cipher (0 if unallocated) — so
                                     alloc/free need no completion-internal size macro. */

    KlWsServerConn *ws;         /**< WebSocket state (NULL until upgrade) */

    KlH2ServerConn *h2;         /**< HTTP/2 state (NULL until upgrade) */
    KlH2ServerConfig *h2_config; /**< HTTP/2 config (set once at pool init) */
    KlRouter *router;           /**< Back-pointer to server router */
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
 * overwrites conn->stream.peer_addr with the real client address.
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
KlConnState kl_conn_on_file_complete(KlConn *c, kl_ssize_t result, int zero_copy);

/** @brief Monotonic clock in milliseconds (for timeout tracking). */
uint64_t kl_monotonic_ms(void);

/**
 * @brief Peer (client) address for a connection — stable accessor.
 *
 * Returns a pointer to the connection's peer address (family KL_AF_UNSPEC when
 * unavailable). Prefer this over reaching into `conn->stream.*`, which is
 * internal/unstable.
 */
const KlSockAddr *kl_conn_peer_addr(const KlConn *c);

#endif
