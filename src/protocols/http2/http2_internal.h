/*
 * http2_internal.h: INTERNAL HTTP/2 server state.
 *
 * The per-connection (KlHttp2ServerConn) and per-stream (KlHttp2ServerStream) bodies live
 * here, not in the public <keel/http2_server.h>, so they are truly opaque to users: the
 * KlHttp2ServerSession vtable treats the connection as a void*, and only http_connection.c,
 * http_server.c and http2_server.c (plus the white-box h2 unit test) touch these fields. Keeping the
 * bodies internal means new internal state (e.g. the output-writer seam below) is not
 * a public-API change.
 */
#ifndef KEEL_SRC_HTTP2_INTERNAL_H
#define KEEL_SRC_HTTP2_INTERNAL_H

#include <keel/http2_server.h>    /* public vtable / callbacks / config typedefs */
#include <keel/http_connection.h>   /* KlHttpConn */
#include "../http/http_conn_internal.h"
#include <keel/http_request.h>
#include <keel/http_response.h>
#include <keel/http_body_reader.h>
#include <keel/http_router.h>
#include <stdint.h>
#include <stddef.h>

/* ── HTTP/2 server drive + output-writer seam ────
 * The HTTP/2-specific cross-TU decls the HTTP core / completion driver call: the
 * plaintext-feed entry and the per-connection output writer. HTTP coordinates HTTP/2
 * through this seam (src/protocols/http/ → src/protocols/http2/, per the frozen §147 rule). */

/* Drive the HTTP/2 server session with already-received plaintext: parse frames +
 * flush produced output. Returns the next KlHttpConnState. Defined in http2_server.c. */
KlHttpConnState kl_http2_server_feed(KlHttpConn *c, const void *data, size_t len);

/* HTTP/2 output boundary: the h2 server writes produced frame bytes through a per-connection
 * writer, which writes the connection (conn_write: the socket on readiness, the output queue on a
 * completion loop). Defined in http2_server.c. */
typedef kl_ssize_t (*KlHttp2WriteFn)(void *ctx, const void *data, size_t len);

/* ── Per-stream state ────────────────────────────────────────────── */

struct KlHttp2ServerStream {
    uint32_t stream_id;         /**< HTTP/2 stream identifier. */
    KlHttpRequest req;              /**< Parsed request for this stream. */
    KlHttpResponse res;             /**< Response builder for this stream. */
    KlHttpBodyReader *body_reader;  /**< Body reader (route-provided or NULL). */
    KlHttpRoute *route;             /**< Matched route (NULL if no match). */
    KlHttpParam params[KL_HTTP_ROUTER_MAX_PARAMS]; /**< Extracted route parameters. */
    int num_params;             /**< Number of extracted parameters. */
    int route_result;           /**< Route match result code. */
    int reader_lazy;            /**< No content-length: make the body reader on the first DATA. */
    size_t body_received;       /**< Total body bytes received so far. */
    int response_submitted;     /**< Non-zero after response submitted. */
    char *hdr_storage;          /**< Contiguous header name/value storage. */
    size_t hdr_storage_len;     /**< Length of hdr_storage in bytes. */
    /* Slots are stable: a stream never moves while it lives, so its response (&res) can be held by
     * a producer across events. */
    int in_use;                 /**< The slot holds a live stream. */
    struct KlHttp2ServerConn *h2c; /**< Owning connection (the stream sink's context needs it). */
    /* A streamed response (kl_http_response_begin_stream): its HEADERS were submitted and the
     * session pulls its body (read_body) from the buffer below as flow control allows. */
    int streaming;              /**< Body pulled by the session; the stream lives until it closes. */
    int body_end;               /**< The producer ended the body. */
    int eof_sent;               /**< read_body reported the end (END_STREAM follows). */
    int deferred;               /**< read_body answered DEFERRED: resume_body is owed on new data. */
    int progress_due;           /**< The session took bytes: fire the writable edge after the flush. */
    int reset_due;              /**< The stream failed: reset it (RST_STREAM) on the next flush. */
    char *obuf;                 /**< Body bytes accepted and not yet pulled: [ooff, olen). */
    size_t ooff, olen, ocap;
};

/* ── Per-connection HTTP/2 state ─────────────────────────────────── */

struct KlHttp2ServerConn {
    KlHttp2ServerSession *session;    /**< Active HTTP/2 session (user-provided vtable). */
    KlHttp2ServerCallbacks callbacks; /**< Callbacks wired to KEEL internals. */
    KlHttpConn *conn;                  /**< Underlying TCP connection. */
    KlHttpRouter *router;              /**< Router for dispatching streams. */
    KlAllocator *alloc;            /**< Allocator for stream/header storage. */
    KlHttp2ServerStream *streams;     /**< Array of active streams. */
    int num_streams;               /**< Number of active streams. */
    int max_streams;               /**< Maximum concurrent streams allowed. */
    int goaway_sent;               /**< Non-zero after GOAWAY sent. */
    int upgrading;                 /**< h2c Upgrade: stream 1 is the HTTP/1.1 request, whose pre-body
                                        middleware already ran */
    /* Output boundary. Produced frame bytes flow through out_write, which writes the connection
     * (conn_write): on a completion loop that puts them on the connection's output queue, one
     * ordered overlapped send at a time. */
    KlHttp2WriteFn out_write;         /**< Output sink for produced frames. */
    void       *out_ctx;           /**< Context for out_write. */
    int slots_hwm;                 /**< One past the highest live slot (find scans [0, slots_hwm)). */
    /* Streamed responses are written from outside the connection's own events (a timer, a watcher):
     * their output is sent by a flush on the next loop iteration, from a zero-delay timer. */
    int64_t flush_timer;           /**< The pending flush timer (valid while flush_armed). */
    int flush_armed;               /**< 1 = a flush is scheduled. */
    int out_refused;               /**< A completion output queue at its bound refused frames: flush
                                        again once a send completes. */
};

/* A send completed on a completion-driven HTTP/2 connection: flush what the session holds back
 * (frames the output queue refused at its bound) and fire due writable edges. Returns the next
 * KlHttpConnState. Defined in http2_server.c. */
KlHttpConnState kl_http2_server_comp_sent(KlHttpConn *c);

#endif /* KEEL_SRC_HTTP2_INTERNAL_H */
