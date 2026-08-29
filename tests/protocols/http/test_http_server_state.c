/*
 * test_http_server_state.c: the server run/drain flags (KlHttpServer.running / .draining) are stored as
 * plain int and accessed exclusively through the lock-free int atomics in src/kl_atomic.h, so the
 * public header is valid C and C++ with one shared object representation. This locks in:
 *   - the int atomic op is lock-free (the SIGTERM/SIGINT stop path runs from a signal handler and must
 *     not fall into a library call); and
 *   - the kl_http_server_stop state machine: a no-drain stop clears running immediately; a drain stop
 *     sets draining and keeps running up until a second stop ends it.
 * White-box on purpose: it drives the flags directly through the same atomic accessors the server uses,
 * with no socket or event loop (stop_wake_wr is left invalid so stop only flips the flags).
 */
#include "utest.h"
#include <keel/keel.h>
#include "../../../src/kl_atomic.h"
#include <string.h>

/* int atomics must be always-lock-free: the stop path can run from a signal handler. */
UTEST(server_state, int_atomic_is_lock_free) {
    ASSERT_EQ(ATOMIC_INT_LOCK_FREE, 2);
    int v = 0;
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

UTEST_MAIN();
