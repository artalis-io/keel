/*
 * http_conn_internal.h: INTERNAL. The concrete KlHttpConn layout + the server/driver connection core.
 *
 * KlHttpConn is opaque on the public surface (a forward-declared, pool-owned borrowed handle in
 * <keel/http_connection.h>). Its concrete struct, its state enum and internal constants, the pool
 * management functions, and the readiness/completion driver entry points all live here. Include this
 * ONLY from HTTP-family implementation TUs (src/protocols/http, http2, websocket) and explicitly
 * justified white-box tests. Substrate and integrations must NOT include it: a completion backend
 * carries a KlStream pointer or a tagged pointer and never holds or dereferences a KlHttpConn.
 *
 * The model-blind protocol core below (dispatch_request/run_post_body/ingest_body/send_complete) is a
 * thin set of non-static handles onto http_connection.c's static helpers, so the completion driver can
 * reuse the core without the readiness transport wrapper and without the completion concept leaking
 * back into http_connection.c. See docs/archive/phases/phase8_iocp_design.md.
 */
#ifndef KEEL_SRC_HTTP_CONN_INTERNAL_H
#define KEEL_SRC_HTTP_CONN_INTERNAL_H

#include <keel/http_connection.h>   /* KlHttpConn forward decl + KlHttpConnPool shell + public accessors */
#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/http1_chunked.h>
#include <keel/file_io.h>
#include <keel/http_request.h>
#include <keel/http_response.h>
#include <keel/http1_parser.h>
#include <keel/http_router.h>
#include <keel/sockaddr.h>
#include <keel/drain.h>
#include <keel/stream.h>
#include <keel/stream_detail.h>   /* struct KlStream layout: KlHttpConn embeds it by value */
#include <keel/listener.h>        /* KlSlotLease */
#include <stddef.h>
#include <stdint.h>

/* Default read buffer size (bytes). Internal. */
#define KL_HTTP_CONN_READ_BUF_SIZE 8192

/* peer_source values (internal; callers use kl_http_conn_peer_addr()). */
#define KL_HTTP_PEER_SOCKET 0   /* Address came from the accepted socket */
#define KL_HTTP_PEER_PROXY  1   /* Address came from a trusted PROXY header */

typedef struct KlTls KlTls;
typedef struct KlWsServerConn KlWsServerConn;
typedef struct KlHttp2ServerConn KlHttp2ServerConn;
typedef struct KlHttp2ServerConfig KlHttp2ServerConfig;

typedef enum {
/* INVARIANT for every consumer of a state RETURNED by one of the kl_http_conn_* drivers: dispatch
 * it semantically. Do not collapse it into a two-way "the one state I expect, else close" test.
 * This enum grows, and such a test silently mis-handles each new member: DRAINING was added in #278
 * and four completion-axis sites treating "not READING" as "close now" closed connections on top of
 * unread request bytes (#270), destroying responses that had already been written. A fifth left the
 * connection registered for nothing at all. Either switch over the states, or route anything the
 * site does not specifically handle to a shared dispatcher (comp_after_state /
 * comp_after_send_complete on the completion axis, the `transition:` block on the readiness axis)
 * rather than to a close. */
    KL_HTTP_CONN_PROXY_HEADER,    /* Reading a PROXY protocol header (pre-TLS) */
    KL_HTTP_CONN_TLS_HANDSHAKE,   /* TLS handshake in progress */
    KL_HTTP_CONN_READING,         /* Reading request headers */
    KL_HTTP_CONN_READING_BODY,    /* Reading request body */
    KL_HTTP_CONN_PROCESSING,      /* Processing request */
    KL_HTTP_CONN_SENDING,         /* Sending response */
    KL_HTTP_CONN_WEBSOCKET,       /* WebSocket connection (upgraded) */
    KL_HTTP_CONN_HTTP2,           /* HTTP/2 connection (upgraded) */
    KL_HTTP_CONN_SUSPENDED,       /* Suspended for async operation */
    KL_HTTP_CONN_DRAINING,        /* Final response flushed; draining the unread request body
                                   * before close so the peer is not RST'd (see #278). */
    KL_HTTP_CONN_CLOSED           /* Connection closed */
} KlHttpConnState;

struct KlHttpConn {
    KlStream stream;            /* Raw-transport subset (fd, alloc, ctx, peer_addr, read_buf/len/cap) */

    KlHttpConnState state;

    uint8_t peer_source;        /* KL_HTTP_PEER_SOCKET | KL_HTTP_PEER_PROXY */

    size_t max_header_size;     /* Max header size (from KlHttpServerConfig) */

    KlHttpRequest req;
    KlHttpResponse res;
    KlHttp1Parser *parser;

    size_t hdr_sent;

    KlHttpRoute *route;
    KlHttpParam params[KL_HTTP_ROUTER_MAX_PARAMS];
    int num_params;
    int route_result;

    uint64_t last_active_ms;
    uint64_t request_start_ms;
    uint64_t body_start_ms;
    /* Request-body framing progress. The post-rejection drain terminates on FRAMING COMPLETION, not
     * on a byte count: for chunked, wire bytes and decoded bytes differ, so only the decoder knows
     * when the request is actually finished (terminal chunk, trailers, or a malformed chunk). The
     * byte figures are an optimization for Content-Length, where remaining is knowable, so the drain
     * does not sit out its deadline waiting for bytes the client already finished sending. */
    int      request_body_complete;    /* framing reached its terminal state */
    int      drain_framing_usable;     /* DRAINING: the chunked decoder can still report completion;
                                        * cleared on decoder error, after which only the bounds apply */
    uint64_t request_body_received;    /* body bytes accounted so far */
    uint64_t drain_deadline_ms;   /* DRAINING: absolute deadline (0 = not draining) */
    size_t   drain_budget;        /* DRAINING: remaining byte budget */
    size_t   reject_drain_max_bytes;   /* per-conn copy of the server config */
    uint32_t reject_drain_timeout_ms;
    KlHttp1ChunkedDecoder chunked_dec;

    KlTls *tls;
    int tls_want;               /* KL_EVENT_READ or KL_EVENT_WRITE during handshake */
    char *comp_cipher;          /* Completion-mode TLS ciphertext scratch (driver-owned) */
    size_t comp_cipher_cap;

    KlWsServerConn *ws;

    KlHttp2ServerConn *h2;
    KlHttp2ServerConfig *h2_config;
    KlHttpRouter *router;
    size_t max_body_size;

    struct KlAsyncOp *async_op;
    uint64_t suspend_start_ms;

    KlFileIO *file_io;
    int file_io_phase;
    size_t file_io_len;
    size_t file_io_sent;

    void (*access_log)(const KlHttpRequest *req, int status,
                       size_t body_bytes, double duration_ms,
                       void *user_data);
    void *access_log_data;

    KlSlotLease slot_lease;     /* Admission credit handed off by KlListener at accept (step 6B) */

    struct KlHttpConn *next_free;   /* Free list linkage */
};

/* Pool management (server-internal) -------------------------------------------------------------- */

/* Initialize a pre-allocated connection pool. Returns 0 on success, -1 on failure. */
int     kl_http_conn_pool_init(KlHttpConnPool *pool, int capacity, KlAllocator *alloc);

/* Reserve one admission credit before posting/arming an accept (step 6B). Returns 1 (taken), 0 (none
 * -> listener backpressure), -1 on NULL pool. */
int     kl_http_conn_pool_reserve(KlHttpConnPool *pool);

/* Return one admission credit to the pool (capped at capacity). No-op on NULL pool. */
void    kl_http_conn_pool_return_credit(KlHttpConnPool *pool);

/* Acquire a connection slot from the pool. Returns NULL if full. */
KlHttpConn *kl_http_conn_acquire(KlHttpConnPool *pool, KlSocketHandle fd);

/* Release a connection back to the pool (closes fd). */
void    kl_http_conn_release(KlHttpConnPool *pool, KlHttpConn *c);

/* Free all pool resources. */
void    kl_http_conn_pool_free(KlHttpConnPool *pool);

/* Readiness/completion driver entry points (server-internal) ------------------------------------- */

/* Process TLS handshake on a connection. Returns the new connection state. */
KlHttpConnState kl_http_conn_on_handshake(KlHttpConn *c);

/* Read/parse a PROXY protocol header (readiness path, MSG_PEEK). 1 = done, 0 = need more, -1 = close. */
int kl_http_conn_read_proxy_header(KlHttpConn *c);

/* Ingest a PROXY header from bytes already in read_buf (completion path). Header bytes to consume
 * (0 = not a PROXY header), -1 = malformed/close, -2 = need more. */
int kl_http_conn_ingest_proxy(KlHttpConn *c, size_t len);

/* Process readable data on a connection (parse headers/body, invoke handler). Returns the new state. */
KlHttpConnState kl_http_conn_on_readable(KlHttpConn *c, KlHttpRouter *router);

/* Process a writable event (send response data). Returns the new state. */
KlHttpConnState kl_http_conn_on_writable(KlHttpConn *c);

/* Handle async file I/O completion (called from the server tick loop). Returns the new state. */
KlHttpConnState kl_http_conn_on_file_complete(KlHttpConn *c, kl_ssize_t result, int zero_copy);

/* Model-blind protocol core (shared by readiness + completion) ----------------------------------- */

/* Headers fully parsed in read_buf: null-terminate, run pre-body path, set up the body reader, and
 * dispatch. `leftover`/`leftover_len` is any body-bytes tail in the same buffer. Returns the next state. */
KlHttpConnState kl_http_conn_dispatch_request(KlHttpConn *c, KlHttpRouter *router,
                                     const char *leftover, size_t leftover_len);

/* Body fully consumed: run post-body middleware and the handler. Returns the next state. */
KlHttpConnState kl_http_conn_run_post_body(KlHttpConn *c, KlHttpRouter *router);

/* Feed `nread` freshly-received request-body bytes (in read_buf[0..nread]) to the chunked decoder /
 * body reader. Returns the next state (KL_HTTP_CONN_READING_BODY = need more). */
KlHttpConnState kl_http_conn_ingest_body(KlHttpConn *c, size_t nread);

/* Post-rejection teardown (#278). ONE ownership point for a final response that intentionally
 * terminates a request: writes the response, then chooses between DRAINING (request input may
 * still be unread, so closing now would RST the response away) and CLOSED. Ordinary keep-alive
 * completions never come through here. */
KlHttpConnState kl_http_conn_reject_final(KlHttpConn *c, const char *resp, size_t len);

/* Begin drain teardown for a connection whose final response is ALREADY fully flushed (the
 * send path's counterpart to kl_http_conn_reject_final). On a completion backend the caller must
 * only call this once the write is physically retired. Returns DRAINING or CLOSED. */
KlHttpConnState kl_http_conn_begin_drain(KlHttpConn *c);

/* One bounded, non-blocking drain read. Returns DRAINING to stay, or CLOSED on body consumed,
 * peer EOF, budget exhaustion, deadline expiry or socket error. */
/* Account nread bytes already sitting in read_buf and decide whether to keep draining: the single
 * framing-aware step both axes share (the completion driver calls this from its recv completion). */
KlHttpConnState kl_http_conn_drain_ingest(KlHttpConn *c, size_t nread, uint64_t now_ms);

KlHttpConnState kl_http_conn_drain_step(KlHttpConn *c, uint64_t now_ms);

/* Response fully sent: log access, then keep-alive reset (-> READING) or close (-> CLOSED). */
KlHttpConnState kl_http_conn_send_complete(KlHttpConn *c);

#endif /* KEEL_SRC_HTTP_CONN_INTERNAL_H */
