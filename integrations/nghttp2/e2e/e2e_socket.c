/*
 * e2e_socket.c — end-to-end HTTP/2 over a REAL loopback socket + KEEL event
 * loop, both sides backed by the nghttp2 adapters.
 *
 * Unlike test_roundtrip.c (which wires the two sessions in memory), this drives:
 *   - the SERVER adapter through KEEL's real HTTP/2 server path: kl_server with
 *     KlConfig.h2 = { .factory = kl_h2_nghttp2_server_session }, entered via h2c
 *     prior-knowledge (the "PRI * HTTP/2.0" preface), routing GET /hello; and
 *   - the CLIENT adapter through kl_h2_client_connect() over a cleartext http://
 *     connection, driven by a KlEventCtx run loop.
 *
 * So both adapters are exercised over a genuine TCP connection + event loop, not
 * just against each other in memory. Exits non-zero on any mismatch/timeout.
 */
#include <keel/keel.h>
#include "keel_h2_nghttp2.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define E2E_PORT 18477

static void nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }

/* ── server side ────────────────────────────────────────────────────── */

static void handle_hello(KlRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{\"msg\":\"hello h2 e2e\"}", 21);
}

static KlServer g_srv;
static void *srv_thread(void *a) { (void)a; kl_server_run(&g_srv); return NULL; }

/* ── client side ────────────────────────────────────────────────────── */

typedef struct { int done, status; char body[256]; size_t body_len; int err; } CliState;

static void on_response(KlH2ClientConn *c, int32_t sid,
                        const KlH2ClientResponse *resp, void *ud) {
    (void)c; (void)sid;
    CliState *st = ud;
    st->status = resp->status;
    st->body_len = resp->body_len < sizeof(st->body) ? resp->body_len : sizeof(st->body) - 1;
    if (resp->body && st->body_len) memcpy(st->body, resp->body, st->body_len);
    st->body[st->body_len] = 0;
    st->done = 1;
}
static void on_error(KlH2ClientConn *c, const char *msg, void *ud) {
    (void)c;
    CliState *st = ud;
    fprintf(stderr, "client error: %s\n", msg);
    st->err = 1; st->done = 1;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();

    /* Server: real KEEL HTTP/2 (h2c prior-knowledge) via the nghttp2 factory. */
    KlH2ServerConfig h2cfg = { .factory = kl_h2_nghttp2_server_session };
    KlConfig cfg = { .port = E2E_PORT, .bind_addr = "127.0.0.1", .h2 = &h2cfg };
    if (kl_server_init(&g_srv, &cfg) < 0) return fail("server init");
    kl_server_route(&g_srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_thread, NULL) != 0) return fail("server thread");
    for (int i = 0; i < 200 && g_srv.bound_port == 0; i++) nap(5);
    if (g_srv.bound_port <= 0) return fail("server never bound");

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", g_srv.bound_port);

    /* Client: real kl_h2_client_connect over cleartext h2c, nghttp2-backed. */
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) return fail("event ctx init");
    KlH2ClientConfig ccfg = { .session = kl_h2_nghttp2_client_session };

    CliState st = {0};
    KlH2ClientConn *c = kl_h2_client_connect(&ev, &alloc, &ccfg, url, on_error, &st);
    if (!c) { kl_event_ctx_free(&ev); return fail("client connect"); }

    /* kl_h2_client_request returns -1 until the async connect reaches ACTIVE
     * (no connection-ready callback is exposed), so retry submit each tick until
     * it takes, then pump until the response callback fires. */
    int32_t sid = -1;
    for (int i = 0; i < 500 && !st.done; i++) {
        if (sid < 0 && !st.err)
            sid = kl_h2_client_request(c, "GET", "/hello", NULL, 0, NULL, 0, on_response, &st);
        if (kl_event_ctx_run(&ev, 16, 20) < 0) break;
    }
    if (sid < 0 && !st.err) { kl_h2_client_free(c); kl_event_ctx_free(&ev);
        kl_server_stop(&g_srv); pthread_join(tid, NULL); kl_server_free(&g_srv);
        return fail("request never submitted (connection never became ACTIVE)"); }

    kl_h2_client_free(c);
    kl_event_ctx_free(&ev);
    kl_server_stop(&g_srv);
    pthread_join(tid, NULL);
    kl_server_free(&g_srv);

    if (st.err)                 return fail("client reported error");
    if (!st.done)              return fail("timed out waiting for response");
    if (st.status != 200)      return fail("status != 200");
    if (strstr(st.body, "hello h2 e2e") == NULL) return fail("body mismatch");

    printf("nghttp2 e2e OK: GET %s/hello -> %d (%zu bytes: %s)\n",
           url, st.status, st.body_len, st.body);
    return 0;
}
