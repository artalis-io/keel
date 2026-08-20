/*
 * h2_internal.h — INTERNAL HTTP/2 server state (PAL 8d-4).
 *
 * The per-connection (KlHttp2ServerConn) and per-stream (KlHttp2ServerStream) bodies live
 * here, not in the public <keel/http2_server.h>, so they are truly opaque to users: the
 * KlHttp2ServerSession vtable treats the connection as a void*, and only connection.c,
 * server.c and h2.c (plus the white-box h2 unit test) touch these fields. Keeping the
 * bodies internal means new internal state — e.g. the output-writer seam below — is not
 * a public-API change.
 */
#ifndef KEEL_SRC_HTTP2_INTERNAL_H
#define KEEL_SRC_HTTP2_INTERNAL_H

#include <keel/http2_server.h>    /* public vtable / callbacks / config typedefs */
#include <keel/http_connection.h>   /* KlHttpConn */
#include <keel/http_request.h>
#include <keel/http_response.h>
#include <keel/http_body_reader.h>
#include <keel/http_router.h>
#include <stdint.h>
#include <stddef.h>
#include "internal.h"          /* KlHttp2WriteFn (the output seam type) */

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
    int headers_done;           /**< Non-zero after HEADERS frame received. */
    int body_done;              /**< Non-zero after END_STREAM received. */
    size_t body_received;       /**< Total body bytes received so far. */
    int response_submitted;     /**< Non-zero after response submitted. */
    char *hdr_storage;          /**< Contiguous header name/value storage. */
    size_t hdr_storage_len;     /**< Length of hdr_storage in bytes. */
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
    /* Output boundary seam (PAL 8d-4). Produced frame bytes flow through out_write; the
     * default writes the socket (conn_write). A completion driver installs a buffering
     * writer (kl_http2_server_set_writer) to collect a feed's frames for one ordered
     * overlapped send — symmetric with the WebSocket server's kl_drain boundary. The
     * readiness path always uses the default writer, so it is unchanged. */
    KlHttp2WriteFn out_write;         /**< Output sink for produced frames. */
    void       *out_ctx;           /**< Context for out_write. */
};

#endif /* KEEL_SRC_HTTP2_INTERNAL_H */
