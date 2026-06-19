#ifndef KEEL_SERVER_H
#define KEEL_SERVER_H

#include <keel/allocator.h>
#include <keel/compress.h>
#include <keel/error.h>
#include <keel/parser.h>
#include <keel/router.h>
#include <keel/tls.h>
#include <keel/h2_server.h>
#include <keel/connection.h>
#include <keel/event_ctx.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct KlWsServerConfig KlWsServerConfig;
/** @brief Factory function for creating request parsers. */
typedef KlParser *(*KlParserFactory)(KlAllocator *alloc);

/** @brief Access log callback — called after each response is fully sent. NULL = disabled. */
typedef void (*KlAccessLogFn)(const KlRequest *req, int status,
                               size_t body_bytes, double duration_ms,
                               void *user_data);

/** @brief Log levels (values match rxi/log.c for zero-cost bridging). @{ */
#define KL_LOG_TRACE 0  /**< Trace */
#define KL_LOG_DEBUG 1  /**< Debug */
#define KL_LOG_INFO  2  /**< Info */
#define KL_LOG_WARN  3  /**< Warning */
#define KL_LOG_ERROR 4  /**< Error */
#define KL_LOG_FATAL 5  /**< Fatal */
/** @} */

/** @brief Diagnostic log callback. NULL = fprintf(stderr) fallback. */
typedef void (*KlLogFn)(int level, const char *fmt, va_list ap,
                         void *user_data);

/** @brief Default max connections. */
#define KL_DEFAULT_MAX_CONNS      256
/** @brief Default read timeout (ms). */
#define KL_DEFAULT_READ_TIMEOUT   30000           /**< ms */
/** @brief Default max body size. */
#define KL_DEFAULT_MAX_BODY_SIZE  (1024 * 1024)   /**< 1 MB */

typedef struct KlConfig {
    int port;
    const char *bind_addr;      /**< default: "0.0.0.0" */
    int max_connections;        /**< default: KL_DEFAULT_MAX_CONNS */
    int read_timeout_ms;        /**< default: KL_DEFAULT_READ_TIMEOUT */
    int body_timeout_ms;        /**< total body deadline; 0 = use read_timeout_ms */
    KlAllocator *alloc;         /**< default: stdlib */
    KlParserFactory parser;     /**< default: kl_parser_llhttp */
    KlAccessLogFn access_log;   /**< default: NULL (disabled) */
    void *access_log_data;      /**< passed as user_data to access_log */
    KlLogFn log_fn;             /**< default: NULL (fprintf stderr) */
    void   *log_user_data;
    int install_signal_handlers; /**< install SIGTERM/SIGINT handlers (single instance only —
                                  * only the last server to call kl_server_run() receives signals) */
    int drain_timeout_ms;        /**< graceful shutdown drain timeout (0 = immediate) */
    KlTlsConfig *tls;           /**< TLS config — NULL = plaintext (default) */
    KlH2ServerConfig *h2;             /**< HTTP/2 config — NULL = disabled (default) */
    size_t max_body_size;       /**< discard-path body limit; default: 1 MB */
    size_t max_header_size;     /**< max header block size; 0 = KL_READ_BUF_SIZE (8192) */
    KlCompressConfig *compress; /**< compression config — NULL = disabled (default) */
} KlConfig;

typedef struct KlAsyncOp KlAsyncOp;

typedef struct KlServer {
    KlConfig config;
    KlAllocator alloc_storage;  /**< owned copy if user didn't provide one */
    KlTlsConfig tls_storage;   /**< owned copy of TLS config (if provided) */
    KlH2ServerConfig h2_storage;     /**< owned copy of H2 config (if provided) */
    KlCompressConfig compress_storage; /**< owned copy of compress config (if provided) */
    KlRouter router;            /**< Route table */
    KlConnPool pool;            /**< Connection pool */
    KlEventCtx ev;              /**< event loop + watcher list */
    int listen_fd;              /**< Listening socket fd */
    int bound_port;             /**< actual port after bind (useful with port=0) */
    int listen_paused;          /**< 1 = listen fd removed from event loop (pool full) */
    _Atomic int running;        /**< Server is running */
    _Atomic int draining;       /**< Graceful shutdown in progress */
    uint64_t drain_deadline_ms; /**< Drain timeout deadline */
    KlAsyncOp *async_ops;       /**< active async ops list */
    KlFileIO *file_io;          /**< async file I/O (auto-created if backend supports it) */
    KlError last_error;         /**< diagnostic: set at point of return -1 */
} KlServer;

/**
 * @brief Initialize server with the given configuration.
 * @param s      Server instance.
 * @param config Configuration (defaults applied for zero fields).
 * @return 0 on success, -1 on failure.
 */
int  kl_server_init(KlServer *s, const KlConfig *config);

/**
 * @brief Register a route on the server.
 * @return 0 on success, -1 on failure.
 */
int  kl_server_route(KlServer *s, const char *method, const char *pattern,
                     KlHandler handler, void *user_data,
                     KlBodyReaderFactory body_reader);

/**
 * @brief Register a streaming-handler route. The handler runs after
 *        the body reader is set up but BEFORE the body is fully
 *        received, so it can pull bytes incrementally (e.g. via the
 *        streaming multipart iterator) and yield mid-stream. The body
 *        reader's on_data callback is responsible for resuming the
 *        yielded handler.
 *
 *        Post-body middleware does NOT run for streaming routes.
 *        A non-NULL body_reader factory is required.
 *
 * @return 0 on success, -1 on failure.
 */
int  kl_server_route_streaming(KlServer *s, const char *method, const char *pattern,
                                KlHandler handler, void *user_data,
                                KlBodyReaderFactory body_reader);

/**
 * @brief Register an async-streaming-handler route (v2.2.0+).
 *
 *        Like kl_server_route_streaming, plus the handler is invoked
 *        BEFORE any leftover body bytes are fed via on_data. The
 *        handler MUST yield on NEED_DATA — the body reader's on_data
 *        callback resumes it for the leftover and subsequent reads.
 *
 *        Enables the full error-path mid-stream early-exit: caps
 *        that fire during leftover processing now resume the parked
 *        handler via on_error so it can write a structured response,
 *        instead of clobbering with the hardcoded 413.
 *
 *        Synchronous C handlers that need the body fully buffered
 *        before they run should keep using kl_server_route_streaming.
 *
 *        See router.h::kl_router_add_streaming_async for the full
 *        contract.
 *
 * @return 0 on success, -1 on failure.
 */
int  kl_server_route_streaming_async(KlServer *s, const char *method,
                                       const char *pattern,
                                       KlHandler handler, void *user_data,
                                       KlBodyReaderFactory body_reader);

/**
 * @brief Register pre-body middleware on the server.
 * @param s       Server instance.
 * @param method  HTTP method filter ("GET", "POST", "*" for any).
 * @param pattern URL pattern — exact or prefix with trailing slash-star.
 * @param fn      Middleware function. Return 0 to continue, non-zero to short-circuit.
 * @param user_data Passed to fn on each invocation.
 * @return 0 on success, -1 on failure.
 */
int  kl_server_use(KlServer *s, const char *method, const char *pattern,
                   KlMiddleware fn, void *user_data);

/**
 * @brief Register post-body middleware on the server.
 *
 * Runs after body reading completes. Can access req->body_reader data.
 * Short-circuiting preserves keep_alive (body already consumed).
 *
 * @param s       Server instance.
 * @param method  HTTP method filter ("GET", "POST", "*" for any).
 * @param pattern URL pattern — exact or prefix with trailing slash-star.
 * @param fn      Middleware function. Return 0 to continue, non-zero to short-circuit.
 * @param user_data Passed to fn on each invocation.
 * @return 0 on success, -1 on failure.
 */
int  kl_server_use_post(KlServer *s, const char *method, const char *pattern,
                        KlMiddleware fn, void *user_data);

/**
 * @brief Register a WebSocket endpoint. Matches GET with Upgrade: websocket.
 * @param s       Server instance.
 * @param pattern URL pattern to match.
 * @param config  WebSocket configuration (callbacks, limits). Must remain valid.
 * @return 0 on success, -1 on failure.
 */
int  kl_server_ws(KlServer *s, const char *pattern, KlWsServerConfig *config);

/**
 * @brief Start the event loop (blocks until stopped).
 * @return 0 on clean shutdown, -1 on fatal error.
 */
int  kl_server_run(KlServer *s);

/**
 * @brief Request server shutdown. If drain_timeout_ms is configured,
 *        enters drain mode first (stops accepting, waits for in-flight).
 */
void kl_server_stop(KlServer *s);

/** @brief Free all server resources (pool, router, event loop). */
void kl_server_free(KlServer *s);

/**
 * @brief Freeze the server's routing state (v2.3.0+).
 *
 * Forwards to @ref kl_router_freeze on the server's embedded router,
 * sealing the route table, pre-middleware array, and post-middleware
 * array into an mprotect-RO arena.  Intended to be called once,
 * AFTER all route/middleware registration is complete and BEFORE
 * @ref kl_server_run is entered.  Post-freeze:
 *
 *   - kl_server_route / kl_server_use / kl_server_use_post all
 *     return -1 (registration is closed).
 *   - A heap-write primitive into the routing tables faults
 *     (SIGSEGV/SIGBUS) instead of pivoting control flow to an
 *     attacker-controlled function pointer or relaxing a middleware
 *     gate.
 *
 * Calling this is optional — Keel will continue to serve correctly
 * without it.  Callers that want the hardening guarantee SHOULD
 * adopt the pattern:
 *
 *   kl_server_init(&s, &cfg);
 *   kl_server_route(&s, "GET", "/", home, NULL, NULL);
 *   kl_server_use  (&s, "*", "/api/...", auth_mw, &auth_ctx);
 *   ...  all registration here
 *   kl_server_freeze(&s);    once, after all adds
 *   kl_server_run(&s);
 *
 * @return 0 on success, -1 on failure (already frozen, arena init
 *         or seal failed, OOM).
 */
int  kl_server_freeze(KlServer *s);

/** @brief Returns 1 if @p s has been frozen via @ref kl_server_freeze. */
int  kl_server_is_frozen(const KlServer *s);

/** @brief Server load statistics — read-only snapshot for load-shedding decisions. */
typedef struct {
    int active_connections;    /**< Currently active connection slots */
    int max_connections;       /**< Configured pool capacity */
    int async_suspended;       /**< Connections in async suspend */
    int listen_paused;         /**< 1 if listener paused (pool full) */
} KlServerStats;

/**
 * @brief Populate a stats snapshot from current server state.
 * @param s    Server instance (may be NULL — zeroes out).
 * @param out  Output struct (may be NULL — no-op).
 */
void kl_server_stats(const KlServer *s, KlServerStats *out);

#endif
