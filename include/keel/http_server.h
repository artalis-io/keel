#ifndef KEEL_HTTP_SERVER_H
#define KEEL_HTTP_SERVER_H

#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/socket.h>
#include <keel/compress.h>
#include <keel/error.h>
#include <keel/http1_parser.h>
#include <keel/http_router.h>
#include <keel/tls.h>
#include <keel/http2_server.h>
#include <keel/http_connection.h>
#include <keel/listener_detail.h>  /* struct KlListener layout — KlHttpServer embeds it (step 6B-1) */
#include <keel/event_ctx.h>
#include <keel/proxy_protocol.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct KlWsServerConfig KlWsServerConfig;
/** @brief Factory function for creating request parsers. */
typedef KlHttp1Parser *(*KlHttp1ParserFactory)(KlAllocator *alloc);

/** @brief Access log callback — called after each response is fully sent. NULL = disabled. */
typedef void (*KlHttpAccessLogFn)(const KlHttpRequest *req, int status,
                               size_t body_bytes, double duration_ms,
                               void *user_data);

/** @brief Log levels (values match rxi/log.c for zero-cost bridging). @{ */
#define KL_HTTP_SERVER_LOG_TRACE 0  /**< Trace */
#define KL_HTTP_SERVER_LOG_DEBUG 1  /**< Debug */
#define KL_HTTP_SERVER_LOG_INFO  2  /**< Info */
#define KL_HTTP_SERVER_LOG_WARN  3  /**< Warning */
#define KL_HTTP_SERVER_LOG_ERROR 4  /**< Error */
#define KL_HTTP_SERVER_LOG_FATAL 5  /**< Fatal */
/** @} */

/** @brief Diagnostic log callback. NULL = fprintf(stderr) fallback. */
typedef void (*KlHttpServerLogFn)(int level, const char *fmt, va_list ap,
                         void *user_data);

/** @brief Default max connections. */
#define KL_HTTP_SERVER_DEFAULT_MAX_CONNS      256
/** @brief Default read timeout (ms). */
#define KL_HTTP_SERVER_DEFAULT_READ_TIMEOUT   30000           /**< ms */
/** @brief Default max body size. */
#define KL_HTTP_SERVER_DEFAULT_MAX_BODY_SIZE  (1024 * 1024)   /**< 1 MB */

typedef enum {
    KL_HTTP_SERVER_TRANSPORT_TCP = 0,   /**< TCP/IP stream socket (default) */
    KL_HTTP_SERVER_TRANSPORT_UNIX = 1   /**< UNIX domain stream socket */
} KlHttpServerTransport;

typedef struct KlHttpServerConfig {
    int port;
    const char *bind_addr;      /**< default: "0.0.0.0" */
    int max_connections;        /**< default: KL_HTTP_SERVER_DEFAULT_MAX_CONNS */
    int read_timeout_ms;        /**< default: KL_HTTP_SERVER_DEFAULT_READ_TIMEOUT */
    int body_timeout_ms;        /**< total body deadline; 0 = use read_timeout_ms */
    KlAllocator *alloc;         /**< default: stdlib */
    KlHttp1ParserFactory parser;     /**< default: kl_http1_parser_llhttp */
    KlHttpAccessLogFn access_log;   /**< default: NULL (disabled) */
    void *access_log_data;      /**< passed as user_data to access_log */
    KlHttpServerLogFn log_fn;             /**< default: NULL (fprintf stderr) */
    void   *log_user_data;
    int install_signal_handlers; /**< install SIGTERM/SIGINT handlers (single instance only —
                                  * only the last server to call kl_http_server_run() receives signals) */
    int drain_timeout_ms;        /**< graceful shutdown drain timeout (0 = immediate) */
    KlTlsConfig *tls;           /**< TLS config — NULL = plaintext (default) */
    KlHttp2ServerConfig *h2;             /**< HTTP/2 config — NULL = disabled (default) */
    size_t max_body_size;       /**< discard-path body limit; default: 1 MB */
    size_t max_header_size;     /**< max header block size; 0 = KL_HTTP_CONN_READ_BUF_SIZE (8192) */
    KlCompressConfig *compress; /**< compression config — NULL = disabled (default) */
    KlHttpServerTransport transport;      /**< default: KL_HTTP_SERVER_TRANSPORT_TCP. Setting unix_socket_path
                                 *   forces UNIX regardless of this field (TCP == 0 is
                                 *   indistinguishable from unset, so a non-NULL path wins). */
    const char *unix_socket_path; /**< AF_UNIX path when transport is KL_HTTP_SERVER_TRANSPORT_UNIX */
    int unix_socket_unlink;     /**< unlink path before bind and on free after successful bind */
    unsigned int unix_socket_mode; /**< chmod socket path after bind; 0 = leave umask/default */
    const char *unix_socket_owner; /**< chown socket to this username; NULL = leave.
                                    *   Changing owner needs privilege (CAP_CHOWN/root). */
    const char *unix_socket_group; /**< chown socket to this group name; NULL = leave.
                                    *   Applied before listen(), so no exposure window. */
    const char *proxy_trusted_cidrs; /**< accept PROXY protocol (v1/v2) headers only from
                                      *   sources in this comma-separated CIDR allowlist
                                      *   ("10.0.0.0/8,::1/128"); NULL = disabled. */
    KlSocketHandle listen_fd;              /**< adopt this pre-bound+listening fd (socket activation);
                                 *   0 = disabled (KEEL creates its own socket). Transport is
                                 *   auto-detected from the fd; KEEL never unlinks an adopted
                                 *   UNIX socket. See kl_systemd_listen_fd(). */
    const KlSocketProvider *sockets; /**< custom socket provider (bring-your-own stack);
                                      *   NULL = built-in POSIX/Winsock default. Must advertise
                                      *   KL_SOCK_CAP_NATIVE_FD — the readiness event loop needs a
                                      *   pollable OS descriptor (rejected at init otherwise). */
    const KlEventProvider *event_provider; /**< custom event backend (bring-your-own readiness
                                      *   loop, e.g. lwIP); NULL = compiled-in default. Pair it
                                      *   with a `sockets` provider whose handles it can poll. */
} KlHttpServerConfig;

/**
 * @brief Peer credentials of a UNIX-domain-socket client.
 *
 * Populated by kl_http_request_peer_cred(). On Linux all three fields are
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

typedef struct KlHttpServer {
    KlHttpServerConfig config;
    KlAllocator alloc_storage;  /**< owned copy if user didn't provide one */
    KlTlsConfig tls_storage;   /**< owned copy of TLS config (if provided) */
    KlHttp2ServerConfig h2_storage;     /**< owned copy of H2 config (if provided) */
    KlCompressConfig compress_storage; /**< owned copy of compress config (if provided) */
    KlHttpRouter router;            /**< Route table */
    KlHttpConnPool pool;            /**< Connection pool */
    KlEventCtx ev;              /**< event loop + watcher list */
    /* ── Accept via KlListener (step 6B) ────────────────────────────────────────────
     * When the loop is a readiness loop (!completion_loop), the readiness accept path is driven by
     * this embedded KlListener with the split-credit pool accounting. INTERNAL/UNSTABLE. */
    KlListener accept_listener;   /**< accept driver (active when accept_via_listener); readiness or completion */
    int  accept_via_listener;     /**< 1 = the KlListener drives accepts (readiness, or a post-driven completion backend) */
    int  accept_setup_done;       /**< completion path: prime_accepts run once (window latched) — 6B-3 2b-ii */
    int  listen_registered;       /**< listen fd currently has READ interest (listener-managed) */
    int  accept_alive;            /**< liveness token for slot leases; 0'd before pool teardown */
    KlSockAddr accept_pending_peer; /**< peer addr stashed for the on_accept hook (single-threaded) */
    KlSocketHandle listen_fd;              /**< Listening socket fd */
    int bound_port;             /**< actual port after bind (useful with port=0) */
    int unix_socket_owned;      /**< this server bound unix_socket_path and may unlink it */
    KlCidr *proxy_cidrs;        /**< parsed proxy_trusted_cidrs (NULL = off) */
    int proxy_cidr_count;       /**< number of trusted CIDRs */
    int listen_paused;          /**< 1 = listen fd removed from event loop (pool full) */
    _Atomic int running;        /**< Server is running */
    _Atomic int draining;       /**< Graceful shutdown in progress */
    uint64_t drain_deadline_ms; /**< Drain timeout deadline */
    /* Self-pipe/loopback wakeup so kl_http_server_stop() wakes the run loop immediately
     * instead of waiting up to KL_POLL_TIMEOUT_MS for the current wait/drain to
     * return. The two handles are a KlPlatWakeup (internal type kept out of this
     * public header); http_server.c constructs the wrapper from them. KL_INVALID_SOCKET
     * if the pair couldn't be opened (falls back to tick-timeout latency). */
    KlSocketHandle stop_wake_rd; /**< wakeup read end (registered as a run-loop watcher) */
    KlSocketHandle stop_wake_wr; /**< wakeup write end (kl_http_server_stop signals it) */
    KlAsyncOp *async_ops;       /**< active async ops list */
    KlFileIO *file_io;          /**< async file I/O (auto-created if backend supports it) */
    KlError last_error;         /**< diagnostic: set at point of return -1 */
} KlHttpServer;

/**
 * @brief Initialize server with the given configuration.
 * @param s      Server instance.
 * @param config Configuration (defaults applied for zero fields).
 * @return 0 on success, -1 on failure.
 */
int  kl_http_server_init(KlHttpServer *s, const KlHttpServerConfig *config);

/**
 * @brief Register a route on the server.
 * @return 0 on success, -1 on failure.
 */
int  kl_http_server_route(KlHttpServer *s, const char *method, const char *pattern,
                     KlHttpHandler handler, void *user_data,
                     KlHttpBodyReaderFactory body_reader);

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
int  kl_http_server_route_streaming(KlHttpServer *s, const char *method, const char *pattern,
                                KlHttpHandler handler, void *user_data,
                                KlHttpBodyReaderFactory body_reader);

/**
 * @brief Register an async-streaming-handler route (v2.2.0+).
 *
 *        Like kl_http_server_route_streaming, plus the handler is invoked
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
 *        before they run should keep using kl_http_server_route_streaming.
 *
 *        See http_router.h::kl_http_router_add_streaming_async for the full
 *        contract.
 *
 * @return 0 on success, -1 on failure.
 */
int  kl_http_server_route_streaming_async(KlHttpServer *s, const char *method,
                                       const char *pattern,
                                       KlHttpHandler handler, void *user_data,
                                       KlHttpBodyReaderFactory body_reader);

/**
 * @brief Register pre-body middleware on the server.
 * @param s       Server instance.
 * @param method  HTTP method filter ("GET", "POST", "*" for any).
 * @param pattern URL pattern — exact or prefix with trailing slash-star.
 * @param fn      Middleware function. Return 0 to continue, non-zero to short-circuit.
 * @param user_data Passed to fn on each invocation.
 * @return 0 on success, -1 on failure.
 */
int  kl_http_server_use(KlHttpServer *s, const char *method, const char *pattern,
                   KlHttpMiddleware fn, void *user_data);

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
int  kl_http_server_use_post(KlHttpServer *s, const char *method, const char *pattern,
                        KlHttpMiddleware fn, void *user_data);

/**
 * @brief Register a WebSocket endpoint. Matches GET with Upgrade: websocket.
 * @param s       Server instance.
 * @param pattern URL pattern to match.
 * @param config  WebSocket configuration (callbacks, limits). Must remain valid.
 * @return 0 on success, -1 on failure.
 */
int  kl_http_server_ws_upgrade(KlHttpServer *s, const char *pattern, KlWsServerConfig *config);

/**
 * @brief Start the event loop (blocks until stopped).
 * @return 0 on clean shutdown, -1 on fatal error.
 */
int  kl_http_server_run(KlHttpServer *s);

/**
 * @brief Request server shutdown. If drain_timeout_ms is configured,
 *        enters drain mode first (stops accepting, waits for in-flight).
 */
void kl_http_server_stop(KlHttpServer *s);

/** @brief Free all server resources (pool, router, event loop). */
void kl_http_server_free(KlHttpServer *s);

/** @brief Server load statistics — read-only snapshot for load-shedding decisions. */
typedef struct {
    int active_connections;    /**< Currently active connection slots */
    int max_connections;       /**< Configured pool capacity */
    int async_suspended;       /**< Connections in async suspend */
    int listen_paused;         /**< 1 if listener paused (pool full) */
} KlHttpServerStats;

/**
 * @brief Populate a stats snapshot from current server state.
 * @param s    Server instance (may be NULL — zeroes out).
 * @param out  Output struct (may be NULL — no-op).
 */
void kl_http_server_stats(const KlHttpServer *s, KlHttpServerStats *out);

/**
 * @brief Optional platform capabilities.
 *
 * A few server helpers below are only meaningful on certain platforms and
 * return -1 (or fail) elsewhere: the peer-credential and systemd socket-
 * activation calls. Rather than relying on a -1 return to discover support,
 * query kl_platform_caps() and gate the calls on the relevant bit — this makes
 * the portability contract explicit and keeps platform assumptions out of
 * application logic.
 */
typedef enum {
    KL_PLATCAP_PEER_CRED          = 1u << 0, /**< kl_http_request_peer_cred / kl_peer_cred_fd yield uid+gid (AF_UNIX; POSIX) */
    KL_PLATCAP_PEER_CRED_PID      = 1u << 1, /**< peer credentials also include the peer pid (Linux SO_PEERCRED / macOS LOCAL_PEERPID) */
    KL_PLATCAP_SYSTEMD_ACTIVATION = 1u << 2  /**< kl_systemd_listen_fd* honor the systemd LISTEN_FDS protocol (Linux/systemd) */
} KlPlatformCap;

/**
 * @brief Bitmask of KlPlatformCap values supported by this build/platform.
 *
 * Resolved by the platform slice (server_plat_*), so it reflects the actual
 * target — e.g. PEER_CRED|PEER_CRED_PID|SYSTEMD_ACTIVATION on Linux, PEER_CRED|
 * PEER_CRED_PID on macOS, 0 on Windows.
 */
unsigned kl_platform_caps(void);

/**
 * @brief Read the peer credentials of a request's connection.
 *
 * Only meaningful for connections accepted on a UNIX-domain socket, and only
 * where KL_PLATCAP_PEER_CRED is set (see kl_platform_caps()).
 *
 * @param req  The request (its connection fd is queried).
 * @param out  Receives the peer credentials on success.
 * @return 0 on success, -1 if unavailable (not a UNIX socket, unsupported
 *         platform, or the socket has no peer credentials).
 */
int kl_http_request_peer_cred(const KlHttpRequest *req, KlPeerCred *out);

/**
 * @brief Read peer credentials directly from a connected socket fd.
 *
 * The building block behind kl_http_request_peer_cred / kl_ws_server_peer_cred.
 * Meaningful for AF_UNIX sockets. On macOS the pid is filled via LOCAL_PEERPID
 * (has_pid = 1); Linux uses SO_PEERCRED.
 *
 * @param fd   Connected socket fd.
 * @param out  Receives the peer credentials on success.
 * @return 0 on success, -1 if unavailable.
 */
int kl_peer_cred_fd(KlSocketHandle fd, KlPeerCred *out);

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
int kl_http_request_peer_label(const KlHttpRequest *req, char *buf, size_t buflen);

/**
 * @brief Format the client's IP address and port for a request.
 *
 * @param req    The request.
 * @param ip     Buffer for the NUL-terminated address ("203.0.113.7" / "2001:db8::1").
 * @param iplen  Size of @p ip (>= INET6_ADDRSTRLEN recommended).
 * @param port   Receives the client port (host byte order); may be NULL.
 * @return 0 on success, -1 if unavailable (e.g. AF_UNIX — use peer credentials).
 */
int kl_http_request_peer_addr(const KlHttpRequest *req, char *ip, size_t iplen,
                         uint16_t *port);

/**
 * @brief Client address for a request as a Keel-neutral KlSockAddr (advanced use).
 *
 * @param req  The request.
 * @return Pointer to the address (owned by the connection), or NULL if
 *         unavailable. Valid until the request completes. Format it with
 *         kl_sockaddr_format(), or read family/port via kl_sockaddr_family/port().
 */
const KlSockAddr *kl_http_request_peer_sockaddr(const KlHttpRequest *req);

/**
 * @brief Extract the verified mTLS client certificate for a request.
 *
 * Server-side mutual TLS: when the client presented a certificate during the
 * handshake, fills @p out with its parsed identity (subject/issuer CN, SANs,
 * SHA-256 fingerprint, validity window) and raw DER.
 *
 * `out->verified` reports whether the certificate passed CA-chain validation.
 * A certificate can be *present* but *unverified* when the server is configured
 * for optional client auth — always check `verified` before trusting the
 * identity. `out->der` points into connection-owned memory valid only until the
 * request completes; copy it if you need it longer.
 *
 * @param req  The request.
 * @param out  Caller-provided struct to populate (zeroed on entry).
 * @return 0 if a certificate was present and @p out filled, -1 otherwise
 *         (plaintext connection, no client certificate, or a TLS backend
 *         without client-certificate support).
 */
int kl_http_request_peer_cert(const KlHttpRequest *req, KlPeerCert *out);

/**
 * @brief Return an inherited listening socket fd from socket activation.
 *
 * Implements the systemd LISTEN_FDS protocol: if LISTEN_PID matches this
 * process and LISTEN_FDS >= 1, returns the first passed fd (SD_LISTEN_FDS_START
 * = 3). The relevant environment variables are unset so they are not
 * inherited by children. Pass the result to KlHttpServerConfig.listen_fd.
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
