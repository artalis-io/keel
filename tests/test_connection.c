#include "utest.h"
#include <keel/connection.h>

UTEST(connection, pool_init_and_free) {
    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    ASSERT_EQ(kl_conn_pool_init(&pool, 8, &a), 0);
    ASSERT_EQ(pool.capacity, 8);
    kl_conn_pool_free(&pool);
}

UTEST(connection, acquire_and_release) {
    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    kl_conn_pool_init(&pool, 4, &a);

    /* Create parsers for the pool */
    for (int i = 0; i < 4; i++) {
        pool.conns[i].parser = kl_parser_llhttp(&a);
    }

    /* Acquire connections with fake fds */
    int fakefd = 100;
    KlConn *c1 = kl_conn_acquire(&pool, fakefd);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_EQ(c1->fd, fakefd);
    ASSERT_EQ(c1->state, KL_CONN_READING);

    KlConn *c2 = kl_conn_acquire(&pool, fakefd + 1);
    ASSERT_TRUE(c2 != NULL);
    ASSERT_TRUE(c1 != c2);

    /* Release — set fd to -1 to avoid closing real fds */
    c1->fd = -1;
    kl_conn_release(&pool, c1);

    /* Should be able to acquire again */
    KlConn *c3 = kl_conn_acquire(&pool, fakefd + 2);
    ASSERT_TRUE(c3 != NULL);

    /* Clean up */
    c2->fd = -1;
    c3->fd = -1;
    kl_conn_pool_free(&pool);
}

UTEST(connection, pool_exhaustion) {
    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    kl_conn_pool_init(&pool, 2, &a);

    for (int i = 0; i < 2; i++) {
        pool.conns[i].parser = kl_parser_llhttp(&a);
    }

    KlConn *c1 = kl_conn_acquire(&pool, 100);
    KlConn *c2 = kl_conn_acquire(&pool, 101);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_TRUE(c2 != NULL);

    /* Pool exhausted */
    KlConn *c3 = kl_conn_acquire(&pool, 102);
    ASSERT_TRUE(c3 == NULL);

    c1->fd = -1;
    c2->fd = -1;
    kl_conn_pool_free(&pool);
}

UTEST(connection, active_count_tracking) {
    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    kl_conn_pool_init(&pool, 4, &a);
    ASSERT_EQ(pool.active_count, 0);

    for (int i = 0; i < 4; i++) {
        pool.conns[i].parser = kl_parser_llhttp(&a);
    }

    KlConn *c1 = kl_conn_acquire(&pool, 100);
    ASSERT_EQ(pool.active_count, 1);

    KlConn *c2 = kl_conn_acquire(&pool, 101);
    ASSERT_EQ(pool.active_count, 2);

    KlConn *c3 = kl_conn_acquire(&pool, 102);
    ASSERT_EQ(pool.active_count, 3);

    /* Release one — count decrements, free_list non-NULL */
    c2->fd = -1;
    kl_conn_release(&pool, c2);
    ASSERT_EQ(pool.active_count, 2);
    ASSERT_TRUE(pool.free_list != NULL);

    /* Acquire again — count back to 3 */
    KlConn *c4 = kl_conn_acquire(&pool, 103);
    ASSERT_TRUE(c4 != NULL);
    ASSERT_EQ(pool.active_count, 3);

    c1->fd = -1;
    c3->fd = -1;
    c4->fd = -1;
    kl_conn_pool_free(&pool);
}

/* ═══════════════════════════════════════════════════════════════════
 * State machine tests
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(connection, state_initial) {
    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    kl_conn_pool_init(&pool, 4, &a);
    for (int i = 0; i < 4; i++)
        pool.conns[i].parser = kl_parser_llhttp(&a);

    KlConn *c = kl_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(c->state, KL_CONN_READING);

    c->fd = -1;
    kl_conn_pool_free(&pool);
}

UTEST(connection, release_resets_state) {
    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    kl_conn_pool_init(&pool, 4, &a);
    for (int i = 0; i < 4; i++)
        pool.conns[i].parser = kl_parser_llhttp(&a);

    KlConn *c = kl_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);

    /* Simulate some activity */
    c->state = KL_CONN_PROCESSING;
    c->read_len = 42;

    /* Release and re-acquire */
    c->fd = -1;
    kl_conn_release(&pool, c);

    KlConn *c2 = kl_conn_acquire(&pool, 101);
    ASSERT_TRUE(c2 != NULL);
    ASSERT_EQ(c2->state, KL_CONN_READING);
    ASSERT_EQ(c2->read_len, (size_t)0);

    c2->fd = -1;
    kl_conn_pool_free(&pool);
}

UTEST(connection, acquire_after_release) {
    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    kl_conn_pool_init(&pool, 2, &a);
    for (int i = 0; i < 2; i++)
        pool.conns[i].parser = kl_parser_llhttp(&a);

    KlConn *c1 = kl_conn_acquire(&pool, 100);
    KlConn *c2 = kl_conn_acquire(&pool, 101);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_TRUE(c2 != NULL);

    /* Pool full */
    ASSERT_TRUE(kl_conn_acquire(&pool, 102) == NULL);

    /* Release c1 */
    c1->fd = -1;
    kl_conn_release(&pool, c1);
    ASSERT_EQ(pool.active_count, 1);

    /* Acquire again — should succeed */
    KlConn *c3 = kl_conn_acquire(&pool, 103);
    ASSERT_TRUE(c3 != NULL);
    ASSERT_EQ(c3->fd, 103);
    ASSERT_EQ(pool.active_count, 2);

    c2->fd = -1;
    c3->fd = -1;
    kl_conn_pool_free(&pool);
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
    KlConnPool pool;
    /* Capacity 0 — should handle gracefully (return error or empty pool) */
    int rc = kl_conn_pool_init(&pool, 0, &a);
    if (rc == 0) {
        /* If it succeeds with 0 capacity, acquire should return NULL */
        KlConn *c = kl_conn_acquire(&pool, 100);
        ASSERT_TRUE(c == NULL);
        kl_conn_pool_free(&pool);
    } else {
        /* Init returning -1 for 0 capacity is also acceptable */
        ASSERT_EQ(rc, -1);
    }
}

UTEST_MAIN();
