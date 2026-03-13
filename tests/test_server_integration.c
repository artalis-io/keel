#include "utest.h"
#include <keel/keel.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

/* ── Helpers ────────────────────────────────────────────────────────── */

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void handle_slow(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    int delay_ms = ctx ? *(int *)ctx : 100;
    usleep((unsigned)(delay_ms * 1000));
    kl_response_json(res, 200, "{\"slow\":true}", 13);
}

static void *server_thread_fn(void *arg) {
    kl_server_run((KlServer *)arg);
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

/* Non-blocking connect — returns fd on success (may still be connecting), -1 on error */
static int connect_nb(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read full response (until EOF or read returns 0) with timeout */
static ssize_t read_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    ssize_t total = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    while (total < (ssize_t)buflen - 1) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) break;
        ssize_t n = read(fd, buf + total, buflen - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

/* Read one complete HTTP response (headers + body by Content-Length) */
static ssize_t read_one_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    ssize_t total = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    while (total < (ssize_t)buflen - 1) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) break;
        ssize_t n = read(fd, buf + total, buflen - (size_t)total - 1);
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

/* Wait for server to bind (max 2s) */
static void wait_for_bind(KlServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

/* ── Pool exhaustion ────────────────────────────────────────────────── */

UTEST(server_integration, pool_exhaustion_rejects) {
    KlServer srv;
    KlConfig cfg = {
        .port = 0,
        .max_connections = 2,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    /* Fill pool (2 connections) */
    int fd1 = connect_to(port);
    int fd2 = connect_to(port);
    ASSERT_TRUE(fd1 >= 0);
    ASSERT_TRUE(fd2 >= 0);

    /* Wait for pool to register both connections */
    usleep(50000);

    /* Third connect — pool full, server paused listening.
     * The OS may queue the connection in the backlog, but it won't
     * get a response because no slot is available. */
    int fd3 = connect_nb(port);

    /* Try to get a response on fd3 — should timeout */
    if (fd3 >= 0) {
        const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        write(fd3, req, strlen(req));
        char buf[1024];
        ssize_t n = read_response(fd3, buf, sizeof(buf), 500);
        /* Either connection refused, or no response (pool exhausted) */
        ASSERT_EQ(n, 0);
        close(fd3);
    }

    /* Release one slot */
    close(fd1);
    usleep(200000);

    /* Now a new connection should succeed */
    int fd4 = connect_to(port);
    ASSERT_TRUE(fd4 >= 0);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    write(fd4, req, strlen(req));
    char buf[1024];
    read_response(fd4, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    close(fd4);
    close(fd2);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

/* ── Backpressure recovery ──────────────────────────────────────────── */

UTEST(server_integration, backpressure_recovery) {
    KlServer srv;
    KlConfig cfg = {
        .port = 0,
        .max_connections = 2,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    /* Fill pool */
    int fd1 = connect_to(port);
    int fd2 = connect_to(port);
    ASSERT_TRUE(fd1 >= 0);
    ASSERT_TRUE(fd2 >= 0);
    usleep(50000);

    /* Release one connection — server should resume accepting */
    close(fd1);
    usleep(200000);

    /* New connection should succeed */
    int fd3 = connect_to(port);
    ASSERT_TRUE(fd3 >= 0);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    write(fd3, req, strlen(req));
    char buf[1024];
    read_response(fd3, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Release again, fill again — second cycle */
    close(fd2);
    close(fd3);
    usleep(200000);

    int fd4 = connect_to(port);
    int fd5 = connect_to(port);
    ASSERT_TRUE(fd4 >= 0);
    ASSERT_TRUE(fd5 >= 0);

    /* Both should work */
    write(fd4, req, strlen(req));
    read_response(fd4, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    close(fd4);
    close(fd5);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

/* ── Keep-alive pipeline ────────────────────────────────────────────── */

UTEST(server_integration, keep_alive_pipeline) {
    KlServer srv;
    KlConfig cfg = { .port = 0 };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* Send 3 sequential requests on one connection */
    for (int i = 0; i < 3; i++) {
        const char *req = (i < 2)
            ? "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n"
            : "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        write(fd, req, strlen(req));

        char buf[2048];
        if (i < 2) {
            read_one_response(fd, buf, sizeof(buf), 2000);
        } else {
            read_response(fd, buf, sizeof(buf), 2000);
        }
        ASSERT_TRUE_MSG(strstr(buf, "200 OK") != NULL,
                         "Request %d should get 200 OK");
        ASSERT_TRUE_MSG(strstr(buf, "{\"ok\":true}") != NULL,
                         "Request %d should have body");
    }

    close(fd);

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

/* ── Drain completes in-flight ──────────────────────────────────────── */

UTEST(server_integration, drain_completes_inflight) {
    static int delay_ms = 200;
    KlServer srv;
    KlConfig cfg = {
        .port = 0,
        .drain_timeout_ms = 2000,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/slow", handle_slow, &delay_ms, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    /* Start a request to the slow handler */
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    write(fd, req, strlen(req));

    /* Immediately trigger drain — handler is still sleeping */
    usleep(50000);
    kl_server_stop(&srv);

    /* Response should still complete (drain allows in-flight to finish) */
    char buf[1024];
    read_response(fd, buf, sizeof(buf), 3000);
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"slow\":true}") != NULL);

    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

/* ── Drain deadline forces exit with idle connections ────────────────── */

UTEST(server_integration, drain_deadline_forces_exit) {
    KlServer srv;
    KlConfig cfg = {
        .port = 0,
        .drain_timeout_ms = 200,
        .read_timeout_ms = 30000,
    };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    /* Open connections but don't send complete requests — they stay in READING */
    int fd1 = connect_to(port);
    int fd2 = connect_to(port);
    ASSERT_TRUE(fd1 >= 0);
    ASSERT_TRUE(fd2 >= 0);

    /* Send partial headers to keep connections alive */
    write(fd1, "GET /hello HTTP/1.1\r\n", 21);
    write(fd2, "GET /hello HTTP/1.1\r\n", 21);
    usleep(50000);

    /* Trigger drain with 200ms deadline — idle connections should be force-closed */
    uint64_t start = kl_monotonic_ms();
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    uint64_t elapsed = kl_monotonic_ms() - start;

    /* Server should exit within ~1.5s (200ms drain + 1000ms poll timeout) */
    ASSERT_TRUE_MSG(elapsed < 2000,
                     "Server should exit within ~2s with drain deadline");

    close(fd1);
    close(fd2);
    kl_server_free(&srv);
}

/* ── Concurrent requests ────────────────────────────────────────────── */

UTEST(server_integration, concurrent_requests) {
    KlServer srv;
    KlConfig cfg = { .port = 0 };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    int fds[4];
    for (int i = 0; i < 4; i++) {
        fds[i] = connect_to(port);
        ASSERT_TRUE(fds[i] >= 0);
    }

    /* Send requests on all 4 connections simultaneously */
    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    for (int i = 0; i < 4; i++) {
        write(fds[i], req, strlen(req));
    }

    /* All should get correct responses */
    for (int i = 0; i < 4; i++) {
        char buf[1024];
        read_response(fds[i], buf, sizeof(buf), 2000);
        ASSERT_TRUE_MSG(strstr(buf, "200 OK") != NULL,
                         "Connection %d should get 200 OK");
        ASSERT_TRUE_MSG(strstr(buf, "{\"ok\":true}") != NULL,
                         "Connection %d should have correct body");
        close(fds[i]);
    }

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST_MAIN();
