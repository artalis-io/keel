/*
 * test_h2_limits.c: the nghttp2 adapters bound what a peer can make them hold (eighteenth audit:
 * W1, W2, W5, W6, W11).
 *
 *   - The server advertises its limits in SETTINGS: MAX_CONCURRENT_STREAMS (it sent empty SETTINGS,
 *     so nghttp2's incoming stream limit stayed unlimited) and MAX_HEADER_LIST_SIZE.
 *   - A request with more headers than KEEL keeps is reset, not stored: every header was copied into
 *     the per-stream record with no count or byte limit (an HPACK bomb).
 *   - A graceful shutdown lets a response in flight finish: it used terminate_session, which stops
 *     all output once its GOAWAY is out.
 *   - The client refuses server push (ENABLE_PUSH = 0), and caps the response headers it stores.
 *
 * Each case drives an adapter session against a raw nghttp2 peer in memory. Exits non-zero on
 * the first failure.
 */
#include "keel_http2_nghttp2.h"
#include <keel/allocator.h>
#include <keel/http2.h>
#include <keel/http_request.h>
#include <nghttp2/nghttp2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define BUF_CAP (2 * 1024 * 1024)
static unsigned char g_c2s[BUF_CAP], g_s2c[BUF_CAP];
static size_t g_c2s_len, g_s2c_len;

static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }
#define NV(n, v) { (uint8_t *)(n), (uint8_t *)(v), sizeof(n) - 1, sizeof(v) - 1, NGHTTP2_NV_FLAG_NONE }

/* ════ Server adapter vs a raw nghttp2 client ════════════════════════════════════════════════ */

static KlHttp2ServerSession *g_ss;
static int g_requests, g_rst_seen, g_goaway_seen;
static size_t g_resp_body, g_resp_len;
static int g_shutdown_at;                 /* call ss->shutdown once this much body has arrived */

static int srv_on_request(void *ud, uint32_t sid, const char *m, size_t ml, const char *p, size_t pl,
                          const char *a, size_t al, const char **hn, const char **hv,
                          const size_t *hnl, const size_t *hvl, int nh) {
    (void)ud; (void)sid; (void)m; (void)ml; (void)p; (void)pl; (void)a; (void)al;
    (void)hn; (void)hv; (void)hnl; (void)hvl; (void)nh;
    g_requests++;
    return 0;
}
static int srv_on_data(void *ud, uint32_t sid, const char *d, size_t n) {
    (void)ud; (void)sid; (void)d; (void)n; return 0;
}
static int srv_on_stream_end(void *ud, uint32_t sid) {
    (void)ud;
    if (g_resp_len == 0) return 0;
    static char body[512 * 1024];
    memset(body, 'b', sizeof body);
    const char *hn[] = { "content-type" };
    const char *hv[] = { "text/plain" };
    (void)g_ss->submit_response(g_ss, sid, 200, hn, hv, 1, body, g_resp_len);
    return 0;
}
static void srv_on_stream_reset(void *ud, uint32_t sid, uint32_t ec) { (void)ud; (void)sid; (void)ec; }
static kl_ssize_t srv_send(void *ud, const void *d, size_t n) {
    (void)ud;
    if (g_s2c_len + n > BUF_CAP) return -1;
    memcpy(g_s2c + g_s2c_len, d, n);
    g_s2c_len += n;
    return (kl_ssize_t)n;
}

static ssize_t cli_send(nghttp2_session *s, const uint8_t *d, size_t n, int fl, void *ud) {
    (void)s; (void)fl; (void)ud;
    if (g_c2s_len + n > BUF_CAP) return NGHTTP2_ERR_WOULDBLOCK;
    memcpy(g_c2s + g_c2s_len, d, n);
    g_c2s_len += n;
    return (ssize_t)n;
}
static int cli_on_frame_recv(nghttp2_session *s, const nghttp2_frame *f, void *ud) {
    (void)s; (void)ud;
    if (f->hd.type == NGHTTP2_RST_STREAM) g_rst_seen = 1;
    if (f->hd.type == NGHTTP2_GOAWAY) g_goaway_seen = 1;
    return 0;
}
static int cli_on_data_chunk(nghttp2_session *s, uint8_t fl, int32_t sid, const uint8_t *d, size_t n,
                             void *ud) {
    (void)s; (void)fl; (void)sid; (void)d; (void)ud;
    g_resp_body += n;
    return 0;
}

static nghttp2_session *raw_client(void) {
    nghttp2_session_callbacks *cbs = NULL;
    nghttp2_session *cs = NULL;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback(cbs, cli_send);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, cli_on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, cli_on_data_chunk);
    nghttp2_session_client_new(&cs, cbs, NULL);
    nghttp2_session_callbacks_del(cbs);
    nghttp2_submit_settings(cs, NGHTTP2_FLAG_NONE, NULL, 0);
    return cs;
}

/* One exchange round between the raw client and the adapter server. */
static int pump_server(nghttp2_session *cs) {
    if (nghttp2_session_send(cs) != 0) return -1;
    if (g_c2s_len) { (void)g_ss->recv(g_ss, g_c2s, g_c2s_len); g_c2s_len = 0; }
    if (g_ss->want_write(g_ss)) (void)g_ss->flush(g_ss);
    if (g_s2c_len) {
        if (nghttp2_session_mem_recv(cs, g_s2c, g_s2c_len) < 0) return -1;
        g_s2c_len = 0;
    }
    if (g_shutdown_at && g_resp_body >= (size_t)g_shutdown_at) {
        g_shutdown_at = 0;
        (void)g_ss->shutdown(g_ss);
    }
    return 0;
}

static KlHttp2ServerSession *server_session(KlAllocator *a) {
    static KlHttp2ServerCallbacks scb;
    memset(&scb, 0, sizeof scb);
    scb.on_request = srv_on_request;
    scb.on_data = srv_on_data;
    scb.on_stream_end = srv_on_stream_end;
    scb.on_stream_reset = srv_on_stream_reset;
    scb.send = srv_send;
    return kl_http2_nghttp2_server_session(a, &scb, NULL);
}

static int case_settings(KlAllocator *a) {
    g_ss = server_session(a);
    nghttp2_session *cs = raw_client();
    for (int i = 0; i < 8; i++) if (pump_server(cs) < 0) return fail("exchange");
    uint32_t streams = nghttp2_session_get_remote_settings(cs, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
    uint32_t hlist = nghttp2_session_get_remote_settings(cs, NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE);
    nghttp2_session_del(cs);
    g_ss->destroy(g_ss);
    printf("  SETTINGS: max_concurrent_streams=%u max_header_list_size=%u\n", streams, hlist);
    if (streams != KL_HTTP2_DEFAULT_MAX_STREAMS)
        return fail("MAX_CONCURRENT_STREAMS is not advertised (was unlimited)");
    if (hlist == 0xffffffffu) return fail("MAX_HEADER_LIST_SIZE is not advertised");
    return 0;
}

static int case_too_many_headers(KlAllocator *a) {
    g_ss = server_session(a);
    nghttp2_session *cs = raw_client();
    g_requests = g_rst_seen = 0;
    enum { NHDR = KL_MAX_HEADERS + 8 };
    static nghttp2_nv nva[4 + NHDR];
    static char names[NHDR][16];
    nghttp2_nv base[] = { NV(":method", "GET"), NV(":path", "/"), NV(":scheme", "http"),
                          NV(":authority", "x") };
    memcpy(nva, base, sizeof base);
    for (int i = 0; i < NHDR; i++) {
        int l = snprintf(names[i], sizeof names[i], "x-h%d", i);
        nva[4 + i].name = (uint8_t *)names[i];
        nva[4 + i].namelen = (size_t)l;
        nva[4 + i].value = (uint8_t *)"v";
        nva[4 + i].valuelen = 1;
        nva[4 + i].flags = NGHTTP2_NV_FLAG_NONE;
    }
    if (nghttp2_submit_request(cs, NULL, nva, 4 + NHDR, NULL, NULL) < 0) return fail("submit");
    for (int i = 0; i < 16; i++) if (pump_server(cs) < 0) return fail("exchange");
    nghttp2_session_del(cs);
    g_ss->destroy(g_ss);
    printf("  %d headers: requests delivered=%d, RST seen=%d\n", (int)NHDR, g_requests, g_rst_seen);
    if (g_requests != 0) return fail("a request over the header limit was stored and delivered");
    if (!g_rst_seen) return fail("a request over the header limit was not reset");
    return 0;
}

static int case_graceful_shutdown(KlAllocator *a) {
    g_ss = server_session(a);
    nghttp2_session *cs = raw_client();
    g_resp_body = 0; g_goaway_seen = 0;
    g_resp_len = 300 * 1024;
    g_shutdown_at = 16 * 1024;            /* stop the server once the response is under way */
    nghttp2_nv nva[] = { NV(":method", "GET"), NV(":path", "/big"), NV(":scheme", "http"),
                         NV(":authority", "x") };
    if (nghttp2_submit_request(cs, NULL, nva, 4, NULL, NULL) < 0) return fail("submit");
    for (int i = 0; i < 400 && g_resp_body < g_resp_len; i++)
        if (pump_server(cs) < 0) return fail("exchange");
    nghttp2_session_del(cs);
    g_ss->destroy(g_ss);
    g_resp_len = 0;
    printf("  graceful shutdown: body %zu of %u, GOAWAY seen=%d\n", g_resp_body, 300 * 1024,
           g_goaway_seen);
    if (g_resp_body != 300 * 1024) return fail("the response in flight was cut by the shutdown");
    if (!g_goaway_seen) return fail("no GOAWAY");
    return 0;
}

/* ════ Client adapter vs a raw nghttp2 server ════════════════════════════════════════════════ */

static nghttp2_session *g_rs;
static int g_cli_closed, g_cli_err, g_cli_responses;
static int g_many_headers;

static int cli_on_send2(KlHttp2ClientSession *s, const void *d, size_t n) {
    (void)s;
    if (g_c2s_len + n > BUF_CAP) return -1;
    memcpy(g_c2s + g_c2s_len, d, n);
    g_c2s_len += n;
    return (int)n;
}
static void cli_on_response2(KlHttp2ClientSession *s, int32_t sid, int status,
                             const KlHttp2ClientHeader *h, int n) {
    (void)s; (void)sid; (void)status; (void)h; (void)n;
    g_cli_responses++;
}
static void cli_on_data2(KlHttp2ClientSession *s, int32_t sid, const char *d, size_t n) {
    (void)s; (void)sid; (void)d; (void)n;
}
static void cli_on_stream_close2(KlHttp2ClientSession *s, int32_t sid, int err) {
    (void)s; (void)sid;
    g_cli_closed = 1;
    g_cli_err = err;
}
static ssize_t rs_send(nghttp2_session *ng, const uint8_t *d, size_t n, int f, void *ud) {
    (void)ng; (void)f; (void)ud;
    if (g_s2c_len + n > BUF_CAP) return NGHTTP2_ERR_WOULDBLOCK;
    memcpy(g_s2c + g_s2c_len, d, n);
    g_s2c_len += n;
    return (ssize_t)n;
}
static int rs_frame_recv(nghttp2_session *ng, const nghttp2_frame *fr, void *ud) {
    (void)ud;
    if (!g_many_headers) return 0;
    if ((fr->hd.type == NGHTTP2_HEADERS || fr->hd.type == NGHTTP2_DATA) &&
        (fr->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        enum { N = 4 * KL_MAX_HEADERS };              /* past the client's cap */
        static nghttp2_nv nva[1 + N];
        static char names[N][16];
        nghttp2_nv st = NV(":status", "200");
        nva[0] = st;
        for (int i = 0; i < N; i++) {
            int l = snprintf(names[i], sizeof names[i], "x-r%d", i);
            nva[1 + i].name = (uint8_t *)names[i];
            nva[1 + i].namelen = (size_t)l;
            nva[1 + i].value = (uint8_t *)"v";
            nva[1 + i].valuelen = 1;
            nva[1 + i].flags = NGHTTP2_NV_FLAG_NONE;
        }
        nghttp2_submit_response(ng, fr->hd.stream_id, nva, 1 + N, NULL);
    }
    return 0;
}

static KlHttp2ClientSession *client_session(KlAllocator *a) {
    KlHttp2ClientSession *cs = kl_http2_nghttp2_client_session(a);
    if (!cs) return NULL;
    cs->keel_cbs.on_send = cli_on_send2;
    cs->keel_cbs.on_response = cli_on_response2;
    cs->keel_cbs.on_data = cli_on_data2;
    cs->keel_cbs.on_stream_close = cli_on_stream_close2;
    nghttp2_session_callbacks *cb;
    nghttp2_session_callbacks_new(&cb);
    nghttp2_session_callbacks_set_send_callback(cb, rs_send);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, rs_frame_recv);
    nghttp2_session_server_new(&g_rs, cb, NULL);
    nghttp2_session_callbacks_del(cb);
    nghttp2_submit_settings(g_rs, NGHTTP2_FLAG_NONE, NULL, 0);
    return cs;
}
static int pump_client(KlHttp2ClientSession *cs) {
    if (cs->flush(cs) < 0) return -1;
    if (g_c2s_len) {
        if (nghttp2_session_mem_recv(g_rs, g_c2s, g_c2s_len) < 0) return -1;
        g_c2s_len = 0;
    }
    if (nghttp2_session_send(g_rs) != 0) return -1;
    if (g_s2c_len) { (void)cs->recv(cs, (const char *)g_s2c, g_s2c_len); g_s2c_len = 0; }
    return 0;
}

static int case_client_refuses_push(KlAllocator *a) {
    g_many_headers = 0;
    KlHttp2ClientSession *cs = client_session(a);
    if (!cs) return fail("client session");
    if (cs->submit_request(cs, "GET", "/", "x", NULL, 0, NULL, 0) < 0) return fail("submit");
    for (int i = 0; i < 8; i++) if (pump_client(cs) < 0) return fail("exchange");
    uint32_t push = nghttp2_session_get_remote_settings(g_rs, NGHTTP2_SETTINGS_ENABLE_PUSH);
    cs->destroy(cs);
    nghttp2_session_del(g_rs);
    printf("  client ENABLE_PUSH=%u\n", push);
    if (push != 0) return fail("the client accepts server push");
    return 0;
}

static int case_client_header_cap(KlAllocator *a) {
    g_many_headers = 1;
    g_cli_closed = g_cli_err = g_cli_responses = 0;
    KlHttp2ClientSession *cs = client_session(a);
    if (!cs) return fail("client session");
    if (cs->submit_request(cs, "GET", "/", "x", NULL, 0, NULL, 0) < 0) return fail("submit");
    for (int i = 0; i < 16; i++) if (pump_client(cs) < 0) break;
    cs->destroy(cs);
    nghttp2_session_del(g_rs);
    g_many_headers = 0;
    printf("  %d response headers: responses=%d closed=%d err=%d\n", 4 * KL_MAX_HEADERS,
           g_cli_responses, g_cli_closed, g_cli_err);
    if (g_cli_responses != 0) return fail("a response over the client's header cap was stored");
    if (!g_cli_closed || g_cli_err == 0) return fail("the stream was not failed");
    return 0;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    int failed = 0;                        /* every case runs, so each one's result is seen */
    failed += case_settings(&alloc);
    failed += case_too_many_headers(&alloc);
    failed += case_graceful_shutdown(&alloc);
    failed += case_client_refuses_push(&alloc);
    failed += case_client_header_cap(&alloc);
    if (failed) { fprintf(stderr, "FAIL: %d case(s)\n", failed); return 1; }
    printf("nghttp2 adapter limits OK\n");
    return 0;
}
