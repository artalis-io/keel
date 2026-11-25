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
#include <keel/file_io.h>          /* KlFileIO (was transitive via http_connection.h before it slimmed) */
#include <keel/async.h>            /* KlAsyncOp (was transitive via http_connection.h before it slimmed) */
#include <keel/listener_detail.h>  /* struct KlListener layout: KlHttpServer embeds it */
#include <keel/event_ctx.h>
#include <keel/proxy_protocol.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>   /* size_t */
#ifdef __cplusplus
extern "C" {
#endif


typedef struct KlWsServerConfig KlWsServerConfig;
/** @brief Factory function for creating request parsers. */
typedef KlHttp1Parser *(*KlHttp1ParserFactory)(KlAllocator *alloc);

/** @brief Access log callback: called after each response is fully sent. NULL = disabled. */
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
/* Post-rejection drain bounds (#278): caps on how much inbound request body the server will read
 * and discard, after a final rejection response, to keep the close from RST'ing that response away. */
#define KL_HTTP_SERVER_DEFAULT_REJECT_DRAIN_BYTES (64u * 1024u)
#define KL_HTTP_SERVER_DEFAULT_REJECT_DRAIN_MS    500u

typedef enum {
    KL_HTTP_SERVER_TRANSPORT_TCP = 0,   /**< TCP/IP stream socket (default) */
    KL_HTTP_SERVER_TRANSPORT_UNIX = 1   /**< UNIX domain stream socket */
} KlHttpServerTransport;

/**
 * @brief What a client's hang-up does to a connection suspended with kl_async_suspend().
 *
 * A FIN cannot be told apart from a half-close (a client that called shutdown(SHUT_WR) after its
 * request and still waits for the response): both are one FIN on the wire. The policy decides.
 *
 * Detection depends on the event backend. On a loop with KL_EVENT_CAP_HANGUP (epoll, kqueue,
 * Linux poll, io_uring, Linux pollcomp) a FIN or reset is seen promptly, even behind unread bytes,
 * without reading them. Elsewhere (the Windows readiness backend, IOCP, poll and pollcomp off
 * Linux, Cosmopolitan, lwIP, UEFI, an injected backend without the capability) the server peeks each suspended
 * connection once a second, which is BEST EFFORT: a FIN or reset behind unread bytes (a pipelined
 * request, an unread body) is invisible to the peek. Keel does not require a finite
 * KlAsyncOp.deadline_ms, so on those backends only an op deadline that ends the op guarantees that
 * such a connection is eventually released.
 *
 * Whatever the policy, a suspended streamed response with output waiting is watched for writability
 * on a readiness loop (the hang-up interest rides along but is not reported on its own), so while its
 * output is moving a half-close is not seen. A reset still ends the op, under every policy: through
 * the failed send, or (macOS poll) through the watch's report of it. The hang-up watch resumes once
 * the output has gone.
 */
typedef enum {
    KL_HTTP_SUSPEND_HANGUP_CANCEL     = 0, /**< default: a FIN or a reset cancels the suspended op
                                            *   (kl_async_cancel: on_cancel once, connection closed,
                                            *   nothing written) */
    KL_HTTP_SUSPEND_HANGUP_RESET_ONLY = 1, /**< half-close kept: only a reset cancels; after a FIN the
                                            *   op runs on and its response is still delivered. A
                                            *   reset after that FIN is not watched for, except
                                            *   while stream output waits (see above). */
    KL_HTTP_SUSPEND_HANGUP_OFF        = 2  /**< no hang-up watch (the behavior before 3.5.0); a
                                            *   streamed response with output waiting is still
                                            *   watched for writability, and a reset then ends the
                                            *   op (see above) */
} KlHttpSuspendHangup;

/*
 * Append-only config (see docs/contracts/compatibility.md): callers
 * zero-initialize and recompile per major version; every member is optional and
 * its zero/NULL value selects the built-in default. New members are appended
 * after event_provider.
 */
typedef struct KlHttpServerConfig {
    int port;
    const char *bind_addr;      /**< default: "0.0.0.0" */
    int max_connections;        /**< default: KL_HTTP_SERVER_DEFAULT_MAX_CONNS */
    int read_timeout_ms;        /**< default: KL_HTTP_SERVER_DEFAULT_READ_TIMEOUT */
    int body_timeout_ms;        /**< total body deadline (time suspended in a KlAsyncOp is not
                                 *   counted); 0 = use read_timeout_ms */
    KlAllocator *alloc;         /**< default: stdlib */
    KlHttp1ParserFactory parser;     /**< default: kl_http1_parser_llhttp */
    KlHttpAccessLogFn access_log;   /**< default: NULL (disabled) */
    void *access_log_data;      /**< passed as user_data to access_log */
    KlHttpServerLogFn log_fn;             /**< default: NULL (fprintf stderr) */
    void   *log_user_data;
    int install_signal_handlers; /**< install SIGTERM/SIGINT handlers (single instance only;
                                  * only the last server to call kl_http_server_run() receives signals) */
    int drain_timeout_ms;        /**< graceful shutdown drain timeout (0 = immediate) */
    KlTlsConfig *tls;           /**< TLS config: NULL = plaintext (default) */
    KlHttp2ServerConfig *h2;             /**< HTTP/2 config: NULL = disabled (default) */
    size_t max_body_size;       /**< discard-path body limit; default: 1 MB */
    /* Post-rejection drain (see docs: early-response teardown). When the server answers a request
     * EARLY, while the client is still uploading, the request body is left unread. Closing a socket
     * with unread received data makes TCP send RST, and the peer then discards the response it had
     * already buffered, so the client sees a reset instead of the 413/431/415/500/408 explaining
     * why. After the response is fully flushed the connection therefore half-closes its send side
     * and drains inbound bytes, bounded BOTH ways so a slow or hostile uploader cannot pin the
     * single-threaded loop. Draining is asynchronous: one bounded read per loop progression, never
     * a blocking read-until-empty. Successful keep-alive responses never enter it (their body is
     * already consumed).
     *
     * Each field is INDEPENDENT and a zero value selects that field's default, per this struct's rule
     * that every member is optional: 0/0 is 64 KiB + 500 ms, 128 KiB/0 is 128 KiB + the default
     * timeout, 0/1000 is the default cap + 1000 ms. To turn the drain OFF use reject_drain_disable
     * below; zeroing these fields cannot do it.
     *
     * The effort is BOUNDED, and the bound wins: if the peer keeps transmitting past
     * reject_drain_max_bytes, Keel may terminate the connection even though doing so prevents
     * reliable delivery of the final response. Delivery and bounded resource consumption genuinely
     * conflict under a flooding peer, and a server must choose the bound. Full contract, including
     * why the byte-cap regression test deliberately does not assert delivery:
     * docs/contracts/early_rejection_drain.md */
    size_t   reject_drain_max_bytes;   /**< cap on post-rejection drained bytes; default 64 KiB */
    uint32_t reject_drain_timeout_ms;  /**< cap on post-rejection drain time; default 500 ms */
    size_t max_header_size;     /**< max header block size; 0 = KL_HTTP_CONN_READ_BUF_SIZE (8192).
                                 *   An incomplete block that reaches it gets a 431, a value below
                                 *   8192 included */
    KlCompressConfig *compress; /**< compression config: NULL = disabled (default) */
    KlHttpServerTransport transport;      /**< default: KL_HTTP_SERVER_TRANSPORT_TCP. Setting unix_socket_path
                                 *   forces UNIX regardless of this field (TCP == 0 is
                                 *   indistinguishable from unset, so a non-NULL path wins). */
    const char *unix_socket_path; /**< AF_UNIX path when transport is KL_HTTP_SERVER_TRANSPORT_UNIX.
                                 *   PRECONDITION for automatic cleanup/ownership below: the COMPLETE
                                 *   ancestor chain of the path (every component, not just the immediate
                                 *   parent) must be within the server's trust domain (owned by the
                                 *   server uid or root, and not writable by untrusted actors unless
                                 *   sticky; on Windows: no untrusted SID may delete/replace/
                                 *   re-permission any component and no component may be a reparse
                                 *   point). An untrusted component fails bind CLOSED rather than acting
                                 *   on an ambiguous entry. Environment-default cwd/temp locations are
                                 *   NOT guaranteed to satisfy this and may be rejected (a world-writable
                                 *   /tmp-style tree, or a directory owned by a foreign uid, is correctly
                                 *   refused); place the socket in an application-owned runtime directory
                                 *   with a restrictive owner/mode or ACL (e.g. a service-owned /run
                                 *   subdirectory, or a per-user runtime directory). See
                                 *   docs/archive/designs/unix_socket_cleanup_security_design.md. */
    int unix_socket_unlink;     /**< unlink a stale, owned socket node before bind and on free after a
                                 *   successful bind. Only removes a validated socket the server owns,
                                 *   under the parent-directory precondition above; else fails closed. */
    unsigned int unix_socket_mode; /**< chmod socket node after bind; 0 = leave umask/default. Applied
                                 *   under the trusted-parent precondition above. */
    const char *unix_socket_owner; /**< chown socket node to this username; NULL = leave. Changing
                                 *   owner needs privilege (CAP_CHOWN/root). Transferring ownership to a
                                 *   DIFFERENT uid additionally requires a parent directory with no
                                 *   group/other write (a sticky shared dir such as /tmp is not accepted
                                 *   for that configuration). */
    const char *unix_socket_group; /**< chown socket node to this group name; NULL = leave.
                                    *   Applied before listen(), so no exposure window. */
    const char *proxy_trusted_cidrs; /**< accept PROXY protocol (v1/v2) headers only from
                                      *   sources in this comma-separated CIDR allowlist
                                      *   ("10.0.0.0/8,::1/128"); NULL = disabled. On a
                                      *   dual-stack (::) listener an IPv4 peer arrives
                                      *   v4-mapped (::ffff:a.b.c.d) and is matched against
                                      *   the IPv4 entries (see kl_cidr_match). */
    KlSocketHandle listen_fd;              /**< adopt this pre-bound+listening fd (socket activation);
                                 *   0 = disabled (KEEL creates its own socket). Transport is
                                 *   auto-detected from the fd; KEEL never unlinks an adopted
                                 *   UNIX socket. See kl_systemd_listen_fd(). */
    const KlSocketProvider *sockets; /**< custom socket provider (bring-your-own stack);
                                      *   NULL = built-in POSIX/Winsock default. Must advertise
                                      *   KL_SOCK_CAP_NATIVE_FD: the readiness event loop needs a
                                      *   pollable OS descriptor (rejected at init otherwise). */
    const KlEventProvider *event_provider; /**< custom event backend (bring-your-own readiness
                                      *   loop, e.g. lwIP); NULL = compiled-in default. Pair it
                                      *   with a `sockets` provider whose handles it can poll. */
    int reject_drain_disable;   /**< 1 = do not drain after an early final response; 0 (default) = drain.
                                 *
                                 *   This is the ONLY way to turn the drain off. The two numeric fields
                                 *   above follow this struct's universal rule, that a zero member
                                 *   selects the built-in default, so neither of them can express
                                 *   "disabled": 0 means 64 KiB / 500 ms, not off.
                                 *
                                 *   When this is 1 the numeric fields are IGNORED, and teardown after
                                 *   an early final response behaves as it did before the drain existed:
                                 *   the connection closes immediately, which can reset away a response
                                 *   the peer had not yet read. Choose it only if you would rather reset
                                 *   than spend bounded work on a rejected request.
                                 *
                                 *   An int rather than a bool because these headers are C11 with no
                                 *   <stdbool.h> dependency, matching has_pid and unix_socket_unlink.
                                 *   Full semantics: docs/contracts/early_rejection_drain.md */
    /* Appended in 3.5.0. */
    KlHttpSuspendHangup suspended_hangup; /**< a suspended connection's client hang-up; 0 = CANCEL
                                           *   (see KlHttpSuspendHangup). Any other value is rejected
                                           *   by kl_http_server_init. */
    /* Appended after 3.5.0. */
    size_t stream_capacity;   /**< each streamed response's bound on pending output (0 = 1 MiB; see
                               *   kl_http_response_stream_try_writev). Per response, not a
                               *   server-wide budget. */
    int header_timeout_ms;    /**< request-head deadline: how long a request may take to deliver its
                               *   complete header block, however steadily its bytes trickle in
                               *   (each fragment refreshes only the idle read_timeout_ms). The
                               *   first request's clock starts at the accept, so it also covers a
                               *   TLS handshake, a PROXY header and an HTTP/2 preface; a keep-alive
                               *   request's starts when its first bytes arrive (ciphertext
                               *   included; one lone CRLF before it is ignored, RFC 9112 2.2). It
                               *   stops once the head is parsed, so request bodies (body_timeout_ms)
                               *   and responses are not affected. 0 = use read_timeout_ms; < 0 =
                               *   no header deadline (the behaviour before this field existed).
                               *   Checked by the timeout sweep, which runs on every loop iteration
                               *   (an idle loop wakes at least once a second): 408 (a bare close
                               *   during a TLS handshake or PROXY header), then close. */
} KlHttpServerConfig;

/**
 * @brief Peer credentials of a UNIX-domain-socket client.
 *
 * Populated by kl_http_request_peer_cred() for a connection accepted on a
 * UNIX-domain socket (any other socket yields -1, never credentials). On Linux
 * all three fields are available (SO_PEERCRED). On macOS uid/gid come from
 * getpeereid and the pid from LOCAL_PEERPID (has_pid = 1 when it is available).
 * On the BSDs only uid/gid are available and has_pid is 0.
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
    int  accept_setup_done;       /**< completion path: prime_accepts run once (window latched) */
    int  listen_registered;       /**< listen fd currently has READ interest (listener-managed) */
    int  accept_alive;            /**< liveness token for slot leases; 0'd before pool teardown */
    KlSockAddr accept_pending_peer; /**< peer addr stashed for the on_accept hook (single-threaded) */
    KlSocketHandle listen_fd;              /**< Listening socket fd */
    int bound_port;             /**< actual port after bind (useful with port=0) */
    int unix_socket_owned;      /**< this server bound unix_socket_path and may unlink it */
    /* Opaque, fixed lifecycle storage for the substrate AF_UNIX node-cleanup state
     * (src/unix_socket_node.h). Managed only by that module; the concrete state type stays in
     * substrate. Zeroed and set to the not-open sentinel by kl_http_server_init. Unused on non-UNIX
     * transports and on platforms without the POSIX node lifecycle. The 192 bytes match
     * KL_UNIX_NODE_STORAGE; a static_assert in http_server_plat_posix.c verifies this storage is
     * large enough, and one in unix_socket_node_posix.c bounds the state.
     *
     * The alignment members give the storage suitable alignment for any member the module
     * places in it. They spell out the widest scalars rather than using max_align_t, which
     * MSVC does not define in C mode: a public header must compile for every consumer, not
     * just the ones that build the library. This is ABI-neutral, because the max_align_t
     * that gcc and clang ship is itself defined in terms of long long and long double. The
     * union carries a tag solely so a _Static_assert in http_server_core.c can name its
     * type and hold it to max_align_t's alignment wherever max_align_t exists; the tag adds
     * no member and changes no layout.
     * Adding this is a pre-3.0 source/ABI revision; see docs/archive/designs/unix_socket_cleanup_security_design.md. */
    union KlHttpServerUnixNode {
        unsigned char opaque[192];
        long double _align_ld;
        void       *_align_p;
        long long   _align_ll;
    } unix_node;
    KlCidr *proxy_cidrs;        /**< parsed proxy_trusted_cidrs (NULL = off) */
    int proxy_cidr_count;       /**< number of trusted CIDRs */
    int listen_paused;          /**< 1 = listen fd removed from event loop (pool full) */
    int running;                /**< Server is running (accessed atomically via src/kl_atomic.h) */
    int draining;               /**< Graceful shutdown in progress (accessed atomically via src/kl_atomic.h) */
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
    /* Appended after the 3.2.0 layout (fields only grow at the end within a major version). */
    uint64_t accept_backoff_until; /**< accepting paused until then (monotonic ms) after descriptor or
                                        memory exhaustion; 0 = not paused */
    /* Appended in 3.5.0: the suspended-connection hang-up sweep, for loops without
     * KL_EVENT_CAP_HANGUP (async.c). INTERNAL. */
    int64_t hup_sweep_timer;       /**< the pending sweep timer (valid while hup_sweep_armed) */
    int     hup_sweep_armed;       /**< 1 = a sweep timer is pending */
    int     hup_sweep_conns;       /**< suspended connections the sweep covers */
    /* Appended after 3.5.0. INTERNAL. */
    KlAsyncOp *deadline_sweep_next; /**< the deadline sweep's next op (async.c); NULL between sweeps */
    /* Appended after 3.5.0: graceful drain. INTERNAL. */
    int     drain_listener_closed; /**< 1 = this drain has stopped listening (http_server_core.c) */
    /* Appended after 3.5.0: run-once enforcement. INTERNAL. */
    int     run_started;           /**< 1 = kl_http_server_run has been entered since init */
    int     stop_requested;        /**< 1 = kl_http_server_stop was called (atomic; honoured by a later run) */
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
 *        handler MUST yield on NEED_DATA by calling
 *        kl_http_request_await_body(req) instead of producing a response:
 *        the body reader's on_data callback resumes it for the leftover
 *        and subsequent reads. When resumed from on_data (not via
 *        kl_async_complete), it signals a finished response with
 *        kl_http_request_send_response(req), the send-side partner to
 *        await_body.
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
 * @param pattern URL pattern: exact or prefix with trailing slash-star.
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
 * @param pattern URL pattern: exact or prefix with trailing slash-star.
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
 *
 * Runs once per kl_http_server_init: any later call, whether the first run
 * served, failed to start, or was stopped, returns -1 at once with
 * last_error KL_ERR_INVALID_ARG and touches no descriptor. (A drain closes
 * the listener, an adopted `listen_fd` included, and that number may since
 * name an unrelated socket.) To serve again, kl_http_server_free and
 * kl_http_server_init a fresh instance.
 *
 * @return 0 on clean shutdown, -1 on fatal error or a repeated run.
 */
int  kl_http_server_run(KlHttpServer *s);

/**
 * @brief Request server shutdown. If drain_timeout_ms is configured,
 *        enters drain mode first; a second call stops at once.
 *
 * A graceful drain, from the run loop's next tick:
 * - **stops listening**: the listen socket is closed (an owned AF_UNIX node is
 *   removed), so a new connection is refused at connect instead of waiting in
 *   the backlog, and a load balancer or fleet fails over at once. An adopted
 *   `listen_fd` (socket activation) is closed too, but the activating manager
 *   keeps its own copy, so there new connections queue for the next instance.
 *   The autonomous completion providers (lwIP raw, UEFI) keep their listener
 *   until kl_http_server_free and close each new connection instead;
 * - **ends keep-alive**: every HTTP/1.1 response begun from then on says
 *   `Connection: close` (unless the handler set a Connection header itself),
 *   one begun before still closes its connection afterwards, and a keep-alive
 *   connection waiting for its next request, nothing of it received, is closed.
 *   A connection that has not sent its first request yet may still send it;
 * - lets in-flight requests, suspended ops and streamed responses finish;
 *   WebSocket connections get a close frame and HTTP/2 connections a GOAWAY.
 *
 * kl_http_server_run returns once nothing is active or `drain_timeout_ms` has
 * passed; kl_http_server_free then cancels what remains (on_cancel once each).
 * A server is run once per kl_http_server_init: after a drain its listener is
 * gone. A stop requested before kl_http_server_run starts (a signal during
 * startup) is honoured: that run returns 0 at once, without listening.
 * Async-signal-safe and thread-safe: it only sets flags and wakes the loop.
 */
void kl_http_server_stop(KlHttpServer *s);

/**
 * @brief Whether a graceful drain is in progress (kl_http_server_stop with a
 *        drain_timeout_ms). Safe from any thread.
 * @return 1 while draining (from the stop that begins the drain until the run
 *         loop stops), 0 otherwise, including once the drain has finished and
 *         after kl_http_server_run has returned.
 */
int  kl_http_server_draining(const KlHttpServer *s);

/** @brief Free all server resources (pool, router, event loop). */
void kl_http_server_free(KlHttpServer *s);

/** @brief Server load statistics: read-only snapshot for load-shedding decisions. */
typedef struct {
    int active_connections;    /**< Currently active connection slots */
    int max_connections;       /**< Configured pool capacity */
    int async_suspended;       /**< Connections in async suspend */
    int listen_paused;         /**< 1 if listener paused (pool full) */
} KlHttpServerStats;

/**
 * @brief Populate a stats snapshot from current server state.
 * @param s    Server instance (may be NULL, zeroes out).
 * @param out  Output struct (may be NULL, no-op).
 */
void kl_http_server_stats(const KlHttpServer *s, KlHttpServerStats *out);

/**
 * @brief Borrowed accessor for the server's embedded event context.
 *
 * Returns the `KlEventCtx` the server drives, for registering watchers, timers, and a thread pool
 * (for example `kl_watcher_add()`, `kl_thread_pool_create()`). The field `s->ev` remains a supported
 * facet, but new code should PREFER this accessor so it does not depend on the field's name or
 * placement.
 *
 * Ownership: borrowed; the context is owned by @p s, so the caller must not free it or use it after
 * `kl_http_server_free(s)`. Lifetime: valid while @p s is initialized. Mutability: the context is
 * live; drive it through the watcher/timer/thread-pool APIs, not by re-initializing it. NULL:
 * returns NULL when @p s is NULL (deterministic).
 */
KlEventCtx       *kl_http_server_event_ctx(KlHttpServer *s);
const KlEventCtx *kl_http_server_event_ctx_const(const KlHttpServer *s);

/**
 * @brief Bound TCP port the server is listening on.
 *
 * Resolved even when the config requested an ephemeral port (port 0): after the listen socket is
 * bound this returns the actual port. Prefer this over reading `s->bound_port` directly.
 *
 * Ownership: none (returns a value). Lifetime: meaningful once the listener is bound (0 before
 * binding). NULL: returns -1 when @p s is NULL (deterministic).
 */
int kl_http_server_bound_port(const KlHttpServer *s);

/**
 * @brief Optional platform capabilities.
 *
 * A few server helpers below are only meaningful on certain platforms and
 * return -1 (or fail) elsewhere: the peer-credential and systemd socket-
 * activation calls. Rather than relying on a -1 return to discover support,
 * query kl_platform_caps() and gate the calls on the relevant bit; this makes
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
 * target: e.g. PEER_CRED|PEER_CRED_PID|SYSTEMD_ACTIVATION on Linux, PEER_CRED|
 * PEER_CRED_PID on macOS, 0 on Windows.
 */
unsigned kl_platform_caps(void);

/**
 * @brief Read the peer credentials of a request's connection.
 *
 * Only meaningful for connections accepted on a UNIX-domain socket, and only
 * where KL_PLATCAP_PEER_CRED is set (see kl_platform_caps()). Only a
 * connection served through the built-in platform socket provider is queried:
 * with any other KlHttpServerConfig.sockets provider (lwIP, UEFI, an injected
 * or decorating transport) the handle is not known to be a host descriptor,
 * so this fails closed with -1, even if the provider advertises
 * KL_SOCK_CAP_NATIVE_FD. `out` is left untouched on every -1.
 *
 * @param req  The request (its connection fd is queried).
 * @param out  Receives the peer credentials on success.
 * @return 0 on success, -1 if unavailable (not a UNIX socket, unsupported
 *         platform, a non-built-in socket provider, or the socket has no
 *         peer credentials).
 */
int kl_http_request_peer_cred(const KlHttpRequest *req, KlPeerCred *out);

/**
 * @brief Read peer credentials directly from a connected socket fd.
 *
 * The building block behind kl_http_request_peer_cred / kl_ws_server_peer_cred.
 * Only an AF_UNIX socket has peer credentials: for any other socket (TCP, UDP)
 * this returns -1 and leaves `out` untouched. On macOS the pid is filled via
 * LOCAL_PEERPID (has_pid = 1); Linux uses SO_PEERCRED.
 *
 * Native descriptors only: `fd` must be a host OS socket descriptor. It is
 * queried as one, and this call cannot tell a handle from another socket
 * provider (lwIP, UEFI, an injected transport) from the host descriptor with
 * the same number, so never pass such a handle; the request/WebSocket
 * wrappers above check the provider and refuse it.
 *
 * @param fd   Connected host socket descriptor.
 * @param out  Receives the peer credentials on success.
 * @return 0 on success, -1 if unavailable (not an AF_UNIX socket, unsupported
 *         platform, or no credentials).
 */
int kl_peer_cred_fd(KlSocketHandle fd, KlPeerCred *out);

/**
 * @brief Read the peer's security label (SELinux/AppArmor) of a request's
 *        connection. Linux-only (SO_PEERSEC).
 *
 * @param req     The request.
 * @param buf     Buffer to receive the NUL-terminated label.
 * @param buflen  Size of buf.
 * @return 0 on success, -1 if unavailable (non-Linux, no label, not a
 *         UNIX socket, or a non-built-in socket provider, as for
 *         kl_http_request_peer_cred).
 */
int kl_http_request_peer_label(const KlHttpRequest *req, char *buf, size_t buflen);

/**
 * @brief Format the client's IP address and port for a request.
 *
 * @param req    The request.
 * @param ip     Buffer for the NUL-terminated address ("203.0.113.7" / "2001:db8::1").
 * @param iplen  Size of @p ip (>= INET6_ADDRSTRLEN recommended).
 * @param port   Receives the client port (host byte order); may be NULL.
 * @return 0 on success, -1 if unavailable (e.g. AF_UNIX, use peer credentials).
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
 * for optional client auth; always check `verified` before trusting the
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

#ifdef __cplusplus
}
#endif

#endif
