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
#include "../../decompress_internal.h"   /* kl_decompress_vtable_valid: required-subset gate */

#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>

#include "socket.h"     /* seam: kl_sock_* + KlSockAddr (no direct sockaddr) */
#include "http_client_internal.h"
#include "kl_cstr.h"    /* locale-free append builders + ASCII case compare */
#include "url_internal.h"   /* kl_url_authority */

/* ── CRLF injection guard ────────────────────────────────────────── */

int kl_http_client_has_crlf(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' || s[i] == '\n')
            return 1;
    }
    return 0;
}

/* ── Authority (Host / absolute-form) ──────────────────────────────── */

/* The one authority builder, shared with the WebSocket and HTTP/2 clients (src/url.c). */
int kl_http_client_authority(const KlUrl *url, char *out, size_t cap)
{
    return kl_url_authority(url, out, cap);
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

    char authority[KL_HTTP_CLIENT_HOSTNAME_MAX + 16];
    int alen = kl_http_client_authority(url, authority, sizeof authority);
    if (alen < 0) return NULL;
    char buf[KL_HTTP_CLIENT_REQ_BUF_SIZE];
    size_t off = 0;
    if (kl_buf_append(buf, sizeof(buf), &off, method) != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, " ", 1) != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, target, (size_t)target_len) != 0 ||
        kl_buf_append(buf, sizeof(buf), &off, " HTTP/1.1\r\nHost: ") != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, authority, (size_t)alen) != 0 ||
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

/* headers + "Proxy-Authorization: <auth>" in a new array (*owned, *out_n entries; the caller frees it
 * at *out_n * sizeof). NULL on allocation failure. */
const KlHttpClientHeader *kl_http_client_with_proxy_auth(KlAllocator *alloc,
                                                         const KlHttpClientHeader *headers,
                                                         int num_headers, const char *auth,
                                                         KlHttpClientHeader **owned, int *out_n)
{
    *owned = NULL;
    if (num_headers < 0 || num_headers > INT_MAX - 1 ||
        (size_t)num_headers + 1 > SIZE_MAX / sizeof(KlHttpClientHeader))
        return NULL;
    KlHttpClientHeader *h = kl_malloc(alloc, ((size_t)num_headers + 1) * sizeof(KlHttpClientHeader));
    if (!h)
        return NULL;
    if (num_headers > 0)
        memcpy(h, headers, (size_t)num_headers * sizeof(KlHttpClientHeader));
    h[num_headers].name = "Proxy-Authorization";
    h[num_headers].value = auth;
    *owned = h;
    *out_n = num_headers + 1;
    return h;
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

    char authority[KL_HTTP_CLIENT_HOSTNAME_MAX + 16];
    int alen = kl_http_client_authority(url, authority, sizeof authority);
    if (alen < 0) return NULL;
    char buf[KL_HTTP_CLIENT_REQ_BUF_SIZE];
    size_t off = 0;
    if (kl_buf_append(buf, sizeof(buf), &off, method) != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, " ", 1) != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, target, (size_t)target_len) != 0 ||
        kl_buf_append(buf, sizeof(buf), &off, " HTTP/1.1\r\nHost: ") != 0 ||
        kl_buf_append_n(buf, sizeof(buf), &off, authority, (size_t)alen) != 0 ||
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
            /* The array is allocated at exactly num_headers entries, and kl_http_client_response_free
             * frees it at that size. Shrinking the count alone would free it with the wrong size, so
             * move the survivors into an array of the new size. If that allocation fails, keep the
             * header: a stale header is harmless, a wrong-size free is not. */
            int n = resp->num_headers - 1;
            KlHttpClientHeader *nh = NULL;
            if (n > 0) {
                nh = kl_malloc(&resp->alloc, (size_t)n * sizeof(KlHttpClientHeader));
                if (!nh) return;
            }
            kl_free(&resp->alloc, (char *)resp->headers[i].name,
                    strlen(resp->headers[i].name) + 1);
            kl_free(&resp->alloc, (char *)resp->headers[i].value,
                    strlen(resp->headers[i].value) + 1);
            if (nh)                                     /* NULL only when no header survives */
                for (int j = 0, k = 0; j < resp->num_headers; j++)
                    if (j != i) nh[k++] = resp->headers[j];
            kl_free(&resp->alloc, resp->headers,
                    (size_t)resp->num_headers * sizeof(KlHttpClientHeader));
            resp->headers = nh;
            resp->num_headers = n;
            return;
        }
    }
}

/* Connection is a comma-separated token list (RFC 9110 7.6.1): "close" anywhere in it closes. */
static int connection_has_close(const char *v)
{
    while (*v) {
        while (*v == ' ' || *v == '\t' || *v == ',') v++;
        const char *t = v;
        while (*v && *v != ',' && *v != ' ' && *v != '\t') v++;
        if (v - t == 5 && kl_ascii_strncasecmp(t, "close", 5) == 0) return 1;
    }
    return 0;
}

int kl_http_client_server_wants_close(const KlHttpClientResponse *resp)
{
    if (resp->closes)                   /* what the parser derived (HTTP/1.0 rules included) */
        return 1;
    for (int i = 0; i < resp->num_headers; i++) {
        if (kl_ascii_strcasecmp(resp->headers[i].name, "Connection") == 0 &&
            connection_has_close(resp->headers[i].value))
            return 1;
    }
    return 0;
}

/* ── Response decompression (buffered) ────────────────────────────── */

/* Accumulates a decompressed body for the bounded buffered path: grows a NUL-terminated buffer and
 * refuses output past max as soon as it is produced, rather than after the whole body inflated. */
typedef struct {
    KlAllocator *alloc;
    char        *buf;
    size_t       len, cap;     /* cap bytes allocated; len + 1 <= cap once anything is held */
    size_t       max;
    int          too_large;
} BoundedBody;

static int bounded_body_emit(void *ctx, const char *data, size_t n)
{
    BoundedBody *b = ctx;
    if (n > b->max || b->len > b->max - n) { b->too_large = 1; return -1; }
    if (b->len + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap : 256;
        while (want < b->len + n + 1) want *= 2;          /* len + n + 1 <= max + 1: no overflow */
        if (want > b->max + 1) want = b->max + 1;
        char *nb = kl_realloc(b->alloc, b->buf, b->cap, want);
        if (!nb) return -1;
        b->buf = nb;
        b->cap = want;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    return 0;
}

/**
 * Post-process a buffered response: decompress body if Content-Encoding
 * matches the decompressor's encoding. Replaces body and removes header.
 */
int kl_http_client_decompress_response_body(KlHttpClientResponse *resp,
                                       KlDecompressConfig *dcfg, size_t max)
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
    if (!kl_decompress_vtable_valid(decomp)) {
        if (decomp->destroy) decomp->destroy(decomp);   /* guard: a malformed table may omit destroy */
        return -1;
    }

    const char *supported = decomp->encoding(decomp);
    if (kl_ascii_strcasecmp(enc, supported) != 0) {
        decomp->destroy(decomp);
        return 0;  /* encoding mismatch: leave body as-is */
    }

    if (max > 0 && max < SIZE_MAX - 1) {
        /* Bounded: inflate through the streaming op and stop at the limit, so a small body that
         * inflates past it costs at most max bytes, not the whole inflated size first. */
        BoundedBody b = { &resp->alloc, NULL, 0, 0, max, 0 };
        int frc = decomp->dfeed(decomp, resp->body, resp->body_len, 1, bounded_body_emit, &b);
        decomp->destroy(decomp);
        if (frc < 0 || b.too_large) {
            if (b.buf) kl_free(&resp->alloc, b.buf, b.cap);
            return b.too_large ? -2 : -1;
        }
        if (b.cap != b.len + 1) {                /* body-shaped: freed later at body_len + 1 */
            char *nb = kl_realloc(&resp->alloc, b.buf, b.cap, b.len + 1);
            if (!nb) { if (b.buf) kl_free(&resp->alloc, b.buf, b.cap); return -1; }
            b.buf = nb;
        }
        b.buf[b.len] = '\0';
        kl_free(&resp->alloc, resp->body, resp->body_len + 1);
        resp->body = b.buf;
        resp->body_len = b.len;
        kl_http_client_remove_header(resp, "Content-Encoding");
        return 0;
    }

    /* Unbounded: one shot. */
    char *out = NULL;
    size_t out_len = 0;
    int rc = decomp->decompress(decomp, resp->body, resp->body_len,
                                 &out, &out_len, &resp->alloc);
    decomp->destroy(decomp);

    if (rc < 0)
        return -1;

    /* The decompressor's buffer is out_len bytes (KlDecompress contract: free with out_len), while a
     * response body is NUL-terminated and freed at body_len + 1. Move it into a body-shaped buffer
     * so every later free uses the size the block was allocated with. */
    if (out_len > SIZE_MAX - 1) {
        kl_free(&resp->alloc, out, out_len);
        return -1;
    }
    char *body = kl_malloc(&resp->alloc, out_len + 1);
    if (!body) {
        kl_free(&resp->alloc, out, out_len);
        return -1;
    }
    if (out_len) memcpy(body, out, out_len);
    body[out_len] = '\0';
    if (out) kl_free(&resp->alloc, out, out_len);

    /* Replace body */
    kl_free(&resp->alloc, resp->body, resp->body_len + 1);
    resp->body = body;
    resp->body_len = out_len;

    /* Remove Content-Encoding header */
    kl_http_client_remove_header(resp, "Content-Encoding");

    return 0;
}

/* ── Streaming decompression wrapper ─────────────────────────────── */

static int decomp_emit_to_user(void *ctx, const char *data, size_t len)
{
    DecompStreamWrap *w = ctx;
    /* max_response_size bounds the decompressed bytes delivered, not only those on the wire. */
    if (w->max > 0 && (len > w->max || w->emitted > w->max - len)) {
        w->failed = 1;
        return -1;
    }
    w->emitted += len;
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
            if (decomp && !kl_decompress_vtable_valid(decomp)) {
                /* Malformed session: leave decompression inactive (graceful, like an
                 * encoding mismatch). Guard destroy (a malformed table may omit it). */
                if (decomp->destroy) decomp->destroy(decomp);
                decomp = NULL;
            }
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
        int r = kl_decompress_stream_feed(&w->ds, data, len, 0, decomp_emit_to_user, w);
        if (r < 0) w->failed = 1;
        return r;
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
        /* Final flush: a failure here (a truncated stream) fails the request; on_complete has no
         * error channel, so the client checks `failed` once the response is parsed. */
        if (kl_decompress_stream_feed(&w->ds, NULL, 0, 1, decomp_emit_to_user, w) < 0)
            w->failed = 1;
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
