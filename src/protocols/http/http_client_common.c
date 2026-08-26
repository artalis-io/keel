/*
 * http_client_common.c: HTTP/1.1 client shared helpers (sync + async)
 *
 * This TU holds the surface shared by the blocking
 * sync client (http_client_sync.c) and the event-driven async client
 * (http_client_async.c): the CRLF injection guard, the plain/TLS I/O abstraction,
 * heap request formatting, response header helpers, response decompression
 * (buffered + streaming wrapper), and kl_http_client_response_free.
 *
 * These must not pull the blocking path: no poll()/read()/write()/errno here.
 * All allocation through KlAllocator. No Hull dependencies.
 */

#include <keel/http_client.h>
#include <keel/decompress.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>

#include "socket.h"     /* seam: kl_sock_* + KlSockAddr (no direct sockaddr) */
#include "http_client_internal.h"
#include "kl_cstr.h"    /* locale-free append builders + ASCII case compare */

/* ── CRLF injection guard ────────────────────────────────────────── */

int kl_http_client_has_crlf(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' || s[i] == '\n')
            return 1;
    }
    return 0;
}

/* ── I/O abstraction (plain or TLS) ──────────────────────────────── */

kl_ssize_t kl_http_client_io_write(const KlSocketProvider *p, KlSocketHandle fd, KlTls *tls,
                              const void *buf, size_t len)
{
    if (tls)
        return tls->write(tls, fd, buf, len);
    return kl_sock_send(p, fd, buf, len);
}

kl_ssize_t kl_http_client_io_read(const KlSocketProvider *p, KlSocketHandle fd, KlTls *tls,
                             void *buf, size_t len)
{
    if (tls)
        return tls->read(tls, fd, buf, len);
    return kl_sock_recv(p, fd, buf, len);
}

/* ── Build request into heap buffer ──────────────────────────────── */

char *kl_http_client_build_request(KlAllocator *alloc,
                              const char *method, const KlUrl *url,
                              const KlHttpClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              size_t *out_len, int keep_alive,
                              const char *absolute_url)
{
    if (kl_http_client_has_crlf(method, strlen(method)))
        return NULL;
    if (url->path_len > INT_MAX || url->host_len > INT_MAX)
        return NULL;

    const char *target;
    int target_len;
    if (absolute_url) {
        target = absolute_url;
        target_len = (int)strlen(absolute_url);
    } else if (url->path_len > 0) {
        target = url->path;
        target_len = (int)url->path_len;
    } else {
        target = "/";
        target_len = 1;
    }

    char buf[KL_HTTP_CLIENT_REQ_BUF_SIZE];
    size_t off = 0;
    if (kl_buf_append(buf, sizeof(buf), &off, method) != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, " ", 1) != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, target, (size_t)target_len) != 0 ||
        kl_buf_append(buf, sizeof(buf), &off, " HTTP/1.1\r\nHost: ") != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, url->host, url->host_len) != 0 ||
        kl_buf_append(buf, sizeof(buf), &off, "\r\n") != 0)
        return NULL;

    for (int i = 0; i < num_headers; i++) {
        if (kl_http_client_has_crlf(headers[i].name, strlen(headers[i].name)) ||
            kl_http_client_has_crlf(headers[i].value, strlen(headers[i].value)))
            return NULL;
        if (kl_buf_append(buf, sizeof(buf), &off, headers[i].name) != 0 ||
            kl_buf_append_n(buf, sizeof(buf), &off, ": ", 2) != 0 ||
            kl_buf_append(buf, sizeof(buf), &off, headers[i].value) != 0 ||
            kl_buf_append(buf, sizeof(buf), &off, "\r\n") != 0)
            return NULL;
    }

    if (body && body_len > 0) {
        if (kl_buf_append(buf, sizeof(buf), &off, "Content-Length: ") != 0 ||
            kl_buf_append_u64(buf, sizeof(buf), &off, body_len) != 0 ||
            kl_buf_append(buf, sizeof(buf), &off, "\r\n") != 0)
            return NULL;
    }

    if (kl_buf_append(buf, sizeof(buf), &off, "Connection: ") != 0 ||
        kl_buf_append(buf, sizeof(buf), &off,
                      keep_alive ? "keep-alive" : "close") != 0 ||
        kl_buf_append(buf, sizeof(buf), &off, "\r\n\r\n") != 0)
        return NULL;

    if (body_len > SIZE_MAX - (size_t)off)
        return NULL;
    size_t total = (size_t)off + body_len;
    char *req = kl_malloc(alloc, total);
    if (!req)
        return NULL;

    memcpy(req, buf, (size_t)off);
    if (body && body_len > 0)
        memcpy(req + off, body, body_len);

    *out_len = total;
    return req;
}

/* ── Build headers-only request into heap buffer (chunked TE) ────── */

char *kl_http_client_build_request_headers_only(KlAllocator *alloc,
                                           const char *method, const KlUrl *url,
                                           const KlHttpClientHeader *headers,
                                           int num_headers, size_t *out_len,
                                           int keep_alive,
                                           const char *absolute_url)
{
    if (kl_http_client_has_crlf(method, strlen(method)))
        return NULL;
    if (url->path_len > INT_MAX || url->host_len > INT_MAX)
        return NULL;

    const char *target;
    int target_len;
    if (absolute_url) {
        target = absolute_url;
        target_len = (int)strlen(absolute_url);
    } else if (url->path_len > 0) {
        target = url->path;
        target_len = (int)url->path_len;
    } else {
        target = "/";
        target_len = 1;
    }

    char buf[KL_HTTP_CLIENT_REQ_BUF_SIZE];
    size_t off = 0;
    if (kl_buf_append(buf, sizeof(buf), &off, method) != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, " ", 1) != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, target, (size_t)target_len) != 0 ||
        kl_buf_append(buf, sizeof(buf), &off, " HTTP/1.1\r\nHost: ") != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, url->host, url->host_len) != 0 ||
        kl_buf_append(buf, sizeof(buf), &off, "\r\n") != 0)
        return NULL;

    for (int i = 0; i < num_headers; i++) {
        if (kl_http_client_has_crlf(headers[i].name, strlen(headers[i].name)) ||
            kl_http_client_has_crlf(headers[i].value, strlen(headers[i].value)))
            return NULL;
        if (kl_buf_append(buf, sizeof(buf), &off, headers[i].name) != 0 ||
            kl_buf_append_n(buf, sizeof(buf), &off, ": ", 2) != 0 ||
            kl_buf_append(buf, sizeof(buf), &off, headers[i].value) != 0 ||
            kl_buf_append(buf, sizeof(buf), &off, "\r\n") != 0)
            return NULL;
    }

    if (kl_buf_append(buf, sizeof(buf), &off,
                      "Transfer-Encoding: chunked\r\nConnection: ") != 0 ||
        kl_buf_append(buf, sizeof(buf), &off,
                      keep_alive ? "keep-alive" : "close") != 0 ||
        kl_buf_append(buf, sizeof(buf), &off, "\r\n\r\n") != 0)
        return NULL;

    char *req = kl_malloc(alloc, (size_t)off);
    if (!req)
        return NULL;
    memcpy(req, buf, (size_t)off);
    *out_len = (size_t)off;
    return req;
}

/* ── Response header helpers ──────────────────────────────────────── */

const char *kl_http_client_find_header_value(const KlHttpClientResponse *resp,
                                        const char *name)
{
    for (int i = 0; i < resp->num_headers; i++) {
        if (kl_ascii_strcasecmp(resp->headers[i].name, name) == 0)
            return resp->headers[i].value;
    }
    return NULL;
}

void kl_http_client_remove_header(KlHttpClientResponse *resp, const char *name)
{
    for (int i = 0; i < resp->num_headers; i++) {
        if (kl_ascii_strcasecmp(resp->headers[i].name, name) == 0) {
            /* Free the header strings */
            kl_free(&resp->alloc, (char *)resp->headers[i].name,
                    strlen(resp->headers[i].name) + 1);
            kl_free(&resp->alloc, (char *)resp->headers[i].value,
                    strlen(resp->headers[i].value) + 1);
            /* Shift remaining headers down */
            for (int j = i; j < resp->num_headers - 1; j++)
                resp->headers[j] = resp->headers[j + 1];
            resp->num_headers--;
            return;
        }
    }
}

int kl_http_client_server_wants_close(const KlHttpClientResponse *resp)
{
    for (int i = 0; i < resp->num_headers; i++) {
        if (kl_ascii_strcasecmp(resp->headers[i].name, "Connection") == 0 &&
            kl_ascii_strcasecmp(resp->headers[i].value, "close") == 0)
            return 1;
    }
    return 0;
}

/* ── Response decompression (buffered) ────────────────────────────── */

/**
 * Post-process a buffered response: decompress body if Content-Encoding
 * matches the decompressor's encoding. Replaces body and removes header.
 */
int kl_http_client_decompress_response_body(KlHttpClientResponse *resp,
                                       KlDecompressConfig *dcfg)
{
    if (!dcfg || !dcfg->factory)
        return 0;  /* no decompression configured: not an error */
    if (!resp->body || resp->body_len == 0)
        return 0;

    const char *enc = kl_http_client_find_header_value(resp, "Content-Encoding");
    if (!enc)
        return 0;  /* no encoding: nothing to do */

    /* Create session and check encoding match */
    KlDecompress *decomp = dcfg->factory(dcfg->ctx, &resp->alloc);
    if (!decomp)
        return -1;

    const char *supported = decomp->encoding(decomp);
    if (kl_ascii_strcasecmp(enc, supported) != 0) {
        decomp->destroy(decomp);
        return 0;  /* encoding mismatch: leave body as-is */
    }

    /* Decompress */
    char *out = NULL;
    size_t out_len = 0;
    int rc = decomp->decompress(decomp, resp->body, resp->body_len,
                                 &out, &out_len, &resp->alloc);
    decomp->destroy(decomp);

    if (rc < 0)
        return -1;

    /* Replace body */
    kl_free(&resp->alloc, resp->body, resp->body_len + 1);
    resp->body = out;
    resp->body_len = out_len;

    /* Remove Content-Encoding header */
    kl_http_client_remove_header(resp, "Content-Encoding");

    return 0;
}

/* ── Streaming decompression wrapper ─────────────────────────────── */

static int decomp_emit_to_user(void *ctx, const char *data, size_t len)
{
    DecompStreamWrap *w = ctx;
    if (!w->user_on_body)
        return 0;
    return w->user_on_body(data, len, w->user_data);
}

int kl_http_client_decomp_on_headers(int status, const KlHttpClientHeader *headers,
                                int num_headers, void *user_data)
{
    DecompStreamWrap *w = user_data;

    /* Check if Content-Encoding matches our decompressor */
    for (int i = 0; i < num_headers; i++) {
        if (kl_ascii_strcasecmp(headers[i].name, "Content-Encoding") == 0) {
            /* Create session and check encoding */
            KlDecompress *decomp = w->dcfg->factory(w->dcfg->ctx,
                                                      w->ds.alloc);
            if (decomp) {
                const char *supported = decomp->encoding(decomp);
                if (kl_ascii_strcasecmp(headers[i].value, supported) == 0) {
                    w->ds.decomp = decomp;
                    w->ds.error = 0;
                    w->active = 1;
                } else {
                    decomp->destroy(decomp);
                }
            }
            break;
        }
    }

    if (w->user_on_headers)
        return w->user_on_headers(status, headers, num_headers, w->user_data);
    return 0;
}

int kl_http_client_decomp_on_body(const char *data, size_t len, void *user_data)
{
    DecompStreamWrap *w = user_data;
    if (w->active) {
        return kl_decompress_stream_feed(&w->ds, data, len, 0,
                                          decomp_emit_to_user, w);
    }
    /* Passthrough */
    if (w->user_on_body)
        return w->user_on_body(data, len, w->user_data);
    return 0;
}

void kl_http_client_decomp_on_complete(void *user_data)
{
    DecompStreamWrap *w = user_data;
    if (w->active) {
        /* Final flush */
        kl_decompress_stream_feed(&w->ds, NULL, 0, 1,
                                   decomp_emit_to_user, w);
        kl_decompress_stream_free(&w->ds);
        w->active = 0;
    }
    if (w->user_on_complete)
        w->user_on_complete(w->user_data);
}

/* ── Response free ────────────────────────────────────────────────── */

void kl_http_client_response_free(KlHttpClientResponse *resp)
{
    if (!resp || !resp->alloc.malloc)
        return;

    if (resp->body) {
        kl_free(&resp->alloc, resp->body, resp->body_len + 1);
        resp->body = NULL;
        resp->body_len = 0;
    }

    if (resp->headers) {
        for (int i = 0; i < resp->num_headers; i++) {
            kl_free(&resp->alloc, (char *)resp->headers[i].name,
                    strlen(resp->headers[i].name) + 1);
            kl_free(&resp->alloc, (char *)resp->headers[i].value,
                    strlen(resp->headers[i].value) + 1);
        }
        kl_free(&resp->alloc, resp->headers,
                (size_t)resp->num_headers * sizeof(KlHttpClientHeader));
        resp->headers = NULL;
    }
    resp->num_headers = 0;
    resp->status = 0;
    memset(&resp->alloc, 0, sizeof(resp->alloc));
}
