/*
 * test_client_interim.c: the nghttp2 client adapter reports the FINAL response after an interim
 * (1xx) one. nghttp2 delivers the final HEADERS block that follows a 1xx as NGHTTP2_HCAT_HEADERS,
 * and the adapter reported only HCAT_RESPONSE, so after "103 Early Hints" then "200" the client saw
 * status 103 (with the 103's headers) and the 200's body. A raw nghttp2 server session sends 103
 * then 200 + body; the Keel client adapter must report 200 with the 200's own headers.
 * Exits non-zero on failure.
 */
#include "keel_http2_nghttp2.h"
#include <keel/allocator.h>
#include <nghttp2/nghttp2.h>

#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define BUF_CAP (64 * 1024)
static unsigned char g_c2s[BUF_CAP]; static size_t g_c2s_len;
static unsigned char g_s2c[BUF_CAP]; static size_t g_s2c_len;

static int  g_status, g_responses, g_closed;
static char g_hdr_final[64];      /* value of x-final in the reported headers ("" if absent) */
static int  g_saw_hint_header;    /* the 103's "link" header leaked into the final response */
static size_t g_body;

/* ── Keel client adapter callbacks ─────────────────────────────────── */
static int cli_on_send(KlHttp2ClientSession *s, const void *d, size_t n) {
    (void)s;
    if (g_c2s_len + n > BUF_CAP) return -1;
    memcpy(g_c2s + g_c2s_len, d, n); g_c2s_len += n;
    return (int)n;
}
static void cli_on_response(KlHttp2ClientSession *s, int32_t sid, int status,
                            const KlHttp2ClientHeader *h, int n) {
    (void)s; (void)sid;
    g_status = status;
    g_responses++;
    g_hdr_final[0] = '\0';
    g_saw_hint_header = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(h[i].name, "x-final") == 0) snprintf(g_hdr_final, sizeof g_hdr_final, "%s", h[i].value);
        if (strcmp(h[i].name, "link") == 0) g_saw_hint_header = 1;
    }
}
static void cli_on_data(KlHttp2ClientSession *s, int32_t sid, const char *d, size_t n) {
    (void)s; (void)sid; (void)d; g_body += n;
}
static void cli_on_stream_close(KlHttp2ClientSession *s, int32_t sid, int err) {
    (void)s; (void)sid; (void)err; g_closed = 1;
}

/* ── Raw nghttp2 server ─────────────────────────────────────────────── */
static nghttp2_session *g_srv;
static ssize_t srv_send(nghttp2_session *ng, const uint8_t *d, size_t n, int f, void *ud) {
    (void)ng; (void)f; (void)ud;
    if (g_s2c_len + n > BUF_CAP) return NGHTTP2_ERR_CALLBACK_FAILURE;
    memcpy(g_s2c + g_s2c_len, d, n); g_s2c_len += n;
    return (ssize_t)n;
}
static ssize_t body_read(nghttp2_session *ng, int32_t sid, uint8_t *buf, size_t len,
                         uint32_t *flags, nghttp2_data_source *src, void *ud) {
    (void)ng; (void)sid; (void)src; (void)ud;
    const char *b = "final body";
    size_t n = strlen(b) < len ? strlen(b) : len;
    memcpy(buf, b, n);
    *flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)n;
}
#define NV(n, v) { (uint8_t *)(n), (uint8_t *)(v), sizeof(n) - 1, sizeof(v) - 1, NGHTTP2_NV_FLAG_NONE }
static int srv_frame_recv(nghttp2_session *ng, const nghttp2_frame *fr, void *ud) {
    (void)ud;
    if ((fr->hd.type == NGHTTP2_HEADERS || fr->hd.type == NGHTTP2_DATA) &&
        (fr->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        nghttp2_nv hint[] = { NV(":status", "103"), NV("link", "</style.css>; rel=preload") };
        nghttp2_submit_headers(ng, NGHTTP2_FLAG_NONE, fr->hd.stream_id, NULL, hint, 2, NULL);
        nghttp2_nv fin[] = { NV(":status", "200"), NV("x-final", "yes") };
        nghttp2_data_provider prd;
        prd.source.ptr = NULL;
        prd.read_callback = body_read;
        nghttp2_submit_response(ng, fr->hd.stream_id, fin, 2, &prd);
    }
    return 0;
}

static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ClientSession *cs = kl_http2_nghttp2_client_session(&alloc);
    if (!cs) return fail("client session create");
    cs->keel_cbs.on_send = cli_on_send;
    cs->keel_cbs.on_response = cli_on_response;
    cs->keel_cbs.on_data = cli_on_data;
    cs->keel_cbs.on_stream_close = cli_on_stream_close;

    nghttp2_session_callbacks *cb;
    nghttp2_session_callbacks_new(&cb);
    nghttp2_session_callbacks_set_send_callback(cb, srv_send);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cb, srv_frame_recv);
    nghttp2_session_server_new(&g_srv, cb, NULL);
    nghttp2_session_callbacks_del(cb);
    nghttp2_submit_settings(g_srv, NGHTTP2_FLAG_NONE, NULL, 0);

    if (cs->submit_request(cs, "GET", "/", "example.com", NULL, 0, NULL, 0) < 0)
        return fail("submit_request");
    for (int i = 0; i < 50; i++) {
        if (cs->flush(cs) < 0) return fail("client flush");
        if (g_c2s_len) {
            if (nghttp2_session_mem_recv(g_srv, g_c2s, g_c2s_len) < 0) return fail("server recv");
            g_c2s_len = 0;
        }
        if (nghttp2_session_send(g_srv) != 0) return fail("server send");
        if (g_s2c_len) {
            if (cs->recv(cs, (const char *)g_s2c, g_s2c_len) < 0) return fail("client recv");
            g_s2c_len = 0;
        } else if (!g_c2s_len) {
            break;
        }
    }
    cs->destroy(cs);
    nghttp2_session_del(g_srv);

    if (!g_closed) return fail("the stream never closed");
    if (g_status != 200) {
        fprintf(stderr, "FAIL: reported status %d, want 200 (the final response after 103)\n", g_status);
        return 1;
    }
    if (strcmp(g_hdr_final, "yes") != 0) return fail("the final response's own headers were not reported");
    if (g_saw_hint_header) return fail("the 103's headers leaked into the final response");
    if (g_body != strlen("final body")) return fail("body length");
    printf("nghttp2 client interim response OK (%d response report(s), status %d)\n", g_responses, g_status);
    return 0;
}
