/*
 * listener.c — Phase-B accept-side listener state machine (step 5). See listener.h.
 *
 * Reserve-before-accept backpressure, sync-completion-safe arming (iterative trampoline), guarded
 * reentrant callbacks, cancel-once, total accepted-fd disposal, and confirmed detachment — the
 * same discipline as KlConnectOp, applied to the inbound side. The pool-owned slot-release
 * capability is baked by value into each KlSlotLease so a committed slot outlives the listener.
 */
#include "listener.h"
#include <string.h>

void kl_slot_lease_release(KlSlotLease *lease) {
    if (!lease || !lease->release) return;
    /* CONSUME the lease before invoking the callback: a repeated release on the (owned) lease is a
     * harmless no-op, and the callback cannot re-enter and double-release through this same lease. */
    KlSlotReleaseFn fn  = lease->release;
    void           *ctx = lease->release_ctx;
    const int      *alive = lease->alive;
    lease->release     = NULL;
    lease->release_ctx = NULL;
    lease->alive       = NULL;
    if (alive && !*alive) return;                 /* pool gone (liveness token is stable) — no-op */
    fn(ctx);
}

/* ── internal helpers ──────────────────────────────────────────────────────────────────────── */

/* Reserve/release are POOL callbacks that MAY reentrantly close the listener — run them under the
 * in_dispatch depth guard so a nested kl_listener_close() defers detachment until the outer frame
 * unwinds (never detach-and-free under our feet). */
static int l_reserve(KlListener *l) {
    if (!l->reserve) return 1;
    l->in_dispatch++;
    int r = l->reserve(l->credit_ctx);
    l->in_dispatch--;
    return r;
}
static void l_release_credit(KlListener *l) {   /* return one raw credit to the pool (guarded) */
    if (l->release) {
        l->in_dispatch++;
        l->release(l->credit_ctx);
        l->in_dispatch--;
    }
}
static void l_release_reserved(KlListener *l) {
    if (l->reserved) {
        l->reserved = 0;                          /* commit state BEFORE the reentrant pool callback */
        l_release_credit(l);
    }
}

/* Detach if closing and everything is retired — deferred while an arm hook or a reentrant callback
 * is on the stack (in_start / in_dispatch). Exactly-once via `detached`. */
static void l_finalize(KlListener *l) {
    if (l->state != KL_LISTENER_CLOSING) return;
    if (l->in_start || l->in_dispatch) return;   /* defer past a start / callback frame */
    if (l->accept_inflight) return;              /* a posted accept is still outstanding */
    if (l->reserved) return;                     /* held reservation not yet released */
    if (l->detached) return;
    l->state    = KL_LISTENER_CLOSED;
    l->detached = 1;
    if (l->on_close) l->on_close(l->ctx);        /* reuse/free legal only after this returns */
}

/* Guarded destructive tail for disposing an accepted fd that cannot become a connection: dispose_fd
 * is an adapter callback that MAY reentrantly close the listener, so it runs under in_dispatch and
 * finalization happens after it returns. Callers must return immediately afterward. */
static void l_dispose(KlListener *l, KlSocketHandle fd) {
    l->in_dispatch++;
    l->dispose_fd(l->ctx, fd);
    l->in_dispatch--;
    l_finalize(l);
}

/* Begin close (public or fatal-internal). Stops accepting, releases the held reservation, and
 * finalizes; detachment waits for a completion-mode posted accept to retire. */
static void l_begin_close(KlListener *l) {
    if (l->state == KL_LISTENER_CLOSED) return;
    if (l->state != KL_LISTENER_CLOSING) {
        l->state = KL_LISTENER_CLOSING;
        if (l->accept_inflight) {
            if (!l->completion_mode) {
                if (l->disarm_accept) l->disarm_accept(l->ctx);
                l->accept_inflight = 0;              /* readiness: interest dropped, nothing completes */
            } else if (l->cancel_accept && !l->accept_cancel_requested) {
                l->accept_cancel_requested = 1;
                l->in_dispatch++;                    /* cancel may inline → on_accept_failed */
                l->cancel_accept(l->ctx);
                l->in_dispatch--;
            }
            /* completion with no cancel hook: the posted accept still completes → disposed there */
        }
        l_release_reserved(l);                       /* the uncommitted reserved slot returns */
    }
    l_finalize(l);
}

/* Arm the next accept, reserving a slot first (backpressure). ITERATIVE trampoline: an arm hook
 * that completes synchronously (on_accepted/on_accept_failed inline) may request another arm; that
 * is deferred via rearm_pending and performed by this loop instead of recursing — bounding the C
 * stack across a run of synchronous accepts or accept failures. */
static void l_arm_loop(KlListener *l) {
    if (l->arming) { l->rearm_pending = 1; return; }   /* defer a nested re-arm to the trampoline */

    for (;;) {
        if (l->state != KL_LISTENER_LISTENING || l->accept_inflight) break;

        if (!l->reserved) {                          /* take a slot credit before accepting */
            int r = l_reserve(l);                    /* may reentrantly close the listener */
            if (r < 0) { l->last_error = -1; l_begin_close(l); break; }   /* pool error → close */
            if (l->state != KL_LISTENER_LISTENING) { /* the reserve hook reentrantly closed us */
                if (r == 1) l_release_credit(l);     /* return the credit we just acquired */
                break;
            }
            if (r == 0) {                            /* backpressure: no credit */
                l->state = KL_LISTENER_PAUSED;
                /* Readiness: drop the persistent listen READ interest so a level-triggered fd stops
                 * firing until a slot frees and kl_listener_notify_slot_free re-arms it. Completion
                 * disarm is a documented no-op. The disarm hook is idempotent (it tracks its own
                 * registration), so calling it here even if nothing was armed is safe. */
                if (!l->completion_mode && l->disarm_accept)
                    l->disarm_accept(l->ctx);
                break;
            }
            l->reserved = 1;
        }

        l->accept_inflight         = 1;
        l->accept_cancel_requested = 0;
        l->accepted_inline         = 0;
        l->rearm_pending           = 0;
        l->arming                  = 1;
        l->in_start++;                    /* defer detachment: the hook may reentrantly close us */
        int rc = l->arm_accept(l->ctx);   /* may sync-complete via on_accepted/on_accept_failed,
                                             or directly call kl_listener_close() */
        l->in_start--;
        l->arming = 0;
        if (l->accepted_inline) {         /* inline-decided; loop only if a re-arm was requested */
            if (l->rearm_pending) continue;
            break;
        }
        if (rc < 0) {                     /* hard arm failure (listen fd broken) → close */
            l->accept_inflight = 0;
            l->last_error = rc;
            l_begin_close(l);
            break;
        }
        break;                            /* armed async — await the completion */
    }
    l_finalize(l);   /* the arm frame has unwound — honor a reentrant close from the arm hook */
}

/* ── public API ────────────────────────────────────────────────────────────────────────────── */

int kl_listener_init(KlListener *l, int completion_mode, const KlListenerHooks *hooks, void *ctx) {
    if (!l || !hooks) return -1;
    if (!hooks->arm_accept || !hooks->on_accept || !hooks->dispose_fd) return -1;
    if (!completion_mode && !hooks->disarm_accept) return -1;   /* readiness must drop interest */
    if (!!hooks->reserve != !!hooks->release) return -1;        /* reserve/release are paired */
    memset(l, 0, sizeof(*l));
    l->completion_mode = completion_mode ? 1 : 0;
    l->reserve         = hooks->reserve;
    l->release         = hooks->release;
    l->credit_ctx      = hooks->credit_ctx;
    l->liveness        = hooks->liveness;
    l->arm_accept      = hooks->arm_accept;
    l->disarm_accept   = hooks->disarm_accept;
    l->cancel_accept   = hooks->cancel_accept;
    l->on_accept       = hooks->on_accept;
    l->dispose_fd      = hooks->dispose_fd;
    l->on_close        = hooks->on_close;
    l->ctx             = ctx;
    l->state           = KL_LISTENER_IDLE;
    l->inited          = 1;
    return 0;
}

int kl_listener_start(KlListener *l) {
    if (!l || !l->inited) return -1;
    if (l->state != KL_LISTENER_IDLE) return -1;
    l->state = KL_LISTENER_LISTENING;
    l_arm_loop(l);
    return 0;
}

void kl_listener_on_accepted(KlListener *l, KlSocketHandle fd) {
    if (!l || !l->inited) return;
    if (!l->accept_inflight) { l_dispose(l, fd); return; }   /* spurious accept — dispose its fd */
    l->accept_inflight = 0;
    if (l->arming) l->accepted_inline = 1;

    if (l->state == KL_LISTENER_CLOSING || l->state == KL_LISTENER_CLOSED) {
        /* teardown: cannot hand off — dispose the fd and return the reserved slot */
        l_release_reserved(l);
        l_dispose(l, fd);          /* guarded tail: finalizes after dispose returns */
        return;
    }

    /* commit the reserved slot to this connection and hand it off as a lease built from the
     * POOL-OWNED release capability (by value) + the nullable liveness token — no listener ref */
    l->reserved = 0;
    KlSlotLease lease = { l->release, l->credit_ctx, l->liveness };
    l->in_dispatch++;
    l->on_accept(l->ctx, fd, lease);    /* by value: ownership transfers to the accepted stream */
    l->in_dispatch--;

    l_arm_loop(l);     /* reserve + arm the next accept */
    l_finalize(l);     /* in case on_accept closed the listener */
}

void kl_listener_on_accept_failed(KlListener *l, int error) {
    if (!l || !l->inited) return;
    if (!l->accept_inflight) return;          /* duplicate/spurious — drop */
    l->accept_inflight = 0;
    if (l->arming) l->accepted_inline = 1;
    l->last_error = error;
    l_release_reserved(l);                     /* the slot for the failed accept returns */

    if (l->state == KL_LISTENER_CLOSING || l->state == KL_LISTENER_CLOSED) {
        l_finalize(l);
        return;
    }
    l_arm_loop(l);     /* transient failure — reserve + re-arm */
    l_finalize(l);
}

void kl_listener_notify_slot_free(KlListener *l) {
    if (!l || !l->inited) return;
    if (l->state != KL_LISTENER_PAUSED) return;
    l->state = KL_LISTENER_LISTENING;
    l_arm_loop(l);     /* a credit is available now — reserve + arm */
}

int kl_listener_close(KlListener *l) {
    if (!l || !l->inited) return -1;
    l_begin_close(l);
    return 0;
}

int kl_listener_state(const KlListener *l) {
    return l ? l->state : KL_LISTENER_CLOSED;
}

int kl_listener_is_detached(const KlListener *l) {
    return (l && l->detached) ? 1 : 0;
}
