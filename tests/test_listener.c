/*
 * test_listener.c — Phase-B step 5: accept-side listener state machine (src/listener.h),
 * exercised in isolation with mock credit/arm/accept/dispose hooks (no live sockets).
 *
 * Covers the four distinct lifetimes: listener lifetime (start/close/detach), pool-owned
 * slot-release capability (reserve/release accounting), nullable credit/liveness target, and
 * accepted-stream lifetime (a lease released after the listener is gone). Plus the carried-over
 * discipline: reserve-before-accept backpressure, sync-completion-safe arming (bounded), guarded
 * reentrant callbacks, cancel-once, total accepted-fd disposal, confirmed detachment.
 */
#include "utest.h"
#include "../src/listener.h"
#include <string.h>

typedef struct {
    KlListener *l;
    int slots;                 /* available pool credits */
    int reserve_calls, release_calls;
    int reserved_now;          /* net reserved (reserve - release) */
    int arm_calls, disarm_calls, cancel_calls;
    int accept_calls, last_fd; KlSlotLease last_lease;   /* owned copy of the last handed-off lease */
    int arm_reentrant_close;   /* arm hook directly calls kl_listener_close (no completion) */
    int arm_accept_then_close; /* arm hook inline-accepts, then calls kl_listener_close */
    int dispose_calls, disposed_fd;
    int close_calls;
    int alive;                 /* liveness flag (1 = pool alive) */
    int reserve_error;         /* reserve returns -1 (pool error) */
    /* programmed arm behavior */
    int hardfail;              /* arm returns -1 */
    int sync_accept_budget;    /* arm inline-accepts this many times, then goes async */
    int sync_fail_budget;      /* arm inline-fails this many times, then goes async */
    int arm_fd_base, arm_fd_next, arm_err;
    /* reentrancy */
    int accept_reentrant_close, dispose_reentrant_close;
    int reserve_reentrant_close, release_reentrant_close;
    int detached_at_accept, detached_at_dispose, detached_at_reserve, detached_at_release;
} LT;

static int lt_reserve(void *ctx) {
    LT *m = ctx; m->reserve_calls++;
    if (m->reserve_error) return -1;
    if (m->slots <= 0) return 0;
    m->slots--; m->reserved_now++;
    m->detached_at_reserve = kl_listener_is_detached(m->l);
    if (m->reserve_reentrant_close) { m->reserve_reentrant_close = 0; kl_listener_close(m->l); }
    return 1;
}
static void lt_release(void *ctx) {
    LT *m = ctx; m->release_calls++; m->slots++; m->reserved_now--;
    m->detached_at_release = kl_listener_is_detached(m->l);
    if (m->release_reentrant_close) { m->release_reentrant_close = 0; kl_listener_close(m->l); }
}
static int lt_arm(void *ctx) {
    LT *m = ctx; m->arm_calls++;
    if (m->arm_accept_then_close) {            /* inline-accept AND then reentrantly close */
        m->arm_accept_then_close = 0;
        kl_listener_on_accepted(m->l, (KlSocketHandle)(m->arm_fd_base + m->arm_fd_next++));
        kl_listener_close(m->l);
        return 0;
    }
    if (m->arm_reentrant_close) { m->arm_reentrant_close = 0; kl_listener_close(m->l); return 0; }
    if (m->hardfail) return -1;
    if (m->sync_accept_budget > 0) {
        m->sync_accept_budget--;
        kl_listener_on_accepted(m->l, (KlSocketHandle)(m->arm_fd_base + m->arm_fd_next++));
        return 0;
    }
    if (m->sync_fail_budget > 0) {
        m->sync_fail_budget--;
        kl_listener_on_accept_failed(m->l, m->arm_err);
        return 0;
    }
    return 0;   /* async — awaiting an external completion */
}
static void lt_disarm(void *ctx) { LT *m = ctx; m->disarm_calls++; }
static void lt_cancel(void *ctx) { LT *m = ctx; m->cancel_calls++; }
static void lt_on_accept(void *ctx, KlSocketHandle fd, KlSlotLease lease) {
    LT *m = ctx; m->accept_calls++; m->last_fd = (int)fd; m->last_lease = lease;  /* take ownership */
    m->detached_at_accept = kl_listener_is_detached(m->l);
    if (m->accept_reentrant_close) { m->accept_reentrant_close = 0; kl_listener_close(m->l); }
}
static void lt_dispose(void *ctx, KlSocketHandle fd) {
    LT *m = ctx; m->dispose_calls++; m->disposed_fd = (int)fd;
    m->detached_at_dispose = kl_listener_is_detached(m->l);
    if (m->dispose_reentrant_close) { m->dispose_reentrant_close = 0; kl_listener_close(m->l); }
}
static void lt_on_close(void *ctx) { LT *m = ctx; m->close_calls++; }

static void lt_setup(LT *m, KlListener *l, int completion_mode) {
    memset(m, 0, sizeof(*m));
    m->l = l; m->slots = 1; m->alive = 1; m->arm_fd_base = 1000;
    KlListenerHooks h = {
        .reserve = lt_reserve, .release = lt_release, .credit_ctx = m, .liveness = &m->alive,
        .arm_accept = lt_arm, .disarm_accept = lt_disarm, .cancel_accept = lt_cancel,
        .on_accept = lt_on_accept, .dispose_fd = lt_dispose, .on_close = lt_on_close,
    };
    kl_listener_init(l, completion_mode, &h, m);
}

/* ── Tests ─────────────────────────────────────────────────────────────────────────────────── */

UTEST(listener, start_reserves_then_arms) {
    KlListener l; LT m; lt_setup(&m, &l, /*completion=*/0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.reserve_calls, 1);            /* a slot is reserved before arming */
    ASSERT_EQ(m.arm_calls, 1);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_LISTENING);
    kl_listener_close(&l);
    ASSERT_EQ(m.release_calls, 1);            /* the held reservation is returned on close */
    ASSERT_EQ(m.close_calls, 1);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, accept_hands_off_lease_and_rearms) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 2;
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_on_accepted(&l, (KlSocketHandle)1000);
    ASSERT_EQ(m.accept_calls, 1);
    ASSERT_EQ(m.last_fd, 1000);
    ASSERT_EQ(m.arm_calls, 2);                /* re-armed for the next connection */
    /* the committed lease releases to the pool via the pool-owned capability */
    kl_slot_lease_release(&m.last_lease);
    ASSERT_EQ(m.release_calls, 1);
}

UTEST(listener, backpressure_pauses_when_no_slot) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);      /* reserves the one slot, arms */
    kl_listener_on_accepted(&l, (KlSocketHandle)1000);   /* commits it; next reserve finds none */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_PAUSED);
    ASSERT_EQ(m.arm_calls, 1);                /* no second arm while paused */

    /* the accepted connection closes → slot returns → resume */
    kl_slot_lease_release(&m.last_lease);     /* slots: 0 → 1 */
    kl_listener_notify_slot_free(&l);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_LISTENING);
    ASSERT_EQ(m.arm_calls, 2);
}

UTEST(listener, readiness_pause_disarms_then_resume_rearms) {
    /* Readiness backpressure must DROP the listen interest on pause (a level-triggered fd would
     * otherwise keep firing), and re-arm on resume. Exposed by the live server wiring (step 6B-1). */
    KlListener l; LT m; lt_setup(&m, &l, /*completion=*/0);
    m.slots = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);          /* reserves the one slot, arms */
    kl_listener_on_accepted(&l, (KlSocketHandle)1000);   /* commits it; next reserve → 0 → PAUSED */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_PAUSED);
    ASSERT_EQ(m.disarm_calls, 1);                 /* pause dropped the listen interest */

    kl_slot_lease_release(&m.last_lease);          /* slot returns */
    kl_listener_notify_slot_free(&l);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_LISTENING);
    ASSERT_EQ(m.arm_calls, 2);                     /* resume re-armed */
    kl_listener_close(&l);
}

UTEST(listener, notify_slot_free_noop_when_listening) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 2;
    ASSERT_EQ(kl_listener_start(&l), 0);
    int arms = m.arm_calls;
    kl_listener_notify_slot_free(&l);         /* not paused — no-op */
    ASSERT_EQ(m.arm_calls, arms);
}

UTEST(listener, accepted_stream_lease_outlives_listener) {
    /* Accept a connection, fully close+detach the listener, THEN release the lease — it must still
     * reach the pool (the release capability is pool-owned, baked by value into the lease). */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 2;
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_on_accepted(&l, (KlSocketHandle)1000);   /* lease captured */
    kl_listener_close(&l);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);            /* listener gone */
    ASSERT_EQ(m.release_calls, 1);                        /* only the held reservation so far */

    kl_slot_lease_release(&m.last_lease);                 /* accepted stream closes AFTER listener */
    ASSERT_EQ(m.release_calls, 2);                        /* committed slot still released to pool */
}

UTEST(listener, lease_liveness_guard_noops_when_pool_gone) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 2;
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_on_accepted(&l, (KlSocketHandle)1000);
    int before = m.release_calls;
    m.alive = 0;                              /* the pool has been torn down */
    kl_slot_lease_release(&m.last_lease);     /* liveness guard → safe no-op */
    ASSERT_EQ(m.release_calls, before);
}

UTEST(listener, unbounded_no_credit_accounting) {
    /* reserve/release NULL → no backpressure; leases carry a NULL release (no-op). */
    KlListener l; LT m; memset(&m, 0, sizeof(m)); m.l = &l; m.arm_fd_base = 1000;
    KlListenerHooks h = {
        .arm_accept = lt_arm, .disarm_accept = lt_disarm,
        .on_accept = lt_on_accept, .dispose_fd = lt_dispose, .on_close = lt_on_close,
    };
    ASSERT_EQ(kl_listener_init(&l, 0, &h, &m), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.reserve_calls, 0);            /* no reserve hook */
    kl_listener_on_accepted(&l, (KlSocketHandle)1000);
    ASSERT_EQ(m.accept_calls, 1);
    ASSERT_EQ(m.arm_calls, 2);                /* keeps accepting — never paused */
    kl_slot_lease_release(&m.last_lease);     /* NULL release — safe no-op */
    ASSERT_EQ(m.release_calls, 0);
}

UTEST(listener, sync_accept_inline) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 10; m.sync_accept_budget = 1;   /* first arm inline-accepts, then async */
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.accept_calls, 1);             /* delivered inline from the arm hook */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_LISTENING);
}

UTEST(listener, many_sync_accepts_bounded) {
    /* A run of synchronous accepts must not recurse (iterative trampoline). */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 100000; m.sync_accept_budget = 50000;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.accept_calls, 50000);
}

UTEST(listener, sync_accept_failures_rearm_bounded) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 100000; m.sync_fail_budget = 10000; m.arm_err = 4;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.accept_calls, 0);
    ASSERT_EQ(m.arm_calls, 10001);            /* failed arms + one final async arm */
    /* each failed accept returned its reserved slot */
    ASSERT_EQ(m.reserved_now, 1);             /* only the final armed accept holds a reservation */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_LISTENING);
}

UTEST(listener, accept_failed_releases_slot_and_rearms) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.reserved_now, 1);
    kl_listener_on_accept_failed(&l, 9);      /* transient accept error */
    ASSERT_EQ(m.release_calls, 1);            /* the reserved slot returned */
    ASSERT_EQ(m.arm_calls, 2);                /* re-armed (slot available again) */
    ASSERT_EQ(m.reserved_now, 1);
}

UTEST(listener, close_while_paused_detaches) {
    /* Teardown while PAUSED (backpressure): close must still reach confirmed detachment. */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 0;                                  /* reserve fails at start → PAUSED */
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_PAUSED);
    kl_listener_close(&l);
    ASSERT_EQ(m.close_calls, 1);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, readiness_close_disarms_and_detaches) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_close(&l);
    ASSERT_EQ(m.disarm_calls, 1);             /* readiness drops interest */
    ASSERT_EQ(m.release_calls, 1);            /* reservation returned */
    ASSERT_EQ(m.close_calls, 1);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_CLOSED);
}

UTEST(listener, completion_close_cancels_and_waits_for_straggler) {
    KlListener l; LT m; lt_setup(&m, &l, /*completion=*/1);
    ASSERT_EQ(kl_listener_start(&l), 0);      /* accept posted (async) */
    kl_listener_close(&l);
    ASSERT_EQ(m.cancel_calls, 1);             /* cancel requested once */
    ASSERT_EQ(m.release_calls, 1);            /* held reservation returned */
    ASSERT_EQ(kl_listener_is_detached(&l), 0);/* posted accept still outstanding */

    /* a straggler accept completes during teardown → its fd is disposed, then detach */
    kl_listener_on_accepted(&l, (KlSocketHandle)2000);
    ASSERT_EQ(m.dispose_calls, 1);
    ASSERT_EQ(m.disposed_fd, 2000);
    ASSERT_EQ(m.accept_calls, 0);             /* never handed off as a live connection */
    ASSERT_EQ(m.close_calls, 1);
}

UTEST(listener, completion_close_cancel_completes_as_failure) {
    KlListener l; LT m; lt_setup(&m, &l, 1);
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_close(&l);
    ASSERT_EQ(m.cancel_calls, 1);
    kl_listener_on_accept_failed(&l, 0);      /* the cancelled accept completes as an error */
    ASSERT_EQ(m.close_calls, 1);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, double_close_no_recancel) {
    KlListener l; LT m; lt_setup(&m, &l, 1);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(kl_listener_close(&l), 0);
    ASSERT_EQ(kl_listener_close(&l), 0);      /* idempotent — no second cancel */
    ASSERT_EQ(m.cancel_calls, 1);
    kl_listener_on_accept_failed(&l, 0);
    ASSERT_EQ(m.close_calls, 1);
}

UTEST(listener, spurious_accept_disposed) {
    /* With one slot, after the first accept the listener PAUSES (no accept in flight). A second
     * on_accepted is then spurious and its fd must be disposed, not handed off. */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_on_accepted(&l, (KlSocketHandle)3000);   /* real accept; then PAUSED (slot spent) */
    ASSERT_EQ(m.accept_calls, 1);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_PAUSED);
    kl_listener_on_accepted(&l, (KlSocketHandle)3001);   /* spurious — no accept in flight */
    ASSERT_EQ(m.accept_calls, 1);                        /* not handed off */
    ASSERT_EQ(m.dispose_calls, 1);                       /* fd disposed */
    ASSERT_EQ(m.disposed_fd, 3001);
}

UTEST(listener, close_during_accept_callback_defers_detach) {
    /* on_accept reentrantly closes the listener: detachment must wait for the callback to unwind. */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 2;
    m.accept_reentrant_close = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_on_accepted(&l, (KlSocketHandle)4000);
    ASSERT_EQ(m.accept_calls, 1);
    ASSERT_EQ(m.detached_at_accept, 0);       /* not detached inside on_accept */
    ASSERT_EQ(m.close_calls, 1);              /* detached once after unwind */
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, close_during_dispose_callback_defers_detach) {
    KlListener l; LT m; lt_setup(&m, &l, 1);
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_close(&l);                    /* CLOSING; posted accept still outstanding */
    m.dispose_reentrant_close = 1;
    kl_listener_on_accepted(&l, (KlSocketHandle)5000);   /* straggler → dispose → reentrant close */
    ASSERT_EQ(m.dispose_calls, 1);
    ASSERT_EQ(m.detached_at_dispose, 0);      /* not detached inside dispose_fd */
    ASSERT_EQ(m.close_calls, 1);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, hard_arm_failure_closes) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.hardfail = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);      /* arm returns -1 → listener closes */
    ASSERT_EQ(m.release_calls, 1);            /* reserved slot returned */
    ASSERT_EQ(m.close_calls, 1);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_CLOSED);
}

UTEST(listener, reserve_error_closes) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.reserve_error = 1;                       /* reserve returns -1 (pool error) */
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.arm_calls, 0);                 /* never armed — reservation failed */
    ASSERT_EQ(m.close_calls, 1);              /* pool error closes the listener */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_CLOSED);
}

UTEST(listener, no_slot_pauses_not_closes) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 0;                              /* reserve returns 0 (backpressure) */
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_PAUSED);
    ASSERT_EQ(m.arm_calls, 0);
}

UTEST(listener, no_reuse_until_reinit) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_close(&l);
    ASSERT_EQ(kl_listener_start(&l), -1);     /* not IDLE — no restart without re-init */
    lt_setup(&m, &l, 0);                       /* re-init = reuse reset */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_IDLE);
    ASSERT_EQ(kl_listener_start(&l), 0);
}

UTEST(listener, init_validation) {
    KlListener l;
    KlListenerHooks h = { 0 };
    ASSERT_EQ(kl_listener_init(&l, 0, &h, NULL), -1);          /* missing arm/on_accept/dispose */
    h.arm_accept = lt_arm; h.on_accept = lt_on_accept; h.dispose_fd = lt_dispose;
    ASSERT_EQ(kl_listener_init(&l, 0, &h, NULL), -1);          /* readiness needs disarm */
    h.disarm_accept = lt_disarm;
    ASSERT_EQ(kl_listener_init(&l, 0, &h, NULL), 0);
    ASSERT_EQ(kl_listener_init(&l, 1, &h, NULL), 0);           /* completion: disarm optional */
    h.reserve = lt_reserve;                                    /* reserve without release */
    ASSERT_EQ(kl_listener_init(&l, 1, &h, NULL), -1);
    h.release = lt_release;
    ASSERT_EQ(kl_listener_init(&l, 1, &h, NULL), 0);
}

UTEST(listener, uninited_is_safe) {
    KlListener l; memset(&l, 0, sizeof(l));
    ASSERT_EQ(kl_listener_start(&l), -1);
    ASSERT_EQ(kl_listener_close(&l), -1);
    kl_listener_on_accepted(&l, (KlSocketHandle)1);   /* no-op, no crash */
    kl_listener_notify_slot_free(&l);
    ASSERT_EQ(kl_listener_is_detached(&l), 0);
}

UTEST(listener, reentrant_close_from_arm_hook_no_completion) {
    /* The arm hook directly closes the listener WITHOUT reporting an accept completion. Detachment
     * must not fire while l_arm_loop is still on the stack (in_start guard), only after it unwinds. */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.arm_reentrant_close = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.arm_calls, 1);
    ASSERT_EQ(m.disarm_calls, 1);             /* readiness close disarmed */
    ASSERT_EQ(m.reserved_now, 0);             /* held reservation returned, balanced */
    ASSERT_EQ(m.close_calls, 1);              /* detached exactly once, after the arm frame unwound */
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, reentrant_close_from_arm_hook_with_inline_completion) {
    /* The arm hook inline-accepts AND then reentrantly closes in the same invocation: exactly one
     * handoff, detachment deferred until the arm frame unwinds, then fired once — no UAF. */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 10; m.arm_accept_then_close = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.accept_calls, 1);             /* the inline accept was handed off */
    ASSERT_EQ(m.close_calls, 1);              /* detached exactly once after unwind */
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, reentrant_close_from_release_hook) {
    /* The pool release() hook reentrantly closes the listener while returning the held reservation:
     * detachment must be deferred (release runs under in_dispatch) and fire once after it unwinds. */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.reserved_now, 1);
    m.release_reentrant_close = 1;
    kl_listener_on_accept_failed(&l, 9);      /* releases the reserved slot → release hook closes */
    ASSERT_EQ(m.detached_at_release, 0);      /* NOT detached inside the release hook */
    ASSERT_EQ(m.close_calls, 1);              /* detached exactly once after unwind */
    ASSERT_EQ(m.reserved_now, 0);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, reentrant_close_from_reserve_hook) {
    /* The pool reserve() hook reentrantly closes the listener: the just-acquired credit is returned,
     * no accept is armed, detachment is deferred then fired once. */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 1;
    m.reserve_reentrant_close = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.detached_at_reserve, 0);      /* NOT detached inside the reserve hook */
    ASSERT_EQ(m.arm_calls, 0);                /* never armed — reserve hook closed us */
    ASSERT_EQ(m.reserved_now, 0);             /* the acquired credit was returned, balanced */
    ASSERT_EQ(m.close_calls, 1);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, double_lease_release_is_noop) {
    /* Consuming release: a second release on the owned lease returns no extra credit. */
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 2;
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_on_accepted(&l, (KlSocketHandle)1000);
    kl_slot_lease_release(&m.last_lease);
    ASSERT_EQ(m.release_calls, 1);
    kl_slot_lease_release(&m.last_lease);     /* consumed — harmless no-op */
    ASSERT_EQ(m.release_calls, 1);
}

UTEST(listener, reserved_slot_returned_on_readiness_close_while_armed) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.reserved_now, 1);              /* one reservation held for the armed accept */
    kl_listener_close(&l);
    ASSERT_EQ(m.reserved_now, 0);              /* balanced: reservation returned, no leak */
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST_MAIN();
