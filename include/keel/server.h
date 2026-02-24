#ifndef KEEL_SERVER_H
#define KEEL_SERVER_H

#include <keel/allocator.h>
#include <keel/parser.h>
#include <keel/router.h>
#include <keel/tls.h>
#include <keel/connection.h>
#include <keel/event.h>
#include <stdatomic.h>
#include <stdint.h>

typedef KlParser *(*KlParserFactory)(KlAllocator *alloc);

/* Access log callback — called after each response is fully sent.
 * NULL = disabled (default, zero overhead). */
typedef void (*KlAccessLogFn)(const KlRequest *req, int status,
                               size_t body_bytes, double duration_ms,
                               void *user_data);

#define KL_DEFAULT_MAX_CONNS    256
#define KL_DEFAULT_READ_TIMEOUT 30000   /* ms */

typedef struct {
    int port;
    const char *bind_addr;      /* default: "0.0.0.0" */
    int max_connections;        /* default: KL_DEFAULT_MAX_CONNS */
    int read_timeout_ms;        /* default: KL_DEFAULT_READ_TIMEOUT */
    int body_timeout_ms;        /* total body deadline; 0 = use read_timeout_ms */
    KlAllocator *alloc;         /* default: stdlib */
    KlParserFactory parser;     /* default: kl_parser_llhttp */
    KlAccessLogFn access_log;   /* default: NULL (disabled) */
    void *access_log_data;      /* passed as user_data to access_log */
    int install_signal_handlers; /* install SIGTERM/SIGINT handlers */
    int drain_timeout_ms;        /* graceful shutdown drain timeout (0 = immediate) */
    KlTlsConfig *tls;           /* TLS config — NULL = plaintext (default) */
} KlConfig;

typedef struct {
    KlConfig config;
    KlAllocator alloc_storage;  /* owned copy if user didn't provide one */
    KlTlsConfig tls_storage;   /* owned copy of TLS config (if provided) */
    KlRouter router;
    KlConnPool pool;
    KlEventLoop loop;
    int listen_fd;
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
