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

UTEST_MAIN();
