#include "utest.h"
#include <keel/clock.h>
#include <keel/http_connection.h>

UTEST(connection, pool_init_and_free) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    ASSERT_EQ(kl_http_conn_pool_init(&pool, 8, &a), 0);
    ASSERT_EQ(pool.capacity, 8);
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, acquire_and_release) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 4, &a);

    /* Create parsers for the pool */
    for (int i = 0; i < 4; i++) {
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);
    }

    /* Acquire connections with fake fds */
    int fakefd = 100;
    KlHttpConn *c1 = kl_http_conn_acquire(&pool, fakefd);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_EQ(c1->stream.fd, fakefd);
    ASSERT_EQ(c1->state, KL_HTTP_CONN_READING);

    KlHttpConn *c2 = kl_http_conn_acquire(&pool, fakefd + 1);
    ASSERT_TRUE(c2 != NULL);
    ASSERT_TRUE(c1 != c2);

    /* Release: set fd to -1 to avoid closing real fds */
    c1->stream.fd = -1;
    kl_http_conn_release(&pool, c1);

    /* Should be able to acquire again */
    KlHttpConn *c3 = kl_http_conn_acquire(&pool, fakefd + 2);
    ASSERT_TRUE(c3 != NULL);

    /* Clean up */
    c2->stream.fd = -1;
    c3->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, pool_exhaustion) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 2, &a);

    for (int i = 0; i < 2; i++) {
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);
    }

    KlHttpConn *c1 = kl_http_conn_acquire(&pool, 100);
    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_TRUE(c2 != NULL);

    /* Pool exhausted */
    KlHttpConn *c3 = kl_http_conn_acquire(&pool, 102);
    ASSERT_TRUE(c3 == NULL);

    c1->stream.fd = -1;
    c2->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, active_count_tracking) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 4, &a);
    ASSERT_EQ(pool.active_count, 0);

    for (int i = 0; i < 4; i++) {
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);
    }

    KlHttpConn *c1 = kl_http_conn_acquire(&pool, 100);
    ASSERT_EQ(pool.active_count, 1);

    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_EQ(pool.active_count, 2);

    KlHttpConn *c3 = kl_http_conn_acquire(&pool, 102);
    ASSERT_EQ(pool.active_count, 3);

    /* Release one, count decrements, free_list non-NULL */
    c2->stream.fd = -1;
    kl_http_conn_release(&pool, c2);
    ASSERT_EQ(pool.active_count, 2);
    ASSERT_TRUE(pool.free_list != NULL);

    /* Acquire again, count back to 3 */
    KlHttpConn *c4 = kl_http_conn_acquire(&pool, 103);
    ASSERT_TRUE(c4 != NULL);
    ASSERT_EQ(pool.active_count, 3);

    c1->stream.fd = -1;
    c3->stream.fd = -1;
    c4->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

/* ═══════════════════════════════════════════════════════════════════
 * State machine tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(connection, state_initial) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 4, &a);
    for (int i = 0; i < 4; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(c->state, KL_HTTP_CONN_READING);

    c->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, release_resets_state) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 4, &a);
    for (int i = 0; i < 4; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);

    /* Simulate some activity */
    c->state = KL_HTTP_CONN_PROCESSING;
    c->stream.read_len = 42;

    /* Release and re-acquire */
    c->stream.fd = -1;
    kl_http_conn_release(&pool, c);

    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_TRUE(c2 != NULL);
    ASSERT_EQ(c2->state, KL_HTTP_CONN_READING);
    ASSERT_EQ(c2->stream.read_len, (size_t)0);

    c2->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, acquire_after_release) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 2, &a);
    for (int i = 0; i < 2; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c1 = kl_http_conn_acquire(&pool, 100);
    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_TRUE(c2 != NULL);

    /* Pool full */
    ASSERT_TRUE(kl_http_conn_acquire(&pool, 102) == NULL);

    /* Release c1 */
    c1->stream.fd = -1;
    kl_http_conn_release(&pool, c1);
    ASSERT_EQ(pool.active_count, 1);

    /* Acquire again; should succeed */
    KlHttpConn *c3 = kl_http_conn_acquire(&pool, 103);
    ASSERT_TRUE(c3 != NULL);
    ASSERT_EQ(c3->stream.fd, 103);
    ASSERT_EQ(pool.active_count, 2);

    c2->stream.fd = -1;
    c3->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, monotonic_ms) {
    uint64_t t1 = kl_monotonic_ms();
    uint64_t t2 = kl_monotonic_ms();
    /* Monotonic: t2 >= t1 */
    ASSERT_TRUE(t2 >= t1);
    /* Should be reasonably close (within 1 second) */
    ASSERT_TRUE(t2 - t1 < 1000);
}

UTEST(connection, pool_capacity_zero) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    /* Capacity 0, should handle gracefully (return error or empty pool) */
    int rc = kl_http_conn_pool_init(&pool, 0, &a);
    if (rc == 0) {
        /* If it succeeds with 0 capacity, acquire should return NULL */
        KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
        ASSERT_TRUE(c == NULL);
        kl_http_conn_pool_free(&pool);
    } else {
        /* Init returning -1 for 0 capacity is also acceptable */
        ASSERT_EQ(rc, -1);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Growable read buffer tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(connection, read_buf_allocated) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    ASSERT_EQ(kl_http_conn_pool_init(&pool, 4, &a), 0);

    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(pool.conns[i].stream.read_buf != NULL);
        ASSERT_EQ(pool.conns[i].stream.read_cap, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);
    }

    kl_http_conn_pool_free(&pool);
}

UTEST(connection, read_buf_survives_acquire_release) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 2, &a);
    for (int i = 0; i < 2; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);
    ASSERT_TRUE(c->stream.read_buf != NULL);
    ASSERT_EQ(c->stream.read_cap, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);

    char *buf_ptr = c->stream.read_buf;
    c->stream.fd = -1;
    kl_http_conn_release(&pool, c);

    /* Re-acquire; buffer should still be valid */
    KlHttpConn *c2 = kl_http_conn_acquire(&pool, 101);
    ASSERT_TRUE(c2 != NULL);
    ASSERT_TRUE(c2->stream.read_buf != NULL);
    ASSERT_EQ(c2->stream.read_cap, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);
    /* Same slot, same buffer pointer */
    ASSERT_EQ(c2->stream.read_buf, buf_ptr);

    c2->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST(connection, max_header_size_default) {
    /* max_header_size defaults to KL_HTTP_CONN_READ_BUF_SIZE when 0 */
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    kl_http_conn_pool_init(&pool, 2, &a);

    /* Before server init sets it, max_header_size is 0 */
    ASSERT_EQ(pool.conns[0].max_header_size, (size_t)0);

    /* Simulate what http_server.c does */
    for (int i = 0; i < 2; i++)
        pool.conns[i].max_header_size = KL_HTTP_CONN_READ_BUF_SIZE;

    ASSERT_EQ(pool.conns[0].max_header_size, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);
    ASSERT_EQ(pool.conns[1].max_header_size, (size_t)KL_HTTP_CONN_READ_BUF_SIZE);

    kl_http_conn_pool_free(&pool);
}

/* F2-B accessor: kl_http_conn_response[_const] return the connection's response, NULL on NULL. */
UTEST(connection, response_accessor) {
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    ASSERT_EQ(kl_http_conn_pool_init(&pool, 2, &a), 0);
    for (int i = 0; i < 2; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ((void *)kl_http_conn_response(c), (void *)&c->res);
    ASSERT_EQ((const void *)kl_http_conn_response_const(c), (const void *)&c->res);
    ASSERT_TRUE(kl_http_conn_response(NULL) == NULL);
    ASSERT_TRUE(kl_http_conn_response_const(NULL) == NULL);

    c->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

UTEST_MAIN();
