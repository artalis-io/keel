/**
 * @file h2_server.h
 * @brief Server-side HTTP/2 API
 *
 * Server-side HTTP/2 types and functions. Shared protocol constants
 * (max streams, window size) live in h2.h.
 */

#ifndef KEEL_HTTP2_SERVER_H
#define KEEL_HTTP2_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <keel/handle.h>   /* kl_ssize_t */
#include <keel/http2.h>
#include <keel/http_request.h>
#include <keel/http_response.h>
#include <keel/http_body_reader.h>
#include <keel/http_router.h>

/* ── Forward declarations ────────────────────────────────────────── */

typedef struct KlHttp2ServerSession KlHttp2ServerSession;
typedef struct KlHttp2ServerCallbacks KlHttp2ServerCallbacks;
typedef struct KlHttp2ServerStream KlHttp2ServerStream;
typedef struct KlHttp2ServerConn KlHttp2ServerConn;
typedef struct KlHttpConn KlHttpConn;

/* ── KlHttp2ServerCallbacks — KEEL provides these to the session ────── */

struct KlHttp2ServerCallbacks {
    int (*on_request)(void *ud, uint32_t stream_id,
                      const char *method, size_t method_len,
                      const char *path, size_t path_len,
                      const char *authority, size_t authority_len,
                      const char **hdr_names, const char **hdr_values,
                      const size_t *hdr_name_lens, const size_t *hdr_value_lens,
                      int num_headers);          /**< Headers received for a new stream. */
    int (*on_data)(void *ud, uint32_t stream_id,
                   const char *data, size_t len); /**< Body data received for a stream. */
    int (*on_stream_end)(void *ud,
                         uint32_t stream_id);     /**< Stream fully received (END_STREAM). */
    void (*on_stream_reset)(void *ud, uint32_t stream_id,
                            uint32_t error_code); /**< Stream reset by peer. */
    /**
     * Write data to the network.  May return short — `< len` bytes
     * written — under socket backpressure.  The session library MUST
     * handle short returns by re-queuing the un-written tail and
     * retrying on the next @ref KlHttp2ServerSession::flush call.  A
     * session implementation that assumes "send always writes
     * everything" will silently truncate HTTP/2 frames under load,
     * which the peer then rejects with PROTOCOL_ERROR or
     * COMPRESSION_ERROR.
     *
     * (KEEL's WebSocket server wraps the equivalent boundary with
     * @ref kl_drain — there's no analogous helper for HTTP/2 yet;
     * the session library is expected to manage its own send queue.)
     */
    kl_ssize_t (*send)(void *ud, const void *data, size_t len);
};

/* ── KlHttp2ServerSession — user-provided vtable ───────────────────── */

struct KlHttp2ServerSession {
    kl_ssize_t (*recv)(KlHttp2ServerSession *self, const void *data,
                       size_t len);                /**< Feed received network data. */
    int (*submit_response)(KlHttp2ServerSession *self, uint32_t stream_id,
                           int status, const char **hdr_names,
                           const char **hdr_values, int num_headers,
                           const void *body, size_t body_len);
                                                   /**< Submit a response for a stream. */
    int (*want_write)(KlHttp2ServerSession *self);    /**< Returns non-zero if output is pending. */
    int (*flush)(KlHttp2ServerSession *self);         /**< Flush pending output via send callback. */
    int (*shutdown)(KlHttp2ServerSession *self);      /**< Initiate graceful GOAWAY. */
    void (*destroy)(KlHttp2ServerSession *self);      /**< Free session resources. */
    /**
     * Optional (may be NULL): returns non-zero while the session still wants to
     * read more input. When a session provides it, KEEL closes the connection
     * once the session wants neither read nor write — i.e. it has terminated
     * (e.g. sent a GOAWAY for a protocol error, or completed a graceful
     * shutdown). A NULL want_read keeps the connection open until the peer
     * closes (legacy behavior).
     */
    int (*want_read)(KlHttp2ServerSession *self);
};

/* ── Factory ─────────────────────────────────────────────────────── */

/** @brief Factory for creating server-side HTTP/2 sessions. */
typedef KlHttp2ServerSession *(*KlHttp2ServerSessionFactory)(KlAllocator *alloc,
                                                        KlHttp2ServerCallbacks *callbacks,
                                                        void *user_data);

/* ── Config ──────────────────────────────────────────────────────── */

typedef struct KlHttp2ServerConfig {
    KlHttp2ServerSessionFactory factory; /**< Session factory (required). */
    int max_concurrent_streams;  /**< 0 = KL_HTTP2_DEFAULT_MAX_STREAMS */
    int initial_window_size;     /**< 0 = KL_HTTP2_DEFAULT_WINDOW_SIZE */
} KlHttp2ServerConfig;

/* Per-stream (KlHttp2ServerStream) and per-connection (KlHttp2ServerConn) state are opaque —
 * their bodies are internal (src/http2_internal.h). Users interact with HTTP/2 only through
 * the KlHttp2ServerSession vtable + KlHttp2ServerConfig above; the connection is passed to the
 * session callbacks as an opaque void*. Keeping the bodies out of this public header lets
 * KEEL evolve internal h2 state (buffering seams, etc.) without a public-API change. */

/* ── Internal functions (used by connection.c, server.c) ─────────── */

/** @brief Upgrade a connection to HTTP/2 (direct h2c). */
int  kl_http2_server_upgrade(KlHttpConn *c, KlHttpRouter *router, KlHttp2ServerConfig *cfg,
                           const char *leftover, size_t leftover_len);
/** @brief Upgrade a connection to HTTP/2 from an HTTP/1.1 Upgrade request. */
int  kl_http2_server_upgrade_from_h1(KlHttpConn *c, KlHttpRouter *router,
                                   KlHttp2ServerConfig *cfg,
                                   const char *leftover, size_t leftover_len);
/** @brief Handle readable event on an HTTP/2 connection. */
int  kl_http2_server_on_readable(KlHttpConn *c);
/** @brief Handle writable event on an HTTP/2 connection. */
int  kl_http2_server_on_writable(KlHttpConn *c);
/** @brief Initiate graceful GOAWAY drain on an HTTP/2 connection. */
void kl_http2_server_drain_shutdown(KlHttpConn *c);
/** @brief Clean up all HTTP/2 state for a connection. */
void kl_http2_server_cleanup(KlHttpConn *c);

#endif
