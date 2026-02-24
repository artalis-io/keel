#ifndef KEEL_H2_H
#define KEEL_H2_H

#include <keel/allocator.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/body_reader.h>
#include <keel/router.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ── Forward declarations ────────────────────────────────────────── */

typedef struct KlH2Session KlH2Session;
typedef struct KlH2Callbacks KlH2Callbacks;
typedef struct KlH2Stream KlH2Stream;
typedef struct KlH2Conn KlH2Conn;
typedef struct KlConn KlConn;

/* ── KlH2Callbacks — KEEL provides these to the session ──────────── */

struct KlH2Callbacks {
    /**
     * @brief Called when a complete HEADERS frame is received (new request).
     * @param ud          Opaque user data (KlH2Conn pointer).
     * @param stream_id   HTTP/2 stream ID.
     * @param method      Request method.
     * @param method_len  Method length.
     * @param path        Request path (may include query).
     * @param path_len    Path length.
     * @param authority   :authority pseudo-header (may be NULL).
     * @param authority_len Authority length.
     * @param hdr_names   Array of header name pointers.
     * @param hdr_values  Array of header value pointers.
     * @param hdr_name_lens  Array of header name lengths.
     * @param hdr_value_lens Array of header value lengths.
     * @param num_headers Number of headers.
     * @return 0 on success, -1 on error.
     */
    int (*on_request)(void *ud, uint32_t stream_id,
                      const char *method, size_t method_len,
                      const char *path, size_t path_len,
                      const char *authority, size_t authority_len,
                      const char **hdr_names, const char **hdr_values,
                      const size_t *hdr_name_lens, const size_t *hdr_value_lens,
                      int num_headers);

    /**
     * @brief Called when DATA frames arrive for a stream.
     * @return 0 on success, -1 to reset the stream.
     */
    int (*on_data)(void *ud, uint32_t stream_id, const char *data, size_t len);

    /**
     * @brief Called when END_STREAM is received (request body complete).
     * @return 0 on success, -1 on error.
     */
    int (*on_stream_end)(void *ud, uint32_t stream_id);

    /**
     * @brief Called when a RST_STREAM is received.
     */
    void (*on_stream_reset)(void *ud, uint32_t stream_id, uint32_t error_code);

    /**
     * @brief Called by the session to send data to the peer.
     * @return Bytes sent, or -1 on error.
     */
    ssize_t (*send)(void *ud, const void *data, size_t len);
};

/* ── KlH2Session — user-provided vtable ──────────────────────────── */

/**
 * @brief Pluggable HTTP/2 session vtable.
 *
 * Users implement this interface to provide HTTP/2 support (nghttp2, etc.).
 * KEEL provides connection lifecycle, stream→request/response mapping,
 * and event loop integration. Handlers see the same KlRequest/KlResponse API.
 */
struct KlH2Session {
    /**
     * @brief Feed received data to the session for parsing.
     * @return Bytes consumed (>= 0), or -1 on fatal error.
     */
    ssize_t (*recv)(KlH2Session *self, const void *data, size_t len);

    /**
     * @brief Submit a response for the given stream.
     * @return 0 on success, -1 on error.
     */
    int (*submit_response)(KlH2Session *self, uint32_t stream_id,
                           int status, const char **hdr_names,
                           const char **hdr_values, int num_headers,
                           const void *body, size_t body_len);

    /**
     * @brief Check if the session has data to send.
     * @return 1 if there is pending output, 0 otherwise.
     */
    int (*want_write)(KlH2Session *self);

    /**
     * @brief Flush pending output (triggers send callback).
     * @return 0 on success, -1 on error.
     */
    int (*flush)(KlH2Session *self);

    /**
     * @brief Initiate graceful shutdown (send GOAWAY).
     * @return 0 on success, -1 on error.
     */
    int (*shutdown)(KlH2Session *self);

    /**
     * @brief Destroy session and free all resources.
     */
    void (*destroy)(KlH2Session *self);
};

/* ── Factory ─────────────────────────────────────────────────────── */

/**
 * @brief Factory creates an HTTP/2 session with KEEL's callbacks wired in.
 * @param alloc      Allocator for session resources.
 * @param callbacks  KEEL-provided callbacks (the session calls these).
 * @param user_data  Opaque pointer passed to callbacks (KlH2Conn).
 * @return New session, or NULL on failure.
 */
typedef KlH2Session *(*KlH2SessionFactory)(KlAllocator *alloc,
                                            KlH2Callbacks *callbacks,
                                            void *user_data);

/* ── Config ──────────────────────────────────────────────────────── */

#define KL_H2_DEFAULT_MAX_STREAMS 128
#define KL_H2_DEFAULT_WINDOW_SIZE 65535

typedef struct KlH2Config {
    KlH2SessionFactory factory;
    int max_concurrent_streams;  /**< 0 = KL_H2_DEFAULT_MAX_STREAMS */
    int initial_window_size;     /**< 0 = KL_H2_DEFAULT_WINDOW_SIZE */
} KlH2Config;

/* ── Per-stream state ────────────────────────────────────────────── */

struct KlH2Stream {
    uint32_t stream_id;
    KlRequest req;
    KlResponse res;
    KlBodyReader *body_reader;
    KlRoute *route;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int route_result;
    int headers_done;
    int body_done;
    int response_submitted;
    char *hdr_storage;        /**< Heap buffer for header name/value copies */
    size_t hdr_storage_len;
};

/* ── Per-connection HTTP/2 state ─────────────────────────────────── */

struct KlH2Conn {
    KlH2Session *session;
    KlH2Callbacks callbacks;
    KlConn *conn;
    KlRouter *router;
    KlAllocator *alloc;
    KlH2Stream *streams;
    int num_streams;
    int max_streams;
    int goaway_sent;
};

/* ── Internal functions (used by connection.c, server.c) ─────────── */

/**
 * @brief Upgrade a connection to HTTP/2 (direct or ALPN).
 * @param c           Connection to upgrade.
 * @param router      Server's router.
 * @param cfg         HTTP/2 configuration.
 * @param leftover    Unprocessed bytes after preface/handshake.
 * @param leftover_len Length of leftover bytes.
 * @return KL_CONN_HTTP2 on success, KL_CONN_CLOSED on failure.
 */
int kl_h2_upgrade(KlConn *c, KlRouter *router, KlH2Config *cfg,
                  const char *leftover, size_t leftover_len);

/**
 * @brief Upgrade from HTTP/1.1 via Upgrade: h2c header.
 * Sends 101 Switching Protocols, then transitions to HTTP/2.
 */
int kl_h2_upgrade_from_h1(KlConn *c, KlRouter *router, KlH2Config *cfg,
                           const char *leftover, size_t leftover_len);

/**
 * @brief Process readable data on an HTTP/2 connection.
 * @return New connection state.
 */
int kl_h2_on_readable(KlConn *c);

/**
 * @brief Process writable event on an HTTP/2 connection.
 * @return New connection state.
 */
int kl_h2_on_writable(KlConn *c);

/**
 * @brief Initiate graceful HTTP/2 shutdown (GOAWAY).
 */
void kl_h2_drain_shutdown(KlConn *c);

/**
 * @brief Clean up all HTTP/2 state on connection release.
 */
void kl_h2_cleanup(KlConn *c);

#endif
