/**
 * @file h2_server.h
 * @brief Server-side HTTP/2 API
 *
 * Server-side HTTP/2 types and functions. Shared protocol constants
 * (max streams, window size) live in h2.h.
 */

#ifndef KEEL_H2_SERVER_H
#define KEEL_H2_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <keel/h2.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/body_reader.h>
#include <keel/router.h>

/* ── Forward declarations ────────────────────────────────────────── */

typedef struct KlH2ServerSession KlH2ServerSession;
typedef struct KlH2ServerCallbacks KlH2ServerCallbacks;
typedef struct KlH2ServerStream KlH2ServerStream;
typedef struct KlH2ServerConn KlH2ServerConn;
typedef struct KlConn KlConn;

/* ── KlH2ServerCallbacks — KEEL provides these to the session ────── */

struct KlH2ServerCallbacks {
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
     * retrying on the next @ref KlH2ServerSession::flush call.  A
     * session implementation that assumes "send always writes
     * everything" will silently truncate HTTP/2 frames under load,
     * which the peer then rejects with PROTOCOL_ERROR or
     * COMPRESSION_ERROR.
     *
     * (KEEL's WebSocket server wraps the equivalent boundary with
     * @ref kl_drain — there's no analogous helper for HTTP/2 yet;
     * the session library is expected to manage its own send queue.)
     */
    ssize_t (*send)(void *ud, const void *data, size_t len);
};

/* ── KlH2ServerSession — user-provided vtable ───────────────────── */

struct KlH2ServerSession {
    ssize_t (*recv)(KlH2ServerSession *self, const void *data,
                    size_t len);                   /**< Feed received network data. */
    int (*submit_response)(KlH2ServerSession *self, uint32_t stream_id,
                           int status, const char **hdr_names,
                           const char **hdr_values, int num_headers,
                           const void *body, size_t body_len);
                                                   /**< Submit a response for a stream. */
    int (*want_write)(KlH2ServerSession *self);    /**< Returns non-zero if output is pending. */
    int (*flush)(KlH2ServerSession *self);         /**< Flush pending output via send callback. */
    int (*shutdown)(KlH2ServerSession *self);      /**< Initiate graceful GOAWAY. */
    void (*destroy)(KlH2ServerSession *self);      /**< Free session resources. */
};

/* ── Factory ─────────────────────────────────────────────────────── */

/** @brief Factory for creating server-side HTTP/2 sessions. */
typedef KlH2ServerSession *(*KlH2ServerSessionFactory)(KlAllocator *alloc,
                                                        KlH2ServerCallbacks *callbacks,
                                                        void *user_data);

/* ── Config ──────────────────────────────────────────────────────── */

typedef struct KlH2ServerConfig {
    KlH2ServerSessionFactory factory; /**< Session factory (required). */
    int max_concurrent_streams;  /**< 0 = KL_H2_DEFAULT_MAX_STREAMS */
    int initial_window_size;     /**< 0 = KL_H2_DEFAULT_WINDOW_SIZE */
} KlH2ServerConfig;

/* ── Per-stream state ────────────────────────────────────────────── */

struct KlH2ServerStream {
    uint32_t stream_id;         /**< HTTP/2 stream identifier. */
    KlRequest req;              /**< Parsed request for this stream. */
    KlResponse res;             /**< Response builder for this stream. */
    KlBodyReader *body_reader;  /**< Body reader (route-provided or NULL). */
    KlRoute *route;             /**< Matched route (NULL if no match). */
    KlParam params[KL_MAX_PARAMS]; /**< Extracted route parameters. */
    int num_params;             /**< Number of extracted parameters. */
    int route_result;           /**< Route match result code. */
    int headers_done;           /**< Non-zero after HEADERS frame received. */
    int body_done;              /**< Non-zero after END_STREAM received. */
    size_t body_received;       /**< Total body bytes received so far. */
    int response_submitted;     /**< Non-zero after response submitted. */
    char *hdr_storage;          /**< Contiguous header name/value storage. */
    size_t hdr_storage_len;     /**< Length of hdr_storage in bytes. */
};

/* ── Per-connection HTTP/2 state ─────────────────────────────────── */

struct KlH2ServerConn {
    KlH2ServerSession *session;    /**< Active HTTP/2 session (user-provided vtable). */
    KlH2ServerCallbacks callbacks; /**< Callbacks wired to KEEL internals. */
    KlConn *conn;                  /**< Underlying TCP connection. */
    KlRouter *router;              /**< Router for dispatching streams. */
    KlAllocator *alloc;            /**< Allocator for stream/header storage. */
    KlH2ServerStream *streams;     /**< Array of active streams. */
    int num_streams;               /**< Number of active streams. */
    int max_streams;               /**< Maximum concurrent streams allowed. */
    int goaway_sent;               /**< Non-zero after GOAWAY sent. */
    /* Completion-loop output capture (PAL 8d-3). When out_capture is set, KEEL's send
     * callback appends produced frame bytes to out_buf instead of writing the socket,
     * so the completion driver can post them as one ordered overlapped send (frames must
     * not reorder). Driver-driven; unused and zero-cost on the readiness path
     * (out_capture stays 0 → output goes straight to conn_write, unchanged). Internal —
     * the session vtable never sees these. */
    char  *out_buf;                /**< Captured outgoing frame bytes (completion). */
    size_t out_len;                /**< Bytes captured. */
    size_t out_cap;                /**< out_buf capacity. */
    int    out_capture;            /**< 1 = capture to out_buf instead of conn_write. */
};

/* ── Internal functions (used by connection.c, server.c) ─────────── */

/** @brief Upgrade a connection to HTTP/2 (direct h2c). */
int  kl_h2_server_upgrade(KlConn *c, KlRouter *router, KlH2ServerConfig *cfg,
                           const char *leftover, size_t leftover_len);
/** @brief Upgrade a connection to HTTP/2 from an HTTP/1.1 Upgrade request. */
int  kl_h2_server_upgrade_from_h1(KlConn *c, KlRouter *router,
                                   KlH2ServerConfig *cfg,
                                   const char *leftover, size_t leftover_len);
/** @brief Handle readable event on an HTTP/2 connection. */
int  kl_h2_server_on_readable(KlConn *c);
/** @brief Handle writable event on an HTTP/2 connection. */
int  kl_h2_server_on_writable(KlConn *c);
/** @brief Initiate graceful GOAWAY drain on an HTTP/2 connection. */
void kl_h2_server_drain_shutdown(KlConn *c);
/** @brief Clean up all HTTP/2 state for a connection. */
void kl_h2_server_cleanup(KlConn *c);

#endif
