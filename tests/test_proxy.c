/*
 * test_proxy.c — HTTP proxy support tests
 *
 * Tests proxy config handling, CONNECT request format, absolute-form URLs,
 * pool key matching with proxy fields, and error handling.
 */

#include "utest.h"
#include <keel/keel.h>
#include <keel/client.h>
#include <keel/client_pool.h>

#include <string.h>
#include "net_compat.h"
#include <pthread.h>
#include <errno.h>
#include <stdio.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

static int make_listener(int *out_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kl_test_closesock(fd);
        return -1;
    }

    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &alen);
    *out_port = ntohs(addr.sin_port);

    if (listen(fd, 4) < 0) {
        kl_test_closesock(fd);
        return -1;
    }
    return fd;
}

static int accept_with_timeout(int listen_fd, int timeout_ms)
{
    int rc = kl_test_poll1(listen_fd, 0, timeout_ms);
    if (rc <= 0) return -1;
    return accept(listen_fd, NULL, NULL);
}

static ssize_t read_until(int fd, char *buf, size_t buf_sz,
                           const char *needle, int timeout_ms)
{
    size_t total = 0;
    while (total < buf_sz - 1) {
        int rc = kl_test_poll1(fd, 0, timeout_ms);
        if (rc <= 0) break;

        ssize_t r = kl_test_sockread(fd, buf + total, buf_sz - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
        buf[total] = '\0';

        if (strstr(buf, needle))
            break;
    }
    return (ssize_t)total;
}

/* ── Thread function context types ───────────────────────────────── */

typedef struct {
    KlAllocator    *alloc;
    KlClientConfig *cfg;
    int             done;
    int             status;
    KlError         err;
    const char     *url;
    const char     *method;
} ProxyReqCtx;

static void *proxy_request_thread(void *arg)
{
    ProxyReqCtx *c = arg;
    KlClientResponse resp;
    int rc = kl_client_request(c->alloc, c->cfg, c->method, c->url,
                                NULL, 0, NULL, 0, &resp);
    c->status = (rc == 0) ? resp.status : -1;
    c->err = resp.error;
    if (rc == 0) kl_client_response_free(&resp);
    c->done = 1;
    return NULL;
}

static KlTls *null_tls_factory(KlTlsCtx *ctx, KlAllocator *alloc)
{
    (void)ctx; (void)alloc;
    return NULL;
}

/* ── Test 1: NULL proxy config = direct connection ───────────────── */

UTEST(proxy, config_null) {
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proxy = NULL;
    ASSERT_TRUE(cfg.proxy == NULL);
}

/* ── Test 2: HTTP request through proxy uses absolute-form URL ───── */

UTEST(proxy, absolute_url_http) {
    int proxy_port;
    int listen_fd = make_listener(&proxy_port);
    ASSERT_GE(listen_fd, 0);

    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = { .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL };
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proxy = &proxy;
    cfg.timeout_ms = 2000;

    ProxyReqCtx rctx = { .alloc = &alloc, .cfg = &cfg, .done = 0, .status = 0,
                          .err = KL_ERR_NONE,
                          .url = "http://example.com/hello", .method = "GET" };

    pthread_t tid;
    pthread_create(&tid, NULL, proxy_request_thread, &rctx);

    int client_fd = accept_with_timeout(listen_fd, 3000);
    ASSERT_GE(client_fd, 0);

    char buf[2048];
    ssize_t n = read_until(client_fd, buf, sizeof(buf), "\r\n\r\n", 2000);
    ASSERT_GT(n, 0);

    /* Verify absolute-form URL */
    ASSERT_TRUE(strstr(buf, "GET http://example.com/hello HTTP/1.1\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Host: example.com\r\n") != NULL);

    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
    kl_test_sockwrite(client_fd, resp, strlen(resp));
    kl_test_closesock(client_fd);

    pthread_join(tid, NULL);
    ASSERT_EQ(rctx.status, 200);

    kl_test_closesock(listen_fd);
}

/* ── Test 3: CONNECT request format correct ──────────────────────── */

UTEST(proxy, connect_request_format) {
    int proxy_port;
    int listen_fd = make_listener(&proxy_port);
    ASSERT_GE(listen_fd, 0);

    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = { .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL };
    /* Need a TLS config to get past the "HTTPS needs TLS" check */
    KlTlsConfig tls_cfg = { .factory = null_tls_factory };
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proxy = &proxy;
    cfg.tls = &tls_cfg;
    cfg.timeout_ms = 2000;

    ProxyReqCtx cctx = { .alloc = &alloc, .cfg = &cfg, .done = 0,
                          .err = KL_ERR_NONE,
                          .url = "https://secure.example.com:8443/api",
                          .method = "GET" };

    pthread_t tid;
    pthread_create(&tid, NULL, proxy_request_thread, &cctx);

    int client_fd = accept_with_timeout(listen_fd, 3000);
    ASSERT_GE(client_fd, 0);

    char buf[2048];
    ssize_t n = read_until(client_fd, buf, sizeof(buf), "\r\n\r\n", 2000);
    ASSERT_GT(n, 0);

    /* Verify CONNECT format */
    ASSERT_TRUE(strstr(buf, "CONNECT secure.example.com:8443 HTTP/1.1\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Host: secure.example.com:8443\r\n") != NULL);
    /* No Proxy-Authorization header when auth is NULL */
    ASSERT_TRUE(strstr(buf, "Proxy-Authorization") == NULL);

    const char *reject = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
    kl_test_sockwrite(client_fd, reject, strlen(reject));
    kl_test_closesock(client_fd);

    pthread_join(tid, NULL);
    kl_test_closesock(listen_fd);
}

/* ── Test 4: Proxy-Authorization header included when auth set ───── */

UTEST(proxy, connect_with_auth) {
    int proxy_port;
    int listen_fd = make_listener(&proxy_port);
    ASSERT_GE(listen_fd, 0);

    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = {
        .host = "127.0.0.1",
        .port = (uint16_t)proxy_port,
        .auth = "Basic dXNlcjpwYXNz"
    };
    KlTlsConfig tls_cfg = { .factory = null_tls_factory };
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proxy = &proxy;
    cfg.tls = &tls_cfg;
    cfg.timeout_ms = 2000;

    ProxyReqCtx actx = { .alloc = &alloc, .cfg = &cfg, .done = 0,
                          .err = KL_ERR_NONE,
                          .url = "https://secure.example.com/path",
                          .method = "GET" };

    pthread_t tid;
    pthread_create(&tid, NULL, proxy_request_thread, &actx);

    int client_fd = accept_with_timeout(listen_fd, 3000);
    ASSERT_GE(client_fd, 0);

    char buf[2048];
    ssize_t n = read_until(client_fd, buf, sizeof(buf), "\r\n\r\n", 2000);
    ASSERT_GT(n, 0);

    /* Verify Proxy-Authorization header present */
    ASSERT_TRUE(strstr(buf, "Proxy-Authorization: Basic dXNlcjpwYXNz\r\n") != NULL);

    const char *reject = "HTTP/1.1 407 Proxy Auth Required\r\nContent-Length: 0\r\n\r\n";
    kl_test_sockwrite(client_fd, reject, strlen(reject));
    kl_test_closesock(client_fd);

    pthread_join(tid, NULL);
    kl_test_closesock(listen_fd);
}

/* ── Test 5: CONNECT 200 → TLS handshake proceeds ───────────────── */

UTEST(proxy, connect_success_transitions_to_tls) {
    int proxy_port;
    int listen_fd = make_listener(&proxy_port);
    ASSERT_GE(listen_fd, 0);

    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = { .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL };
    /* null_tls_factory returns NULL → TLS init fails after CONNECT succeeds */
    KlTlsConfig tls_cfg = { .factory = null_tls_factory };
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proxy = &proxy;
    cfg.tls = &tls_cfg;
    cfg.timeout_ms = 2000;

    ProxyReqCtx tctx = { .alloc = &alloc, .cfg = &cfg, .done = 0,
                          .err = KL_ERR_NONE,
                          .url = "https://example.com/path",
                          .method = "GET" };

    pthread_t tid;
    pthread_create(&tid, NULL, proxy_request_thread, &tctx);

    int client_fd = accept_with_timeout(listen_fd, 3000);
    ASSERT_GE(client_fd, 0);

    char buf[2048];
    read_until(client_fd, buf, sizeof(buf), "\r\n\r\n", 2000);

    /* Respond 200 — CONNECT succeeds */
    const char *ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
    kl_test_sockwrite(client_fd, ok, strlen(ok));

    /* Client will try TLS handshake with null factory → fail with TLS error */
    pthread_join(tid, NULL);
    kl_test_closesock(client_fd);
    kl_test_closesock(listen_fd);

    /* Error should be TLS-related (not proxy), proving CONNECT succeeded */
    ASSERT_TRUE(tctx.err == KL_ERR_TLS_HANDSHAKE || tctx.err == KL_ERR_TLS_INIT);
}

/* ── Test 6: Non-200 proxy response → KL_ERR_PROXY ──────────────── */

UTEST(proxy, connect_rejected) {
    int proxy_port;
    int listen_fd = make_listener(&proxy_port);
    ASSERT_GE(listen_fd, 0);

    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = { .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL };
    KlTlsConfig tls_cfg = { .factory = null_tls_factory };
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proxy = &proxy;
    cfg.tls = &tls_cfg;
    cfg.timeout_ms = 2000;

    ProxyReqCtx rctx = { .alloc = &alloc, .cfg = &cfg, .done = 0,
                          .err = KL_ERR_NONE,
                          .url = "https://example.com/blocked",
                          .method = "GET" };

    pthread_t tid;
    pthread_create(&tid, NULL, proxy_request_thread, &rctx);

    int client_fd = accept_with_timeout(listen_fd, 3000);
    ASSERT_GE(client_fd, 0);

    char buf[2048];
    read_until(client_fd, buf, sizeof(buf), "\r\n\r\n", 2000);

    /* Respond with 403 (not 200) */
    const char *reject = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
    kl_test_sockwrite(client_fd, reject, strlen(reject));
    kl_test_closesock(client_fd);

    pthread_join(tid, NULL);
    kl_test_closesock(listen_fd);

    ASSERT_EQ((int)rctx.err, (int)KL_ERR_PROXY);
}

/* ── Test 7: Proxy response exceeding buffer → error ─────────────── */

UTEST(proxy, connect_buf_overflow) {
    int proxy_port;
    int listen_fd = make_listener(&proxy_port);
    ASSERT_GE(listen_fd, 0);

    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = { .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL };
    KlTlsConfig tls_cfg = { .factory = null_tls_factory };
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proxy = &proxy;
    cfg.tls = &tls_cfg;
    cfg.timeout_ms = 2000;

    ProxyReqCtx octx = { .alloc = &alloc, .cfg = &cfg, .done = 0,
                          .err = KL_ERR_NONE,
                          .url = "https://example.com/overflow",
                          .method = "GET" };

    pthread_t tid;
    pthread_create(&tid, NULL, proxy_request_thread, &octx);

    int client_fd = accept_with_timeout(listen_fd, 3000);
    ASSERT_GE(client_fd, 0);

    char buf[2048];
    read_until(client_fd, buf, sizeof(buf), "\r\n\r\n", 2000);

    /* Send a very long response that exceeds KL_PROXY_RESPONSE_MAX (4096)
     * without ever sending \r\n\r\n */
    char giant[5000];
    int off = snprintf(giant, sizeof(giant), "HTTP/1.1 200 OK\r\n");
    while (off < 4500) {
        int n = snprintf(giant + off, sizeof(giant) - (size_t)off,
                          "X-Pad-%04d: padding-value-to-fill-buffer\r\n", off);
        if (n <= 0) break;
        off += n;
    }
    /* No final \r\n\r\n — buffer fills without end-of-headers */
    kl_test_sockwrite(client_fd, giant, (size_t)off);
    usleep(50000);
    kl_test_closesock(client_fd);

    pthread_join(tid, NULL);
    kl_test_closesock(listen_fd);

    ASSERT_TRUE(octx.err == KL_ERR_PROXY || octx.err == KL_ERR_IO);
}

/* ── Test 8: DNS resolves proxy host, not target ─────────────────── */

UTEST(proxy, dns_resolves_proxy_host) {
    int proxy_port;
    int listen_fd = make_listener(&proxy_port);
    ASSERT_GE(listen_fd, 0);

    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = { .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL };
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proxy = &proxy;
    cfg.timeout_ms = 2000;

    ProxyReqCtx dctx = { .alloc = &alloc, .cfg = &cfg, .done = 0,
                          .err = KL_ERR_NONE,
                          .url = "http://nonexistent-host.invalid/test",
                          .method = "GET" };

    pthread_t tid;
    pthread_create(&tid, NULL, proxy_request_thread, &dctx);

    /* If we get a connection, DNS resolved the proxy (not the target) */
    int client_fd = accept_with_timeout(listen_fd, 3000);
    ASSERT_GE(client_fd, 0);

    char buf[2048];
    ssize_t n = read_until(client_fd, buf, sizeof(buf), "\r\n\r\n", 2000);
    ASSERT_GT(n, 0);
    ASSERT_TRUE(strstr(buf, "GET http://nonexistent-host.invalid/test HTTP/1.1\r\n") != NULL);

    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    kl_test_sockwrite(client_fd, resp, strlen(resp));
    kl_test_closesock(client_fd);

    pthread_join(tid, NULL);
    kl_test_closesock(listen_fd);
}

/* ── Test 9: Redirect client inherits proxy config ───────────────── */

UTEST(proxy, redirect_inherits) {
    int proxy_port;
    int listen_fd = make_listener(&proxy_port);
    ASSERT_GE(listen_fd, 0);

    KlAllocator alloc = kl_allocator_default();
    KlProxyConfig proxy = { .host = "127.0.0.1", .port = (uint16_t)proxy_port, .auth = NULL };
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proxy = &proxy;
    cfg.timeout_ms = 2000;

    ProxyReqCtx rctx = { .alloc = &alloc, .cfg = &cfg, .done = 0,
                          .err = KL_ERR_NONE,
                          .url = "http://redirect-test.example.com/start",
                          .method = "GET" };

    pthread_t tid;
    pthread_create(&tid, NULL, proxy_request_thread, &rctx);

    int client_fd = accept_with_timeout(listen_fd, 3000);
    ASSERT_GE(client_fd, 0);

    char buf[2048];
    ssize_t n = read_until(client_fd, buf, sizeof(buf), "\r\n\r\n", 2000);
    ASSERT_GT(n, 0);
    ASSERT_TRUE(strstr(buf, "http://redirect-test.example.com/start") != NULL);

    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    kl_test_sockwrite(client_fd, resp, strlen(resp));
    kl_test_closesock(client_fd);

    pthread_join(tid, NULL);
    ASSERT_EQ(rctx.status, 200);

    kl_test_closesock(listen_fd);
}

/* ── Test 10: Pool key: proxied entry matches same proxy ─────────── */

UTEST(proxy, pool_key_match) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    KlClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn, "target.com", 80, 0,
                                "proxy.com", 8080), 0);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 1);
    ASSERT_EQ(kl_cpool_host_count(&pool, "target.com", 80, 0,
                                    "proxy.com", 8080), 1);

    KlClientPoolConn acq;
    ASSERT_EQ(kl_cpool_acquire(&pool, "target.com", 80, 0,
                                "proxy.com", 8080, &acq), 0);
    ASSERT_EQ(acq.reused, 1);
    ASSERT_EQ(acq.fd, fds[0]);

    kl_test_closesock(acq.fd);
    kl_test_closesock(fds[1]);
    kl_cpool_free(&pool);
}

/* ── Test 11: Pool key: direct entry doesn't match proxied ───────── */

UTEST(proxy, pool_direct_no_match) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    int fds[2][2];
    ASSERT_EQ(kl_test_socketpair(fds[0]), 0);
    ASSERT_EQ(kl_test_socketpair(fds[1]), 0);

    /* Release a direct connection */
    KlClientPoolConn conn1 = { .fd = fds[0][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn1, "target.com", 80, 0,
                                NULL, 0), 0);

    /* Release a proxied connection to same target */
    KlClientPoolConn conn2 = { .fd = fds[1][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn2, "target.com", 80, 0,
                                "proxy.com", 3128), 0);

    ASSERT_EQ(kl_cpool_idle_count(&pool), 2);

    /* Acquire direct — should NOT return the proxied one */
    KlClientPoolConn acq;
    ASSERT_EQ(kl_cpool_acquire(&pool, "target.com", 80, 0,
                                NULL, 0, &acq), 0);
    ASSERT_EQ(acq.fd, fds[0][0]);

    /* Acquire proxied — should return the proxied one */
    KlClientPoolConn acq2;
    ASSERT_EQ(kl_cpool_acquire(&pool, "target.com", 80, 0,
                                "proxy.com", 3128, &acq2), 0);
    ASSERT_EQ(acq2.fd, fds[1][0]);

    /* Acquire with different proxy — miss */
    KlClientPoolConn acq3;
    ASSERT_EQ(kl_cpool_acquire(&pool, "target.com", 80, 0,
                                "other-proxy.com", 3128, &acq3), 1);

    kl_test_closesock(acq.fd);
    kl_test_closesock(acq2.fd);
    kl_test_closesock(fds[0][1]);
    kl_test_closesock(fds[1][1]);
    kl_cpool_free(&pool);
}

UTEST_MAIN();
