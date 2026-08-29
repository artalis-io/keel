/*
 * allocator_validate.h: INTERNAL. Shared valid-allocator predicate.
 *
 * A KlAllocator is a caller-supplied vtable (allocator.h). kl_malloc/kl_realloc/kl_free call its
 * malloc/realloc/free ops unconditionally, so a non-NULL allocator whose malloc, realloc, or free is
 * NULL faults on the first allocation. Public boundaries that accept an allocator validate it once, at
 * the point where it is first stored or used, before any allocation or state mutation. `ctx` is
 * optional (opaque, passed to the ops).
 *
 * Contract (F2-C2):
 *   - A NON-NULL allocator is valid only when malloc, realloc, and free are all non-NULL.
 *   - NULL is NOT judged here: each boundary resolves its own documented NULL meaning first (a NULL
 *     that selects a default is resolved to that default, then validated; a NULL that is invalid is
 *     rejected by the boundary). A malformed non-NULL allocator is never silently replaced by a
 *     default.
 *   - kl_malloc/kl_realloc/kl_free themselves do NOT call this (hot path); they carry an explicit
 *     valid-allocator precondition, as do void-return APIs that cannot fail safely.
 *
 * Header-only so every boundary shares one definition; not installed, no ABI commitment.
 */
#ifndef KEEL_SRC_ALLOCATOR_VALIDATE_H
#define KEEL_SRC_ALLOCATOR_VALIDATE_H

#include <keel/allocator.h>

/* True iff `a` is a usable allocator: non-NULL with all three required ops. A NULL `a` returns 0
 * here; a boundary that treats NULL as a documented default must resolve it BEFORE calling this. */
static inline int kl_allocator_ops_valid(const KlAllocator *a) {
    return a && a->malloc && a->realloc && a->free;
}

#endif /* KEEL_SRC_ALLOCATOR_VALIDATE_H */
