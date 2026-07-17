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
#include <keel/proxy_protocol.h>
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

typedef enum {
    KL_TRANSPORT_TCP = 0,   /**< TCP/IP stream socket (default) */
    KL_TRANSPORT_UNIX = 1   /**< UNIX domain stream socket */
} KlTransport;

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
    KlTransport transport;      /**< default: KL_TRANSPORT_TCP. Setting unix_socket_path
                                 *   forces UNIX regardless of this field (TCP == 0 is
                                 *   indistinguishable from unset, so a non-NULL path wins). */
    const char *unix_socket_path; /**< AF_UNIX path when transport is KL_TRANSPORT_UNIX */
    int unix_socket_unlink;     /**< unlink path before bind and on free after successful bind */
    unsigned int unix_socket_mode; /**< chmod socket path after bind; 0 = leave umask/default */
    const char *unix_socket_owner; /**< chown socket to this username; NULL = leave.
                                    *   Changing owner needs privilege (CAP_CHOWN/root). */
    const char *unix_socket_group; /**< chown socket to this group name; NULL = leave.
                                    *   Applied before listen(), so no exposure window. */
    const char *proxy_trusted_cidrs; /**< accept PROXY protocol (v1/v2) headers only from
                                      *   sources in this comma-separated CIDR allowlist
                                      *   ("10.0.0.0/8,::1/128"); NULL = disabled. */
    int listen_fd;              /**< adopt this pre-bound+listening fd (socket activation);
                                 *   0 = disabled (KEEL creates its own socket). Transport is
                                 *   auto-detected from the fd; KEEL never unlinks an adopted
                                 *   UNIX socket. See kl_systemd_listen_fd(). */
} KlConfig;

/**
 * @brief Peer credentials of a UNIX-domain-socket client.
 *
 * Populated by kl_request_peer_cred(). On Linux all three fields are
 * available (SO_PEERCRED). On macOS/BSD only uid/gid are available
 * (getpeereid) and has_pid is 0.
 */
typedef struct {
    long uid;       /**< Peer effective user id */
    long gid;       /**< Peer effective group id */
    long pid;       /**< Peer process id (valid only if has_pid) */
    int  has_pid;   /**< 1 if pid is valid (Linux), 0 otherwise */
} KlPeerCred;

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
    int unix_socket_owned;      /**< this server bound unix_socket_path and may unlink it */
    KlCidr *proxy_cidrs;        /**< parsed proxy_trusted_cidrs (NULL = off) */
    int proxy_cidr_count;       /**< number of trusted CIDRs */
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

/**
 * @brief Read the peer credentials of a request's connection.
 *
 * Only meaningful for connections accepted on a UNIX-domain socket.
 *
 * @param req  The request (its connection fd is queried).
 * @param out  Receives the peer credentials on success.
 * @return 0 on success, -1 if unavailable (not a UNIX socket, unsupported
 *         platform, or the socket has no peer credentials).
 */
int kl_request_peer_cred(const KlRequest *req, KlPeerCred *out);

/**
 * @brief Read peer credentials directly from a connected socket fd.
 *
 * The building block behind kl_request_peer_cred / kl_ws_server_peer_cred.
 * Meaningful for AF_UNIX sockets. On macOS the pid is filled via LOCAL_PEERPID
 * (has_pid = 1); Linux uses SO_PEERCRED.
 *
 * @param fd   Connected socket fd.
 * @param out  Receives the peer credentials on success.
 * @return 0 on success, -1 if unavailable.
 */
int kl_peer_cred_fd(int fd, KlPeerCred *out);

/**
 * @brief Read the peer's security label (SELinux/AppArmor) of a request's
 *        connection. Linux-only (SO_PEERSEC).
 *
 * @param req     The request.
 * @param buf     Buffer to receive the NUL-terminated label.
 * @param buflen  Size of buf.
 * @return 0 on success, -1 if unavailable (non-Linux, no label, or not a
 *         UNIX socket).
 */
int kl_request_peer_label(const KlRequest *req, char *buf, size_t buflen);

/**
 * @brief Format the client's IP address and port for a request.
 *
 * @param req    The request.
 * @param ip     Buffer for the NUL-terminated address ("203.0.113.7" / "2001:db8::1").
 * @param iplen  Size of @p ip (>= INET6_ADDRSTRLEN recommended).
 * @param port   Receives the client port (host byte order); may be NULL.
 * @return 0 on success, -1 if unavailable (e.g. AF_UNIX — use peer credentials).
 */
int kl_request_peer_addr(const KlRequest *req, char *ip, size_t iplen,
                         uint16_t *port);

/**
 * @brief Raw client socket address for a request (for advanced use).
 *
 * @param req  The request.
 * @param len  Receives the address length; may be NULL.
 * @return Pointer to the address (owned by the connection), or NULL if
 *         unavailable. Valid until the request completes.
 */
const struct sockaddr *kl_request_peer_sockaddr(const KlRequest *req,
                                                socklen_t *len);

/**
 * @brief Return an inherited listening socket fd from socket activation.
 *
 * Implements the systemd LISTEN_FDS protocol: if LISTEN_PID matches this
 * process and LISTEN_FDS >= 1, returns the first passed fd (SD_LISTEN_FDS_START
 * = 3). The relevant environment variables are unset so they are not
 * inherited by children. Pass the result to KlConfig.listen_fd.
 *
 * @return The inherited fd (>= 3), or -1 if socket activation is not in use.
 */
int kl_systemd_listen_fd(void);

/**
 * @brief Like kl_systemd_listen_fd(), but also reports how many fds were passed.
 *
 * When socket activation is in use, the passed fds are consecutive starting at
 * SD_LISTEN_FDS_START (3): fd 3, 4, ..., 3 + *count - 1. Returns the first fd
 * (3), or -1 if socket activation is not in use (in which case *count is 0).
 *
 * @param count  Receives the number of passed fds (may be NULL).
 * @return The first inherited fd (3), or -1 if not socket-activated.
 */
int kl_systemd_listen_fds(int *count);

/**
 * @brief Return an inherited fd by its systemd socket name (FileDescriptorName).
 *
 * Matches @p name against the colon-separated LISTEN_FDNAMES list and returns
 * the corresponding fd (SD_LISTEN_FDS_START + index). Like the other
 * kl_systemd_listen_fd* helpers, this consumes (unsets) the LISTEN_* env vars,
 * so call exactly one of them once.
 *
 * @param name  The socket name from the unit's ListenStream=/FileDescriptorName=.
 * @return The matching fd (>= 3), or -1 if not socket-activated / no match.
 */
int kl_systemd_listen_fd_by_name(const char *name);

#endif
