#include "utest.h"
#include <keel/keel.h>

#include <string.h>

UTEST(server_stats, stats_initial) {
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 4;

    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);

    KlHttpServerStats stats;
    kl_http_server_stats(&s, &stats);

    ASSERT_EQ(stats.active_connections, 0);
    ASSERT_EQ(stats.max_connections, 4);
    ASSERT_EQ(stats.async_suspended, 0);
    ASSERT_EQ(stats.listen_paused, 0);

    kl_http_server_free(&s);
}

UTEST(server_stats, stats_active_count) {
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 4;

    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);

    /* Simulate active connections by acquiring from pool */
    KlHttpConn *c1 = kl_http_conn_acquire(&s.pool, 100);
    ASSERT_TRUE(c1 != NULL);
    KlHttpConn *c2 = kl_http_conn_acquire(&s.pool, 101);
    ASSERT_TRUE(c2 != NULL);

    KlHttpServerStats stats;
    kl_http_server_stats(&s, &stats);
    ASSERT_EQ(stats.active_connections, 2);

    /* Release one */
    kl_http_conn_release(&s.pool, c1);
    kl_http_server_stats(&s, &stats);
    ASSERT_EQ(stats.active_connections, 1);

    kl_http_conn_release(&s.pool, c2);
    kl_http_server_stats(&s, &stats);
    ASSERT_EQ(stats.active_connections, 0);

    kl_http_server_free(&s);
}

UTEST(server_stats, stats_max_connections) {
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 16;

    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);

    KlHttpServerStats stats;
    kl_http_server_stats(&s, &stats);
    ASSERT_EQ(stats.max_connections, 16);

    kl_http_server_free(&s);
}

UTEST(server_stats, stats_null_safety) {
    KlHttpServerStats stats;
    memset(&stats, 0xFF, sizeof(stats));

    /* NULL server — should zero out */
    kl_http_server_stats(NULL, &stats);
    ASSERT_EQ(stats.active_connections, 0);
    ASSERT_EQ(stats.max_connections, 0);
    ASSERT_EQ(stats.async_suspended, 0);
    ASSERT_EQ(stats.listen_paused, 0);

    /* NULL out — should not crash */
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 4;
    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);

    kl_http_server_stats(&s, NULL);  /* no-op, no crash */

    kl_http_server_free(&s);
}

UTEST_MAIN();
