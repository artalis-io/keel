#include "utest.h"
#include <keel/keel.h>
#include <keel/client_pool.h>

#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>

/* ── Unit tests: pool init/free ──────────────────────────────────── */

UTEST(cpool, init_defaults) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;

    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);
    ASSERT_EQ(pool.capacity, KL_CPOOL_DEFAULT_CAPACITY);
    ASSERT_EQ(pool.max_per_host, KL_CPOOL_DEFAULT_MAX_PER_HOST);
    ASSERT_EQ(pool.idle_ms, (uint64_t)KL_CPOOL_DEFAULT_IDLE_MS);
    ASSERT_EQ(pool.active, 0);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 0);

    kl_cpool_free(&pool);
}

UTEST(cpool, init_custom) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    KlClientPoolConfig cfg = { .capacity = 8, .max_per_host = 2, .idle_ms = 5000 };

    ASSERT_EQ(kl_cpool_init(&pool, &cfg, &a, NULL), 0);
    ASSERT_EQ(pool.capacity, 8);
    ASSERT_EQ(pool.max_per_host, 2);
    ASSERT_EQ(pool.idle_ms, (uint64_t)5000);

    kl_cpool_free(&pool);
}

UTEST(cpool, init_null_args) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;

    ASSERT_EQ(kl_cpool_init(NULL, NULL, &a, NULL), -1);
    ASSERT_EQ(kl_cpool_init(&pool, NULL, NULL, NULL), -1);
}

UTEST(cpool, init_negative_capacity) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    KlClientPoolConfig cfg = { .capacity = -1, .max_per_host = 1, .idle_ms = 1000 };

    /* Negative capacity should use the default (positive) */
    ASSERT_EQ(kl_cpool_init(&pool, &cfg, &a, NULL), 0);
    ASSERT_EQ(pool.capacity, KL_CPOOL_DEFAULT_CAPACITY);
    kl_cpool_free(&pool);
}

UTEST(cpool, free_null) {
    kl_cpool_free(NULL);  /* should not crash */
    ASSERT_TRUE(1);
}

/* ── Unit tests: acquire/release ─────────────────────────────────── */

UTEST(cpool, acquire_empty_miss) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    KlClientPoolConn conn;
    ASSERT_EQ(kl_cpool_acquire(&pool, "example.com", 80, 0, &conn), 1);

    kl_cpool_free(&pool);
}

UTEST(cpool, release_then_acquire) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    /* Create a socketpair to get a valid, testable fd */
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    /* Release one end into the pool */
    KlClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn, "example.com", 80, 0), 0);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 1);
    ASSERT_EQ(kl_cpool_host_count(&pool, "example.com", 80, 0), 1);

    /* Acquire — should hit */
    KlClientPoolConn acq;
    ASSERT_EQ(kl_cpool_acquire(&pool, "example.com", 80, 0, &acq), 0);
    ASSERT_EQ(acq.reused, 1);
    ASSERT_EQ(acq.fd, fds[0]);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 0);

    close(acq.fd);
    close(fds[1]);
    kl_cpool_free(&pool);
}

UTEST(cpool, acquire_wrong_host) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    KlClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn, "host-a.com", 80, 0), 0);

    KlClientPoolConn acq;
    ASSERT_EQ(kl_cpool_acquire(&pool, "host-b.com", 80, 0, &acq), 1);  /* miss */

    kl_cpool_free(&pool);
    close(fds[1]);
}

UTEST(cpool, acquire_wrong_port) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    KlClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn, "example.com", 80, 0), 0);

    KlClientPoolConn acq;
    ASSERT_EQ(kl_cpool_acquire(&pool, "example.com", 443, 0, &acq), 1);  /* miss */

    kl_cpool_free(&pool);
    close(fds[1]);
}

UTEST(cpool, acquire_wrong_tls) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    KlClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn, "example.com", 443, 0), 0);

    KlClientPoolConn acq;
    ASSERT_EQ(kl_cpool_acquire(&pool, "example.com", 443, 1, &acq), 1);  /* miss */

    kl_cpool_free(&pool);
    close(fds[1]);
}

UTEST(cpool, max_per_host_evicts_oldest) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    KlClientPoolConfig cfg = { .capacity = 8, .max_per_host = 2, .idle_ms = 60000 };
    ASSERT_EQ(kl_cpool_init(&pool, &cfg, &a, NULL), 0);

    /* Create 3 socketpairs */
    int fds[3][2];
    for (int i = 0; i < 3; i++)
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]), 0);

    /* Release 3 connections for same host (max_per_host=2) */
    for (int i = 0; i < 3; i++) {
        KlClientPoolConn conn = { .fd = fds[i][0], .tls = NULL, .reused = 0, ._entry = NULL };
        ASSERT_EQ(kl_cpool_release(&pool, &conn, "example.com", 80, 0), 0);
    }

    /* Only 2 should remain (oldest evicted) */
    ASSERT_EQ(kl_cpool_host_count(&pool, "example.com", 80, 0), 2);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 2);

    for (int i = 0; i < 3; i++)
        close(fds[i][1]);

    kl_cpool_free(&pool);
}

UTEST(cpool, pool_full_evicts_lru) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    KlClientPoolConfig cfg = { .capacity = 2, .max_per_host = 4, .idle_ms = 60000 };
    ASSERT_EQ(kl_cpool_init(&pool, &cfg, &a, NULL), 0);

    /* Fill pool */
    int fds[3][2];
    for (int i = 0; i < 3; i++)
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]), 0);

    char host[32];
    for (int i = 0; i < 2; i++) {
        snprintf(host, sizeof(host), "host%d.com", i);
        KlClientPoolConn conn = { .fd = fds[i][0], .tls = NULL, .reused = 0, ._entry = NULL };
        ASSERT_EQ(kl_cpool_release(&pool, &conn, host, 80, 0), 0);
    }
    ASSERT_EQ(kl_cpool_idle_count(&pool), 2);

    /* Release one more — should evict oldest */
    KlClientPoolConn conn3 = { .fd = fds[2][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn3, "host2.com", 80, 0), 0);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 2);

    for (int i = 0; i < 3; i++)
        close(fds[i][1]);

    kl_cpool_free(&pool);
}

UTEST(cpool, discard_closes_fd) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    KlClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    kl_cpool_discard(&pool, &conn);
    ASSERT_EQ(conn.fd, -1);

    /* Verify fd is closed — write should fail */
    char c = 'x';
    ASSERT_TRUE(write(fds[0], &c, 1) < 0);

    close(fds[1]);
    kl_cpool_free(&pool);
}

UTEST(cpool, idle_count) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    ASSERT_EQ(kl_cpool_idle_count(&pool), 0);

    int fds[2][2];
    for (int i = 0; i < 2; i++)
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]), 0);

    KlClientPoolConn c1 = { .fd = fds[0][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &c1, "a.com", 80, 0), 0);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 1);
    ASSERT_EQ(kl_cpool_host_count(&pool, "a.com", 80, 0), 1);

    KlClientPoolConn c2 = { .fd = fds[1][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &c2, "b.com", 80, 0), 0);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 2);

    /* Acquire one */
    KlClientPoolConn acq;
    ASSERT_EQ(kl_cpool_acquire(&pool, "a.com", 80, 0, &acq), 0);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 1);

    /* Discard acquired */
    kl_cpool_discard(&pool, &acq);

    for (int i = 0; i < 2; i++)
        close(fds[i][1]);

    kl_cpool_free(&pool);
}

UTEST(cpool, evict_expired) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    KlClientPoolConfig cfg = { .capacity = 4, .max_per_host = 4, .idle_ms = 1 };
    ASSERT_EQ(kl_cpool_init(&pool, &cfg, &a, NULL), 0);

    int fds[2][2];
    for (int i = 0; i < 2; i++)
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]), 0);

    KlClientPoolConn c1 = { .fd = fds[0][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &c1, "a.com", 80, 0), 0);

    /* Wait for expiry */
    usleep(5000);

    /* Release a second (fresh) one */
    KlClientPoolConn c2 = { .fd = fds[1][0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &c2, "b.com", 80, 0), 0);

    int evicted = kl_cpool_evict_expired(&pool);
    ASSERT_TRUE(evicted >= 1);
    /* The fresh connection may or may not have expired depending on timing */
    ASSERT_TRUE(kl_cpool_idle_count(&pool) <= 1);

    for (int i = 0; i < 2; i++)
        close(fds[i][1]);

    kl_cpool_free(&pool);
}

UTEST(cpool, stale_detection) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    /* Release one end */
    KlClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn, "example.com", 80, 0), 0);

    /* Close the other end (simulate server disconnect) */
    close(fds[1]);
    usleep(10000);  /* let OS propagate the close */

    /* Acquire should detect stale and return miss */
    KlClientPoolConn acq;
    ASSERT_EQ(kl_cpool_acquire(&pool, "example.com", 80, 0, &acq), 1);
    ASSERT_EQ(kl_cpool_idle_count(&pool), 0);

    kl_cpool_free(&pool);
}

UTEST(cpool, hostname_too_long) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    /* Build a hostname that's too long */
    char long_host[KL_CLIENT_HOSTNAME_MAX + 10];
    memset(long_host, 'a', sizeof(long_host) - 1);
    long_host[sizeof(long_host) - 1] = '\0';

    KlClientPoolConn conn = { .fd = fds[0], .tls = NULL, .reused = 0, ._entry = NULL };
    ASSERT_EQ(kl_cpool_release(&pool, &conn, long_host, 80, 0), -1);

    /* fd should be closed by discard inside release */
    close(fds[1]);
    kl_cpool_free(&pool);
}

UTEST(cpool, acquire_null_args) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    KlClientPoolConn conn;
    ASSERT_EQ(kl_cpool_acquire(NULL, "x", 80, 0, &conn), -1);
    ASSERT_EQ(kl_cpool_acquire(&pool, NULL, 80, 0, &conn), -1);
    ASSERT_EQ(kl_cpool_acquire(&pool, "x", 80, 0, NULL), -1);

    kl_cpool_free(&pool);
}

UTEST(cpool, discard_null) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    kl_cpool_discard(&pool, NULL);  /* should not crash */
    ASSERT_TRUE(1);

    kl_cpool_free(&pool);
}

UTEST(cpool, host_count_null) {
    ASSERT_EQ(kl_cpool_host_count(NULL, "x", 80, 0), 0);
    ASSERT_EQ(kl_cpool_idle_count(NULL), 0);
}

/* ── Integration tests: pooled sync requests ─────────────────────── */

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void *server_thread_fn(void *arg) {
    kl_server_run((KlServer *)arg);
    return NULL;
}

static void wait_for_bind(KlServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

UTEST(cpool, sync_pooled_reuse) {
    /* Start a real server */
    KlServer srv;
    KlConfig cfg = { .port = 0, .max_connections = 4 };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", srv.bound_port);

    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);

    /* First request — pool miss, establishes connection */
    KlClientResponse resp1;
    ASSERT_EQ(kl_client_request_pooled(&pool, &a, NULL, "GET", url,
                                         NULL, 0, NULL, 0, &resp1), 0);
    ASSERT_EQ(resp1.status, 200);
    kl_client_response_free(&resp1);

    /* Connection should now be in pool */
    ASSERT_EQ(kl_cpool_idle_count(&pool), 1);

    /* Second request — pool hit, reuses connection */
    KlClientResponse resp2;
    ASSERT_EQ(kl_client_request_pooled(&pool, &a, NULL, "GET", url,
                                         NULL, 0, NULL, 0, &resp2), 0);
    ASSERT_EQ(resp2.status, 200);
    kl_client_response_free(&resp2);

    ASSERT_EQ(kl_cpool_idle_count(&pool), 1);

    kl_cpool_free(&pool);
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

typedef struct { int done; int status; } AsyncPoolCtx;

static void async_pool_done(KlClient *cl, void *ud) {
    AsyncPoolCtx *ax = ud;
    const KlClientResponse *r = kl_client_response(cl);
    ax->status = r ? r->status : -1;
    ax->done = 1;
}

UTEST(cpool, async_pooled_reuse) {
    /* Start a real server */
    KlServer srv;
    KlConfig cfg = { .port = 0, .max_connections = 4 };
    ASSERT_EQ(0, kl_server_init(&srv, &cfg));
    kl_server_route(&srv, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_fn, &srv);
    wait_for_bind(&srv);
    ASSERT_TRUE(srv.bound_port > 0);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", srv.bound_port);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, &ev), 0);

    /* Async request 1 */
    AsyncPoolCtx ctx1 = { 0, 0 };

    KlClient *c1 = kl_client_start_pooled(&pool, &ev, &a, NULL, "GET", url,
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
        kl_client_free(c1);
    }

    /* Connection should be in pool */
    ASSERT_EQ(kl_cpool_idle_count(&pool), 1);

    /* Async request 2 — should reuse */
    AsyncPoolCtx ctx2 = { 0, 0 };
    KlClient *c2 = kl_client_start_pooled(&pool, &ev, &a, NULL, "GET", url,
                                             NULL, 0, NULL, 0,
                                             async_pool_done, &ctx2);
    if (c2) {
        for (int i = 0; i < 1000 && !ctx2.done; i++) {
            kl_event_ctx_run(&ev, 16, 10);
            kl_timer_fire(&ev);
        }
        ASSERT_TRUE(ctx2.done);
        ASSERT_EQ(ctx2.status, 200);
        kl_client_free(c2);
    }

    kl_cpool_free(&pool);
    kl_event_ctx_free(&ev);
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

/* ── Pooled sync: input validation ───────────────────────────────── */

UTEST(cpool, sync_pooled_null_args) {
    KlAllocator a = kl_allocator_default();
    KlClientPool pool;
    ASSERT_EQ(kl_cpool_init(&pool, NULL, &a, NULL), 0);
    KlClientResponse resp;

    ASSERT_EQ(kl_client_request_pooled(NULL, &a, NULL, "GET", "http://x",
                                         NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_client_request_pooled(&pool, NULL, NULL, "GET", "http://x",
                                         NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_client_request_pooled(&pool, &a, NULL, NULL, "http://x",
                                         NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_client_request_pooled(&pool, &a, NULL, "GET", NULL,
                                         NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_client_request_pooled(&pool, &a, NULL, "GET", "http://x",
                                         NULL, 0, NULL, 0, NULL), -1);

    kl_cpool_free(&pool);
}

UTEST(cpool, async_pooled_null_args) {
    ASSERT_TRUE(kl_client_start_pooled(NULL, NULL, NULL, NULL,
                                         "GET", "http://x",
                                         NULL, 0, NULL, 0,
                                         NULL, NULL) == NULL);
}

UTEST_MAIN();
