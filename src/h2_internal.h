/*
 * h2_internal.h — INTERNAL HTTP/2 server state (PAL 8d-4).
 *
 * The per-connection (KlH2ServerConn) and per-stream (KlH2ServerStream) bodies live
 * here, not in the public <keel/h2_server.h>, so they are truly opaque to users: the
 * KlH2ServerSession vtable treats the connection as a void*, and only connection.c,
 * server.c and h2.c (plus the white-box h2 unit test) touch these fields. Keeping the
 * bodies internal means new internal state — e.g. the output-writer seam below — is not
 * a public-API change.
 */
#ifndef KEEL_SRC_H2_INTERNAL_H
#define KEEL_SRC_H2_INTERNAL_H

#include <keel/h2_server.h>    /* public vtable / callbacks / config typedefs */
#include <keel/connection.h>   /* KlConn */
#include <keel/request.h>
#include <keel/response.h>
#include <keel/body_reader.h>
#include <keel/router.h>
#include <stdint.h>
#include <stddef.h>
#include "internal.h"          /* KlH2WriteFn (the output seam type) */

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
    /* Output boundary seam (PAL 8d-4). Produced frame bytes flow through out_write; the
     * default writes the socket (conn_write). A completion driver installs a buffering
     * writer (kl_h2_server_set_writer) to collect a feed's frames for one ordered
     * overlapped send — symmetric with the WebSocket server's kl_drain boundary. The
     * readiness path always uses the default writer, so it is unchanged. */
    KlH2WriteFn out_write;         /**< Output sink for produced frames. */
    void       *out_ctx;           /**< Context for out_write. */
};

#endif /* KEEL_SRC_H2_INTERNAL_H */
