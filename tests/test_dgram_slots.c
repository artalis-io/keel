/*
 * test_dgram_slots.c — Phase B step 1: packet-slot storage foundation.
 *
 * Covers the storage invariants only (no send/recv/pause/close semantics yet):
 * zero/invalid capacities, size-arithmetic overflow, allocation failure + reuse, slot
 * independence (disjoint payloads), and outbound exhaustion not affecting the inbound slot.
 */
#include "utest.h"

#include "../src/datagram_slots.h"

#include <stdint.h>
#include <string.h>

/* ── A failing allocator (malloc always NULL) to drive the alloc-failure path ─────────────── */
static void *fail_malloc(void *ctx, size_t size)                     { (void)ctx; (void)size; return NULL; }
static void *fail_realloc(void *c, void *p, size_t o, size_t n)      { (void)c; (void)p; (void)o; (void)n; return NULL; }
static void  fail_free(void *ctx, void *p, size_t size)             { (void)ctx; (void)p; (void)size; }
static KlAllocator failing_allocator(void) {
    KlAllocator a = { fail_malloc, fail_realloc, fail_free, NULL };
    return a;
}

/* A slots object left zeroed (post-failure / pre-init) is safely reusable. Macro, not a
 * function, so utest's ASSERT_* (which reference the per-test result) stay in the test body. */
#define ASSERT_ZEROED(sp) do {                       \
    ASSERT_TRUE((sp)->block == NULL);                \
    ASSERT_EQ((int)(sp)->block_size, 0);             \
    ASSERT_EQ((int)(sp)->slot_count, 0);             \
    ASSERT_EQ((int)(sp)->free_n, 0);                 \
} while (0)

UTEST(dgram_slots, rejects_zero_and_invalid_capacities) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;

    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 0, 16, 32), -1);   /* zero slot_count */
    ASSERT_ZEROED(&s);
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 4, 0, 32), -1);    /* zero outbound cap */
    ASSERT_ZEROED(&s);
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 4, 16, 0), -1);    /* zero inbound cap */
    ASSERT_ZEROED(&s);
    ASSERT_EQ(kl_dgram_slots_init(NULL, &a, 4, 16, 32), -1); /* NULL object */
    ASSERT_EQ(kl_dgram_slots_init(&s, NULL, 4, 16, 32), -1); /* NULL allocator */
    ASSERT_ZEROED(&s);

    /* A valid init after the rejections still works (object was left reusable). */
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 4, 16, 32), 0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 4);
    kl_dgram_slots_free(&s);
    ASSERT_ZEROED(&s);
}

UTEST(dgram_slots, rejects_size_overflow) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;

    /* slot_count * sizeof(KlDgramSlot) overflows. */
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, SIZE_MAX, 1, 1), -1);
    ASSERT_ZEROED(&s);

    /* slot_count * out_cap overflows (both large). */
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, (SIZE_MAX / 2) + 1, 4, 1), -1);
    ASSERT_ZEROED(&s);

    /* outbound payload total + inbound cap overflows: 1 * SIZE_MAX, then + 1. */
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 1, SIZE_MAX, 1), -1);
    ASSERT_ZEROED(&s);

    /* A sane init still succeeds afterwards. */
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 2, 8, 8), 0);
    kl_dgram_slots_free(&s);
}

UTEST(dgram_slots, alloc_failure_unwinds_and_reuses) {
    KlAllocator fail = failing_allocator();
    KlDgramSlots s;

    ASSERT_EQ(kl_dgram_slots_init(&s, &fail, 4, 16, 32), -1);
    ASSERT_ZEROED(&s);   /* nothing leaked/dangling; object reusable */

    /* Re-init the SAME object with a working allocator succeeds. */
    KlAllocator ok = kl_allocator_default();
    ASSERT_EQ(kl_dgram_slots_init(&s, &ok, 4, 16, 32), 0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 4);
    kl_dgram_slots_free(&s);
}

UTEST(dgram_slots, acquire_release_exhaustion) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 3, 16, 16), 0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 3);

    KlDgramSlot *x = kl_dgram_slots_acquire(&s);
    KlDgramSlot *y = kl_dgram_slots_acquire(&s);
    KlDgramSlot *z = kl_dgram_slots_acquire(&s);
    ASSERT_TRUE(x && y && z);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 0);
    ASSERT_TRUE(kl_dgram_slots_acquire(&s) == NULL);   /* exhausted */

    /* Each acquired slot has its capacity + a reset transient state. */
    ASSERT_EQ((int)x->cap, 16);
    ASSERT_EQ((int)x->len, 0);
    ASSERT_EQ(x->tos, -1);

    kl_dgram_slots_release(&s, y);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 1);
    KlDgramSlot *w = kl_dgram_slots_acquire(&s);       /* reuse the returned slot */
    ASSERT_TRUE(w != NULL);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 0);

    kl_dgram_slots_free(&s);
}

UTEST(dgram_slots, slot_independence) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 3, 16, 32), 0);

    KlDgramSlot *s0 = kl_dgram_slots_acquire(&s);
    KlDgramSlot *s1 = kl_dgram_slots_acquire(&s);
    KlDgramSlot *s2 = kl_dgram_slots_acquire(&s);
    KlDgramSlot *in = kl_dgram_slots_inbound(&s);
    ASSERT_TRUE(s0 && s1 && s2 && in);

    /* Distinct, non-overlapping payload regions, all inside the single block. */
    unsigned char *lo = s.block, *hi = s.block + s.block_size;
    KlDgramSlot *all[4] = { s0, s1, s2, in };
    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(all[i]->data >= lo && all[i]->data + all[i]->cap <= hi);
        for (int j = i + 1; j < 4; j++) {
            /* regions [data, data+cap) must not overlap */
            ASSERT_TRUE(all[i]->data + all[i]->cap <= all[j]->data ||
                        all[j]->data + all[j]->cap <= all[i]->data);
        }
    }

    /* Write a distinct pattern to each; verify no cross-contamination. */
    memset(s0->data, 0xA1, s0->cap);
    memset(s1->data, 0xB2, s1->cap);
    memset(s2->data, 0xC3, s2->cap);
    memset(in->data, 0xD4, in->cap);
    for (size_t k = 0; k < s0->cap; k++) ASSERT_EQ(s0->data[k], 0xA1);
    for (size_t k = 0; k < s1->cap; k++) ASSERT_EQ(s1->data[k], 0xB2);
    for (size_t k = 0; k < s2->cap; k++) ASSERT_EQ(s2->data[k], 0xC3);
    for (size_t k = 0; k < in->cap; k++) ASSERT_EQ(in->data[k], 0xD4);

    kl_dgram_slots_free(&s);
}

UTEST(dgram_slots, exhaustion_does_not_affect_inbound) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 2, 16, 64), 0);

    KlDgramSlot *in = kl_dgram_slots_inbound(&s);
    ASSERT_EQ((int)in->cap, 64);

    /* Exhaust all outbound slots. */
    ASSERT_TRUE(kl_dgram_slots_acquire(&s) != NULL);
    ASSERT_TRUE(kl_dgram_slots_acquire(&s) != NULL);
    ASSERT_TRUE(kl_dgram_slots_acquire(&s) == NULL);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 0);

    /* The inbound slot is fully usable regardless of outbound exhaustion. */
    memset(in->data, 0xEE, in->cap);
    for (size_t k = 0; k < in->cap; k++) ASSERT_EQ(in->data[k], 0xEE);
    ASSERT_TRUE(kl_dgram_slots_inbound(&s) == in);   /* same dedicated slot */

    kl_dgram_slots_free(&s);
}

UTEST(dgram_slots, release_guards_are_noops) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 2, 16, 16), 0);

    KlDgramSlot *x = kl_dgram_slots_acquire(&s);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 1);

    kl_dgram_slots_release(&s, x);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 2);
    kl_dgram_slots_release(&s, x);       /* double release — must be a no-op (no free-list corruption) */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 2);

    kl_dgram_slots_release(&s, NULL);    /* NULL — no-op */
    KlDgramSlot foreign; memset(&foreign, 0, sizeof(foreign));
    kl_dgram_slots_release(&s, &foreign);/* not one of our slots — no-op */
    kl_dgram_slots_release(&s, kl_dgram_slots_inbound(&s)); /* inbound is not an outbound slot — no-op */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 2);

    /* Still fully functional: can acquire both. */
    ASSERT_TRUE(kl_dgram_slots_acquire(&s) != NULL);
    ASSERT_TRUE(kl_dgram_slots_acquire(&s) != NULL);
    ASSERT_TRUE(kl_dgram_slots_acquire(&s) == NULL);

    kl_dgram_slots_free(&s);
}

UTEST(dgram_slots, free_is_idempotent_and_null_safe) {
    kl_dgram_slots_free(NULL);           /* NULL-safe */

    KlDgramSlots s;
    memset(&s, 0, sizeof(s));
    kl_dgram_slots_free(&s);             /* never-inited (zeroed) — no-op */

    KlAllocator a = kl_allocator_default();
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 2, 8, 8), 0);
    kl_dgram_slots_free(&s);
    ASSERT_ZEROED(&s);
    kl_dgram_slots_free(&s);             /* double free — no-op (block already NULL) */
    ASSERT_ZEROED(&s);
}

UTEST_MAIN();
