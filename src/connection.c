#include <keel/connection.h>
#include <keel/body_reader.h>
#include <keel/chunked.h>
#include <keel/event.h>
#include <keel/tls.h>
#include <keel/websocket_server.h>
#include <keel/h2_server.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include "internal.h"

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

static const char kl_431_response[] =
    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char kl_100_continue[] =
    "HTTP/1.1 100 Continue\r\n"
    "\r\n";

uint64_t kl_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int kl_conn_pool_init(KlConnPool *pool, int capacity, KlAllocator *alloc) {
    if (capacity <= 0) return -1;
    pool->alloc = alloc;
    pool->capacity = capacity;
    pool->active_count = 0;
    if ((size_t)capacity > SIZE_MAX / sizeof(KlConn)) return -1;
    pool->conns = kl_malloc(alloc, sizeof(KlConn) * (size_t)capacity);
    if (!pool->conns) return -1;

    memset(pool->conns, 0, sizeof(KlConn) * (size_t)capacity);

    /* Build free list and allocate read buffers */
    pool->free_list = &pool->conns[0];
    for (int i = 0; i < capacity; i++) {
        pool->conns[i].read_buf = kl_malloc(alloc, KL_READ_BUF_SIZE);
        if (!pool->conns[i].read_buf) {
            for (int j = 0; j < i; j++)
                kl_free(alloc, pool->conns[j].read_buf, KL_READ_BUF_SIZE);
            kl_free(alloc, pool->conns, sizeof(KlConn) * (size_t)capacity);
            pool->conns = NULL;
            return -1;
        }
        pool->conns[i].read_cap = KL_READ_BUF_SIZE;
        pool->conns[i].next_free = (i < capacity - 1) ? &pool->conns[i + 1] : NULL;
        pool->conns[i].fd = -1;
        pool->conns[i].state = KL_CONN_CLOSED;
        pool->conns[i].alloc = alloc;
    }

    return 0;
}

KlConn *kl_conn_acquire(KlConnPool *pool, int fd) {
    if (!pool->free_list) return NULL;

    KlConn *c = pool->free_list;
    pool->free_list = c->next_free;
    c->next_free = NULL;
    pool->active_count++;

    c->fd = fd;
    c->state = KL_CONN_READING;
    c->read_len = 0;
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

static void conn_cleanup_body_reader(KlConn *c) {
    if (c->req.body_reader) {
        c->req.body_reader->destroy(c->req.body_reader);
        c->req.body_reader = NULL;
    }
}

void kl_conn_release(KlConnPool *pool, KlConn *c) {
    /* WebSocket cleanup — notify on_close if needed, free state */
    kl_ws_server_cleanup(c);
    /* HTTP/2 cleanup — destroy streams, session, free state */
    kl_h2_server_cleanup(c);

    /* TLS shutdown before close(fd) — best-effort close_notify.
     * Retry on WANT_WRITE to flush the close_notify record.
     * Give up on WANT_READ (can't block for peer's close_notify). */
    if (c->tls && c->fd >= 0) {
        KlTlsResult sr = c->tls->shutdown(c->tls, c->fd);
        for (int i = 0; sr == KL_TLS_WANT_WRITE && i < 4; i++)
            sr = c->tls->shutdown(c->tls, c->fd);
        c->tls->reset(c->tls);
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    conn_cleanup_body_reader(c);
    if (c->parser) {
        c->parser->reset(c->parser);
    }
    if (c->res.hdr_buf) {
        kl_response_free(&c->res);
    }
    c->async_op = NULL;
    c->state = KL_CONN_CLOSED;
    c->route = NULL;
    c->next_free = pool->free_list;
    pool->free_list = c;
    pool->active_count--;
}

void kl_conn_pool_free(KlConnPool *pool) {
    if (pool->conns) {
        /* Close any active connections */
        for (int i = 0; i < pool->capacity; i++) {
            kl_ws_server_cleanup(&pool->conns[i]);
            kl_h2_server_cleanup(&pool->conns[i]);
            if (pool->conns[i].req.body_reader) {
                pool->conns[i].req.body_reader->destroy(
                    pool->conns[i].req.body_reader);
                pool->conns[i].req.body_reader = NULL;
            }
            if (pool->conns[i].tls && pool->conns[i].fd >= 0) {
                KlTlsResult sr = pool->conns[i].tls->shutdown(
                    pool->conns[i].tls, pool->conns[i].fd);
                for (int j = 0; sr == KL_TLS_WANT_WRITE && j < 4; j++)
                    sr = pool->conns[i].tls->shutdown(
                        pool->conns[i].tls, pool->conns[i].fd);
            }
            if (pool->conns[i].fd >= 0) {
                close(pool->conns[i].fd);
            }
            if (pool->conns[i].parser) {
                pool->conns[i].parser->destroy(pool->conns[i].parser);
            }
            if (pool->conns[i].tls) {
                pool->conns[i].tls->destroy(pool->conns[i].tls);
            }
            if (pool->conns[i].res.hdr_buf) {
                kl_response_free(&pool->conns[i].res);
            }
            if (pool->conns[i].read_buf) {
                kl_free(pool->alloc, pool->conns[i].read_buf,
                        pool->conns[i].read_cap);
            }
        }
        kl_free(pool->alloc, pool->conns,
                sizeof(KlConn) * (size_t)pool->capacity);
        pool->conns = NULL;
    }
}

static void conn_log_access(KlConn *c) {
    if (!c->access_log) return;
    size_t bytes = 0;
    if (c->res.body_mode == KL_BODY_BUFFER) bytes = c->res.body_len;
    else if (c->res.body_mode == KL_BODY_FILE && c->res.file_size > 0)
        bytes = (size_t)c->res.file_size;
    uint64_t now = kl_monotonic_ms();
    double duration = (double)(now - c->request_start_ms);
    c->access_log(&c->req, c->res.status, bytes, duration, c->access_log_data);
}

/*
 * Initialize response for the current request.  Extracted from conn_process
 * so middleware can write headers/status before the handler runs.
 */
static int conn_init_response(KlConn *c) {
    c->req._server_ctx = c;
    c->request_start_ms = kl_monotonic_ms();
    if (c->res.hdr_buf) {
        kl_response_reset(&c->res);
    } else {
        if (kl_response_init(&c->res, c->res.alloc) < 0)
            return -1;
    }
    c->res.conn_fd = c->fd;
    c->res.tls = c->tls;
    c->res.keep_alive = c->req.keep_alive;
    c->res.head_request = (c->req.method_len == 4 &&
                           memcmp(c->req.method, "HEAD", 4) == 0);
    return 0;
}

/*
 * Process a fully parsed request: handler → response setup.
 * Response must already be initialized via conn_init_response().
 */
static KlConnState conn_process(KlConn *c) {
    c->state = KL_CONN_PROCESSING;

    if (c->route_result == 200 && c->route) {
        c->route->handler(&c->req, &c->res, c->route->user_data);
    } else if (c->route_result == 405) {
        kl_response_error(&c->res, 405, "Method Not Allowed");
    } else {
        kl_response_error(&c->res, 404, "Not Found");
    }

    /* If handler suspended the connection for async I/O, don't transition */
    if (c->state == KL_CONN_SUSPENDED)
        return KL_CONN_SUSPENDED;

    /* If streaming, the handler already sent everything — unless drain is pending */
    if (c->res.body_mode == KL_BODY_STREAM) {
        if (c->res.drain_enabled && kl_drain_pending(&c->res.drain)) {
            c->state = KL_CONN_SENDING;
            return c->state;
        }
        conn_log_access(c);
        c->state = KL_CONN_CLOSED;
        return c->state;
    }

    c->state = KL_CONN_SENDING;
    return c->state;
}

/*
 * Run post-body middleware, then invoke the handler.
 * Body has already been consumed, so keep_alive is preserved on short-circuit.
 */
static KlConnState conn_run_post_middleware_and_handle(KlConn *c,
                                                       KlRouter *router) {
    if (kl_router_run_post_middleware(router, &c->req, &c->res) != 0) {
        /* Body already consumed — keep_alive preserved */
        if (c->res.body_mode == KL_BODY_STREAM) {
            conn_log_access(c);
            c->state = KL_CONN_CLOSED;
            return c->state;
        }
        c->state = KL_CONN_SENDING;
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
 *       KL_CONN_READING_BODY; the body reader's on_data callback
 *       resumes the handler when more bytes arrive
 *   - Yield for async I/O (db.async, http.fetch, etc.) → handler sets
 *       state KL_CONN_SUSPENDED via kl_async_suspend
 *   - Emit a streaming response (SSE / chunked) → drain or close
 */
static KlConnState conn_invoke_streaming_handler(KlConn *c) {
    if (!c->route || !c->route->handler) {
        c->state = KL_CONN_CLOSED;
        return c->state;
    }
    c->state = KL_CONN_PROCESSING;
    c->route->handler(&c->req, &c->res, c->route->user_data);

    /* Yields keep the conn alive without transitioning to SENDING. */
    if (c->state == KL_CONN_SUSPENDED) return KL_CONN_SUSPENDED;
    if (c->state == KL_CONN_READING_BODY) return KL_CONN_READING_BODY;

    /* Streaming response — handler already wrote chunks. */
    if (c->res.body_mode == KL_BODY_STREAM) {
        if (c->res.drain_enabled && kl_drain_pending(&c->res.drain)) {
            c->state = KL_CONN_SENDING;
            return c->state;
        }
        conn_log_access(c);
        c->state = KL_CONN_CLOSED;
        return c->state;
    }

    /* Standard buffered response — send it. */
    c->state = KL_CONN_SENDING;
    return c->state;
}

KlConnState kl_conn_on_handshake(KlConn *c) {
    if (!c->tls) {
        c->state = KL_CONN_CLOSED;
        return c->state;
    }
    KlTlsResult r = c->tls->handshake(c->tls, c->fd);
    c->last_active_ms = kl_monotonic_ms();
    switch (r) {
        case KL_TLS_OK:
            c->tls_want = 0;
            /* Check ALPN for HTTP/2 negotiation */
            if (c->h2_config && c->tls->alpn_protocol) {
                const char *proto = c->tls->alpn_protocol(c->tls);
                if (proto && proto[0] == 'h' && proto[1] == '2' &&
                    proto[2] == '\0') {
                    int hr = kl_h2_server_upgrade(c, c->router, c->h2_config,
                                           NULL, 0);
                    if (hr == KL_CONN_HTTP2) {
                        c->state = KL_CONN_HTTP2;
                        return c->state;
                    }
                }
            }
            c->state = KL_CONN_READING;
            return c->state;
        case KL_TLS_WANT_READ:
            c->tls_want = KL_EVENT_READ;
            return KL_CONN_TLS_HANDSHAKE;
        case KL_TLS_WANT_WRITE:
            c->tls_want = KL_EVENT_WRITE;
            return KL_CONN_TLS_HANDSHAKE;
        case KL_TLS_ERROR:
            c->state = KL_CONN_CLOSED;
            return c->state;
    }
    c->state = KL_CONN_CLOSED;
    return c->state;
}

/*
 * Null-terminate method, path, query, and all header name/value strings
 * in-place in read_buf.  The byte after each string is an HTTP syntax
 * char (\r, :, ?, space) that is never read again post-parse.
 */
static void conn_null_terminate_headers(KlConn *c) {
    c->read_buf[(size_t)(c->req.method - c->read_buf) + c->req.method_len] = '\0';
    c->read_buf[(size_t)(c->req.path - c->read_buf) + c->req.path_len] = '\0';
    if (c->req.query)
        c->read_buf[(size_t)(c->req.query - c->read_buf) + c->req.query_len] = '\0';
    for (int i = 0; i < c->req.num_headers; i++) {
        c->read_buf[(size_t)(c->req.headers[i].name - c->read_buf) + c->req.headers[i].name_len] = '\0';
        c->read_buf[(size_t)(c->req.headers[i].value - c->read_buf) + c->req.headers[i].value_len] = '\0';
    }
}

/*
 * Unified request dispatch: route match, middleware, WS/H2 upgrade, body handling.
 * Called from both HEADERS_OK (has_leftover=true with leftover data) and
 * PARSE_OK (has_leftover=false, complete request with no body).
 */
static KlConnState conn_dispatch_request(KlConn *c, KlRouter *router,
                                          const char *leftover_buf,
                                          size_t leftover_len) {
    /* Route match */
    c->route_result = kl_router_match(router,
                                       c->req.method, c->req.method_len,
                                       c->req.path, c->req.path_len,
                                       &c->route, c->params,
                                       &c->num_params);

    /* Expose route params on request for handlers */
    memcpy(c->req.params, c->params,
           sizeof(KlParam) * (size_t)c->num_params);
    c->req.num_params = c->num_params;

    /* Init response and run pre-body middleware */
    if (conn_init_response(c) < 0) {
        c->state = KL_CONN_CLOSED;
        return c->state;
    }
    if (kl_router_run_middleware(router, &c->req, &c->res) != 0) {
        /* Middleware short-circuited — body may be unread */
        c->req.keep_alive = 0;
        c->res.keep_alive = 0;
        if (c->res.body_mode == KL_BODY_STREAM) {
            conn_log_access(c);
            c->state = KL_CONN_CLOSED;
            return c->state;
        }
        c->state = KL_CONN_SENDING;
        return c->state;
    }

    /* WebSocket upgrade — branch before body reading */
    if (c->route_result == 200 && c->route &&
        c->route->ws_config) {
        c->state = (KlConnState)kl_ws_server_upgrade(
            c, leftover_buf, leftover_len);
        return c->state;
    }

    /* HTTP/2 cleartext upgrade (Upgrade: h2c) */
    if (c->h2_config != NULL) {
        size_t ug_len;
        const char *ug = kl_request_header_len(
            &c->req, "Upgrade", &ug_len);
        if (ug && ug_len == 3 &&
            strncasecmp(ug, "h2c", 3) == 0) {
            c->state = (KlConnState)kl_h2_server_upgrade_from_h1(
                c, router, c->h2_config,
                leftover_buf, leftover_len);
            return c->state;
        }
    }

    /* Determine if there's a body to read */
    int has_body = (c->req.content_length > 0 || c->req.chunked);

    if (has_body && c->route && c->route->body_reader) {
        /* Create body reader — factory can inspect headers */
        KlBodyReader *br = c->route->body_reader(
            c->alloc, &c->req, c->route->user_data);
        if (!br) {
            best_effort_conn_write(c, kl_415_response,
                        sizeof(kl_415_response) - 1);
            c->state = KL_CONN_CLOSED;
            return c->state;
        }
        c->req.body_reader = br;

        /* Send 100 Continue if client expects it */
        size_t expect_len;
        const char *expect = kl_request_header_len(
            &c->req, "Expect", &expect_len);
        if (expect && expect_len == 12 &&
            strncasecmp(expect, "100-continue", 12) == 0) {
            best_effort_conn_write(c, kl_100_continue,
                                   sizeof(kl_100_continue) - 1);
        }

        /* Parse leftover body data in-place (no memmove) */
        if (c->req.chunked) {
            kl_chunked_init(&c->chunked_dec);
            if (leftover_len > 0) {
                int rc = kl_chunked_decode(&c->chunked_dec,
                            leftover_buf, leftover_len,
                            c->req.body_reader);
                if (rc < 0) {
                    c->req.body_reader->on_error(c->req.body_reader);
                    c->state = KL_CONN_CLOSED;
                    return c->state;
                }
                if (rc == 1) {
                    c->req.body_reader->on_complete(c->req.body_reader);
                    if (c->route->streaming_handler)
                        return conn_invoke_streaming_handler(c);
                    return conn_run_post_middleware_and_handle(c, router);
                }
            }
        } else if (leftover_len > 0) {
            size_t body_consumed;
            KlParseResult bpr = c->parser->parse(
                c->parser, &c->req,
                leftover_buf, leftover_len,
                &body_consumed);
            (void)body_consumed;

            if (bpr == KL_PARSE_ERROR) {
                c->req.body_reader->on_error(c->req.body_reader);
                c->state = KL_CONN_CLOSED;
                return c->state;
            }
            if (bpr == KL_PARSE_OK) {
                if (c->route->streaming_handler)
                    return conn_invoke_streaming_handler(c);
                return conn_run_post_middleware_and_handle(c, router);
            }
            /* INCOMPLETE — need more body data */
        }

        /* Streaming-handler routes: invoke the handler NOW (after any
         * leftover has been fed via on_data). The handler will consume
         * what's available and yield on NEED_DATA, leaving the conn in
         * KL_CONN_READING_BODY so the event loop continues pumping
         * on_data. The body reader's on_data callback resumes the
         * yielded handler. */
        if (c->route->streaming_handler) {
            KlConnState s = conn_invoke_streaming_handler(c);
            if (s != KL_CONN_READING_BODY)
                return s;
            /* Handler yielded waiting for more body — fall through to
             * enter READING_BODY so the event loop continues pumping. */
        }

        c->read_len = 0;
        c->body_start_ms = kl_monotonic_ms();
        c->state = KL_CONN_READING_BODY;
        return c->state;

    } else if (!has_body) {
        /* No body — header pointers valid, process immediately.
         * Streaming routes still run the handler (with req->body_reader
         * == NULL); they're expected to detect and handle gracefully.
         * Skipping the streaming branch here would silently bypass the
         * handler for CL:0 streaming requests — degenerate but real. */
        if (c->route && c->route->streaming_handler)
            return conn_invoke_streaming_handler(c);
        return conn_run_post_middleware_and_handle(c, router);

    } else {
        /* Body present but no reader — discard body */

        /* Content-Length early reject */
        if (!c->req.chunked &&
            c->max_body_size > 0 &&
            c->req.content_length > c->max_body_size) {
            best_effort_conn_write(c, kl_413_response,
                                   sizeof(kl_413_response) - 1);
            c->state = KL_CONN_CLOSED;
            return c->state;
        }

        if (c->req.chunked) {
            kl_chunked_init(&c->chunked_dec);
            if (leftover_len > 0) {
                int rc = kl_chunked_decode(&c->chunked_dec,
                            leftover_buf, leftover_len, NULL);
                if (rc < 0) {
                    c->state = KL_CONN_CLOSED;
                    return c->state;
                }
                if (c->max_body_size > 0 &&
                    c->chunked_dec.total_body > c->max_body_size) {
                    best_effort_conn_write(c, kl_413_response,
                                           sizeof(kl_413_response) - 1);
                    c->state = KL_CONN_CLOSED;
                    return c->state;
                }
                if (rc == 1) {
                    return conn_run_post_middleware_and_handle(c, router);
                }
            }
        } else if (leftover_len > 0) {
            size_t body_consumed;
            KlParseResult bpr = c->parser->parse(
                c->parser, &c->req,
                leftover_buf, leftover_len,
                &body_consumed);
            (void)body_consumed;

            if (bpr == KL_PARSE_ERROR) {
                c->state = KL_CONN_CLOSED;
                return c->state;
            }
            if (bpr == KL_PARSE_OK) {
                return conn_run_post_middleware_and_handle(c, router);
            }
        }

        c->read_len = 0;
        c->body_start_ms = kl_monotonic_ms();
        c->state = KL_CONN_READING_BODY;
        return c->state;
    }
}

KlConnState kl_conn_on_readable(KlConn *c, KlRouter *router) {
    c->last_active_ms = kl_monotonic_ms();

    if (c->state == KL_CONN_READING) {
        int hdr_drains = 0;
read_more_headers: ;
        /* Read available data */
        size_t space = c->read_cap - c->read_len;
        if (space == 0) {
            if (c->read_cap >= c->max_header_size) {
                best_effort_conn_write(c, kl_431_response,
                                       sizeof(kl_431_response) - 1);
                c->state = KL_CONN_CLOSED;
                return c->state;
            }
            size_t new_cap = c->read_cap * 2;
            if (new_cap > c->max_header_size)
                new_cap = c->max_header_size;
            if (new_cap < c->read_cap || new_cap > SIZE_MAX / 2) {
                best_effort_conn_write(c, kl_431_response,
                                       sizeof(kl_431_response) - 1);
                c->state = KL_CONN_CLOSED;
                return c->state;
            }
            char *nb = kl_realloc(c->alloc, c->read_buf,
                                   c->read_cap, new_cap);
            if (!nb) {
                best_effort_conn_write(c, kl_431_response,
                                       sizeof(kl_431_response) - 1);
                c->state = KL_CONN_CLOSED;
                return c->state;
            }
            c->read_buf = nb;
            c->read_cap = new_cap;
            c->parser->reset(c->parser);
            memset(&c->req, 0, sizeof(c->req));
            space = c->read_cap - c->read_len;
        }

        ssize_t nr = conn_read(c, c->read_buf + c->read_len, space);
        if (nr <= 0) {
            c->state = KL_CONN_CLOSED;
            return c->state;
        }
        c->read_len += (size_t)nr;

        /* HTTP/2 connection preface detection (before HTTP/1.1 parser) */
        if (c->h2_config != NULL) {
            static const char h2_preface[24] =
                "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
            if (c->read_len >= 24) {
                if (memcmp(c->read_buf, h2_preface, 24) == 0) {
                    c->state = (KlConnState)kl_h2_server_upgrade(
                        c, router, c->h2_config,
                        c->read_buf + 24, c->read_len - 24);
                    return c->state;
                }
                /* Not a preface — fall through to HTTP/1.1 parser */
            } else if (memcmp(c->read_buf, h2_preface, c->read_len) == 0) {
                /* Partial preface match — wait for more data */
                return KL_CONN_READING;
            }
        }

        /* Try parsing */
        size_t consumed = 0;
        KlParseResult pr = c->parser->parse(c->parser, &c->req,
                                             c->read_buf, c->read_len,
                                             &consumed);

        if (pr == KL_PARSE_INCOMPLETE) {
            /* TLS may buffer multiple records — drain before re-arming */
            if (c->tls && c->tls->pending(c->tls) > 0
                && ++hdr_drains < KL_TLS_DRAIN_MAX)
                goto read_more_headers;
            return KL_CONN_READING;
        }
        if (pr == KL_PARSE_ERROR) {
            c->state = KL_CONN_CLOSED;
            return c->state;
        }

        if (pr == KL_PARSE_HEADERS_OK) {
            conn_null_terminate_headers(c);
            return conn_dispatch_request(c, router,
                                          c->read_buf + consumed,
                                          c->read_len - consumed);
        }

        if (pr == KL_PARSE_OK) {
            return conn_dispatch_request(c, router, NULL, 0);
        }

        c->state = KL_CONN_CLOSED;
        return c->state;

    } else if (c->state == KL_CONN_READING_BODY) {
        int body_drains = 0;
read_more_body: ;
        /* Sliding window: read into start of buffer, parse, repeat */
        ; /* empty statement after label before declaration */
        ssize_t nr = conn_read(c, c->read_buf, c->read_cap);
        if (nr <= 0) {
            if (c->req.body_reader)
                c->req.body_reader->on_error(c->req.body_reader);
            c->state = KL_CONN_CLOSED;
            return c->state;
        }

        if (c->req.chunked) {
            int rc = kl_chunked_decode(&c->chunked_dec,
                        c->read_buf, (size_t)nr, c->req.body_reader);
            if (rc < 0) {
                if (c->req.body_reader)
                    c->req.body_reader->on_error(c->req.body_reader);
                /* Streaming routes: if the body-reader's on_error chain
                 * resumed a handler that caught the parser error and
                 * prepared a response, c->state will already be SENDING
                 * (or CLOSED). Honor that response instead of clobbering
                 * it with the hardcoded 413 — sibling of the v2.1.0/v2.1.1
                 * mid-stream early-exit logic on the success-return path
                 * (just below). Force keep-alive off: the body wasn't
                 * drained, so unread bytes would bleed into the next
                 * request on this conn. */
                if (c->route && c->route->streaming_handler &&
                    (c->state == KL_CONN_SENDING ||
                     c->state == KL_CONN_CLOSED)) {
                    c->req.keep_alive = 0;
                    c->res.keep_alive = 0;
                    return c->state;
                }
                best_effort_conn_write(c, kl_413_response,
                                       sizeof(kl_413_response) - 1);
                c->state = KL_CONN_CLOSED;
                return c->state;
            }
            /* Discard-path chunked limit */
            if (!c->req.body_reader &&
                c->max_body_size > 0 &&
                c->chunked_dec.total_body > c->max_body_size) {
                best_effort_conn_write(c, kl_413_response,
                                       sizeof(kl_413_response) - 1);
                c->state = KL_CONN_CLOSED;
                return c->state;
            }
            if (rc == 1) {
                if (c->req.body_reader)
                    c->req.body_reader->on_complete(c->req.body_reader);
                /* Streaming handlers were invoked at body-reader-setup
                 * time and have been pumped by on_data callbacks during
                 * READING_BODY. on_complete may have triggered a final
                 * resume inside the body reader — c->state is whatever
                 * the handler set. */
                if (c->route && c->route->streaming_handler)
                    return c->state;
                /* c->router is used (not function param) because READING_BODY
                 * is a continuation — the original router was saved on the
                 * connection during the READING headers phase. */
                return conn_run_post_middleware_and_handle(c, c->router);
            }
            /* Mid-stream non-READING_BODY transitions from streaming
             * handlers: honor the handler's chosen state. */
            if (c->route && c->route->streaming_handler) {
                if (c->state == KL_CONN_SUSPENDED) {
                    /* Handler yielded for async I/O — kl_async_suspend
                     * removed the FD. kl_async_complete will resume.
                     * Keep-alive stays as-is; the async op continues
                     * the request normally. */
                    return c->state;
                }
                if (c->state == KL_CONN_SENDING ||
                    c->state == KL_CONN_CLOSED) {
                    /* Handler completed synchronously inside an on_data
                     * resume. Force keep-alive off so unread body bytes
                     * don't bleed into the next request on this conn. */
                    c->req.keep_alive = 0;
                    c->res.keep_alive = 0;
                    return c->state;
                }
            }
            /* TLS may buffer multiple records — drain before re-arming */
            if (c->tls && c->tls->pending(c->tls) > 0
                && ++body_drains < KL_TLS_DRAIN_MAX)
                goto read_more_body;
            return KL_CONN_READING_BODY;
        }

        /* Content-Length body — existing parser path */
        size_t consumed = 0;
        KlParseResult pr = c->parser->parse(c->parser, &c->req,
                                             c->read_buf, (size_t)nr,
                                             &consumed);

        if (pr == KL_PARSE_ERROR) {
            if (c->req.body_reader)
                c->req.body_reader->on_error(c->req.body_reader);
            /* Streaming routes: same handler-state-honoring logic as
             * the chunked path's rc<0 branch above — mirrors v2.1.0/
             * v2.1.1's mid-stream early-exit, just for the error-
             * return path. */
            if (c->route && c->route->streaming_handler &&
                (c->state == KL_CONN_SENDING ||
                 c->state == KL_CONN_CLOSED)) {
                c->req.keep_alive = 0;
                c->res.keep_alive = 0;
                return c->state;
            }
            /* Body reader rejected (413) */
            best_effort_conn_write(c, kl_413_response, sizeof(kl_413_response) - 1);
            c->state = KL_CONN_CLOSED;
            return c->state;
        }

        if (pr == KL_PARSE_OK) {
            /* Same skip-for-streaming logic as the chunked path above. */
            if (c->route && c->route->streaming_handler)
                return c->state;
            /* c->router: see comment above — READING_BODY continuation */
            return conn_run_post_middleware_and_handle(c, c->router);
        }

        /* Mid-stream non-READING_BODY transitions from streaming
         * handlers — same logic as the chunked path above. */
        if (c->route && c->route->streaming_handler) {
            if (c->state == KL_CONN_SUSPENDED) {
                return c->state;
            }
            if (c->state == KL_CONN_SENDING ||
                c->state == KL_CONN_CLOSED) {
                c->req.keep_alive = 0;
                c->res.keep_alive = 0;
                return c->state;
            }
        }
        /* INCOMPLETE — TLS may buffer multiple records */
        if (c->tls && c->tls->pending(c->tls) > 0
            && ++body_drains < KL_TLS_DRAIN_MAX)
            goto read_more_body;
        return KL_CONN_READING_BODY;
    }

    return c->state;
}

/* ── Keep-alive reset helper ─────────────────────────────────────── */

static KlConnState conn_keepalive_reset(KlConn *c) {
    conn_cleanup_body_reader(c);
    kl_response_reset(&c->res);
    c->parser->reset(c->parser);
    memset(&c->req, 0, sizeof(c->req));
    c->read_len = 0;
    if (c->read_cap > KL_READ_BUF_SIZE) {
        char *shrunk = kl_realloc(c->alloc, c->read_buf,
                                   c->read_cap, KL_READ_BUF_SIZE);
        if (shrunk) {
            c->read_buf = shrunk;
            c->read_cap = KL_READ_BUF_SIZE;
        }
    }
    c->hdr_sent = 0;
    c->route = NULL;
    c->num_params = 0;
    c->route_result = 0;
    c->body_start_ms = 0;
    c->file_io_phase = FILE_IO_IDLE;
    c->last_active_ms = kl_monotonic_ms();
    c->state = KL_CONN_READING;
    return c->state;
}

static KlConnState conn_send_complete(KlConn *c) {
    conn_log_access(c);
    if (c->req.keep_alive)
        return conn_keepalive_reset(c);
    c->state = KL_CONN_CLOSED;
    return c->state;
}

/* ── Async file I/O state machine ────────────────────────────────── */

static KlConnState conn_file_flush(KlConn *c);

static KlConnState conn_file_submit_read(KlConn *c) {
    if (c->res.file_offset >= c->res.file_size)
        return conn_send_complete(c);

    size_t remaining = (size_t)(c->res.file_size - c->res.file_offset);
    size_t to_read = remaining < c->read_cap ? remaining : c->read_cap;

    /* Try zero-copy splice first (non-TLS only, buf=NULL) */
    if (!c->tls &&
        c->file_io->submit(c->file_io, c->res.file_fd, NULL,
                            to_read, c->res.file_offset,
                            c->fd, c) == 0) {
        c->file_io_phase = FILE_IO_READING;
        c->state = KL_CONN_SENDING;
        return c->state;
    }

    /* Fallback: async read into buffer */
    if (c->file_io->submit(c->file_io, c->res.file_fd, c->read_buf,
                            to_read, c->res.file_offset,
                            c->fd, c) < 0) {
        c->file_io_phase = FILE_IO_IDLE;
        c->state = KL_CONN_CLOSED;
        return c->state;
    }

    c->file_io_phase = FILE_IO_READING;
    c->state = KL_CONN_SENDING;
    return c->state;
}

static KlConnState conn_file_flush(KlConn *c) {
    while (c->file_io_sent < c->file_io_len) {
        ssize_t nw = write(c->fd, c->read_buf + c->file_io_sent,
                           c->file_io_len - c->file_io_sent);
        if (nw < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                c->state = KL_CONN_SENDING;
                return c->state;  /* wait for WRITE event */
            }
            if (errno == EINTR) continue;
            c->file_io_phase = FILE_IO_IDLE;
            c->state = KL_CONN_CLOSED;
            return c->state;
        }
        c->file_io_sent += (size_t)nw;
    }

    /* Buffer fully sent */
    c->res.file_offset += (off_t)c->file_io_len;
    c->file_io_phase = FILE_IO_IDLE;
    return conn_file_submit_read(c);  /* next chunk or finish */
}

KlConnState kl_conn_on_file_complete(KlConn *c, ssize_t result, int zero_copy) {
    c->last_active_ms = kl_monotonic_ms();

    if (c->file_io_phase == FILE_IO_CANCELLING) {
        c->file_io_phase = FILE_IO_IDLE;
        c->state = KL_CONN_CLOSED;
        return c->state;
    }

    if (result <= 0) {
        c->file_io_phase = FILE_IO_IDLE;
        c->state = KL_CONN_CLOSED;
        return c->state;
    }

    if (zero_copy) {
        /* Data already sent to socket via splice — advance offset, next chunk */
        c->res.file_offset += (off_t)result;
        c->file_io_phase = FILE_IO_IDLE;
        return conn_file_submit_read(c);
    }

    c->file_io_len = (size_t)result;
    c->file_io_sent = 0;
    c->file_io_phase = FILE_IO_WRITING;
    return conn_file_flush(c);
}

/* ── Writable event handler ──────────────────────────────────────── */

KlConnState kl_conn_on_writable(KlConn *c) {
    c->last_active_ms = kl_monotonic_ms();

    if (c->state != KL_CONN_SENDING) return c->state;

    /* Resume io_uring file write after EAGAIN */
    if (c->file_io_phase == FILE_IO_WRITING)
        return conn_file_flush(c);

    /* Start io_uring async file send (non-TLS only) */
    if (c->res.body_mode == KL_BODY_FILE && !c->tls && c->file_io) {
        if (!c->res.headers_sent) {
            int r = kl_response_send(&c->res);
            if (r < 0) { c->state = KL_CONN_CLOSED; return c->state; }
            if (!c->res.headers_sent) return KL_CONN_SENDING;
        }
        if (c->res.head_request)
            return conn_send_complete(c);
        return conn_file_submit_read(c);
    }

    int r = kl_response_send(&c->res);
    if (r < 0) {
        c->state = KL_CONN_CLOSED;
    } else if (r == 0) {
        return conn_send_complete(c);
    }
    /* r > 0: more to send, stay in SENDING state */

    return c->state;
}
