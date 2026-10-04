/*
 * e2e_idle.c: an idle HTTP/2 connection is timed out (eighteenth audit, W3).
 *
 * The idle sweep skipped HTTP/2 connections entirely ("PING keepalive is the session's
 * responsibility"), and the nghttp2 session has no idle handling, so a client that opened an HTTP/2
 * connection and then said nothing held its slot forever: a few hundred such sockets fill
 * max_connections. Here a KEEL HTTP/2 client makes one request to a KEEL server with
 * read_timeout_ms = 300, gets its answer, and then stays connected and silent: the server must close
 * the connection within a few seconds. Exits non-zero otherwise.
 */
#include <keel/keel.h>
#include "keel_http2_nghttp2.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static int fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); return 1; }

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{}", 2);
}

static KlHttpServer g_srv;
static void *srv_thread(void *a) { (void)a; kl_http_server_run(&g_srv); return NULL; }

static int g_answered, g_closed;
static void on_response(KlHttp2ClientConn *c, int32_t sid, const KlHttp2ClientResponse *r, void *ud) {
    (void)c; (void)sid; (void)r; (void)ud;
    g_answered = 1;
}
static void on_error(KlHttp2ClientConn *c, const char *msg, void *ud) {
    (void)c; (void)msg; (void)ud;
    g_closed = 1;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlHttp2ServerConfig h2cfg = { .factory = kl_http2_nghttp2_server_session };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &h2cfg,
                               .read_timeout_ms = 300 };
    if (kl_http_server_init(&g_srv, &cfg) < 0) return fail("server init");
    kl_http_server_route(&g_srv, "GET", "/hello", handle_hello, NULL, NULL);
    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_thread, NULL) != 0) return fail("server thread");
    for (int i = 0; i < 200 && g_srv.bound_port == 0; i++) nap(5);
    if (g_srv.bound_port <= 0) return fail("server never bound");

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", g_srv.bound_port);
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) return fail("event ctx");
    KlHttp2ClientConfig ccfg;
    memset(&ccfg, 0, sizeof ccfg);
    ccfg.session = kl_http2_nghttp2_client_session;
    KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &alloc, &ccfg, url, on_error, NULL);
    if (!c) return fail("client connect");
    for (int i = 0; i < 200 && kl_http2_client_request(c, "GET", "/hello", NULL, 0, NULL, 0,
                                                       on_response, NULL) < 0; i++)
        (void)kl_event_ctx_run(&ev, 16, 10);            /* until connected */
    for (int i = 0; i < 300 && !g_answered && !g_closed; i++)
        (void)kl_event_ctx_run(&ev, 16, 10);
    int answered = g_answered, closed_early = g_closed;

    for (int i = 0; i < 400 && !g_closed; i++)          /* now idle: up to ~4 s */
        (void)kl_event_ctx_run(&ev, 16, 10);
    int closed = g_closed;

    kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    kl_http_server_stop(&g_srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_srv);
    if (!answered || closed_early) return fail("the request before going idle was not answered");
    if (!closed) return fail("an idle HTTP/2 connection was never timed out");
    printf("nghttp2 idle connection timed out OK\n");
    return 0;
}
