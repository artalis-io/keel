/*
 * http2_nghttp2_server.c: server-side KlHttp2ServerSession backed by nghttp2.
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
#include <keel/http2.h>          /* KL_HTTP2_DEFAULT_MAX_STREAMS, KL_HTTP2_MAX_HEADER_LIST_SIZE */
#include <keel/http_request.h>   /* KL_MAX_HEADERS */

#include <nghttp2/nghttp2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Session + per-stream state ─────────────────────────────────────── */

typedef struct NgServerStream NgServerStream;

typedef struct {
    KlHttp2ServerSession    base;       /* must be first */
    KlAllocator         *alloc;
    nghttp2_session     *ng;
    KlHttp2ServerCallbacks *cbs;        /* KEEL-provided (borrowed) */
    void                *ud;         /* KEEL user_data for cbs */
    int                  in_recv;    /* 1 while inside nghttp2_session_mem_recv */
    NgServerStream      *live;       /* every stream record not yet freed (see ng_server_destroy) */
} NgServerSession;

struct NgServerStream {
    NgServerStream *prev, *next;     /* the session's live list; unlinked on free */
    NgServerSession *sess;           /* NULL until linked */
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
    size_t        hlist;             /* header list so far (RFC 9113 6.5.2 accounting) */
    /* Response body copy (nghttp2 pulls DATA asynchronously). */
    char         *resp_body;
    size_t        resp_body_len, resp_body_off;
};

/* ── Helpers ────────────────────────────────────────────────────────── */

static char *ng_dup(KlAllocator *a, const char *src, size_t len) {
    char *p = kl_malloc(a, len + 1);
    if (!p) return NULL;
    memcpy(p, src, len);
    p[len] = '\0';
    return p;
}

/* A new, zeroed stream record, linked into the session's live list. */
static NgServerStream *ng_sstream_new(NgServerSession *s) {
    NgServerStream *st = kl_malloc(s->alloc, sizeof(*st));
    if (!st) return NULL;
    memset(st, 0, sizeof(*st));
    st->alloc = s->alloc;
    st->sess = s;
    st->next = s->live;
    if (s->live) s->live->prev = st;
    s->live = st;
    return st;
}

static void ng_sstream_free(NgServerStream *st) {
    if (!st) return;
    if (st->sess) {                                  /* unlink from the live list */
        if (st->prev) st->prev->next = st->next;
        else          st->sess->live = st->next;
        if (st->next) st->next->prev = st->prev;
    }
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
    if ((size_t)ncap > SIZE_MAX / sizeof(size_t)) return -1;
    /* All four arrays are made at the new size before any old one is let go, so a failure part way
     * leaves every array at st->cap (the size the free path uses). */
    size_t oc = (size_t)st->cap, nc = (size_t)ncap;
    const char **nn = kl_malloc(st->alloc, nc * sizeof(*nn));
    const char **nv = kl_malloc(st->alloc, nc * sizeof(*nv));
    size_t *nl = kl_malloc(st->alloc, nc * sizeof(*nl));
    size_t *vl = kl_malloc(st->alloc, nc * sizeof(*vl));
    if (!nn || !nv || !nl || !vl) {
        if (nn) kl_free(st->alloc, nn, nc * sizeof(*nn));
        if (nv) kl_free(st->alloc, nv, nc * sizeof(*nv));
        if (nl) kl_free(st->alloc, nl, nc * sizeof(*nl));
        if (vl) kl_free(st->alloc, vl, nc * sizeof(*vl));
        return -1;
    }
    if (oc > 0) {
        memcpy(nn, st->names, oc * sizeof(*nn));
        memcpy(nv, st->values, oc * sizeof(*nv));
        memcpy(nl, st->name_lens, oc * sizeof(*nl));
        memcpy(vl, st->value_lens, oc * sizeof(*vl));
        kl_free(st->alloc, st->names, oc * sizeof(*nn));
        kl_free(st->alloc, st->values, oc * sizeof(*nv));
        kl_free(st->alloc, st->name_lens, oc * sizeof(*nl));
        kl_free(st->alloc, st->value_lens, oc * sizeof(*vl));
    }
    st->names = nn; st->values = nv; st->name_lens = nl; st->value_lens = vl;
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
    NgServerStream *st = ng_sstream_new(s);
    if (!st) return NGHTTP2_ERR_CALLBACK_FAILURE;
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

    /* Bound what a request makes us hold: past KL_MAX_HEADERS fields (KEEL keeps no more) or the
     * advertised header-list size, reset the stream. Each field is copied below, so an unbounded
     * list (one HPACK table entry referenced again and again) was a peer-driven memory cost. */
    NgServerSession *s = st->sess;
    size_t cap = (s && s->cbs->max_header_list_size) ? s->cbs->max_header_list_size
                                                     : KL_HTTP2_MAX_HEADER_LIST_SIZE;
    st->hlist += namelen + valuelen + 32;
    if (st->hlist > cap || (!(namelen > 0 && name[0] == ':') && st->n >= KL_MAX_HEADERS))
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;

    if (namelen > 0 && name[0] == ':') {           /* pseudo-header */
        char **slot = NULL; size_t *slen = NULL;
        if (namelen == 7 && memcmp(name, ":method", 7) == 0)   { slot = &st->method;    slen = &st->method_len; }
        else if (namelen == 5 && memcmp(name, ":path", 5) == 0){ slot = &st->path;      slen = &st->path_len; }
        else if (namelen == 10 && memcmp(name, ":authority", 10) == 0) { slot = &st->authority; slen = &st->authority_len; }
        else return 0;                              /* :scheme etc.: ignored */
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
    int rc = s->cbs->on_request(s->ud, (uint32_t)sid,
                        st->method ? st->method : "", st->method_len,
                        st->path ? st->path : "", st->path_len,
                        st->authority ? st->authority : "", st->authority_len,
                        st->names, st->values, st->name_lens, st->value_lens,
                        st->n);
    /* The server could not take the request (it answers a stream it refuses itself): reset the
     * stream so the client is not left waiting for a response that never comes. */
    if (rc < 0)
        (void)nghttp2_submit_rst_stream(s->ng, NGHTTP2_FLAG_NONE, sid, NGHTTP2_INTERNAL_ERROR);
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
        if (s->cbs->on_stream_end(s->ud, (uint32_t)sid) < 0)   /* no response could be made */
            (void)nghttp2_submit_rst_stream(ng, NGHTTP2_FLAG_NONE, sid, NGHTTP2_INTERNAL_ERROR);
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
    /* Report every close: KEEL has already finished and forgotten a stream it answered (a no-op
     * then), and one it still tracks was cut short, by a reset with any code, NO_ERROR included,
     * so its slot must be released. */
    s->cbs->on_stream_reset(s->ud, (uint32_t)stream_id, error_code);
    ng_sstream_free(st);
    return 0;
}

/* A response that ended its stream (END_STREAM sent) while the client is still sending the request:
 * KEEL answered early (a 413 for an over-limit body, a refused reader) and has dropped the stream.
 * Reset it with NO_ERROR (RFC 9113 8.1) so the client stops; nghttp2 would otherwise keep opening
 * the flow-control windows, and the client's upload would be received and discarded unbounded. */
static int ng_on_frame_send_cb(nghttp2_session *ng, const nghttp2_frame *frame, void *user_data) {
    (void)user_data;
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) &&
        nghttp2_session_get_stream_remote_close(ng, frame->hd.stream_id) == 0)
        (void)nghttp2_submit_rst_stream(ng, NGHTTP2_FLAG_NONE, frame->hd.stream_id, NGHTTP2_NO_ERROR);
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
     * batch, generating the correct STREAM_CLOSED/PROTOCOL_ERROR, before KEEL's
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
        st->resp_body = kl_malloc(s->alloc, body_len);   /* copy: pulled async */
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
    /* Graceful: a GOAWAY naming the last stream processed, after which no new stream is accepted
     * but those already open run to completion (terminate_session would stop all output once its
     * GOAWAY is out, cutting responses in flight). The session then wants neither read nor write
     * once they are done, and KEEL closes the connection. */
    if (nghttp2_submit_goaway(s->ng, NGHTTP2_FLAG_NONE,
                              nghttp2_session_get_last_proc_stream_id(s->ng),
                              NGHTTP2_NO_ERROR, NULL, 0) != 0)
        return -1;
    return 0;
}

/* HTTP2-Settings is a SETTINGS payload in base64url without padding (RFC 7540 3.2.1). Decodes
 * into out (cap bytes); returns the decoded length, or -1 if malformed or too long. */
static long ng_b64url_decode(const char *in, size_t len, uint8_t *out, size_t cap) {
    while (len > 0 && in[len - 1] == '=') len--;   /* tolerate padding */
    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = in[i];
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '-') v = 62;
        else if (ch == '_') v = 63;
        else return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1u;
        }
    }
    return (long)o;
}

/* h2c Upgrade (RFC 7540 3.2): apply the client's HTTP2-Settings and open stream 1 half-closed
 * (remote) for the upgrading request. Stream 1 gets the same per-stream record as any request
 * stream, so the response KEEL submits on it carries its body. */
static int ng_server_upgrade(KlHttp2ServerSession *self, const char *settings, size_t settings_len,
                             int head_request) {
    NgServerSession *s = (NgServerSession *)self;
    uint8_t payload[256];                    /* 6 bytes per setting; 42 settings is plenty */
    long n = ng_b64url_decode(settings, settings_len, payload, sizeof(payload));
    if (n < 0 || n % 6 != 0) return -1;
    NgServerStream *st = ng_sstream_new(s);
    if (!st) return -1;
    st->delivered = 1;                      /* KEEL builds stream 1's request itself */
    if (nghttp2_session_upgrade2(s->ng, payload, (size_t)n, head_request, st) != 0) {
        ng_sstream_free(st);
        return -1;
    }
    /* upgrade2 does not attach stream_user_data on a server session: attach it explicitly, or the
     * response submitted on stream 1 finds no record to carry its body. */
    if (nghttp2_session_set_stream_user_data(s->ng, 1, st) != 0) {
        ng_sstream_free(st);
        return -1;
    }
    return 0;
}

static void ng_server_destroy(KlHttp2ServerSession *self) {
    NgServerSession *s = (NgServerSession *)self;
    if (!s) return;
    if (s->ng) nghttp2_session_del(s->ng);
    /* nghttp2_session_del frees its streams without calling on_stream_close, which is where a
     * record is normally freed: free the records of streams still open (a peer that dropped the
     * connection mid-response would otherwise leak the response body copy). */
    while (s->live) ng_sstream_free(s->live);
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
    s->base.upgrade = ng_server_upgrade;

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
    nghttp2_session_callbacks_set_on_frame_send_callback(cbs, ng_on_frame_send_cb);

    /* Expect the client connection preface ("PRI * HTTP/2.0...") on the stream:
     * nghttp2's default. KEEL feeds the full preface through for all three h2
     * server entry paths (ALPN-negotiated h2 over TLS, h2c Upgrade, and h2c
     * prior-knowledge: http_connection.c hands over the whole buffer, magic included),
     * so nghttp2 consumes it itself. */
    int rc = nghttp2_session_server_new(&s->ng, cbs, s);
    nghttp2_session_callbacks_del(cbs);
    if (rc != 0) {
        kl_free(alloc, s, sizeof(*s));
        return NULL;
    }

    /* Server connection preface: SETTINGS with the limits KEEL enforces. Empty SETTINGS left
     * nghttp2's incoming stream limit unlimited, so a peer could open streams without bound. */
    nghttp2_settings_entry iv[3];
    size_t niv = 0;
    iv[niv].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    iv[niv++].value = callbacks->max_concurrent_streams ? callbacks->max_concurrent_streams
                                                         : KL_HTTP2_DEFAULT_MAX_STREAMS;
    iv[niv].settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE;
    iv[niv++].value = callbacks->max_header_list_size ? callbacks->max_header_list_size
                                                       : KL_HTTP2_MAX_HEADER_LIST_SIZE;
    if (callbacks->initial_window_size) {
        iv[niv].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
        iv[niv++].value = callbacks->initial_window_size;
    }
    if (nghttp2_submit_settings(s->ng, NGHTTP2_FLAG_NONE, iv, niv) != 0) {
        nghttp2_session_del(s->ng);
        kl_free(alloc, s, sizeof(*s));
        return NULL;
    }
    return &s->base;
}
