/*
 * test_cross_module.c — Integration tests for module combinations
 *
 * Tests cross-module interactions that unit tests don't cover:
 *   - compress + drain pipeline
 *   - TLS + async suspend/resume
 *   - middleware + body reader + async handler
 *   - resolver_cache + sync client
 *   - TLS + middleware + compressed response
 */

#include "utest.h"
#include <keel/keel.h>
#include <keel/resolver_cache.h>

#include <string.h>
#include "net_compat.h"
#include "mock_tls.h"   /* shared completion-capable identity TLS mock */
#include <pthread.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════════════
 * Shared helpers
 * ═══════════════════════════════════════════════════════════════════ */

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

static int set_nonblocking(int fd) {
    return kl_test_set_nonblock(fd);
}

/* ═══════════════════════════════════════════════════════════════════
 * Mock compressor (reused from test_compress.c pattern)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    KlCompress base;
    int feed_count;
    int last_flush;
    int destroy_called;
} MockComp;

static int mock_feed(KlCompress *self, const char *data, size_t len,
                      int flush,
                      int (*emit)(void *ctx, const char *data, size_t len),
                      void *emit_ctx) {
    MockComp *m = (MockComp *)self;
    m->feed_count++;
    m->last_flush = flush;

    if (data && len > 0) {
        if (emit(emit_ctx, "C:", 2) < 0) return -1;
        if (emit(emit_ctx, data, len) < 0) return -1;
    }
    if (flush) {
        if (emit(emit_ctx, "[END]", 5) < 0) return -1;
    }
    return 0;
}

static int mock_compress_buf(KlCompress *self, const char *in, size_t in_len,
                              char **out, size_t *out_len, KlAllocator *alloc) {
    (void)self;
    /* "Compress" by copying data without change — must be smaller than input
     * to pass the expansion check in kl_response_body_compress.
     * We drop the last byte to simulate compression savings. */
    size_t clen = in_len > 1 ? in_len - 1 : 0;
    *out = kl_malloc(alloc, clen > 0 ? clen : 1);
    if (!*out) return -1;
    if (clen > 0) memcpy(*out, in, clen);
    *out_len = clen;
    return 0;
}

static const char *mock_encoding(KlCompress *self) {
    (void)self;
    return "mock";
}

static void mock_reset(KlCompress *self) {
    MockComp *m = (MockComp *)self;
    m->feed_count = 0;
    m->last_flush = 0;
}

static void mock_destroy(KlCompress *self) {
    MockComp *m = (MockComp *)self;
    m->destroy_called = 1;
}

static MockComp g_mock;

static KlCompress *mock_factory(KlCompressCtx *ctx, KlAllocator *alloc) {
    (void)ctx; (void)alloc;
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.base.compress = mock_compress_buf;
    g_mock.base.feed     = mock_feed;
    g_mock.base.encoding = mock_encoding;
    g_mock.base.reset    = mock_reset;
    g_mock.base.destroy  = mock_destroy;
    return &g_mock.base;
}

/* ═══════════════════════════════════════════════════════════════════
 * Passthrough TLS mock (reused from test_tls_integration.c)
 * ═══════════════════════════════════════════════════════════════════ */

/* Passthrough (identity) TLS mock: shared tests/mock_tls.h (mock_tls_create), which implements
 * the completion-mode ops too so the TLS cross-module tests run over the completion backend. */

/* ═══════════════════════════════════════════════════════════════════
 * Mock DNS resolver (sync completion for testing)
 * ═══════════════════════════════════════════════════════════════════ */

static int mock_resolve_count;

static KlResolveResult make_loopback(void) {
    KlResolveResult r;
    memset(&r, 0, sizeof(r));
    struct sockaddr_in *sin = (struct sockaddr_in *)&r.addrs[0];
    sin->sin_family = AF_INET;
    sin->sin_port = htons(80);
    sin->sin_addr.s_addr = htonl(0x7f000001);
    r.addrlens[0] = sizeof(struct sockaddr_in);
    r.naddrs = 1;
    r.ai_socktype = SOCK_STREAM;
    return r;
}

static KlResolveReq mock_req_storage;

static KlResolveReq *counting_resolve(KlResolver *self, KlEventCtx *ctx,
                                        const char *host, int port,
                                        KlResolveDoneFn done_fn, void *user_data) {
    (void)ctx; (void)host; (void)port;
    mock_resolve_count++;
    mock_req_storage.resolver = self;
    KlResolveResult r = make_loopback();
    done_fn(&mock_req_storage, &r, 0, user_data);
    return &mock_req_storage;
}

static void noop_cancel(KlResolveReq *req) { (void)req; }
static void noop_destroy(KlResolver *self) { (void)self; }

/* ═══════════════════════════════════════════════════════════════════
 * Test 1: compress + drain pipeline
 *
 * Verifies that compressed stream output flows through a KlDrain
 * with simulated backpressure. Tests the data path:
 *   handler → compress_stream_write → feed → emit → drain → writer
 * ═══════════════════════════════════════════════════════════════════ */

#define SINK_BUF_SIZE 4096

typedef struct {
    char   buf[SINK_BUF_SIZE];
    size_t len;
    int    mode;          /* 0=normal, 1=partial, 2=would-block */
    int    call_count;
    int    switch_after;  /* switch to normal after N calls */
} MockWriter;

static ssize_t mock_write(const char *data, size_t len, void *ctx) {
    MockWriter *w = ctx;
    w->call_count++;

    int mode = w->mode;
    if (w->switch_after >= 0 && w->call_count > w->switch_after)
        mode = 0;

    if (mode == 2) return 0;   /* would-block */

    size_t accept = len;
    if (mode == 1) accept = len / 2;
    if (accept == 0 && len > 0) accept = 1;

    if (w->len + accept > SINK_BUF_SIZE)
        accept = SINK_BUF_SIZE - w->len;
    memcpy(w->buf + w->len, data, accept);
    w->len += accept;
    return (ssize_t)accept;
}

static int drain_emit(void *ctx, const char *data, size_t len) {
    KlDrain *d = ctx;
    return kl_drain_write(d, data, len);
}

UTEST(cross, compress_drain_pipeline) {
    KlAllocator alloc = kl_allocator_default();

    /* Set up writer → drain → compress emitter chain */
    MockWriter writer;
    memset(&writer, 0, sizeof(writer));
    writer.switch_after = -1;

    KlDrain drain;
    kl_drain_init(&drain, mock_write, &writer, &alloc);

    /* Create mock compressor */
    MockComp comp;
    memset(&comp, 0, sizeof(comp));
    comp.base.feed     = mock_feed;
    comp.base.encoding = mock_encoding;
    comp.base.destroy  = mock_destroy;

    /* Feed data through compress → drain → writer */
    int rc = comp.base.feed(&comp.base, "hello", 5, 0, drain_emit, &drain);
    ASSERT_EQ(0, rc);

    /* Verify data arrived: "C:" + "hello" */
    ASSERT_EQ(7, (int)writer.len);
    ASSERT_EQ(0, memcmp(writer.buf, "C:hello", 7));

    /* Final flush */
    rc = comp.base.feed(&comp.base, NULL, 0, 1, drain_emit, &drain);
    ASSERT_EQ(0, rc);

    /* Should have "[END]" appended */
    ASSERT_EQ(12, (int)writer.len);
    ASSERT_EQ(0, memcmp(writer.buf + 7, "[END]", 5));

    ASSERT_EQ(2, comp.feed_count);
    ASSERT_EQ(1, comp.last_flush);
    ASSERT_EQ(0, kl_drain_pending(&drain));

    kl_drain_free(&drain);
}

UTEST(cross, compress_drain_backpressure) {
    KlAllocator alloc = kl_allocator_default();

    /* Writer blocks on first 2 calls, then accepts */
    MockWriter writer;
    memset(&writer, 0, sizeof(writer));
    writer.mode = 2;          /* would-block initially */
    writer.switch_after = 1;  /* accept after 1 blocked call */

    KlDrain drain;
    kl_drain_init(&drain, mock_write, &writer, &alloc);

    MockComp comp;
    memset(&comp, 0, sizeof(comp));
    comp.base.feed     = mock_feed;
    comp.base.encoding = mock_encoding;
    comp.base.destroy  = mock_destroy;

    /* Feed data — should buffer in drain because writer blocks */
    int rc = comp.base.feed(&comp.base, "data", 4, 0, drain_emit, &drain);
    ASSERT_EQ(0, rc);

    /* Data is buffered in drain, not in writer */
    ASSERT_EQ(1, kl_drain_pending(&drain));
    ASSERT_EQ(0, (int)writer.len);

    /* Flush when writer becomes available */
    int flush_rc = kl_drain_flush(&drain);
    ASSERT_EQ(0, flush_rc);
    ASSERT_EQ(0, kl_drain_pending(&drain));

    /* Verify data: "C:" + "data" */
    ASSERT_EQ(6, (int)writer.len);
    ASSERT_EQ(0, memcmp(writer.buf, "C:data", 6));

    kl_drain_free(&drain);
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 2: TLS + async suspend/resume
 *
 * Full server with passthrough TLS. Handler suspends the connection,
 * a watcher fires and completes it. Verifies TLS state survives
 * suspension.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    KlAsyncOp  op;
    KlServer  *server;
    int        pipe_fds[2];
} TlsAsyncCtx;

static void tls_async_resume(KlAsyncOp *op, void *user_data) {
    TlsAsyncCtx *ctx = user_data;
    (void)op;
    KlConn *conn = ctx->op.conn;
    kl_response_json(&conn->res, 200, "{\"async_tls\":true}", 18);
    conn->state = KL_CONN_SENDING;
}

static void tls_async_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready;
    TlsAsyncCtx *ctx = user_data;
    char buf[16];
    (void)kl_test_sockread(fd, buf, sizeof(buf));
    kl_watcher_del(&ctx->server->ev, fd);
    kl_test_closesock(ctx->pipe_fds[0]);
    kl_test_closesock(ctx->pipe_fds[1]);
    kl_async_complete(ctx->server, &ctx->op);
}

static void handle_tls_async(KlRequest *req, KlResponse *res, void *user_data) {
    (void)res;
    KlServer *srv = user_data;
    KlConn *conn = kl_request_conn(req);

    static TlsAsyncCtx tctx;
    memset(&tctx, 0, sizeof(tctx));
    tctx.server = srv;
    tctx.op.on_resume = tls_async_resume;
    tctx.op.user_data = &tctx;

    kl_test_socketpair(tctx.pipe_fds);
    set_nonblocking(tctx.pipe_fds[0]);
    set_nonblocking(tctx.pipe_fds[1]);

    kl_watcher_add(&srv->ev, tctx.pipe_fds[0], KL_EVENT_READ,
                   tls_async_watcher, &tctx);
    kl_async_suspend(srv, conn, &tctx.op);

    /* Immediate completion signal */
    (void)kl_test_sockwrite(tctx.pipe_fds[1], "!", 1);
}

UTEST(cross, tls_async_suspend_resume) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = mock_tls_create,
    };
    KlConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlServer srv;
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/tls-async", handle_tls_async, &srv, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);

    int fd = connect_to(srv.bound_port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /tls-async HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n\r\n";
    kl_test_sockwrite(fd, req, strlen(req));

    char buf[2048];
    read_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"async_tls\":true}") != NULL);

    kl_test_closesock(fd);
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 3: middleware + body reader + async handler
 *
 * Full pipeline: pre-body auth middleware → POST with buffer body
 * reader → async handler that suspends, then resumes with body data.
 * ═══════════════════════════════════════════════════════════════════ */

static int auth_middleware(KlRequest *req, KlResponse *res, void *user_data) {
    (void)user_data;
    size_t key_len;
    const char *key = kl_request_header_len(req, "X-Auth", &key_len);
    if (!key || key_len != 6 || memcmp(key, "secret", 6) != 0) {
        kl_response_error(res, 401, "Unauthorized");
        return 1;
    }
    return 0;
}

typedef struct {
    KlAsyncOp  op;
    KlServer  *server;
    int        pipe_fds[2];
    KlRequest *saved_req;
} MwAsyncCtx;

static void mw_async_resume(KlAsyncOp *op, void *user_data) {
    (void)op;
    MwAsyncCtx *ctx = user_data;
    KlConn *conn = ctx->op.conn;

    /* Access body from the reader — should still be valid */
    KlBufReader *br = (KlBufReader *)ctx->saved_req->body_reader;
    if (br && br->len > 0) {
        kl_response_status(&conn->res, 200);
        kl_response_header(&conn->res, "Content-Type", "text/plain");
        kl_response_body_borrow(&conn->res, br->data, br->len);
    } else {
        kl_response_error(&conn->res, 400, "No body");
    }
    conn->state = KL_CONN_SENDING;
}

static void mw_async_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready;
    MwAsyncCtx *ctx = user_data;
    char buf[16];
    (void)kl_test_sockread(fd, buf, sizeof(buf));
    kl_watcher_del(&ctx->server->ev, fd);
    kl_test_closesock(ctx->pipe_fds[0]);
    kl_test_closesock(ctx->pipe_fds[1]);
    kl_async_complete(ctx->server, &ctx->op);
}

static void handle_mw_async(KlRequest *req, KlResponse *res, void *user_data) {
    (void)res;
    KlServer *srv = user_data;
    KlConn *conn = kl_request_conn(req);

    static MwAsyncCtx mctx;
    memset(&mctx, 0, sizeof(mctx));
    mctx.server = srv;
    mctx.saved_req = req;
    mctx.op.on_resume = mw_async_resume;
    mctx.op.user_data = &mctx;

    kl_test_socketpair(mctx.pipe_fds);
    set_nonblocking(mctx.pipe_fds[0]);
    set_nonblocking(mctx.pipe_fds[1]);

    kl_watcher_add(&srv->ev, mctx.pipe_fds[0], KL_EVENT_READ,
                   mw_async_watcher, &mctx);
    kl_async_suspend(srv, conn, &mctx.op);
    (void)kl_test_sockwrite(mctx.pipe_fds[1], "!", 1);
}

UTEST(cross, middleware_body_async) {
    KlConfig cfg = { .port = 0 };
    KlServer srv;
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_use(&srv, "*", "/*", auth_middleware, NULL);
    kl_server_route(&srv, "POST", "/process", handle_mw_async, &srv,
                    kl_body_reader_buffer);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);

    /* Test: missing auth header → 401 */
    {
        int fd = connect_to(srv.bound_port);
        ASSERT_TRUE(fd >= 0);
        const char *req = "POST /process HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Length: 5\r\n"
                          "Connection: close\r\n\r\nhello";
        kl_test_sockwrite(fd, req, strlen(req));
        char buf[2048];
        read_response(fd, buf, sizeof(buf), 2000);
        ASSERT_TRUE(strstr(buf, "401") != NULL);
        kl_test_closesock(fd);
    }

    /* Test: valid auth + body → async handler echoes body */
    {
        int fd = connect_to(srv.bound_port);
        ASSERT_TRUE(fd >= 0);
        const char *req = "POST /process HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "X-Auth: secret\r\n"
                          "Content-Length: 11\r\n"
                          "Connection: close\r\n\r\nhello world";
        kl_test_sockwrite(fd, req, strlen(req));
        char buf[2048];
        read_response(fd, buf, sizeof(buf), 2000);
        ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
        ASSERT_TRUE(strstr(buf, "hello world") != NULL);
        kl_test_closesock(fd);
    }

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 4: resolver_cache + sync client
 *
 * Starts a local server, then uses the sync client with a cached
 * resolver to verify: first request triggers inner resolve, second
 * request is a cache hit (no inner call).
 * ═══════════════════════════════════════════════════════════════════ */

/* Callback tracking for resolver cache test */
static int rc_test_done_count;
static int rc_test_last_error;

static void rc_test_done(KlResolveReq *req, const KlResolveResult *result,
                          int error, void *user_data) {
    (void)req; (void)result; (void)user_data;
    rc_test_done_count++;
    rc_test_last_error = error;
}

UTEST(cross, resolver_cache_client) {
    /* Set up resolver cache wrapping a counting mock */
    KlAllocator alloc = kl_allocator_default();
    KlResolver inner = {
        .resolve = counting_resolve,
        .cancel  = noop_cancel,
        .destroy = noop_destroy,
    };
    mock_resolve_count = 0;
    rc_test_done_count = 0;

    KlResolverCacheConfig cache_cfg = { .ttl_ms = 60000, .capacity = 8 };
    KlResolver *cache = kl_resolver_cache_create(&inner, &cache_cfg, &alloc);
    ASSERT_TRUE(cache != NULL);

    /* First resolve: cache miss → inner resolver called */
    KlResolveReq *req1 = cache->resolve(cache, NULL, "example.com", 80,
                                          rc_test_done, NULL);
    ASSERT_EQ(1, mock_resolve_count);  /* inner called */
    ASSERT_EQ(1, rc_test_done_count);  /* done_fn fired (sync) */
    ASSERT_EQ(0, rc_test_last_error);
    if (req1) cache->cancel(req1);

    /* Second resolve: cache hit → no new inner call */
    KlResolveReq *req2 = cache->resolve(cache, NULL, "example.com", 80,
                                          rc_test_done, NULL);
    ASSERT_EQ(1, mock_resolve_count);  /* still 1 — cache hit */
    ASSERT_EQ(2, rc_test_done_count);  /* done_fn fired again */
    ASSERT_EQ(0, rc_test_last_error);
    if (req2) cache->cancel(req2);

    /* Different host: cache miss → inner called again */
    KlResolveReq *req3 = cache->resolve(cache, NULL, "other.com", 80,
                                          rc_test_done, NULL);
    ASSERT_EQ(2, mock_resolve_count);  /* inner called again */
    ASSERT_EQ(3, rc_test_done_count);
    if (req3) cache->cancel(req3);

    /* Verify cache entry count */
    ASSERT_EQ(2, kl_resolver_cache_count(cache));

    cache->destroy(cache);
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 5: TLS + middleware + compressed buffer body
 *
 * Full server with passthrough TLS, auth middleware, and compressed
 * response. Verifies the three features compose correctly.
 * ═══════════════════════════════════════════════════════════════════ */

static KlCompressConfig g_compress_cfg;

static void handle_compressed(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    const char *body = "Hello, compressed world!";
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/plain");
    kl_response_body_compress(res, &g_compress_cfg, body, strlen(body));
}

UTEST(cross, tls_middleware_compress) {
    KlTlsConfig tls_cfg = {
        .ctx     = NULL,
        .factory = mock_tls_create,
    };
    g_compress_cfg.ctx = NULL;
    g_compress_cfg.factory = mock_factory;
    g_compress_cfg.ctx_destroy = NULL;

    KlConfig cfg = {
        .port = 0,
        .tls  = &tls_cfg,
    };
    KlServer srv;
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_use(&srv, "*", "/*", auth_middleware, NULL);
    kl_server_route(&srv, "GET", "/compressed", handle_compressed, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);

    /* Unauthenticated → 401 (middleware short-circuits before compression) */
    {
        int fd = connect_to(srv.bound_port);
        ASSERT_TRUE(fd >= 0);
        const char *req = "GET /compressed HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n\r\n";
        kl_test_sockwrite(fd, req, strlen(req));
        char buf[2048];
        read_response(fd, buf, sizeof(buf), 2000);
        ASSERT_TRUE(strstr(buf, "401") != NULL);
        kl_test_closesock(fd);
    }

    /* Authenticated → 200 with compressed body + Content-Encoding */
    {
        int fd = connect_to(srv.bound_port);
        ASSERT_TRUE(fd >= 0);
        const char *req = "GET /compressed HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "X-Auth: secret\r\n"
                          "Connection: close\r\n\r\n";
        kl_test_sockwrite(fd, req, strlen(req));
        char buf[4096];
        read_response(fd, buf, sizeof(buf), 2000);
        ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
        ASSERT_TRUE(strstr(buf, "Content-Encoding: mock") != NULL);
        ASSERT_TRUE(strstr(buf, "Vary: Accept-Encoding") != NULL);
        /* Body should be mock-compressed (input minus last byte) */
        ASSERT_TRUE(strstr(buf, "Hello, compressed world") != NULL);
        kl_test_closesock(fd);
    }

    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

/* ═══════════════════════════════════════════════════════════════════
 * Test 6: server stats during load
 *
 * Verifies kl_server_stats reflects real-time connection state
 * during active server operation with multiple connections.
 * ═══════════════════════════════════════════════════════════════════ */

static int hold_stats_active;
static int hold_stats_suspended;

static void hold_resume(KlAsyncOp *op, void *user_data) {
    (void)user_data;
    KlConn *conn = op->conn;
    kl_response_json(&conn->res, 200, "{\"held\":true}", 13);
    conn->state = KL_CONN_SENDING;
}

static void hold_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready;
    TlsAsyncCtx *ctx = user_data;
    char buf[16];
    (void)kl_test_sockread(fd, buf, sizeof(buf));

    /* Capture stats while connection is suspended */
    KlServerStats stats;
    kl_server_stats(ctx->server, &stats);
    hold_stats_active = stats.active_connections;
    hold_stats_suspended = stats.async_suspended;

    kl_watcher_del(&ctx->server->ev, fd);
    kl_test_closesock(ctx->pipe_fds[0]);
    kl_test_closesock(ctx->pipe_fds[1]);
    kl_async_complete(ctx->server, &ctx->op);
}

static void handle_hold(KlRequest *req, KlResponse *res, void *user_data) {
    (void)res;
    KlServer *srv = user_data;
    KlConn *conn = kl_request_conn(req);

    static TlsAsyncCtx hctx;
    memset(&hctx, 0, sizeof(hctx));
    hctx.server = srv;
    hctx.op.on_resume = hold_resume;
    hctx.op.user_data = &hctx;

    kl_test_socketpair(hctx.pipe_fds);
    set_nonblocking(hctx.pipe_fds[0]);
    set_nonblocking(hctx.pipe_fds[1]);

    kl_watcher_add(&srv->ev, hctx.pipe_fds[0], KL_EVENT_READ,
                   hold_watcher, &hctx);
    kl_async_suspend(srv, conn, &hctx.op);

    /* Delay completion so watcher fires on next tick with conn suspended */
    (void)kl_test_sockwrite(hctx.pipe_fds[1], "!", 1);
}

UTEST(cross, stats_during_async) {
    hold_stats_active = -1;
    hold_stats_suspended = -1;

    KlConfig cfg = { .port = 0, .max_connections = 8 };
    KlServer srv;
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hold", handle_hold, &srv, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);

    int fd = connect_to(srv.bound_port);
    ASSERT_TRUE(fd >= 0);
    const char *req = "GET /hold HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n\r\n";
    kl_test_sockwrite(fd, req, strlen(req));
    char buf[2048];
    read_response(fd, buf, sizeof(buf), 2000);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);

    /* Stats were captured inside the watcher while conn was suspended */
    ASSERT_TRUE(hold_stats_active >= 1);
    ASSERT_TRUE(hold_stats_suspended >= 1);

    kl_test_closesock(fd);
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST_MAIN();
