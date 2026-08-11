/*
 * datagram_recv.c — INTERNAL serial receive + strict pause/resume over the dedicated inbound slot
 * (Phase B, step 3). See datagram_recv.h. Mirrors the KlStream read machinery: pre-arm before the
 * hook, iterative trampoline for synchronous (inline) completion, strict pause with a single held
 * datagram, and paused/stopped re-checked after every delivery.
 */
#include "datagram_recv.h"

#include <string.h>   /* (none needed beyond declarations) */

static const KlSockAddr *addr_or_null(const KlSockAddr *a) {
    return (a && kl_sockaddr_family(a) != KL_AF_UNSPEC) ? a : NULL;
}

void kl_dgram_recv_set_activity_cb(KlDgramRecv *r, void (*cb)(void *ctx, int delta), void *ctx) {
    if (!r) return;
    r->on_activity = cb; r->activity_ctx = ctx;
}
static void recv_enter(KlDgramRecv *r) { if (r->on_activity) r->on_activity(r->activity_ctx, +1); }
/* recv_leave is the LAST action of a public op — it may detach + free the object via the coordinator. */
static void recv_leave(KlDgramRecv *r) { if (r->on_activity) r->on_activity(r->activity_ctx, -1); }

int kl_dgram_recv_init(KlDgramRecv *r, KlDgramSlots *slots, int completion,
                       KlDgramRecvDeliverFn deliver, void *deliver_ctx,
                       KlDgramRecvArmFn arm, KlDgramRecvDisarmFn disarm,
                       KlDgramRecvPullFn pull, void *hook_ctx) {
    if (!r)
        return -1;
    memset(r, 0, sizeof(*r));
    if (!slots || slots->in_cap == 0 || !deliver || !arm)
        return -1;
    if (!completion && (!disarm || !pull))   /* readiness pause MUST drop interest + pull to drain */
        return -1;
    r->slots       = slots;
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
 * (completion mode) cannot grow the C stack — a re-arm requested during a synchronous delivery sets
 * rearm_pending and returns; this loop performs it. recv_inflight is set BEFORE the hook so a
 * synchronous on_complete sees it. Precondition: the caller decided arming is due. */
static int recv_arm(KlDgramRecv *r) {
    if (r->arming) { r->rearm_pending = 1; return 0; }   /* defer nested re-arm to the trampoline */

    int result = 0;
    for (;;) {
        if (r->stopped || r->paused || r->recv_inflight || r->held) break;
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
        if (rc < 0) {                        /* genuine arm failure — no sync completion */
            if (r->recv_inflight) r->recv_inflight = 0;
            r->stopped = 1;
            result = -1;
            break;
        }
        break;                               /* posted async — await the completion */
    }
    return result;
}

/* Deliver one VALID datagram from the inbound slot, then re-arm — unless the callback paused/stopped.
 * A failed receive NEVER reaches here (it is routed through recv_fail). State re-checked AFTER the
 * callback. */
static int recv_deliver_and_continue(KlDgramRecv *r, size_t len) {
    KlDgramSlot *in = kl_dgram_slots_inbound(r->slots);
    r->deliver(r->deliver_ctx, in->data, len,
               addr_or_null(&in->peer), addr_or_null(&in->local), in->flags);
    if (r->stopped || r->paused) return 0;   /* callback stopped/paused — do not re-arm */
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
    if (r->paused || r->stopped || r->recv_inflight || r->held)
        return 0;
    recv_enter(r);
    int ret = recv_arm(r);
    recv_leave(r);                           /* LAST action — an inline completion may detach */
    return ret;
}

int kl_dgram_recv_on_complete(KlDgramRecv *r, size_t len, int ok) {
    if (!r || !r->inited)
        return -1;
    if (!r->recv_inflight)
        return 0;                            /* duplicate/spurious — drop; do NOT disturb held data */
    recv_enter(r);
    r->recv_inflight = 0;                    /* this receive op has retired */
    if (r->arming)
        r->completed_inline = 1;             /* synchronous completion of the current arm */

    int ret;
    if (r->stopped)                          /* a stopped/cancelled op retired — drop the data */
        ret = 0;
    else if (!ok)                            /* a FAILED receive is not a datagram — never deliver */
        { recv_fail(r); ret = -1; }
    else if (len > r->slots->in_cap)         /* provider contract violation — fail safely */
        { recv_fail(r); ret = -1; }
    else if (r->paused)                      /* STRICT: hold the valid datagram, do not deliver */
        { r->held = 1; r->held_len = len; ret = 0; }
    else
        ret = recv_deliver_and_continue(r, len);
    recv_leave(r);                           /* LAST action — coordinator finalize may detach (frees r) */
    return ret;
}

int kl_dgram_recv_on_readable(KlDgramRecv *r) {
    if (!r || !r->inited || r->completion)
        return -1;                           /* readiness only */
    if (r->stopped || r->paused || !r->recv_inflight)
        return 0;
    recv_enter(r);
    int ret = 0;
    /* Serial provider receives: one pull → one delivery, re-checking paused/stopped each iteration. */
    while (!r->paused && !r->stopped && r->recv_inflight) {
        size_t len = 0;
        int pr = r->pull(r->hook_ctx, &len);
        if (pr == 0)
            break;                           /* would-block — drained */
        if (pr < 0)                          /* fatal receive error — disarm + clear (recv_fail) */
            { recv_fail(r); ret = -1; break; }
        if (len > r->slots->in_cap)          /* provider contract violation — fail safely */
            { recv_fail(r); ret = -1; break; }
        KlDgramSlot *in = kl_dgram_slots_inbound(r->slots);
        r->deliver(r->deliver_ctx, in->data, len,
                   addr_or_null(&in->peer), addr_or_null(&in->local), in->flags);
        /* loop re-checks paused/stopped (the delivery may have set them) */
    }
    recv_leave(r);                           /* LAST action — coordinator finalize may detach (frees r) */
    return ret;
}

void kl_dgram_recv_pause(KlDgramRecv *r) {
    if (!r || !r->inited || r->paused)
        return;                              /* idempotent */
    recv_enter(r);
    r->paused = 1;
    if (!r->completion && r->recv_inflight) {
        /* Readiness: drop READ interest (only if armed); a pending readable is cancelled — a
         * SYNCHRONOUS physical retirement. */
        r->disarm(r->hook_ctx);
        r->recv_inflight = 0;
    }
    /* Completion: leave a posted recv physically in flight — held on completion. */
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
    else if (r->held) {
        size_t len = r->held_len;
        r->held = 0; r->held_len = 0;        /* consume the valid held datagram exactly once */
        ret = recv_deliver_and_continue(r, len);
    } else if (r->recv_inflight)             /* completion: a recv posted before pause is still out */
        ret = 0;
    else
        ret = recv_arm(r);
    recv_leave(r);                           /* LAST action — a delivery/inline completion may detach */
    return ret;
}

void kl_dgram_recv_stop(KlDgramRecv *r) {
    if (!r || !r->inited || r->stopped)
        return;                              /* idempotent */
    recv_enter(r);
    r->stopped  = 1;
    r->held     = 0;                         /* discard held completion — not delivered */
    r->held_len = 0;
    if (!r->completion) {
        if (r->recv_inflight) r->disarm(r->hook_ctx);   /* SYNCHRONOUS physical retirement */
        r->recv_inflight = 0;
    }
    /* Completion: a posted recv may still be PHYSICALLY outstanding; it retires (dropped) on its
     * completion. The logical/physical split is what step-4 detachment builds on. */
    recv_leave(r);
}

int kl_dgram_recv_free(KlDgramRecv *r) {
    if (!r)
        return 0;
    if (r->recv_inflight)
        return -1;   /* a receive references the inbound slot — must not free underneath it */
    memset(r, 0, sizeof(*r));   /* reusable (borrows no heap; the slots are the caller's) */
    return 0;
}
