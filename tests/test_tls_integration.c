#include "utest.h"
#include <keel/keel.h>
#include <keel/tls.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>

/* ═══════════════════════════════════════════════════════════════════
 * Passthrough TLS mock
 *
 * Wraps real socket I/O through the KlTls vtable without encryption.
 * This validates the full server TLS code path: handshake state
 * transitions, conn_read/conn_write via vtable, and TLS shutdown.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    KlTls        base;
    KlAllocator *alloc;
    int          handshake_done;
} PassthroughTls;

static KlTlsResult pt_handshake(KlTls *self, KlSocketHandle fd) {
    PassthroughTls *pt = (PassthroughTls *)self;
    (void)fd;
    pt->handshake_done = 1;
    return KL_TLS_OK;
}

static ssize_t pt_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len) {
    (void)self;
    ssize_t r;
    do { r = read(fd, buf, len); } while (r < 0 && errno == EINTR);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return r;
}

static ssize_t pt_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len) {
    (void)self;
    ssize_t r;
    do { r = write(fd, buf, len); } while (r < 0 && errno == EINTR);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return r;
}

static KlTlsResult pt_shutdown(KlTls *self, KlSocketHandle fd) {
    (void)self; (void)fd;
    return KL_TLS_OK;
}

static size_t pt_pending(KlTls *self) {
    (void)self;
    return 0;
}

static void pt_reset(KlTls *self) {
    PassthroughTls *pt = (PassthroughTls *)self;
    pt->handshake_done = 0;
}

static void pt_destroy(KlTls *self) {
    PassthroughTls *pt = (PassthroughTls *)self;
    kl_free(pt->alloc, pt, sizeof(*pt));
}

static KlTls *pt_factory(KlTlsCtx *ctx, KlAllocator *alloc) {
    (void)ctx;
    PassthroughTls *pt = kl_malloc(alloc, sizeof(*pt));
    if (!pt) return NULL;
    memset(pt, 0, sizeof(*pt));
    pt->alloc             = alloc;
    pt->base.handshake    = pt_handshake;
    pt->base.read         = pt_read;
    pt->base.write        = pt_write;
    pt->base.shutdown     = pt_shutdown;
    pt->base.pending      = pt_pending;
    pt->base.reset        = pt_reset;
    pt->base.destroy      = pt_destroy;
    pt->base.alpn_protocol = NULL;
    pt->base.set_hostname  = NULL;
    return &pt->base;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void wait_for_bind(KlServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
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

/* ═══════════════════════════════════════════════════════════════════
 * Tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(tls_integration, hello_request) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = pt_factory,
    };
    KlConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlServer srv;
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);
    int port = srv.bound_port;

    /* Connect and send plain HTTP — passthrough TLS is transparent */
    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    write(fd, req, strlen(req));

    char buf[2048];
    read_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    close(fd);
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST(tls_integration, keep_alive) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = pt_factory,
    };
    KlConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlServer srv;
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    int port = srv.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    /* First request (keep-alive) */
    const char *req1 = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(fd, req1, strlen(req1));

    char buf[2048];
    read_one_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    /* Second request on same connection (tests TLS reset()) */
    const char *req2 = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    write(fd, req2, strlen(req2));

    read_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    close(fd);
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST(tls_integration, concurrent) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = pt_factory,
    };
    KlConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlServer srv;
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

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
        write(fds[i], req, strlen(req));
    }

    for (int i = 0; i < 3; i++) {
        char buf[2048];
        read_response(fds[i], buf, sizeof(buf), 2000);
        ASSERT_TRUE_MSG(strstr(buf, "200 OK") != NULL,
                         "TLS connection %d should get 200 OK");
        ASSERT_TRUE_MSG(strstr(buf, "{\"ok\":true}") != NULL,
                         "TLS connection %d should have correct body");
        close(fds[i]);
    }

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST_MAIN();
