/*
 * test_listener.c: accept-side listener state machine (src/listener.h),
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
    KlSlotLease held[64]; int held_n;   /* every handed-off lease, for credit-conservation checks */
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
    int retry_budget;          /* arm returns -2 (transient: nothing posted) this many times */
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
    if (m->retry_budget > 0) { m->retry_budget--; return -2; }
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
    return 0;   /* async; awaiting an external completion */
}
static void lt_disarm(void *ctx) { LT *m = ctx; m->disarm_calls++; }
static void lt_cancel(void *ctx) { LT *m = ctx; m->cancel_calls++; }
static void lt_on_accept(void *ctx, KlSocketHandle fd, KlSlotLease lease) {
    LT *m = ctx; m->accept_calls++; m->last_fd = (int)fd; m->last_lease = lease;  /* take ownership */
    if (m->held_n < 64) m->held[m->held_n++] = lease;   /* track every committed lease */
    m->detached_at_accept = kl_listener_is_detached(m->l);
    if (m->accept_reentrant_close) { m->accept_reentrant_close = 0; kl_listener_close(m->l); }
}
/* Committed (handed-off but not-yet-released) leases: a released lease is consumed (NULL fn). */
static int committed_leases(const LT *m) {
    int n = 0;
    for (int i = 0; i < m->held_n; i++) if (m->held[i].release) n++;
    return n;
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
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
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
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_PAUSED);
    ASSERT_EQ(m.arm_calls, 1);                /* no second arm while paused */

    /* the accepted connection closes → slot returns → resume */
    kl_slot_lease_release(&m.last_lease);     /* slots: 0 → 1 */
    kl_listener_notify_slot_free(&l);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
    ASSERT_EQ(m.arm_calls, 2);
}

UTEST(listener, readiness_pause_disarms_then_resume_rearms) {
    /* Readiness backpressure must DROP the listen interest on pause (a level-triggered fd would
     * otherwise keep firing), and re-arm on resume. Exposed by the live server wiring. */
    KlListener l; LT m; lt_setup(&m, &l, /*completion=*/0);
    m.slots = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);          /* reserves the one slot, arms */
    kl_listener_on_accepted(&l, (KlSocketHandle)1000);   /* commits it; next reserve → 0 → PAUSED */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_PAUSED);
    ASSERT_EQ(m.disarm_calls, 1);                 /* pause dropped the listen interest */

    kl_slot_lease_release(&m.last_lease);          /* slot returns */
    kl_listener_notify_slot_free(&l);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
    ASSERT_EQ(m.arm_calls, 2);                     /* resume re-armed */
    kl_listener_close(&l);
}

UTEST(listener, notify_slot_free_noop_when_listening) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 2;
    ASSERT_EQ(kl_listener_start(&l), 0);
    int arms = m.arm_calls;
    kl_listener_notify_slot_free(&l);         /* not paused; no-op */
    ASSERT_EQ(m.arm_calls, arms);
}

UTEST(listener, accepted_stream_lease_outlives_listener) {
    /* Accept a connection, fully close+detach the listener, THEN release the lease; it must still
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
    ASSERT_EQ(m.arm_calls, 2);                /* keeps accepting; never paused */
    kl_slot_lease_release(&m.last_lease);     /* NULL release; safe no-op */
    ASSERT_EQ(m.release_calls, 0);
}

UTEST(listener, sync_accept_inline) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 10; m.sync_accept_budget = 1;   /* first arm inline-accepts, then async */
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.accept_calls, 1);             /* delivered inline from the arm hook */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
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
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
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
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_PAUSED);
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
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_CLOSED);
}

UTEST(listener, completion_close_cancels_and_waits_for_straggler) {
    KlListener l; LT m; lt_setup(&m, &l, /*completion=*/1);
    ASSERT_EQ(kl_listener_start(&l), 0);      /* accept posted (async) */
    kl_listener_close(&l);
    ASSERT_EQ(m.cancel_calls, 1);             /* cancel requested once */
    ASSERT_EQ(m.release_calls, 0);            /* credit stays with the posted accept until it retires */
    ASSERT_EQ(kl_listener_is_detached(&l), 0);/* posted accept still outstanding */

    /* a straggler accept completes during teardown → return its credit, dispose its fd, then detach */
    kl_listener_on_accepted(&l, (KlSocketHandle)2000);
    ASSERT_EQ(m.dispose_calls, 1);
    ASSERT_EQ(m.disposed_fd, 2000);
    ASSERT_EQ(m.release_calls, 1);            /* the posted accept's reservation returned on retire */
    ASSERT_EQ(m.accept_calls, 0);             /* never handed off as a live connection */
    ASSERT_EQ(m.close_calls, 1);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
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
    ASSERT_EQ(kl_listener_close(&l), 0);      /* idempotent; no second cancel */
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
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_PAUSED);
    kl_listener_on_accepted(&l, (KlSocketHandle)3001);   /* spurious; no accept in flight */
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
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_CLOSED);
}

/* A completion accept post can fail for a moment (no memory for the op, a full submission queue, a
 * connection reset before AcceptEx took it): the hook says so with -2 (KL_LISTENER_ARM_RETRY), and
 * the listener returns the credit and pauses, to try again when told a slot is free (the server's
 * sweep tells it every tick). It used to treat every failed post as a broken listen socket and
 * close: the server kept running and never accepted again. */
UTEST(listener, transient_arm_failure_pauses_and_retries) {
    KlListener l; LT m; lt_setup(&m, &l, 1);
    m.retry_budget = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.close_calls, 0);                          /* was: 1, the listener closed */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_PAUSED);
    ASSERT_EQ(m.release_calls, 1);                        /* the post's credit came back */
    ASSERT_EQ(m.reserved_now, 0);
    kl_listener_notify_slot_free(&l);                     /* the next tick: post again */
    ASSERT_EQ(m.arm_calls, 2);
    ASSERT_EQ(m.reserved_now, 1);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
}

UTEST(listener, reserve_error_closes) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.reserve_error = 1;                       /* reserve returns -1 (pool error) */
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.arm_calls, 0);                 /* never armed; reservation failed */
    ASSERT_EQ(m.close_calls, 1);              /* pool error closes the listener */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_CLOSED);
}

UTEST(listener, no_slot_pauses_not_closes) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    m.slots = 0;                              /* reserve returns 0 (backpressure) */
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_PAUSED);
    ASSERT_EQ(m.arm_calls, 0);
}

UTEST(listener, no_reuse_until_reinit) {
    KlListener l; LT m; lt_setup(&m, &l, 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_close(&l);
    ASSERT_EQ(kl_listener_start(&l), -1);     /* not IDLE; no restart without re-init */
    lt_setup(&m, &l, 0);                       /* re-init = reuse reset */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_IDLE);
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
     * handoff, detachment deferred until the arm frame unwinds, then fired once; no UAF. */
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
    ASSERT_EQ(m.arm_calls, 0);                /* never armed; reserve hook closed us */
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
    kl_slot_lease_release(&m.last_lease);     /* consumed; harmless no-op */
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

/* ── Counted bounded-accept window: one reserved credit per posted accept ──────────────────── */

UTEST(listener, window_posts_up_to_window) {
    KlListener l; LT m; lt_setup(&m, &l, /*completion=*/1);
    m.slots = 10;
    ASSERT_EQ(kl_listener_set_accept_window(&l, 3), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.arm_calls, 3);                 /* posted 3 accepts up front (IOCP-style backlog) */
    ASSERT_EQ(m.reserve_calls, 3);             /* one credit each */
    ASSERT_EQ(l.inflight, 3);

    kl_listener_close(&l);
    ASSERT_EQ(m.cancel_calls, 1);              /* one batch cancel */
    ASSERT_EQ(m.release_calls, 0);             /* credits ride with the posted accepts */
    kl_listener_on_accept_failed(&l, 0);
    kl_listener_on_accept_failed(&l, 0);
    ASSERT_EQ(kl_listener_is_detached(&l), 0); /* detach waits for ALL posted accepts */
    kl_listener_on_accept_failed(&l, 0);
    ASSERT_EQ(m.release_calls, 3);             /* each completion returned exactly its reservation */
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, window_capped_by_credits) {
    KlListener l; LT m; lt_setup(&m, &l, 1);
    m.slots = 2;
    ASSERT_EQ(kl_listener_set_accept_window(&l, 5), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.arm_calls, 2);                 /* window 5 but only 2 credits → 2 posted */
    ASSERT_EQ(l.inflight, 2);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);   /* inflight>0 → not paused */
    kl_listener_close(&l);
    kl_listener_on_accept_failed(&l, 0);
    kl_listener_on_accept_failed(&l, 0);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, window_tops_up_on_completion) {
    KlListener l; LT m; lt_setup(&m, &l, 1);
    m.slots = 10;
    ASSERT_EQ(kl_listener_set_accept_window(&l, 2), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.arm_calls, 2);
    kl_listener_on_accepted(&l, (KlSocketHandle)500);   /* one connects → commit + refill window */
    ASSERT_EQ(m.accept_calls, 1);
    ASSERT_EQ(m.arm_calls, 3);                 /* posted a replacement to refill the window */
    ASSERT_EQ(l.inflight, 2);
    kl_slot_lease_release(&m.last_lease);
    kl_listener_close(&l);
    kl_listener_on_accept_failed(&l, 0);
    kl_listener_on_accept_failed(&l, 0);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, window_exhaustion_pauses_then_resumes) {
    KlListener l; LT m; lt_setup(&m, &l, 1);
    m.slots = 2;
    ASSERT_EQ(kl_listener_set_accept_window(&l, 2), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);       /* posts 2, credits exhausted */
    kl_listener_on_accepted(&l, (KlSocketHandle)600);   /* commit; top-up reserve fails */
    kl_listener_on_accepted(&l, (KlSocketHandle)601);   /* commit; inflight 0 → PAUSED */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_PAUSED);
    ASSERT_EQ(m.arm_calls, 2);                 /* no posts while out of credit */

    kl_slot_lease_release(&m.last_lease);       /* a connection closes → a credit frees */
    kl_listener_notify_slot_free(&l);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
    ASSERT_EQ(m.arm_calls, 3);                 /* resumed; posted one */
    kl_listener_close(&l);
    kl_listener_on_accept_failed(&l, 0);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, completion_exhaustion_queues_not_drops) {
    /* When the pool is exhausted, a POST-DRIVEN completion listener must PAUSE; post
     * NO accept; so a further connection waits in the kernel TCP backlog. It must NOT accept the
     * fd and then drop it (the accept-then-reset failure mode). Proof: while exhausted no accept is
     * posted (arm frozen) and nothing is disposed; when a slot frees, the listener posts and the
     * queued connection is accepted; never reset. */
    KlListener l; LT m; lt_setup(&m, &l, /*completion=*/1);
    m.slots = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);       /* reserve the one credit, post one accept */
    ASSERT_EQ(m.arm_calls, 1);

    kl_listener_on_accepted(&l, (KlSocketHandle)2000);   /* commit → pool exhausted → PAUSE */
    ASSERT_EQ(m.accept_calls, 1);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_PAUSED);
    ASSERT_EQ(m.arm_calls, 1);                  /* no further accept posted while exhausted */
    ASSERT_EQ(m.dispose_calls, 0);             /* nothing accepted-and-dropped; it queues instead */

    /* The first connection closes → its credit returns → the queued connection is now served. */
    kl_slot_lease_release(&m.last_lease);       /* slots: 0 → 1 */
    kl_listener_notify_slot_free(&l);
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
    ASSERT_EQ(m.arm_calls, 2);                  /* posts an accept for the previously-queued conn */
    kl_listener_on_accepted(&l, (KlSocketHandle)2001);
    ASSERT_EQ(m.accept_calls, 2);              /* the queued connection is accepted, not dropped */
    ASSERT_EQ(m.dispose_calls, 0);

    kl_slot_lease_release(&m.last_lease);
    kl_listener_close(&l);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

UTEST(listener, window_sync_completion_bounded) {
    /* Many synchronous accept completions while filling a window must not recurse (pump trampoline)
     * and must still leave the window filled with async posts once the sync budget is exhausted. */
    KlListener l; LT m; lt_setup(&m, &l, 1);
    m.slots = 100000; m.sync_accept_budget = 5000;
    ASSERT_EQ(kl_listener_set_accept_window(&l, 4), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.accept_calls, 5000);           /* all inline accepts delivered, no recursion */
    ASSERT_EQ(l.inflight, 4);                   /* window filled with async posts after the budget */
}

UTEST(listener, set_accept_window_validation) {
    KlListener l; LT m; lt_setup(&m, &l, 1);
    ASSERT_EQ(kl_listener_set_accept_window(&l, 0), -1);   /* window must be >= 1 */
    ASSERT_EQ(kl_listener_set_accept_window(&l, 4), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(kl_listener_set_accept_window(&l, 8), -1);   /* init-time only (not after start) */
}

UTEST(listener, readiness_rejects_window_over_one) {
    /* A readiness arm is one persistent interest, not N posted ops; a window > 1 is rejected. */
    KlListener l; LT m; lt_setup(&m, &l, /*completion=*/0);
    ASSERT_EQ(kl_listener_set_accept_window(&l, 2), -1);
    ASSERT_EQ(kl_listener_set_accept_window(&l, 1), 0);    /* window 1 is fine */
}

UTEST(listener, credit_conservation_through_lifecycle) {
    /* available credits + posted accepts + committed leases == capacity, at every phase. */
    KlListener l; LT m; lt_setup(&m, &l, /*completion=*/1);
    const int CAP = 4;
    m.slots = CAP;
    ASSERT_EQ(kl_listener_set_accept_window(&l, 3), 0);
#define INV() ASSERT_EQ(m.slots + l.inflight + committed_leases(&m), CAP)

    ASSERT_EQ(kl_listener_start(&l), 0);               /* fill window: 3 posted */
    ASSERT_EQ(l.inflight, 3);
    INV();

    kl_listener_on_accepted(&l, (KlSocketHandle)10);   /* commit + refill */
    INV();
    kl_listener_on_accepted(&l, (KlSocketHandle)11);   /* commit; credit now exhausted, stays LISTENING */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
    INV();

    kl_slot_lease_release(&m.held[0]);                 /* a connection closes → a credit frees */
    kl_listener_notify_slot_free(&l);
    INV();

    kl_listener_close(&l);                             /* completion close: batch cancel */
    while (l.inflight > 0) kl_listener_on_accept_failed(&l, 0);   /* drain posted accepts */
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
    INV();

    for (int i = 0; i < m.held_n; i++)                 /* release the remaining committed leases */
        if (m.held[i].release) kl_slot_lease_release(&m.held[i]);
    ASSERT_EQ(m.slots, CAP);                            /* every credit returned to the pool */
#undef INV
}

/* ── Object handoff family (a non-socket transport's own connection object) ───────────────────── */

typedef struct { int tag; } Conn;   /* stands in for an adapter object, e.g. a pipe stream */
typedef struct {
    LT      base;                  /* reuses the credit/arm mocks */
    int     accept_obj_calls, dispose_obj_calls;
    void   *last_conn, *disposed_conn;
    KlSlotLease obj_lease;
} LO;
static void lo_on_accept_obj(void *ctx, void *conn, KlSlotLease lease) {
    LO *m = ctx; m->accept_obj_calls++; m->last_conn = conn; m->obj_lease = lease;
}
static void lo_dispose_obj(void *ctx, void *conn) {
    LO *m = ctx; m->dispose_obj_calls++; m->disposed_conn = conn;
}
static int lo_setup(LO *m, KlListener *l, int completion_mode) {
    memset(m, 0, sizeof(*m));
    m->base.l = l; m->base.slots = 1; m->base.alive = 1;
    KlListenerHooks h = {
        .reserve = lt_reserve, .release = lt_release, .credit_ctx = &m->base,
        .liveness = &m->base.alive,
        .arm_accept = lt_arm, .disarm_accept = lt_disarm, .cancel_accept = lt_cancel,
        .on_close = lt_on_close,
        .on_accept_obj = lo_on_accept_obj, .dispose_obj = lo_dispose_obj,
    };
    return kl_listener_init(l, completion_mode, &h, m);
}
/* lt_* hooks take the LT via ctx; the object hooks take the LO. The listener passes ONE ctx to every
 * hook, so LT must be the first member of LO (it is) for the shared mocks to see their fields. */

UTEST(listener_obj, init_requires_exactly_one_complete_family) {
    KlListener l; LT m; memset(&m, 0, sizeof m);
    KlListenerHooks base = { .arm_accept = lt_arm, .disarm_accept = lt_disarm };
    KlListenerHooks h;
    h = base;                                                         /* neither family */
    ASSERT_EQ(kl_listener_init(&l, 0, &h, &m), -1);
    h = base; h.on_accept = lt_on_accept; h.dispose_fd = lt_dispose;  /* fd family */
    ASSERT_EQ(kl_listener_init(&l, 0, &h, &m), 0);
    h = base; h.on_accept_obj = lo_on_accept_obj; h.dispose_obj = lo_dispose_obj;   /* obj family */
    ASSERT_EQ(kl_listener_init(&l, 0, &h, &m), 0);
    h = base; h.on_accept = lt_on_accept; h.dispose_fd = lt_dispose;  /* both families */
    h.on_accept_obj = lo_on_accept_obj; h.dispose_obj = lo_dispose_obj;
    ASSERT_EQ(kl_listener_init(&l, 0, &h, &m), -1);
    h = base; h.on_accept_obj = lo_on_accept_obj;                    /* half an obj family */
    ASSERT_EQ(kl_listener_init(&l, 0, &h, &m), -1);
    h = base; h.dispose_obj = lo_dispose_obj;
    ASSERT_EQ(kl_listener_init(&l, 0, &h, &m), -1);
    h = base; h.on_accept = lt_on_accept; h.dispose_obj = lo_dispose_obj;   /* crossed halves */
    ASSERT_EQ(kl_listener_init(&l, 0, &h, &m), -1);
}

UTEST(listener_obj, accept_hands_off_object_and_lease) {
    KlListener l; LO m; ASSERT_EQ(lo_setup(&m, &l, 1), 0);
    m.base.slots = 2;
    ASSERT_EQ(kl_listener_start(&l), 0);
    Conn c = { 7 };
    kl_listener_on_accepted_obj(&l, &c);
    ASSERT_EQ(m.accept_obj_calls, 1);
    ASSERT_TRUE(m.last_conn == &c);                  /* passed through untouched */
    ASSERT_EQ(m.base.accept_calls, 0);               /* the fd hook is never used */
    ASSERT_EQ(m.base.arm_calls, 2);                  /* refilled the window */
    ASSERT_EQ(m.base.reserved_now, 2);               /* one committed, one held by the new post */
    kl_slot_lease_release(&m.obj_lease);             /* the owner releases the committed credit */
    ASSERT_EQ(m.base.reserved_now, 1);
    kl_listener_close(&l);
    kl_listener_on_accept_failed(&l, -1);            /* the batch cancel retires the posted accept */
    ASSERT_EQ(m.base.reserved_now, 0);
    ASSERT_EQ(m.base.close_calls, 1);
}

UTEST(listener_obj, teardown_disposes_object_and_returns_credit) {
    KlListener l; LO m; ASSERT_EQ(lo_setup(&m, &l, 1), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    kl_listener_close(&l);                           /* CLOSING: one accept still posted */
    ASSERT_EQ(m.base.close_calls, 0);
    Conn c = { 1 };
    kl_listener_on_accepted_obj(&l, &c);             /* it completes anyway (raced the cancel) */
    ASSERT_EQ(m.accept_obj_calls, 0);                /* never handed off while closing */
    ASSERT_EQ(m.dispose_obj_calls, 1);
    ASSERT_TRUE(m.disposed_conn == &c);
    ASSERT_EQ(m.base.reserved_now, 0);               /* its credit went back */
    ASSERT_EQ(m.base.close_calls, 1);                /* and that was the last retirement */
}

UTEST(listener_obj, spurious_object_is_disposed) {
    KlListener l; LO m; ASSERT_EQ(lo_setup(&m, &l, 1), 0);
    Conn c = { 2 };
    kl_listener_on_accepted_obj(&l, &c);             /* nothing posted (IDLE) */
    ASSERT_EQ(m.dispose_obj_calls, 1);
    ASSERT_EQ(m.accept_obj_calls, 0);
}

UTEST(listener_obj, wrong_family_entry_is_refused_not_leaked) {
    /* object-family listener fed an fd: retired as failed, credit returned, fd never disposed here */
    KlListener l; LO m; ASSERT_EQ(lo_setup(&m, &l, 1), 0);
    m.base.slots = 1;
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.base.reserved_now, 1);
    kl_listener_on_accepted(&l, (KlSocketHandle)42);
    ASSERT_EQ(m.accept_obj_calls, 0);
    ASSERT_EQ(m.base.accept_calls, 0);
    ASSERT_EQ(m.dispose_obj_calls, 0);
    ASSERT_EQ(m.base.dispose_calls, 0);
    ASSERT_EQ(m.base.arm_calls, 2);                  /* the failed accept was refilled */
    ASSERT_EQ(m.base.reserved_now, 1);               /* old credit returned, new one held: no leak */
    kl_listener_close(&l);
    kl_listener_on_accept_failed(&l, -1);
    ASSERT_EQ(m.base.close_calls, 1);

    /* fd-family listener fed an object: the same refusal */
    KlListener l2; LT f; lt_setup(&f, &l2, 1);
    ASSERT_EQ(kl_listener_start(&l2), 0);
    Conn c = { 3 };
    kl_listener_on_accepted_obj(&l2, &c);
    ASSERT_EQ(f.accept_calls, 0);
    ASSERT_EQ(f.dispose_calls, 0);
    ASSERT_EQ(f.reserved_now, 1);
    ASSERT_EQ(f.arm_calls, 2);
    kl_listener_close(&l2);
    kl_listener_on_accept_failed(&l2, -1);
    ASSERT_EQ(f.close_calls, 1);
}

UTEST(listener_obj, window_accepts_objects_like_fds) {
    KlListener l; LO m; ASSERT_EQ(lo_setup(&m, &l, 1), 0);
    m.base.slots = 8;
    ASSERT_EQ(kl_listener_set_accept_window(&l, 4), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.base.arm_calls, 4);
    Conn c[3] = { {0}, {1}, {2} };
    for (int i = 0; i < 3; i++) kl_listener_on_accepted_obj(&l, &c[i]);
    ASSERT_EQ(m.accept_obj_calls, 3);
    ASSERT_EQ(m.base.arm_calls, 7);                  /* each accept refilled the window */
    kl_listener_close(&l);
    ASSERT_EQ(m.base.cancel_calls, 1);               /* one batch cancel */
    for (int i = 0; i < 4; i++) kl_listener_on_accept_failed(&l, -1);
    ASSERT_EQ(m.base.close_calls, 1);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);
}

/* ── Object-family ownership edges (each mirrors an fd-family case, through the object hooks) ── */

/* An arm hook that completes object accepts INLINE, then optionally hard-fails. */
typedef struct {
    LO    lo;                  /* LT at offset 0 via LO.base: the shared credit mocks still work */
    Conn  pool[64];
    int   sync_budget;         /* inline object accepts before going async */
    int   hardfail_after;      /* arm returns -1 once this many arms have run (0 = never) */
    int   close_in_accept;     /* on_accept_obj reentrantly closes the listener */
    int   close_in_dispose;    /* dispose_obj reentrantly closes the listener */
    int   detached_in_accept, detached_in_dispose;
} LS;
static int ls_arm(void *ctx) {
    LS *m = ctx; LT *b = &m->lo.base;
    b->arm_calls++;
    if (m->hardfail_after && b->arm_calls >= m->hardfail_after) return -1;
    if (m->sync_budget > 0) {
        m->sync_budget--;
        kl_listener_on_accepted_obj(b->l, &m->pool[b->arm_calls % 64]);
    }
    return 0;
}
static void ls_on_accept_obj(void *ctx, void *conn, KlSlotLease lease) {
    LS *m = ctx;
    m->lo.accept_obj_calls++; m->lo.last_conn = conn; m->lo.obj_lease = lease;
    if (m->close_in_accept) {
        m->close_in_accept = 0;
        kl_listener_close(m->lo.base.l);
        m->detached_in_accept = kl_listener_is_detached(m->lo.base.l);
    }
}
static void ls_dispose_obj(void *ctx, void *conn) {
    LS *m = ctx;
    m->lo.dispose_obj_calls++; m->lo.disposed_conn = conn;
    if (m->close_in_dispose) {
        m->close_in_dispose = 0;
        kl_listener_close(m->lo.base.l);
        m->detached_in_dispose = kl_listener_is_detached(m->lo.base.l);
    }
}
static int ls_setup(LS *m, KlListener *l, int slots) {
    memset(m, 0, sizeof(*m));
    m->lo.base.l = l; m->lo.base.slots = slots; m->lo.base.alive = 1;
    KlListenerHooks h = {
        .reserve = lt_reserve, .release = lt_release, .credit_ctx = &m->lo.base,
        .liveness = &m->lo.base.alive,
        .arm_accept = ls_arm, .cancel_accept = lt_cancel, .on_close = lt_on_close,
        .on_accept_obj = ls_on_accept_obj, .dispose_obj = ls_dispose_obj,
    };
    return kl_listener_init(l, /*completion=*/1, &h, m);
}

UTEST(listener_obj, synchronous_inline_accepts_hand_off_each_object_once) {
    KlListener l; LS m; ASSERT_EQ(ls_setup(&m, &l, 64), 0);
    m.sync_budget = 40;                               /* a long run of inline completions */
    ASSERT_EQ(kl_listener_start(&l), 0);
    ASSERT_EQ(m.lo.accept_obj_calls, 40);             /* every inline accept handed off */
    ASSERT_EQ(m.lo.dispose_obj_calls, 0);
    ASSERT_EQ(m.lo.base.arm_calls, 41);               /* then one async post stays pending */
    ASSERT_EQ(kl_listener_state(&l), KL_LISTENER_STATE_LISTENING);
    kl_listener_close(&l);
    kl_listener_on_accept_failed(&l, -1);
    ASSERT_EQ(m.lo.base.close_calls, 1);
}

UTEST(listener_obj, owner_closing_inside_accept_keeps_the_object) {
    KlListener l; LS m; ASSERT_EQ(ls_setup(&m, &l, 4), 0);
    ASSERT_EQ(kl_listener_set_accept_window(&l, 2), 0);
    ASSERT_EQ(kl_listener_start(&l), 0);              /* two accepts posted */
    m.close_in_accept = 1;
    Conn c = { 9 };
    kl_listener_on_accepted_obj(&l, &c);
    ASSERT_EQ(m.lo.accept_obj_calls, 1);
    ASSERT_TRUE(m.lo.last_conn == &c);                /* the owner has it ... */
    ASSERT_EQ(m.lo.dispose_obj_calls, 0);             /* ... and it is never disposed */
    ASSERT_EQ(m.detached_in_accept, 0);               /* no detach inside the callback */
    ASSERT_EQ(m.lo.base.close_calls, 0);              /* the other posted accept is outstanding */
    kl_listener_on_accept_failed(&l, -1);             /* it retires (cancelled) */
    ASSERT_EQ(m.lo.base.close_calls, 1);
    ASSERT_EQ(m.lo.dispose_obj_calls, 0);
    kl_slot_lease_release(&m.lo.obj_lease);
    ASSERT_EQ(m.lo.base.reserved_now, 0);             /* every credit accounted for */

    /* Window 1: the handed-off accept was the LAST one in flight, so only the deferral keeps the
     * listener from detaching inside the callback; it detaches as the callback unwinds. */
    KlListener l1; LS m1; ASSERT_EQ(ls_setup(&m1, &l1, 4), 0);
    ASSERT_EQ(kl_listener_start(&l1), 0);
    m1.close_in_accept = 1;
    Conn c1 = { 10 };
    kl_listener_on_accepted_obj(&l1, &c1);
    ASSERT_EQ(m1.lo.accept_obj_calls, 1);
    ASSERT_EQ(m1.detached_in_accept, 0);              /* not while the owner's callback ran */
    ASSERT_EQ(kl_listener_is_detached(&l1), 1);       /* but as soon as it returned */
    ASSERT_EQ(m1.lo.base.close_calls, 1);
    ASSERT_EQ(m1.lo.dispose_obj_calls, 0);            /* and the object stayed the owner's */
}

UTEST(listener_obj, refill_failure_never_disposes_a_handed_off_object) {
    KlListener l; LS m; ASSERT_EQ(ls_setup(&m, &l, 4), 0);
    m.hardfail_after = 2;                             /* the refill after the first accept fails */
    ASSERT_EQ(kl_listener_start(&l), 0);
    Conn c = { 5 };
    kl_listener_on_accepted_obj(&l, &c);              /* handed off, then the refill arm fails */
    ASSERT_EQ(m.lo.accept_obj_calls, 1);
    ASSERT_EQ(m.lo.dispose_obj_calls, 0);
    ASSERT_EQ(kl_listener_is_detached(&l), 1);        /* hard arm failure closed it; nothing posted */
    ASSERT_EQ(m.lo.base.close_calls, 1);
    kl_slot_lease_release(&m.lo.obj_lease);
    ASSERT_EQ(m.lo.base.reserved_now, 0);             /* the failed refill's credit came back too */
}

UTEST(listener_obj, close_inside_dispose_defers_detach_until_it_returns) {
    KlListener l; LS m; ASSERT_EQ(ls_setup(&m, &l, 4), 0);
    m.close_in_dispose = 1;
    Conn c = { 6 };
    kl_listener_on_accepted_obj(&l, &c);              /* spurious (IDLE): disposed */
    ASSERT_EQ(m.lo.dispose_obj_calls, 1);
    ASSERT_EQ(m.detached_in_dispose, 0);              /* not detached while dispose was running */
    ASSERT_EQ(kl_listener_is_detached(&l), 1);        /* detached as it unwound, exactly once */
    ASSERT_EQ(m.lo.base.close_calls, 1);
}

UTEST_MAIN();
