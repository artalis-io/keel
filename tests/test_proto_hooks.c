/*
 * test_proto_hooks.c — the install-once protocol-hook registry invariant (S-7-review
 * Finding 2). The tables are process-wide compiled-in capability registrations; the
 * setter must accept the first install, an idempotent re-install of the SAME table, and a
 * NULL reset, but reject a DIFFERENT live table (keeping the first). Exercised via the
 * completion-mode ws-drive registry, which — unlike the server ws/h2 tables — is NOT
 * auto-installed by a load-time constructor, so it starts pristine (NULL) in this unit.
 */
#include "utest.h"
#include "../src/proto_hooks.h"   /* internal seam; its includes are all public <keel/...> */

static void drive_a(struct KlServer *s, KlHttpConn *c) { (void)s; (void)c; }
static void drive_b(struct KlServer *s, KlHttpConn *c) { (void)s; (void)c; }

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

    /* A DIFFERENT live table is rejected — the first table is kept (install-once). */
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

UTEST_MAIN();
