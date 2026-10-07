#include "utest.h"
#include <keel/keel.h>
#include <keel/http_client_pool.h>
#include <keel/socket.h>

#include <limits.h>
#include <string.h>
#include "net_compat.h"
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include "mock_tls.h"
#include <errno.h>

/* ── Unit tests: pool init/free ──────────────────────────────────── */

UTEST(cpool, init_defaults) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;

    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);
    ASSERT_EQ(pool.capacity, KL_HTTP_CLIENT_POOL_DEFAULT_CAPACITY);
    ASSERT_EQ(pool.max_per_host, KL_HTTP_CLIENT_POOL_DEFAULT_MAX_PER_HOST);
    ASSERT_EQ(pool.idle_ms, (uint64_t)KL_HTTP_CLIENT_POOL_DEFAULT_IDLE_MS);
    ASSERT_EQ(pool.active, 0);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 0);

    kl_http_client_pool_free(&pool);
}

/* A freed pool is empty: no capacity over a table that is gone, so a later call finds nothing to
 * walk and a second free is harmless. */
UTEST(cpool, free_clears_capacity) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);
    kl_http_client_pool_free(&pool);
    ASSERT_TRUE(pool.entries == NULL);
    ASSERT_EQ(pool.capacity, 0);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 0);
    kl_http_client_pool_free(&pool);
}

/* The pooled path must refuse an https request whose TLS config has no factory too: going ahead
 * would send it in plaintext and file the plain connection in the pool as a TLS one. */
UTEST(cpool, pooled_https_tls_without_factory) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, &ev), 0);
    KlTlsConfig tls = { .ctx = NULL, .factory = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tls = &tls;

    KlHttpClient *c = kl_http_client_start_pooled(&pool, &ev, &a, &cfg, "GET",
                                                  "https://127.0.0.1:1/", NULL, 0, NULL, 0,
                                                  NULL, NULL);
    int refused = (c == NULL);
    kl_http_client_free(c);
    kl_http_client_pool_free(&pool);
    kl_event_ctx_free(&ev);
    ASSERT_TRUE(refused);
}

/* A pooled start whose configured provider the ctx's loop cannot drive is refused, and the refusal
 * leaves the caller's shared ctx as it was: the provider was written to ctx->sockets before the
 * check, so every later start on the ctx without a provider of its own was refused too. */
static const KlSocketOps g_unwatchable_ops = { .name = "unwatchable" };
static const KlSocketProvider g_unwatchable_provider = {
    &g_unwatchable_ops, NULL, KL_SOCK_CAP_WRITEV, NULL,     /* no NATIVE_FD: not watchable */
};

UTEST(cpool, refused_provider_leaves_ctx_sockets_unchanged) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, &ev), 0);
    const KlSocketProvider *before = ev.sockets;
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sockets = &g_unwatchable_provider;

    KlHttpClient *c = kl_http_client_start_pooled(&pool, &ev, &a, &cfg, "GET",
                                                  "http://127.0.0.1:1/", NULL, 0, NULL, 0,
                                                  NULL, NULL);
    int refused = (c == NULL);
    const KlSocketProvider *after = ev.sockets;  /* a completion loop adopts its native one */
    kl_http_client_free(c);

    memset(&cfg, 0, sizeof cfg);                 /* no provider of its own: the ctx's is used */
    KlHttpClient *c2 = kl_http_client_start_pooled(&pool, &ev, &a, &cfg, "GET",
                                                   "http://127.0.0.1:1/", NULL, 0, NULL, 0,
                                                   NULL, NULL);
    int second_ok = (c2 != NULL);
    kl_http_client_free(c2);
    kl_http_client_pool_free(&pool);
    kl_event_ctx_free(&ev);
    if (refused) ASSERT_TRUE(after == before);   /* was: the unwatchable provider */
    ASSERT_TRUE(after != &g_unwatchable_provider);
    ASSERT_TRUE(second_ok);                      /* was: refused, the ctx kept the bad provider */
}

UTEST(cpool, init_custom) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    KlHttpClientPoolConfig cfg = { .capacity = 8, .max_per_host = 2, .idle_ms = 5000 };

    ASSERT_EQ(kl_http_client_pool_init(&pool, &cfg, &a, NULL), 0);
    ASSERT_EQ(pool.capacity, 8);
    ASSERT_EQ(pool.max_per_host, 2);
    ASSERT_EQ(pool.idle_ms, (uint64_t)5000);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, init_null_args) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;

    ASSERT_EQ(kl_http_client_pool_init(NULL, NULL, &a, NULL), -1);
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, NULL, NULL), -1);
}

UTEST(cpool, init_negative_capacity) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    KlHttpClientPoolConfig cfg = { .capacity = -1, .max_per_host = 1, .idle_ms = 1000 };

    /* Negative capacity should use the default (positive) */
    ASSERT_EQ(kl_http_client_pool_init(&pool, &cfg, &a, NULL), 0);
    ASSERT_EQ(pool.capacity, KL_HTTP_CLIENT_POOL_DEFAULT_CAPACITY);
    kl_http_client_pool_free(&pool);
}

UTEST(cpool, free_null) {
    kl_http_client_pool_free(NULL);  /* should not crash */
    ASSERT_TRUE(1);
}

/* ── Unit tests: acquire/release ─────────────────────────────────── */

UTEST(cpool, acquire_empty_miss) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    KlHttpClientPoolConn conn;
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, "example.com", 80, 0, NULL, 0, &conn), 1);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, release_then_acquire) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    /* Create a socketpair to get a valid, testable fd */
    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    /* Release one end into the pool */
    KlHttpClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &conn, "example.com", 80, 0, NULL, 0), 0);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 1);
    ASSERT_EQ(kl_http_client_pool_host_count(&pool, "example.com", 80, 0, NULL, 0), 1);

    /* Acquire: should hit */
    KlHttpClientPoolConn acq;
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, "example.com", 80, 0, NULL, 0, &acq), 0);
    ASSERT_EQ(acq.reused, 1);
    ASSERT_EQ(acq.fd, fds[0]);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 0);

    kl_test_closesock(acq.fd);
    kl_test_closesock(fds[1]);
    kl_http_client_pool_free(&pool);
}

UTEST(cpool, acquire_wrong_host) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    KlHttpClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &conn, "host-a.com", 80, 0, NULL, 0), 0);

    KlHttpClientPoolConn acq;
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, "host-b.com", 80, 0, NULL, 0, &acq), 1);  /* miss */

    kl_http_client_pool_free(&pool);
    kl_test_closesock(fds[1]);
}

UTEST(cpool, acquire_wrong_port) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    KlHttpClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &conn, "example.com", 80, 0, NULL, 0), 0);

    KlHttpClientPoolConn acq;
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, "example.com", 443, 0, NULL, 0, &acq), 1);  /* miss */

    kl_http_client_pool_free(&pool);
    kl_test_closesock(fds[1]);
}

UTEST(cpool, acquire_wrong_tls) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    KlHttpClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &conn, "example.com", 443, 0, NULL, 0), 0);

    KlHttpClientPoolConn acq;
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, "example.com", 443, 1, NULL, 0, &acq), 1);  /* miss */

    kl_http_client_pool_free(&pool);
    kl_test_closesock(fds[1]);
}

/* A TLS connection is reused only under the config it was made with (ctx + factory); the legacy
 * acquire, which carries no config, does not match it either. */
static int g_unit_ctx_a, g_unit_ctx_b;
static KlTls *unit_factory(KlTlsCtx *ctx, KlAllocator *alloc) { (void)ctx; (void)alloc; return NULL; }
UTEST(cpool, acquire_tls_matches_only_its_config) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);
    KlTlsConfig ta = { .ctx = (KlTlsCtx *)&g_unit_ctx_a, .factory = unit_factory };
    KlTlsConfig tb = { .ctx = (KlTlsCtx *)&g_unit_ctx_b, .factory = unit_factory };

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    KlHttpClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release_tls(&pool, &conn, "example.com", 443, &ta, NULL, 0), 0);

    KlHttpClientPoolConn acq;
    ASSERT_EQ(kl_http_client_pool_acquire_tls(&pool, "example.com", 443, &tb, NULL, 0, &acq), 1);
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, "example.com", 443, 1, NULL, 0, &acq), 1);
    ASSERT_EQ(kl_http_client_pool_host_count(&pool, "example.com", 443, 1, NULL, 0), 1);
    ASSERT_EQ(kl_http_client_pool_acquire_tls(&pool, "example.com", 443, &ta, NULL, 0, &acq), 0);
    ASSERT_EQ((int)acq.fd, fds[0]);

    kl_test_closesock(fds[0]);
    kl_http_client_pool_free(&pool);
    kl_test_closesock(fds[1]);
}

UTEST(cpool, max_per_host_evicts_oldest) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    KlHttpClientPoolConfig cfg = { .capacity = 8, .max_per_host = 2, .idle_ms = 60000 };
    ASSERT_EQ(kl_http_client_pool_init(&pool, &cfg, &a, NULL), 0);

    /* Create 3 socketpairs */
    int fds[3][2];
    for (int i = 0; i < 3; i++)
        ASSERT_EQ(kl_test_socketpair(fds[i]), 0);

    /* Release 3 connections for same host (max_per_host=2) */
    for (int i = 0; i < 3; i++) {
        KlHttpClientPoolConn conn = { .fd = fds[i][0], .tls = NULL, .reused = 0, ._entry = NULL };
        ASSERT_EQ(kl_http_client_pool_release(&pool, &conn, "example.com", 80, 0, NULL, 0), 0);
    }

    /* Only 2 should remain (oldest evicted) */
    ASSERT_EQ(kl_http_client_pool_host_count(&pool, "example.com", 80, 0, NULL, 0), 2);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 2);

    for (int i = 0; i < 3; i++)
        kl_test_closesock(fds[i][1]);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, pool_full_evicts_lru) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    KlHttpClientPoolConfig cfg = { .capacity = 2, .max_per_host = 4, .idle_ms = 60000 };
    ASSERT_EQ(kl_http_client_pool_init(&pool, &cfg, &a, NULL), 0);

    /* Fill pool */
    int fds[3][2];
    for (int i = 0; i < 3; i++)
        ASSERT_EQ(kl_test_socketpair(fds[i]), 0);

    char host[32];
    for (int i = 0; i < 2; i++) {
        snprintf(host, sizeof(host), "host%d.com", i);
        KlHttpClientPoolConn conn = { .fd = fds[i][0], .tls = NULL, .reused = 0, ._entry = NULL };
        ASSERT_EQ(kl_http_client_pool_release(&pool, &conn, host, 80, 0, NULL, 0), 0);
    }
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 2);

    /* Release one more: should evict oldest */
    KlHttpClientPoolConn conn3 = { .fd = fds[2][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &conn3, "host2.com", 80, 0, NULL, 0), 0);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 2);

    for (int i = 0; i < 3; i++)
        kl_test_closesock(fds[i][1]);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, discard_closes_fd) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    KlHttpClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    kl_http_client_pool_discard(&pool, &conn);
    ASSERT_EQ(conn.fd, -1);

    /* Verify fd is closed: write should fail */
    char c = 'x';
    ASSERT_TRUE(kl_test_sockwrite(fds[0], &c, 1) < 0);

    kl_test_closesock(fds[1]);
    kl_http_client_pool_free(&pool);
}

UTEST(cpool, idle_count) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 0);

    int fds[2][2];
    for (int i = 0; i < 2; i++)
        ASSERT_EQ(kl_test_socketpair(fds[i]), 0);

    KlHttpClientPoolConn c1 = { .fd = fds[0][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &c1, "a.com", 80, 0, NULL, 0), 0);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 1);
    ASSERT_EQ(kl_http_client_pool_host_count(&pool, "a.com", 80, 0, NULL, 0), 1);

    KlHttpClientPoolConn c2 = { .fd = fds[1][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &c2, "b.com", 80, 0, NULL, 0), 0);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 2);

    /* Acquire one */
    KlHttpClientPoolConn acq;
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, "a.com", 80, 0, NULL, 0, &acq), 0);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 1);

    /* Discard acquired */
    kl_http_client_pool_discard(&pool, &acq);

    for (int i = 0; i < 2; i++)
        kl_test_closesock(fds[i][1]);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, evict_expired) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    KlHttpClientPoolConfig cfg = { .capacity = 4, .max_per_host = 4, .idle_ms = 1 };
    ASSERT_EQ(kl_http_client_pool_init(&pool, &cfg, &a, NULL), 0);

    int fds[2][2];
    for (int i = 0; i < 2; i++)
        ASSERT_EQ(kl_test_socketpair(fds[i]), 0);

    KlHttpClientPoolConn c1 = { .fd = fds[0][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &c1, "a.com", 80, 0, NULL, 0), 0);

    /* Wait for expiry */
    kl_test_sleep_ms(5);

    /* Release a second (fresh) one */
    KlHttpClientPoolConn c2 = { .fd = fds[1][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &c2, "b.com", 80, 0, NULL, 0), 0);

    int evicted = kl_http_client_pool_evict_expired(&pool);
    ASSERT_TRUE(evicted >= 1);
    /* The fresh connection may or may not have expired depending on timing */
    ASSERT_TRUE(kl_http_client_pool_idle_count(&pool) <= 1);

    for (int i = 0; i < 2; i++)
        kl_test_closesock(fds[i][1]);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, stale_detection) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    /* Release one end */
    KlHttpClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &conn, "example.com", 80, 0, NULL, 0), 0);

    /* Close the other end (simulate server disconnect) */
    kl_test_closesock(fds[1]);
    kl_test_sleep_ms(10);  /* let OS propagate the close */

    /* Acquire should detect stale and return miss */
    KlHttpClientPoolConn acq;
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, "example.com", 80, 0, NULL, 0, &acq), 1);
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 0);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, hostname_too_long) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    /* Build a hostname that's too long */
    char long_host[KL_HTTP_CLIENT_HOSTNAME_MAX + 10];
    memset(long_host, 'a', sizeof(long_host) - 1);
    long_host[sizeof(long_host) - 1] = '\0';

    KlHttpClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &conn, long_host, 80, 0, NULL, 0), -1);

    /* fd should be closed by discard inside release */
    kl_test_closesock(fds[1]);
    kl_http_client_pool_free(&pool);
}

UTEST(cpool, acquire_null_args) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    KlHttpClientPoolConn conn;
    ASSERT_EQ(kl_http_client_pool_acquire(NULL, "x", 80, 0, NULL, 0, &conn), -1);
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, NULL, 80, 0, NULL, 0, &conn), -1);
    ASSERT_EQ(kl_http_client_pool_acquire(&pool, "x", 80, 0, NULL, 0, NULL), -1);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, discard_null) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    kl_http_client_pool_discard(&pool, NULL);  /* should not crash */
    ASSERT_TRUE(1);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, host_count_null) {
    ASSERT_EQ(kl_http_client_pool_host_count(NULL, "x", 80, 0, NULL, 0), 0);
    ASSERT_EQ(kl_http_client_pool_idle_count(NULL), 0);
}

/* ── Integration tests: pooled sync requests ─────────────────────── */

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static void server_thread_fn(void *arg) {
    kl_http_server_run((KlHttpServer *)arg);
}

static void wait_for_bind(KlHttpServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) kl_test_sleep_ms(10);
}

UTEST(cpool, sync_pooled_reuse) {
    /* Start a real server */
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", srv.bound_port);

    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);

    /* First request: pool miss, establishes connection */
    KlHttpClientResponse resp1;
    ASSERT_EQ(kl_http_client_request_pooled(&pool, &a, NULL, "GET", url,
                                         NULL, 0, NULL, 0, &resp1), 0);
    ASSERT_EQ(resp1.status, 200);
    kl_http_client_response_free(&resp1);

    /* Connection should now be in pool */
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 1);

    /* Second request: pool hit, reuses connection */
    KlHttpClientResponse resp2;
    ASSERT_EQ(kl_http_client_request_pooled(&pool, &a, NULL, "GET", url,
                                         NULL, 0, NULL, 0, &resp2), 0);
    ASSERT_EQ(resp2.status, 200);
    kl_http_client_response_free(&resp2);

    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 1);

    kl_http_client_pool_free(&pool);
    kl_http_server_stop(&srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&srv);
}

typedef struct { int done; int status; } AsyncPoolCtx;

static void async_pool_done(KlHttpClient *cl, void *ud) {
    AsyncPoolCtx *ax = ud;
    const KlHttpClientResponse *r = kl_http_client_response(cl);
    ax->status = r ? r->status : -1;
    ax->done = 1;
}

UTEST(cpool, async_pooled_reuse) {
    /* Start a real server */
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    kl_http_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    KlPlatThread tid;
    kl_plat_thread_create(&tid, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", srv.bound_port);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, &ev), 0);

    /* Async request 1 */
    AsyncPoolCtx ctx1 = { 0, 0 };

    KlHttpClient *c1 = kl_http_client_start_pooled(&pool, &ev, &a, NULL, "GET", url,
                                             NULL, 0, NULL, 0,
                                             async_pool_done, &ctx1);
    /* Run event loop until done */
    if (c1) {
        for (int i = 0; i < 1000 && !ctx1.done; i++) {
            kl_event_ctx_run(&ev, 16, 10);
            kl_timer_fire(&ev);
        }
        ASSERT_TRUE(ctx1.done);
        ASSERT_EQ(ctx1.status, 200);
        kl_http_client_free(c1);
    }

    /* Connection should be in pool */
    ASSERT_EQ(kl_http_client_pool_idle_count(&pool), 1);

    /* Async request 2: should reuse */
    AsyncPoolCtx ctx2 = { 0, 0 };
    KlHttpClient *c2 = kl_http_client_start_pooled(&pool, &ev, &a, NULL, "GET", url,
                                             NULL, 0, NULL, 0,
                                             async_pool_done, &ctx2);
    if (c2) {
        for (int i = 0; i < 1000 && !ctx2.done; i++) {
            kl_event_ctx_run(&ev, 16, 10);
            kl_timer_fire(&ev);
        }
        ASSERT_TRUE(ctx2.done);
        ASSERT_EQ(ctx2.status, 200);
        kl_http_client_free(c2);
    }

    kl_http_client_pool_free(&pool);
    kl_event_ctx_free(&ev);
    kl_http_server_stop(&srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&srv);
}

/* ── Pooled TLS: the connection is keyed by the TLS config ────────────────
 * A TLS connection was pooled under (host, port, is_tls) only, so a request made under one TLS
 * config could reuse a connection made under another: one with verification off, or another mTLS
 * identity. Two configs here share the identity mock factory but not their ctx (the context that
 * would hold the trust store and client identity). Request A, then B, then A again: B must open its
 * own connection, and the second A must reuse A's, leaving two idle connections. */
static int g_ctx_a, g_ctx_b;   /* distinct addresses stand in for two real TLS contexts */
static KlTlsConfig g_tls_a = { .ctx = (KlTlsCtx *)&g_ctx_a, .factory = mock_tls_create };
static KlTlsConfig g_tls_b = { .ctx = (KlTlsCtx *)&g_ctx_b, .factory = mock_tls_create };
static KlTlsConfig g_tls_srv = { .ctx = NULL, .factory = mock_tls_create };
static KlHttpServer g_tls_pool_srv;
static KlPlatThread g_tls_pool_tid;

static int tls_pool_server_start(void) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 8, .tls = &g_tls_srv };
    if (kl_http_server_init(&g_tls_pool_srv, &cfg) != 0) return -1;
    kl_http_server_route(&g_tls_pool_srv, "GET", "/hello", handle_hello, NULL, NULL);
    kl_plat_thread_create(&g_tls_pool_tid, server_thread_fn, &g_tls_pool_srv);
    wait_for_bind(&g_tls_pool_srv);
    return g_tls_pool_srv.bound_port > 0 ? 0 : -1;
}
static void tls_pool_server_stop(void) {
    kl_http_server_stop(&g_tls_pool_srv);
    kl_plat_thread_join(&g_tls_pool_tid);
    kl_http_server_free(&g_tls_pool_srv);
}

UTEST(cpool, sync_pooled_tls_keyed_by_config) {
    ASSERT_EQ(tls_pool_server_start(), 0);
    char url[128];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/hello", g_tls_pool_srv.bound_port);
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    int ok = kl_http_client_pool_init(&pool, NULL, &a, NULL) == 0;
    KlHttpClientConfig ca = { .tls = &g_tls_a, .timeout_ms = 2000 };
    KlHttpClientConfig cb = { .tls = &g_tls_b, .timeout_ms = 2000 };
    int st[3] = { 0, 0, 0 };
    int idle_after_b = -1, idle_after_a2 = -1;
    const KlHttpClientConfig *seq[3] = { &ca, &cb, &ca };
    for (int i = 0; ok && i < 3; i++) {
        KlHttpClientResponse r;
        memset(&r, 0, sizeof r);
        if (kl_http_client_request_pooled(&pool, &a, seq[i], "GET", url, NULL, 0, NULL, 0, &r) == 0)
            st[i] = r.status;
        kl_http_client_response_free(&r);
        if (i == 1) idle_after_b = kl_http_client_pool_idle_count(&pool);
        if (i == 2) idle_after_a2 = kl_http_client_pool_idle_count(&pool);
    }
    if (ok) kl_http_client_pool_free(&pool);
    tls_pool_server_stop();
    ASSERT_TRUE(ok);
    ASSERT_EQ(st[0], 200);
    ASSERT_EQ(st[1], 200);
    ASSERT_EQ(st[2], 200);
    ASSERT_EQ(idle_after_b, 2);    /* was 1: B reused A's connection */
    ASSERT_EQ(idle_after_a2, 2);   /* A's connection reused by A */
}

UTEST(cpool, async_pooled_tls_keyed_by_config) {
    ASSERT_EQ(tls_pool_server_start(), 0);
    char url[128];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/hello", g_tls_pool_srv.bound_port);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    int ok = kl_event_ctx_init(&ev, &a) == 0;
    KlHttpClientPool pool;
    if (ok) ok = kl_http_client_pool_init(&pool, NULL, &a, &ev) == 0;
    KlHttpClientConfig ca = { .tls = &g_tls_a, .timeout_ms = 2000 };
    KlHttpClientConfig cb = { .tls = &g_tls_b, .timeout_ms = 2000 };
    const KlHttpClientConfig *seq[3] = { &ca, &cb, &ca };
    int st[3] = { 0, 0, 0 };
    int idle_after_b = -1, idle_after_a2 = -1;
    for (int i = 0; ok && i < 3; i++) {
        AsyncPoolCtx x = { 0, 0 };
        KlHttpClient *c = kl_http_client_start_pooled(&pool, &ev, &a, seq[i], "GET", url,
                                                      NULL, 0, NULL, 0, async_pool_done, &x);
        for (int k = 0; c && k < 300 && !x.done; k++) kl_event_ctx_run(&ev, 16, 10);
        st[i] = x.status;
        kl_http_client_free(c);
        if (i == 1) idle_after_b = kl_http_client_pool_idle_count(&pool);
        if (i == 2) idle_after_a2 = kl_http_client_pool_idle_count(&pool);
    }
    if (ok) {
        kl_http_client_pool_free(&pool);
        kl_event_ctx_free(&ev);
    }
    tls_pool_server_stop();
    ASSERT_TRUE(ok);
    ASSERT_EQ(st[0], 200);
    ASSERT_EQ(st[1], 200);
    ASSERT_EQ(st[2], 200);
    ASSERT_EQ(idle_after_b, 2);
    ASSERT_EQ(idle_after_a2, 2);
}

/* ── Pooled sync: input validation ───────────────────────────────── */

UTEST(cpool, sync_pooled_null_args) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_client_request_pooled(NULL, &a, NULL, "GET", "http://x",
                                         NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_http_client_request_pooled(&pool, NULL, NULL, "GET", "http://x",
                                         NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_http_client_request_pooled(&pool, &a, NULL, NULL, "http://x",
                                         NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_http_client_request_pooled(&pool, &a, NULL, "GET", NULL,
                                         NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_http_client_request_pooled(&pool, &a, NULL, "GET", "http://x",
                                         NULL, 0, NULL, 0, NULL), -1);

    kl_http_client_pool_free(&pool);
}

UTEST(cpool, async_pooled_null_args) {
    ASSERT_TRUE(kl_http_client_start_pooled(NULL, NULL, NULL, NULL,
                                         "GET", "http://x",
                                         NULL, 0, NULL, 0,
                                         NULL, NULL) == NULL);
}

/* An idle pooled connection that has unsolicited bytes waiting (a stray response, a 408 before the
 * server closes) must not be reused: the next request would read them as its response. */
UTEST(cpool, idle_connection_with_pending_bytes_is_not_reused) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);
    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    KlHttpClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_http_client_pool_release(&pool, &conn, "example.com", 80, 0, NULL, 0), 0);
    ASSERT_EQ(kl_test_sockwrite(fds[1], "HTTP/1.1 408", 12), 12);
    kl_test_sleep_ms(20);
    KlHttpClientPoolConn acq;
    int r = kl_http_client_pool_acquire(&pool, "example.com", 80, 0, NULL, 0, &acq);
    if (r == 0) kl_test_closesock((int)acq.fd);
    int idle = kl_http_client_pool_idle_count(&pool);
    kl_http_client_pool_free(&pool);
    kl_test_closesock(fds[1]);
    ASSERT_EQ(r, 1);               /* was 0: handed out with the stray bytes queued */
    ASSERT_EQ(idle, 0);            /* and discarded, not left in the pool */
}

UTEST_MAIN();
