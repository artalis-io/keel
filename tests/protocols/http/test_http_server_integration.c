#include "utest.h"
#include <keel/clock.h>
#include <keel/keel.h>
#include "socket.h"   /* internal: exercise the socket-provider seam */
#include "net_compat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ── Helpers ────────────────────────────────────────────────────────── */

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

/* A large body forces partial sends through the serialized writev fallback. */
static char g_big_body[64 * 1024];
static void handle_big(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    for (size_t i = 0; i < sizeof(g_big_body); i++)
        g_big_body[i] = (char)('A' + (i % 26));
    kl_http_response_json(res, 200, g_big_body, sizeof(g_big_body));
}

/* ── A provider with native fds but NO vectored/sendfile support: forces the
 * serialized writev + pread-send sendfile fallbacks in http_response.c. send/recv
 * are plain POSIX; the other ops fall back to POSIX (NULL). ────────────────── */
static ssize_t noviv_send(void *c, KlSocketHandle fd, const void *b, size_t n) {
    (void)c; return send((int)fd, b, n, 0);
}
static ssize_t noviv_recv(void *c, KlSocketHandle fd, void *b, size_t n) {
    (void)c; return recv((int)fd, b, n, 0);
}
static const KlSocketOps NOVIV_OPS = { .send = noviv_send, .recv = noviv_recv,
                                       .name = "noviv" };
static const KlSocketProvider g_noviv = { &NOVIV_OPS, NULL, KL_SOCK_CAP_NATIVE_FD, NULL };

/* Serve a fixed file (sendfile path) from a global path. Used only by the
 * sendfile_fallback test, which is POSIX-only (hardcoded /tmp + CRT file I/O). */
#if !defined(_WIN32)
static char g_file_path[256];
static void handle_file(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    int fd = open(g_file_path, O_RDONLY);
    if (fd < 0) { kl_http_response_json(res, 500, "{}", 2); return; }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); kl_http_response_json(res, 500, "{}", 2); return; }
    kl_http_response_file(res, fd, st.st_size);   /* Keel closes fd on reset */
}
#endif

static void handle_slow(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    int delay_ms = ctx ? *(int *)ctx : 100;
    usleep((unsigned)(delay_ms * 1000));
    kl_http_response_json(res, 200, "{\"slow\":true}", 13);
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

/* Non-blocking connect: returns fd on success (may still be connecting), -1 on error */
static int connect_nb(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    kl_test_set_nonblock(fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        kl_test_closesock(fd);
        return -1;
    }
    return fd;
}

/* Read full response (until EOF or read returns 0) with timeout */
static ssize_t read_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    ssize_t total = 0;

    while (total < (ssize_t)buflen - 1) {
        int pr = kl_test_poll1(fd, 0, timeout_ms);
        if (pr <= 0) break;
        long n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

/* Read one complete HTTP response (headers + body by Content-Length) */
static ssize_t read_one_response(int fd, char *buf, size_t buflen, int timeout_ms) {
    ssize_t total = 0;

    while (total < (ssize_t)buflen - 1) {
        int pr = kl_test_poll1(fd, 0, timeout_ms);
        if (pr <= 0) break;
        long n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1);
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
static void wait_for_bind(KlHttpServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

/* ── Pool exhaustion ────────────────────────────────────────────────── */

UTEST(server_integration, pool_exhaustion_rejects) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = {
        .port = 0,
        .max_connections = 2,
    };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

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

    /* Stats must reflect the readiness listener's PAUSED state when the pool is full. */
    KlHttpServerStats st_full;
    kl_http_server_stats(&srv, &st_full);
    ASSERT_EQ(st_full.active_connections, 2);
    ASSERT_EQ(st_full.listen_paused, 1);

    /* Third connect: pool full, server paused listening.
     * The OS may queue the connection in the backlog, but it won't
     * get a response because no slot is available. */
    int fd3 = connect_nb(port);

    /* Try to get a response on fd3: should timeout */
    if (fd3 >= 0) {
        const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        kl_test_sockwrite(fd3, req, strlen(req));
        char buf[1024];
        ssize_t n = read_response(fd3, buf, sizeof(buf), 500);
        /* Either connection refused, or no response (pool exhausted) */
        ASSERT_EQ(n, 0);
        kl_test_closesock(fd3);
    }

    /* Release one slot */
    kl_test_closesock(fd1);
    usleep(200000);

    /* Now a new connection should succeed */
    int fd4 = connect_to(port);
    ASSERT_TRUE(fd4 >= 0);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    kl_test_sockwrite(fd4, req, strlen(req));
    char buf[1024];
    read_response(fd4, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_test_closesock(fd4);
    kl_test_closesock(fd2);

    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
}

/* ── Listener teardown: kl_http_server_free must complete the accept listener's
 *    close/detachment while armed AND while paused. ASan/UBSan verify no leak/UAF. ─────── */

UTEST(server_integration, teardown_while_armed) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);

    /* No connections → listener LISTENING (armed). */
    KlHttpServerStats st;
    kl_http_server_stats(&srv, &st);
    ASSERT_EQ(st.listen_paused, 0);

    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);   /* must close+detach the armed listener cleanly */
}

UTEST(server_integration, teardown_while_paused) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 1 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    int port = srv.bound_port;

    int fd1 = connect_to(port);     /* fills the single slot */
    int fd2 = connect_nb(port);     /* backlog; listener PAUSED */
    usleep(50000);
    KlHttpServerStats st;
    kl_http_server_stats(&srv, &st);
    ASSERT_EQ(st.listen_paused, 1);

    /* Free the server while the listener is PAUSED with a held reservation dropped and interest
     * disarmed: the close/detachment contract must complete. */
    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
    if (fd1 >= 0) kl_test_closesock(fd1);
    if (fd2 >= 0) kl_test_closesock(fd2);
}

/* ── Backpressure recovery ──────────────────────────────────────────── */

UTEST(server_integration, backpressure_recovery) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = {
        .port = 0,
        .max_connections = 2,
    };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

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

    /* Release one connection: server should resume accepting */
    kl_test_closesock(fd1);
    usleep(200000);

    /* New connection should succeed */
    int fd3 = connect_to(port);
    ASSERT_TRUE(fd3 >= 0);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    kl_test_sockwrite(fd3, req, strlen(req));
    char buf[1024];
    read_response(fd3, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Release again, fill again: second cycle */
    kl_test_closesock(fd2);
    kl_test_closesock(fd3);
    usleep(200000);

    int fd4 = connect_to(port);
    int fd5 = connect_to(port);
    ASSERT_TRUE(fd4 >= 0);
    ASSERT_TRUE(fd5 >= 0);

    /* Both should work */
    kl_test_sockwrite(fd4, req, strlen(req));
    read_response(fd4, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    kl_test_closesock(fd4);
    kl_test_closesock(fd5);

    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
}

/* ── Keep-alive pipeline ────────────────────────────────────────────── */

UTEST(server_integration, keep_alive_pipeline) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

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
        kl_test_sockwrite(fd, req, strlen(req));

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

    kl_test_closesock(fd);

    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
}

/* ── Drain completes in-flight ──────────────────────────────────────── */

UTEST(server_integration, drain_completes_inflight) {
    static int delay_ms = 200;
    KlHttpServer srv;
    KlHttpServerConfig cfg = {
        .port = 0,
        .drain_timeout_ms = 2000,
    };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/slow", handle_slow, &delay_ms, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    /* Start a request to the slow handler */
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    kl_test_sockwrite(fd, req, strlen(req));

    /* Immediately trigger drain: handler is still sleeping */
    usleep(50000);
    kl_http_server_stop(&srv);

    /* Response should still complete (drain allows in-flight to finish) */
    char buf[1024];
    read_response(fd, buf, sizeof(buf), 3000);
    kl_test_closesock(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"slow\":true}") != NULL);

    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
}

/* ── Drain deadline forces exit with idle connections ────────────────── */

UTEST(server_integration, drain_deadline_forces_exit) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = {
        .port = 0,
        .drain_timeout_ms = 200,
        .read_timeout_ms = 30000,
    };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    /* Open connections but don't send complete requests: they stay in READING */
    int fd1 = connect_to(port);
    int fd2 = connect_to(port);
    ASSERT_TRUE(fd1 >= 0);
    ASSERT_TRUE(fd2 >= 0);

    /* Send partial headers to keep connections alive */
    kl_test_sockwrite(fd1, "GET /hello HTTP/1.1\r\n", 21);
    kl_test_sockwrite(fd2, "GET /hello HTTP/1.1\r\n", 21);
    usleep(50000);

    /* Trigger drain with 200ms deadline: idle connections should be force-closed */
    uint64_t start = kl_monotonic_ms();
    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    uint64_t elapsed = kl_monotonic_ms() - start;

    /* Server should exit within ~1.5s (200ms drain + 1000ms poll timeout) */
    ASSERT_TRUE_MSG(elapsed < 2000,
                     "Server should exit within ~2s with drain deadline");

    kl_test_closesock(fd1);
    kl_test_closesock(fd2);
    kl_http_server_free(&srv);
}

/* ── Concurrent requests ────────────────────────────────────────────── */

UTEST(server_integration, concurrent_requests) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

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
        kl_test_sockwrite(fds[i], req, strlen(req));
    }

    /* All should get correct responses */
    for (int i = 0; i < 4; i++) {
        char buf[1024];
        read_response(fds[i], buf, sizeof(buf), 2000);
        ASSERT_TRUE_MSG(strstr(buf, "200 OK") != NULL,
                         "Connection %d should get 200 OK");
        ASSERT_TRUE_MSG(strstr(buf, "{\"ok\":true}") != NULL,
                         "Connection %d should have correct body");
        kl_test_closesock(fds[i]);
    }

    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
}

/* Conformance: driving accepted-connection I/O through the explicit POSIX
 * provider (the vtable dispatch path, not just the NULL fast path) is
 * byte-identical to the default. Proves the server's KlHttpConn.ctx->sockets
 * plumbing + conn_read/conn_write seam adoption.
 *
 * POSIX-only: references kl_socket_provider_posix(), which lives in the
 * POSIX-only socket_posix.c TU and is not part of the Winsock build (the
 * Windows seam uses socket_winsock.c). No net_compat mapping applies:
 * excluded from the Windows build, unchanged on POSIX. */
#if !defined(_WIN32)
UTEST(server_integration, explicit_posix_provider_roundtrip) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);
    /* Post-init, pre-run: stamp the event ctx with the explicit POSIX provider;
     * accept() + accepted-connection reads/writes now dispatch through it. */
    srv.ev.sockets = kl_socket_provider_posix();

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);
    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, req, strlen(req)) > 0);
    char buf[1024];
    ssize_t n = read_response(fd, buf, sizeof(buf), 1000);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "200") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);
    kl_test_closesock(fd);

    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
}
#endif /* !_WIN32 */

/* A provider without KL_SOCK_CAP_WRITEV makes http_response.c serialize its
 * vectored writes through kl_sock_send. The full 64 KB body must still arrive
 * byte-correct, proving the serialized writev fallback. */
UTEST(server_integration, serialized_writev_fallback) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/big", handle_big, NULL, NULL);
    srv.ev.sockets = &g_noviv;   /* native fds, no writev/sendfile */

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);
    const char *req = "GET /big HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, req, strlen(req)) > 0);

    /* Read the whole response (headers + 64 KB body). */
    char *buf = malloc(128 * 1024);
    ASSERT_TRUE(buf != NULL);
    ssize_t n = read_response(fd, buf, 128 * 1024, 2000);
    ASSERT_TRUE(n > 0);
    char *body = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body != NULL);
    body += 4;
    ssize_t body_len = n - (body - buf);
    ASSERT_EQ(body_len, (ssize_t)sizeof(g_big_body));
    /* Verify the pattern round-tripped through the serialized path. */
    int ok = 1;
    for (size_t i = 0; i < sizeof(g_big_body) && ok; i++)
        if (body[i] != (char)('A' + (i % 26))) ok = 0;
    ASSERT_TRUE(ok);
    free(buf);
    kl_test_closesock(fd);

    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
}

/* A provider without KL_SOCK_CAP_SENDFILE makes http_response.c serve KL_HTTP_BODY_FILE
 * via pread + kl_sock_send instead of sendfile(); the file body must arrive
 * byte-correct. */
#if !defined(_WIN32)   /* hardcoded /tmp path + CRT file I/O: POSIX-specific */
UTEST(server_integration, sendfile_fallback) {
    /* Write a known temp file (~40 KB, spanning multiple read chunks). */
    snprintf(g_file_path, sizeof(g_file_path), "/tmp/keel_sf_%d.dat", (int)getpid());
    int wf = open(g_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_TRUE(wf >= 0);
    const size_t FLEN = 40000;
    for (size_t i = 0; i < FLEN; i++) {
        char c = (char)('a' + (i % 26));
        ASSERT_EQ(write(wf, &c, 1), (ssize_t)1);
    }
    close(wf);

    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/file", handle_file, NULL, NULL);
    srv.ev.sockets = &g_noviv;   /* no sendfile → pread+send fallback */

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);
    const char *req = "GET /file HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(kl_test_sockwrite(fd, req, strlen(req)) > 0);

    char *buf = malloc(64 * 1024);
    ASSERT_TRUE(buf != NULL);
    ssize_t n = read_response(fd, buf, 64 * 1024, 2000);
    ASSERT_TRUE(n > 0);
    char *body = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body != NULL);
    body += 4;
    ssize_t body_len = n - (body - buf);
    ASSERT_EQ(body_len, (ssize_t)FLEN);
    int ok = 1;
    for (size_t i = 0; i < FLEN && ok; i++)
        if (body[i] != (char)('a' + (i % 26))) ok = 0;
    ASSERT_TRUE(ok);
    free(buf);
    kl_test_closesock(fd);

    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);
    unlink(g_file_path);
}
#endif

UTEST_MAIN();
