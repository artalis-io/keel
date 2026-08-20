/*
 * test_read_flow_control.c — Phase 1 read-side body flow control (kl_http_request_pause_body /
 * kl_http_request_resume_body).
 *
 * A custom body reader pauses after the first chunk and schedules an async resume on a timer.
 * The client sends the body in two halves with a gap, so the server MUST stop reading after the
 * first half (pause), then continue after the timer resumes (READ re-armed / recv re-posted).
 * Verifies: no data lost, on_complete fires, and the pause actually took effect (reading stopped
 * until the timer resumed it). Model-independent — the same test runs over readiness here and
 * over the completion backends via the parity fixture (Phase 3).
 */
#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"
#include <pthread.h>
#include <string.h>
#include <time.h>

#define RFC_PORT 18466
#define RFC_HALF 4096                 /* each half > one TCP segment, forces multi-read */
#define RFC_TOTAL (RFC_HALF * 2)

static void rfc_nap(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Results recorded by the reader (survive its destroy). */
static size_t g_total;
static int    g_calls;
static int    g_calls_at_pause;       /* on_data call count when the pause was issued */
static int    g_resumed;
static int    g_completed;

typedef struct {
    KlHttpBodyReader base;
    KlAllocator *alloc;
    KlHttpRequest   *req;
    int          paused;
} PauseReader;

static void rfc_resume(void *ud) {
    PauseReader *r = ud;
    g_resumed = 1;
    kl_http_request_resume_body(r->req);
}

static int rfc_on_data(KlHttpBodyReader *self, const char *data, size_t len) {
    (void)data;
    PauseReader *r = (PauseReader *)self;
    g_total += len;
    g_calls++;
    if (!r->paused) {                 /* pause on the first chunk, resume ~30ms later */
        r->paused = 1;
        g_calls_at_pause = g_calls;
        KlHttpConn *c = kl_http_request_conn(r->req);
        kl_http_request_pause_body(r->req);
        kl_timer_add(c->stream.ctx, 30, rfc_resume, r);
    }
    return 0;
}
static void rfc_on_complete(KlHttpBodyReader *self) { (void)self; g_completed = 1; }
static void rfc_on_error(KlHttpBodyReader *self) { (void)self; }
static void rfc_destroy(KlHttpBodyReader *self) {
    PauseReader *r = (PauseReader *)self;
    kl_free(r->alloc, r, sizeof(*r));
}

static KlHttpBodyReader *rfc_factory(KlAllocator *alloc, const KlHttpRequest *req, void *ud) {
    (void)ud;
    PauseReader *r = kl_malloc(alloc, sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc;
    r->req = (KlHttpRequest *)req;   /* factory param is const; req is mutable for pause/resume */
    r->base.on_data = rfc_on_data;
    r->base.on_complete = rfc_on_complete;
    r->base.on_error = rfc_on_error;
    r->base.destroy = rfc_destroy;
    return &r->base;
}

static void rfc_handler(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static KlHttpServer g_srv;
static void *rfc_srv_thread(void *a) { (void)a; kl_http_server_run(&g_srv); return NULL; }

static int rfc_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { kl_test_closesock(fd); return -1; }
    return fd;
}

UTEST(read_flow_control, pause_midbody_then_resume) {
    g_total = g_calls = g_calls_at_pause = g_resumed = g_completed = 0;

    KlHttpServerConfig cfg = { .port = RFC_PORT, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_http_server_init(&g_srv, &cfg));
    kl_http_server_route(&g_srv, "POST", "/p", rfc_handler, NULL, rfc_factory);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, rfc_srv_thread, NULL));
    for (int i = 0; i < 200 && g_srv.bound_port == 0; i++) rfc_nap(5);
    ASSERT_TRUE(g_srv.bound_port > 0);

    int fd = rfc_connect(g_srv.bound_port);
    ASSERT_TRUE(fd >= 0);

    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
                      "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n"
                      "Connection: close\r\n\r\n", RFC_TOTAL);
    ASSERT_EQ((long)hn, kl_test_sockwrite(fd, hdr, (size_t)hn));

    static char half[RFC_HALF];
    memset(half, 'A', sizeof(half));
    ASSERT_EQ((long)RFC_HALF, kl_test_sockwrite(fd, half, RFC_HALF));  /* first half → on_data → pause */

    /* Window: server reads the first half, pauses, then the 30ms timer resumes it. We haven't
     * sent the second half, so a correct pause means reading is stopped meanwhile. */
    rfc_nap(120);   /* > 30ms resume timer */
    ASSERT_TRUE(g_calls_at_pause >= 1);   /* paused after the first chunk(s) arrived */
    ASSERT_EQ(1, g_resumed);              /* timer resumed reading */
    ASSERT_EQ(0, g_completed);            /* body not finished — second half not sent yet */

    memset(half, 'B', sizeof(half));
    ASSERT_EQ((long)RFC_HALF, kl_test_sockwrite(fd, half, RFC_HALF));  /* second half → completes */

    /* Read the response (server closes after Connection: close). */
    char buf[512]; size_t got = 0;
    while (got < sizeof(buf) - 1) {
        if (kl_test_poll1(fd, 0, 2000) <= 0) break;
        long n = kl_test_sockread(fd, buf + got, sizeof(buf) - 1 - got);
        if (n <= 0) break;
        got += (size_t)n; buf[got] = 0;
        if (strstr(buf, "200 OK")) break;
    }
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    ASSERT_EQ((size_t)RFC_TOTAL, g_total);   /* no bytes lost across pause/resume */
    ASSERT_EQ(1, g_completed);               /* orderly finish after resume */

    kl_test_closesock(fd);
    kl_http_server_stop(&g_srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_srv);
}

/* A body reader that pauses on the first chunk and NEVER resumes. */
static int rfc_pause_forever_on_data(KlHttpBodyReader *self, const char *data, size_t len) {
    (void)data;
    PauseReader *r = (PauseReader *)self;
    g_total += len; g_calls++;
    if (!r->paused) { r->paused = 1; g_calls_at_pause = g_calls; kl_http_request_pause_body(r->req); }
    return 0;
}
static KlHttpBodyReader *rfc_pause_forever_factory(KlAllocator *alloc, const KlHttpRequest *req, void *ud) {
    (void)ud;
    PauseReader *r = kl_malloc(alloc, sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc; r->req = (KlHttpRequest *)req;
    r->base.on_data = rfc_pause_forever_on_data;
    r->base.on_complete = rfc_on_complete;
    r->base.on_error = rfc_on_error;
    r->base.destroy = rfc_destroy;
    return &r->base;
}

/* Shutdown-while-paused: a conn parked in read-paused (no outstanding op on completion / READ
 * interest off on readiness) must tear down cleanly on kl_http_server_stop — no hang, no leak, no
 * UAF (the ASan run is the oracle for the last two; join returning is the oracle for no-hang). */
UTEST(read_flow_control, shutdown_while_paused) {
    g_total = g_calls = g_calls_at_pause = g_resumed = g_completed = 0;

    KlHttpServerConfig cfg = { .port = RFC_PORT + 1, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_http_server_init(&g_srv, &cfg));
    kl_http_server_route(&g_srv, "POST", "/p", rfc_handler, NULL, rfc_pause_forever_factory);

    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, rfc_srv_thread, NULL));
    for (int i = 0; i < 200 && g_srv.bound_port == 0; i++) rfc_nap(5);
    ASSERT_TRUE(g_srv.bound_port > 0);

    int fd = rfc_connect(g_srv.bound_port);
    ASSERT_TRUE(fd >= 0);
    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
                      "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n", RFC_TOTAL);
    ASSERT_EQ((long)hn, kl_test_sockwrite(fd, hdr, (size_t)hn));
    static char half[RFC_HALF];
    memset(half, 'A', sizeof(half));
    ASSERT_EQ((long)RFC_HALF, kl_test_sockwrite(fd, half, RFC_HALF));  /* first half → pause, no resume */

    rfc_nap(80);
    ASSERT_TRUE(g_calls_at_pause >= 1);   /* it did pause */
    ASSERT_EQ(0, g_completed);            /* and never completed (no resume, no more reads) */

    /* Tear down with the conn still paused mid-body. join must return (no hang). */
    kl_test_closesock(fd);
    kl_http_server_stop(&g_srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_srv);
}

UTEST_MAIN();
