/*
 * test_connect_op.c: outbound-connect terminal-once state machine
 * (src/connect_op.h), exercised in isolation with mock resolve/connect/cancel hooks (no live
 * sockets, no timers).
 *
 * Verifies the four principles: terminal exactly once; synchronous resolver/connect completion
 * safety (inline resolve + inline connect, bounded stack); cancellation requested once per active
 * op (resolve + each racing attempt), reentrancy-safe; and no detachment until every outstanding
 * op is physically retired (Happy Eyeballs losers included).
 */
#include "utest.h"
#include "../src/connect_op.h"
#include <string.h>

/* Programmed behavior per op. */
enum { M_ASYNC = 0, M_SYNC_OK, M_SYNC_FAIL, M_HARDFAIL };

typedef struct {
    KlConnectOp *op;
    /* observations */
    int done_calls; KlConnectResult last_result; int last_fd; int last_error;
    int detach_calls;
    int resolve_calls, resolve_cancel_calls;
    int attempt_calls[KL_CONNECT_MAX_ADDRS], attempt_cancel_calls[KL_CONNECT_MAX_ADDRS];
    int arm_delay_calls, cancel_delay_calls;
    int arm_deadline_calls, cancel_deadline_calls;
    int dispose_calls, disposed_fd;
    int dispose_reentrant_cancel;   /* dispose_fd reentrantly calls kl_connect_op_cancel */
    int detached_at_cancel;   /* snapshot of is_detached observed from inside a cancel hook */
    int detached_at_done;     /* snapshot of is_detached observed from inside on_done */
    int detached_at_dispose;  /* snapshot of is_detached observed from inside dispose_fd */
    /* programmed resolve */
    int resolve_mode, resolve_naddrs, resolve_err;
    /* programmed attempts */
    int attempt_mode[KL_CONNECT_MAX_ADDRS], attempt_err[KL_CONNECT_MAX_ADDRS];
    int fd_base;
    /* cancel-hook behaviors */
    int cancel_attempt_sync_fail[KL_CONNECT_MAX_ADDRS]; /* cancel_attempt → on_attempt_failed inline */
    int cancel_attempt_reentrant;                        /* cancel_attempt → kl_connect_op_cancel */
    int cancel_resolve_sync_fail;                        /* cancel_resolve → on_resolve_failed inline */
    /* on_done reentrancy behaviors */
    int on_done_cancel;        /* on_done calls kl_connect_op_cancel */
    int on_done_fail;          /* on_done retires a loser via on_attempt_failed */
    int on_done_fail_idx;
    /* timer arm behaviors */
    int arm_delay_fail;          /* arm_delay returns -1 */
    int arm_delay_inline_fire;   /* arm_delay calls on_delay inline */
    int arm_deadline_fail;       /* arm_deadline returns -1 */
    int arm_deadline_inline_fire;/* arm_deadline calls on_deadline inline */
    int arm_deadline_err;
} CO;

static int co_start_resolve(void *ctx) {
    CO *m = ctx; m->resolve_calls++;
    switch (m->resolve_mode) {
    case M_SYNC_OK:   kl_connect_op_on_resolved(m->op, m->resolve_naddrs); return 0;
    case M_SYNC_FAIL: kl_connect_op_on_resolve_failed(m->op, m->resolve_err); return 0;
    case M_HARDFAIL:  return -1;
    default:          return 0;   /* async; a completion arrives later */
    }
}
static void co_cancel_resolve(void *ctx) {
    CO *m = ctx; m->resolve_cancel_calls++;
    m->detached_at_cancel = kl_connect_op_is_detached(m->op);
    if (m->cancel_resolve_sync_fail) kl_connect_op_on_resolve_failed(m->op, m->resolve_err);
}
static int co_start_attempt(void *ctx, int idx, int *out_err) {
    CO *m = ctx; m->attempt_calls[idx]++;
    switch (m->attempt_mode[idx]) {
    case M_SYNC_OK:   kl_connect_op_on_attempt_connected(m->op, idx, (KlSocketHandle)(m->fd_base + idx)); return 0;
    case M_SYNC_FAIL: kl_connect_op_on_attempt_failed(m->op, idx, m->attempt_err[idx]); return 0;
    case M_HARDFAIL:  if (out_err) *out_err = m->attempt_err[idx]; return -1;
    default:          return 0;   /* async */
    }
}
static void co_cancel_attempt(void *ctx, int idx) {
    CO *m = ctx; m->attempt_cancel_calls[idx]++;
    m->detached_at_cancel = kl_connect_op_is_detached(m->op);
    if (m->cancel_attempt_reentrant)         kl_connect_op_cancel(m->op);
    if (m->cancel_attempt_sync_fail[idx])    kl_connect_op_on_attempt_failed(m->op, idx, 99);
}
static void co_dispose_fd(void *ctx, KlSocketHandle fd) {
    CO *m = ctx; m->dispose_calls++; m->disposed_fd = (int)fd;
    m->detached_at_dispose = kl_connect_op_is_detached(m->op);
    if (m->dispose_reentrant_cancel) { m->dispose_reentrant_cancel = 0; kl_connect_op_cancel(m->op); }
}
static int co_arm_delay(void *ctx) {
    CO *m = ctx; m->arm_delay_calls++;
    if (m->arm_delay_inline_fire) { m->arm_delay_inline_fire = 0; kl_connect_op_on_delay(m->op); }
    return m->arm_delay_fail ? -1 : 0;
}
static void co_cancel_delay(void *ctx)   { CO *m = ctx; m->cancel_delay_calls++; }
static int co_arm_deadline(void *ctx, int *out_err) {
    CO *m = ctx; m->arm_deadline_calls++;
    if (m->arm_deadline_inline_fire) { m->arm_deadline_inline_fire = 0; kl_connect_op_on_deadline(m->op, m->arm_deadline_err); }
    if (m->arm_deadline_fail) { if (out_err) *out_err = m->arm_deadline_err; return -1; }
    return 0;
}
static void co_cancel_deadline(void *ctx){ CO *m = ctx; m->cancel_deadline_calls++; }
static void co_on_done(void *ctx, KlConnectResult r, KlSocketHandle fd, int err) {
    CO *m = ctx; m->done_calls++; m->last_result = r; m->last_fd = (int)fd; m->last_error = err;
    m->detached_at_done = kl_connect_op_is_detached(m->op);
    if (m->on_done_cancel) { m->on_done_cancel = 0; kl_connect_op_cancel(m->op); }
    if (m->on_done_fail)   { m->on_done_fail = 0; kl_connect_op_on_attempt_failed(m->op, m->on_done_fail_idx, 0); }
}
static void co_on_detach(void *ctx) { CO *m = ctx; m->detach_calls++; }

static void co_setup(CO *m, KlConnectOp *op) {
    memset(m, 0, sizeof(*m));
    m->op = op; m->fd_base = 100;
    KlConnectOpHooks h = {
        .start_resolve  = co_start_resolve,   .cancel_resolve  = co_cancel_resolve,
        .start_attempt  = co_start_attempt,   .cancel_attempt  = co_cancel_attempt,
        .dispose_fd     = co_dispose_fd,
        .arm_delay      = co_arm_delay,       .cancel_delay    = co_cancel_delay,
        .arm_deadline   = co_arm_deadline,    .cancel_deadline = co_cancel_deadline,
        .on_done        = co_on_done,         .on_detach       = co_on_detach,
    };
    kl_connect_op_init(op, &h, m);
}

/* ── Tests ─────────────────────────────────────────────────────────────────────────────────── */

UTEST(connect_op, sync_resolve_sync_connect_single_addr) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 1;
    m.attempt_mode[0] = M_SYNC_OK;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_SUCCESS);
    ASSERT_EQ(m.last_fd, 100);
    ASSERT_EQ(m.detach_calls, 1);                 /* detached after unwind (no losers, no in-flight) */
    ASSERT_EQ(kl_connect_op_is_detached(&op), 1);
    ASSERT_EQ(kl_connect_op_state(&op), KL_CONNECT_OP_STATE_DETACHED);
}

UTEST(connect_op, async_resolve_then_async_connect_success) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(kl_connect_op_state(&op), KL_CONNECT_OP_STATE_RESOLVING);
    ASSERT_EQ(m.done_calls, 0);

    kl_connect_op_on_resolved(&op, 2);            /* enter CONNECTING, start attempt 0 */
    ASSERT_EQ(kl_connect_op_state(&op), KL_CONNECT_OP_STATE_CONNECTING);
    ASSERT_EQ(m.attempt_calls[0], 1);
    ASSERT_EQ(m.arm_delay_calls, 1);              /* staggered next armed */

    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)55);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_SUCCESS);
    ASSERT_EQ(m.last_fd, 55);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, resolve_failure_is_terminal) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolve_failed(&op, 42);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_FAILED);
    ASSERT_EQ(m.last_error, 42);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, resolve_start_hardfail_is_terminal) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_HARDFAIL;                  /* start_resolve returns -1, no completion */
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_FAILED);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, all_attempts_fail_reports_last_error) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 2;
    m.attempt_mode[0] = M_SYNC_FAIL; m.attempt_err[0] = 7;
    m.attempt_mode[1] = M_SYNC_FAIL; m.attempt_err[1] = 9;   /* fast-start on failure, no delay */
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.attempt_calls[0], 1);
    ASSERT_EQ(m.attempt_calls[1], 1);
    ASSERT_EQ(m.arm_delay_calls, 0);              /* a failure must not wait out the delay */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_FAILED);
    ASSERT_EQ(m.last_error, 9);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, hard_local_fail_skips_to_next_address) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 2;
    m.attempt_mode[0] = M_HARDFAIL;               /* socket() failed; never pending */
    m.attempt_mode[1] = M_SYNC_OK;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.attempt_calls[0], 1);
    ASSERT_EQ(m.attempt_calls[1], 1);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_SUCCESS);
    ASSERT_EQ(m.last_fd, 101);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, happy_eyeballs_winner_cancels_loser_once) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);            /* attempt 0 in flight */
    kl_connect_op_on_delay(&op);                  /* stagger → attempt 1 in flight (2 pending) */
    ASSERT_EQ(m.attempt_calls[1], 1);

    kl_connect_op_on_attempt_connected(&op, 1, (KlSocketHandle)201);  /* attempt 1 WINS */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_SUCCESS);
    ASSERT_EQ(m.last_fd, 201);
    ASSERT_EQ(m.attempt_cancel_calls[0], 1);      /* loser cancel-requested exactly once */
    ASSERT_EQ(m.attempt_cancel_calls[1], 0);      /* winner never cancelled */
    ASSERT_EQ(kl_connect_op_is_detached(&op), 0); /* NOT detached; loser still outstanding */

    kl_connect_op_on_attempt_failed(&op, 0, 0);   /* loser's cancellation completes */
    ASSERT_EQ(m.detach_calls, 1);                 /* now fully retired → detached */
    ASSERT_EQ(m.done_calls, 1);                   /* terminal still exactly once */
}

UTEST(connect_op, straggler_connect_after_win_does_not_refire) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);
    kl_connect_op_on_delay(&op);                  /* both pending */

    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)300);  /* 0 wins */
    ASSERT_EQ(m.done_calls, 1);
    kl_connect_op_on_attempt_connected(&op, 1, (KlSocketHandle)301);  /* straggler also connects */
    ASSERT_EQ(m.done_calls, 1);                   /* terminal NOT re-fired */
    ASSERT_EQ(m.last_fd, 300);                    /* winner unchanged */
    ASSERT_EQ(m.detach_calls, 1);                 /* straggler retired → detached */
}

UTEST(connect_op, deadline_fails_and_cancels_all_pending) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);
    kl_connect_op_on_delay(&op);                  /* 2 pending */

    kl_connect_op_on_deadline(&op, 13);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_FAILED);
    ASSERT_EQ(m.last_error, 13);
    ASSERT_EQ(m.attempt_cancel_calls[0], 1);
    ASSERT_EQ(m.attempt_cancel_calls[1], 1);
    ASSERT_EQ(kl_connect_op_is_detached(&op), 0); /* wait for both cancellations to complete */

    kl_connect_op_on_attempt_failed(&op, 0, 0);
    ASSERT_EQ(m.detach_calls, 0);
    kl_connect_op_on_attempt_failed(&op, 1, 0);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, cancel_while_resolving) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(kl_connect_op_cancel(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_CANCELLED);
    ASSERT_EQ(m.resolve_cancel_calls, 1);         /* resolve cancel-requested once */
    ASSERT_EQ(kl_connect_op_is_detached(&op), 0); /* resolve op still physically outstanding */

    kl_connect_op_on_resolved(&op, 2);            /* straggler resolve completes → dropped */
    ASSERT_EQ(m.attempt_calls[0], 0);             /* never starts connecting after cancel */
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, repeat_cancel_requests_once_per_op) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);
    kl_connect_op_on_delay(&op);                  /* 2 pending */

    ASSERT_EQ(kl_connect_op_cancel(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.attempt_cancel_calls[0], 1);
    ASSERT_EQ(m.attempt_cancel_calls[1], 1);
    ASSERT_EQ(kl_connect_op_cancel(&op), 0);      /* both still outstanding; no re-request */
    ASSERT_EQ(m.attempt_cancel_calls[0], 1);
    ASSERT_EQ(m.attempt_cancel_calls[1], 1);
    ASSERT_EQ(m.done_calls, 1);                   /* terminal still once */

    kl_connect_op_on_attempt_failed(&op, 0, 0);
    kl_connect_op_on_attempt_failed(&op, 1, 0);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, sync_cancel_completion_detaches_once) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    m.attempt_mode[0] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 1);            /* attempt 0 in flight */
    m.cancel_attempt_sync_fail[0] = 1;            /* cancel completes the attempt inline */

    ASSERT_EQ(kl_connect_op_cancel(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_CANCELLED);
    ASSERT_EQ(m.detached_at_cancel, 0);           /* not detached mid-cancel */
    ASSERT_EQ(m.detach_calls, 1);                 /* detached once after unwind */
}

UTEST(connect_op, reentrant_cancel_from_hook_detaches_once) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    m.attempt_mode[0] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 1);
    m.cancel_attempt_reentrant = 1;               /* cancel_attempt reentrantly calls cancel */

    ASSERT_EQ(kl_connect_op_cancel(&op), 0);
    ASSERT_EQ(m.attempt_cancel_calls[0], 1);      /* reentrant call did NOT re-invoke the hook */
    ASSERT_EQ(m.detached_at_cancel, 0);           /* depth counter kept detachment deferred */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(kl_connect_op_is_detached(&op), 0); /* attempt still physically outstanding */

    kl_connect_op_on_attempt_failed(&op, 0, 0);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, sync_cancel_resolve_completion_detaches_once) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.resolve_err = 5;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    m.cancel_resolve_sync_fail = 1;               /* cancel_resolve completes the resolve inline */
    ASSERT_EQ(kl_connect_op_cancel(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_CANCELLED);
    ASSERT_EQ(m.resolve_cancel_calls, 1);
    ASSERT_EQ(m.detached_at_cancel, 0);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, duplicate_completions_dropped) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    m.attempt_mode[0] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 1);
    kl_connect_op_on_resolved(&op, 1);            /* duplicate resolve; dropped */
    ASSERT_EQ(m.attempt_calls[0], 1);

    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)7);
    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)8);   /* duplicate; dropped */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_fd, 7);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, many_sync_failures_bounded) {
    /* All 8 addresses hard/sync-fail inline via the fast-start chain; bounded stack. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = KL_CONNECT_MAX_ADDRS;
    for (int i = 0; i < KL_CONNECT_MAX_ADDRS; i++) { m.attempt_mode[i] = M_SYNC_FAIL; m.attempt_err[i] = i + 1; }
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    for (int i = 0; i < KL_CONNECT_MAX_ADDRS; i++) ASSERT_EQ(m.attempt_calls[i], 1);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_FAILED);
    ASSERT_EQ(m.last_error, KL_CONNECT_MAX_ADDRS);   /* last address's error */
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, resolve_naddrs_clamped) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC;
    m.attempt_mode[0] = M_SYNC_OK;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 999);          /* clamped to KL_CONNECT_MAX_ADDRS */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_SUCCESS);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, no_reuse_until_reinit) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 1; m.attempt_mode[0] = M_SYNC_OK;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(kl_connect_op_is_detached(&op), 1);
    ASSERT_EQ(kl_connect_op_start(&op), -1);       /* not IDLE; cannot restart without re-init */

    co_setup(&m, &op);                             /* re-init = reuse reset */
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 1; m.attempt_mode[0] = M_SYNC_OK;
    ASSERT_EQ(kl_connect_op_state(&op), KL_CONNECT_OP_STATE_IDLE);
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, cancel_after_success_is_idempotent) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 1; m.attempt_mode[0] = M_SYNC_OK;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.detach_calls, 1);
    ASSERT_EQ(kl_connect_op_cancel(&op), 0);       /* already detached; no-op */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, init_rejects_missing_hooks) {
    KlConnectOp op;
    KlConnectOpHooks h = { 0 };
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), -1);         /* missing required hooks */
    h.start_resolve = co_start_resolve;
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), -1);         /* still missing start_attempt/on_done */
    h.start_attempt = co_start_attempt;
    h.on_done = co_on_done;
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), -1);         /* dispose_fd required */
    h.dispose_fd = co_dispose_fd;
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), 0);
    ASSERT_EQ(kl_connect_op_init(NULL, &h, NULL), -1);
    ASSERT_EQ(kl_connect_op_init(&op, NULL, NULL), -1);
}

UTEST(connect_op, uninited_is_safe) {
    KlConnectOp op; memset(&op, 0, sizeof(op));
    ASSERT_EQ(kl_connect_op_start(&op), -1);
    ASSERT_EQ(kl_connect_op_cancel(&op), -1);
    kl_connect_op_on_resolved(&op, 1);            /* no-op, no crash */
    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)1);
    ASSERT_EQ(kl_connect_op_is_detached(&op), 0);
}

UTEST(connect_op, one_addr_async_then_fail_is_terminal) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 1);            /* attempt 0 in flight, no more addrs */
    ASSERT_EQ(m.arm_delay_calls, 0);              /* nothing to stagger */
    ASSERT_EQ(m.done_calls, 0);
    kl_connect_op_on_attempt_failed(&op, 0, 77);  /* only address failed */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_FAILED);
    ASSERT_EQ(m.last_error, 77);
    ASSERT_EQ(m.detach_calls, 1);
}

/* ── Fix 1: on_done reentrancy must not cause premature detach / UAF ──────────────────────── */

UTEST(connect_op, on_done_reentrant_cancel_no_outstanding) {
    /* on_done calls cancel with nothing else outstanding; detachment must wait for the terminal
     * dispatch to unwind, then fire exactly once. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 1; m.attempt_mode[0] = M_SYNC_OK;
    m.on_done_cancel = 1;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.detached_at_done, 0);             /* NOT detached inside on_done */
    ASSERT_EQ(m.detach_calls, 1);                 /* detached once after unwind */
}

UTEST(connect_op, on_done_reentrant_retire_final_loser) {
    /* on_done retires the final loser. Detachment must be deferred until the terminal dispatch
     * unwinds even though nothing is outstanding once the loser retires. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);
    kl_connect_op_on_delay(&op);                  /* both pending */
    m.on_done_fail = 1; m.on_done_fail_idx = 1;   /* on_done retires loser 1 */

    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)400);   /* 0 wins */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.detached_at_done, 0);
    ASSERT_EQ(m.detach_calls, 1);                 /* loser retired within on_done; detached once */
}

UTEST(connect_op, on_done_reentrant_both_paths) {
    /* on_done both cancels AND retires the final loser; the depth guard must still yield exactly
     * one detachment, never mid-dispatch. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);
    kl_connect_op_on_delay(&op);
    m.on_done_cancel = 1; m.on_done_fail = 1; m.on_done_fail_idx = 1;

    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)410);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.detached_at_done, 0);
    ASSERT_EQ(m.detach_calls, 1);
    ASSERT_EQ(kl_connect_op_is_detached(&op), 1);
}

/* ── Fix 2: timers are part of confirmed detachment ───────────────────────────────────────── */

UTEST(connect_op, timers_disarmed_on_success) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.arm_deadline_calls, 1);           /* deadline armed at start */
    kl_connect_op_on_resolved(&op, 2);
    ASSERT_EQ(m.arm_delay_calls, 1);              /* stagger armed */

    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)500);
    ASSERT_EQ(m.cancel_delay_calls, 1);           /* delay disarmed at terminal */
    ASSERT_EQ(m.cancel_deadline_calls, 1);        /* deadline disarmed at terminal */
    ASSERT_EQ(m.detach_calls, 1);                 /* timers retired → detached */
}

UTEST(connect_op, deadline_disarms_delay_and_detaches) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);            /* attempt 0 in flight, delay armed */
    ASSERT_EQ(m.arm_delay_calls, 1);

    kl_connect_op_on_deadline(&op, 13);
    ASSERT_EQ(m.cancel_delay_calls, 1);           /* the fired deadline disarms the pending delay */
    ASSERT_EQ(m.cancel_deadline_calls, 0);        /* the deadline itself fired; not cancelled */
    ASSERT_EQ(m.attempt_cancel_calls[0], 1);      /* pending attempt cancelled */
    kl_connect_op_on_attempt_failed(&op, 0, 0);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, stale_delay_event_dropped) {
    /* A duplicate/stale delay event must be consumed exactly once; it must NOT start an extra
     * address. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);            /* attempt 0 in flight; delay armed */
    kl_connect_op_on_delay(&op);                  /* consumes the delay → attempt 1 in flight */
    ASSERT_EQ(m.attempt_calls[1], 1);
    kl_connect_op_on_delay(&op);                  /* stale duplicate; dropped */
    ASSERT_EQ(m.attempt_calls[2], 0);
    ASSERT_EQ(m.attempt_calls[1], 1);
}

UTEST(connect_op, stale_deadline_event_dropped) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 1; m.attempt_mode[0] = M_SYNC_OK;
    ASSERT_EQ(kl_connect_op_start(&op), 0);       /* success → deadline disarmed, detached */
    ASSERT_EQ(m.detach_calls, 1);
    kl_connect_op_on_deadline(&op, 9);            /* stale (already disarmed); dropped, no crash */
    ASSERT_EQ(m.done_calls, 1);
}

/* ── Fix 3: straggler socket ownership ─────────────────────────────────────────────────────── */

UTEST(connect_op, straggler_fd_disposed) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);
    kl_connect_op_on_delay(&op);                  /* both pending */

    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)600);  /* winner */
    ASSERT_EQ(m.last_fd, 600);
    ASSERT_EQ(m.dispose_calls, 0);

    kl_connect_op_on_attempt_connected(&op, 1, (KlSocketHandle)601);  /* straggler connects */
    ASSERT_EQ(m.dispose_calls, 1);                /* straggler fd handed to dispose_fd */
    ASSERT_EQ(m.disposed_fd, 601);
    ASSERT_EQ(m.last_fd, 600);                    /* winner fd unchanged */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.detach_calls, 1);
}

/* ── Smaller: hard-local-fail error propagates ─────────────────────────────────────────────── */

UTEST(connect_op, hard_local_fail_error_propagates) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 2;
    m.attempt_mode[0] = M_HARDFAIL; m.attempt_err[0] = 111;
    m.attempt_mode[1] = M_HARDFAIL; m.attempt_err[1] = 222;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_FAILED);
    ASSERT_EQ(m.last_error, 222);                 /* last address's platform error, not 0 */
    ASSERT_EQ(m.detach_calls, 1);
}

/* ── init: timer hooks must be paired ──────────────────────────────────────────────────────── */

UTEST(connect_op, init_requires_paired_timer_hooks) {
    KlConnectOp op;
    KlConnectOpHooks h = {
        .start_resolve = co_start_resolve, .start_attempt = co_start_attempt, .on_done = co_on_done,
        .dispose_fd = co_dispose_fd,
        .arm_delay = co_arm_delay,   /* no cancel_delay */
    };
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), -1);
    h.cancel_delay = co_cancel_delay;
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), 0);
    h.arm_deadline = co_arm_deadline;   /* no cancel_deadline */
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), -1);
    h.cancel_deadline = co_cancel_deadline;
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), 0);
}

UTEST(connect_op, init_requires_dispose_fd) {
    KlConnectOp op;
    KlConnectOpHooks h = {
        .start_resolve = co_start_resolve, .start_attempt = co_start_attempt, .on_done = co_on_done,
    };
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), -1);   /* dispose_fd is mandatory */
    h.dispose_fd = co_dispose_fd;
    ASSERT_EQ(kl_connect_op_init(&op, &h, NULL), 0);
}

/* ── Fix (round 2): timer arm failure + inline firing ──────────────────────────────────────── */

UTEST(connect_op, deadline_arm_failure_fails_connect) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.arm_deadline_fail = 1; m.arm_deadline_err = 55;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.arm_deadline_calls, 1);
    ASSERT_EQ(m.resolve_calls, 0);                /* resolve never started */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_FAILED);
    ASSERT_EQ(m.last_error, 55);
    ASSERT_EQ(m.detach_calls, 1);
    ASSERT_EQ(m.cancel_deadline_calls, 0);        /* nothing to cancel; never armed */
}

UTEST(connect_op, delay_arm_failure_fast_starts_next) {
    /* arm_delay fails → the machine must fast-start the next address rather than stall. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 2;
    m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    m.arm_delay_fail = 1;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.attempt_calls[0], 1);
    ASSERT_EQ(m.attempt_calls[1], 1);             /* next address started immediately (no stagger) */
    ASSERT_EQ(kl_connect_op_is_detached(&op), 0); /* both in flight */
    ASSERT_EQ(op.delay_armed, 0);                 /* no phantom armed timer */

    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)700);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.attempt_cancel_calls[1], 1);      /* the eagerly-started loser is cancelled */
    kl_connect_op_on_attempt_failed(&op, 1, 0);
    ASSERT_EQ(m.detach_calls, 1);
}

UTEST(connect_op, delay_inline_fire_then_success_return) {
    /* arm_delay fires on_delay inline, then returns 0. The inline fire is consumed exactly once and
     * starts the next address; no phantom armed timer remains. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 2;
    m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    m.arm_delay_inline_fire = 1;                  /* fires inline, returns 0 */
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.attempt_calls[0], 1);
    ASSERT_EQ(m.attempt_calls[1], 1);             /* inline delay started address 1 */
    ASSERT_EQ(op.delay_armed, 0);                 /* consumed; not left armed */
    ASSERT_EQ(kl_connect_op_is_detached(&op), 0);
}

UTEST(connect_op, delay_inline_fire_then_error_return) {
    /* arm_delay fires inline AND returns -1: the inline consumption must win (return ignored), so
     * no spurious fast-start beyond what the inline fire did, and no phantom timer. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_SYNC_OK; m.resolve_naddrs = 2;
    m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    m.arm_delay_inline_fire = 1; m.arm_delay_fail = 1;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.attempt_calls[0], 1);
    ASSERT_EQ(m.attempt_calls[1], 1);
    ASSERT_EQ(m.attempt_calls[2], 0);            /* only 2 addresses; no over-start */
    ASSERT_EQ(op.delay_armed, 0);
}

UTEST(connect_op, deadline_inline_fire_at_start) {
    /* arm_deadline fires on_deadline inline (e.g. a 0ms deadline): the op fails terminally without
     * ever starting the resolve, detaches once, no phantom timer. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.arm_deadline_inline_fire = 1; m.arm_deadline_err = 88;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(m.last_result, KL_CONNECT_FAILED);
    ASSERT_EQ(m.last_error, 88);
    ASSERT_EQ(m.resolve_calls, 0);               /* deadline fired before resolve started */
    ASSERT_EQ(op.deadline_armed, 0);
    ASSERT_EQ(m.detach_calls, 1);
}

/* ── Fix (round 2): total descriptor disposal ──────────────────────────────────────────────── */

UTEST(connect_op, connected_out_of_range_index_disposed) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 1);
    kl_connect_op_on_attempt_connected(&op, 99, (KlSocketHandle)900);  /* out-of-range */
    ASSERT_EQ(m.dispose_calls, 1);
    ASSERT_EQ(m.disposed_fd, 900);
    ASSERT_EQ(m.done_calls, 0);                   /* not accepted as winner */
}

UTEST(connect_op, duplicate_connected_completion_disposed) {
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 1);
    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)910);  /* winner */
    ASSERT_EQ(m.last_fd, 910);
    ASSERT_EQ(m.dispose_calls, 0);
    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)911);  /* duplicate on inactive slot */
    ASSERT_EQ(m.dispose_calls, 1);               /* the duplicate's fd is disposed, not leaked */
    ASSERT_EQ(m.disposed_fd, 911);
    ASSERT_EQ(m.done_calls, 1);
}

UTEST(connect_op, dispose_reentrant_cancel_defers_detach) {
    /* Disposal of the FINAL straggler reentrantly cancels: detachment must not fire inside the
     * dispose_fd hook (guarded destructive tail), only after it returns. */
    KlConnectOp op; CO m; co_setup(&m, &op);
    m.resolve_mode = M_ASYNC; m.attempt_mode[0] = M_ASYNC; m.attempt_mode[1] = M_ASYNC;
    ASSERT_EQ(kl_connect_op_start(&op), 0);
    kl_connect_op_on_resolved(&op, 2);
    kl_connect_op_on_delay(&op);                  /* both pending */

    kl_connect_op_on_attempt_connected(&op, 0, (KlSocketHandle)800);  /* 0 wins; loser 1 pending */
    ASSERT_EQ(m.done_calls, 1);
    ASSERT_EQ(kl_connect_op_is_detached(&op), 0); /* loser 1 still outstanding */

    m.dispose_reentrant_cancel = 1;
    kl_connect_op_on_attempt_connected(&op, 1, (KlSocketHandle)801);  /* final straggler connects */
    ASSERT_EQ(m.dispose_calls, 1);
    ASSERT_EQ(m.disposed_fd, 801);
    ASSERT_EQ(m.detached_at_dispose, 0);          /* NOT detached inside the disposal hook */
    ASSERT_EQ(m.detach_calls, 1);                 /* detached once, after disposal returned */
    ASSERT_EQ(m.done_calls, 1);                   /* terminal still exactly once */
}

UTEST_MAIN();
