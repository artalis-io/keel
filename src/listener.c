/*
 * listener.c: the accept-side listener state machine. See listener.h.
 *
 * Reserve-before-accept backpressure, sync-completion-safe posting (iterative pump trampoline),
 * guarded reentrant callbacks, cancel-once, total accepted-fd disposal, and confirmed detachment.
 * The accepted value is a PASS-THROUGH: an fd (the socket handoff family) or an adapter-materialized
 * connection object (the object family, for a transport that is not a socket). Nothing here
 * interprets either; both ride one retire/commit/dispose/refill path.
 * The listener keeps up to `window` accepts posted concurrently, ONE reserved pool credit per
 * posted accept (window=1 for readiness / io_uring / pollcomp; KL_IOCP_ACCEPT_BACKLOG for IOCP so
 * its multi-deep AcceptEx concurrency is preserved). Each completion consumes exactly one
 * reservation into one lease (success) or returns it (failure/cancel); the pool-owned release
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
    if (alive && !*alive) return;                 /* pool gone (liveness token is stable): no-op */
    fn(ctx);
}

/* ── internal helpers ──────────────────────────────────────────────────────────────────────── */

/* Reserve/release are POOL callbacks that MAY reentrantly close the listener; run them under the
 * in_dispatch depth guard so a nested kl_listener_close() defers detachment until the outer frame
 * unwinds. */
static int l_reserve(KlListener *l) {
    if (!l->reserve) return 1;                     /* unbounded (no slot accounting) */
    l->in_dispatch++;
    int r = l->reserve(l->credit_ctx);
    l->in_dispatch--;
    return r;
}
static void l_release_credit(KlListener *l) {      /* return one raw credit to the pool (guarded) */
    if (l->release) {
        l->in_dispatch++;
        l->release(l->credit_ctx);
        l->in_dispatch--;
    }
}

/* Detach if closing and everything is retired, deferred while a start/arm hook or a reentrant
 * callback is on the stack (in_start / in_dispatch). Exactly-once via `detached`. Every posted
 * accept holds a reservation, so `inflight == 0` also means no credit is held uncommitted. */
static void l_finalize(KlListener *l) {
    if (l->state != KL_LISTENER_STATE_CLOSING) return;
    if (l->in_start || l->in_dispatch) return;   /* defer past a start / callback frame */
    if (l->inflight) return;                      /* posted accepts still outstanding */
    if (l->detached) return;
    l->state    = KL_LISTENER_STATE_CLOSED;
    l->detached = 1;
    if (l->on_close) l->on_close(l->ctx);        /* reuse/free legal only after this returns */
}

/* One accepted value, in whichever handoff family this listener was initialised with. Internal:
 * callers only ever see their own family's type. */
typedef struct {
    int            is_obj;
    KlSocketHandle fd;     /* fd family */
    void          *conn;   /* object family: the adapter's connection object */
} LAccepted;

/* Guarded destructive tail for disposing an accepted value that cannot become a connection: the
 * dispose hook is an adapter callback that MAY reentrantly close the listener, so it runs under
 * in_dispatch and finalization happens after it returns. Callers must return immediately afterward. */
static void l_dispose(KlListener *l, LAccepted a) {
    l->in_dispatch++;
    if (a.is_obj) l->dispose_obj(l->ctx, a.conn);
    else          l->dispose_fd(l->ctx, a.fd);
    l->in_dispatch--;
    l_finalize(l);
}

/* Begin close (public or fatal-internal). Stops accepting and retires the reservations held by the
 * posted accepts; detachment waits until every posted accept retires. */
static void l_begin_close(KlListener *l) {
    if (l->state == KL_LISTENER_STATE_CLOSED) return;
    if (l->state != KL_LISTENER_STATE_CLOSING) {
        l->state = KL_LISTENER_STATE_CLOSING;
        if (l->inflight > 0) {
            if (!l->completion_mode) {
                /* Readiness: drop the listen interest; the pending interest is cancelled, so nothing
                 * completes; return every posted accept's credit now. */
                if (l->disarm_accept) l->disarm_accept(l->ctx);
                while (l->inflight > 0) { l->inflight--; l_release_credit(l); }
            } else if (l->cancel_accept && !l->accept_cancel_requested) {
                /* Completion: request a batch cancel once; each posted accept completes as failed
                 * (kl_listener_on_accept_failed) and returns its credit + decrements inflight. */
                l->accept_cancel_requested = 1;
                l->in_dispatch++;                 /* cancel may inline → on_accept_failed */
                l->cancel_accept(l->ctx);
                l->in_dispatch--;
            }
            /* completion with no cancel hook: the posted accepts still complete → retired there */
        }
    }
    l_finalize(l);
}

/* Post accepts up to the window, reserving ONE credit per posted accept (backpressure). ITERATIVE
 * pump trampoline: an arm hook that completes synchronously (on_accepted/on_accept_failed inline)
 * may request another pump; that is deferred via pump_pending and performed by this loop instead of
 * recursing, bounding the C stack across a run of synchronous accepts/failures. */
static void l_pump(KlListener *l) {
    if (l->pumping) { l->pump_pending = 1; return; }   /* defer a nested pump to the trampoline */

    do {
        l->pump_pending = 0;
        while (l->state == KL_LISTENER_STATE_LISTENING && l->inflight < l->window) {
            int r = l_reserve(l);                        /* may reentrantly close the listener */
            if (r < 0) { l->last_error = -1; l_begin_close(l); break; }   /* pool error → close */
            if (l->state != KL_LISTENER_STATE_LISTENING) {     /* the reserve hook reentrantly closed us */
                if (r == 1) l_release_credit(l);         /* return the credit we just acquired */
                break;
            }
            if (r == 0) {                                /* backpressure: no credit */
                if (l->inflight == 0) {
                    l->state = KL_LISTENER_STATE_PAUSED;
                    /* Readiness: drop the persistent listen READ interest so a level-triggered fd
                     * stops firing until a slot frees and notify_slot_free re-arms it. Completion
                     * disarm is a documented no-op; the disarm hook is idempotent. */
                    if (!l->completion_mode && l->disarm_accept)
                        l->disarm_accept(l->ctx);
                }
                break;   /* below window but out of credit; hold what is posted */
            }

            /* Credit held; post one accept. Mark it inflight BEFORE the hook so a synchronous
             * on_accepted/on_accept_failed (which decrements inflight) is seen. */
            int before = l->inflight;
            l->inflight = before + 1;
            l->pumping  = 1;
            l->in_start++;
            int rc = l->arm_accept(l->ctx);   /* may sync-complete, or reentrantly close */
            l->in_start--;
            l->pumping  = 0;

            if (l->inflight == before)        /* sync-completed this post: try to post more */
                continue;
            if (rc < 0) {                     /* post failure: never became a real accept */
                l->inflight = before;
                l_release_credit(l);          /* return this post's credit */
                l->last_error = rc;
                if (rc == KL_LISTENER_ARM_RETRY) {
                    /* Transient: hold what is posted. With nothing posted, pause until told a slot
                     * is free (the adapter's next chance to post), exactly as for no credit. */
                    if (l->inflight == 0 && l->state == KL_LISTENER_STATE_LISTENING) {
                        l->state = KL_LISTENER_STATE_PAUSED;
                        if (!l->completion_mode && l->disarm_accept)
                            l->disarm_accept(l->ctx);
                    }
                    break;
                }
                l_begin_close(l);             /* listen fd broken → close */
                break;
            }
            /* posted async (inflight == before + 1): loop to fill the rest of the window */
        }
    } while (l->pump_pending);

    l_finalize(l);   /* the pump frame has unwound; honor a reentrant close from an arm hook */
}

/* ── public API ────────────────────────────────────────────────────────────────────────────── */

int kl_listener_init(KlListener *l, int completion_mode, const KlListenerHooks *hooks, void *ctx) {
    if (!l || !hooks) return -1;
    if (!hooks->arm_accept) return -1;
    /* Exactly one COMPLETE handoff family: fd (on_accept + dispose_fd) or object (on_accept_obj +
     * dispose_obj). Neither, both, or a half-configured family is rejected. */
    int fd_family  = hooks->on_accept || hooks->dispose_fd;
    int obj_family = hooks->on_accept_obj || hooks->dispose_obj;
    if (fd_family == obj_family) return -1;
    if (fd_family  && (!hooks->on_accept || !hooks->dispose_fd)) return -1;
    if (obj_family && (!hooks->on_accept_obj || !hooks->dispose_obj)) return -1;
    if (!completion_mode && !hooks->disarm_accept) return -1;   /* readiness must drop interest */
    if (!!hooks->reserve != !!hooks->release) return -1;        /* reserve/release are paired */
    memset(l, 0, sizeof(*l));
    l->completion_mode = completion_mode ? 1 : 0;
    l->window          = 1;                 /* default: one accept in flight (readiness/io_uring/…) */
    l->reserve         = hooks->reserve;
    l->release         = hooks->release;
    l->credit_ctx      = hooks->credit_ctx;
    l->liveness        = hooks->liveness;
    l->arm_accept      = hooks->arm_accept;
    l->disarm_accept   = hooks->disarm_accept;
    l->cancel_accept   = hooks->cancel_accept;
    l->on_accept       = hooks->on_accept;
    l->dispose_fd      = hooks->dispose_fd;
    l->on_accept_obj   = hooks->on_accept_obj;
    l->dispose_obj     = hooks->dispose_obj;
    l->on_close        = hooks->on_close;
    l->ctx             = ctx;
    l->state           = KL_LISTENER_STATE_IDLE;
    l->inited          = 1;
    return 0;
}

int kl_listener_set_accept_window(KlListener *l, int window) {
    if (!l || !l->inited || l->state != KL_LISTENER_STATE_IDLE) return -1;   /* init-time only */
    if (window < 1) return -1;
    /* A readiness arm is a single persistent interest registration, not N distinct posted
     * operations; a window > 1 would reserve several credits against one registration. Only a
     * completion backend (which posts N discrete accept ops) may widen the window. */
    if (!l->completion_mode && window != 1) return -1;
    l->window = window;
    return 0;
}

int kl_listener_start(KlListener *l) {
    if (!l || !l->inited) return -1;
    if (l->state != KL_LISTENER_STATE_IDLE) return -1;
    l->state = KL_LISTENER_STATE_LISTENING;
    l_pump(l);
    return 0;
}

/* The shared accept-completion body for both handoff families. */
static void l_on_accepted(KlListener *l, LAccepted a) {
    if (l->inflight <= 0) { l_dispose(l, a); return; }    /* spurious accept: dispose it */
    l->inflight--;                                        /* this posted accept retired */

    if (l->state == KL_LISTENER_STATE_CLOSING || l->state == KL_LISTENER_STATE_CLOSED) {
        /* teardown: cannot hand off; return this accept's credit and dispose the value */
        l_release_credit(l);
        l_dispose(l, a);           /* guarded tail: finalizes after dispose returns */
        return;
    }

    /* Commit this accept's reserved credit to the connection, handed off as a lease built from the
     * POOL-OWNED release capability (by value) + the nullable liveness token, no listener ref. */
    KlSlotLease lease = { l->release, l->credit_ctx, l->liveness };
    l->in_dispatch++;
    if (a.is_obj) l->on_accept_obj(l->ctx, a.conn, lease);   /* by value: ownership transfers */
    else          l->on_accept(l->ctx, a.fd, lease);
    l->in_dispatch--;

    l_pump(l);         /* top up to the window (also finalizes if on_accept closed us) */
}

void kl_listener_on_accepted(KlListener *l, KlSocketHandle fd) {
    if (!l || !l->inited) return;
    if (l->on_accept_obj) {                  /* object-family listener: refuse, retire as failed */
        kl_listener_on_accept_failed(l, -1);
        return;
    }
    LAccepted a = { 0, fd, NULL };
    l_on_accepted(l, a);
}

void kl_listener_on_accepted_obj(KlListener *l, void *conn) {
    if (!l || !l->inited) return;
    if (!l->on_accept_obj) {                 /* fd-family listener: refuse, retire as failed */
        kl_listener_on_accept_failed(l, -1);
        return;
    }
    LAccepted a = { 1, KL_INVALID_SOCKET, conn };
    l_on_accepted(l, a);
}

void kl_listener_on_accept_failed(KlListener *l, int error) {
    if (!l || !l->inited) return;
    if (l->inflight <= 0) return;             /* duplicate/spurious: drop */
    l->inflight--;                            /* this posted accept retired */
    l->last_error = error;
    l_release_credit(l);                       /* return this accept's credit */

    if (l->state == KL_LISTENER_STATE_CLOSING || l->state == KL_LISTENER_STATE_CLOSED) {
        l_finalize(l);
        return;
    }
    l_pump(l);         /* transient failure: top the window back up */
}

void kl_listener_notify_slot_free(KlListener *l) {
    if (!l || !l->inited) return;
    if (l->state == KL_LISTENER_STATE_PAUSED) l->state = KL_LISTENER_STATE_LISTENING;
    if (l->state != KL_LISTENER_STATE_LISTENING) return;
    l_pump(l);         /* a credit is available now: top up (re-arms readiness interest if paused) */
}

int kl_listener_close(KlListener *l) {
    if (!l || !l->inited) return -1;
    l_begin_close(l);
    return 0;
}

KlListenerState kl_listener_state(const KlListener *l) {
    return l ? (KlListenerState)l->state : KL_LISTENER_STATE_CLOSED;
}

int kl_listener_is_detached(const KlListener *l) {
    return (l && l->detached) ? 1 : 0;
}
