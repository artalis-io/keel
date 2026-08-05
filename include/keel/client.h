/**
 * @file client.h
 * @brief HTTP/1.1 client (sync + async).
 *
 * Sync API: blocking request/response with poll()-based I/O.
 * Async API: non-blocking state machine driven by KlEventCtx watchers.
 *
 * The async client requires only KlEventCtx (not KlServer), so it
 * can be used standalone or embedded in any event-loop-based program.
 */

#ifndef KEEL_CLIENT_H
#define KEEL_CLIENT_H

#include <keel/allocator.h>
#include <keel/decompress.h>
#include <keel/error.h>
#include <keel/event_ctx.h>
#include <keel/parser.h>
#include <keel/resolver.h>
#include <keel/socket.h>
#include <keel/tls.h>
#include <keel/url.h>

/* ── Constants ────────────────────────────────────────────────────── */

/** @brief Maximum hostname length. */
#define KL_CLIENT_HOSTNAME_MAX       256
/** @brief Request buffer size. */
#define KL_CLIENT_REQ_BUF_SIZE       4096
/** @brief Maximum request headers. */
#define KL_CLIENT_MAX_REQ_HEADERS    64
/** @brief Default timeout (ms). */
#define KL_CLIENT_DEFAULT_TIMEOUT_MS 30000
/** @brief Default max response size. */
#define KL_CLIENT_DEFAULT_MAX_RESP   (4 * 1024 * 1024)
/** @brief Receive buffer size. */
#define KL_CLIENT_RECV_BUF_SIZE      8192
/** @brief Chunked encoding buffer size. */
#define KL_CLIENT_CHUNK_BUF_SIZE     4096
/** @brief Chunked header size. */
#define KL_CLIENT_CHUNK_HDR_SIZE     24    /**< Chunk-size line: up to 16 hex digits + "\r\n" + NUL (18+1); 24 = margin. Real chunks are <= chunk_buf so <= 3 hex digits. */
/** @brief Final chunk length. */
#define KL_CLIENT_FINAL_CHUNK_LEN    5     /**< "0\\r\\n\\r\\n" */
/** @brief Default Happy Eyeballs Connection Attempt Delay, ms (RFC 8305 §5). */
#define KL_CLIENT_CONNECT_ATTEMPT_DELAY_MS 250

/* ── Proxy ────────────────────────────────────────────────────────── */

/**
 * @brief HTTP proxy configuration (borrowed pointers, caller-owned).
 */
typedef struct {
    const char *host;       /**< Proxy hostname */
    uint16_t    port;       /**< Proxy port (typically 8080, 3128) */
    const char *auth;       /**< "Basic <base64>" or NULL (no auth) */
} KlProxyConfig;

/* ── Types ────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;   /**< Header name */
    const char *value;  /**< Header value */
} KlClientHeader;

typedef struct KlClientResponse {
    int              status;
    char            *body;
    size_t           body_len;
    KlClientHeader  *headers;
    int              num_headers;
    KlAllocator      alloc;     /**< Allocator used for body/headers (stored by value) */
    KlError          error;     /**< Diagnostic error code (KL_ERR_NONE on success) */
} KlClientResponse;

typedef struct {
    int              timeout_ms;        /**< Connect/send/recv timeout (0 = default 30s) */
    size_t           max_response_size;  /**< Max response body (0 = default 4 MB) */
    KlTlsConfig     *tls;              /**< TLS config for HTTPS (NULL = no HTTPS) */
    KlResolver      *resolver;          /**< Async DNS resolver — takes precedence over the default. */
    int              system_dns;        /**< Async client: force blocking getaddrinfo (preserves /etc/hosts + search domains). Default 0 = built-in async DNS. Ignored when `resolver` is set. */
    KlDecompressConfig *decompress;     /**< Response decompression (NULL = no decompression) */
    KlProxyConfig   *proxy;             /**< HTTP proxy (NULL = direct connection) */
    int              connect_attempt_delay_ms; /**< Async: Happy Eyeballs Connection Attempt Delay (RFC 8305). 0 = default 250ms. A large value degenerates to sequential connect. */
    const KlSocketProvider *sockets;    /**< custom socket provider (bring-your-own stack); NULL =
                                         *   built-in default. When set, applied to the client's
                                         *   event context. The async client may instead set
                                         *   ctx.sockets directly; a non-NULL value here takes
                                         *   precedence. Must advertise KL_SOCK_CAP_NATIVE_FD. */
} KlClientConfig;

/* ── Streaming types ──────────────────────────────────────────────── */

/**
 * @brief Response body streaming callback (push-based).
 * @param data  Body chunk data.
 * @param len   Chunk length.
 * @param user_data  User context from KlClientStreamCfg.
 * @return 0 to continue, -1 to abort.
 */
typedef int (*KlClientBodyFn)(const char *data, size_t len, void *user_data);

/**
 * @brief Called when response headers are complete.
 * @return 0 to continue, -1 to abort.
 */
typedef int (*KlClientHeadersFn)(int status, const KlClientHeader *headers,
                                  int num_headers, void *user_data);

/**
 * @brief Request body streaming callback (pull-based, like read()).
 * @param buf      Buffer to fill.
 * @param buf_len  Buffer capacity.
 * @param user_data  User context from KlClientStreamCfg.
 * @return >0 bytes produced, 0 = EOF, -1 = error.
 */
typedef kl_ssize_t (*KlClientReadFn)(char *buf, size_t buf_len, void *user_data);

/**
 * @brief Per-request streaming configuration.
 *
 * When stream is non-NULL and on_body is set, response body chunks are
 * pushed via on_body instead of being accumulated into KlClientResponse.body.
 *
 * When body_read is set, the request body is pulled via body_read and sent
 * with Transfer-Encoding: chunked (body/body_len parameters are ignored).
 */
typedef struct {
    KlClientBodyFn     on_body;        /**< Response body callback (NULL = buffer) */
    KlClientHeadersFn  on_headers;    /**< NULL = skip */
    void             (*on_complete)(void *user_data);  /**< NULL = skip */

    KlClientReadFn     body_read;     /**< Request body pull callback (NULL = use body/body_len) */

    void              *user_data;     /**< shared across all callbacks */
} KlClientStreamCfg;

/* ── Sync API ─────────────────────────────────────────────────────── */

/**
 * @brief Perform a synchronous (blocking) HTTP request.
 *
 * Connects, optionally performs TLS handshake, sends request, receives
 * and parses response. Blocks until complete, error, or timeout.
 *
 * @param alloc       Allocator for response data.
 * @param cfg         Client config (timeouts, TLS, limits). May be NULL for defaults.
 * @param method      HTTP method ("GET", "POST", etc.).
 * @param url         Full URL ("http://host/path" or "https://host/path").
 * @param headers     Request headers (may be NULL if num_headers == 0).
 * @param num_headers Number of request headers.
 * @param body        Request body (may be NULL).
 * @param body_len    Request body length.
 * @param resp        Output: populated on success. Caller must call kl_client_response_free().
 * @return 0 on success, -1 on error.
 */
int kl_client_request(KlAllocator *alloc, const KlClientConfig *cfg,
                      const char *method, const char *url,
                      const KlClientHeader *headers, int num_headers,
                      const char *body, size_t body_len,
                      KlClientResponse *resp);

/**
 * @brief Perform a synchronous (blocking) HTTP request with streaming.
 *
 * Same as kl_client_request but supports response and request body streaming.
 * When stream is NULL, behaves identically to kl_client_request.
 *
 * @param alloc       Allocator for response data.
 * @param cfg         Client config (timeouts, TLS, limits). May be NULL for defaults.
 * @param method      HTTP method ("GET", "POST", etc.).
 * @param url         Full URL ("http://host/path" or "https://host/path").
 * @param headers     Request headers (may be NULL if num_headers == 0).
 * @param num_headers Number of request headers.
 * @param body        Request body (may be NULL).
 * @param body_len    Request body length.
 * @param stream      Streaming config (NULL = buffer mode, same as kl_client_request).
 * @param resp        Output: populated on success. Caller must call kl_client_response_free().
 * @return 0 on success, -1 on error.
 */
int kl_client_request_s(KlAllocator *alloc, const KlClientConfig *cfg,
                         const char *method, const char *url,
                         const KlClientHeader *headers, int num_headers,
                         const char *body, size_t body_len,
                         const KlClientStreamCfg *stream,
                         KlClientResponse *resp);

/**
 * @brief Free response resources (body, headers).
 */
void kl_client_response_free(KlClientResponse *resp);

/* ── Async API (requires KlEventCtx, NOT KlServer) ───────────────── */

typedef struct KlClient KlClient;

/**
 * @brief Callback invoked when an async HTTP request completes.
 *
 * Called on the event loop thread. Check kl_client_error() for status,
 * then kl_client_response() for the response data.
 */
typedef void (*KlClientDoneFn)(KlClient *client, void *user_data);

/**
 * @brief Start an asynchronous HTTP request.
 *
 * Creates a non-blocking socket, initiates connect, and registers a
 * watcher on the event context. The state machine drives:
 *   connect → TLS handshake → send → recv/parse
 *
 * When complete (or on error), on_done is called on the event loop thread.
 * The caller must then inspect the result and call kl_client_free().
 *
 * @param ev_ctx      Event context for watcher registration (NOT KlServer).
 * @param alloc       Allocator for client state and response.
 * @param cfg         Client config (timeouts, TLS, limits). May be NULL for defaults.
 * @param method      HTTP method.
 * @param url         Full URL.
 * @param headers     Request headers (may be NULL).
 * @param num_headers Number of request headers.
 * @param body        Request body (may be NULL).
 * @param body_len    Request body length.
 * @param on_done     Callback invoked on completion or error.
 * @param user_data   User data passed to on_done.
 * @return Client handle, or NULL on immediate failure.
 */
KlClient *kl_client_start(KlEventCtx *ev_ctx, KlAllocator *alloc,
                           const KlClientConfig *cfg,
                           const char *method, const char *url,
                           const KlClientHeader *headers, int num_headers,
                           const char *body, size_t body_len,
                           KlClientDoneFn on_done, void *user_data);

/**
 * @brief Start an asynchronous HTTP request with streaming.
 *
 * Same as kl_client_start but supports response and request body streaming.
 * When stream is NULL, behaves identically to kl_client_start.
 *
 * @param ev_ctx      Event context for watcher registration.
 * @param alloc       Allocator for client state and response.
 * @param cfg         Client config (timeouts, TLS, limits). May be NULL for defaults.
 * @param method      HTTP method.
 * @param url         Full URL.
 * @param headers     Request headers (may be NULL).
 * @param num_headers Number of request headers.
 * @param body        Request body (may be NULL).
 * @param body_len    Request body length.
 * @param stream      Streaming config (NULL = buffer mode, same as kl_client_start).
 * @param on_done     Callback invoked on completion or error.
 * @param user_data   User data passed to on_done.
 * @return Client handle, or NULL on immediate failure.
 */
KlClient *kl_client_start_s(KlEventCtx *ev_ctx, KlAllocator *alloc,
                              const KlClientConfig *cfg,
                              const char *method, const char *url,
                              const KlClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              const KlClientStreamCfg *stream,
                              KlClientDoneFn on_done, void *user_data);

/**
 * @brief Get the response from a completed async request.
 * @return Response pointer (valid until kl_client_free), or NULL if error.
 */
const KlClientResponse *kl_client_response(const KlClient *client);

/**
 * @brief Check if the async request completed with an error.
 * @return 0 on success, -1 on error.
 */
int kl_client_error(const KlClient *client);

/**
 * @brief Get the specific error code from a completed async request.
 * @return KL_ERR_NONE on success, specific KlError on failure.
 */
KlError kl_client_last_error(const KlClient *client);

/**
 * @brief Cancel an in-flight async request (removes watcher, closes socket).
 */
void kl_client_cancel(KlClient *client);

/**
 * @brief Free all client resources (response, buffers, parser).
 *
 * Safe to call after kl_client_cancel() or after completion callback.
 */
void kl_client_free(KlClient *client);

/* ── Streaming parser factory ─────────────────────────────────────── */

/**
 * @brief Create a streaming-capable llhttp response parser.
 *
 * When on_body is non-NULL, body chunks are forwarded to the callback
 * instead of being accumulated. Headers/status are still populated in
 * KlClientResponse. Body is NULL and body_len is 0 in the response.
 *
 * @param max_response_size  Body size limit (still enforced in streaming mode).
 * @param alloc              Allocator for parser state.
 * @param on_body            Body chunk callback (NULL = accumulate).
 * @param on_headers         Headers-complete callback (NULL = skip).
 * @param on_complete        Body-complete callback (NULL = skip).
 * @param stream_user_data   User data passed to all callbacks.
 * @return Parser instance, or NULL on allocation failure.
 */
KlResponseParser *kl_response_parser_llhttp_s(size_t max_response_size,
                                                KlAllocator *alloc,
                                                KlClientBodyFn on_body,
                                                KlClientHeadersFn on_headers,
                                                void (*on_complete)(void *),
                                                void *stream_user_data);

#endif /* KEEL_CLIENT_H */
