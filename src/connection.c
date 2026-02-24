#include <keel/connection.h>
#include <keel/body_reader.h>
#include <keel/chunked.h>
#include <keel/event.h>
#include <keel/tls.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include "internal.h"

#define KL_TLS_DRAIN_MAX 256  /* max pending drain iterations per event */

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
    if ((size_t)capacity > SIZE_MAX / sizeof(KlConn)) return -1;
    pool->conns = kl_malloc(alloc, sizeof(KlConn) * (size_t)capacity);
    if (!pool->conns) return -1;

    memset(pool->conns, 0, sizeof(KlConn) * (size_t)capacity);

    /* Build free list */
    pool->free_list = &pool->conns[0];
    for (int i = 0; i < capacity - 1; i++) {
        pool->conns[i].next_free = &pool->conns[i + 1];
        pool->conns[i].fd = -1;
        pool->conns[i].state = KL_CONN_CLOSED;
        pool->conns[i].alloc = alloc;
    }
    pool->conns[capacity - 1].next_free = NULL;
    pool->conns[capacity - 1].fd = -1;
    pool->conns[capacity - 1].state = KL_CONN_CLOSED;
    pool->conns[capacity - 1].alloc = alloc;

    return 0;
}

KlConn *kl_conn_acquire(KlConnPool *pool, int fd) {
    if (!pool->free_list) return NULL;

    KlConn *c = pool->free_list;
    pool->free_list = c->next_free;
    c->next_free = NULL;

    c->fd = fd;
    c->state = KL_CONN_READING;
    c->read_len = 0;
    c->hdr_sent = 0;
    c->route = NULL;
    c->num_params = 0;
    c->route_result = 0;
    c->last_active_ms = kl_monotonic_ms();
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
    c->state = KL_CONN_CLOSED;
    c->route = NULL;
    c->next_free = pool->free_list;
    pool->free_list = c;
}

void kl_conn_pool_free(KlConnPool *pool) {
    if (pool->conns) {
        /* Close any active connections */
        for (int i = 0; i < pool->capacity; i++) {
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

    /* If streaming, the handler already sent everything */
    if (c->res.body_mode == KL_BODY_STREAM) {
        conn_log_access(c);
        c->state = KL_CONN_CLOSED;
        return c->state;
    }

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

KlConnState kl_conn_on_readable(KlConn *c, KlRouter *router) {
    c->last_active_ms = kl_monotonic_ms();

    if (c->state == KL_CONN_READING) {
        int hdr_drains = 0;
read_more_headers: ;
        /* Read available data */
        size_t space = KL_READ_BUF_SIZE - c->read_len;
        if (space == 0) {
            best_effort_conn_write(c, kl_413_response, sizeof(kl_413_response) - 1);
            c->state = KL_CONN_CLOSED;
            return c->state;
        }

        ssize_t nr = conn_read(c, c->read_buf + c->read_len, space);
        if (nr <= 0) {
            c->state = KL_CONN_CLOSED;
            return c->state;
        }
        c->read_len += (size_t)nr;

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
            /*
             * Header pointers (method, path, headers) reference read_buf.
             * Route match and factory creation must happen before any
             * buffer modification.  Leftover body data is parsed in-place
             * from read_buf + consumed, preserving header pointers for
             * handlers that complete in a single read cycle.
             */
            size_t leftover = c->read_len - consumed;

            /* Route match — header pointers are still valid */
            c->route_result = kl_router_match(router,
                                               c->req.method, c->req.method_len,
                                               c->req.path, c->req.path_len,
                                               &c->route, c->params,
                                               &c->num_params);

            /* Run middleware before body reading / handler */
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
                    if (leftover > 0) {
                        int rc = kl_chunked_decode(&c->chunked_dec,
                                    c->read_buf + consumed, leftover,
                                    c->req.body_reader);
                        if (rc < 0) {
                            c->req.body_reader->on_error(c->req.body_reader);
                            c->state = KL_CONN_CLOSED;
                            return c->state;
                        }
                        if (rc == 1) {
                            c->req.body_reader->on_complete(c->req.body_reader);
                            return conn_process(c);
                        }
                    }
                } else if (leftover > 0) {
                    size_t body_consumed;
                    KlParseResult bpr = c->parser->parse(
                        c->parser, &c->req,
                        c->read_buf + consumed, leftover,
                        &body_consumed);
                    (void)body_consumed;

                    if (bpr == KL_PARSE_ERROR) {
                        c->req.body_reader->on_error(c->req.body_reader);
                        c->state = KL_CONN_CLOSED;
                        return c->state;
                    }
                    if (bpr == KL_PARSE_OK) {
                        return conn_process(c);
                    }
                    /* INCOMPLETE — need more body data */
                }

                c->read_len = 0;
                c->body_start_ms = kl_monotonic_ms();
                c->state = KL_CONN_READING_BODY;
                return c->state;

            } else if (!has_body) {
                /* No body — header pointers valid, process immediately */
                return conn_process(c);

            } else {
                /* Body present but no reader — discard body */
                if (c->req.chunked) {
                    kl_chunked_init(&c->chunked_dec);
                    if (leftover > 0) {
                        int rc = kl_chunked_decode(&c->chunked_dec,
                                    c->read_buf + consumed, leftover, NULL);
                        if (rc < 0) {
                            c->state = KL_CONN_CLOSED;
                            return c->state;
                        }
                        if (rc == 1) {
                            return conn_process(c);
                        }
                    }
                } else if (leftover > 0) {
                    size_t body_consumed;
                    KlParseResult bpr = c->parser->parse(
                        c->parser, &c->req,
                        c->read_buf + consumed, leftover,
                        &body_consumed);
                    (void)body_consumed;

                    if (bpr == KL_PARSE_ERROR) {
                        c->state = KL_CONN_CLOSED;
                        return c->state;
                    }
                    if (bpr == KL_PARSE_OK) {
                        return conn_process(c);
                    }
                }

                c->read_len = 0;
                c->body_start_ms = kl_monotonic_ms();
                c->state = KL_CONN_READING_BODY;
                return c->state;
            }
        }

        /* KL_PARSE_OK — request with no body completed in one shot
         * (e.g., GET that completed before HEADERS_OK was seen because
         *  the entire message fit in one parse call) */
        if (pr == KL_PARSE_OK) {
            /* Route match (wasn't done during HEADERS_OK) */
            c->route_result = kl_router_match(router,
                                               c->req.method, c->req.method_len,
                                               c->req.path, c->req.path_len,
                                               &c->route, c->params,
                                               &c->num_params);

            /* Run middleware */
            if (conn_init_response(c) < 0) {
                c->state = KL_CONN_CLOSED;
                return c->state;
            }
            if (kl_router_run_middleware(router, &c->req, &c->res) != 0) {
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

            return conn_process(c);
        }

        c->state = KL_CONN_CLOSED;
        return c->state;

    } else if (c->state == KL_CONN_READING_BODY) {
        int body_drains = 0;
read_more_body: ;
        /* Sliding window: read into start of buffer, parse, repeat */
        ; /* empty statement after label before declaration */
        ssize_t nr = conn_read(c, c->read_buf, KL_READ_BUF_SIZE);
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
                best_effort_conn_write(c, kl_413_response,
                                       sizeof(kl_413_response) - 1);
                c->state = KL_CONN_CLOSED;
                return c->state;
            }
            if (rc == 1) {
                if (c->req.body_reader)
                    c->req.body_reader->on_complete(c->req.body_reader);
                return conn_process(c);
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
            /* Body reader rejected (413) */
            best_effort_conn_write(c, kl_413_response, sizeof(kl_413_response) - 1);
            c->state = KL_CONN_CLOSED;
            return c->state;
        }

        if (pr == KL_PARSE_OK) {
            return conn_process(c);
        }

        /* INCOMPLETE — TLS may buffer multiple records */
        if (c->tls && c->tls->pending(c->tls) > 0
            && ++body_drains < KL_TLS_DRAIN_MAX)
            goto read_more_body;
        return KL_CONN_READING_BODY;
    }

    return c->state;
}

KlConnState kl_conn_on_writable(KlConn *c) {
    c->last_active_ms = kl_monotonic_ms();

    if (c->state != KL_CONN_SENDING) return c->state;

    int r = kl_response_send(&c->res);
    if (r < 0) {
        c->state = KL_CONN_CLOSED;
    } else if (r == 0) {
        /* Done sending — log before keep-alive reset clears request */
        conn_log_access(c);
        if (c->req.keep_alive) {
            /* Clean up body reader before resetting request */
            conn_cleanup_body_reader(c);
            /* Fast reset — reuses header buffer, no alloc/free */
            kl_response_reset(&c->res);
            c->parser->reset(c->parser);
            memset(&c->req, 0, sizeof(c->req));
            c->read_len = 0;
            c->hdr_sent = 0;
            c->route = NULL;
            c->num_params = 0;
            c->route_result = 0;
            c->body_start_ms = 0;
            c->last_active_ms = kl_monotonic_ms();
            c->state = KL_CONN_READING;
        } else {
            c->state = KL_CONN_CLOSED;
        }
    }
    /* r > 0: more to send, stay in SENDING state */

    return c->state;
}
