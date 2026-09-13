/*
 * test_atomic_lock_free.c: the lock-free contract in src/kl_atomic.h.
 *
 * Keel requires something semantic -- the flag the server-stop path touches must be operated on
 * without a lock, because that path runs from a signal or console-control handler. ATOMIC_INT_LOCK_FREE
 * answers that with 0 (never), 2 (always) or 1 (depends on the object, ask at runtime). The build used
 * to assert == 2, which rejected MSVC: it reports 1 while atomic_is_lock_free() returns true for the
 * very object Keel uses. These tests pin the replacement policy so it cannot quietly regress into
 * either a blanket assertion or a compiler-specific exemption.
 */
#include "utest.h"
#include "kl_atomic.h"
#include <keel/http_server.h>
#include <keel/error.h>
#include "platform_thread.h"
#include <string.h>

/* The policy agrees with the macro. The macro is the premise, not the requirement: this asserts the
 * mapping from premise to answer, which is the part the code controls. */
UTEST(atomic_lock_free, policy_matches_the_macro) {
    int flag = 0;
    /* ATOMIC_INT_LOCK_FREE == 0 cannot be reached: kl_atomic.h #errors the build, so a host that
     * compiled this suite has already ruled it out. */
    ASSERT_NE(0, ATOMIC_INT_LOCK_FREE);
#if ATOMIC_INT_LOCK_FREE == 2
    ASSERT_EQ(1, KL_ATOMIC_INT_LOCK_FREE_STATIC);
    ASSERT_EQ(1, kl_atomic_int_is_lock_free(&flag));   /* guaranteed, no probe */
#else
    ASSERT_EQ(0, KL_ATOMIC_INT_LOCK_FREE_STATIC);
    /* The runtime query is the whole point of the == 1 case. Every platform Keel supports answers
     * yes; a platform that answered no would be refused by kl_http_server_init below. */
    ASSERT_EQ(1, kl_atomic_int_is_lock_free(&flag));
#endif
}

UTEST(atomic_lock_free, null_is_not_lock_free) {
    /* Defensive, and it matters: the server treats 0 as "refuse to initialise", so a NULL slipping
     * through must not read as a pass. */
    ASSERT_EQ(0, kl_atomic_int_is_lock_free(NULL));
}

UTEST(atomic_lock_free, load_and_store_round_trip) {
    int v = 0;
    kl_atomic_store_int(&v, 1);
    ASSERT_EQ(1, kl_atomic_load_int(&v));
    ASSERT_EQ(1, v);                      /* plain reads see it: one object representation */
    kl_atomic_store_int(&v, 0);
    ASSERT_EQ(0, kl_atomic_load_int(&v));
    kl_atomic_store_int(&v, -7);
    ASSERT_EQ(-7, kl_atomic_load_int(&v));   /* signed values survive the interlocked lowering */
}

/* A load must not disturb the value. Worth asserting because the MSVC lowering implements the load as
 * a compare-exchange against itself, which really does write -- the same bits back. If that ever
 * became a compare-exchange with the wrong comparand, every flag would read as zero and the server
 * would exit its loop immediately. */
UTEST(atomic_lock_free, load_does_not_modify) {
    int v = 12345;
    for (int i = 0; i < 100; i++)
        ASSERT_EQ(12345, kl_atomic_load_int(&v));
    ASSERT_EQ(12345, v);
}

/* Cross-thread visibility, through the PAL threading seam rather than pthreads directly, so this runs
 * on MSVC too. Deterministic: the writer is joined before the value is read, so there is no race for
 * the verdict and no sleep anywhere. */
static int xt_flag;
static void xt_writer(void *arg) {
    (void)arg;
    kl_atomic_store_int(&xt_flag, 99);
}
UTEST(atomic_lock_free, store_is_visible_across_threads) {
    KlPlatThread th;
    xt_flag = 0;
    ASSERT_EQ(0, kl_plat_thread_create(&th, xt_writer, NULL));
    kl_plat_thread_join(&th);
    ASSERT_EQ(99, kl_atomic_load_int(&xt_flag));
}

/* The policy is wired into the server, not just available. A host whose stop flag is not lock-free is
 * refused with KL_ERR_UNSUPPORTED instead of silently running with a handler that could take a lock;
 * on every supported host it initialises, which is what this asserts. */
UTEST(atomic_lock_free, server_init_accepts_a_lock_free_platform) {
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;                  /* ephemeral: init does not bind */
    cfg.bind_addr = "127.0.0.1";
    ASSERT_EQ(0, kl_http_server_init(&s, &cfg));
    /* The very objects the stop path stores to. */
    ASSERT_EQ(1, kl_atomic_int_is_lock_free(&s.running));
    ASSERT_EQ(1, kl_atomic_int_is_lock_free(&s.draining));
    kl_http_server_free(&s);
}

UTEST_MAIN();
