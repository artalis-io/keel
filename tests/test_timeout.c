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

UTEST_MAIN();
