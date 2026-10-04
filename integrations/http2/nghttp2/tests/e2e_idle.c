/*
 * e2e_idle.c: an idle HTTP/2 connection is timed out (eighteenth audit, W3).
 *
 * The idle sweep skipped HTTP/2 connections entirely ("PING keepalive is the session's
 * responsibility"), and the nghttp2 session has no idle handling, so a client that opened an HTTP/2
 * connection and then said nothing held its slot forever: a few hundred such sockets fill
 * max_connections. Here a KEEL HTTP/2 client makes one request to a KEEL server with
 * read_timeout_ms = 300, gets its answer, and then stays connected and silent: the server must close
 * the connection within a few seconds. Exits non-zero otherwise.
 *
 * Two more cases drive a raw nghttp2 client over a plain socket, to see the frames:
 *   - the idle timeout sends a GOAWAY before it closes (RFC 9113 6.8), not a bare close;
 *   - a graceful drain closes a connection as soon as its session is done (GOAWAY out, no stream
 *     left), rather than at the idle timeout or the drain deadline.
 */
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <keel/keel.h>
#include "keel_http2_nghttp2.h"
#include <nghttp2/nghttp2.h>

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

static int case_idle_timeout(void) {
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

/* ── A raw nghttp2 client over a blocking socket ─────────────────────────────────────────────── */

#if defined(_WIN32)
typedef SOCKET RawSock;
#define RAW_BAD INVALID_SOCKET
static void raw_close(RawSock fd) { closesocket(fd); }
static int raw_poll(RawSock fd, int ms) {
    WSAPOLLFD p = { fd, POLLRDNORM, 0 };
    return WSAPoll(&p, 1, ms);
}
#else
typedef int RawSock;
#define RAW_BAD (-1)
static void raw_close(RawSock fd) { close(fd); }
static int raw_poll(RawSock fd, int ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    return poll(&p, 1, ms);
}
#endif

typedef struct {
    RawSock fd;
    nghttp2_session *ng;
    int answered, goaway, eof;
    uint64_t eof_ms;
} RawClient;

static ssize_t raw_send_cb(nghttp2_session *ng, const uint8_t *d, size_t n, int fl, void *ud) {
    (void)ng; (void)fl;
    RawClient *c = ud;
    int w = (int)send(c->fd, (const char *)d, (int)n, 0);
    return w > 0 ? (ssize_t)w : NGHTTP2_ERR_CALLBACK_FAILURE;
}
static int raw_frame_recv_cb(nghttp2_session *ng, const nghttp2_frame *f, void *ud) {
    (void)ng;
    RawClient *c = ud;
    if (f->hd.type == NGHTTP2_GOAWAY) c->goaway = 1;
    if ((f->hd.type == NGHTTP2_HEADERS || f->hd.type == NGHTTP2_DATA) &&
        (f->hd.flags & NGHTTP2_FLAG_END_STREAM))
        c->answered++;
    return 0;
}

static int raw_connect(RawClient *c, int port) {
    memset(c, 0, sizeof *c);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd == RAW_BAD) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0x7f000001u);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) != 0) { raw_close(c->fd); return -1; }
    nghttp2_session_callbacks *cbs = NULL;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback(cbs, raw_send_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, raw_frame_recv_cb);
    nghttp2_session_client_new(&c->ng, cbs, c);
    nghttp2_session_callbacks_del(cbs);
    nghttp2_submit_settings(c->ng, NGHTTP2_FLAG_NONE, NULL, 0);
    return 0;
}

/* Send what is queued, then wait up to ms for the server and take in what it sent. */
static void raw_pump(RawClient *c, int ms) {
    if (c->eof) return;
    (void)nghttp2_session_send(c->ng);
    if (raw_poll(c->fd, ms) <= 0) return;
    char buf[16384];
    int n = (int)recv(c->fd, buf, (int)sizeof buf, 0);
    if (n <= 0) { c->eof = 1; c->eof_ms = kl_monotonic_ms(); return; }
    (void)nghttp2_session_mem_recv(c->ng, (const uint8_t *)buf, (size_t)n);
}

static int raw_get(RawClient *c, const char *path) {
    nghttp2_nv nva[] = {
        { (uint8_t *)":method", (uint8_t *)"GET", 7, 3, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":path", (uint8_t *)path, 5, strlen(path), NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":scheme", (uint8_t *)"http", 7, 4, NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)":authority", (uint8_t *)"x", 10, 1, NGHTTP2_NV_FLAG_NONE },
    };
    int before = c->answered;
    if (nghttp2_submit_request(c->ng, NULL, nva, 4, NULL, NULL) < 0) return -1;
    for (int i = 0; i < 200 && c->answered == before && !c->eof; i++) raw_pump(c, 10);
    return c->answered > before ? 0 : -1;
}

static void raw_free(RawClient *c) {
    if (c->ng) nghttp2_session_del(c->ng);
    raw_close(c->fd);
}

/* The idle timeout closes an HTTP/2 connection with a GOAWAY first. */
static KlHttpServer g_gsrv;
static void *gsrv_thread(void *a) { (void)a; kl_http_server_run(&g_gsrv); return NULL; }

static int case_idle_goaway(void) {
    KlHttp2ServerConfig h2cfg = { .factory = kl_http2_nghttp2_server_session };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &h2cfg,
                               .read_timeout_ms = 300 };
    if (kl_http_server_init(&g_gsrv, &cfg) < 0) return fail("server init");
    kl_http_server_route(&g_gsrv, "GET", "/hello", handle_hello, NULL, NULL);
    pthread_t tid;
    if (pthread_create(&tid, NULL, gsrv_thread, NULL) != 0) return fail("server thread");
    for (int i = 0; i < 200 && g_gsrv.bound_port == 0; i++) nap(5);

    RawClient c;
    int ok = raw_connect(&c, g_gsrv.bound_port) == 0 && raw_get(&c, "/hello") == 0;
    for (int i = 0; ok && i < 400 && !c.eof; i++) raw_pump(&c, 10);   /* idle: up to ~4 s */
    int eof = c.eof, goaway = c.goaway;
    if (ok || c.ng) raw_free(&c);

    kl_http_server_stop(&g_gsrv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_gsrv);
    printf("  idle close: eof %d, GOAWAY first %d\n", eof, goaway);
    if (!ok) return fail("the request before going idle was not answered");
    if (!eof) return fail("an idle HTTP/2 connection was never timed out");
    if (!goaway) return fail("the idle timeout closed the connection without a GOAWAY");
    printf("nghttp2 idle close sends GOAWAY OK\n");
    return 0;
}

/* A graceful drain: the GOAWAY goes out, the session has no stream left, so it is done, and the
 * connection closes then, well before the idle timeout and the drain deadline (both 5 s here). */
static KlHttpServer g_dsrv;
static void *dsrv_thread(void *a) { (void)a; kl_http_server_run(&g_dsrv); return NULL; }

static int case_drain_closes_a_done_session(void) {
    KlHttp2ServerConfig h2cfg = { .factory = kl_http2_nghttp2_server_session };
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .h2 = &h2cfg,
                               .read_timeout_ms = 5000, .drain_timeout_ms = 5000 };
    if (kl_http_server_init(&g_dsrv, &cfg) < 0) return fail("server init");
    kl_http_server_route(&g_dsrv, "GET", "/hello", handle_hello, NULL, NULL);
    pthread_t tid;
    if (pthread_create(&tid, NULL, dsrv_thread, NULL) != 0) return fail("server thread");
    for (int i = 0; i < 200 && g_dsrv.bound_port == 0; i++) nap(5);

    RawClient c;
    int ok = raw_connect(&c, g_dsrv.bound_port) == 0 && raw_get(&c, "/hello") == 0;
    uint64_t t0 = kl_monotonic_ms();
    kl_http_server_stop(&g_dsrv);                          /* drain: GOAWAY, then wait for streams */
    for (int i = 0; ok && i < 700 && !c.eof; i++) raw_pump(&c, 10);   /* up to ~7 s */
    int eof = c.eof, goaway = c.goaway;
    uint64_t took = eof ? c.eof_ms - t0 : 0;
    if (ok || c.ng) raw_free(&c);

    pthread_join(tid, NULL);
    kl_http_server_free(&g_dsrv);
    printf("  drain: GOAWAY %d, closed %d after %llu ms\n", goaway, eof, (unsigned long long)took);
    if (!ok) return fail("the request before the drain was not answered");
    if (!goaway) return fail("the drain sent no GOAWAY");
    if (!eof || took > 2500)
        return fail("a session done after GOAWAY was held until a timeout or the drain deadline");
    printf("nghttp2 drain closes a done session OK\n");
    return 0;
}

int main(void) {
    int failed = 0;                        /* every case runs, so each one's result is seen */
    failed += case_idle_timeout();
    failed += case_idle_goaway();
    failed += case_drain_closes_a_done_session();
    if (failed) { fprintf(stderr, "FAIL: %d case(s)\n", failed); return 1; }
    return 0;
}
