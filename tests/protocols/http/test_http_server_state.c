/*
 * test_http_server_state.c: the server run/drain flags (KlHttpServer.running / .draining) are stored as
 * plain int and accessed exclusively through the lock-free int atomics in src/kl_atomic.h, so the
 * public header is valid C and C++ with one shared object representation. This locks in:
 *   - the int atomic op is lock-free (the SIGTERM/SIGINT stop path runs from a signal handler and must
 *     not fall into a library call); and
 *   - the kl_http_server_stop state machine: a no-drain stop clears running immediately; a drain stop
 *     sets draining and keeps running up until a second stop ends it; and a stop that overlaps the
 *     first one's entry into the drain neither rewrites its deadline nor ends it.
 * White-box on purpose: it drives the flags directly through the same atomic accessors the server uses,
 * with no socket or event loop (stop_wake_wr is left invalid so stop only flips the flags).
 */
#include "utest.h"
#include <keel/keel.h>
#include "../../../src/kl_atomic.h"
#include "../../../src/platform_thread.h"
#include "../../../src/protocols/http/http_internal.h"
#include <string.h>

/* The int atomic the stop path uses must be lock-free: kl_http_server_stop can run from a signal
 * handler and must not fall into a library call. ATOMIC_INT_LOCK_FREE alone cannot answer that:
 * it reports 2 (every int object) or 1 (object-dependent, ask at runtime), and MSVC reports 1
 * while the object itself is lock-free. So assert the property of the ACTUAL object, through the
 * same accessor src/kl_atomic.h uses, rather than the platform-wide constant. That is the stronger
 * claim, and it is the one the stop path actually depends on. */
UTEST(server_state, int_atomic_is_lock_free) {
    ASSERT_NE(ATOMIC_INT_LOCK_FREE, 0);  /* 0 would mean never lock-free: unusable here. */
    int v = 0;
    ASSERT_TRUE(kl_atomic_int_is_lock_free(&v));
    kl_atomic_store_int(&v, 1);
    ASSERT_EQ(kl_atomic_load_int(&v), 1);
    kl_atomic_store_int(&v, 0);
    ASSERT_EQ(kl_atomic_load_int(&v), 0);
}

/* No drain timeout: the first stop clears running at once (draining untouched). */
UTEST(server_state, stop_without_drain_clears_running) {
    KlHttpServer s;
    memset(&s, 0, sizeof(s));
    s.stop_wake_wr = KL_INVALID_SOCKET;   /* no wakeup pipe: stop only flips the flags */
    s.config.drain_timeout_ms = 0;
    kl_atomic_store_int(&s.running, 1);
    kl_atomic_store_int(&s.draining, 0);

    kl_http_server_stop(&s);
    ASSERT_EQ(kl_atomic_load_int(&s.running), 0);
    ASSERT_EQ(kl_atomic_load_int(&s.draining), 0);
}

/* With a drain timeout: the first stop enters drain (running stays up), a second stop ends it. */
UTEST(server_state, stop_with_drain_then_stop) {
    KlHttpServer s;
    memset(&s, 0, sizeof(s));
    s.stop_wake_wr = KL_INVALID_SOCKET;
    s.config.drain_timeout_ms = 100;
    kl_atomic_store_int(&s.running, 1);
    kl_atomic_store_int(&s.draining, 0);

    kl_http_server_stop(&s);                        /* enter drain */
    ASSERT_EQ(kl_atomic_load_int(&s.draining), 1);
    ASSERT_EQ(kl_atomic_load_int(&s.running), 1);

    kl_http_server_stop(&s);                        /* already draining -> clear running */
    ASSERT_EQ(kl_atomic_load_int(&s.running), 0);
    ASSERT_EQ(kl_atomic_load_int(&s.draining), 1);
}

/* kl_http_server_draining reports a drain only while the loop runs: once the drain has stopped it
 * (running cleared, draining left set), and after the run, a stop that sets draining again reads 0. */
UTEST(server_state, draining_reads_zero_once_the_loop_has_stopped) {
    KlHttpServer s;
    memset(&s, 0, sizeof(s));
    s.stop_wake_wr = KL_INVALID_SOCKET;
    s.config.drain_timeout_ms = 100;
    kl_atomic_store_int(&s.running, 1);
    kl_atomic_store_int(&s.draining, 0);
    ASSERT_EQ(0, kl_http_server_draining(&s));

    kl_http_server_stop(&s);                        /* the drain begins */
    ASSERT_EQ(1, kl_http_server_draining(&s));

    kl_atomic_store_int(&s.running, 0);             /* the drain ends the loop, as drain_progress does */
    ASSERT_EQ(0, kl_http_server_draining(&s));

    kl_atomic_store_int(&s.draining, 0);            /* a stop after the run has returned */
    kl_http_server_stop(&s);
    ASSERT_EQ(0, kl_http_server_draining(&s));
}

static void stop_from_thread(void *arg) { kl_http_server_stop((KlHttpServer *)arg); }

/* The first caller is held between claiming the drain and publishing it (the two steps
 * kl_http_server_stop runs). A stop from another thread in that interval observes the claim in
 * progress: it must not write the deadline and must not end the server. Once the first caller has
 * published, the drain carries the first caller's deadline, and a further stop escalates as usual. */
UTEST(server_state, overlapping_stop_leaves_a_claimed_drain_alone) {
    KlHttpServer s;
    memset(&s, 0, sizeof(s));
    s.stop_wake_wr = KL_INVALID_SOCKET;
    s.config.drain_timeout_ms = 60000;
    kl_atomic_store_int(&s.running, 1);
    kl_atomic_store_int(&s.draining, 0);

    ASSERT_EQ(1, kl_http_server_stop_claim(&s));    /* the first caller claims, then pauses */
    ASSERT_EQ(0, kl_http_server_stop_claim(&s));    /* nobody else can claim it */
    const uint64_t sentinel = 0x5a5a5a5a5a5aull;    /* unpublished: no caller may have written this */
    s.drain_deadline_ms = sentinel;

    KlPlatThread th;
    ASSERT_EQ(0, kl_plat_thread_create(&th, stop_from_thread, &s));
    kl_plat_thread_join(&th);                       /* the second caller ran entirely in the interval */

    EXPECT_EQ(sentinel, s.drain_deadline_ms);       /* deadline untouched */
    EXPECT_EQ(1, kl_atomic_load_int(&s.running));   /* not forced to exit */
    EXPECT_EQ(0, kl_atomic_load_int(&s.draining));  /* the drain is still the first caller's to publish */
    EXPECT_EQ(KL_HTTP_STOP_CLAIMED, kl_atomic_load_int(&s.stop_requested));

    const uint64_t before = kl_monotonic_ms();
    kl_http_server_stop_publish(&s);                /* the first caller resumes */
    EXPECT_EQ(1, kl_atomic_load_int(&s.draining));
    EXPECT_EQ(1, kl_atomic_load_int(&s.running));
    EXPECT_EQ(KL_HTTP_STOP_ESTABLISHED, kl_atomic_load_int(&s.stop_requested));
    EXPECT_GE(s.drain_deadline_ms, before + 60000u);
    EXPECT_LE(s.drain_deadline_ms, kl_monotonic_ms() + 60000u);

    const uint64_t published = s.drain_deadline_ms;
    ASSERT_EQ(0, kl_plat_thread_create(&th, stop_from_thread, &s));
    kl_plat_thread_join(&th);                       /* a stop of the established drain */
    EXPECT_EQ(0, kl_atomic_load_int(&s.running));   /* escalates: stops at once */
    EXPECT_EQ(published, s.drain_deadline_ms);      /* and still never rewrites the deadline */
}

UTEST_MAIN();
