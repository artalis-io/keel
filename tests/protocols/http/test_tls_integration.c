#include "utest.h"
#include <keel/keel.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"   /* shared identity TLS mock: completion-capable (feed_input/drain_output) */
#include <string.h>
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include <errno.h>

/* The passthrough TLS mock (identity, no crypto) now lives in tests/mock_tls.h and implements
 * the completion-mode ops too (feed_input/drain_output), so this suite runs over the completion
 * backend as well as readiness. The factory is mock_tls_create. */

/* ── Helpers ────────────────────────────────────────────────────────── */

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static void wait_for_bind(KlHttpServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) kl_test_sleep_ms(10);
}

static void server_thread_fn(void *arg) {
    kl_http_server_run((KlHttpServer *)arg);
}

static int connect_to(int port) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kl_test_closesock(fd);
        return -1;
    }
    return fd;
}

static kl_ssize_t read_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    kl_ssize_t total = 0;
    while (total < (kl_ssize_t)buflen - 1) {
        int pr = kl_test_poll1(fd, 0, timeout_ms);
        if (pr <= 0) break;
        kl_ssize_t n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

static kl_ssize_t read_one_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    kl_ssize_t total = 0;
    while (total < (kl_ssize_t)buflen - 1) {
        int pr = kl_test_poll1(fd, 0, timeout_ms);
        if (pr <= 0) break;
        kl_ssize_t n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            hdr_end += 4;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) {
                size_t cl_val = (size_t)strtol(cl + 15, NULL, 10);
                size_t body_rcvd = (size_t)(total - (hdr_end - buf));
                if (body_rcvd >= cl_val) break;
            }
        }
    }
    buf[total] = '\0';
    return total;
}

/* ═══════════════════════════════════════════════════════════════════
 * Tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(tls_integration, hello_request) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = mock_tls_create,
    };
    KlHttpServerConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlHttpServer srv;
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    /* Connect and send plain HTTP: passthrough TLS is transparent */
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    kl_test_sockwrite(fd, req, strlen(req));

    char buf[2048];
    read_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&srv);
}

UTEST(tls_integration, keep_alive) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = mock_tls_create,
    };
    KlHttpServerConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlHttpServer srv;
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &srv);
    wait_for_bind(&srv);
    int port = srv.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* First request (keep-alive) */
    const char *req1 = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    kl_test_sockwrite(fd, req1, strlen(req1));

    char buf[2048];
    read_one_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    /* Second request on same connection (tests TLS reset()) */
    const char *req2 = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    kl_test_sockwrite(fd, req2, strlen(req2));

    read_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&srv);
}

UTEST(tls_integration, concurrent) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = mock_tls_create,
    };
    KlHttpServerConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlHttpServer srv;
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &srv);
    wait_for_bind(&srv);
    int port = srv.bound_port;

    /* 3 concurrent TLS connections */
    int fds[3];
    for (int i = 0; i < 3; i++) {
        fds[i] = connect_to(port);
        ASSERT_TRUE(fds[i] >= 0);
    }

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    for (int i = 0; i < 3; i++) {
        kl_test_sockwrite(fds[i], req, strlen(req));
    }

    for (int i = 0; i < 3; i++) {
        char buf[2048];
        read_response(fds[i], buf, sizeof(buf), 2000);
        ASSERT_TRUE_MSG(strstr(buf, "200 OK") != NULL,
                         "TLS connection %d should get 200 OK");
        ASSERT_TRUE_MSG(strstr(buf, "{\"ok\":true}") != NULL,
                         "TLS connection %d should have correct body");
        kl_test_closesock(fds[i]);
    }

    kl_http_server_stop(&srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&srv);
}

/* ── A TLS record split across reads ────────────────────────────────────
 * On a real network a TLS record often spans several TCP segments, so a readable event can arrive
 * with only part of a record: the engine takes the bytes and read() returns 0, WANT_READ. That is
 * not end of stream (the KlTls contract: 0 = WANT_READ, -1 = error or closed). The server must
 * wait for the rest, in the header phase and in the body phase. mock_tls_split_record simulates
 * the split (socket mode only, so this exercises the readiness transport). */

static void handle_echo_body(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    kl_http_response_status(res, 200);
    if (br && br->len > 0)
        (void)kl_http_response_body_copy(res, br->data, br->len);
}

static KlHttpServer split_srv;

/* Send `req` in two writes split at `cut`, with the server told the record ends at the end of
 * the request; returns the response. */
static void split_round_trip(int port, const char *req, size_t cut, char *buf, size_t cap) {
    buf[0] = '\0';
    int fd = connect_to(port);
    if (fd < 0) return;
    (void)kl_test_sockwrite(fd, req, cut);
    kl_test_sleep_ms(150);                    /* the server sees the first part on its own */
    (void)kl_test_sockwrite(fd, req + cut, strlen(req) - cut);
    read_response(fd, buf, cap, 3000);
    kl_test_closesock(fd);
}

UTEST(tls_integration, record_split_in_headers_waits_for_the_rest) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg };
    ASSERT_EQ(0, kl_http_server_init(&split_srv, &cfg));
    kl_http_server_route(&split_srv, "GET", "/hello", handle_hello, NULL, NULL);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &split_srv);
    wait_for_bind(&split_srv);

    static char buf[2048];
    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    mock_tls_split_after = 0;
    mock_tls_split_record = strlen(req);
    split_round_trip(split_srv.bound_port, req, 20, buf, sizeof buf);
    mock_tls_split_record = 0;

    kl_http_server_stop(&split_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&split_srv);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
}

UTEST(tls_integration, record_split_in_body_waits_for_the_rest) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg };
    ASSERT_EQ(0, kl_http_server_init(&split_srv, &cfg));
    kl_http_server_route(&split_srv, "POST", "/echo", handle_echo_body,
                         (void *)(size_t)(64 * 1024), kl_http_body_reader_buffer);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &split_srv);
    wait_for_bind(&split_srv);

    static char buf[2048];
    const char *hdrs = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n"
                       "Connection: close\r\n\r\n";
    static char req[256];
    snprintf(req, sizeof req, "%s0123456789", hdrs);
    mock_tls_split_after = strlen(hdrs);       /* the headers arrive whole; the body record splits */
    mock_tls_split_record = 10;
    /* headers alone first, then half the body, then the rest */
    int fd = connect_to(split_srv.bound_port);
    buf[0] = '\0';
    if (fd >= 0) {
        (void)kl_test_sockwrite(fd, req, strlen(hdrs));
        kl_test_sleep_ms(150);
        (void)kl_test_sockwrite(fd, req + strlen(hdrs), 4);
        kl_test_sleep_ms(150);
        (void)kl_test_sockwrite(fd, req + strlen(hdrs) + 4, 6);
        read_response(fd, buf, sizeof buf, 3000);
        kl_test_closesock(fd);
    }
    mock_tls_split_record = 0;
    mock_tls_split_after = 0;

    kl_http_server_stop(&split_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&split_srv);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "0123456789") != NULL);
}

/* A paused body stays paused over TLS. The completion TLS drive posted the next receive whatever the
 * pause state, so kl_http_request_pause_body had no effect on HTTPS uploads on IOCP or io_uring:
 * every later chunk still reached on_data. */
typedef struct { KlHttpBodyReader base; KlAllocator *alloc; const KlHttpRequest *req; } TlsPauseReader;
static int g_tls_pause_calls;
static int tls_pause_on_data(KlHttpBodyReader *self, const char *data, size_t len) {
    (void)data; (void)len;
    g_tls_pause_calls++;
    kl_http_request_pause_body(((TlsPauseReader *)self)->req);
    return 0;
}
static void tls_pause_noop(KlHttpBodyReader *self) { (void)self; }
static void tls_pause_destroy(KlHttpBodyReader *self) {
    TlsPauseReader *r = (TlsPauseReader *)self;
    kl_free(r->alloc, r, sizeof(*r));
}
static KlHttpBodyReader *tls_pause_factory(KlAllocator *alloc, const KlHttpRequest *req, void *ud) {
    (void)ud;
    TlsPauseReader *r = kl_malloc(alloc, sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->base.on_data = tls_pause_on_data;
    r->base.on_complete = tls_pause_noop;
    r->base.on_error = tls_pause_noop;
    r->base.destroy = tls_pause_destroy;
    r->alloc = alloc;
    r->req = req;
    return &r->base;
}
static void handle_never(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 200);
}
static KlHttpServer pause_tls_srv;

UTEST(tls_integration, paused_body_stays_paused) {
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpServerConfig cfg = { .port = 0, .tls = &tls_cfg, .body_timeout_ms = 10000 };
    ASSERT_EQ(0, kl_http_server_init(&pause_tls_srv, &cfg));
    kl_http_server_route(&pause_tls_srv, "POST", "/pause", handle_never, NULL, tls_pause_factory);
    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &pause_tls_srv);
    wait_for_bind(&pause_tls_srv);

    g_tls_pause_calls = 0;
    const char *h = "POST /pause HTTP/1.1\r\nHost: localhost\r\nContent-Length: 30\r\n\r\n";
    int fd = connect_to(pause_tls_srv.bound_port);
    if (fd >= 0) {
        (void)kl_test_sockwrite(fd, h, strlen(h));
        for (int i = 0; i < 3; i++) {                      /* three separate chunks */
            kl_test_sleep_ms(150);
            (void)kl_test_sockwrite(fd, "0123456789", 10);
        }
        kl_test_sleep_ms(300);                             /* time for any wrongly read chunk */
    }
    kl_http_server_stop(&pause_tls_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&pause_tls_srv);
    if (fd >= 0) kl_test_closesock(fd);
    ASSERT_EQ(g_tls_pause_calls, 1);                       /* was (completion): 3, the pause ignored */
}

UTEST_MAIN();
