/*
 * datagram_recv.c: INTERNAL serial receive + strict pause/resume over the dedicated inbound slot.
 * See datagram_recv.h. Mirrors the KlStream read machinery: pre-arm before the
 * hook, iterative trampoline for synchronous (inline) completion, strict pause with a single held
 * datagram, and paused/stopped re-checked after every delivery.
 */
#include "datagram_recv.h"

#include <string.h>

static void recv_fail(KlDgramRecv *r);   /* defined below; used by the view drain */

/* Clear the inbound slot's per-datagram metadata before the provider fills it, so a prior packet's
 * source/dest/flags can NEVER survive into the next receive (contract §8: no stale metadata across
 * slot reuse). Called before every arm (completion) / pull (readiness). Never runs while a datagram
 * is held (no arm/pull in that state), so a held snapshot is preserved. */
static void recv_reset_inbound(KlDgramRecv *r) {
    KlDgramSlot *in = kl_dgram_inbound_slot(r->inbound);
    memset(&in->peer,  0, sizeof(in->peer));    /* family → KL_AF_UNSPEC (0) = absent */
    memset(&in->local, 0, sizeof(in->local));
    in->len   = 0;
    in->flags = 0;
    in->tos   = -1;
}

/* Finalize the inbound datagram from the provider-populated slot fields, enforcing the Tier-1
 * receive contract in place: captured-prefix length (never exceeds in_cap), explicit
 * KL_DGRAM_TRUNCATED (size overflow OR a provider hint, e.g. IOCP MSG_TRUNC at exact capacity),
 * KL_DGRAM_HAS_LOCAL only when a valid local is present (stale local cleared otherwise), and a
 * MANDATORY peer. `reported_len` is the provider's datagram length. Returns 1 = deliverable, 0 =
 * provider contract violation (peer absent) → the caller fails without delivering. */
static int recv_finalize(KlDgramRecv *r, size_t reported_len) {
    KlDgramSlot *in = kl_dgram_inbound_slot(r->inbound);
    if (kl_sockaddr_family(&in->peer) == KL_AF_UNSPEC)
        return 0;                                /* peer mandatory (invariant 7 / DNS anti-spoof) */
    size_t   in_cap    = r->inbound->slot.cap;
    int      truncated = (reported_len > in_cap) || (in->flags & KL_DGRAM_TRUNCATED);
    size_t   captured  = reported_len > in_cap ? in_cap : reported_len;
    int      has_local = (in->flags & KL_DGRAM_HAS_LOCAL) &&
                         kl_sockaddr_family(&in->local) != KL_AF_UNSPEC;
    if (!has_local)
        memset(&in->local, 0, sizeof(in->local));  /* never leak a prior packet's local */
    in->len   = captured;                        /* canonical: what the callback observes */
    in->flags = (truncated ? KL_DGRAM_TRUNCATED : 0u) | (has_local ? KL_DGRAM_HAS_LOCAL : 0u);
    return 1;
}

/* Deliver the finalized inbound datagram exactly once: `peer` is always non-NULL; `local` is passed
 * only when KL_DGRAM_HAS_LOCAL is set. Reads only the canonical slot fields (set by recv_finalize),
 * so a held datagram delivers the same snapshot on resume. */
static void recv_present(KlDgramRecv *r) {
    KlDgramSlot *in = kl_dgram_inbound_slot(r->inbound);
    const KlSockAddr *local = (in->flags & KL_DGRAM_HAS_LOCAL) ? &in->local : NULL;
    r->deliver(r->deliver_ctx, in->data, in->len, &in->peer, local, in->flags);
}

void kl_dgram_recv_set_activity_cb(KlDgramRecv *r, void (*cb)(void *ctx, int delta), void *ctx) {
    if (!r) return;
    r->on_activity = cb; r->activity_ctx = ctx;
}
void kl_dgram_recv_set_view_pull(KlDgramRecv *r, KlDgramRecvViewFn view_pull, void *ctx) {
    if (!r) return;
    r->view_pull = view_pull;
    if (ctx) r->hook_ctx = ctx;   /* the batch cursor's context (else keep the readiness hook_ctx) */
}

/* Deliver ONE borrowed view (peer-mandatory finalize on the view, NOT the recv_cap clamp; the
 * view carries its own len/flags). Returns 0 delivered, -1 = peer-absent contract violation (fails). */
static int recv_present_view(KlDgramRecv *r, const KlDgramRxView *v) {
    if (kl_sockaddr_family(&v->peer) == KL_AF_UNSPEC) { recv_fail(r); return -1; }
    if (v->len > 0 && v->data == NULL) { recv_fail(r); return -1; }   /* malformed borrowed payload */
    const KlSockAddr *local = (v->flags & KL_DGRAM_HAS_LOCAL) ? &v->local : NULL;
    r->deliver(r->deliver_ctx, v->data, v->len, &v->peer, local, v->flags);
    return 0;
}

/* The borrowed-view readable drain (batch mode). Same serial contract as the inbound-slot loop: one
 * view_pull → deliver, re-checking paused/stopped each iteration. `allow_refill` = 1 for the readable
 * edge (refill via recv_batch when the cursor drains); 0 for the resume held-drain (deliver only
 * already-buffered datagrams, NEVER read new kernel data). Runs inside a recv_enter/leave bracket. */
static int recv_drive_view(KlDgramRecv *r, int allow_refill) {
    int ret = 0;
    while (!r->paused && !r->stopped) {
        KlDgramRxView v; memset(&v, 0, sizeof(v));
        int pr = r->view_pull(r->hook_ctx, &v, allow_refill);
        if (pr == 0) break;                              /* drained (would-block / cursor empty) */
        if (pr < 0) { recv_fail(r); ret = -1; break; }   /* fatal receive error */
        if (recv_present_view(r, &v) < 0) { ret = -1; break; }
    }
    return ret;
}
static void recv_enter(KlDgramRecv *r) { if (r->on_activity) r->on_activity(r->activity_ctx, +1); }
/* recv_leave is the LAST action of a public op; it may detach + free the object via the coordinator. */
static void recv_leave(KlDgramRecv *r) { if (r->on_activity) r->on_activity(r->activity_ctx, -1); }

int kl_dgram_recv_init(KlDgramRecv *r, KlDgramInbound *inbound, int completion,
                       KlDgramRecvDeliverFn deliver, void *deliver_ctx,
                       KlDgramRecvArmFn arm, KlDgramRecvDisarmFn disarm,
                       KlDgramRecvPullFn pull, void *hook_ctx) {
    if (!r)
        return -1;
    memset(r, 0, sizeof(*r));
    if (!inbound || inbound->slot.cap == 0 || !deliver || !arm)
        return -1;
    if (!completion && (!disarm || !pull))   /* readiness pause MUST drop interest + pull to drain */
        return -1;
    r->inbound     = inbound;
    r->completion  = completion ? 1 : 0;
    r->arm         = arm;
    r->disarm      = disarm;
    r->pull        = pull;
    r->hook_ctx    = hook_ctx;
    r->deliver     = deliver;
    r->deliver_ctx = deliver_ctx;
    r->inited      = 1;
    return 0;
}

/* Arm the next receive. ITERATIVE trampoline: a provider that completes synchronously inside arm()
 * (completion mode) cannot grow the C stack: a re-arm requested during a synchronous delivery sets
 * rearm_pending and returns; this loop performs it. recv_inflight is set BEFORE the hook so a
 * synchronous on_complete sees it. Precondition: the caller decided arming is due. */
static int recv_arm(KlDgramRecv *r) {
    if (r->arming) { r->rearm_pending = 1; return 0; }   /* defer nested re-arm to the trampoline */

    int result = 0;
    for (;;) {
        if (r->stopped || r->paused || r->recv_inflight || r->held) break;
        recv_reset_inbound(r);               /* clean slot before the provider fills it (incl. inline) */
        r->recv_inflight    = 1;
        r->completed_inline = 0;
        r->rearm_pending    = 0;
        r->arming           = 1;
        int rc = r->arm(r->hook_ctx);        /* may sync-complete (on_complete), pause, stop */
        r->arming = 0;
        if (r->completed_inline) {
            if (r->rearm_pending) continue;  /* iterative, not recursive */
            break;
        }
        if (rc < 0) {                        /* genuine arm failure: no sync completion */
            if (r->recv_inflight) r->recv_inflight = 0;
            r->stopped = 1;
            result = -1;
            break;
        }
        break;                               /* posted async; await the completion */
    }
    return result;
}

/* Deliver one finalized datagram from the canonical inbound slot, then re-arm, unless the callback
 * paused/stopped. A failed receive NEVER reaches here (routed through recv_fail); a truncated one
 * DOES (delivered with KL_DGRAM_TRUNCATED). State re-checked AFTER the callback. */
static int recv_deliver_and_continue(KlDgramRecv *r) {
    recv_present(r);
    if (r->stopped || r->paused) return 0;   /* callback stopped/paused; do not re-arm */
    if (r->recv_inflight) return 0;          /* a synchronous completion already re-armed */
    return recv_arm(r);
}

/* A receive FAILED (provider error) or violated the length contract: stop the receive side, set the
 * error state, and NEVER deliver (a failure is not a datagram, and is never held). Readiness: drop
 * READ interest EXACTLY ONCE and clear recv_inflight, so nothing is left logically armed and cleanup
 * /free are not wedged. Completion: physical retirement is the caller's (recv_inflight is already
 * cleared before this in on_complete). Safe under a subsequent idempotent kl_dgram_recv_stop. */
static void recv_fail(KlDgramRecv *r) {
    r->error    = 1;
    r->stopped  = 1;
    r->held     = 0;
    r->held_len = 0;
    if (!r->completion && r->recv_inflight) {
        r->disarm(r->hook_ctx);
        r->recv_inflight = 0;
    }
}

int kl_dgram_recv_start(KlDgramRecv *r) {
    if (!r || !r->inited)
        return -1;
    r->started = 1;   /* latched regardless of the arm result / callback; attach is now refused */
    if (r->paused || r->stopped || r->recv_inflight || r->held)
        return 0;
    recv_enter(r);
    int ret = recv_arm(r);
    recv_leave(r);                           /* LAST action; an inline completion may detach */
    return ret;
}

int kl_dgram_recv_on_complete(KlDgramRecv *r, size_t len, int ok) {
    if (!r || !r->inited)
        return -1;
    if (!r->recv_inflight)
        return 0;                            /* duplicate/spurious: drop; do NOT disturb held data */
    recv_enter(r);
    r->recv_inflight = 0;                    /* this receive op has retired */
    if (r->arming)
        r->completed_inline = 1;             /* synchronous completion of the current arm */

    int ret;
    if (r->stopped)                          /* a stopped/cancelled op retired; drop the data */
        ret = 0;
    else if (!ok)                            /* a FAILED receive is not a datagram; never deliver */
        { recv_fail(r); ret = -1; }
    else if (!recv_finalize(r, len))         /* peer absent = provider contract violation; fail safe */
        { recv_fail(r); ret = -1; }
    else if (r->paused)                      /* STRICT: hold the finalized datagram (canonical slot) */
        { r->held = 1; r->held_len = kl_dgram_inbound_slot(r->inbound)->len; ret = 0; }
    else
        ret = recv_deliver_and_continue(r);
    recv_leave(r);                           /* LAST action; coordinator finalize may detach (frees r) */
    return ret;
}

int kl_dgram_recv_on_readable(KlDgramRecv *r) {
    if (!r || !r->inited || r->completion)
        return -1;                           /* readiness only */
    if (r->stopped || r->paused || !r->recv_inflight)
        return 0;
    recv_enter(r);
    int ret;
    if (r->view_pull) {                      /* borrowed-view batch mode */
        ret = recv_drive_view(r, /*allow_refill*/1);
        recv_leave(r);
        return ret;
    }
    ret = 0;
    /* Serial provider receives: one pull → one delivery, re-checking paused/stopped each iteration. */
    while (!r->paused && !r->stopped && r->recv_inflight) {
        recv_reset_inbound(r);               /* fresh metadata slot before the provider fills it */
        size_t len = 0;
        int pr = r->pull(r->hook_ctx, &len);
        if (pr == 0)
            break;                           /* would-block; drained */
        if (pr < 0)                          /* fatal receive error: disarm + clear (recv_fail) */
            { recv_fail(r); ret = -1; break; }
        if (!recv_finalize(r, len))          /* peer absent = provider contract violation; fail safe */
            { recv_fail(r); ret = -1; break; }
        recv_present(r);                     /* one datagram (captured-prefix; TRUNCATED if oversized) */
        /* loop re-checks paused/stopped (the delivery may have set them) */
    }
    recv_leave(r);                           /* LAST action; coordinator finalize may detach (frees r) */
    return ret;
}

void kl_dgram_recv_pause(KlDgramRecv *r) {
    if (!r || !r->inited || r->paused)
        return;                              /* idempotent */
    recv_enter(r);
    r->paused = 1;
    if (!r->completion && r->recv_inflight) {
        /* Readiness: drop READ interest (only if armed); a pending readable is cancelled: a
         * SYNCHRONOUS physical retirement. */
        r->disarm(r->hook_ctx);
        r->recv_inflight = 0;
    }
    /* Completion: leave a posted recv physically in flight; held on completion. */
    recv_leave(r);
}

int kl_dgram_recv_resume(KlDgramRecv *r) {
    if (!r || !r->inited || !r->paused)
        return 0;                            /* idempotent */
    recv_enter(r);
    r->paused = 0;
    int ret;
    if (r->stopped)
        ret = 0;
    else if (r->view_pull) {
        /* Batch mode: pause dropped READ interest but the ext CURSOR may hold undelivered
         * logical datagrams. Drain the held cursor FIRST (allow_refill=0: deliver only buffered data,
         * never read new kernel data), THEN re-arm READ interest if still active. Ordering matters: the
         * held data is delivered even if the re-arm fails, and no new socket data is consumed ahead of
         * the retained buffer. */
        ret = recv_drive_view(r, /*allow_refill*/0);
        if (ret == 0 && !r->paused && !r->stopped && !r->recv_inflight)
            ret = recv_arm(r);
    } else if (r->held) {
        r->held = 0; r->held_len = 0;        /* consume once; the canonical snapshot is still in-slot */
        ret = recv_deliver_and_continue(r);  /* delivers the preserved metadata (peer/local/flags/len) */
    } else if (r->recv_inflight)             /* completion: a recv posted before pause is still out */
        ret = 0;
    else
        ret = recv_arm(r);
    recv_leave(r);                           /* LAST action; a delivery/inline completion may detach */
    return ret;
}

void kl_dgram_recv_stop(KlDgramRecv *r) {
    if (!r || !r->inited || r->stopped)
        return;                              /* idempotent */
    recv_enter(r);
    r->stopped  = 1;
    r->held     = 0;                         /* discard held completion; not delivered */
    r->held_len = 0;
    if (!r->completion) {
        if (r->recv_inflight) r->disarm(r->hook_ctx);   /* SYNCHRONOUS physical retirement */
        r->recv_inflight = 0;
    }
    /* Completion: a posted recv may still be PHYSICALLY outstanding; it retires (dropped) on its
     * completion. The logical/physical split is what confirmed-detachment close builds on. */
    recv_leave(r);
}

int kl_dgram_recv_free(KlDgramRecv *r) {
    if (!r)
        return 0;
    if (r->recv_inflight)
        return -1;   /* a receive references the inbound slot; must not free underneath it */
    memset(r, 0, sizeof(*r));   /* reusable (borrows no heap; the slots are the caller's) */
    return 0;
}
