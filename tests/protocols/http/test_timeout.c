#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"
#include <string.h>
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
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

static void server_thread(void *arg) {
    (void)arg;
    kl_http_server_run(&timeout_server);
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

/* Read with timeout (poll-based, avoids blocking forever if server
 * doesn't send anything before test timeout) */
static kl_ssize_t read_with_timeout(int fd, char *buf, size_t buflen, int ms) {
    int r = kl_test_poll1(fd, 0, ms);
    if (r <= 0) return r; /* 0 = timeout, -1 = error */

    return kl_test_sockread(fd, buf, buflen);
}

/* Wait for server to bind (max 2s) */
static void wait_for_bind(KlHttpServer *s) {
    for (int i = 0; i < 200 && kl_http_server_bound_port(s) == 0; i++) kl_test_sleep_ms(10);
}

UTEST(timeout, idle_connection) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = kl_http_server_bound_port(&timeout_server);

    /* Connect but send nothing */
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Wait for 408 */
    char buf[4096];
    kl_ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, partial_headers) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = kl_http_server_bound_port(&timeout_server);

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Send partial request line, then stall */
    const char *partial = "GET /ok HTT";
    (void)kl_test_sockwrite(fd, partial, strlen(partial));

    char buf[4096];
    kl_ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    kl_plat_thread_join(&tid);
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

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = kl_http_server_bound_port(&timeout_server);

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
    kl_ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, active_not_affected) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = kl_http_server_bound_port(&timeout_server);

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Send complete request promptly */
    const char *req = "GET /ok HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)kl_test_sockwrite(fd, req, strlen(req));

    char buf[4096];
    kl_ssize_t total = 0;
    kl_ssize_t n;
    while ((n = read_with_timeout(fd, buf + total,
                                  sizeof(buf) - (size_t)total - 1, 2000)) > 0) {
        total += n;
    }
    buf[total] = '\0';

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    kl_plat_thread_join(&tid);
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

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = kl_http_server_bound_port(&timeout_server);

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
    kl_ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 3000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    kl_plat_thread_join(&tid);
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

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = kl_http_server_bound_port(&timeout_server);

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
    kl_ssize_t total = 0;
    kl_ssize_t n;
    while ((n = read_with_timeout(fd, buf + total,
                                   sizeof(buf) - (size_t)total - 1, 3000)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_test_closesock(fd);
    kl_http_server_stop(&timeout_server);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, keepalive_idle_timeout) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = kl_http_server_bound_port(&timeout_server);

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Send first request (keepalive) */
    const char *req1 = "GET /ok HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    (void)kl_test_sockwrite(fd, req1, strlen(req1));

    /* Read first response */
    char buf[4096];
    kl_ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
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
    kl_plat_thread_join(&tid);
    kl_http_server_free(&timeout_server);
}

UTEST(timeout, concurrent_timeouts) {
    KlHttpServerConfig cfg = {
        .port = 0,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_http_server_init(&timeout_server, &cfg), 0);
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread, NULL);
    wait_for_bind(&timeout_server);
    int port = kl_http_server_bound_port(&timeout_server);

    /* Connect two idle clients */
    int fd1 = connect_to(port);
    int fd2 = connect_to(port);
    ASSERT_TRUE(fd1 >= 0);
    ASSERT_TRUE(fd2 >= 0);

    /* Both should timeout independently with 408 */
    char buf1[4096], buf2[4096];
    kl_ssize_t n1 = read_with_timeout(fd1, buf1, sizeof(buf1) - 1, 2000);
    kl_ssize_t n2 = read_with_timeout(fd2, buf2, sizeof(buf2) - 1, 2000);

    ASSERT_TRUE(n1 > 0);
    buf1[n1] = '\0';
    ASSERT_TRUE(strstr(buf1, "408") != NULL);

    ASSERT_TRUE(n2 > 0);
    buf2[n2] = '\0';
    ASSERT_TRUE(strstr(buf2, "408") != NULL);

    kl_test_closesock(fd1);
    kl_test_closesock(fd2);
    kl_http_server_stop(&timeout_server);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&timeout_server);
}

/* ═══════════════════════════════════════════════════════════════════
 * Header deadline (slow-drip request heads)
 *
 * read_timeout_ms is an inactivity timeout: every fragment refreshes it,
 * so a head dripped a byte at a time never trips it. The header deadline
 * (header_timeout_ms, 0 = read_timeout_ms) is absolute from the head's
 * first bytes, restarts with each keep-alive request, and stops once the
 * head is parsed.
 * ═══════════════════════════════════════════════════════════════════ */

static void start_server(KlHttpServerConfig *cfg, KlPlatThread *tid, int *port) {
    *port = 0;
    if (kl_http_server_init(&timeout_server, cfg) != 0) return;
    kl_http_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);
    kl_http_server_route(&timeout_server, "POST", "/echo", handle_ok, NULL, kl_http_body_reader_buffer);
    kl_plat_thread_create(tid, server_thread, NULL);
    wait_for_bind(&timeout_server);
    *port = kl_http_server_bound_port(&timeout_server);
}

static void stop_server(KlPlatThread *tid) {
    kl_http_server_stop(&timeout_server);
    kl_plat_thread_join(tid);
    kl_http_server_free(&timeout_server);
}

/* Send `s` in `frags` roughly equal fragments, `gap_ms` apart. Returns -1 as soon as
 * the server has answered (or closed) before the last fragment, else 0. */
static int send_fragmented(int fd, const char *s, int frags, int gap_ms) {
    size_t len = strlen(s), off = 0;
    for (int i = 0; i < frags; i++) {
        size_t end = (i == frags - 1) ? len : len * (size_t)(i + 1) / (size_t)frags;
        if (end > off && kl_test_sockwrite(fd, s + off, end - off) != (long)(end - off)) return -1;
        off = end;
        if (i < frags - 1 && kl_test_poll1(fd, 0, gap_ms) != 0) return -1;   /* waits gap_ms */
    }
    return 0;
}

/* Read one response (up to its blank line + a short body) into buf. */
static kl_ssize_t read_response(int fd, char *buf, size_t cap, int ms) {
    kl_ssize_t total = 0, n;
    while ((size_t)total < cap - 1 &&
           (n = read_with_timeout(fd, buf + total, cap - (size_t)total - 1, ms)) > 0) {
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "{\"ok\":true}") || strstr(buf, "Content-Length: 0\r\n\r\n")) break;
    }
    buf[total] = '\0';
    return total;
}

UTEST(timeout, header_slow_drip_gets_408_at_deadline) {
    /* Bytes 150 ms apart never trip the 1000 ms idle timeout; the header deadline,
     * defaulting to read_timeout_ms, ends the request ~1 s after its first byte. */
    KlHttpServerConfig cfg = { .port = 0, .read_timeout_ms = 1000 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /ok HTTP/1.1\r\nHost: localhost\r\nX-Slow: dripdripdripdrip\r\n\r\n";
    uint64_t t0 = kl_monotonic_ms();
    int answered_early = 0;
    for (size_t i = 0; i < strlen(req); i++) {
        if (kl_test_poll1(fd, 0, 150) != 0) { answered_early = 1; break; }
        if (kl_test_sockwrite(fd, req + i, 1) != 1) { answered_early = 1; break; }
    }
    char buf[4096];
    kl_ssize_t n = read_response(fd, buf, sizeof(buf), 3000);
    uint64_t elapsed = kl_monotonic_ms() - t0;
    EXPECT_TRUE(answered_early);              /* cut off before the head was complete */
    EXPECT_TRUE(n > 0 && strstr(buf, "408") != NULL);
    EXPECT_TRUE(elapsed < 1000 + 1000 + 2500);  /* deadline + one sweep + scheduling slack */

    kl_test_closesock(fd);
    stop_server(&tid);
}

UTEST(timeout, header_explicit_deadline_shorter_than_idle) {
    /* header_timeout_ms is independent of read_timeout_ms: a 600 ms head deadline under
     * a 5 s idle timeout cuts the drip off long before the idle timeout could. */
    KlHttpServerConfig cfg = { .port = 0, .read_timeout_ms = 5000, .header_timeout_ms = 600 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /ok HTTP/1.1\r\nHost: localhost\r\nX-Slow: dripdripdripdrip\r\n\r\n";
    uint64_t t0 = kl_monotonic_ms();
    for (size_t i = 0; i < strlen(req); i++) {
        if (kl_test_poll1(fd, 0, 100) != 0) break;
        if (kl_test_sockwrite(fd, req + i, 1) != 1) break;
    }
    char buf[4096];
    kl_ssize_t n = read_response(fd, buf, sizeof(buf), 3000);
    uint64_t elapsed = kl_monotonic_ms() - t0;
    EXPECT_TRUE(n > 0 && strstr(buf, "408") != NULL);
    EXPECT_TRUE(elapsed < 600 + 1000 + 2000);   /* the drip alone takes ~6 s */

    kl_test_closesock(fd);
    stop_server(&tid);
}

UTEST(timeout, header_fragmented_within_deadline_succeeds) {
    /* An ordinary head in five fragments over ~1.6 s, inside a 2 s deadline (the
     * default: read_timeout_ms). Each gap is well inside the idle timeout. The sweep
     * runs as each fragment arrives, so it sees the head incomplete at ~1.2 s. */
    KlHttpServerConfig cfg = { .port = 0, .read_timeout_ms = 2000 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    ASSERT_EQ(send_fragmented(fd, "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                              5, 400), 0);
    char buf[4096];
    ASSERT_TRUE(read_response(fd, buf, sizeof(buf), 2000) > 0);
    EXPECT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_test_closesock(fd);
    stop_server(&tid);
}

UTEST(timeout, header_deadline_restarts_per_keepalive_request) {
    /* Two keep-alive requests, each head fragmented over ~900 ms of a 2000 ms deadline,
     * 600 ms apart. The second head is still incomplete at ~2.1 s after the first began
     * (the sweep runs as each fragment arrives), so a deadline carried over from the first
     * request would cut the second off. */
    KlHttpServerConfig cfg = { .port = 0, .read_timeout_ms = 2000 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /ok HTTP/1.1\r\nHost: localhost\r\nX-Pad: abcdefghijklmnop\r\n\r\n";
    char buf[4096];
    for (int r = 0; r < 2; r++) {
        ASSERT_EQ(send_fragmented(fd, req, 4, 300), 0);
        ASSERT_TRUE(read_response(fd, buf, sizeof(buf), 2000) > 0);
        EXPECT_TRUE(strstr(buf, "200 OK") != NULL);
        kl_test_sleep_ms(600);                  /* idle between requests, under read_timeout_ms */
    }

    kl_test_closesock(fd);
    stop_server(&tid);
}

UTEST(timeout, header_deadline_does_not_apply_to_body) {
    /* A 300 ms head deadline must not cut off a body that takes ~1.5 s under a 4 s body
     * deadline: the header deadline stops once the head is parsed. The head itself comes
     * in two fragments, so its deadline did start. */
    KlHttpServerConfig cfg = { .port = 0, .read_timeout_ms = 1000, .header_timeout_ms = 300,
                               .body_timeout_ms = 4000 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *head = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n"
                       "Connection: close\r\n\r\n";
    ASSERT_EQ(send_fragmented(fd, head, 2, 100), 0);
    ASSERT_EQ(send_fragmented(fd, "0123456789", 10, 150), 0);
    char buf[4096];
    ASSERT_TRUE(read_response(fd, buf, sizeof(buf), 2000) > 0);
    EXPECT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_test_closesock(fd);
    stop_server(&tid);
}

/* Drip `s` a byte every `gap_ms` until the server answers or closes; returns the elapsed ms
 * from the first byte to the answer (read into buf) or to the end of the drip. */
static uint64_t drip_until_answered(int fd, const char *s, int gap_ms, char *buf, size_t cap,
                                    int *answered) {
    uint64_t t0 = kl_monotonic_ms();
    *answered = 0;
    for (size_t i = 0; i < strlen(s); i++) {
        if (kl_test_sockwrite(fd, s + i, 1) != 1) { *answered = 1; break; }
        if (kl_test_poll1(fd, 0, gap_ms) != 0) { *answered = 1; break; }
    }
    buf[0] = '\0';
    if (*answered) (void)read_response(fd, buf, cap, 2000);
    return kl_monotonic_ms() - t0;
}

static KlHttp2ServerSession *null_h2_factory(KlAllocator *alloc, KlHttp2ServerCallbacks *cb, void *ud) {
    (void)alloc; (void)cb; (void)ud;
    return NULL;   /* never reached: the preface is never completed */
}

UTEST(timeout, header_partial_h2_preface_drip_gets_408_at_deadline) {
    /* With HTTP/2 enabled a partial prior-knowledge preface is held before any parse. A
     * keep-alive request whose bytes are such a preface, dripped, still meets the deadline:
     * it starts when the bytes arrive, not when they are first parsed. */
    KlHttp2ServerConfig h2 = { .factory = null_h2_factory };
    KlHttpServerConfig cfg = { .port = 0, .read_timeout_ms = 1000, .h2 = &h2 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);
    char buf[4096];
    const char *req = "GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(kl_test_sockwrite(fd, req, strlen(req)), (long)strlen(req));
    ASSERT_TRUE(read_response(fd, buf, sizeof buf, 2000) > 0);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    int answered = 0;
    uint64_t elapsed = drip_until_answered(fd, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r", 300, buf, sizeof buf,
                                           &answered);
    EXPECT_TRUE(answered);
    /* Deadline + up to a second of sweep latency + slack; the drip alone takes ~6.9 s. */
    EXPECT_TRUE(elapsed < 1000 + 1000 + 2500);
    EXPECT_TRUE(buf[0] == '\0' || strstr(buf, "408") != NULL);

    kl_test_closesock(fd);
    stop_server(&tid);
}

UTEST(timeout, header_lone_crlf_does_not_start_the_deadline) {
    /* RFC 9112 2.2: an empty line before a request is ignored, so it must not start the next
     * request's deadline on an idle keep-alive connection (the idle timeout still applies). */
    KlHttpServerConfig cfg = { .port = 0, .read_timeout_ms = 4000, .header_timeout_ms = 600 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);
    char buf[4096];
    const char *req = "GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(kl_test_sockwrite(fd, req, strlen(req)), (long)strlen(req));
    ASSERT_TRUE(read_response(fd, buf, sizeof buf, 2000) > 0);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    ASSERT_EQ(kl_test_sockwrite(fd, "\r\n", 2), 2L);
    /* Longer than the deadline plus a sweep: a started deadline would have been enforced. */
    EXPECT_EQ(kl_test_poll1(fd, 0, 2000), 0);
    ASSERT_EQ(kl_test_sockwrite(fd, req, strlen(req)), (long)strlen(req));
    ASSERT_TRUE(read_response(fd, buf, sizeof buf, 2000) > 0);
    EXPECT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_test_closesock(fd);
    stop_server(&tid);
}

UTEST(timeout, header_two_empty_lines_start_the_deadline) {
    /* Only one empty line is ignored: a second one is the start of a request, so a client that
     * dripped empty lines would meet the deadline. */
    KlHttpServerConfig cfg = { .port = 0, .read_timeout_ms = 4000, .header_timeout_ms = 600 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);
    char buf[4096];
    const char *req = "GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(kl_test_sockwrite(fd, req, strlen(req)), (long)strlen(req));
    ASSERT_TRUE(read_response(fd, buf, sizeof buf, 2000) > 0);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    ASSERT_EQ(kl_test_sockwrite(fd, "\n\n", 2), 2L);
    /* Answered (408) or closed within the deadline plus a sweep and slack. */
    EXPECT_TRUE(kl_test_poll1(fd, 0, 600 + 1000 + 2000) > 0);

    kl_test_closesock(fd);
    stop_server(&tid);
}

UTEST(timeout, header_deadline_is_per_connection_not_per_slot) {
    /* One slot, reused: a connection that started a head and went away must not leave its
     * start time to the next connection in the slot. */
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 1, .read_timeout_ms = 4000,
                               .header_timeout_ms = 600 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, "GET /ok HT", 10);
    kl_test_sleep_ms(100);
    kl_test_closesock(fd);
    kl_test_sleep_ms(1500);                     /* the slot is free again, its start long past */

    fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(send_fragmented(fd, "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                              3, 150), 0);
    char buf[4096];
    ASSERT_TRUE(read_response(fd, buf, sizeof buf, 2000) > 0);
    EXPECT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_test_closesock(fd);
    stop_server(&tid);
}

UTEST(timeout, header_deadline_disabled_by_a_negative_value) {
    /* header_timeout_ms < 0 restores the idle timeout alone: a drip inside it is served. */
    KlHttpServerConfig cfg = { .port = 0, .read_timeout_ms = 1000, .header_timeout_ms = -1 };
    KlPlatThread tid; int port;
    start_server(&cfg, &tid, &port);
    ASSERT_TRUE(port > 0);
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(send_fragmented(fd, "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                              8, 300), 0);    /* ~2.1 s in all, 300 ms gaps */
    char buf[4096];
    ASSERT_TRUE(read_response(fd, buf, sizeof buf, 2000) > 0);
    EXPECT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_test_closesock(fd);
    stop_server(&tid);
}

UTEST_MAIN();
