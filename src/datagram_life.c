/*
 * datagram_life.c — transport-neutral liveness + refcount token for datagram completion ops.
 * See datagram_life.h. Single-threaded (event-loop thread only); a plain-int refcount.
 */
#include "datagram_life.h"

#include <string.h>

struct KlDgramLife {
    KlAllocator *alloc;                 /* event-ctx / backend allocator (outlives KlUdp) */
    int          refs;                  /* owner ref (1) + one per posted backend op */
    int          live;                  /* 1 = target valid; 0 = owner freed (kl_udp_free) */
    void        *target;                /* the owner (KlUdp), or NULL once dead */
    void       (*on_final)(void *ctx);  /* frees owner receive storage on the final release */
    void        *final_ctx;
    KlDgramOwnerKind  kind;             /* completion-routing identity (7B-2a) */
    KlDgramDispatchFn dispatch;         /* the owner's completion handler */
};

KlDgramLife *kl_dgram_life_create(KlAllocator *alloc, void *target,
                                  void (*on_final)(void *final_ctx), void *final_ctx,
                                  KlDgramOwnerKind kind, KlDgramDispatchFn dispatch) {
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
    l->kind      = kind;
    l->dispatch  = dispatch;
    return l;
}

KlDgramOwnerKind kl_dgram_life_kind(const KlDgramLife *l) {
    return l ? l->kind : KL_DGRAM_OWNER_UDP;
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
