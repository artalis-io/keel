/*
 * http2_nghttp2_client.c: client-side KlHttp2ClientSession backed by nghttp2.
 *
 * Maps the KEEL client session vtable (recv / submit_request / flush / destroy)
 * onto an nghttp2 client session, and nghttp2's receive callbacks back onto the
 * KEEL callbacks (on_send / on_response / on_data / on_stream_close). nghttp2 is
 * confined to this TU; no nghttp2 type crosses into KEEL headers.
 *
 * Ownership: per-stream state (accumulated response headers, a copy of the
 * request body) hangs off nghttp2's per-stream user_data and is freed exactly
 * once, on stream close. The request body is copied at submit time so the caller
 * may free it immediately (KEEL's copied-immediately write contract).
 *
 * SPDX-License-Identifier: MIT
 */
#include "keel_http2_nghttp2.h"

#include <nghttp2/nghttp2.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>   /* ssize_t (classic nghttp2 callback return type) */

/* ── Session + per-stream state ─────────────────────────────────────── */

typedef struct {
    KlHttp2ClientSession base;      /* must be first: vtable the driver sees */
    KlAllocator      *alloc;
    nghttp2_session  *ng;
} NgClientSession;

typedef struct {
    KlAllocator      *alloc;
    int               status;
    KlHttp2ClientHeader *hdrs;      /* accumulated response headers (dup'd) */
    int               n;
    int               cap;
    char             *body;      /* copy of the request body (or NULL) */
    size_t            body_len;
    size_t            body_off;
} NgClientStream;

/* ── Small helpers ──────────────────────────────────────────────────── */

static char *ng_dup(KlAllocator *a, const char *src, size_t len) {
    char *p = kl_malloc(a, len + 1);
    if (!p) return NULL;
    memcpy(p, src, len);
    p[len] = '\0';
    return p;
}

static int ng_parse_status(const uint8_t *v, size_t len) {
    int s = 0;
    for (size_t i = 0; i < len && v[i] >= '0' && v[i] <= '9'; i++)
        s = s * 10 + (v[i] - '0');
    return s;
}

static void ng_stream_free(NgClientStream *st) {
    if (!st) return;
    KlAllocator *a = st->alloc;
    for (int i = 0; i < st->n; i++) {
        kl_free(a, (void *)st->hdrs[i].name, strlen(st->hdrs[i].name) + 1);
        kl_free(a, (void *)st->hdrs[i].value, strlen(st->hdrs[i].value) + 1);
    }
    if (st->hdrs) kl_free(a, st->hdrs, (size_t)st->cap * sizeof(*st->hdrs));
    if (st->body) kl_free(a, st->body, st->body_len);
    kl_free(a, st, sizeof(*st));
}

static int ng_stream_add_header(NgClientStream *st,
                                const uint8_t *name, size_t namelen,
                                const uint8_t *value, size_t valuelen) {
    if (st->n == st->cap) {
        int ncap = st->cap ? st->cap * 2 : 8;
        if ((size_t)ncap > SIZE_MAX / sizeof(*st->hdrs)) return -1;
        KlHttp2ClientHeader *nh = kl_realloc(st->alloc, st->hdrs,
                                          (size_t)st->cap * sizeof(*st->hdrs),
                                          (size_t)ncap * sizeof(*st->hdrs));
        if (!nh) return -1;
        st->hdrs = nh;
        st->cap = ncap;
    }
    char *nm = ng_dup(st->alloc, (const char *)name, namelen);
    char *vl = ng_dup(st->alloc, (const char *)value, valuelen);
    if (!nm || !vl) {
        if (nm) kl_free(st->alloc, nm, namelen + 1);
        if (vl) kl_free(st->alloc, vl, valuelen + 1);
        return -1;
    }
    st->hdrs[st->n].name = nm;
    st->hdrs[st->n].value = vl;
    st->n++;
    return 0;
}

/* ── nghttp2 → KEEL callbacks ───────────────────────────────────────── */

static ssize_t ng_send_cb(nghttp2_session *ng, const uint8_t *data,
                                size_t length, int flags, void *user_data) {
    (void)ng; (void)flags;
    NgClientSession *s = user_data;
    int w = s->base.keel_cbs.on_send(&s->base, data, length);
    if (w < 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (w == 0) return NGHTTP2_ERR_WOULDBLOCK;    /* nothing sent → would-block */
    return (ssize_t)w;                       /* nghttp2 buffers any tail */
}

static int ng_on_header_cb(nghttp2_session *ng, const nghttp2_frame *frame,
                           const uint8_t *name, size_t namelen,
                           const uint8_t *value, size_t valuelen,
                           uint8_t flags, void *user_data) {
    (void)flags; (void)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    NgClientStream *st = nghttp2_session_get_stream_user_data(ng, frame->hd.stream_id);
    if (!st) return 0;
    if (namelen == 7 && memcmp(name, ":status", 7) == 0) {
        st->status = ng_parse_status(value, valuelen);
        return 0;
    }
    if (namelen > 0 && name[0] == ':') return 0;   /* other pseudo-headers */
    (void)ng_stream_add_header(st, name, namelen, value, valuelen);
    return 0;
}

static int ng_on_frame_recv_cb(nghttp2_session *ng, const nghttp2_frame *frame,
                               void *user_data) {
    NgClientSession *s = user_data;
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        NgClientStream *st = nghttp2_session_get_stream_user_data(ng, frame->hd.stream_id);
        if (st)
            s->base.keel_cbs.on_response(&s->base, frame->hd.stream_id,
                                         st->status, st->hdrs, st->n);
    }
    return 0;
}

static int ng_on_data_chunk_cb(nghttp2_session *ng, uint8_t flags,
                               int32_t stream_id, const uint8_t *data,
                               size_t len, void *user_data) {
    (void)ng; (void)flags;
    NgClientSession *s = user_data;
    s->base.keel_cbs.on_data(&s->base, stream_id, (const char *)data, len);
    return 0;
}

static int ng_on_stream_close_cb(nghttp2_session *ng, int32_t stream_id,
                                 uint32_t error_code, void *user_data) {
    NgClientSession *s = user_data;
    NgClientStream *st = nghttp2_session_get_stream_user_data(ng, stream_id);
    s->base.keel_cbs.on_stream_close(&s->base, stream_id, (int)error_code);
    ng_stream_free(st);
    return 0;
}

/* ── Request body data provider ─────────────────────────────────────── */

static ssize_t ng_body_read_cb(nghttp2_session *ng, int32_t stream_id,
                                     uint8_t *buf, size_t length,
                                     uint32_t *data_flags,
                                     nghttp2_data_source *source,
                                     void *user_data) {
    (void)source; (void)user_data;
    NgClientStream *st = nghttp2_session_get_stream_user_data(ng, stream_id);
    if (!st) { *data_flags |= NGHTTP2_DATA_FLAG_EOF; return 0; }
    size_t remain = st->body_len - st->body_off;
    size_t n = remain < length ? remain : length;
    if (n) { memcpy(buf, st->body + st->body_off, n); st->body_off += n; }
    if (st->body_off >= st->body_len) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

/* ── KEEL vtable ────────────────────────────────────────────────────── */

static int ng_client_recv(KlHttp2ClientSession *self, const char *data, size_t len) {
    NgClientSession *s = (NgClientSession *)self;
    ssize_t r = nghttp2_session_mem_recv(s->ng, (const uint8_t *)data, len);
    return r < 0 ? -1 : (int)r;
}

static int ng_client_flush(KlHttp2ClientSession *self) {
    NgClientSession *s = (NgClientSession *)self;
    return nghttp2_session_send(s->ng) == 0 ? 0 : -1;
}

static int32_t ng_client_submit(KlHttp2ClientSession *self,
                                const char *method, const char *path,
                                const char *authority,
                                const KlHttp2ClientHeader *hdrs, int n,
                                const char *body, size_t body_len) {
    NgClientSession *s = (NgClientSession *)self;

    NgClientStream *st = kl_malloc(s->alloc, sizeof(*st));
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->alloc = s->alloc;
    if (body && body_len) {
        st->body = kl_malloc(s->alloc, body_len);   /* copy: caller may free */
        if (!st->body) { kl_free(s->alloc, st, sizeof(*st)); return -1; }
        memcpy(st->body, body, body_len);
        st->body_len = body_len;
    }

    /* :method :scheme :authority :path, then user headers. */
    int nv_cap = 4 + (n > 0 ? n : 0);
    if ((size_t)nv_cap > SIZE_MAX / sizeof(nghttp2_nv)) {
        ng_stream_free(st); return -1;
    }
    nghttp2_nv *nva = kl_malloc(s->alloc, (size_t)nv_cap * sizeof(nghttp2_nv));
    if (!nva) { ng_stream_free(st); return -1; }
    size_t nvlen = 0;
    #define NG_PUT(nm, vl) do { \
        nva[nvlen].name = (uint8_t *)(nm); nva[nvlen].namelen = strlen(nm); \
        nva[nvlen].value = (uint8_t *)(vl); nva[nvlen].valuelen = strlen(vl); \
        nva[nvlen].flags = NGHTTP2_NV_FLAG_NONE; nvlen++; \
    } while (0)
    NG_PUT(":method", method);
    NG_PUT(":scheme", "https");
    NG_PUT(":authority", authority);
    NG_PUT(":path", path);
    for (int i = 0; i < n; i++) {
        nva[nvlen].name = (uint8_t *)hdrs[i].name; nva[nvlen].namelen = strlen(hdrs[i].name);
        nva[nvlen].value = (uint8_t *)hdrs[i].value; nva[nvlen].valuelen = strlen(hdrs[i].value);
        nva[nvlen].flags = NGHTTP2_NV_FLAG_NONE; nvlen++;
    }
    #undef NG_PUT

    nghttp2_data_provider prd;
    nghttp2_data_provider *prdp = NULL;
    if (st->body_len) {
        prd.source.ptr = st;
        prd.read_callback = ng_body_read_cb;
        prdp = &prd;
    }

    int32_t sid = nghttp2_submit_request(s->ng, NULL, nva, nvlen, prdp, st);
    kl_free(s->alloc, nva, (size_t)nv_cap * sizeof(nghttp2_nv));
    if (sid < 0) { ng_stream_free(st); return -1; }
    return sid;
}

static void ng_client_destroy(KlHttp2ClientSession *self) {
    NgClientSession *s = (NgClientSession *)self;
    if (!s) return;
    if (s->ng) nghttp2_session_del(s->ng);
    kl_free(s->alloc, s, sizeof(*s));
}

/* ── Factory ────────────────────────────────────────────────────────── */

KlHttp2ClientSession *kl_http2_nghttp2_client_session(KlAllocator *alloc) {
    NgClientSession *s = kl_malloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->base.recv = ng_client_recv;
    s->base.submit_request = ng_client_submit;
    s->base.flush = ng_client_flush;
    s->base.destroy = ng_client_destroy;

    nghttp2_session_callbacks *cbs = NULL;
    if (nghttp2_session_callbacks_new(&cbs) != 0) {
        kl_free(alloc, s, sizeof(*s));
        return NULL;
    }
    nghttp2_session_callbacks_set_send_callback(cbs, ng_send_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, ng_on_header_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, ng_on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, ng_on_data_chunk_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, ng_on_stream_close_cb);

    int rc = nghttp2_session_client_new(&s->ng, cbs, s);
    nghttp2_session_callbacks_del(cbs);
    if (rc != 0) {
        kl_free(alloc, s, sizeof(*s));
        return NULL;
    }

    /* Queue the client's initial SETTINGS (sent with the preface on first flush). */
    if (nghttp2_submit_settings(s->ng, NGHTTP2_FLAG_NONE, NULL, 0) != 0) {
        nghttp2_session_del(s->ng);
        kl_free(alloc, s, sizeof(*s));
        return NULL;
    }
    return &s->base;
}
