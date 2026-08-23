/*
 * http2_nghttp2_server.c — server-side KlHttp2ServerSession backed by nghttp2.
 *
 * Maps the KEEL server session vtable (recv / submit_response / want_write /
 * flush / shutdown / destroy) onto an nghttp2 server session, and nghttp2's
 * receive callbacks back onto the KEEL-provided KlHttp2ServerCallbacks
 * (on_request / on_data / on_stream_end / on_stream_reset / send). nghttp2 is
 * confined to this TU; no nghttp2 type crosses into KEEL headers.
 *
 * Short writes: nghttp2's send callback re-queues any tail the KEEL `send`
 * callback leaves unsent (return < len), so frames are never truncated under
 * backpressure. Response bodies are copied at submit time (nghttp2 pulls DATA
 * frames asynchronously) and freed on stream close.
 *
 * SPDX-License-Identifier: MIT
 */
#include "keel_http2_nghttp2.h"

#include <nghttp2/nghttp2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Session + per-stream state ─────────────────────────────────────── */

typedef struct {
    KlHttp2ServerSession    base;       /* must be first */
    KlAllocator         *alloc;
    nghttp2_session     *ng;
    KlHttp2ServerCallbacks *cbs;        /* KEEL-provided (borrowed) */
    void                *ud;         /* KEEL user_data for cbs */
    int                  in_recv;    /* 1 while inside nghttp2_session_mem_recv */
} NgServerSession;

typedef struct {
    KlAllocator  *alloc;
    /* Accumulated request pseudo-headers + regular headers (valid until the
     * on_request delivery; the driver copies out during that call). */
    char         *method, *path, *authority;
    size_t        method_len, path_len, authority_len;
    const char  **names;
    const char  **values;
    size_t       *name_lens;
    size_t       *value_lens;
    int           n, cap;
    int           delivered;         /* on_request already fired */
    /* Response body copy (nghttp2 pulls DATA asynchronously). */
    char         *resp_body;
    size_t        resp_body_len, resp_body_off;
} NgServerStream;

/* ── Helpers ────────────────────────────────────────────────────────── */

static char *ng_dup(KlAllocator *a, const char *src, size_t len) {
    char *p = kl_malloc(a, len + 1);
    if (!p) return NULL;
    memcpy(p, src, len);
    p[len] = '\0';
    return p;
}

static void ng_sstream_free(NgServerStream *st) {
    if (!st) return;
    KlAllocator *a = st->alloc;
    for (int i = 0; i < st->n; i++) {
        kl_free(a, (void *)st->names[i], st->name_lens[i] + 1);
        kl_free(a, (void *)st->values[i], st->value_lens[i] + 1);
    }
    if (st->names)      kl_free(a, st->names,      (size_t)st->cap * sizeof(*st->names));
    if (st->values)     kl_free(a, st->values,     (size_t)st->cap * sizeof(*st->values));
    if (st->name_lens)  kl_free(a, st->name_lens,  (size_t)st->cap * sizeof(*st->name_lens));
    if (st->value_lens) kl_free(a, st->value_lens, (size_t)st->cap * sizeof(*st->value_lens));
    if (st->method)     kl_free(a, st->method,     st->method_len + 1);
    if (st->path)       kl_free(a, st->path,       st->path_len + 1);
    if (st->authority)  kl_free(a, st->authority,  st->authority_len + 1);
    if (st->resp_body)  kl_free(a, st->resp_body,  st->resp_body_len);
    kl_free(a, st, sizeof(*st));
}

static int ng_sstream_grow(NgServerStream *st) {
    if (st->n != st->cap) return 0;
    int ncap = st->cap ? st->cap * 2 : 8;
    if ((size_t)ncap > SIZE_MAX / sizeof(char *)) return -1;
    const char **nn = kl_realloc(st->alloc, st->names, (size_t)st->cap * sizeof(*nn), (size_t)ncap * sizeof(*nn));
    if (!nn) return -1;
    st->names = nn;
    const char **nv = kl_realloc(st->alloc, st->values, (size_t)st->cap * sizeof(*nv), (size_t)ncap * sizeof(*nv));
    if (!nv) return -1;
    st->values = nv;
    size_t *nl = kl_realloc(st->alloc, st->name_lens, (size_t)st->cap * sizeof(*nl), (size_t)ncap * sizeof(*nl));
    if (!nl) return -1;
    st->name_lens = nl;
    size_t *vl = kl_realloc(st->alloc, st->value_lens, (size_t)st->cap * sizeof(*vl), (size_t)ncap * sizeof(*vl));
    if (!vl) return -1;
    st->value_lens = vl;
    st->cap = ncap;
    return 0;
}

/* ── nghttp2 → KEEL callbacks ───────────────────────────────────────── */

static ssize_t ng_send_cb(nghttp2_session *ng, const uint8_t *data,
                                size_t length, int flags, void *user_data) {
    (void)ng; (void)flags;
    NgServerSession *s = user_data;
    ssize_t w = s->cbs->send(s->ud, data, length);
    if (w < 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (w == 0) return NGHTTP2_ERR_WOULDBLOCK;    /* nothing sent → would-block */
    return (ssize_t)w;                       /* nghttp2 buffers any tail */
}

static int ng_on_begin_headers_cb(nghttp2_session *ng, const nghttp2_frame *frame,
                                  void *user_data) {
    NgServerSession *s = user_data;
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
    NgServerStream *st = kl_malloc(s->alloc, sizeof(*st));
    if (!st) return NGHTTP2_ERR_CALLBACK_FAILURE;
    memset(st, 0, sizeof(*st));
    st->alloc = s->alloc;
    nghttp2_session_set_stream_user_data(ng, frame->hd.stream_id, st);
    return 0;
}

static int ng_on_header_cb(nghttp2_session *ng, const nghttp2_frame *frame,
                           const uint8_t *name, size_t namelen,
                           const uint8_t *value, size_t valuelen,
                           uint8_t flags, void *user_data) {
    (void)flags; (void)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    NgServerStream *st = nghttp2_session_get_stream_user_data(ng, frame->hd.stream_id);
    if (!st) return 0;

    if (namelen > 0 && name[0] == ':') {           /* pseudo-header */
        char **slot = NULL; size_t *slen = NULL;
        if (namelen == 7 && memcmp(name, ":method", 7) == 0)   { slot = &st->method;    slen = &st->method_len; }
        else if (namelen == 5 && memcmp(name, ":path", 5) == 0){ slot = &st->path;      slen = &st->path_len; }
        else if (namelen == 10 && memcmp(name, ":authority", 10) == 0) { slot = &st->authority; slen = &st->authority_len; }
        else return 0;                              /* :scheme etc. — ignored */
        if (*slot) return 0;                        /* keep first */
        *slot = ng_dup(st->alloc, (const char *)value, valuelen);
        if (*slot) *slen = valuelen;
        return 0;
    }

    if (ng_sstream_grow(st) < 0) return 0;
    char *nm = ng_dup(st->alloc, (const char *)name, namelen);
    char *vl = ng_dup(st->alloc, (const char *)value, valuelen);
    if (!nm || !vl) {
        if (nm) kl_free(st->alloc, nm, namelen + 1);
        if (vl) kl_free(st->alloc, vl, valuelen + 1);
        return 0;
    }
    st->names[st->n] = nm;      st->name_lens[st->n] = namelen;
    st->values[st->n] = vl;     st->value_lens[st->n] = valuelen;
    st->n++;
    return 0;
}

static void ng_deliver_request(NgServerSession *s, int32_t sid, NgServerStream *st) {
    if (st->delivered) return;
    st->delivered = 1;
    s->cbs->on_request(s->ud, (uint32_t)sid,
                        st->method ? st->method : "", st->method_len,
                        st->path ? st->path : "", st->path_len,
                        st->authority ? st->authority : "", st->authority_len,
                        st->names, st->values, st->name_lens, st->value_lens,
                        st->n);
}

static int ng_on_frame_recv_cb(nghttp2_session *ng, const nghttp2_frame *frame,
                               void *user_data) {
    NgServerSession *s = user_data;
    int32_t sid = frame->hd.stream_id;
    NgServerStream *st;

    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        st = nghttp2_session_get_stream_user_data(ng, sid);
        if (st) ng_deliver_request(s, sid, st);
    }
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        s->cbs->on_stream_end(s->ud, (uint32_t)sid);
    }
    return 0;
}

static int ng_on_data_chunk_cb(nghttp2_session *ng, uint8_t flags,
                               int32_t stream_id, const uint8_t *data,
                               size_t len, void *user_data) {
    (void)ng; (void)flags;
    NgServerSession *s = user_data;
    return s->cbs->on_data(s->ud, (uint32_t)stream_id, (const char *)data, len) < 0
               ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
}

static int ng_on_stream_close_cb(nghttp2_session *ng, int32_t stream_id,
                                 uint32_t error_code, void *user_data) {
    NgServerSession *s = user_data;
    NgServerStream *st = nghttp2_session_get_stream_user_data(ng, stream_id);
    if (error_code != NGHTTP2_NO_ERROR)
        s->cbs->on_stream_reset(s->ud, (uint32_t)stream_id, error_code);
    ng_sstream_free(st);
    return 0;
}

/* ── Response body data provider ────────────────────────────────────── */

static ssize_t ng_resp_body_read_cb(nghttp2_session *ng, int32_t stream_id,
                                          uint8_t *buf, size_t length,
                                          uint32_t *data_flags,
                                          nghttp2_data_source *source,
                                          void *user_data) {
    (void)source; (void)user_data;
    NgServerStream *st = nghttp2_session_get_stream_user_data(ng, stream_id);
    if (!st) { *data_flags |= NGHTTP2_DATA_FLAG_EOF; return 0; }
    size_t remain = st->resp_body_len - st->resp_body_off;
    size_t n = remain < length ? remain : length;
    if (n) { memcpy(buf, st->resp_body + st->resp_body_off, n); st->resp_body_off += n; }
    if (st->resp_body_off >= st->resp_body_len) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}

/* ── KEEL vtable ────────────────────────────────────────────────────── */

static kl_ssize_t ng_server_recv(KlHttp2ServerSession *self, const void *data, size_t len) {
    NgServerSession *s = (NgServerSession *)self;
    /* Guard against re-entrant send: KEEL's HTTP/2 server adapter submits a response + flushes
     * from within on_stream_end, which nghttp2 invokes inside mem_recv. Calling
     * nghttp2_session_send() re-entrantly there corrupts processing of later
     * frames in the same batch (an illegal trailing DATA/HEADERS would be missed).
     * Deferring the flush (see ng_server_flush) lets nghttp2 finish the whole
     * batch — generating the correct STREAM_CLOSED/PROTOCOL_ERROR — before KEEL's
     * post-recv flush (kl_http2_server_feed) sends everything in order. */
    s->in_recv = 1;
    ssize_t r = nghttp2_session_mem_recv(s->ng, (const uint8_t *)data, len);
    s->in_recv = 0;
    if (r < 0) {
        /* Fatal connection error: nghttp2 has queued a GOAWAY with the error
         * code. Flush it before we report -1 (KEEL closes on -1 without another
         * flush), so the peer sees the GOAWAY rather than a bare reset/timeout. */
        nghttp2_session_send(s->ng);
        return -1;
    }
    return (ssize_t)r;
}

static int ng_server_submit_response(KlHttp2ServerSession *self, uint32_t stream_id,
                                     int status, const char **hdr_names,
                                     const char **hdr_values, int num_headers,
                                     const void *body, size_t body_len) {
    NgServerSession *s = (NgServerSession *)self;
    NgServerStream *st = nghttp2_session_get_stream_user_data(s->ng, (int32_t)stream_id);

    if (st && body && body_len) {
        st->resp_body = kl_malloc(s->alloc, body_len);   /* copy — pulled async */
        if (!st->resp_body) return -1;
        memcpy(st->resp_body, body, body_len);
        st->resp_body_len = body_len;
    }

    if (num_headers < 0) num_headers = 0;
    int nv_cap = 1 + num_headers;
    if ((size_t)nv_cap > SIZE_MAX / sizeof(nghttp2_nv)) return -1;
    nghttp2_nv *nva = kl_malloc(s->alloc, (size_t)nv_cap * sizeof(nghttp2_nv));
    if (!nva) return -1;

    char status_str[8];
    int sl = snprintf(status_str, sizeof(status_str), "%d", status);
    if (sl < 0) { kl_free(s->alloc, nva, (size_t)nv_cap * sizeof(nghttp2_nv)); return -1; }
    size_t nvlen = 0;
    nva[nvlen].name = (uint8_t *)":status"; nva[nvlen].namelen = 7;
    nva[nvlen].value = (uint8_t *)status_str; nva[nvlen].valuelen = (size_t)sl;
    nva[nvlen].flags = NGHTTP2_NV_FLAG_NONE; nvlen++;
    for (int i = 0; i < num_headers; i++) {
        nva[nvlen].name = (uint8_t *)hdr_names[i]; nva[nvlen].namelen = strlen(hdr_names[i]);
        nva[nvlen].value = (uint8_t *)hdr_values[i]; nva[nvlen].valuelen = strlen(hdr_values[i]);
        nva[nvlen].flags = NGHTTP2_NV_FLAG_NONE; nvlen++;
    }

    nghttp2_data_provider prd;
    nghttp2_data_provider *prdp = NULL;
    if (st && st->resp_body_len) {
        prd.source.ptr = st;
        prd.read_callback = ng_resp_body_read_cb;
        prdp = &prd;
    }

    int rc = nghttp2_submit_response(s->ng, (int32_t)stream_id, nva, nvlen, prdp);
    kl_free(s->alloc, nva, (size_t)nv_cap * sizeof(nghttp2_nv));
    return rc == 0 ? 0 : -1;
}

static int ng_server_want_write(KlHttp2ServerSession *self) {
    NgServerSession *s = (NgServerSession *)self;
    return nghttp2_session_want_write(s->ng);
}

static int ng_server_want_read(KlHttp2ServerSession *self) {
    NgServerSession *s = (NgServerSession *)self;
    return nghttp2_session_want_read(s->ng);
}

static int ng_server_flush(KlHttp2ServerSession *self) {
    NgServerSession *s = (NgServerSession *)self;
    /* Defer while inside recv (see ng_server_recv); KEEL flushes again right
     * after mem_recv returns, so nothing is lost. */
    if (s->in_recv) return 0;
    return nghttp2_session_send(s->ng) == 0 ? 0 : -1;
}

static int ng_server_shutdown(KlHttp2ServerSession *self) {
    NgServerSession *s = (NgServerSession *)self;
    /* Graceful GOAWAY carrying the last processed stream id. */
    return nghttp2_session_terminate_session(s->ng, NGHTTP2_NO_ERROR) == 0 ? 0 : -1;
}

static void ng_server_destroy(KlHttp2ServerSession *self) {
    NgServerSession *s = (NgServerSession *)self;
    if (!s) return;
    if (s->ng) nghttp2_session_del(s->ng);
    kl_free(s->alloc, s, sizeof(*s));
}

/* ── Factory ────────────────────────────────────────────────────────── */

KlHttp2ServerSession *kl_http2_nghttp2_server_session(KlAllocator *alloc,
                                                KlHttp2ServerCallbacks *callbacks,
                                                void *user_data) {
    if (!callbacks) return NULL;
    NgServerSession *s = kl_malloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->cbs = callbacks;
    s->ud = user_data;
    s->base.recv = ng_server_recv;
    s->base.submit_response = ng_server_submit_response;
    s->base.want_write = ng_server_want_write;
    s->base.flush = ng_server_flush;
    s->base.shutdown = ng_server_shutdown;
    s->base.destroy = ng_server_destroy;
    s->base.want_read = ng_server_want_read;

    nghttp2_session_callbacks *cbs = NULL;
    if (nghttp2_session_callbacks_new(&cbs) != 0) {
        kl_free(alloc, s, sizeof(*s));
        return NULL;
    }
    nghttp2_session_callbacks_set_send_callback(cbs, ng_send_cb);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, ng_on_begin_headers_cb);
    nghttp2_session_callbacks_set_on_header_callback(cbs, ng_on_header_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, ng_on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, ng_on_data_chunk_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, ng_on_stream_close_cb);

    /* Expect the client connection preface ("PRI * HTTP/2.0...") on the stream —
     * nghttp2's default. KEEL feeds the full preface through for all three h2
     * server entry paths (ALPN-negotiated h2 over TLS, h2c Upgrade, and h2c
     * prior-knowledge — http_connection.c hands over the whole buffer, magic included),
     * so nghttp2 consumes it itself. */
    int rc = nghttp2_session_server_new(&s->ng, cbs, s);
    nghttp2_session_callbacks_del(cbs);
    if (rc != 0) {
        kl_free(alloc, s, sizeof(*s));
        return NULL;
    }

    /* Server connection preface: initial SETTINGS. */
    if (nghttp2_submit_settings(s->ng, NGHTTP2_FLAG_NONE, NULL, 0) != 0) {
        nghttp2_session_del(s->ng);
        kl_free(alloc, s, sizeof(*s));
        return NULL;
    }
    return &s->base;
}
