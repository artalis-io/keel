#include <keel/clock.h>
#include <keel/http_connection.h>
#include <keel/http_body_reader.h>
#include <keel/http1_chunked.h>
#include <keel/event.h>
#include <keel/tls.h>
#include <keel/websocket_server.h>
#include "../websocket/websocket_server_internal.h"
#include <keel/http2_server.h>
#include <keel/proxy_protocol.h>
#include "http_conn_internal.h"
#include "http_proto_hooks.h"        /* ws/h2 upgrade seam: core never names ws/h2 directly */
#include <assert.h>
#include <string.h>
#include "kl_cstr.h"          /* kl_ascii_strncasecmp: freestanding-safe, locale-free */
/* Would-block / EINTR classified via the kl_sock_io_status seam (KlIoStatus), not
 * raw errno, so this TU carries no errno symbol into the freestanding archive. */
#include <stdint.h>
#include "http_internal.h"
#include "http2_internal.h"
#include "socket.h"

/* Socket provider for this connection's fd, via the raw KlStream seam. NULL (POSIX
 * default) for a standalone pool with no event ctx wired; see kl_http_conn_release. */
static inline const KlSocketProvider *conn_sp(const KlHttpConn *c) {
    return kl_stream_provider(&c->stream);
}

#define KL_TLS_DRAIN_MAX 256  /* max pending drain iterations per event */

/* File I/O phase constants */
#define FILE_IO_IDLE       0
#define FILE_IO_READING    1
#define FILE_IO_WRITING    2
#define FILE_IO_CANCELLING 3

static const char kl_413_response[] =
    "HTTP/1.1 413 Payload Too Large\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char kl_415_response[] =
    "HTTP/1.1 415 Unsupported Media Type\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

/* 431 response now lives as KL_HTTP_431_RESPONSE in http_internal.h (shared with the
 * completion path). */

static const char kl_100_continue[] =
    "HTTP/1.1 100 Continue\r\n"
    "\r\n";

int kl_http_conn_pool_init(KlHttpConnPool *pool, int capacity, KlAllocator *alloc) {
    if (capacity <= 0) return -1;
    pool->alloc = alloc;
    pool->capacity = capacity;
    pool->active_count = 0;
    pool->free_credits = capacity;   /* admission rights: one per slot (credit layer) */
    if ((size_t)capacity > SIZE_MAX / sizeof(KlHttpConn)) return -1;
    pool->conns = kl_malloc(alloc, sizeof(KlHttpConn) * (size_t)capacity);
    if (!pool->conns) return -1;

    memset(pool->conns, 0, sizeof(KlHttpConn) * (size_t)capacity);

    /* Build free list and allocate read buffers */
    pool->free_list = &pool->conns[0];
    for (int i = 0; i < capacity; i++) {
        pool->conns[i].stream.read_buf = kl_malloc(alloc, KL_HTTP_CONN_READ_BUF_SIZE);
        if (!pool->conns[i].stream.read_buf) {
            for (int j = 0; j < i; j++)
                kl_free(alloc, pool->conns[j].stream.read_buf, KL_HTTP_CONN_READ_BUF_SIZE);
            kl_free(alloc, pool->conns, sizeof(KlHttpConn) * (size_t)capacity);
            pool->conns = NULL;
            return -1;
        }
        /* Base-init the embedded raw stream: sets read_buf/read_cap, invalid handle,
         * and clears all facet state. The read_buf was allocated just above; kl_stream_init records
         * it as the stable receive buffer. Arguments are known-valid, but check the result and
         * unwind on failure to keep initialization discipline intact against future changes. */
        char *rb = pool->conns[i].stream.read_buf;
        if (kl_stream_init(&pool->conns[i].stream, rb, KL_HTTP_CONN_READ_BUF_SIZE) < 0) {
            for (int j = 0; j <= i; j++)   /* free through i (rb for slot i was set above) */
                kl_free(alloc, pool->conns[j].stream.read_buf, KL_HTTP_CONN_READ_BUF_SIZE);
            kl_free(alloc, pool->conns, sizeof(KlHttpConn) * (size_t)capacity);
            pool->conns = NULL;
            return -1;
        }
        pool->conns[i].next_free = (i < capacity - 1) ? &pool->conns[i + 1] : NULL;
        pool->conns[i].state = KL_HTTP_CONN_CLOSED;
        pool->conns[i].stream.alloc = alloc;
    }

    return 0;
}

int kl_http_conn_pool_reserve(KlHttpConnPool *pool) {
    if (!pool) return -1;                     /* fail closed on a NULL pool */
    if (pool->free_credits <= 0) return 0;    /* admission full → listener backpressure */
    pool->free_credits--;
    return 1;
}

void kl_http_conn_pool_return_credit(KlHttpConnPool *pool) {
    if (!pool) return;                        /* defensive no-op */
    /* Over-return means broken lease accounting (the free_credits + reserved + active == capacity
     * invariant was violated); catch it in debug/tests instead of silently hiding it. */
    assert(pool->free_credits < pool->capacity);
    if (pool->free_credits < pool->capacity) pool->free_credits++;   /* release-once safety net */
}

KlHttpConn *kl_http_conn_acquire(KlHttpConnPool *pool, KlSocketHandle fd) {
    if (!pool->free_list) return NULL;

    KlHttpConn *c = pool->free_list;
    pool->free_list = c->next_free;
    c->next_free = NULL;
    pool->active_count++;

    c->stream.fd = fd;
    c->state = KL_HTTP_CONN_READING;
    c->stream.read_len = 0;
    c->hdr_sent = 0;
    c->route = NULL;
    c->num_params = 0;
    c->route_result = 0;
    c->last_active_ms = kl_monotonic_ms();
    c->ws = NULL;
    c->h2 = NULL;
    c->async_op = NULL;
    c->suspend_start_ms = 0;
    c->file_io_phase = FILE_IO_IDLE;
    memset(&c->req, 0, sizeof(c->req));

    return c;
}

const KlSockAddr *kl_http_conn_peer_addr(const KlHttpConn *c) {
    return &c->stream.peer_addr;
}

KlHttpResponse *kl_http_conn_response(KlHttpConn *c) {
    return c ? &c->res : NULL;
}

const KlHttpResponse *kl_http_conn_response_const(const KlHttpConn *c) {
    return c ? &c->res : NULL;
}

static void conn_cleanup_body_reader(KlHttpConn *c) {
    if (c->req.body_reader) {
        c->req.body_reader->destroy(c->req.body_reader);
        c->req.body_reader = NULL;
    }
}

void kl_http_conn_release(KlHttpConnPool *pool, KlHttpConn *c) {
    /* WebSocket / HTTP-2 cleanup via the per-protocol upgrade seam (no-op when the
     * module isn't linked, e.g. a freestanding HTTP/1.1 server). */
    const KlWsServerHooks *wsh = kl_ws_server_hooks();
    const KlHttp2ServerHooks *h2h = kl_http2_server_hooks();
    if (wsh && wsh->cleanup) wsh->cleanup(c);
    if (h2h && h2h->cleanup) h2h->cleanup(c);

    /* TLS shutdown before close(fd): best-effort close_notify.
     * Retry on WANT_WRITE to flush the close_notify record.
     * Give up on WANT_READ (can't block for peer's close_notify). */
    if (c->tls && kl_handle_valid(c->stream.fd)) {
        KlTlsResult sr = c->tls->shutdown(c->tls, c->stream.fd);
        for (int i = 0; sr == KL_TLS_WANT_WRITE && i < 4; i++)
            sr = c->tls->shutdown(c->tls, c->stream.fd);
        c->tls->reset(c->tls);
    }
    if (kl_handle_valid(c->stream.fd)) {
        kl_sock_close(conn_sp(c), c->stream.fd);
        c->stream.fd = KL_INVALID_SOCKET;
    }
    conn_cleanup_body_reader(c);
    if (c->parser) {
        c->parser->reset(c->parser);
    }
    if (c->res.hdr_buf) {
        kl_http_response_free(&c->res);
    }
    c->async_op = NULL;
    c->state = KL_HTTP_CONN_CLOSED;
    c->route = NULL;
    c->next_free = pool->free_list;
    pool->free_list = c;
    pool->active_count--;
}

void kl_http_conn_pool_free(KlHttpConnPool *pool) {
    if (pool->conns) {
        /* Close any active connections */
        const KlWsServerHooks *wsh = kl_ws_server_hooks();
        const KlHttp2ServerHooks *h2h = kl_http2_server_hooks();
        for (int i = 0; i < pool->capacity; i++) {
            if (wsh && wsh->cleanup) wsh->cleanup(&pool->conns[i]);
            if (h2h && h2h->cleanup) h2h->cleanup(&pool->conns[i]);
            if (pool->conns[i].req.body_reader) {
                pool->conns[i].req.body_reader->destroy(
                    pool->conns[i].req.body_reader);
                pool->conns[i].req.body_reader = NULL;
            }
            if (pool->conns[i].tls && pool->conns[i].tls->shutdown &&
                kl_handle_valid(pool->conns[i].stream.fd)) {
                KlTlsResult sr = pool->conns[i].tls->shutdown(
                    pool->conns[i].tls, pool->conns[i].stream.fd);
                for (int j = 0; sr == KL_TLS_WANT_WRITE && j < 4; j++)
                    sr = pool->conns[i].tls->shutdown(
                        pool->conns[i].tls, pool->conns[i].stream.fd);
            }
            if (kl_handle_valid(pool->conns[i].stream.fd)) {
                kl_sock_close(conn_sp(&pool->conns[i]), pool->conns[i].stream.fd);
            }
            if (pool->conns[i].parser) {
                pool->conns[i].parser->destroy(pool->conns[i].parser);
            }
            if (pool->conns[i].tls && pool->conns[i].tls->destroy) {
                pool->conns[i].tls->destroy(pool->conns[i].tls);
            }
            if (pool->conns[i].res.hdr_buf) {
                kl_http_response_free(&pool->conns[i].res);
            }
            if (pool->conns[i].stream.read_buf) {
                kl_free(pool->alloc, pool->conns[i].stream.read_buf,
                        pool->conns[i].stream.read_cap);
            }
            if (pool->conns[i].comp_cipher) {   /* interim completion TLS scratch */
                kl_free(pool->alloc, pool->conns[i].comp_cipher,
                        pool->conns[i].comp_cipher_cap);
            }
        }
        kl_free(pool->alloc, pool->conns,
                sizeof(KlHttpConn) * (size_t)pool->capacity);
        pool->conns = NULL;
    }
}

static void conn_log_access(KlHttpConn *c) {
    if (!c->access_log) return;
    size_t bytes = 0;
    if (c->res.body_mode == KL_HTTP_BODY_BUFFER) bytes = c->res.body_len;
    else if (c->res.body_mode == KL_HTTP_BODY_FILE && c->res.file_size > 0)
        bytes = (size_t)c->res.file_size;
    uint64_t now = kl_monotonic_ms();
    double duration = (double)(now - c->request_start_ms);
    c->access_log(&c->req, c->res.status, bytes, duration, c->access_log_data);
}

/*
 * Initialize response for the current request, so middleware can write headers/status before the handler runs.
 */
static int conn_init_response(KlHttpConn *c) {
    c->req._server_ctx = c;
    c->request_start_ms = kl_monotonic_ms();
    if (c->res.hdr_buf) {
        kl_http_response_reset(&c->res);
    } else {
        if (kl_http_response_init(&c->res, c->res.alloc) < 0)
            return -1;
    }
    c->res.conn_fd = c->stream.fd;
    c->res.tls = c->tls;
    c->res.ctx = c->stream.ctx;   /* socket provider for writev/sendfile (ctx->sockets) */
    c->res.keep_alive = c->req.keep_alive;
    c->res.head_request = (c->req.method_len == 4 &&
                           memcmp(c->req.method, "HEAD", 4) == 0);
    return 0;
}

/*
 * Process a fully parsed request: handler → response setup.
 * Response must already be initialized via conn_init_response().
 */
static KlHttpConnState conn_process(KlHttpConn *c) {
    c->state = KL_HTTP_CONN_PROCESSING;

    if (c->route_result == 200 && c->route) {
        c->route->handler(&c->req, &c->res, c->route->user_data);
    } else if (c->route_result == 405) {
        kl_http_response_error(&c->res, 405, "Method Not Allowed");
    } else {
        kl_http_response_error(&c->res, 404, "Not Found");
    }

    /* If handler suspended the connection for async I/O, don't transition */
    if (c->state == KL_HTTP_CONN_SUSPENDED)
        return KL_HTTP_CONN_SUSPENDED;

    /* If streaming, the handler already sent everything, unless drain is pending */
    if (c->res.body_mode == KL_HTTP_BODY_STREAM) {
        if (c->res.drain_enabled && kl_drain_pending(&c->res.drain)) {
            c->state = KL_HTTP_CONN_SENDING;
            return c->state;
        }
        conn_log_access(c);
        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;
    }

    c->state = KL_HTTP_CONN_SENDING;
    return c->state;
}

/*
 * Run post-body middleware, then invoke the handler.
 * Body has already been consumed, so keep_alive is preserved on short-circuit.
 */
static KlHttpConnState conn_run_post_middleware_and_handle(KlHttpConn *c,
                                                       KlHttpRouter *router) {
    if (kl_http_router_run_post_middleware(router, &c->req, &c->res) != 0) {
        /* Body already consumed: keep_alive preserved */
        if (c->res.body_mode == KL_HTTP_BODY_STREAM) {
            conn_log_access(c);
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }
        c->state = KL_HTTP_CONN_SENDING;
        return c->state;
    }
    return conn_process(c);
}

/*
 * Invoke a streaming-handler route's handler. The handler is expected
 * to consume the body incrementally (e.g. via the streaming multipart
 * pull iterator) and may yield mid-stream. Post-body middleware is NOT
 * run for streaming routes (the handler runs before on_complete so a
 * post-body middleware would not see a complete body).
 *
 * Post-handler transition mirrors conn_process. The handler can:
 *   - Complete synchronously (return with response set) → SENDING
 *   - Yield waiting for more body bytes → handler sets state
 *       KL_HTTP_CONN_READING_BODY; the body reader's on_data callback
 *       resumes the handler when more bytes arrive
 *   - Yield for async I/O (db.async, http.fetch, etc.) → handler sets
 *       state KL_HTTP_CONN_SUSPENDED via kl_async_suspend
 *   - Emit a streaming response (SSE / chunked) → drain or close
 */
static KlHttpConnState conn_invoke_streaming_handler(KlHttpConn *c) {
    if (!c->route || !c->route->handler) {
        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;
    }
    c->state = KL_HTTP_CONN_PROCESSING;
    c->route->handler(&c->req, &c->res, c->route->user_data);

    /* Yields keep the conn alive without transitioning to SENDING. */
    if (c->state == KL_HTTP_CONN_SUSPENDED) return KL_HTTP_CONN_SUSPENDED;
    if (c->state == KL_HTTP_CONN_READING_BODY) return KL_HTTP_CONN_READING_BODY;

    /* Streaming response: handler already wrote chunks. */
    if (c->res.body_mode == KL_HTTP_BODY_STREAM) {
        if (c->res.drain_enabled && kl_drain_pending(&c->res.drain)) {
            c->state = KL_HTTP_CONN_SENDING;
            return c->state;
        }
        conn_log_access(c);
        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;
    }

    /* Standard buffered response: send it. */
    c->state = KL_HTTP_CONN_SENDING;
    return c->state;
}

KlHttpConnState kl_http_conn_on_handshake(KlHttpConn *c) {
    if (!c->tls) {
        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;
    }
    /* Route the TLS socket-BIO through the connection's own socket provider, so
     * TLS I/O matches the connection's stack (e.g. lwIP) without per-app config.
     * Idempotent: a pointer store the backend consults on the readiness path. */
    if (c->tls->set_socket_provider)
        c->tls->set_socket_provider(c->tls, conn_sp(c));
    KlTlsResult r = c->tls->handshake(c->tls, c->stream.fd);
    c->last_active_ms = kl_monotonic_ms();
    switch (r) {
        case KL_TLS_OK:
            c->tls_want = 0;
            /* Check ALPN for HTTP/2 negotiation */
            if (c->h2_config && c->tls->alpn_protocol) {
                const char *proto = c->tls->alpn_protocol(c->tls);
                const KlHttp2ServerHooks *h2h = kl_http2_server_hooks();
                if (proto && proto[0] == 'h' && proto[1] == '2' &&
                    proto[2] == '\0' && h2h && h2h->upgrade) {
                    int hr = h2h->upgrade(c, c->router, c->h2_config,
                                          NULL, 0);
                    if (hr == KL_HTTP_CONN_HTTP2) {
                        c->state = KL_HTTP_CONN_HTTP2;
                        return c->state;
                    }
                }
            }
            c->state = KL_HTTP_CONN_READING;
            return c->state;
        case KL_TLS_WANT_READ:
            c->tls_want = KL_EVENT_READ;
            return KL_HTTP_CONN_TLS_HANDSHAKE;
        case KL_TLS_WANT_WRITE:
            c->tls_want = KL_EVENT_WRITE;
            return KL_HTTP_CONN_TLS_HANDSHAKE;
        case KL_TLS_ERROR:
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
    }
    c->state = KL_HTTP_CONN_CLOSED;
    return c->state;
}

int kl_http_conn_read_proxy_header(KlHttpConn *c) {
    uint8_t buf[KL_PROXY_HEADER_MAX];
    ssize_t n;
    do {
        n = kl_stream_recv_peek(&c->stream, buf, sizeof(buf));
    } while (n < 0 && kl_stream_io_status(&c->stream) == KL_IO_INTERRUPTED);
    if (n < 0)
        return (kl_stream_io_status(&c->stream) == KL_IO_WOULD_BLOCK) ? 0 : -1;
    if (n == 0)
        return -1;   /* peer closed before sending a header */

    KlSockAddr peer;
    size_t consumed = 0;
    const KlProxyHooks *ph = kl_proxy_hooks();
    if (!ph || !ph->parse) return 1;   /* PROXY module not linked: treat as no header */
    KlProxyResult r = ph->parse(buf, (size_t)n, &consumed, &peer);

    switch (r) {
        case KL_PROXY_NEED_MORE:
            /* Full peek buffer and still incomplete → malformed / oversized. */
            return ((size_t)n >= sizeof(buf)) ? -1 : 0;
        case KL_PROXY_INVALID:
            return -1;
        case KL_PROXY_NONE:
            return 1;   /* not a PROXY header; leave bytes for TLS/HTTP */
        case KL_PROXY_OK:
            break;
    }

    /* Consume exactly the header bytes (they were only peeked). */
    size_t left = consumed;
    while (left > 0) {
        size_t want = left < sizeof(buf) ? left : sizeof(buf);
        ssize_t rd;
        rd = kl_stream_recv(&c->stream, buf, want);
        if (rd <= 0)
            return -1;
        left -= (size_t)rd;
    }

    if (kl_sockaddr_family(&peer) != KL_AF_UNSPEC) {
        c->stream.peer_addr = peer;
        c->peer_source = KL_HTTP_PEER_PROXY;
    }
    /* LOCAL/UNKNOWN (KL_AF_UNSPEC): keep the socket address. */
    return 1;
}

/* Model-blind PROXY-header ingest for the completion path: parse a PROXY header from bytes
 * already received into read_buf[0..len] (an overlapped recv, not a socket peek; the readiness
 * counterpart above peeks + consumes on the socket). On a real header sets c->stream.peer_addr as
 * KL_HTTP_PEER_PROXY. Returns the number of leading header bytes to consume (0 = not a PROXY header,
 * proceed with all bytes as HTTP/TLS), -1 on malformed/oversized, or -2 if more bytes are needed
 * (the caller posts another recv). */
int kl_http_conn_ingest_proxy(KlHttpConn *c, size_t len) {
    KlSockAddr peer;
    size_t consumed = 0;
    const KlProxyHooks *ph = kl_proxy_hooks();
    if (!ph || !ph->parse) return 0;   /* PROXY module not linked: no header */
    KlProxyResult r = ph->parse((const uint8_t *)c->stream.read_buf, len,
                                &consumed, &peer);
    switch (r) {
        case KL_PROXY_NEED_MORE:
            /* A PROXY header can't exceed KL_PROXY_HEADER_MAX; still incomplete past that is
             * malformed (also bounds the wait against a slow/withholding sender). */
            return (len >= KL_PROXY_HEADER_MAX) ? -1 : -2;
        case KL_PROXY_INVALID:
            return -1;
        case KL_PROXY_NONE:
            return 0;   /* not a PROXY header: proceed with all buffered bytes */
        case KL_PROXY_OK:
            if (kl_sockaddr_family(&peer) != KL_AF_UNSPEC) {
                c->stream.peer_addr = peer;
                c->peer_source = KL_HTTP_PEER_PROXY;
            }
            return (int)consumed;
    }
    return -1;
}

/*
 * Null-terminate method, path, query, and all header name/value strings
 * in-place in read_buf.  The byte after each string is an HTTP syntax
 * char (\r, :, ?, space) that is never read again post-parse.
 */
static void conn_null_terminate_headers(KlHttpConn *c) {
    c->stream.read_buf[(size_t)(c->req.method - c->stream.read_buf) + c->req.method_len] = '\0';
    c->stream.read_buf[(size_t)(c->req.path - c->stream.read_buf) + c->req.path_len] = '\0';
    if (c->req.query)
        c->stream.read_buf[(size_t)(c->req.query - c->stream.read_buf) + c->req.query_len] = '\0';
    for (int i = 0; i < c->req.num_headers; i++) {
        c->stream.read_buf[(size_t)(c->req.headers[i].name - c->stream.read_buf) + c->req.headers[i].name_len] = '\0';
        c->stream.read_buf[(size_t)(c->req.headers[i].value - c->stream.read_buf) + c->req.headers[i].value_len] = '\0';
    }
}

/*
 * Unified request dispatch: route match, middleware, WS/H2 upgrade, body handling.
 * Called from both HEADERS_OK (has_leftover=true with leftover data) and
 * PARSE_OK (has_leftover=false, complete request with no body).
 */
static KlHttpConnState conn_dispatch_request(KlHttpConn *c, KlHttpRouter *router,
                                          const char *leftover_buf,
                                          size_t leftover_len) {
    /* Null-terminate the parsed request fields in read_buf so handlers get valid
     * C strings. Done here in the shared core (not at the call sites) so the
     * readiness and completion paths can't drift; the completion driver reaches
     * this via kl_http_conn_dispatch_request. Idempotent + confined to the header
     * region (before any leftover body at read_buf + consumed). */
    conn_null_terminate_headers(c);

    /* Route match */
    c->route_result = kl_http_router_match(router,
                                       c->req.method, c->req.method_len,
                                       c->req.path, c->req.path_len,
                                       &c->route, c->params,
                                       &c->num_params);

    /* Expose route params on request for handlers */
    memcpy(c->req.params, c->params,
           sizeof(KlHttpParam) * (size_t)c->num_params);
    c->req.num_params = c->num_params;

    /* Init response and run pre-body middleware */
    if (conn_init_response(c) < 0) {
        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;
    }
    if (kl_http_router_run_middleware(router, &c->req, &c->res) != 0) {
        /* Middleware short-circuited: body may be unread */
        c->req.keep_alive = 0;
        c->res.keep_alive = 0;
        if (c->res.body_mode == KL_HTTP_BODY_STREAM) {
            conn_log_access(c);
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }
        c->state = KL_HTTP_CONN_SENDING;
        return c->state;
    }

    /* WebSocket upgrade: branch before body reading */
    const KlWsServerHooks *wsh = kl_ws_server_hooks();
    if (c->route_result == 200 && c->route &&
        c->route->ws_config && wsh && wsh->upgrade) {
        c->state = (KlHttpConnState)wsh->upgrade(
            c, leftover_buf, leftover_len);
        return c->state;
    }

    /* HTTP/2 cleartext upgrade (Upgrade: h2c) */
    if (c->h2_config != NULL) {
        size_t ug_len;
        const char *ug = kl_http_request_header_len(
            &c->req, "Upgrade", &ug_len);
        const KlHttp2ServerHooks *h2h = kl_http2_server_hooks();
        if (ug && ug_len == 3 &&
            kl_ascii_strncasecmp(ug, "h2c", 3) == 0 && h2h && h2h->upgrade_from_h1) {
            c->state = (KlHttpConnState)h2h->upgrade_from_h1(
                c, router, c->h2_config,
                leftover_buf, leftover_len);
            return c->state;
        }
    }

    /* Determine if there's a body to read */
    int has_body = (c->req.content_length > 0 || c->req.chunked);

    if (has_body && c->route && c->route->body_reader) {
        /* Create body reader: factory can inspect headers */
        KlHttpBodyReader *br = c->route->body_reader(
            c->stream.alloc, &c->req, c->route->user_data);
        if (!br) {
            best_effort_conn_write(c, kl_415_response,
                        sizeof(kl_415_response) - 1);
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }
        c->req.body_reader = br;

        /* Send 100 Continue if client expects it */
        size_t expect_len;
        const char *expect = kl_http_request_header_len(
            &c->req, "Expect", &expect_len);
        if (expect && expect_len == 12 &&
            kl_ascii_strncasecmp(expect, "100-continue", 12) == 0) {
            best_effort_conn_write(c, kl_100_continue,
                                   sizeof(kl_100_continue) - 1);
        }

        /* v2.2.0+ ── streaming_async routes: invoke the handler BEFORE
         * feeding any leftover body bytes. The handler is expected to
         * yield on NEED_DATA (empty buffer); the body reader's on_data
         * callback will then resume it for the leftover AND subsequent
         * socket reads, giving the handler a chance to catch parser
         * errors that fire inside on_data, including caps that hit
         * on the very first byte chunk. */
        if (c->route->streaming_handler && c->route->streaming_async) {
            /* streaming_async implies streaming_handler; enforced by
             * kl_http_router_add_streaming_async. Assert here to catch
             * downstream API misuse (direct KlHttpRoute mutation) in
             * debug builds without runtime cost in release. */
            assert(c->route->streaming_handler);
            KlHttpConnState s = conn_invoke_streaming_handler(c);
            if (s != KL_HTTP_CONN_READING_BODY) {
                /* Handler completed synchronously (sent a response,
                 * suspended for async I/O, or emitted a streaming
                 * response) without parking on the body reader. The
                 * leftover bytes, and any subsequent body bytes still
                 * in the kernel buffer, are now stranded: nothing in
                 * the conn state machine will drain them. Force
                 * keep-alive off so the connection closes after the
                 * response sends, instead of bleeding stale body bytes
                 * into a re-used connection's next request. Mirror of
                 * the post-handler force-off at the READING_BODY rc<0
                 * and SENDING/CLOSED branches below. */
                c->req.keep_alive = 0;
                c->res.keep_alive = 0;
                return s;
            }
            /* Handler parked. Fall through to leftover processing
             * which will resume it via the body reader's on_data. */
        }

        /* Parse leftover body data in-place (no memmove) */
        if (c->req.chunked) {
            kl_http1_chunked_init(&c->chunked_dec);
            if (leftover_len > 0) {
                int rc = kl_http1_chunked_decode(&c->chunked_dec,
                            leftover_buf, leftover_len,
                            c->req.body_reader);
                if (rc < 0) {
                    c->req.body_reader->on_error(c->req.body_reader);
                    /* v2.1.2/v2.2.0: honor streaming handler's
                     * response if the on_error chain resumed it
                     * (see READING_BODY paths below for the twin). */
                    if (c->route->streaming_handler &&
                        (c->state == KL_HTTP_CONN_SENDING ||
                         c->state == KL_HTTP_CONN_CLOSED)) {
                        c->req.keep_alive = 0;
                        c->res.keep_alive = 0;
                        return c->state;
                    }
                    c->state = KL_HTTP_CONN_CLOSED;
                    return c->state;
                }
                if (rc == 1) {
                    c->req.body_reader->on_complete(c->req.body_reader);
                    if (c->route->streaming_handler) {
                        /* For streaming_async: handler was invoked
                         * above and may have completed during the
                         * on_data + on_complete chain; return its
                         * state. Otherwise: invoke now (legacy). */
                        if (c->route->streaming_async)
                            return c->state;
                        return conn_invoke_streaming_handler(c);
                    }
                    return conn_run_post_middleware_and_handle(c, router);
                }
            }
        } else if (leftover_len > 0) {
            size_t body_consumed;
            KlHttp1ParseResult bpr = c->parser->parse(
                c->parser, &c->req,
                leftover_buf, leftover_len,
                &body_consumed);
            (void)body_consumed;

            if (bpr == KL_HTTP1_PARSE_ERROR) {
                c->req.body_reader->on_error(c->req.body_reader);
                /* v2.1.2/v2.2.0: same handler-state-honoring as the
                 * chunked path. */
                if (c->route->streaming_handler &&
                    (c->state == KL_HTTP_CONN_SENDING ||
                     c->state == KL_HTTP_CONN_CLOSED)) {
                    c->req.keep_alive = 0;
                    c->res.keep_alive = 0;
                    return c->state;
                }
                c->state = KL_HTTP_CONN_CLOSED;
                return c->state;
            }
            if (bpr == KL_HTTP1_PARSE_OK) {
                if (c->route->streaming_handler) {
                    if (c->route->streaming_async)
                        return c->state;
                    return conn_invoke_streaming_handler(c);
                }
                return conn_run_post_middleware_and_handle(c, router);
            }
            /* INCOMPLETE: need more body data */
        }

        /* Legacy streaming-handler (not async): invoke the handler NOW
         * (after any leftover has been fed via on_data). The handler
         * will consume what's available and yield on NEED_DATA, leaving
         * the conn in KL_HTTP_CONN_READING_BODY so the event loop continues
         * pumping on_data. Async routes already invoked above. */
        if (c->route->streaming_handler && !c->route->streaming_async) {
            KlHttpConnState s = conn_invoke_streaming_handler(c);
            if (s != KL_HTTP_CONN_READING_BODY)
                return s;
        }

        c->stream.read_len = 0;
        c->body_start_ms = kl_monotonic_ms();
        c->state = KL_HTTP_CONN_READING_BODY;
        return c->state;

    } else if (!has_body) {
        /* No body: header pointers valid, process immediately.
         * Streaming routes still run the handler (with req->body_reader
         * == NULL); they're expected to detect and handle gracefully.
         * Skipping the streaming branch here would silently bypass the
         * handler for CL:0 streaming requests, degenerate but real. */
        if (c->route && c->route->streaming_handler)
            return conn_invoke_streaming_handler(c);
        return conn_run_post_middleware_and_handle(c, router);

    } else {
        /* Body present but no reader: discard body */

        /* Content-Length early reject */
        if (!c->req.chunked &&
            c->max_body_size > 0 &&
            c->req.content_length > c->max_body_size) {
            best_effort_conn_write(c, kl_413_response,
                                   sizeof(kl_413_response) - 1);
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }

        if (c->req.chunked) {
            kl_http1_chunked_init(&c->chunked_dec);
            if (leftover_len > 0) {
                int rc = kl_http1_chunked_decode(&c->chunked_dec,
                            leftover_buf, leftover_len, NULL);
                if (rc < 0) {
                    c->state = KL_HTTP_CONN_CLOSED;
                    return c->state;
                }
                if (c->max_body_size > 0 &&
                    c->chunked_dec.total_body > c->max_body_size) {
                    best_effort_conn_write(c, kl_413_response,
                                           sizeof(kl_413_response) - 1);
                    c->state = KL_HTTP_CONN_CLOSED;
                    return c->state;
                }
                if (rc == 1) {
                    return conn_run_post_middleware_and_handle(c, router);
                }
            }
        } else if (leftover_len > 0) {
            size_t body_consumed;
            KlHttp1ParseResult bpr = c->parser->parse(
                c->parser, &c->req,
                leftover_buf, leftover_len,
                &body_consumed);
            (void)body_consumed;

            if (bpr == KL_HTTP1_PARSE_ERROR) {
                c->state = KL_HTTP_CONN_CLOSED;
                return c->state;
            }
            if (bpr == KL_HTTP1_PARSE_OK) {
                return conn_run_post_middleware_and_handle(c, router);
            }
        }

        c->stream.read_len = 0;
        c->body_start_ms = kl_monotonic_ms();
        c->state = KL_HTTP_CONN_READING_BODY;
        return c->state;
    }
}

KlHttpConnState kl_http_conn_on_readable(KlHttpConn *c, KlHttpRouter *router) {
    c->last_active_ms = kl_monotonic_ms();

    if (c->state == KL_HTTP_CONN_READING) {
        int hdr_drains = 0;
read_more_headers: ;
        /* Read available data */
        size_t space = c->stream.read_cap - c->stream.read_len;
        if (space == 0) {
            if (c->stream.read_cap >= c->max_header_size) {
                best_effort_conn_write(c, KL_HTTP_431_RESPONSE,
                                       sizeof(KL_HTTP_431_RESPONSE) - 1);
                c->state = KL_HTTP_CONN_CLOSED;
                return c->state;
            }
            size_t new_cap = c->stream.read_cap * 2;
            if (new_cap > c->max_header_size)
                new_cap = c->max_header_size;
            if (new_cap < c->stream.read_cap || new_cap > SIZE_MAX / 2) {
                best_effort_conn_write(c, KL_HTTP_431_RESPONSE,
                                       sizeof(KL_HTTP_431_RESPONSE) - 1);
                c->state = KL_HTTP_CONN_CLOSED;
                return c->state;
            }
            char *nb = kl_realloc(c->stream.alloc, c->stream.read_buf,
                                   c->stream.read_cap, new_cap);
            if (!nb) {
                best_effort_conn_write(c, KL_HTTP_431_RESPONSE,
                                       sizeof(KL_HTTP_431_RESPONSE) - 1);
                c->state = KL_HTTP_CONN_CLOSED;
                return c->state;
            }
            c->stream.read_buf = nb;
            c->stream.read_cap = new_cap;
            c->parser->reset(c->parser);
            memset(&c->req, 0, sizeof(c->req));
            space = c->stream.read_cap - c->stream.read_len;
        }

        ssize_t nr = conn_read(c, c->stream.read_buf + c->stream.read_len, space);
        if (nr <= 0) {
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }
        c->stream.read_len += (size_t)nr;

        /* HTTP/2 connection preface detection (before HTTP/1.1 parser) */
        if (c->h2_config != NULL) {
            /* Size deduced (25 incl. NUL) so newer compilers don't warn
             * under -Wunterminated-string-initialization; the 24-byte
             * preface is compared with explicit length 24 below. */
            static const char h2_preface[] =
                "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
            if (c->stream.read_len >= 24) {
                if (memcmp(c->stream.read_buf, h2_preface, 24) == 0) {
                    /* Hand the session the FULL preface (magic included); the
                     * session's HTTP/2 engine consumes the client connection
                     * preface itself, matching the ALPN-h2 and h2c-Upgrade paths
                     * where the magic arrives on the stream normally. */
                    const KlHttp2ServerHooks *h2hp = kl_http2_server_hooks();
                    if (!h2hp || !h2hp->upgrade)
                        return c->state;   /* HTTP/2 not linked: stay HTTP/1.1 */
                    c->state = (KlHttpConnState)h2hp->upgrade(
                        c, router, c->h2_config,
                        c->stream.read_buf, c->stream.read_len);
                    return c->state;
                }
                /* Not a preface: fall through to HTTP/1.1 parser */
            } else if (memcmp(c->stream.read_buf, h2_preface, c->stream.read_len) == 0) {
                /* Partial preface match: wait for more data */
                return KL_HTTP_CONN_READING;
            }
        }

        /* Try parsing */
        size_t consumed = 0;
        KlHttp1ParseResult pr = c->parser->parse(c->parser, &c->req,
                                             c->stream.read_buf, c->stream.read_len,
                                             &consumed);

        if (pr == KL_HTTP1_PARSE_INCOMPLETE) {
            /* TLS may buffer multiple records; drain before re-arming */
            if (c->tls && c->tls->pending(c->tls) > 0
                && ++hdr_drains < KL_TLS_DRAIN_MAX)
                goto read_more_headers;
            return KL_HTTP_CONN_READING;
        }
        if (pr == KL_HTTP1_PARSE_ERROR) {
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }

        if (pr == KL_HTTP1_PARSE_HEADERS_OK) {
            return conn_dispatch_request(c, router,
                                          c->stream.read_buf + consumed,
                                          c->stream.read_len - consumed);
        }

        if (pr == KL_HTTP1_PARSE_OK) {
            return conn_dispatch_request(c, router, NULL, 0);
        }

        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;

    } else if (c->state == KL_HTTP_CONN_READING_BODY) {
        int body_drains = 0;
read_more_body: ;
        /* Transport: sliding window read into the start of the buffer, then feed
         * the model-blind body core. On TLS, drain buffered records before
         * re-arming (the socket won't signal readable again). */
        ssize_t nr = conn_read(c, c->stream.read_buf, c->stream.read_cap);
        if (nr <= 0) {
            if (c->req.body_reader)
                c->req.body_reader->on_error(c->req.body_reader);
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }
        KlHttpConnState bst = kl_http_conn_ingest_body(c, (size_t)nr);
        if (bst == KL_HTTP_CONN_READING_BODY && c->tls && c->tls->pending(c->tls) > 0
            && ++body_drains < KL_TLS_DRAIN_MAX)
            goto read_more_body;
        return bst;
    }

    return c->state;
}

/* ── Keep-alive reset helper ─────────────────────────────────────── */

static KlHttpConnState conn_keepalive_reset(KlHttpConn *c) {
    conn_cleanup_body_reader(c);
    kl_http_response_reset(&c->res);
    c->parser->reset(c->parser);
    memset(&c->req, 0, sizeof(c->req));
    c->stream.read_len = 0;
    if (c->stream.read_cap > KL_HTTP_CONN_READ_BUF_SIZE) {
        char *shrunk = kl_realloc(c->stream.alloc, c->stream.read_buf,
                                   c->stream.read_cap, KL_HTTP_CONN_READ_BUF_SIZE);
        if (shrunk) {
            c->stream.read_buf = shrunk;
            c->stream.read_cap = KL_HTTP_CONN_READ_BUF_SIZE;
        }
    }
    c->hdr_sent = 0;
    c->route = NULL;
    c->num_params = 0;
    c->route_result = 0;
    c->body_start_ms = 0;
    c->file_io_phase = FILE_IO_IDLE;
    c->last_active_ms = kl_monotonic_ms();
    c->state = KL_HTTP_CONN_READING;
    return c->state;
}

static KlHttpConnState conn_send_complete(KlHttpConn *c) {
    conn_log_access(c);
    if (c->req.keep_alive)
        return conn_keepalive_reset(c);
    c->state = KL_HTTP_CONN_CLOSED;
    return c->state;
}

/* ── Async file I/O state machine ────────────────────────────────── */

static KlHttpConnState conn_file_flush(KlHttpConn *c);

static KlHttpConnState conn_file_submit_read(KlHttpConn *c) {
    if (c->res.file_offset >= c->res.file_size)
        return conn_send_complete(c);

    size_t remaining = (size_t)(c->res.file_size - c->res.file_offset);
    size_t to_read = remaining < c->stream.read_cap ? remaining : c->stream.read_cap;

    /* Try zero-copy splice first (non-TLS only, buf=NULL) */
    if (!c->tls &&
        c->file_io->submit(c->file_io, c->res.file_fd, NULL,
                            to_read, c->res.file_offset,
                            c->stream.fd, c) == 0) {
        c->file_io_phase = FILE_IO_READING;
        c->state = KL_HTTP_CONN_SENDING;
        return c->state;
    }

    /* Fallback: async read into buffer */
    if (c->file_io->submit(c->file_io, c->res.file_fd, c->stream.read_buf,
                            to_read, c->res.file_offset,
                            c->stream.fd, c) < 0) {
        c->file_io_phase = FILE_IO_IDLE;
        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;
    }

    c->file_io_phase = FILE_IO_READING;
    c->state = KL_HTTP_CONN_SENDING;
    return c->state;
}

static KlHttpConnState conn_file_flush(KlHttpConn *c) {
    while (c->file_io_sent < c->file_io_len) {
        ssize_t nw = kl_stream_send(&c->stream, c->stream.read_buf + c->file_io_sent,
                                    c->file_io_len - c->file_io_sent);
        if (nw < 0) {
            KlIoStatus st = kl_stream_io_status(&c->stream);
            if (st == KL_IO_WOULD_BLOCK) {
                c->state = KL_HTTP_CONN_SENDING;
                return c->state;  /* wait for WRITE event */
            }
            if (st == KL_IO_INTERRUPTED) continue;
            c->file_io_phase = FILE_IO_IDLE;
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }
        c->file_io_sent += (size_t)nw;
    }

    /* Buffer fully sent */
    c->res.file_offset += (uint64_t)c->file_io_len;
    c->file_io_phase = FILE_IO_IDLE;
    return conn_file_submit_read(c);  /* next chunk or finish */
}

KlHttpConnState kl_http_conn_on_file_complete(KlHttpConn *c, ssize_t result, int zero_copy) {
    c->last_active_ms = kl_monotonic_ms();

    if (c->file_io_phase == FILE_IO_CANCELLING) {
        c->file_io_phase = FILE_IO_IDLE;
        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;
    }

    if (result <= 0) {
        c->file_io_phase = FILE_IO_IDLE;
        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;
    }

    if (zero_copy) {
        /* Data already sent to socket via splice: advance offset, next chunk */
        c->res.file_offset += (uint64_t)result;
        c->file_io_phase = FILE_IO_IDLE;
        return conn_file_submit_read(c);
    }

    c->file_io_len = (size_t)result;
    c->file_io_sent = 0;
    c->file_io_phase = FILE_IO_WRITING;
    return conn_file_flush(c);
}

/* ── Writable event handler ──────────────────────────────────────── */

KlHttpConnState kl_http_conn_on_writable(KlHttpConn *c) {
    c->last_active_ms = kl_monotonic_ms();

    if (c->state != KL_HTTP_CONN_SENDING) return c->state;

    /* Resume io_uring file write after EAGAIN */
    if (c->file_io_phase == FILE_IO_WRITING)
        return conn_file_flush(c);

    /* Start io_uring async file send (non-TLS only) */
    if (c->res.body_mode == KL_HTTP_BODY_FILE && !c->tls && c->file_io) {
        if (!c->res.headers_sent) {
            int r = kl_http_response_send(&c->res);
            if (r < 0) { c->state = KL_HTTP_CONN_CLOSED; return c->state; }
            if (!c->res.headers_sent) return KL_HTTP_CONN_SENDING;
        }
        if (c->res.head_request)
            return conn_send_complete(c);
        return conn_file_submit_read(c);
    }

    int r = kl_http_response_send(&c->res);
    if (r < 0) {
        c->state = KL_HTTP_CONN_CLOSED;
    } else if (r == 0) {
        return conn_send_complete(c);
    }
    /* r > 0: more to send, stay in SENDING state */

    return c->state;
}

/* ── Model-blind protocol core ─────────────────────────
 * Thin non-static handles onto the static helpers above, so the IOCP completion
 * driver can reuse the exact parse→route→handle→lifecycle core after a completed
 * WSARecv, without the readiness transport wrapper and without http_connection.c
 * learning which event model produced the bytes. kl_http_conn_on_readable is unchanged
 * (still calls the statics directly), so the readiness path stays byte-identical.
 * See http_conn_internal.h / docs/archive/phases/phase8_iocp_design.md §4. */
KlHttpConnState kl_http_conn_dispatch_request(KlHttpConn *c, KlHttpRouter *router,
                                     const char *leftover, size_t leftover_len) {
    return conn_dispatch_request(c, router, leftover, leftover_len);
}

KlHttpConnState kl_http_conn_run_post_body(KlHttpConn *c, KlHttpRouter *router) {
    return conn_run_post_middleware_and_handle(c, router);
}

/* The model-blind request-body core. Feed `nread` freshly-received
 * bytes (already in read_buf[0..nread]) to the chunked decoder / body reader, apply
 * limits, honor streaming-handler transitions. Returns the next state
 * (KL_HTTP_CONN_READING_BODY = need more bytes). kl_http_conn_on_readable calls this inline
 * after a recv (byte-identical); the completion driver calls it after a WSARecv:
 * same core, no event model leaked in. */
KlHttpConnState kl_http_conn_ingest_body(KlHttpConn *c, size_t nread) {
    if (c->req.chunked) {
        int rc = kl_http1_chunked_decode(&c->chunked_dec, c->stream.read_buf, nread,
                                   c->req.body_reader);
        if (rc < 0) {
            if (c->req.body_reader)
                c->req.body_reader->on_error(c->req.body_reader);
            /* Streaming routes: honor a response the on_error chain already set
             * instead of clobbering it with 413. Force keep-alive off (unread body
             * would bleed into the next request). */
            if (c->route && c->route->streaming_handler &&
                (c->state == KL_HTTP_CONN_SENDING || c->state == KL_HTTP_CONN_CLOSED)) {
                c->req.keep_alive = 0;
                c->res.keep_alive = 0;
                return c->state;
            }
            best_effort_conn_write(c, kl_413_response, sizeof(kl_413_response) - 1);
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }
        /* Discard-path chunked limit */
        if (!c->req.body_reader && c->max_body_size > 0 &&
            c->chunked_dec.total_body > c->max_body_size) {
            best_effort_conn_write(c, kl_413_response, sizeof(kl_413_response) - 1);
            c->state = KL_HTTP_CONN_CLOSED;
            return c->state;
        }
        if (rc == 1) {
            if (c->req.body_reader)
                c->req.body_reader->on_complete(c->req.body_reader);
            /* c->router (not a param): READING_BODY is a continuation of the
             * request whose router was saved during the headers phase. */
            if (c->route && c->route->streaming_handler)
                return c->state;
            return conn_run_post_middleware_and_handle(c, c->router);
        }
        /* Mid-stream transitions from streaming handlers: honor the chosen state. */
        if (c->route && c->route->streaming_handler) {
            if (c->state == KL_HTTP_CONN_SUSPENDED)
                return c->state;
            if (c->state == KL_HTTP_CONN_SENDING || c->state == KL_HTTP_CONN_CLOSED) {
                c->req.keep_alive = 0;
                c->res.keep_alive = 0;
                return c->state;
            }
        }
        return KL_HTTP_CONN_READING_BODY;
    }

    /* Content-Length body: parser path */
    size_t consumed = 0;
    KlHttp1ParseResult pr = c->parser->parse(c->parser, &c->req,
                                        c->stream.read_buf, nread, &consumed);
    if (pr == KL_HTTP1_PARSE_ERROR) {
        if (c->req.body_reader)
            c->req.body_reader->on_error(c->req.body_reader);
        if (c->route && c->route->streaming_handler &&
            (c->state == KL_HTTP_CONN_SENDING || c->state == KL_HTTP_CONN_CLOSED)) {
            c->req.keep_alive = 0;
            c->res.keep_alive = 0;
            return c->state;
        }
        best_effort_conn_write(c, kl_413_response, sizeof(kl_413_response) - 1);
        c->state = KL_HTTP_CONN_CLOSED;
        return c->state;
    }
    if (pr == KL_HTTP1_PARSE_OK) {
        if (c->route && c->route->streaming_handler)
            return c->state;
        return conn_run_post_middleware_and_handle(c, c->router);
    }
    if (c->route && c->route->streaming_handler) {
        if (c->state == KL_HTTP_CONN_SUSPENDED)
            return c->state;
        if (c->state == KL_HTTP_CONN_SENDING || c->state == KL_HTTP_CONN_CLOSED) {
            c->req.keep_alive = 0;
            c->res.keep_alive = 0;
            return c->state;
        }
    }
    return KL_HTTP_CONN_READING_BODY;
}

KlHttpConnState kl_http_conn_send_complete(KlHttpConn *c) {
    return conn_send_complete(c);
}
