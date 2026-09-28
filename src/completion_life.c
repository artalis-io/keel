/*
 * completion_life.c: transport-neutral liveness + refcount token for completion ops.
 * See completion_life.h. Single-threaded (event-loop thread only); a plain-int refcount.
 */
#include "completion_life.h"

#include <string.h>

struct KlCompLife {
    KlAllocator *alloc;                 /* event-ctx / backend allocator (outlives the transport) */
    int          refs;                  /* owner ref (1) + one per posted backend op */
    int          live;                  /* 1 = target valid; 0 = owner torn down */
    void        *target;                /* the owner (KlDgramCore), or NULL once dead */
    void       (*on_final)(void *ctx);  /* frees owner receive storage on the final release */
    void        *final_ctx;
    KlCompLifeDispatchFn dispatch;         /* the owner's completion handler (routing identity) */
};

KlCompLife *kl_comp_life_create(KlAllocator *alloc, void *target,
                                  void (*on_final)(void *final_ctx), void *final_ctx,
                                  KlCompLifeDispatchFn dispatch) {
    if (!alloc)
        return NULL;
    KlCompLife *l = kl_malloc(alloc, sizeof(*l));
    if (!l)
        return NULL;
    l->alloc     = alloc;
    l->refs      = 1;                   /* the owner reference */
    l->live      = 1;
    l->target    = target;
    l->on_final  = on_final;
    l->final_ctx = final_ctx;
    l->dispatch  = dispatch;
    return l;
}

KlCompLifeDispatchFn kl_comp_life_dispatch(const KlCompLife *l) {
    return l ? l->dispatch : (KlCompLifeDispatchFn)0;
}

void kl_comp_life_retain(KlCompLife *l) {
    if (l)
        l->refs++;
}

void kl_comp_life_release(KlCompLife *l) {
    if (!l)
        return;
    if (--l->refs > 0)
        return;
    /* Final release: free the owner-side receive storage (on_final frees `final_ctx`, NOT the token),
     * then free the token itself. */
    if (l->on_final)
        l->on_final(l->final_ctx);      /* frees the inbound slot + receive machine (owner storage) */
    kl_free(l->alloc, l, sizeof(*l));
}

void kl_comp_life_mark_dead(KlCompLife *l) {
    if (l) {
        l->live   = 0;
        l->target = NULL;
    }
}

void *kl_comp_life_target(const KlCompLife *l) {
    return (l && l->live) ? l->target : NULL;
}
