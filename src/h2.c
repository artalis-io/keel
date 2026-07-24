#include <keel/h2_server.h>
#include <keel/connection.h>
#include <keel/router.h>
#include <keel/body_reader.h>
#include <keel/event.h>
#include <keel/tls.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include "internal.h"
#include "platform.h"   /* kl_plat_file_pread */

/* ═══════════════════════════════════════════════════════════════════
 * Stream management (static helpers)
 * ═══════════════════════════════════════════════════════════════════ */

static KlH2ServerStream *h2_stream_find(KlH2ServerConn *h2c,
                                         uint32_t stream_id) {
    for (int i = 0; i < h2c->num_streams; i++) {
        if (h2c->streams[i].stream_id == stream_id)
            return &h2c->streams[i];
    }
    return NULL;
}

static KlH2ServerStream *h2_stream_create(KlH2ServerConn *h2c,
                                            uint32_t stream_id) {
    if (h2c->num_streams >= h2c->max_streams)
        return NULL;

    KlH2ServerStream *s = &h2c->streams[h2c->num_streams];
    memset(s, 0, sizeof(*s));
    s->stream_id = stream_id;
    h2c->num_streams++;
    return s;
}

static void h2_stream_destroy(KlH2ServerConn *h2c, KlH2ServerStream *stream) {
    if (stream->body_reader) {
        stream->body_reader->destroy(stream->body_reader);
        stream->body_reader = NULL;
    }
    if (stream->hdr_storage) {
        kl_free(h2c->alloc, stream->hdr_storage, stream->hdr_storage_len);
        stream->hdr_storage = NULL;
    }
    if (stream->res.hdr_buf) {
        kl_response_free(&stream->res);
    }

    /* Swap-remove: replace this stream with the last one */
    int idx = (int)(stream - h2c->streams);
    int last = h2c->num_streams - 1;
    if (idx < last) {
        h2c->streams[idx] = h2c->streams[last];
    }
    h2c->num_streams--;
}

/* ═══════════════════════════════════════════════════════════════════
 * Response header extraction
 * ═══════════════════════════════════════════════════════════════════ */

#define H2_MAX_RESP_HEADERS 64

static int h2_extract_response_headers(char *hdr_buf, size_t hdr_len,
                                        const char **names, const char **values,
                                        int max_headers) {
    if (!hdr_buf || hdr_len == 0) return 0;

    const char *p = hdr_buf;
    const char *end = hdr_buf + hdr_len;

    int count = 0;
    while (p < end - 1 && count < max_headers) {
        if (p[0] == '\r' && p[1] == '\n')
            break;

        const char *colon = p;
        while (colon < end && *colon != ':')
            colon++;
        if (colon >= end) break;

        names[count] = p;

        const char *val = colon + 1;
        while (val < end && *val == ' ')
            val++;

        values[count] = val;

        const char *eol = val;
        while (eol < end - 1 && !(eol[0] == '\r' && eol[1] == '\n'))
            eol++;

        hdr_buf[colon - hdr_buf] = '\0';
        hdr_buf[eol - hdr_buf] = '\0';

        count++;
        p = (eol < end - 1) ? eol + 2 : end;
    }

    return count;
}

/* ═══════════════════════════════════════════════════════════════════
 * Response submission
 * ═══════════════════════════════════════════════════════════════════ */

static int h2_submit_response(KlH2ServerConn *h2c, KlH2ServerStream *stream) {
    if (stream->response_submitted) return 0;
    stream->response_submitted = 1;

    KlResponse *res = &stream->res;
    const void *body = NULL;
    size_t body_len = 0;
    char *file_buf = NULL;

    if (res->body_mode == KL_BODY_BUFFER) {
        body = res->body;
        body_len = res->body_len;
    } else if (res->body_mode == KL_BODY_FILE) {
        if (res->file_size > 16 * 1024 * 1024) {
            const char *err_names[] = {"content-type"};
            const char *err_values[] = {"text/plain"};
            const char *err_body = "File too large for HTTP/2 response";
            h2c->session->submit_response(h2c->session, stream->stream_id,
                                           500, err_names, err_values, 1,
                                           err_body, strlen(err_body));
            return 0;
        }
        if (res->file_size > 0) {
            size_t fsize = (size_t)res->file_size;
            file_buf = kl_malloc(h2c->alloc, fsize);
            if (!file_buf) {
                const char *err_names[] = {"content-type"};
                const char *err_values[] = {"text/plain"};
                const char *err_body = "Internal server error";
                h2c->session->submit_response(h2c->session, stream->stream_id,
                                               500, err_names, err_values, 1,
                                               err_body, strlen(err_body));
                return 0;
            }
            ssize_t nr = kl_plat_file_pread(res->file_fd, file_buf, fsize, 0);
            if (nr > 0) {
                body = file_buf;
                body_len = (size_t)nr;
            }
        }
    } else if (res->body_mode == KL_BODY_STREAM) {
        const char *err_names[] = {"content-type"};
        const char *err_values[] = {"text/plain"};
        const char *err_body = "Streaming responses not supported over HTTP/2";
        h2c->session->submit_response(h2c->session, stream->stream_id,
                                       500, err_names, err_values, 1,
                                       err_body, strlen(err_body));
        return 0;
    }

    const char *names[H2_MAX_RESP_HEADERS];
    const char *values[H2_MAX_RESP_HEADERS];
    int num_hdrs = h2_extract_response_headers(
        res->hdr_buf, res->hdr_len, names, values, H2_MAX_RESP_HEADERS);

    const char *filt_names[H2_MAX_RESP_HEADERS];
    const char *filt_values[H2_MAX_RESP_HEADERS];
    int filt_count = 0;
    for (int i = 0; i < num_hdrs; i++) {
        if (strcasecmp(names[i], "connection") == 0) continue;
        if (strcasecmp(names[i], "transfer-encoding") == 0) continue;
        if (strcasecmp(names[i], "keep-alive") == 0) continue;
        filt_names[filt_count] = names[i];
        filt_values[filt_count] = values[i];
        filt_count++;
    }

    int rc = h2c->session->submit_response(h2c->session, stream->stream_id,
                                            res->status, filt_names,
                                            filt_values, filt_count,
                                            body, body_len);

    if (file_buf)
        kl_free(h2c->alloc, file_buf, (size_t)res->file_size);

    return rc;
}

/* ═══════════════════════════════════════════════════════════════════
 * Callback implementations (wired into KlH2ServerCallbacks)
 * ═══════════════════════════════════════════════════════════════════ */

static int h2_cb_on_request(void *ud, uint32_t stream_id,
                             const char *method, size_t method_len,
                             const char *path, size_t path_len,
                             const char *authority, size_t authority_len,
                             const char **hdr_names, const char **hdr_values,
                             const size_t *hdr_name_lens,
                             const size_t *hdr_value_lens,
                             int num_headers) {
    KlH2ServerConn *h2c = ud;
    (void)authority;
    (void)authority_len;

    KlH2ServerStream *stream = h2_stream_create(h2c, stream_id);
    if (!stream) return -1;

    /* Clamp to max headers (vtable may provide unchecked value) */
    if (num_headers > KL_MAX_HEADERS)
        num_headers = KL_MAX_HEADERS;

    /* Calculate total header storage needed */
    size_t total = method_len + 1 + path_len + 1;
    if (method_len > SIZE_MAX / 2 || path_len > SIZE_MAX / 2) {
        h2_stream_destroy(h2c, stream);
        return -1;
    }
    for (int i = 0; i < num_headers; i++) {
        size_t entry = hdr_name_lens[i] + 1 + hdr_value_lens[i] + 1;
        if (hdr_name_lens[i] > SIZE_MAX / 2 || hdr_value_lens[i] > SIZE_MAX / 2 ||
            entry > SIZE_MAX - total) {
            h2_stream_destroy(h2c, stream);
            return -1;
        }
        total += entry;
    }

    stream->hdr_storage = kl_malloc(h2c->alloc, total);
    if (!stream->hdr_storage) {
        h2_stream_destroy(h2c, stream);
        return -1;
    }
    stream->hdr_storage_len = total;

    char *p = stream->hdr_storage;
    KlRequest *req = &stream->req;
    memset(req, 0, sizeof(*req));

    /* Method */
    memcpy(p, method, method_len);
    p[method_len] = '\0';
    req->method = p;
    req->method_len = method_len;
    p += method_len + 1;

    /* Path — split query if present */
    memcpy(p, path, path_len);
    p[path_len] = '\0';
    req->path = p;
    req->path_len = path_len;

    for (size_t i = 0; i < path_len; i++) {
        if (p[i] == '?') {
            req->path_len = i;
            req->query = p + i + 1;
            req->query_len = path_len - i - 1;
            break;
        }
    }
    p += path_len + 1;

    /* Headers */
    int hdr_count = 0;
    size_t content_length = 0;
    for (int i = 0; i < num_headers && hdr_count < KL_MAX_HEADERS; i++) {
        memcpy(p, hdr_names[i], hdr_name_lens[i]);
        p[hdr_name_lens[i]] = '\0';
        req->headers[hdr_count].name = p;
        req->headers[hdr_count].name_len = hdr_name_lens[i];
        p += hdr_name_lens[i] + 1;

        memcpy(p, hdr_values[i], hdr_value_lens[i]);
        p[hdr_value_lens[i]] = '\0';
        req->headers[hdr_count].value = p;
        req->headers[hdr_count].value_len = hdr_value_lens[i];
        p += hdr_value_lens[i] + 1;

        if (hdr_name_lens[i] == 14 &&
            strncasecmp(req->headers[hdr_count].name, "content-length", 14) == 0) {
            content_length = 0;
            int cl_valid = (hdr_value_lens[i] > 0) ? 1 : 0;
            for (size_t j = 0; j < hdr_value_lens[i]; j++) {
                char ch = req->headers[hdr_count].value[j];
                if (ch < '0' || ch > '9') { cl_valid = 0; break; }
                if (content_length > SIZE_MAX / 10) { cl_valid = 0; break; }
                content_length = content_length * 10 + (size_t)(ch - '0');
            }
            if (!cl_valid) {
                h2_stream_destroy(h2c, stream);
                return -1;
            }
        }

        hdr_count++;
    }
    req->num_headers = hdr_count;
    req->content_length = content_length;
    req->version_major = 2;
    req->version_minor = 0;
    req->keep_alive = 1;

    /* Route match */
    stream->route_result = kl_router_match(h2c->router,
                                            req->method, req->method_len,
                                            req->path, req->path_len,
                                            &stream->route, stream->params,
                                            &stream->num_params);
    memcpy(req->params, stream->params,
           sizeof(KlParam) * (size_t)stream->num_params);
    req->num_params = stream->num_params;

    /* Initialize response */
    if (kl_response_init(&stream->res, h2c->alloc) < 0) {
        h2_stream_destroy(h2c, stream);
        return -1;
    }
    stream->res.conn_fd = h2c->conn->fd;
    stream->res.tls = h2c->conn->tls;
    stream->res.ctx = h2c->conn->ctx;
    stream->res.keep_alive = 1;
    stream->res.head_request = (req->method_len == 4 &&
                                 memcmp(req->method, "HEAD", 4) == 0);

    /* Run middleware */
    if (kl_router_run_middleware(h2c->router, req, &stream->res) != 0) {
        int rc = h2_submit_response(h2c, stream);
        if (h2c->session->want_write(h2c->session))
            h2c->session->flush(h2c->session);
        h2_stream_destroy(h2c, stream);
        return rc < 0 ? -1 : 0;
    }

    /* Create body reader if needed */
    int has_body = (req->content_length > 0);
    if (has_body && stream->route && stream->route->body_reader) {
        KlBodyReader *br = stream->route->body_reader(
            h2c->alloc, req, stream->route->user_data);
        if (!br) {
            kl_response_error(&stream->res, 415, "Unsupported Media Type");
            int rc = h2_submit_response(h2c, stream);
            if (h2c->session->want_write(h2c->session))
                h2c->session->flush(h2c->session);
            h2_stream_destroy(h2c, stream);
            return rc < 0 ? -1 : 0;
        }
        stream->body_reader = br;
        req->body_reader = br;
    }

    stream->headers_done = 1;

    if (!has_body) {
        stream->body_done = 1;
    }

    return 0;
}

static int h2_cb_on_data(void *ud, uint32_t stream_id,
                          const char *data, size_t len) {
    KlH2ServerConn *h2c = ud;
    KlH2ServerStream *stream = h2_stream_find(h2c, stream_id);
    if (!stream) return -1;

    /* Enforce body size limit (mirrors HTTP/1.1 path in connection.c) */
    size_t max = h2c->conn->max_body_size;
    if (max > 0) {
        if (len > max - stream->body_received) return -1;
        stream->body_received += len;
    }

    if (stream->body_reader) {
        return stream->body_reader->on_data(stream->body_reader, data, len);
    }

    return 0;
}

static int h2_cb_on_stream_end(void *ud, uint32_t stream_id) {
    KlH2ServerConn *h2c = ud;
    KlH2ServerStream *stream = h2_stream_find(h2c, stream_id);
    if (!stream) return -1;

    stream->body_done = 1;

    if (stream->body_reader)
        stream->body_reader->on_complete(stream->body_reader);

    if (kl_router_run_post_middleware(h2c->router, &stream->req,
                                      &stream->res) != 0) {
        int rc = h2_submit_response(h2c, stream);
        if (h2c->session->want_write(h2c->session))
            h2c->session->flush(h2c->session);
        h2_stream_destroy(h2c, stream);
        return rc < 0 ? -1 : 0;
    }

    if (stream->route_result == 200 && stream->route && stream->route->handler) {
        stream->route->handler(&stream->req, &stream->res,
                                stream->route->user_data);
    } else if (stream->route_result == 405) {
        kl_response_error(&stream->res, 405, "Method Not Allowed");
    } else {
        kl_response_error(&stream->res, 404, "Not Found");
    }

    int rc = h2_submit_response(h2c, stream);
    if (h2c->session->want_write(h2c->session))
        h2c->session->flush(h2c->session);

    h2_stream_destroy(h2c, stream);
    return rc < 0 ? -1 : 0;
}

static void h2_cb_on_stream_reset(void *ud, uint32_t stream_id,
                                    uint32_t error_code) {
    KlH2ServerConn *h2c = ud;
    (void)error_code;

    KlH2ServerStream *stream = h2_stream_find(h2c, stream_id);
    if (!stream) return;

    if (stream->body_reader)
        stream->body_reader->on_error(stream->body_reader);

    h2_stream_destroy(h2c, stream);
}

static ssize_t h2_cb_send(void *ud, const void *data, size_t len) {
    KlH2ServerConn *h2c = ud;
    return conn_write(h2c->conn, data, len);
}

/* ═══════════════════════════════════════════════════════════════════
 * Connection lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

int kl_h2_server_upgrade(KlConn *c, KlRouter *router, KlH2ServerConfig *cfg,
                          const char *leftover, size_t leftover_len) {
    KlAllocator *alloc = c->alloc;

    KlH2ServerConn *h2c = kl_malloc(alloc, sizeof(KlH2ServerConn));
    if (!h2c) return KL_CONN_CLOSED;
    memset(h2c, 0, sizeof(*h2c));

    h2c->conn = c;
    h2c->alloc = alloc;
    h2c->router = router;

    h2c->max_streams = cfg->max_concurrent_streams > 0
                       ? cfg->max_concurrent_streams
                       : KL_H2_DEFAULT_MAX_STREAMS;

    if ((size_t)h2c->max_streams > SIZE_MAX / sizeof(KlH2ServerStream)) {
        kl_free(alloc, h2c, sizeof(KlH2ServerConn));
        return KL_CONN_CLOSED;
    }
    size_t streams_size = sizeof(KlH2ServerStream) * (size_t)h2c->max_streams;
    h2c->streams = kl_malloc(alloc, streams_size);
    if (!h2c->streams) {
        kl_free(alloc, h2c, sizeof(KlH2ServerConn));
        return KL_CONN_CLOSED;
    }
    memset(h2c->streams, 0, streams_size);

    h2c->callbacks.on_request = h2_cb_on_request;
    h2c->callbacks.on_data = h2_cb_on_data;
    h2c->callbacks.on_stream_end = h2_cb_on_stream_end;
    h2c->callbacks.on_stream_reset = h2_cb_on_stream_reset;
    h2c->callbacks.send = h2_cb_send;

    h2c->session = cfg->factory(alloc, &h2c->callbacks, h2c);
    if (!h2c->session) {
        kl_free(alloc, h2c->streams, streams_size);
        kl_free(alloc, h2c, sizeof(KlH2ServerConn));
        return KL_CONN_CLOSED;
    }

    if (!h2c->session->recv || !h2c->session->submit_response ||
        !h2c->session->want_write || !h2c->session->flush ||
        !h2c->session->shutdown || !h2c->session->destroy) {
        if (h2c->session->destroy)
            h2c->session->destroy(h2c->session);
        kl_free(alloc, h2c->streams, streams_size);
        kl_free(alloc, h2c, sizeof(KlH2ServerConn));
        return KL_CONN_CLOSED;
    }

    if (leftover && leftover_len > 0) {
        ssize_t r = h2c->session->recv(h2c->session, leftover, leftover_len);
        if (r < 0) {
            h2c->session->destroy(h2c->session);
            kl_free(alloc, h2c->streams, streams_size);
            kl_free(alloc, h2c, sizeof(KlH2ServerConn));
            return KL_CONN_CLOSED;
        }
    }

    if (h2c->session->want_write(h2c->session))
        h2c->session->flush(h2c->session);

    c->h2 = h2c;
    c->state = KL_CONN_HTTP2;
    return KL_CONN_HTTP2;
}

static const char h2c_101_response[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Connection: Upgrade\r\n"
    "Upgrade: h2c\r\n"
    "\r\n";

int kl_h2_server_upgrade_from_h1(KlConn *c, KlRouter *router,
                                  KlH2ServerConfig *cfg,
                                  const char *leftover, size_t leftover_len) {
    if (conn_write_all(c, h2c_101_response, sizeof(h2c_101_response) - 1) < 0)
        return KL_CONN_CLOSED;

    return kl_h2_server_upgrade(c, router, cfg, leftover, leftover_len);
}

int kl_h2_server_on_readable(KlConn *c) {
    KlH2ServerConn *h2c = c->h2;
    if (!h2c || !h2c->session) return KL_CONN_CLOSED;

    int drains = 0;
read_more:
    ;
    ssize_t nr = conn_read(c, c->read_buf, c->read_cap);
    if (nr <= 0) return KL_CONN_CLOSED;

    c->last_active_ms = kl_monotonic_ms();

    ssize_t consumed = h2c->session->recv(h2c->session, c->read_buf, (size_t)nr);
    if (consumed < 0) return KL_CONN_CLOSED;

    if (h2c->session->want_write(h2c->session)) {
        if (h2c->session->flush(h2c->session) < 0)
            return KL_CONN_CLOSED;
    }

    if (c->tls && c->tls->pending(c->tls) > 0 && ++drains < 256)
        goto read_more;

    return KL_CONN_HTTP2;
}

int kl_h2_server_on_writable(KlConn *c) {
    KlH2ServerConn *h2c = c->h2;
    if (!h2c || !h2c->session) return KL_CONN_CLOSED;

    if (h2c->session->flush(h2c->session) < 0)
        return KL_CONN_CLOSED;

    return KL_CONN_HTTP2;
}

void kl_h2_server_drain_shutdown(KlConn *c) {
    KlH2ServerConn *h2c = c->h2;
    if (!h2c || !h2c->session || h2c->goaway_sent) return;

    h2c->session->shutdown(h2c->session);
    if (h2c->session->want_write(h2c->session))
        h2c->session->flush(h2c->session);
    h2c->goaway_sent = 1;
}

void kl_h2_server_cleanup(KlConn *c) {
    KlH2ServerConn *h2c = c->h2;
    if (!h2c) return;

    while (h2c->num_streams > 0)
        h2_stream_destroy(h2c, &h2c->streams[h2c->num_streams - 1]);

    if (h2c->streams) {
        kl_free(h2c->alloc, h2c->streams,
                sizeof(KlH2ServerStream) * (size_t)h2c->max_streams);
    }

    if (h2c->session)
        h2c->session->destroy(h2c->session);

    kl_free(h2c->alloc, h2c, sizeof(KlH2ServerConn));
    c->h2 = NULL;
}
