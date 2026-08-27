/**
 * @file http2_client.h
 * @brief HTTP/2 client API
 *
 * Async HTTP/2 client driven by KlEventCtx watchers.
 * Uses a pluggable session vtable (KlHttp2ClientSession) so the actual
 * HTTP/2 framing can be backed by nghttp2 or any other library.
 * Shared protocol constants (max streams, window size) from http2.h.
 */

#ifndef KEEL_HTTP2_CLIENT_H
#define KEEL_HTTP2_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <keel/allocator.h>
#include <keel/event_ctx.h>
#include <keel/http2.h>
#include <keel/tls.h>
#ifdef __cplusplus
extern "C" {
#endif


/* ── Defaults ────────────────────────────────────────────────────── */

/** @brief Default connect/request timeout (ms). */
#define KL_HTTP2_CLIENT_DEFAULT_TIMEOUT_MS  30000
/** @brief Receive buffer size (bytes). */
#define KL_HTTP2_CLIENT_RECV_BUF_SIZE       16384

/* ── Types ───────────────────────────────────────────────────────── */

typedef struct KlHttp2ClientConn    KlHttp2ClientConn;
typedef struct KlHttp2ClientStream  KlHttp2ClientStream;
typedef struct KlHttp2ClientSession KlHttp2ClientSession;

typedef struct {
    const char *name;   /**< Header name. */
    const char *value;  /**< Header value. */
} KlHttp2ClientHeader;

/** Accumulated response from a single stream. */
typedef struct {
    int              status;      /**< HTTP status code. */
    KlHttp2ClientHeader *headers;   /**< Response headers array. */
    int              num_headers; /**< Number of response headers. */
    int              headers_cap; /**< Allocated capacity of headers array. */
    char             *body;       /**< Response body (allocator-owned). */
    size_t           body_len;    /**< Length of response body in bytes. */
    size_t           body_cap;    /**< Allocated capacity of body buffer. */
} KlHttp2ClientResponse;

/* ── Session callbacks (session -> KEEL) ─────────────────────────── */

typedef struct {
    /** Session has data to send to the network. */
    int  (*on_send)(KlHttp2ClientSession *s, const void *data, size_t len);
    /** Response headers received for a stream. */
    void (*on_response)(KlHttp2ClientSession *s, int32_t stream_id,
                        int status, const KlHttp2ClientHeader *hdrs, int n);
    /** Response body data received for a stream. */
    void (*on_data)(KlHttp2ClientSession *s, int32_t stream_id,
                    const char *data, size_t len);
    /** Stream closed (0 = no error). */
    void (*on_stream_close)(KlHttp2ClientSession *s, int32_t stream_id, int err);
} KlHttp2ClientCallbacks;

/* ── Session vtable (user provides, wraps nghttp2 etc.) ──────────── */

/*
 * Append-only vtable (see docs/contracts/compatibility.md): the adapter
 * zero-initializes and recompiles per major version. Required (the adapter fills
 * them; core calls them): recv, submit_request, flush, destroy. KEEL-managed
 * (core writes them; the adapter leaves them zero): keel_cbs, keel_ctx. New ops
 * are appended after keel_ctx. See docs/f2_c_extensibility_decision.md for the
 * mixed-ownership note.
 */
struct KlHttp2ClientSession {
    /** Feed received network data into the session. */
    int (*recv)(KlHttp2ClientSession *self, const char *data, size_t len);
    /** Submit an HTTP/2 request, returns stream ID or -1. */
    int32_t (*submit_request)(KlHttp2ClientSession *self,
                              const char *method, const char *path,
                              const char *authority,
                              const KlHttp2ClientHeader *hdrs, int n,
                              const char *body, size_t body_len);
    /** Flush pending output (triggers on_send callbacks). */
    int (*flush)(KlHttp2ClientSession *self);
    /** Destroy session and free resources. */
    void (*destroy)(KlHttp2ClientSession *self);
    /** KEEL-managed: set by kl_http2_client_connect. */
    KlHttp2ClientCallbacks keel_cbs;
    /** KEEL-managed: opaque pointer to KlHttp2ClientConn. */
    void *keel_ctx;
};

/** @brief Factory for creating client-side HTTP/2 sessions.
 *  The signature is frozen (see docs/contracts/compatibility.md): pass new
 *  construction inputs through the allocator context, never by changing it. */
typedef KlHttp2ClientSession *(*KlHttp2ClientSessionFactory)(KlAllocator *alloc);

/* ── Config ──────────────────────────────────────────────────────── */

/*
 * Append-only config (see docs/contracts/compatibility.md): callers
 * zero-initialize and recompile per major version; session is required; every
 * other member is optional and its zero/NULL value selects the built-in default.
 * New members are appended after session.
 */
typedef struct {
    int                       timeout_ms;              /**< 0 = default */
    int                       max_concurrent_streams;  /**< 0 = KL_HTTP2_DEFAULT_MAX_STREAMS */
    KlTlsConfig              *tls;
    KlHttp2ClientSessionFactory  session;                 /**< required */
} KlHttp2ClientConfig;

/* ── Callbacks ───────────────────────────────────────────────────── */

/** @brief Per-stream response completion callback. */
typedef void (*KlHttp2ClientResponseFn)(KlHttp2ClientConn *c, int32_t stream_id,
                                      const KlHttp2ClientResponse *resp,
                                      void *user_data);
/** @brief Connection-level error callback. */
typedef void (*KlHttp2ClientErrorFn)(KlHttp2ClientConn *c, const char *msg,
                                   void *user_data);

/* ── Public API ──────────────────────────────────────────────────── */

/**
 * @brief Connect to an HTTP/2 server.
 *
 * Initiates TCP connect, optional TLS handshake, then creates the
 * H2 session via the factory. Drives I/O via KlEventCtx watchers.
 *
 * @param ev      Event context.
 * @param alloc   Allocator (must outlive connection).
 * @param cfg     Configuration (session factory required).
 * @param url     Target URL (http:// or https://).
 * @param on_error Error callback.
 * @param user_data Opaque pointer for callbacks.
 * @return Connection handle, or NULL on failure.
 */
KlHttp2ClientConn *kl_http2_client_connect(KlEventCtx *ev, KlAllocator *alloc,
                                      const KlHttp2ClientConfig *cfg,
                                      const char *url,
                                      KlHttp2ClientErrorFn on_error,
                                      void *user_data);

/**
 * @brief Submit an HTTP/2 request.
 *
 * @return Stream ID (>0) on success, -1 on error.
 */
int32_t kl_http2_client_request(KlHttp2ClientConn *c, const char *method,
                              const char *path,
                              const KlHttp2ClientHeader *hdrs, int n,
                              const char *body, size_t body_len,
                              KlHttp2ClientResponseFn on_resp, void *ud);

/** @brief Close the HTTP/2 client connection. */
void kl_http2_client_close(KlHttp2ClientConn *c);
/** @brief Free all HTTP/2 client resources. */
void kl_http2_client_free(KlHttp2ClientConn *c);
/** @brief Free a response's headers and body (allocator-owned). */
void kl_http2_client_response_free(KlHttp2ClientResponse *resp, KlAllocator *alloc);

#ifdef __cplusplus
}
#endif

#endif
