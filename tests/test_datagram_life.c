/*
 * test_datagram_life.c — the transport-neutral stable-liveness token (src/datagram_life.h).
 *
 * The token underlies safe teardown of datagram completion ops: each posted op holds a ref; the
 * owner (KlUdp) holds one; on_final (freeing the receive storage) runs EXACTLY ONCE, on the final
 * release, whether that is the owner's or the last op's. mark_dead clears the target so a late
 * completion recovers NULL and never touches the freed owner. These ownership invariants are the
 * ones the backend conversions rely on; verified here directly (backend-independent).
 */
#include "utest.h"

#include "../src/datagram_life.h"

#include <keel/allocator.h>

static int g_final;
static void on_final(void *ctx) { (void)ctx; g_final++; }

/* owner-only lifetime: create (owner ref) → release → on_final once */
UTEST(dgram_life, owner_only) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlDgramLife *l = kl_dgram_life_create(&a, (void *)0x1234, on_final, NULL);
    ASSERT_TRUE(l != NULL);
    ASSERT_EQ(kl_dgram_life_target(l), (void *)0x1234);   /* live target */
    kl_dgram_life_release(l);                              /* final release */
    ASSERT_EQ(g_final, 1);
}

/* mark_dead clears the target but does NOT free (owner ref still held); release then frees once */
UTEST(dgram_life, mark_dead_then_release) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlDgramLife *l = kl_dgram_life_create(&a, (void *)0x1, on_final, NULL);
    kl_dgram_life_mark_dead(l);
    ASSERT_EQ(kl_dgram_life_target(l), NULL);              /* dead → NULL target */
    ASSERT_EQ(g_final, 0);                                 /* mark_dead does not free */
    kl_dgram_life_release(l);
    ASSERT_EQ(g_final, 1);
}

/* op outlives the owner: owner marks dead + releases, the op ref keeps the storage alive, the op's
 * release is the FINAL one → on_final fires exactly once, AFTER the op is done (free-while-in-flight) */
UTEST(dgram_life, op_outlives_owner) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlDgramLife *l = kl_dgram_life_create(&a, (void *)0x1, on_final, NULL);
    kl_dgram_life_retain(l);                               /* a posted op takes a ref (refs=2) */

    /* owner (kl_udp_free): mark dead + drop owner ref — storage NOT freed (op still holds a ref) */
    kl_dgram_life_mark_dead(l);
    kl_dgram_life_release(l);
    ASSERT_EQ(g_final, 0);
    ASSERT_EQ(kl_dgram_life_target(l), NULL);              /* dead: a late completion sees no owner */

    /* op reaped later: releases the last ref → final release frees the storage exactly once */
    kl_dgram_life_release(l);
    ASSERT_EQ(g_final, 1);
}

/* several outstanding ops: on_final fires only when the LAST reference (owner + every op) is gone */
UTEST(dgram_life, many_ops_final_once) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlDgramLife *l = kl_dgram_life_create(&a, (void *)0x1, on_final, NULL);
    for (int i = 0; i < 5; i++) kl_dgram_life_retain(l);   /* 5 posted ops (refs=6) */
    kl_dgram_life_release(l);                              /* owner drops */
    for (int i = 0; i < 4; i++) { kl_dgram_life_release(l); ASSERT_EQ(g_final, 0); }
    kl_dgram_life_release(l);                              /* the last op */
    ASSERT_EQ(g_final, 1);                                 /* exactly once */
}

/* mark_dead is idempotent; a NULL token is a no-op everywhere */
UTEST(dgram_life, idempotent_and_null_safe) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlDgramLife *l = kl_dgram_life_create(&a, (void *)0x9, on_final, NULL);
    kl_dgram_life_mark_dead(l);
    kl_dgram_life_mark_dead(l);                            /* idempotent */
    ASSERT_EQ(kl_dgram_life_target(l), NULL);
    kl_dgram_life_release(l);
    ASSERT_EQ(g_final, 1);
    /* NULL safety */
    kl_dgram_life_retain(NULL);
    kl_dgram_life_release(NULL);
    kl_dgram_life_mark_dead(NULL);
    ASSERT_EQ(kl_dgram_life_target(NULL), NULL);
}

UTEST_MAIN();
