/*
 * test_dgram_slots.c — Phase B: packet-slot storage foundation (7A-1 storage separation).
 *
 * Covers the two INDEPENDENTLY-OWNED storage objects and their invariants (no send/recv/pause/close
 * semantics here): the OBJECT-owned outbound pool (KlDgramSlots) and the LIFE-ownable inbound slot
 * (KlDgramInbound) — zero/invalid capacities, size-arithmetic overflow, allocation failure + reuse,
 * acquire/release exhaustion, slot payload independence, and — crucially for the split — that the two
 * are SEPARATE allocations with disjoint lifetimes.
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

#define ASSERT_ZEROED(sp) do {                       \
    ASSERT_TRUE((sp)->block == NULL);                \
    ASSERT_EQ((int)(sp)->block_size, 0);             \
    ASSERT_EQ((int)(sp)->slot_count, 0);             \
    ASSERT_EQ((int)(sp)->free_n, 0);                 \
} while (0)

#define ASSERT_IN_ZEROED(ip) do {                    \
    ASSERT_TRUE((ip)->block == NULL);                \
    ASSERT_EQ((int)(ip)->block_size, 0);             \
} while (0)

/* ══ Outbound pool (KlDgramSlots — object-owned) ════════════════════════════════════════════ */

UTEST(dgram_slots, rejects_zero_and_invalid_capacities) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;

    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 0, 16), -1);   /* zero slot_count */
    ASSERT_ZEROED(&s);
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 4, 0), -1);    /* zero outbound cap */
    ASSERT_ZEROED(&s);
    ASSERT_EQ(kl_dgram_slots_init(NULL, &a, 4, 16), -1); /* NULL object */
    ASSERT_EQ(kl_dgram_slots_init(&s, NULL, 4, 16), -1); /* NULL allocator */
    ASSERT_ZEROED(&s);

    /* A valid init after the rejections still works (object was left reusable). */
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 4, 16), 0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 4);
    kl_dgram_slots_free(&s);
    ASSERT_ZEROED(&s);
}

UTEST(dgram_slots, rejects_size_overflow) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;

    ASSERT_EQ(kl_dgram_slots_init(&s, &a, SIZE_MAX, 1), -1);        /* count * sizeof(slot) overflows */
    ASSERT_ZEROED(&s);
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, (SIZE_MAX / 2) + 1, 4), -1); /* count * out_cap overflows */
    ASSERT_ZEROED(&s);
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 1, SIZE_MAX), -1);        /* the payload total overflows */
    ASSERT_ZEROED(&s);

    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 2, 8), 0);
    kl_dgram_slots_free(&s);
}

UTEST(dgram_slots, alloc_failure_unwinds_and_reuses) {
    KlAllocator fail = failing_allocator();
    KlDgramSlots s;

    ASSERT_EQ(kl_dgram_slots_init(&s, &fail, 4, 16), -1);
    ASSERT_ZEROED(&s);   /* nothing leaked/dangling; object reusable */

    KlAllocator ok = kl_allocator_default();
    ASSERT_EQ(kl_dgram_slots_init(&s, &ok, 4, 16), 0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 4);
    kl_dgram_slots_free(&s);
}

UTEST(dgram_slots, acquire_release_exhaustion) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 3, 16), 0);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 3);

    KlDgramSlot *x = kl_dgram_slots_acquire(&s);
    KlDgramSlot *y = kl_dgram_slots_acquire(&s);
    KlDgramSlot *z = kl_dgram_slots_acquire(&s);
    ASSERT_TRUE(x && y && z);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 0);
    ASSERT_TRUE(kl_dgram_slots_acquire(&s) == NULL);   /* exhausted */

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

UTEST(dgram_slots, outbound_slot_independence) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 3, 16), 0);

    KlDgramSlot *s0 = kl_dgram_slots_acquire(&s);
    KlDgramSlot *s1 = kl_dgram_slots_acquire(&s);
    KlDgramSlot *s2 = kl_dgram_slots_acquire(&s);
    ASSERT_TRUE(s0 && s1 && s2);

    /* Distinct, non-overlapping payload regions, all inside the single outbound block. */
    unsigned char *lo = s.block, *hi = s.block + s.block_size;
    KlDgramSlot *all[3] = { s0, s1, s2 };
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(all[i]->data >= lo && all[i]->data + all[i]->cap <= hi);
        for (int j = i + 1; j < 3; j++)
            ASSERT_TRUE(all[i]->data + all[i]->cap <= all[j]->data ||
                        all[j]->data + all[j]->cap <= all[i]->data);
    }

    memset(s0->data, 0xA1, s0->cap);
    memset(s1->data, 0xB2, s1->cap);
    memset(s2->data, 0xC3, s2->cap);
    for (size_t k = 0; k < s0->cap; k++) ASSERT_EQ(s0->data[k], 0xA1);
    for (size_t k = 0; k < s1->cap; k++) ASSERT_EQ(s1->data[k], 0xB2);
    for (size_t k = 0; k < s2->cap; k++) ASSERT_EQ(s2->data[k], 0xC3);

    kl_dgram_slots_free(&s);
}

UTEST(dgram_slots, release_guards_are_noops) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots s;
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 2, 16), 0);

    KlDgramSlot *x = kl_dgram_slots_acquire(&s);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 1);

    kl_dgram_slots_release(&s, x);
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 2);
    kl_dgram_slots_release(&s, x);       /* double release — no-op (no free-list corruption) */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 2);

    kl_dgram_slots_release(&s, NULL);    /* NULL — no-op */
    KlDgramSlot foreign; memset(&foreign, 0, sizeof(foreign));
    kl_dgram_slots_release(&s, &foreign);/* not one of our slots — no-op */
    ASSERT_EQ((int)kl_dgram_slots_free_count(&s), 2);

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
    ASSERT_EQ(kl_dgram_slots_init(&s, &a, 2, 8), 0);
    kl_dgram_slots_free(&s);
    ASSERT_ZEROED(&s);
    kl_dgram_slots_free(&s);             /* double free — no-op (block already NULL) */
    ASSERT_ZEROED(&s);
}

/* ══ Inbound slot (KlDgramInbound — its own allocation, life-token-ownable) ══════════════════ */

UTEST(dgram_inbound, rejects_zero_and_null) {
    KlAllocator a = kl_allocator_default();
    KlDgramInbound in;

    ASSERT_EQ(kl_dgram_inbound_init(&in, &a, 0), -1);    /* zero cap */
    ASSERT_IN_ZEROED(&in);
    ASSERT_EQ(kl_dgram_inbound_init(NULL, &a, 32), -1);  /* NULL object */
    ASSERT_EQ(kl_dgram_inbound_init(&in, NULL, 32), -1); /* NULL allocator */
    ASSERT_IN_ZEROED(&in);

    ASSERT_EQ(kl_dgram_inbound_init(&in, &a, 32), 0);    /* reusable after rejections */
    ASSERT_EQ((int)kl_dgram_inbound_slot(&in)->cap, 32);
    kl_dgram_inbound_free(&in);
    ASSERT_IN_ZEROED(&in);
}

UTEST(dgram_inbound, alloc_failure_unwinds_and_reuses) {
    KlAllocator fail = failing_allocator();
    KlDgramInbound in;

    ASSERT_EQ(kl_dgram_inbound_init(&in, &fail, 64), -1);
    ASSERT_IN_ZEROED(&in);

    KlAllocator ok = kl_allocator_default();
    ASSERT_EQ(kl_dgram_inbound_init(&in, &ok, 64), 0);
    KlDgramSlot *slot = kl_dgram_inbound_slot(&in);
    ASSERT_EQ((int)slot->cap, 64);
    memset(slot->data, 0xD4, slot->cap);                /* the whole payload is usable */
    for (size_t k = 0; k < slot->cap; k++) ASSERT_EQ(slot->data[k], 0xD4);
    kl_dgram_inbound_free(&in);
}

UTEST(dgram_inbound, free_is_idempotent_and_null_safe) {
    kl_dgram_inbound_free(NULL);

    KlDgramInbound in;
    memset(&in, 0, sizeof(in));
    kl_dgram_inbound_free(&in);          /* never-inited — no-op */

    KlAllocator a = kl_allocator_default();
    ASSERT_EQ(kl_dgram_inbound_init(&in, &a, 16), 0);
    kl_dgram_inbound_free(&in);
    ASSERT_IN_ZEROED(&in);
    kl_dgram_inbound_free(&in);          /* double free — no-op */
    ASSERT_IN_ZEROED(&in);
}

/* The split's whole point: the outbound pool and the inbound slot are SEPARATE allocations with
 * INDEPENDENT lifetimes — freeing one must not touch the other (so a life token can free the inbound
 * long after the object freed its outbound pool). */
UTEST(dgram_inbound, separate_lifetime_from_outbound) {
    KlAllocator a = kl_allocator_default();
    KlDgramSlots out;
    KlDgramInbound in;
    ASSERT_EQ(kl_dgram_slots_init(&out, &a, 2, 16), 0);
    ASSERT_EQ(kl_dgram_inbound_init(&in, &a, 64), 0);

    /* Disjoint allocations. */
    ASSERT_TRUE(out.block != in.block);
    KlDgramSlot *islot = kl_dgram_inbound_slot(&in);
    ASSERT_TRUE(islot->data < out.block || islot->data >= out.block + out.block_size);

    /* Free the OUTBOUND pool first; the inbound slot stays fully usable (ASan/LSan clean). */
    kl_dgram_slots_free(&out);
    ASSERT_ZEROED(&out);
    memset(islot->data, 0xEE, islot->cap);
    for (size_t k = 0; k < islot->cap; k++) ASSERT_EQ(islot->data[k], 0xEE);
    ASSERT_TRUE(kl_dgram_inbound_slot(&in) == islot);

    kl_dgram_inbound_free(&in);
}

UTEST_MAIN();
