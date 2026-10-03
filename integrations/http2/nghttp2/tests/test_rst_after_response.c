/*
 * test_rst_after_response.c: a response sent while the client is still uploading ends the upload.
 * KEEL answers an over-limit body with a 413 (END_STREAM) and drops the stream, but the nghttp2
 * adapter never reset it, and nghttp2 keeps updating the flow-control windows on its own, so a
 * client streaming an unbounded body kept sending and the server took and discarded all of it:
 * max_body_size no longer bounded the bytes received. RFC 9113 8.1: after a complete response the
 * server may send RST_STREAM with NO_ERROR to stop the request.
 *
 * A raw nghttp2 client uploads a body with no end; the adapter's server answers 413 on the first
 * DATA (as KEEL does once the limit is passed). The client must see RST_STREAM(NO_ERROR) for the
 * stream. Exits non-zero on failure.
 */
#include "keel_http2_nghttp2.h"
#include <keel/allocator.h>
#include <nghttp2/nghttp2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_CAP (1024 * 1024)
static unsigned char g_c2s[BUF_CAP], g_s2c[BUF_CAP];
static size_t g_c2s_len, g_s2c_len;
static KlHttp2ServerSession *g_ss;
static int g_answered, g_status, g_rst_seen;
static uint32_t g_rst_code = 0xffffffffu;
static size_t g_uploaded;

/* ── the adapter's server side ── */
static int srv_on_request(void *ud, uint32_t sid, const char *m, size_t ml, const char *p, size_t pl,
                          const char *a, size_t al, const char **hn, const char **hv,
                          const size_t *hnl, const size_t *hvl, int nh) {
    (void)ud; (void)sid; (void)m; (void)ml; (void)p; (void)pl; (void)a; (void)al;
    (void)hn; (void)hv; (void)hnl; (void)hvl; (void)nh;
    return 0;
}
static int srv_on_data(void *ud, uint32_t sid, const char *d, size_t n) {
    (void)ud; (void)d; (void)n;
    if (!g_answered) {                              /* over the limit: 413, as KEEL answers */
        const char *hn[] = { "content-type" };
        const char *hv[] = { "text/plain" };
        if (g_ss->submit_response(g_ss, sid, 413, hn, hv, 1, "too large", 9) == 0) g_answered = 1;
    }
    return 0;
}
static int srv_on_stream_end(void *ud, uint32_t sid) { (void)ud; (void)sid; return 0; }
static void srv_on_stream_reset(void *ud, uint32_t sid, uint32_t ec) { (void)ud; (void)sid; (void)ec; }
static kl_ssize_t srv_send(void *ud, const void *d, size_t n) {
    (void)ud;
    if (g_s2c_len + n > BUF_CAP) return -1;
    memcpy(g_s2c + g_s2c_len, d, n);
    g_s2c_len += n;
    return (kl_ssize_t)n;
}

/* ── a raw nghttp2 client that uploads without end ── */
static ssize_t cli_send(nghttp2_session *s, const uint8_t *d, size_t n, int flags, void *ud) {
    (void)s; (void)flags; (void)ud;
    if (g_c2s_len + n > BUF_CAP) return NGHTTP2_ERR_WOULDBLOCK;
    memcpy(g_c2s + g_c2s_len, d, n);
    g_c2s_len += n;
    return (ssize_t)n;
}
static ssize_t cli_read_body(nghttp2_session *s, int32_t sid, uint8_t *buf, size_t len,
                             uint32_t *flags, nghttp2_data_source *src, void *ud) {
    (void)s; (void)sid; (void)flags; (void)src; (void)ud;
    size_t n = len < 4096 ? len : 4096;
    memset(buf, 'u', n);                            /* never NGHTTP2_DATA_FLAG_EOF */
    g_uploaded += n;
    return (ssize_t)n;
}
static int cli_on_header(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name, size_t nl,
                         const uint8_t *value, size_t vl, uint8_t flags, void *ud) {
    (void)s; (void)f; (void)flags; (void)ud;
    if (nl == 7 && memcmp(name, ":status", 7) == 0 && vl == 3)
        g_status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + (value[2] - '0');
    return 0;
}
static int cli_on_frame_recv(nghttp2_session *s, const nghttp2_frame *f, void *ud) {
    (void)s; (void)ud;
    if (f->hd.type == NGHTTP2_RST_STREAM) {
        g_rst_seen = 1;
        g_rst_code = f->rst_stream.error_code;
    }
    return 0;
}

static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }

#define NV(n, v) { (uint8_t *)(n), (uint8_t *)(v), sizeof(n) - 1, sizeof(v) - 1, NGHTTP2_NV_FLAG_NONE }

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ServerCallbacks scb = {
        .on_request = srv_on_request, .on_data = srv_on_data, .on_stream_end = srv_on_stream_end,
        .on_stream_reset = srv_on_stream_reset, .send = srv_send,
    };
    g_ss = kl_http2_nghttp2_server_session(&alloc, &scb, NULL);
    if (!g_ss) return fail("server session create");

    nghttp2_session_callbacks *cbs = NULL;
    nghttp2_session *cs = NULL;
    if (nghttp2_session_callbacks_new(&cbs) != 0) return fail("callbacks");
    nghttp2_session_callbacks_set_send_callback(cbs, cli_send);
    nghttp2_session_callbacks_set_on_header_callback(cbs, cli_on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, cli_on_frame_recv);
    int rc = nghttp2_session_client_new(&cs, cbs, NULL);
    nghttp2_session_callbacks_del(cbs);
    if (rc != 0) return fail("client session create");
    (void)nghttp2_submit_settings(cs, NGHTTP2_FLAG_NONE, NULL, 0);
    nghttp2_nv nva[] = { NV(":method", "POST"), NV(":path", "/up"), NV(":scheme", "http"),
                         NV(":authority", "example.com") };
    nghttp2_data_provider prd;
    memset(&prd, 0, sizeof prd);
    prd.read_callback = cli_read_body;
    if (nghttp2_submit_request(cs, NULL, nva, sizeof nva / sizeof nva[0], &prd, NULL) < 0)
        return fail("submit_request");

    for (int i = 0; i < 64 && !g_rst_seen; i++) {
        if (nghttp2_session_send(cs) != 0) break;
        if (g_c2s_len) {
            (void)g_ss->recv(g_ss, g_c2s, g_c2s_len);
            g_c2s_len = 0;
        }
        if (g_ss->want_write(g_ss)) (void)g_ss->flush(g_ss);
        if (g_s2c_len) {
            if (nghttp2_session_mem_recv(cs, g_s2c, g_s2c_len) < 0) break;
            g_s2c_len = 0;
        }
    }

    nghttp2_session_del(cs);
    g_ss->destroy(g_ss);

    if (!g_answered || g_status != 413) return fail("the server's 413 never reached the client");
    if (!g_rst_seen) {
        fprintf(stderr, "FAIL: no RST_STREAM after the 413; the client uploaded %zu bytes\n", g_uploaded);
        return 1;
    }
    if (g_rst_code != NGHTTP2_NO_ERROR) {
        fprintf(stderr, "FAIL: RST_STREAM code %u, want NO_ERROR\n", (unsigned)g_rst_code);
        return 1;
    }
    printf("nghttp2 RST_STREAM(NO_ERROR) after an early response OK (%zu bytes uploaded)\n", g_uploaded);
    return 0;
}
