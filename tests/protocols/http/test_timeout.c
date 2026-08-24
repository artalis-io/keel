#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"
#include <string.h>
#include <pthread.h>
#include <errno.h>

/* Short timeout for fast tests */
#define TEST_TIMEOUT_MS 200

static void handle_ok(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

/* Static server shared by server_thread. Each test uses a unique port
 * and does init/stop/free, so sequential utest execution is safe. */
static KlHttpServer timeout_server;

static void *server_thread(void *arg) {
    (void)arg;
    kl_http_server_run(&timeout_server);
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

/* Read with timeout (poll-based, avoids blocking forever if server
 * doesn't send anything before test timeout) */
static ssize_t read_with_timeout(int fd, char *buf, size_t buflen, int ms) {
    int r = kl_test_poll1(fd, 0, ms);
    if (r <= 0) return r; /* 0 = timeout, -1 = error */

    return kl_test_sockread(fd, buf, buflen);
}

/* Wait for server to bind (max 2s) */
static void wait_for_bind(KlHttpServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

UTEST(timeout, idle_connection) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = timeout_server.bound_port;

    /* Connect but send nothing */
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Wait for 408 */
    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, partial_headers) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = timeout_server.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Send partial request line, then stall */
    const char *partial = "GET /ok HTT";
    (void)kl_test_sockwrite(fd, partial, strlen(partial));

    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, partial_body) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "POST", "/echo", handle_ok,
                    NULL, kl_http_body_reader_buffer);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = timeout_server.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Send headers + partial body, then stall */
    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 100\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "partial";
    (void)kl_test_sockwrite(fd, req, strlen(req));

    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, active_not_affected) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = timeout_server.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Send complete request promptly */
    const char *req = "GET /ok HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)kl_test_sockwrite(fd, req, strlen(req));

    char buf[4096];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read_with_timeout(fd, buf + total,
                                  sizeof(buf) - (size_t)total - 1, 2000)) > 0) {
        total += n;
    }
    buf[total] = '\0';

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_http_server_free(&timeout_server);
}

/* ═══════════════════════════════════════════════════════════════════
 * Additional timeout tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(timeout, body_timeout) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = 2000,     /* generous header timeout */
        .body_timeout_ms = TEST_TIMEOUT_MS, /* tight body timeout */
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "POST", "/echo", handle_ok,
                    NULL, kl_http_body_reader_buffer);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = timeout_server.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Send headers with large content-length, then trickle body */
    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 1000\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "x"; /* only 1 byte of 1000 */
    (void)kl_test_sockwrite(fd, req, strlen(req));

    /* Wait for timeout response */
    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 3000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, fast_large_body) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = 2000,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "POST", "/echo", handle_ok,
                    NULL, kl_http_body_reader_buffer);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = timeout_server.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Build a request with a 4KB body sent all at once */
    char body[4096];
    memset(body, 'A', sizeof(body));

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", sizeof(body));
    ASSERT_TRUE(hlen > 0);

    (void)kl_test_sockwrite(fd, header, (size_t)hlen);
    (void)kl_test_sockwrite(fd, body, sizeof(body));

    /* Should get 200 OK, not a timeout */
    char buf[4096];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read_with_timeout(fd, buf + total,
                                   sizeof(buf) - (size_t)total - 1, 3000)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, keepalive_idle_timeout) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = timeout_server.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Send first request (keepalive) */
    const char *req1 = "GET /ok HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    (void)kl_test_sockwrite(fd, req1, strlen(req1));

    /* Read first response */
    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Don't send second request; wait for idle timeout.
     * The connection should be closed with a 408 or just closed. */
    n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    /* n <= 0 means connection closed, or n > 0 with 408 */
    if (n > 0) {
        buf[n] = '\0';
        ASSERT_TRUE(strstr(buf, "408") != NULL);
    } else {
        /* Connection closed without response; also acceptable */
        ASSERT_TRUE(n <= 0);
    }

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, concurrent_timeouts) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = timeout_server.bound_port;

    /* Connect two idle clients */
    int fd1 = connect_to(port);
    int fd2 = connect_to(port);
    ASSERT_TRUE(fd1 >= 0);
    ASSERT_TRUE(fd2 >= 0);

    /* Both should timeout independently with 408 */
    char buf1[4096], buf2[4096];
    ssize_t n1 = read_with_timeout(fd1, buf1, sizeof(buf1) - 1, 2000);
    ssize_t n2 = read_with_timeout(fd2, buf2, sizeof(buf2) - 1, 2000);

    ASSERT_TRUE(n1 > 0);
    buf1[n1] = '\0';
    ASSERT_TRUE(strstr(buf1, "408") != NULL);

    ASSERT_TRUE(n2 > 0);
    buf2[n2] = '\0';
    ASSERT_TRUE(strstr(buf2, "408") != NULL);

    kl_test_closesock(fd1);
    kl_test_closesock(fd2);
    kl_http_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_http_server_free(&timeout_server);
}

UTEST_MAIN();
