#include "utest.h"
#include <keel/keel.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"   /* shared identity TLS mock: completion-capable (feed_input/drain_output) */
#include <string.h>
#include <pthread.h>
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
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

static void *server_thread_fn(void *arg) {
    kl_http_server_run((KlHttpServer *)arg);
    return NULL;
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
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

static ssize_t read_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    ssize_t total = 0;
    while (total < (ssize_t)buflen - 1) {
        int pr = kl_test_poll1(fd, 0, timeout_ms);
        if (pr <= 0) break;
        ssize_t n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

static ssize_t read_one_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    ssize_t total = 0;
    while (total < (ssize_t)buflen - 1) {
        int pr = kl_test_poll1(fd, 0, timeout_ms);
        if (pr <= 0) break;
        ssize_t n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1);
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

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
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
    pthread_join(tid, NULL);
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

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
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
    pthread_join(tid, NULL);
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

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
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
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
}

UTEST_MAIN();
