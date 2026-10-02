/*
 * test_destroy_open_streams.c: destroying an nghttp2 adapter session that still has open streams
 * frees every per-stream record. The adapters freed a stream's record (the server's holds a copy of
 * the response body, the client's a copy of the request body) only in nghttp2's on_stream_close
 * callback, and nghttp2_session_del frees its streams without calling it. So a peer that requests a
 * response and drops the connection before it is sent leaked the copy, once per stream.
 *
 * The client sends a POST with a body; the server answers it, but its send callback reports
 * would-block, so nothing reaches the client and both streams stay open. Then both sessions are
 * destroyed, and a counting allocator must be back to zero. Exits non-zero on failure.
 */
#include "keel_http2_nghttp2.h"
#include <keel/allocator.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static long g_live;
static void *c_malloc(void *ctx, size_t n) { (void)ctx; void *p = malloc(n ? n : 1); if (p) g_live++; return p; }
static void *c_realloc(void *ctx, void *p, size_t o, size_t n) {
    (void)ctx; (void)o; void *q = realloc(p, n ? n : 1); if (q && !p) g_live++; return q;
}
static void c_free(void *ctx, void *p, size_t n) { (void)ctx; (void)n; if (p) { g_live--; free(p); } }

#define BUF_CAP (64 * 1024)
static unsigned char g_c2s[BUF_CAP];
static size_t g_c2s_len;
static KlHttp2ServerSession *g_ss;
static int g_answered;

static int cli_on_send(KlHttp2ClientSession *s, const void *data, size_t len) {
    (void)s;
    if (g_c2s_len + len > BUF_CAP) return -1;
    memcpy(g_c2s + g_c2s_len, data, len);
    g_c2s_len += len;
    return (int)len;
}

static int srv_on_request(void *ud, uint32_t sid, const char *m, size_t ml, const char *p, size_t pl,
                          const char *a, size_t al, const char **hn, const char **hv,
                          const size_t *hnl, const size_t *hvl, int nh) {
    (void)ud; (void)sid; (void)m; (void)ml; (void)p; (void)pl; (void)a; (void)al;
    (void)hn; (void)hv; (void)hnl; (void)hvl; (void)nh;
    return 0;
}
static int srv_on_data(void *ud, uint32_t sid, const char *d, size_t n) {
    (void)ud; (void)sid; (void)d; (void)n; return 0;
}
static int srv_on_stream_end(void *ud, uint32_t sid) {
    (void)ud;
    static char body[256 * 1024];                   /* the adapter copies it */
    memset(body, 'r', sizeof body);
    const char *hn[] = { "content-type" };
    const char *hv[] = { "text/plain" };
    if (g_ss->submit_response(g_ss, sid, 200, hn, hv, 1, body, sizeof body) == 0) g_answered = 1;
    return 0;
}
static void srv_on_stream_reset(void *ud, uint32_t sid, uint32_t ec) { (void)ud; (void)sid; (void)ec; }
static kl_ssize_t srv_send(void *ud, const void *d, size_t n) {
    (void)ud; (void)d; (void)n;
    return 0;                                       /* the peer never reads: would-block */
}

static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }

int main(void) {
    KlAllocator alloc = { c_malloc, c_realloc, c_free, NULL };

    KlHttp2ClientSession *cs = kl_http2_nghttp2_client_session(&alloc);
    if (!cs) return fail("client session create");
    cs->keel_cbs.on_send = cli_on_send;
    cs->keel_ctx = NULL;

    KlHttp2ServerCallbacks scb = {
        .on_request = srv_on_request, .on_data = srv_on_data, .on_stream_end = srv_on_stream_end,
        .on_stream_reset = srv_on_stream_reset, .send = srv_send,
    };
    g_ss = kl_http2_nghttp2_server_session(&alloc, &scb, NULL);
    if (!g_ss) { cs->destroy(cs); return fail("server session create"); }

    static char req_body[1024];                     /* the client adapter copies it; small enough
                                                     * to fit the window, so the request ends */
    memset(req_body, 'q', sizeof req_body);
    int32_t sid = cs->submit_request(cs, "POST", "/upload", "example.com", NULL, 0,
                                     req_body, sizeof req_body);
    if (sid < 0) { cs->destroy(cs); g_ss->destroy(g_ss); return fail("submit_request"); }
    for (int i = 0; i < 64; i++) {                  /* the client sends what flow control allows */
        if (cs->flush(cs) < 0) break;
        if (!g_c2s_len) break;
        (void)g_ss->recv(g_ss, g_c2s, g_c2s_len);
        g_c2s_len = 0;
        if (g_ss->want_write(g_ss)) (void)g_ss->flush(g_ss);
    }

    cs->destroy(cs);                                /* the client's stream is still open */
    g_ss->destroy(g_ss);                            /* so is the server's, with a queued response */

    if (!g_answered) return fail("the server never answered (its response record was not exercised)");
    if (g_live != 0) {
        fprintf(stderr, "FAIL: %ld allocation(s) not freed after destroying sessions with open streams\n",
                g_live);
        return 1;
    }
    printf("nghttp2 destroy with open streams OK (server answered: %d)\n", g_answered);
    return 0;
}
