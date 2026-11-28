/*
 * atomic_flags.h: plain-int flags and counters shared between a server's loop thread (handlers,
 * callbacks, timers) and the test thread that polls them.
 *
 * Both sides go through these (seq-cst, src/kl_atomic.h), so a test thread that waits for a
 * callback's flag, or reads its counter, is not a data race. Declare the variables as plain int.
 */
#ifndef KEEL_TESTS_ATOMIC_FLAGS_H
#define KEEL_TESTS_ATOMIC_FLAGS_H

#include "kl_atomic.h"

#define T_LOAD(x)     kl_atomic_load_int(&(x))
#define T_STORE(x, v) kl_atomic_store_int(&(x), (v))
#define T_INC(x)      kl_test_atomic_add(&(x), 1)

static inline int kl_test_atomic_add(int *p, int d) {
    int o;
    do { o = kl_atomic_load_int(p); } while (!kl_atomic_cas_int(p, o, o + d));
    return o + d;
}

#endif /* KEEL_TESTS_ATOMIC_FLAGS_H */
