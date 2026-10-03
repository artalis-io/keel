#include <keel/clock.h>
#include <keel/http2_server.h>
#include <keel/http_connection.h>
#include <keel/http_router.h>
#include <keel/http_body_reader.h>
#include <keel/event.h>
#include <keel/tls.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include "http_internal.h"
#include "socket.h"        /* kl_sock_io_status: would-block classification */
#include "kl_cstr.h"   /* kl_ascii_strn?casecmp: ASCII, locale-free, portable */
#include "http2_internal.h"       /* KlHttp2ServerConn / KlHttp2ServerStream bodies (opaque now) */
#include "platform.h"   /* kl_plat_file_pread */
#include "http_proto_hooks.h"       /* H2 server upgrade seam: registered for the core */

/* ═══════════════════════════════════════════════════════════════════
 * Stream management (static helpers)
 * ═══════════════════════════════════════════════════════════════════ */

static KlHttp2ServerStream *h2_stream_find(KlHttp2ServerConn *h2c,
                                         uint32_t stream_id) {
    for (int i = 0; i < h2c->num_streams; i++) {
        if (h2c->streams[i].stream_id == stream_id)
            return &h2c->streams[i];
    }
    return NULL;
}

static KlHttp2ServerStream *h2_stream_create(KlHttp2ServerConn *h2c,
                                            uint32_t stream_id) {
    if (h2c->num_streams >= h2c->max_streams)
        return NULL;

    KlHttp2ServerStream *s = &h2c->streams[h2c->num_streams];
    memset(s, 0, sizeof(*s));
    s->stream_id = stream_id;
    h2c->num_streams++;
    return s;
}

static void h2_stream_destroy(KlHttp2ServerConn *h2c, KlHttp2ServerStream *stream) {
    if (stream->body_reader) {
        stream->body_reader->destroy(stream->body_reader);
        stream->body_reader = NULL;
    }
    if (stream->hdr_storage) {
        kl_free(h2c->alloc, stream->hdr_storage, stream->hdr_storage_len);
        stream->hdr_storage = NULL;
    }
    if (stream->res.hdr_buf) {
        kl_http_response_free(&stream->res);
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

/* A 500 in place of a response HTTP/2 cannot send as asked (a streaming body, an oversized or
 * unreadable file). A HEAD keeps the headers-only rule: the error text goes out only for other
 * methods. */
static void h2_submit_500(KlHttp2ServerConn *h2c, KlHttp2ServerStream *stream, const char *msg) {
    const char *err_names[] = {"content-type"};
    const char *err_values[] = {"text/plain"};
    int head = stream->res.head_request;
    h2c->session->submit_response(h2c->session, stream->stream_id, 500, err_names, err_values, 1,
                                  head ? NULL : msg, head ? 0 : strlen(msg));
}

static int h2_submit_response(KlHttp2ServerConn *h2c, KlHttp2ServerStream *stream) {
    if (stream->response_submitted) return 0;
    stream->response_submitted = 1;

    KlHttpResponse *res = &stream->res;
    const void *body = NULL;
    size_t body_len = 0;
    char *file_buf = NULL;

    if (res->body_mode == KL_HTTP_BODY_BUFFER) {
        body = res->body;
        body_len = res->body_len;
    } else if (res->body_mode == KL_HTTP_BODY_FILE) {
        if (res->file_size > 16 * 1024 * 1024) {
            h2_submit_500(h2c, stream, "File too large for HTTP/2 response");
            return 0;
        }
        if (res->file_size > 0 && !res->head_request) {     /* HEAD: no body to read */
            size_t fsize = (size_t)res->file_size;
            file_buf = kl_malloc(h2c->alloc, fsize);
            if (!file_buf) {
                h2_submit_500(h2c, stream, "Internal server error");
                return 0;
            }
            kl_ssize_t nr = kl_plat_file_pread(res->file_fd, file_buf, fsize, 0);
            /* All of it, or a 500: the file may have shrunk after the handler sized it, and HTTP/2
             * sends no content-length here, so a short body would look complete to the client. */
            if (nr < 0 || (size_t)nr != fsize) {
                kl_free(h2c->alloc, file_buf, fsize);
                h2_submit_500(h2c, stream, "File shorter than its declared size");
                return 0;
            }
            body = file_buf;
            body_len = fsize;
        }
    } else if (res->body_mode == KL_HTTP_BODY_STREAM) {
        h2_submit_500(h2c, stream, "Streaming responses not supported over HTTP/2");
        return 0;
    }
    if (res->head_request) {            /* HEAD: the headers only, as on HTTP/1.1 */
        body = NULL;
        body_len = 0;
    }

    const char *names[H2_MAX_RESP_HEADERS];
    const char *values[H2_MAX_RESP_HEADERS];
    int num_hdrs = h2_extract_response_headers(
        res->hdr_buf, res->hdr_len, names, values, H2_MAX_RESP_HEADERS);

    const char *filt_names[H2_MAX_RESP_HEADERS];
    const char *filt_values[H2_MAX_RESP_HEADERS];
    int filt_count = 0;
    for (int i = 0; i < num_hdrs; i++) {
        if (kl_ascii_strcasecmp(names[i], "connection") == 0) continue;
        if (kl_ascii_strcasecmp(names[i], "transfer-encoding") == 0) continue;
        if (kl_ascii_strcasecmp(names[i], "keep-alive") == 0) continue;
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

/* Stream 1 of an h2c upgrade: take over what the HTTP/1.1-phase middleware left on the connection's
 * request and response, since it does not run again for the stream. The response headers are its
 * "Name: value\r\n" lines, re-added one by one. 0, or -1 on allocation failure. */
static int h2_carry_upgrade_state(KlHttp2ServerStream *stream, const KlHttpRequest *req1,
                                  const KlHttpResponse *res1) {
    stream->req.ctx = req1->ctx;
    const char *p = res1->hdr_buf, *end = res1->hdr_buf ? res1->hdr_buf + res1->hdr_len : NULL;
    while (p && p < end) {
        const char *eol = p;
        while (eol + 1 < end && !(eol[0] == '\r' && eol[1] == '\n')) eol++;
        if (eol + 1 >= end) break;                    /* no terminated line left */
        const char *colon = p;                        /* (no memchr: freestanding builds) */
        while (colon < eol && *colon != ':') colon++;
        if (colon < eol) {
            const char *v = colon + 1;
            while (v < eol && *v == ' ') v++;
            size_t nl = (size_t)(colon - p), vl = (size_t)(eol - v);
            char *tmp = kl_malloc(stream->res.alloc, nl + vl + 2);
            if (!tmp) return -1;
            memcpy(tmp, p, nl); tmp[nl] = '\0';
            memcpy(tmp + nl + 1, v, vl); tmp[nl + 1 + vl] = '\0';
            int rc = kl_http_response_header(&stream->res, tmp, tmp + nl + 1);
            kl_free(stream->res.alloc, tmp, nl + vl + 2);
            if (rc < 0) return -1;
        }
        p = eol + 2;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * Callback implementations (wired into KlHttp2ServerCallbacks)
 * ═══════════════════════════════════════════════════════════════════ */

static int h2_cb_on_request(void *ud, uint32_t stream_id,
                             const char *method, size_t method_len,
                             const char *path, size_t path_len,
                             const char *authority, size_t authority_len,
                             const char **hdr_names, const char **hdr_values,
                             const size_t *hdr_name_lens,
                             const size_t *hdr_value_lens,
                             int num_headers) {
    KlHttp2ServerConn *h2c = ud;

    KlHttp2ServerStream *stream = h2_stream_create(h2c, stream_id);
    if (!stream) {
        /* Stream table full: refuse this stream, not the connection (a -1 here is fatal to every
         * stream on it). Its DATA, if any, is then ignored as for any finished stream. */
        static const char *const busy_names[] = { "content-type" };
        static const char *const busy_values[] = { "text/plain" };
        static const char busy_body[] = "Service Unavailable";
        if (h2c->session->submit_response(h2c->session, stream_id, 503,
                                          (const char **)busy_names, (const char **)busy_values,
                                          1, busy_body, sizeof(busy_body) - 1) != 0)
            return -1;
        return 0;
    }

    /* Clamp to max headers (vtable may provide unchecked value) */
    if (num_headers > KL_MAX_HEADERS)
        num_headers = KL_MAX_HEADERS;

    /* Converge :authority (HTTP/2) with Host (HTTP/1.1): the shared request model
     * exposes authority via a "host" header, so handlers read it the same way over
     * both protocols. Inject a synthetic host header from :authority unless the
     * peer already sent one (it should not, in HTTP/2). */
    int has_host = 0;
    for (int i = 0; i < num_headers; i++) {
        if (hdr_name_lens[i] == 4 && kl_ascii_strncasecmp(hdr_names[i], "host", 4) == 0) {
            has_host = 1;
            break;
        }
    }
    int inject_host = (authority && authority_len > 0 && !has_host &&
                       num_headers < KL_MAX_HEADERS);

    /* Calculate total header storage needed */
    size_t total = method_len + 1 + path_len + 1;
    if (method_len > SIZE_MAX / 2 || path_len > SIZE_MAX / 2 ||
        authority_len > SIZE_MAX / 2) {
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
    if (inject_host) {
        size_t host_entry = 4 + 1 + authority_len + 1;   /* "host" + authority */
        if (host_entry > SIZE_MAX - total) {
            h2_stream_destroy(h2c, stream);
            return -1;
        }
        total += host_entry;
    }

    stream->hdr_storage = kl_malloc(h2c->alloc, total);
    if (!stream->hdr_storage) {
        h2_stream_destroy(h2c, stream);
        return -1;
    }
    stream->hdr_storage_len = total;

    char *p = stream->hdr_storage;
    KlHttpRequest *req = &stream->req;
    memset(req, 0, sizeof(*req));

    /* Method */
    memcpy(p, method, method_len);
    p[method_len] = '\0';
    req->method = p;
    req->method_len = method_len;
    p += method_len + 1;

    /* Path: split query if present */
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
    int cl_present = 0;
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
            kl_ascii_strncasecmp(req->headers[hdr_count].name, "content-length", 14) == 0) {
            cl_present = 1;
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

    /* Synthetic host header from :authority (converges with HTTP/1.1 Host). */
    if (inject_host && hdr_count < KL_MAX_HEADERS) {
        memcpy(p, "host", 4);
        p[4] = '\0';
        req->headers[hdr_count].name = p;
        req->headers[hdr_count].name_len = 4;
        p += 5;
        memcpy(p, authority, authority_len);
        p[authority_len] = '\0';
        req->headers[hdr_count].value = p;
        req->headers[hdr_count].value_len = authority_len;
        /* host is the last thing written into hdr_storage; no further p advance. */
        hdr_count++;
    }

    req->num_headers = hdr_count;
    req->content_length = content_length;
    req->version_major = 2;
    req->version_minor = 0;
    req->keep_alive = 1;

    /* Route match */
    stream->route_result = kl_http_router_match(h2c->router,
                                            req->method, req->method_len,
                                            req->path, req->path_len,
                                            &stream->route, stream->params,
                                            &stream->num_params);
    memcpy(req->params, stream->params,
           sizeof(KlHttpParam) * (size_t)stream->num_params);
    req->num_params = stream->num_params;

    /* Initialize response */
    if (kl_http_response_init(&stream->res, h2c->alloc) < 0) {
        h2_stream_destroy(h2c, stream);
        return -1;
    }
    stream->res.conn_fd = h2c->conn->stream.fd;
    stream->res.tls = h2c->conn->tls;
    stream->res.ctx = h2c->conn->stream.ctx;
    stream->res.keep_alive = 1;
    stream->res.head_request = (req->method_len == 4 &&
                                 memcmp(req->method, "HEAD", 4) == 0);

    /* The upgrading request's stream 1 does not run the pre-body middleware again: it ran in the
     * HTTP/1.1 phase, on the connection's own request and response. Carry what it left there: the
     * request context (how middleware hands data to the handler) and the response headers it added. */
    if (h2c->upgrading && stream_id == 1 &&
        h2_carry_upgrade_state(stream, &h2c->conn->req, &h2c->conn->res) < 0) {
        h2_stream_destroy(h2c, stream);
        return -1;
    }

    /* Run middleware, except for the upgrading request's stream 1: it ran in its HTTP/1.1 phase. */
    if (!(h2c->upgrading && stream_id == 1) &&
        kl_http_router_run_middleware(h2c->router, req, &stream->res) != 0) {
        int rc = h2_submit_response(h2c, stream);
        if (h2c->session->want_write(h2c->session))
            h2c->session->flush(h2c->session);
        h2_stream_destroy(h2c, stream);
        return rc < 0 ? -1 : 0;
    }

    /* Create body reader if needed. HTTP/2 frames a body by END_STREAM, so content-length is
     * optional: without one, the route's reader is made on the first DATA frame (h2_cb_on_data). A
     * request that ends with its HEADERS then never gets one, and runs its handler as a bodiless
     * HTTP/1.1 request does, rather than meeting a factory that needs a body. */
    int has_body = (req->content_length > 0) ||
                   (!cl_present && stream->route && stream->route->body_reader);
    if (!cl_present && stream->route && stream->route->body_reader)
        stream->reader_lazy = 1;
    else if (has_body && stream->route && stream->route->body_reader) {
        KlHttpBodyReader *br = stream->route->body_reader(
            h2c->alloc, req, stream->route->user_data);
        if (!br) {
            kl_http_response_error(&stream->res, 415, "Unsupported Media Type");
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
    KlHttp2ServerConn *h2c = ud;
    KlHttp2ServerStream *stream = h2_stream_find(h2c, stream_id);
    /* A stream already answered (a rejected request, a refused stream) still receives the client's
     * DATA: ignore it. A -1 is fatal to the whole session, and every other stream on it. */
    if (!stream) return 0;

    if (stream->reader_lazy && !stream->body_reader) {     /* the first DATA of a length-less body */
        stream->reader_lazy = 0;
        KlHttpBodyReader *br = stream->route->body_reader(h2c->alloc, &stream->req,
                                                          stream->route->user_data);
        if (!br) {                                          /* refused on this stream only */
            kl_http_response_error(&stream->res, 415, "Unsupported Media Type");
            int rc = h2_submit_response(h2c, stream);
            if (h2c->session->want_write(h2c->session))
                h2c->session->flush(h2c->session);
            h2_stream_destroy(h2c, stream);
            return rc < 0 ? -1 : 0;
        }
        stream->body_reader = br;
        stream->req.body_reader = br;
    }

    /* Enforce body size limit (mirrors HTTP/1.1 path in http_connection.c). An over-limit body, or
     * a reader that refuses the data, is answered 413 on THIS stream and the stream is dropped; a
     * -1 here would fail the whole session, and every other stream multiplexed on it. Later DATA
     * for the stream finds no stream and is ignored (above). */
    size_t max = h2c->conn->max_body_size;
    int refuse = 0;
    if (max > 0) {
        if (len > max - stream->body_received) refuse = 1;
        else stream->body_received += len;
    }
    if (!refuse && stream->body_reader &&
        stream->body_reader->on_data(stream->body_reader, data, len) < 0)
        refuse = 1;
    if (refuse) {
        if (stream->body_reader) stream->body_reader->on_error(stream->body_reader);
        kl_http_response_error(&stream->res, 413, "Payload Too Large");
        int rc = h2_submit_response(h2c, stream);
        if (h2c->session->want_write(h2c->session))
            h2c->session->flush(h2c->session);
        h2_stream_destroy(h2c, stream);
        return rc < 0 ? -1 : 0;
    }
    return 0;
}

static int h2_cb_on_stream_end(void *ud, uint32_t stream_id) {
    KlHttp2ServerConn *h2c = ud;
    KlHttp2ServerStream *stream = h2_stream_find(h2c, stream_id);
    if (!stream) return 0;                     /* already answered: see h2_cb_on_data */

    stream->body_done = 1;

    if (stream->body_reader)
        stream->body_reader->on_complete(stream->body_reader);

    if (kl_http_router_run_post_middleware(h2c->router, &stream->req,
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
        kl_http_response_error(&stream->res, 405, "Method Not Allowed");
    } else {
        kl_http_response_error(&stream->res, 404, "Not Found");
    }

    int rc = h2_submit_response(h2c, stream);
    if (h2c->session->want_write(h2c->session))
        h2c->session->flush(h2c->session);

    h2_stream_destroy(h2c, stream);
    return rc < 0 ? -1 : 0;
}

static void h2_cb_on_stream_reset(void *ud, uint32_t stream_id,
                                    uint32_t error_code) {
    KlHttp2ServerConn *h2c = ud;
    (void)error_code;

    KlHttp2ServerStream *stream = h2_stream_find(h2c, stream_id);
    if (!stream) return;

    if (stream->body_reader)
        stream->body_reader->on_error(stream->body_reader);

    h2_stream_destroy(h2c, stream);
}

/* Default output writer: write the socket (TLS-aware conn_write). Used on the readiness
 * path and whenever a completion driver has not installed a buffering writer. */
static kl_ssize_t h2_out_conn_write(void *ctx, const void *data, size_t len) {
    KlHttp2ServerConn *h2c = ctx;
    KlHttpConn *c = h2c->conn;
    kl_ssize_t nw = conn_write(c, data, len);
    /* A full send buffer on plaintext is "nothing sent yet", as TLS reports WANT_WRITE: the
     * session keeps the tail and the want_write hook re-arms WRITE. -1 here is fatal to the
     * whole session (every stream), so it is reserved for real errors. */
    if (nw < 0 && !c->tls &&
        kl_sock_io_status(c->stream.ctx ? c->stream.ctx->sockets : NULL) == KL_IO_WOULD_BLOCK)
        return 0;
    return nw;
}

/* The session emits produced frame bytes here; route them through the output seam
 * (default: the socket; a completion driver can install a buffering writer). */
static kl_ssize_t h2_cb_send(void *ud, const void *data, size_t len) {
    KlHttp2ServerConn *h2c = ud;
    return h2c->out_write(h2c->out_ctx, data, len);
}

/* Install a custom output writer (fn != NULL) or restore the default socket writer
 * (fn == NULL). The completion driver brackets a feed with this to capture the produced
 * frames into its own buffer for one ordered overlapped send. See internal.h. */
void kl_http2_server_set_writer(KlHttpConn *c, KlHttp2WriteFn fn, void *ctx) {
    if (!c->h2) return;
    if (fn) {
        c->h2->out_write = fn;
        c->h2->out_ctx = ctx;
    } else {
        c->h2->out_write = h2_out_conn_write;
        c->h2->out_ctx = c->h2;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Connection lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

/* Free an h2 connection object that never became c->h2 (session destroyed too). */
static void h2_conn_abandon(KlHttp2ServerConn *h2c) {
    KlAllocator *alloc = h2c->alloc;
    while (h2c->num_streams > 0)                /* streams a fed leftover created: their storage */
        h2_stream_destroy(h2c, &h2c->streams[h2c->num_streams - 1]);
    if (h2c->session && h2c->session->destroy)
        h2c->session->destroy(h2c->session);
    kl_free(alloc, h2c->streams, sizeof(KlHttp2ServerStream) * (size_t)h2c->max_streams);
    kl_free(alloc, h2c, sizeof(KlHttp2ServerConn));
}

/* Create the h2 connection object and its session, with no data fed. NULL on failure, with
 * everything freed (a session the factory returned is destroyed). */
static KlHttp2ServerConn *h2_conn_open(KlHttpConn *c, KlHttpRouter *router,
                                       KlHttp2ServerConfig *cfg) {
    KlAllocator *alloc = c->stream.alloc;

    KlHttp2ServerConn *h2c = kl_malloc(alloc, sizeof(KlHttp2ServerConn));
    if (!h2c) return NULL;
    memset(h2c, 0, sizeof(*h2c));

    h2c->conn = c;
    h2c->alloc = alloc;
    h2c->router = router;

    h2c->max_streams = cfg->max_concurrent_streams > 0
                       ? cfg->max_concurrent_streams
                       : KL_HTTP2_DEFAULT_MAX_STREAMS;

    if ((size_t)h2c->max_streams > SIZE_MAX / sizeof(KlHttp2ServerStream)) {
        kl_free(alloc, h2c, sizeof(KlHttp2ServerConn));
        return NULL;
    }
    size_t streams_size = sizeof(KlHttp2ServerStream) * (size_t)h2c->max_streams;
    h2c->streams = kl_malloc(alloc, streams_size);
    if (!h2c->streams) {
        kl_free(alloc, h2c, sizeof(KlHttp2ServerConn));
        return NULL;
    }
    memset(h2c->streams, 0, streams_size);

    h2c->callbacks.on_request = h2_cb_on_request;
    h2c->callbacks.on_data = h2_cb_on_data;
    h2c->callbacks.on_stream_end = h2_cb_on_stream_end;
    h2c->callbacks.on_stream_reset = h2_cb_on_stream_reset;
    h2c->callbacks.send = h2_cb_send;
    h2c->out_write = h2_out_conn_write;   /* default output sink; driver may override */
    h2c->out_ctx = h2c;

    h2c->session = cfg->factory(alloc, &h2c->callbacks, h2c);
    if (!h2c->session ||
        !h2c->session->recv || !h2c->session->submit_response ||
        !h2c->session->want_write || !h2c->session->flush ||
        !h2c->session->shutdown || !h2c->session->destroy) {
        h2_conn_abandon(h2c);
        return NULL;
    }
    return h2c;
}

int kl_http2_server_upgrade(KlHttpConn *c, KlHttpRouter *router, KlHttp2ServerConfig *cfg,
                          const char *leftover, size_t leftover_len) {
    KlHttp2ServerConn *h2c = h2_conn_open(c, router, cfg);
    if (!h2c) return KL_HTTP_CONN_CLOSED;

    if (leftover && leftover_len > 0) {
        kl_ssize_t r = h2c->session->recv(h2c->session, leftover, leftover_len);
        if (r < 0) {
            h2_conn_abandon(h2c);
            return KL_HTTP_CONN_CLOSED;
        }
    }

    if (h2c->session->want_write(h2c->session))
        h2c->session->flush(h2c->session);

    c->h2 = h2c;
    c->state = KL_HTTP_CONN_HTTP2;
    return KL_HTTP_CONN_HTTP2;
}

static const char h2c_101_response[] =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Connection: Upgrade\r\n"
    "Upgrade: h2c\r\n"
    "\r\n";

/* Hop-by-hop request headers that do not carry over to the HTTP/2 stream (RFC 7540 8.1.2.2). */
static int h2c_hop_by_hop(const char *name, size_t len) {
    static const char *const hop[] = { "connection", "upgrade", "http2-settings", "keep-alive",
                                       "proxy-connection", "transfer-encoding", "te" };
    for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); i++)
        if (strlen(hop[i]) == len && kl_ascii_strncasecmp(name, hop[i], len) == 0) return 1;
    return 0;
}

/* RFC 7540 3.2: answer the upgrading HTTP/1.1 request on stream 1 of the new HTTP/2 connection.
 * The request (c->req) is still intact: this runs at header time, before any body is read. */
int kl_http2_server_upgrade_from_h1(KlHttpConn *c, KlHttpRouter *router,
                                  KlHttp2ServerConfig *cfg,
                                  const char *leftover, size_t leftover_len) {
    const KlHttpRequest *req = &c->req;

    /* Eligibility, decided before anything goes on the wire, so a decline can still be answered
     * over HTTP/1.1: exactly one HTTP2-Settings header, and no request body (the body would have
     * to be read in full before switching; RFC 9113 lets a server ignore the Upgrade instead). */
    const char *settings = NULL;
    size_t settings_len = 0;
    int n_settings = 0;
    for (int i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == 14 &&
            kl_ascii_strncasecmp(req->headers[i].name, "http2-settings", 14) == 0) {
            settings = req->headers[i].value;
            settings_len = req->headers[i].value_len;
            n_settings++;
        }
    }
    if (n_settings != 1 || req->content_length > 0 || req->chunked || !req->method || !req->path)
        return KL_HTTP2_UPGRADE_DECLINED;
    /* HTTP2-Settings is base64url of whole 6-byte settings (RFC 7540 3.2.1). Check its shape now,
     * while a bad one can still be answered over HTTP/1.1, rather than after the 101. */
    {
        size_t len = settings_len;
        while (len > 0 && settings[len - 1] == '=') len--;   /* padding is tolerated */
        for (size_t i = 0; i < len; i++) {
            char ch = settings[i];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
                return KL_HTTP2_UPGRADE_DECLINED;
        }
        if (len % 4 == 1 || ((len / 4) * 3 + (len % 4 ? len % 4 - 1 : 0)) % 6 != 0)
            return KL_HTTP2_UPGRADE_DECLINED;
    }
    if (req->path_len > SIZE_MAX / 4 || req->query_len > SIZE_MAX / 4)
        return KL_HTTP2_UPGRADE_DECLINED;

    KlHttp2ServerConn *h2c = h2_conn_open(c, router, cfg);
    if (!h2c) return KL_HTTP2_UPGRADE_DECLINED;
    if (!h2c->session->upgrade) {               /* the session cannot take over: stay HTTP/1.1 */
        h2_conn_abandon(h2c);
        return KL_HTTP2_UPGRADE_DECLINED;
    }

    /* Rebuild the request target (the server NUL-terminated the path where its '?' was). */
    size_t target_len = req->path_len + (req->query ? 1 + req->query_len : 0);
    char *target = kl_malloc(h2c->alloc, target_len + 1);
    if (!target) {
        h2_conn_abandon(h2c);
        return KL_HTTP2_UPGRADE_DECLINED;
    }
    memcpy(target, req->path, req->path_len);
    if (req->query) {
        target[req->path_len] = '?';
        memcpy(target + req->path_len + 1, req->query, req->query_len);
    }
    target[target_len] = '\0';

    /* Hand the settings to the session BEFORE the 101: it can still refuse them (a value out of
     * range, more settings than it takes), and a refusal is then a decline answered over HTTP/1.1.
     * The session writes nothing until it is flushed, so nothing reaches the wire before the 101. */
    int is_head = req->method_len == 4 && memcmp(req->method, "HEAD", 4) == 0;
    if (h2c->session->upgrade(h2c->session, settings, settings_len, is_head) != 0) {
        kl_free(h2c->alloc, target, target_len + 1);
        h2_conn_abandon(h2c);
        return KL_HTTP2_UPGRADE_DECLINED;
    }

    if (conn_write_all(c, h2c_101_response, sizeof(h2c_101_response) - 1) < 0) {
        kl_free(h2c->alloc, target, target_len + 1);
        h2_conn_abandon(h2c);
        return KL_HTTP_CONN_CLOSED;
    }

    c->h2 = h2c;
    c->state = KL_HTTP_CONN_HTTP2;

    /* Stream 1 is the upgrading request, through the same path as any HTTP/2 request. */
    const char *names[KL_MAX_HEADERS], *values[KL_MAX_HEADERS];
    size_t name_lens[KL_MAX_HEADERS], value_lens[KL_MAX_HEADERS];
    int nh = 0;
    for (int i = 0; i < req->num_headers && nh < KL_MAX_HEADERS; i++) {
        if (h2c_hop_by_hop(req->headers[i].name, req->headers[i].name_len)) continue;
        names[nh] = req->headers[i].name;
        name_lens[nh] = req->headers[i].name_len;
        values[nh] = req->headers[i].value;
        value_lens[nh] = req->headers[i].value_len;
        nh++;
    }
    h2c->upgrading = 1;                          /* its pre-body middleware already ran */
    int rc = h2_cb_on_request(h2c, 1, req->method, req->method_len, target, target_len,
                              NULL, 0, names, values, name_lens, value_lens, nh);
    kl_free(h2c->alloc, target, target_len + 1);
    if (rc == 0 && h2_stream_find(h2c, 1))      /* not already answered by middleware */
        rc = h2_cb_on_stream_end(h2c, 1);
    h2c->upgrading = 0;
    if (rc < 0) return KL_HTTP_CONN_CLOSED;     /* c->h2 is freed by the connection's cleanup */

    if (leftover && leftover_len > 0 &&
        h2c->session->recv(h2c->session, leftover, leftover_len) < 0)
        return KL_HTTP_CONN_CLOSED;

    if (h2c->session->want_write(h2c->session))
        h2c->session->flush(h2c->session);
    return KL_HTTP_CONN_HTTP2;
}

/* Transport-agnostic h2 core: feed already-received plaintext to the session (parse
 * frames + flush produced output). Shared by the readiness drive below and the
 * completion driver; see internal.h. */
KlHttpConnState kl_http2_server_feed(KlHttpConn *c, const void *data, size_t len) {
    KlHttp2ServerConn *h2c = c->h2;
    if (!h2c || !h2c->session) return KL_HTTP_CONN_CLOSED;

    c->last_active_ms = kl_monotonic_ms();

    kl_ssize_t consumed = h2c->session->recv(h2c->session, data, len);
    if (consumed < 0) return KL_HTTP_CONN_CLOSED;

    if (h2c->session->want_write(h2c->session)) {
        if (h2c->session->flush(h2c->session) < 0)
            return KL_HTTP_CONN_CLOSED;
    }

    /* Session-done close: once the session (if it can report readiness) wants
     * neither read nor write, it has terminated: e.g. it sent a GOAWAY for a
     * protocol violation, or finished a graceful shutdown. Close now rather than
     * lingering until the peer times out. Flushed above, so the GOAWAY is on the
     * wire. want_read is optional (NULL → legacy: stay open until peer closes). */
    if (h2c->session->want_read &&
        !h2c->session->want_read(h2c->session) &&
        !h2c->session->want_write(h2c->session))
        return KL_HTTP_CONN_CLOSED;

    return KL_HTTP_CONN_HTTP2;
}

int kl_http2_server_on_readable(KlHttpConn *c) {
    if (!c->h2 || !c->h2->session) return KL_HTTP_CONN_CLOSED;

    int drains = 0;
read_more:
    ;
    kl_ssize_t nr = conn_read(c, c->stream.read_buf, c->stream.read_cap);
    if (nr == 0 && c->tls)
        return KL_HTTP_CONN_HTTP2;   /* TLS WANT_READ: part of a record arrived; wait for it */
    if (nr <= 0) return KL_HTTP_CONN_CLOSED;

    KlHttpConnState st = kl_http2_server_feed(c, c->stream.read_buf, (size_t)nr);
    if (st != KL_HTTP_CONN_HTTP2) return (int)st;

    if (c->tls && c->tls->pending(c->tls) > 0 && ++drains < 256)
        goto read_more;

    return KL_HTTP_CONN_HTTP2;
}

int kl_http2_server_on_writable(KlHttpConn *c) {
    KlHttp2ServerConn *h2c = c->h2;
    if (!h2c || !h2c->session) return KL_HTTP_CONN_CLOSED;

    if (h2c->session->flush(h2c->session) < 0)
        return KL_HTTP_CONN_CLOSED;

    return KL_HTTP_CONN_HTTP2;
}

void kl_http2_server_drain_shutdown(KlHttpConn *c) {
    KlHttp2ServerConn *h2c = c->h2;
    if (!h2c || !h2c->session || h2c->goaway_sent) return;

    h2c->session->shutdown(h2c->session);
    if (h2c->session->want_write(h2c->session))
        h2c->session->flush(h2c->session);
    h2c->goaway_sent = 1;
}

void kl_http2_server_cleanup(KlHttpConn *c) {
    KlHttp2ServerConn *h2c = c->h2;
    if (!h2c) return;

    while (h2c->num_streams > 0)
        h2_stream_destroy(h2c, &h2c->streams[h2c->num_streams - 1]);

    if (h2c->streams) {
        kl_free(h2c->alloc, h2c->streams,
                sizeof(KlHttp2ServerStream) * (size_t)h2c->max_streams);
    }

    if (h2c->session)
        h2c->session->destroy(h2c->session);

    kl_free(h2c->alloc, h2c, sizeof(KlHttp2ServerConn));
    c->h2 = NULL;
}

/* ── HTTP/2 server upgrade seam registration (http_proto_hooks.h) ─────────────────
 * The shared server core reaches HTTP/2 only through this table; installing it
 * both wires the core and forces server_h2.o out of the static archive. */
/* Readiness WRITE-interest predicate (h2 analogue of ws drain_pending): does the session
 * have queued output? Keeps http_server.c's rearm from peeking KlHttp2ServerConn internals. */
static int kl_http2_server_want_write_hook(const KlHttpConn *c) {
    return c->h2 && c->h2->session && c->h2->session->want_write(c->h2->session);
}

static const KlHttp2ServerHooks kl_http2_server_hooks_table = {
    .upgrade         = kl_http2_server_upgrade,
    .upgrade_from_h1 = kl_http2_server_upgrade_from_h1,
    .on_readable     = kl_http2_server_on_readable,
    .on_writable     = kl_http2_server_on_writable,
    .want_write      = kl_http2_server_want_write_hook,
    .cleanup         = kl_http2_server_cleanup,
    .drain_shutdown  = kl_http2_server_drain_shutdown,
};

/* No GCC constructor auto-installing this. kl_http_server_init() calls every installer
 * explicitly and is the documented mechanism, so the constructor was a second lifecycle
 * mechanism for the same thing: redundant on GCC and unavailable under MSVC. kl_proxy_hooks_install
 * never had one and works the same way, which is what confirmed the explicit path is sufficient. */
void kl_http2_server_hooks_install(void) {
    kl_http2_server_hooks_set(&kl_http2_server_hooks_table);
}

