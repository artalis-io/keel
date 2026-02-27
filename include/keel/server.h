#ifndef KEEL_SERVER_H
#define KEEL_SERVER_H

#include <keel/allocator.h>
#include <keel/parser.h>
#include <keel/router.h>
#include <keel/tls.h>
#include <keel/h2.h>
#include <keel/connection.h>
#include <keel/event.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct KlWsConfig KlWsConfig;
typedef KlParser *(*KlParserFactory)(KlAllocator *alloc);

/* Access log callback — called after each response is fully sent.
 * NULL = disabled (default, zero overhead). */
typedef void (*KlAccessLogFn)(const KlRequest *req, int status,
                               size_t body_bytes, double duration_ms,
                               void *user_data);

/* Log levels (values match rxi/log.c for zero-cost bridging) */
#define KL_LOG_TRACE 0
#define KL_LOG_DEBUG 1
#define KL_LOG_INFO  2
#define KL_LOG_WARN  3
#define KL_LOG_ERROR 4
#define KL_LOG_FATAL 5

/* Diagnostic log callback. NULL = fprintf(stderr) fallback. */
typedef void (*KlLogFn)(int level, const char *fmt, va_list ap,
                         void *user_data);

#define KL_DEFAULT_MAX_CONNS    256
#define KL_DEFAULT_READ_TIMEOUT 30000   /* ms */

typedef struct KlConfig {
    int port;
    const char *bind_addr;      /* default: "0.0.0.0" */
    int max_connections;        /* default: KL_DEFAULT_MAX_CONNS */
    int read_timeout_ms;        /* default: KL_DEFAULT_READ_TIMEOUT */
    int body_timeout_ms;        /* total body deadline; 0 = use read_timeout_ms */
    KlAllocator *alloc;         /* default: stdlib */
    KlParserFactory parser;     /* default: kl_parser_llhttp */
    KlAccessLogFn access_log;   /* default: NULL (disabled) */
    void *access_log_data;      /* passed as user_data to access_log */
    KlLogFn log_fn;             /* default: NULL (fprintf stderr) */
    void   *log_user_data;
    int install_signal_handlers; /* install SIGTERM/SIGINT handlers */
    int drain_timeout_ms;        /* graceful shutdown drain timeout (0 = immediate) */
    KlTlsConfig *tls;           /* TLS config — NULL = plaintext (default) */
    KlH2Config *h2;             /* HTTP/2 config — NULL = disabled (default) */
} KlConfig;

typedef struct KlServer {
    KlConfig config;
    KlAllocator alloc_storage;  /* owned copy if user didn't provide one */
    KlTlsConfig tls_storage;   /* owned copy of TLS config (if provided) */
    KlH2Config h2_storage;     /* owned copy of H2 config (if provided) */
    KlRouter router;
    KlConnPool pool;
    KlEventLoop loop;
    int listen_fd;
    int listen_paused;          /* 1 = listen fd removed from event loop (pool full) */
    _Atomic int running;
    _Atomic int draining;
    uint64_t drain_deadline_ms;
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
 * @brief Register middleware on the server.
 * @param method  HTTP method filter ("GET", "POST", "*" for any).
 * @param pattern URL pattern — exact or prefix with trailing slash-star.
 * @param fn      Middleware function. Return 0 to continue, non-zero to short-circuit.
 * @param user_data Passed to fn on each invocation.
 * @return 0 on success, -1 on failure.
 */
int  kl_server_use(KlServer *s, const char *method, const char *pattern,
                   KlMiddleware fn, void *user_data);

/**
 * @brief Register a WebSocket endpoint. Matches GET with Upgrade: websocket.
 * @param s       Server instance.
 * @param pattern URL pattern to match.
 * @param config  WebSocket configuration (callbacks, limits). Must remain valid.
 * @return 0 on success, -1 on failure.
 */
int  kl_server_ws(KlServer *s, const char *pattern, KlWsConfig *config);

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

#endif
