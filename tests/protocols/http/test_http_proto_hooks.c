/*
 * test_http_proto_hooks.c: the install-once protocol-hook registry invariant.
 * The tables are process-wide compiled-in capability registrations; the
 * setter must accept the first install, an idempotent re-install of the SAME table, and a
 * NULL reset, but reject a DIFFERENT live table (keeping the first). Exercised via the
 * completion-mode ws-drive registry, which, unlike the server ws/h2 tables, is NOT
 * auto-installed by a load-time constructor, so it starts pristine (NULL) in this unit.
 */
#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include "http_proto_hooks.h"   /* internal seam; its includes are all public <keel/...> */
#include "platform_thread.h"
#include <stdatomic.h>

static void drive_a(struct KlHttpServer *s, KlHttpConn *c) { (void)s; (void)c; }
static void drive_b(struct KlHttpServer *s, KlHttpConn *c) { (void)s; (void)c; }

UTEST(proto_hooks, install_once_registry) {
    static const KlWsCompHooks table_a = { drive_a };
    static const KlWsCompHooks table_b = { drive_b };

    /* Pristine: nothing installed this registry yet. */
    ASSERT_TRUE(kl_ws_comp_hooks() == NULL);

    /* First install takes. */
    kl_ws_comp_hooks_set(&table_a);
    ASSERT_TRUE(kl_ws_comp_hooks() == &table_a);

    /* Idempotent re-install of the SAME table is fine. */
    kl_ws_comp_hooks_set(&table_a);
    ASSERT_TRUE(kl_ws_comp_hooks() == &table_a);

    /* A DIFFERENT live table is rejected; the first table is kept (install-once). */
    kl_ws_comp_hooks_set(&table_b);
    ASSERT_TRUE(kl_ws_comp_hooks() == &table_a);

    /* NULL reset is allowed... */
    kl_ws_comp_hooks_set(NULL);
    ASSERT_TRUE(kl_ws_comp_hooks() == NULL);

    /* ...and a fresh install after reset takes (even the previously-rejected table). */
    kl_ws_comp_hooks_set(&table_b);
    ASSERT_TRUE(kl_ws_comp_hooks() == &table_b);

    kl_ws_comp_hooks_set(NULL);   /* leave the registry pristine for any later consumer */
}

static const KlWsServerHooks concurrent_ws = {0};
static const KlHttp2ServerHooks concurrent_h2 = {0};
static const KlWsCompHooks concurrent_wsc = { drive_a };
static const KlHttp2CompHooks concurrent_h2c = { drive_a };
static const KlProxyHooks concurrent_proxy = {0};
static atomic_int concurrent_ready, concurrent_bad;
static void concurrent_install(void *ctx) {
    (void)ctx;
    atomic_fetch_add(&concurrent_ready, 1);
    while (atomic_load(&concurrent_ready) < 2) {}
    for (int i = 0; i < 10000; i++) {
        kl_ws_server_hooks_set(&concurrent_ws);
        kl_http2_server_hooks_set(&concurrent_h2);
        kl_ws_comp_hooks_set(&concurrent_wsc);
        kl_http2_comp_hooks_set(&concurrent_h2c);
        kl_proxy_hooks_set(&concurrent_proxy);
        if (kl_ws_server_hooks() != &concurrent_ws || kl_http2_server_hooks() != &concurrent_h2 ||
            kl_ws_comp_hooks() != &concurrent_wsc || kl_http2_comp_hooks() != &concurrent_h2c ||
            kl_proxy_hooks() != &concurrent_proxy)
            atomic_store(&concurrent_bad, 1);
    }
}

UTEST(proto_hooks, independent_threads_install_and_read_the_same_tables) {
    kl_ws_server_hooks_set(NULL); kl_http2_server_hooks_set(NULL);
    kl_ws_comp_hooks_set(NULL); kl_http2_comp_hooks_set(NULL); kl_proxy_hooks_set(NULL);
    atomic_store(&concurrent_ready, 0); atomic_store(&concurrent_bad, 0);
    KlPlatThread a, b;
    ASSERT_EQ(0, kl_plat_thread_create(&a, concurrent_install, NULL));
    int rc = kl_plat_thread_create(&b, concurrent_install, NULL);
    if (rc != 0) { atomic_store(&concurrent_ready, 2); kl_plat_thread_join(&a); }
    ASSERT_EQ(0, rc);
    kl_plat_thread_join(&a); kl_plat_thread_join(&b);
    ASSERT_EQ(0, atomic_load(&concurrent_bad));
    kl_ws_server_hooks_set(NULL); kl_http2_server_hooks_set(NULL);
    kl_ws_comp_hooks_set(NULL); kl_http2_comp_hooks_set(NULL); kl_proxy_hooks_set(NULL);
}

UTEST_MAIN();
