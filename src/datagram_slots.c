/*
 * datagram_slots.c — INTERNAL packet-slot storage for the datagram transport (Phase B).
 *
 * See datagram_slots.h. 7A-1 storage separation: the OUTBOUND pool (KlDgramSlots — object-owned) and
 * the INBOUND slot (KlDgramInbound — life-token-ownable) are DISTINCT allocations with distinct
 * owners. One init-time allocation each; all size arithmetic overflow-checked; acquire/release
 * allocation-free.
 */
#include "datagram_slots.h"

#include <stdint.h>   /* SIZE_MAX */
#include <string.h>   /* memset */

/* ── Overflow-checked size arithmetic (each returns 1 on success, 0 on overflow) ─────────── */

static int sz_mul(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}
static int sz_add(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) return 0;
    *out = a + b;
    return 1;
}
/* Round `x` up to a multiple of `align` (a power of two), overflow-checked. */
static int sz_align_up(size_t x, size_t align, size_t *out) {
    size_t m = align - 1;
    if (x > SIZE_MAX - m) return 0;
    *out = (x + m) & ~m;
    return 1;
}

int kl_dgram_slots_init(KlDgramSlots *s, KlAllocator *a,
                        size_t slot_count, size_t out_cap) {
    if (!s || !a)
        return -1;
    memset(s, 0, sizeof(*s));   /* leave the object zeroed on every failure path (reusable) */
    if (slot_count == 0 || out_cap == 0)
        return -1;

    /* Section sizes, each checked. Layout in the block:
     *   [ KlDgramSlot out[slot_count] ][ size_t freelist[slot_count] ][ outbound payload bytes ]
     * The metadata sections are aligned so the free-list (size_t) lands on a size_t boundary;
     * payload bytes are char and need no alignment. */
    size_t slots_bytes, free_bytes, outpay_bytes, meta_bytes, total;
    if (!sz_mul(slot_count, sizeof(KlDgramSlot), &slots_bytes)) return -1;
    if (!sz_mul(slot_count, sizeof(size_t), &free_bytes))       return -1;
    if (!sz_mul(slot_count, out_cap, &outpay_bytes))           return -1;

    if (!sz_align_up(slots_bytes, sizeof(size_t), &slots_bytes)) return -1; /* freelist alignment */
    if (!sz_add(slots_bytes, free_bytes, &meta_bytes))          return -1;
    if (!sz_add(meta_bytes, outpay_bytes, &total))              return -1;

    unsigned char *block = kl_malloc(a, total);
    if (!block)
        return -1;

    s->alloc      = a;
    s->block      = block;
    s->block_size = total;
    s->slot_count = slot_count;
    s->out_cap    = out_cap;
    s->out        = (KlDgramSlot *)block;
    s->freelist   = (size_t *)(block + slots_bytes);

    unsigned char *pay = block + meta_bytes;   /* outbound payload region base */
    for (size_t i = 0; i < slot_count; i++) {
        memset(&s->out[i], 0, sizeof(s->out[i]));
        s->out[i].data = pay + i * out_cap;    /* i*out_cap < outpay_bytes (checked) — no overflow */
        s->out[i].cap  = out_cap;
        s->out[i].tos  = -1;
        /* Fill the free-list so acquire hands out index 0, 1, 2, … first (LIFO over a reversed
         * initial fill). */
        s->freelist[i] = slot_count - 1 - i;
    }
    s->free_n = slot_count;
    return 0;
}

void kl_dgram_slots_free(KlDgramSlots *s) {
    if (!s)
        return;
    if (s->block && s->alloc)
        kl_free(s->alloc, s->block, s->block_size);
    memset(s, 0, sizeof(*s));   /* reusable */
}

/* ── Inbound slot (its own allocation; life-token-ownable) ─────────────────────────────────── */

int kl_dgram_inbound_init(KlDgramInbound *in, KlAllocator *a, size_t cap) {
    if (!in || !a)
        return -1;
    memset(in, 0, sizeof(*in));   /* zeroed + reusable on every failure path */
    if (cap == 0)
        return -1;

    unsigned char *block = kl_malloc(a, cap);   /* payload only; the slot metadata is embedded */
    if (!block)
        return -1;

    in->alloc      = a;
    in->block      = block;
    in->block_size = cap;
    memset(&in->slot, 0, sizeof(in->slot));
    in->slot.data = block;
    in->slot.cap  = cap;
    in->slot.tos  = -1;
    return 0;
}

void kl_dgram_inbound_free(KlDgramInbound *in) {
    if (!in)
        return;
    if (in->block && in->alloc)
        kl_free(in->alloc, in->block, in->block_size);
    memset(in, 0, sizeof(*in));   /* reusable */
}

KlDgramSlot *kl_dgram_slots_acquire(KlDgramSlots *s) {
    if (!s || s->free_n == 0)
        return NULL;
    size_t idx = s->freelist[--s->free_n];
    KlDgramSlot *slot = &s->out[idx];
    /* Reset transient per-packet metadata (storage-level only). `cap`/`data` are fixed. */
    slot->len   = 0;
    slot->tos   = -1;
    slot->flags = 0;
    slot->recoverable = 0;
    slot->in_use = 1;
    memset(&slot->peer,  0, sizeof(slot->peer));
    memset(&slot->local, 0, sizeof(slot->local));
    return slot;
}

void kl_dgram_slots_release(KlDgramSlots *s, KlDgramSlot *slot) {
    if (!s || !slot)
        return;
    /* Must be one of THIS pool's outbound slots. Establish membership by pointer EQUALITY — which is
     * defined for any two pointers — rather than relational `<`/`>=` against s->out, which is UB for a
     * FOREIGN pointer (the API accepts one as a guarded no-op). The equality scan also yields the index
     * for the free-list without a (foreign-pointer) subtraction. slot_count is a small fixed bound. */
    size_t idx = s->slot_count;
    for (size_t i = 0; i < s->slot_count; i++) {
        if (&s->out[i] == slot) { idx = i; break; }
    }
    if (idx == s->slot_count)   /* not one of our slots — no-op */
        return;
    if (!slot->in_use)          /* not acquired → double-release, ignore */
        return;
    if (s->free_n >= s->slot_count)   /* defensive: free-list is full, nothing to return */
        return;
    slot->in_use = 0;
    s->freelist[s->free_n++] = idx;
}
