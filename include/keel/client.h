/*
 * client.h — HTTP/1.1 client (sync + async)
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
#include <keel/event_ctx.h>
#include <keel/parser.h>
#include <keel/resolver.h>
#include <keel/tls.h>
#include <keel/url.h>

/* ── Constants ────────────────────────────────────────────────────── */

#define KL_CLIENT_HOSTNAME_MAX       256
#define KL_CLIENT_REQ_BUF_SIZE       4096
#define KL_CLIENT_MAX_REQ_HEADERS    64
#define KL_CLIENT_DEFAULT_TIMEOUT_MS 30000
#define KL_CLIENT_DEFAULT_MAX_RESP   (4 * 1024 * 1024)
#define KL_CLIENT_RECV_BUF_SIZE      8192

/* ── Types ────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *value;
} KlClientHeader;

typedef struct KlClientResponse {
    int              status;
    char            *body;
    size_t           body_len;
    KlClientHeader  *headers;
    int              num_headers;
    KlAllocator      alloc;     /**< Allocator used for body/headers (stored by value) */
} KlClientResponse;

typedef struct {
    int              timeout_ms;        /**< Connect/send/recv timeout (0 = default 30s) */
    size_t           max_response_size;  /**< Max response body (0 = default 4 MB) */
    KlTlsConfig     *tls;              /**< TLS config for HTTPS (NULL = no HTTPS) */
    KlResolver      *resolver;          /**< Async DNS resolver (NULL = sync getaddrinfo) */
} KlClientConfig;

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
 * @brief Cancel an in-flight async request (removes watcher, closes socket).
 */
void kl_client_cancel(KlClient *client);

/**
 * @brief Free all client resources (response, buffers, parser).
 *
 * Safe to call after kl_client_cancel() or after completion callback.
 */
void kl_client_free(KlClient *client);

#endif /* KEEL_CLIENT_H */
