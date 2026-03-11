/*
 * h2_client.h — HTTP/2 client API
 *
 * Async HTTP/2 client driven by KlEventCtx watchers.
 * Uses a pluggable session vtable (KlH2ClientSession) so the actual
 * HTTP/2 framing can be backed by nghttp2 or any other library.
 * Shared protocol constants (max streams, window size) from h2.h.
 */

#ifndef KEEL_H2_CLIENT_H
#define KEEL_H2_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <keel/allocator.h>
#include <keel/event_ctx.h>
#include <keel/h2.h>
#include <keel/tls.h>

/* ── Defaults ────────────────────────────────────────────────────── */

#define KL_H2_CLIENT_DEFAULT_TIMEOUT_MS  30000
#define KL_H2_CLIENT_RECV_BUF_SIZE       16384

/* ── Types ───────────────────────────────────────────────────────── */

typedef struct KlH2ClientConn    KlH2ClientConn;
typedef struct KlH2ClientStream  KlH2ClientStream;
typedef struct KlH2ClientSession KlH2ClientSession;

typedef struct {
    const char *name;
    const char *value;
} KlH2ClientHeader;

/** Accumulated response from a single stream. */
typedef struct {
    int              status;
    KlH2ClientHeader *headers;
    int              num_headers;
    int              headers_cap;
    char             *body;
    size_t           body_len;
    size_t           body_cap;
} KlH2ClientResponse;

/* ── Session callbacks (session -> KEEL) ─────────────────────────── */

typedef struct {
    /** Session has data to send to the network. */
    int  (*on_send)(KlH2ClientSession *s, const void *data, size_t len);
    /** Response headers received for a stream. */
    void (*on_response)(KlH2ClientSession *s, int32_t stream_id,
                        int status, const KlH2ClientHeader *hdrs, int n);
    /** Response body data received for a stream. */
    void (*on_data)(KlH2ClientSession *s, int32_t stream_id,
                    const char *data, size_t len);
    /** Stream closed (0 = no error). */
    void (*on_stream_close)(KlH2ClientSession *s, int32_t stream_id, int err);
} KlH2ClientCallbacks;

/* ── Session vtable (user provides, wraps nghttp2 etc.) ──────────── */

struct KlH2ClientSession {
    /** Feed received network data into the session. */
    int (*recv)(KlH2ClientSession *self, const char *data, size_t len);
    /** Submit an HTTP/2 request, returns stream ID or -1. */
    int32_t (*submit_request)(KlH2ClientSession *self,
                              const char *method, const char *path,
                              const char *authority,
                              const KlH2ClientHeader *hdrs, int n,
                              const char *body, size_t body_len);
    /** Flush pending output (triggers on_send callbacks). */
    int (*flush)(KlH2ClientSession *self);
    /** Destroy session and free resources. */
    void (*destroy)(KlH2ClientSession *self);
    /** KEEL-managed: set by kl_h2_client_connect. */
    KlH2ClientCallbacks keel_cbs;
    /** KEEL-managed: opaque pointer to KlH2ClientConn. */
    void *keel_ctx;
};

typedef KlH2ClientSession *(*KlH2ClientSessionFactory)(KlAllocator *alloc);

/* ── Config ──────────────────────────────────────────────────────── */

typedef struct {
    int                       timeout_ms;              /**< 0 = default */
    int                       max_concurrent_streams;  /**< 0 = KL_H2_DEFAULT_MAX_STREAMS */
    KlTlsConfig              *tls;
    KlH2ClientSessionFactory  session;                 /**< required */
} KlH2ClientConfig;

/* ── Callbacks ───────────────────────────────────────────────────── */

typedef void (*KlH2ClientResponseFn)(KlH2ClientConn *c, int32_t stream_id,
                                      const KlH2ClientResponse *resp,
                                      void *user_data);
typedef void (*KlH2ClientErrorFn)(KlH2ClientConn *c, const char *msg,
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
KlH2ClientConn *kl_h2_client_connect(KlEventCtx *ev, KlAllocator *alloc,
                                      const KlH2ClientConfig *cfg,
                                      const char *url,
                                      KlH2ClientErrorFn on_error,
                                      void *user_data);

/**
 * @brief Submit an HTTP/2 request.
 *
 * @return Stream ID (>0) on success, -1 on error.
 */
int32_t kl_h2_client_request(KlH2ClientConn *c, const char *method,
                              const char *path,
                              const KlH2ClientHeader *hdrs, int n,
                              const char *body, size_t body_len,
                              KlH2ClientResponseFn on_resp, void *ud);

void kl_h2_client_close(KlH2ClientConn *c);
void kl_h2_client_free(KlH2ClientConn *c);
void kl_h2_client_response_free(KlH2ClientResponse *resp, KlAllocator *alloc);

#endif
