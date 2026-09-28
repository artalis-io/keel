/*
 * test_completion_life.c: the transport-neutral stable-liveness token (src/completion_life.h).
 *
 * The token underlies safe teardown of completion ops (datagrams, named-pipe streams): each posted
 * op holds a ref; the owner (e.g. the datagram core) holds one; on_final (freeing the receive storage) runs EXACTLY ONCE, on the final
 * release, whether that is the owner's or the last op's. mark_dead clears the target so a late
 * completion recovers NULL and never touches the freed owner. These ownership invariants are the
 * ones the backend conversions rely on; verified here directly (backend-independent).
 */
#include "utest.h"

#include "../src/completion_life.h"

#include <keel/allocator.h>

static int g_final;
static void on_final(void *ctx) { (void)ctx; g_final++; }

/* owner-only lifetime: create (owner ref) → release → on_final once */
UTEST(comp_life, owner_only) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlCompLife *l = kl_comp_life_create(&a, (void *)0x1234, on_final, NULL, (KlCompLifeDispatchFn)0);
    ASSERT_TRUE(l != NULL);
    ASSERT_EQ(kl_comp_life_target(l), (void *)0x1234);   /* live target */
    kl_comp_life_release(l);                              /* final release */
    ASSERT_EQ(g_final, 1);
}

/* mark_dead clears the target but does NOT free (owner ref still held); release then frees once */
UTEST(comp_life, mark_dead_then_release) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlCompLife *l = kl_comp_life_create(&a, (void *)0x1, on_final, NULL, (KlCompLifeDispatchFn)0);
    kl_comp_life_mark_dead(l);
    ASSERT_EQ(kl_comp_life_target(l), NULL);              /* dead → NULL target */
    ASSERT_EQ(g_final, 0);                                 /* mark_dead does not free */
    kl_comp_life_release(l);
    ASSERT_EQ(g_final, 1);
}

/* op outlives the owner: owner marks dead + releases, the op ref keeps the storage alive, the op's
 * release is the FINAL one → on_final fires exactly once, AFTER the op is done (free-while-in-flight) */
UTEST(comp_life, op_outlives_owner) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlCompLife *l = kl_comp_life_create(&a, (void *)0x1, on_final, NULL, (KlCompLifeDispatchFn)0);
    kl_comp_life_retain(l);                               /* a posted op takes a ref (refs=2) */

    /* owner drop: mark dead + drop owner ref; storage NOT freed (op still holds a ref) */
    kl_comp_life_mark_dead(l);
    kl_comp_life_release(l);
    ASSERT_EQ(g_final, 0);
    ASSERT_EQ(kl_comp_life_target(l), NULL);              /* dead: a late completion sees no owner */

    /* op reaped later: releases the last ref → final release frees the storage exactly once */
    kl_comp_life_release(l);
    ASSERT_EQ(g_final, 1);
}

/* several outstanding ops: on_final fires only when the LAST reference (owner + every op) is gone */
UTEST(comp_life, many_ops_final_once) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlCompLife *l = kl_comp_life_create(&a, (void *)0x1, on_final, NULL, (KlCompLifeDispatchFn)0);
    for (int i = 0; i < 5; i++) kl_comp_life_retain(l);   /* 5 posted ops (refs=6) */
    kl_comp_life_release(l);                              /* owner drops */
    for (int i = 0; i < 4; i++) { kl_comp_life_release(l); ASSERT_EQ(g_final, 0); }
    kl_comp_life_release(l);                              /* the last op */
    ASSERT_EQ(g_final, 1);                                 /* exactly once */
}

/* mark_dead is idempotent; a NULL token is a no-op everywhere */
UTEST(comp_life, idempotent_and_null_safe) {
    KlAllocator a = kl_allocator_default();
    g_final = 0;
    KlCompLife *l = kl_comp_life_create(&a, (void *)0x9, on_final, NULL, (KlCompLifeDispatchFn)0);
    kl_comp_life_mark_dead(l);
    kl_comp_life_mark_dead(l);                            /* idempotent */
    ASSERT_EQ(kl_comp_life_target(l), NULL);
    kl_comp_life_release(l);
    ASSERT_EQ(g_final, 1);
    /* NULL safety */
    kl_comp_life_retain(NULL);
    kl_comp_life_release(NULL);
    kl_comp_life_mark_dead(NULL);
    ASSERT_EQ(kl_comp_life_target(NULL), NULL);
}

UTEST_MAIN();
