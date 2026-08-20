/*
 * test_roundtrip.c — in-memory client<->server roundtrip over the nghttp2
 * adapters. No sockets, no KEEL event loop: the two sessions are wired directly,
 * each side's on_send output fed into the other's recv, so a full HTTP/2 request
 * + response is exercised through real nghttp2 framing. Validates both adapters
 * against each other. Exits non-zero on any mismatch.
 */
#include "keel_h2_nghttp2.h"
#include <keel/allocator.h>

#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define BUF_CAP  (64 * 1024)

typedef struct {
    unsigned char c2s[BUF_CAP]; size_t c2s_len;   /* client -> server */
    unsigned char s2c[BUF_CAP]; size_t s2c_len;   /* server -> client */

    /* server-observed request */
    char req_method[16]; char req_path[64];
    int  got_request, got_stream_end;

    /* client-observed response */
    int    resp_status;
    char   resp_body[256]; size_t resp_body_len;
    int    resp_closed;

    KlHttp2ServerSession *ss;
    int    responded;
} Ctx;

static int fail(const char *msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

/* ── client callbacks (KlHttp2ClientSession keel_cbs) ──────────────────── */

static int cli_on_send(KlHttp2ClientSession *s, const void *data, size_t len) {
    Ctx *c = s->keel_ctx;
    if (c->c2s_len + len > BUF_CAP) return -1;
    memcpy(c->c2s + c->c2s_len, data, len);
    c->c2s_len += len;
    return (int)len;
}
static void cli_on_response(KlHttp2ClientSession *s, int32_t sid, int status,
                            const KlHttp2ClientHeader *hdrs, int n) {
    (void)sid; (void)hdrs; (void)n;
    ((Ctx *)s->keel_ctx)->resp_status = status;
}
static void cli_on_data(KlHttp2ClientSession *s, int32_t sid, const char *data, size_t len) {
    (void)sid;
    Ctx *c = s->keel_ctx;
    if (c->resp_body_len + len < sizeof(c->resp_body)) {
        memcpy(c->resp_body + c->resp_body_len, data, len);
        c->resp_body_len += len;
    }
}
static void cli_on_stream_close(KlHttp2ClientSession *s, int32_t sid, int err) {
    (void)sid; (void)err;
    ((Ctx *)s->keel_ctx)->resp_closed = 1;
}

/* ── server callbacks (KlHttp2ServerCallbacks) ─────────────────────────── */

static int srv_on_request(void *ud, uint32_t sid,
                          const char *method, size_t method_len,
                          const char *path, size_t path_len,
                          const char *authority, size_t authority_len,
                          const char **hn, const char **hv,
                          const size_t *hnl, const size_t *hvl, int nh) {
    (void)sid; (void)authority; (void)authority_len; (void)hn; (void)hv;
    (void)hnl; (void)hvl; (void)nh;
    Ctx *c = ud;
    size_t m = method_len < sizeof(c->req_method) - 1 ? method_len : sizeof(c->req_method) - 1;
    size_t p = path_len < sizeof(c->req_path) - 1 ? path_len : sizeof(c->req_path) - 1;
    memcpy(c->req_method, method, m); c->req_method[m] = 0;
    memcpy(c->req_path, path, p); c->req_path[p] = 0;
    c->got_request = 1;
    return 0;
}
static int srv_on_data(void *ud, uint32_t sid, const char *data, size_t len) {
    (void)ud; (void)sid; (void)data; (void)len; return 0;
}
static int srv_on_stream_end(void *ud, uint32_t sid) {
    Ctx *c = ud;
    c->got_stream_end = 1;
    if (!c->responded) {
        c->responded = 1;
        const char *hn[] = { "content-type" };
        const char *hv[] = { "text/plain" };
        const char *body = "hello h2";
        c->ss->submit_response(c->ss, sid, 200, hn, hv, 1, body, strlen(body));
    }
    return 0;
}
static void srv_on_stream_reset(void *ud, uint32_t sid, uint32_t ec) {
    (void)ud; (void)sid; (void)ec;
}
static kl_ssize_t srv_send(void *ud, const void *data, size_t len) {
    Ctx *c = ud;
    if (c->s2c_len + len > BUF_CAP) return -1;
    memcpy(c->s2c + c->s2c_len, data, len);
    c->s2c_len += len;
    return (ssize_t)len;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    static Ctx c;
    memset(&c, 0, sizeof(c));

    KlHttp2ClientSession *cs = kl_http2_nghttp2_client_session(&alloc);
    if (!cs) return fail("client session create");
    cs->keel_cbs.on_send = cli_on_send;
    cs->keel_cbs.on_response = cli_on_response;
    cs->keel_cbs.on_data = cli_on_data;
    cs->keel_cbs.on_stream_close = cli_on_stream_close;
    cs->keel_ctx = &c;

    KlHttp2ServerCallbacks scb = {
        .on_request = srv_on_request,
        .on_data = srv_on_data,
        .on_stream_end = srv_on_stream_end,
        .on_stream_reset = srv_on_stream_reset,
        .send = srv_send,
    };
    KlHttp2ServerSession *ss = kl_http2_nghttp2_server_session(&alloc, &scb, &c);
    if (!ss) { cs->destroy(cs); return fail("server session create"); }
    c.ss = ss;

    /* GET / */
    int32_t sid = cs->submit_request(cs, "GET", "/", "example.com", NULL, 0, NULL, 0);
    if (sid < 0) { cs->destroy(cs); ss->destroy(ss); return fail("submit_request"); }

    /* Pump until quiescent: move client->server and server->client bytes. The
     * server session consumes the client connection preface itself (nghttp2
     * default), so feed the client's bytes through verbatim — magic included. */
    for (int iter = 0; iter < 100; iter++) {
        int moved = 0;
        if (cs->flush(cs) < 0) { cs->destroy(cs); ss->destroy(ss); return fail("client flush"); }
        if (c.c2s_len) {
            if (ss->recv(ss, c.c2s, c.c2s_len) < 0) { cs->destroy(cs); ss->destroy(ss); return fail("server recv"); }
            c.c2s_len = 0; moved = 1;
        }
        if (ss->want_write(ss)) ss->flush(ss);
        if (c.s2c_len) {
            if (cs->recv(cs, (const char *)c.s2c, c.s2c_len) < 0) { cs->destroy(cs); ss->destroy(ss); return fail("client recv"); }
            c.s2c_len = 0; moved = 1;
        }
        if (!moved) break;
    }

    cs->destroy(cs);
    ss->destroy(ss);

    /* Assertions. */
    if (!c.got_request)                     return fail("server never got request");
    if (strcmp(c.req_method, "GET") != 0)   return fail("method != GET");
    if (strcmp(c.req_path, "/") != 0)       return fail("path != /");
    if (!c.got_stream_end)                  return fail("no END_STREAM on request");
    if (c.resp_status != 200)               return fail("status != 200");
    if (c.resp_body_len != 8 || memcmp(c.resp_body, "hello h2", 8) != 0)
        return fail("response body mismatch");
    if (!c.resp_closed)                     return fail("client stream not closed");

    printf("nghttp2 roundtrip OK: %s %s -> %d (%zu bytes body)\n",
           c.req_method, c.req_path, c.resp_status, c.resp_body_len);
    return 0;
}
