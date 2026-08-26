/*
 * datagram_life.c: transport-neutral liveness + refcount token for datagram completion ops.
 * See datagram_life.h. Single-threaded (event-loop thread only); a plain-int refcount.
 */
#include "datagram_life.h"

#include <string.h>

struct KlDgramLife {
    KlAllocator *alloc;                 /* event-ctx / backend allocator (outlives the transport) */
    int          refs;                  /* owner ref (1) + one per posted backend op */
    int          live;                  /* 1 = target valid; 0 = owner torn down */
    void        *target;                /* the owner (KlDgramCore), or NULL once dead */
    void       (*on_final)(void *ctx);  /* frees owner receive storage on the final release */
    void        *final_ctx;
    KlDgramDispatchFn dispatch;         /* the owner's completion handler (routing identity) */
};

KlDgramLife *kl_dgram_life_create(KlAllocator *alloc, void *target,
                                  void (*on_final)(void *final_ctx), void *final_ctx,
                                  KlDgramDispatchFn dispatch) {
    if (!alloc)
        return NULL;
    KlDgramLife *l = kl_malloc(alloc, sizeof(*l));
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

KlDgramDispatchFn kl_dgram_life_dispatch(const KlDgramLife *l) {
    return l ? l->dispatch : (KlDgramDispatchFn)0;
}

void kl_dgram_life_retain(KlDgramLife *l) {
    if (l)
        l->refs++;
}

void kl_dgram_life_release(KlDgramLife *l) {
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

void kl_dgram_life_mark_dead(KlDgramLife *l) {
    if (l) {
        l->live   = 0;
        l->target = NULL;
    }
}

void *kl_dgram_life_target(const KlDgramLife *l) {
    return (l && l->live) ? l->target : NULL;
}
