/*
 * connect_op.h — INTERNAL. Phase-B outbound-connect terminal-once state machine (step 4).
 *
 * A model-agnostic state machine for establishing one outbound connection: name resolution
 * followed by Happy Eyeballs (RFC 8305) racing connect over the resolved address list. It applies
 * the same discipline as the KlStream read/close machinery (steps 2B/3):
 *
 *   - TERMINAL EXACTLY ONCE. on_done fires once with SUCCESS (the winning fd), FAILED (an error),
 *     or CANCELLED — never twice, never after a prior terminal.
 *   - SYNCHRONOUS-COMPLETION SAFETY. The resolver contract allows resolve() to call its done_fn
 *     synchronously (see resolver.h), and a connect attempt may complete inline (loopback). The
 *     start hooks may therefore drive on_resolved / on_attempt_connected / on_attempt_failed from
 *     inside the hook. recv-style sentinels (an in_start depth counter) bound the C stack and stop
 *     an inline completion from firing detachment while a start hook is still on the stack.
 *   - CANCELLATION REQUESTED ONCE PER ACTIVE OPERATION. The resolve and each racing attempt are
 *     cancel-requested at most once (resolve_cancel_requested / attempt_cancel_requested[]); the
 *     flag is set before the hook (so a synchronous retirement is safe) and reset only when a new
 *     op of that kind starts. in_cancel is a DEPTH counter so a reentrant cancel is safe.
 *   - NO REUSE UNTIL CONFIRMED DETACHMENT. on_detach fires only after the terminal AND every
 *     outstanding operation is physically retired — the resolve (resolve_inflight == 0), all
 *     racing attempts (pending == 0), AND both timers (delay/deadline disarmed). The timers are
 *     tracked as first-class resources: they are synchronously disarmed at terminal (the
 *     cancel_delay/cancel_deadline hooks must retire immediately, as KEEL kl_timer_cancel does),
 *     so a stale timer can never fire into a freed/re-inited op. Reuse (or free) is legal only
 *     after on_detach; re-init with kl_connect_op_init() (which zeroes all state) is the reset.
 *
 * Socket ownership: the WINNING descriptor transfers exactly once through on_done; EVERY other
 * connected descriptor — straggler, duplicate/spurious completion, or out-of-range index — is
 * handed to the (required) dispose_fd hook for the adapter to close. No descriptor is leaked.
 *
 * Timer arm failure: arm_delay/arm_deadline return status. The armed flag is set BEFORE the hook
 * and the arm frame is guarded against detachment, so an inline firing is detected (and its
 * return ignored). A deadline that cannot be armed FAILS the connect (a deadline is essential); a
 * delay that cannot be armed degrades the stagger — the machine fast-starts the next address.
 *
 * The op owns NO timers, sockets, or event loop: the adapter arms the Connection Attempt Delay
 * and the overall deadline (via the arm_delay/arm_deadline hooks) and drives the on_* entry
 * points. Kept internal until the Phase-B public surface is finalized.
 */
#ifndef KEEL_SRC_CONNECT_OP_H
#define KEEL_SRC_CONNECT_OP_H

#include <keel/handle.h>   /* KlSocketHandle, KL_INVALID_SOCKET */

/** Max racing addresses (matches KL_RESOLVE_MAX_ADDRS without coupling to resolver.h). */
#define KL_CONNECT_MAX_ADDRS 8

/* Lifecycle phase (KlConnectOp.state). */
enum {
    KL_CONNECT_STATE_IDLE = 0,   /* not started */
    KL_CONNECT_STATE_RESOLVING,  /* a name resolution is in flight */
    KL_CONNECT_STATE_CONNECTING, /* racing connect over the resolved list */
    KL_CONNECT_STATE_DONE,       /* terminal decided (on_done fired) */
    KL_CONNECT_STATE_DETACHED    /* all ops retired; on_detach fired; reuse legal */
};

/** Terminal result. */
typedef enum {
    KL_CONNECT_SUCCESS = 0,   /* a connect attempt won; fd is the connected socket */
    KL_CONNECT_FAILED,        /* resolve failed / all attempts failed / deadline */
    KL_CONNECT_CANCELLED      /* kl_connect_op_cancel() before a natural terminal */
} KlConnectResult;

/* ── Adapter hooks ─────────────────────────────────────────────────────────────────────────── */

/* Begin name resolution. May complete synchronously (call kl_connect_op_on_resolved /
 * on_resolve_failed inside the call). Returns 0 if a resolve is now in flight (or completed
 * inline), -1 if it could not even be started (→ terminal FAILED). */
typedef int (*KlConnectResolveFn)(void *ctx);

/* Request cancellation of the in-flight resolve (invoked at most once per resolve op). */
typedef void (*KlConnectResolveCancelFn)(void *ctx);

/* Begin one connect attempt to address index `idx`. May complete synchronously (call
 * kl_connect_op_on_attempt_connected / on_attempt_failed inside the call). Returns 0 if an
 * attempt is now in flight (or completed inline), -1 on a HARD LOCAL failure that never became
 * pending (e.g. socket() failed) — the machine advances to the next address immediately. On -1
 * the hook MUST write the platform error to *out_err (reported if the whole list is exhausted). */
typedef int (*KlConnectAttemptFn)(void *ctx, int idx, int *out_err);

/* Request cancellation of racing attempt `idx` (invoked at most once per attempt op). */
typedef void (*KlConnectAttemptCancelFn)(void *ctx, int idx);

/* Dispose of a connected socket that will NOT be the winner. The op transfers the WINNING fd
 * exactly once through on_done; EVERY other connected fd — a straggler (a loser that connects
 * after the race is decided by a winner/failure/cancel/deadline), a duplicate/spurious connected
 * completion, or an out-of-range index — is handed here for the adapter to close. REQUIRED: a
 * losing attempt can always connect late, so descriptor ownership must never be dropped. */
typedef void (*KlConnectDisposeFn)(void *ctx, KlSocketHandle fd);

/* Arm the Connection Attempt Delay after a new attempt starts (RFC 8305 §5 stagger). The adapter
 * calls kl_connect_op_on_delay() when it fires. Returns 0 if armed, -1 if the timer could not be
 * armed (e.g. kl_timer_add() alloc failure) — the machine then fast-starts the next address
 * immediately (the stagger is best-effort). The hook MAY fire the delay synchronously (call
 * kl_connect_op_on_delay inside it); that inline consumption is detected and the return is
 * ignored. Optional (NULL disables staggering); if set, cancel_delay MUST also be set. */
typedef int (*KlConnectArmDelayFn)(void *ctx);
/* Disarm the Connection Attempt Delay. MUST synchronously retire the timer (KEEL kl_timer_cancel
 * does): after this returns no delay callback will fire. Required iff arm_delay is set. */
typedef void (*KlConnectCancelDelayFn)(void *ctx);

/* Arm the overall connect deadline at start. The adapter calls kl_connect_op_on_deadline() when
 * it fires. Returns 0 if armed, -1 if it could not be armed — a deadline is essential, so an
 * arm failure FAILS the connect (terminal FAILED with *out_err). The hook MAY fire the deadline
 * synchronously; that inline consumption is detected and the return is ignored. Optional; if set,
 * cancel_deadline MUST also be set. */
typedef int (*KlConnectArmDeadlineFn)(void *ctx, int *out_err);
/* Disarm the overall deadline. MUST synchronously retire the timer. Required iff arm_deadline. */
typedef void (*KlConnectCancelDeadlineFn)(void *ctx);

/* Terminal callback — fires EXACTLY ONCE. On SUCCESS, `fd` is the winning socket (ownership
 * transfers to the callback) and `error` 0; otherwise `fd` is KL_INVALID_SOCKET and `error`
 * carries the failure code. The op is NOT yet reusable here — wait for on_detach. It MAY
 * reentrantly cancel/retire; detachment is deferred until the terminal dispatch unwinds. */
typedef void (*KlConnectDoneFn)(void *ctx, KlConnectResult result, KlSocketHandle fd, int error);

/* Detachment callback — fires EXACTLY ONCE after the terminal AND every outstanding op AND both
 * timers are retired. Reuse/free of the op is legal only after this returns. */
typedef void (*KlConnectDetachFn)(void *ctx);

typedef struct {
    KlConnectResolveFn        start_resolve;    /* required */
    KlConnectResolveCancelFn  cancel_resolve;   /* optional */
    KlConnectAttemptFn        start_attempt;    /* required */
    KlConnectAttemptCancelFn  cancel_attempt;   /* optional */
    KlConnectDisposeFn        dispose_fd;       /* required (owner of every non-winner socket) */
    KlConnectArmDelayFn       arm_delay;         /* optional (pairs with cancel_delay) */
    KlConnectCancelDelayFn    cancel_delay;      /* required iff arm_delay */
    KlConnectArmDeadlineFn    arm_deadline;      /* optional (pairs with cancel_deadline) */
    KlConnectCancelDeadlineFn cancel_deadline;   /* required iff arm_deadline */
    KlConnectDoneFn           on_done;           /* required */
    KlConnectDetachFn         on_detach;         /* optional */
} KlConnectOpHooks;

/* ── The op ────────────────────────────────────────────────────────────────────────────────── */

typedef struct KlConnectOp {
    int  state;               /* KL_CONNECT_STATE_* */
    int  inited;              /* 1 once kl_connect_op_init ran */
    int  terminal;            /* on_done fired (exactly-once guard) */
    int  detached;            /* on_detach fired (exactly-once guard) */
    KlConnectResult result;   /* terminal result */
    KlSocketHandle  win_fd;   /* winning fd on SUCCESS, else KL_INVALID_SOCKET */
    int  error;               /* terminal error code (0 on success) */
    int  last_error;          /* most recent failure code (reported on exhaustion) */

    /* resolve op */
    int  resolve_inflight;
    int  resolve_cancel_requested;

    /* racing connect attempts */
    int  naddrs;              /* usable addresses after resolve (<= KL_CONNECT_MAX_ADDRS) */
    int  next_idx;            /* cursor: next address to try */
    int  pending;             /* active attempts count */
    unsigned char attempt_active[KL_CONNECT_MAX_ADDRS];
    unsigned char attempt_cancel_requested[KL_CONNECT_MAX_ADDRS];

    /* timers as first-class outstanding resources (retired synchronously at terminal) */
    int  delay_armed;         /* the Connection Attempt Delay is armed */
    int  deadline_armed;      /* the overall deadline is armed */

    /* sync-completion / reentrancy sentinels */
    int  in_start;            /* DEPTH: inside a start hook — defer detachment (reuse-after-detach) */
    int  in_dispatch;         /* DEPTH: inside the terminal dispatch (on_done + cancels) — defer detach */

    KlConnectResolveFn        start_resolve;
    KlConnectResolveCancelFn  cancel_resolve;
    KlConnectAttemptFn        start_attempt;
    KlConnectAttemptCancelFn  cancel_attempt;
    KlConnectDisposeFn        dispose_fd;
    KlConnectArmDelayFn       arm_delay;
    KlConnectCancelDelayFn    cancel_delay;
    KlConnectArmDeadlineFn    arm_deadline;
    KlConnectCancelDeadlineFn cancel_deadline;
    KlConnectDoneFn           on_done;
    KlConnectDetachFn         on_detach;
    void *ctx;
} KlConnectOp;

/* Install hooks and reset all state (zeroes the op). Requires start_resolve, start_attempt,
 * on_done. Returns 0, or -1 on a NULL/missing-required-hook. Re-init is the reuse reset. */
int kl_connect_op_init(KlConnectOp *op, const KlConnectOpHooks *hooks, void *ctx);

/* Begin: enter RESOLVING and start the resolve. Returns 0, or -1 if not inited / not IDLE. */
int kl_connect_op_start(KlConnectOp *op);

/* Resolution completed with `naddrs` addresses (>= 1 on success; clamped to KL_CONNECT_MAX_ADDRS):
 * enter CONNECTING and begin racing. A duplicate/spurious call (no resolve in flight) is dropped. */
void kl_connect_op_on_resolved(KlConnectOp *op, int naddrs);

/* Resolution failed with `error`: terminal FAILED (unless already terminal). Dropped if no
 * resolve is in flight. */
void kl_connect_op_on_resolve_failed(KlConnectOp *op, int error);

/* Racing attempt `idx` connected with `fd`: the first to do so WINS (terminal SUCCESS) and the
 * losers are cancel-requested; a later straggler simply retires. Dropped if the attempt is not
 * active (duplicate/spurious). */
void kl_connect_op_on_attempt_connected(KlConnectOp *op, int idx, KlSocketHandle fd);

/* Racing attempt `idx` failed with `error`: fast-start the next address (§5, no delay), or
 * terminal FAILED when the list is exhausted and nothing is pending. A straggler after a terminal
 * just retires. Dropped if the attempt is not active. */
void kl_connect_op_on_attempt_failed(KlConnectOp *op, int idx, int error);

/* The Connection Attempt Delay fired: start the next address if still racing. Consumed EXACTLY
 * ONCE — a stale/duplicate delay event (the timer was already disarmed or fired) is dropped and
 * does NOT start an extra address. */
void kl_connect_op_on_delay(KlConnectOp *op);

/* The overall deadline fired: terminal FAILED with `error` and cancel every outstanding op.
 * Consumed exactly once (a stale/duplicate deadline event is dropped). */
void kl_connect_op_on_deadline(KlConnectOp *op, int error);

/* Request an abortive cancel: terminal CANCELLED (if not already terminal) and cancel every
 * outstanding op — once each. Safe to call reentrantly from a hook. Idempotent once detached.
 * Returns 0, or -1 if not inited. */
int kl_connect_op_cancel(KlConnectOp *op);

/* Current lifecycle phase (KL_CONNECT_STATE_*). */
int kl_connect_op_state(const KlConnectOp *op);

/* 1 once on_detach has fired (fully retired; reusable), else 0. */
int kl_connect_op_is_detached(const KlConnectOp *op);

#endif /* KEEL_SRC_CONNECT_OP_H */
