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

/* Deliver one datagram from the inbound slot, then re-arm — unless it was a fatal receive (ok==0)
 * or the callback paused/stopped. State re-checked AFTER the callback. */
static int recv_deliver_and_continue(KlDgramRecv *r, size_t len, int ok) {
    if (!ok) { r->stopped = 1; len = 0; }
    KlDgramSlot *in = kl_dgram_slots_inbound(r->slots);
    r->deliver(r->deliver_ctx, in->data, len,
               addr_or_null(&in->peer), addr_or_null(&in->local), in->flags);
    if (!ok) return 0;                       /* fatal delivered — never re-arm */
    if (r->stopped || r->paused) return 0;   /* callback stopped/paused — do not re-arm */
    if (r->recv_inflight) return 0;          /* a synchronous completion already re-armed */
    return recv_arm(r);
}

int kl_dgram_recv_start(KlDgramRecv *r) {
    if (!r || !r->inited)
        return -1;
    if (r->paused || r->stopped || r->recv_inflight || r->held)
        return 0;
    return recv_arm(r);
}

int kl_dgram_recv_on_complete(KlDgramRecv *r, size_t len, int ok) {
    if (!r || !r->inited)
        return -1;
    if (!r->recv_inflight)
        return 0;                            /* duplicate/spurious — drop; do NOT disturb held data */
    r->recv_inflight = 0;                    /* this receive op has retired */
    if (r->arming)
        r->completed_inline = 1;             /* synchronous completion of the current arm */
    if (r->stopped)
        return 0;                            /* retirement of a stopped op — drop */

    if (ok && len > r->slots->in_cap) {      /* provider contract violation — fail safely */
        r->stopped = 1;
        return -1;
    }
    if (!ok) len = 0;

    if (r->paused) {                         /* STRICT: hold in the inbound slot, do not deliver */
        r->held     = 1;
        r->held_len = len;
        r->held_ok  = ok ? 1 : 0;
        return 0;
    }
    return recv_deliver_and_continue(r, len, ok ? 1 : 0);
}

int kl_dgram_recv_on_readable(KlDgramRecv *r) {
    if (!r || !r->inited || r->completion)
        return -1;                           /* readiness only */
    if (r->stopped || r->paused || !r->recv_inflight)
        return 0;
    /* Serial provider receives: one pull → one delivery, re-checking paused/stopped each iteration. */
    while (!r->paused && !r->stopped && r->recv_inflight) {
        size_t len = 0;
        int pr = r->pull(r->hook_ctx, &len);
        if (pr == 0)
            break;                           /* would-block — drained */
        if (pr < 0) {                        /* fatal receive error */
            r->stopped = 1;
            return -1;
        }
        if (len > r->slots->in_cap) {        /* provider contract violation — fail safely */
            r->stopped = 1;
            return -1;
        }
        KlDgramSlot *in = kl_dgram_slots_inbound(r->slots);
        r->deliver(r->deliver_ctx, in->data, len,
                   addr_or_null(&in->peer), addr_or_null(&in->local), in->flags);
        /* loop re-checks paused/stopped (the delivery may have set them) */
    }
    return 0;
}

void kl_dgram_recv_pause(KlDgramRecv *r) {
    if (!r || !r->inited || r->paused)
        return;                              /* idempotent */
    r->paused = 1;
    if (!r->completion && r->recv_inflight) {
        /* Readiness: drop READ interest (only if armed); a pending readable is cancelled. */
        r->disarm(r->hook_ctx);
        r->recv_inflight = 0;
    }
    /* Completion: leave a posted recv physically in flight — held on completion. */
}

int kl_dgram_recv_resume(KlDgramRecv *r) {
    if (!r || !r->inited || !r->paused)
        return 0;                            /* idempotent */
    r->paused = 0;
    if (r->stopped)
        return 0;

    if (r->held) {
        size_t len = r->held_len;
        int ok = r->held_ok;
        r->held = 0; r->held_len = 0; r->held_ok = 0;   /* consume exactly once */
        return recv_deliver_and_continue(r, len, ok);
    }
    if (r->recv_inflight)                     /* completion: a recv posted before pause is still out */
        return 0;
    return recv_arm(r);
}

void kl_dgram_recv_stop(KlDgramRecv *r) {
    if (!r || !r->inited || r->stopped)
        return;                              /* idempotent */
    r->stopped  = 1;
    r->held     = 0;                         /* discard held completion — not delivered */
    r->held_len = 0;
    r->held_ok  = 0;
    if (!r->completion) {
        if (r->recv_inflight) r->disarm(r->hook_ctx);
        r->recv_inflight = 0;
    }
    /* Completion: a posted recv may still be PHYSICALLY outstanding; it retires (dropped) on its
     * completion. The logical/physical split is what step-4 detachment builds on. */
}

int kl_dgram_recv_free(KlDgramRecv *r) {
    if (!r)
        return 0;
    if (r->recv_inflight)
        return -1;   /* a receive references the inbound slot — must not free underneath it */
    memset(r, 0, sizeof(*r));   /* reusable (borrows no heap; the slots are the caller's) */
    return 0;
}
