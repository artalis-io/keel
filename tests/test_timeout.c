#include "utest.h"
#include <keel/keel.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

/* Short timeout for fast tests */
#define TEST_TIMEOUT_MS 200
#define TEST_PORT       18090

static void handle_ok(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

/* Static server shared by server_thread. Each test uses a unique port
 * and does init/stop/free, so sequential utest execution is safe. */
static KlServer timeout_server;

static void *server_thread(void *arg) {
    (void)arg;
    kl_server_run(&timeout_server);
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
        close(fd);
        return -1;
    }
    return fd;
}

/* Read with timeout (select-based, avoids blocking forever if server
 * doesn't send anything before test timeout) */
static ssize_t read_with_timeout(int fd, char *buf, size_t buflen, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    int r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) return r; /* 0 = timeout, -1 = error */

    return read(fd, buf, buflen);
}

UTEST(timeout, idle_connection) {
    KlConfig cfg = {
        .port = TEST_PORT,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_server_init(&timeout_server, &cfg), 0);
    kl_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    usleep(100000); /* wait for server to start */

    /* Connect but send nothing */
    int fd = connect_to(TEST_PORT);
    ASSERT_TRUE(fd >= 0);

    /* Wait for 408 */
    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    close(fd);
    kl_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_server_free(&timeout_server);
}

UTEST(timeout, partial_headers) {
    KlConfig cfg = {
        .port = TEST_PORT + 1,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_server_init(&timeout_server, &cfg), 0);
    kl_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    usleep(100000);

    int fd = connect_to(TEST_PORT + 1);
    ASSERT_TRUE(fd >= 0);

    /* Send partial request line, then stall */
    const char *partial = "GET /ok HTT";
    (void)write(fd, partial, strlen(partial));

    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    close(fd);
    kl_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_server_free(&timeout_server);
}

UTEST(timeout, partial_body) {
    KlConfig cfg = {
        .port = TEST_PORT + 2,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_server_init(&timeout_server, &cfg), 0);
    kl_server_route(&timeout_server, "POST", "/echo", handle_ok,
                    NULL, kl_body_reader_buffer);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    usleep(100000);

    int fd = connect_to(TEST_PORT + 2);
    ASSERT_TRUE(fd >= 0);

    /* Send headers + partial body, then stall */
    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 100\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "partial";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    close(fd);
    kl_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_server_free(&timeout_server);
}

UTEST(timeout, active_not_affected) {
    KlConfig cfg = {
        .port = TEST_PORT + 3,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_server_init(&timeout_server, &cfg), 0);
    kl_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    usleep(100000);

    int fd = connect_to(TEST_PORT + 3);
    ASSERT_TRUE(fd >= 0);

    /* Send complete request promptly */
    const char *req = "GET /ok HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

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

    close(fd);
    kl_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_server_free(&timeout_server);
}

/* ═══════════════════════════════════════════════════════════════════
 * Additional timeout tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(timeout, body_timeout) {
    KlConfig cfg = {
        .port = TEST_PORT + 4,
        .read_timeout_ms = 2000,     /* generous header timeout */
        .body_timeout_ms = TEST_TIMEOUT_MS, /* tight body timeout */
    };
    ASSERT_EQ(kl_server_init(&timeout_server, &cfg), 0);
    kl_server_route(&timeout_server, "POST", "/echo", handle_ok,
                    NULL, kl_body_reader_buffer);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    usleep(100000);

    int fd = connect_to(TEST_PORT + 4);
    ASSERT_TRUE(fd >= 0);

    /* Send headers with large content-length, then trickle body */
    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 1000\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "x"; /* only 1 byte of 1000 */
    (void)write(fd, req, strlen(req));

    /* Wait for timeout response */
    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 3000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "408") != NULL);

    close(fd);
    kl_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_server_free(&timeout_server);
}

UTEST(timeout, fast_large_body) {
    KlConfig cfg = {
        .port = TEST_PORT + 5,
        .read_timeout_ms = 2000,
    };
    ASSERT_EQ(kl_server_init(&timeout_server, &cfg), 0);
    kl_server_route(&timeout_server, "POST", "/echo", handle_ok,
                    NULL, kl_body_reader_buffer);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    usleep(100000);

    int fd = connect_to(TEST_PORT + 5);
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

    (void)write(fd, header, (size_t)hlen);
    (void)write(fd, body, sizeof(body));

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

    close(fd);
    kl_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_server_free(&timeout_server);
}

UTEST(timeout, keepalive_idle_timeout) {
    KlConfig cfg = {
        .port = TEST_PORT + 6,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_server_init(&timeout_server, &cfg), 0);
    kl_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    usleep(100000);

    int fd = connect_to(TEST_PORT + 6);
    ASSERT_TRUE(fd >= 0);

    /* Send first request (keepalive) */
    const char *req1 = "GET /ok HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    (void)write(fd, req1, strlen(req1));

    /* Read first response */
    char buf[4096];
    ssize_t n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Don't send second request — wait for idle timeout.
     * The connection should be closed with a 408 or just closed. */
    n = read_with_timeout(fd, buf, sizeof(buf) - 1, 2000);
    /* n <= 0 means connection closed, or n > 0 with 408 */
    if (n > 0) {
        buf[n] = '\0';
        ASSERT_TRUE(strstr(buf, "408") != NULL);
    } else {
        /* Connection closed without response — also acceptable */
        ASSERT_TRUE(n <= 0);
    }

    close(fd);
    kl_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_server_free(&timeout_server);
}

UTEST(timeout, concurrent_timeouts) {
    KlConfig cfg = {
        .port = TEST_PORT + 7,
        .read_timeout_ms = TEST_TIMEOUT_MS,
    };
    ASSERT_EQ(kl_server_init(&timeout_server, &cfg), 0);
    kl_server_route(&timeout_server, "GET", "/ok", handle_ok, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, NULL);
    usleep(100000);

    /* Connect two idle clients */
    int fd1 = connect_to(TEST_PORT + 7);
    int fd2 = connect_to(TEST_PORT + 7);
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

    close(fd1);
    close(fd2);
    kl_server_stop(&timeout_server);
    pthread_join(tid, NULL);
    kl_server_free(&timeout_server);
}

UTEST_MAIN();
