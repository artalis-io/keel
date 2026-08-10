/*
 * connect_op.c — Phase-B outbound-connect terminal-once state machine (step 4). See connect_op.h.
 *
 * Structure mirrors the KlStream read/close discipline: the terminal decision (on_done) is
 * immediate and exactly-once; DETACHMENT (on_detach) is deferred until every outstanding op is
 * physically retired AND no start/cancel hook is still on the stack (so a synchronous inline
 * completion cannot detach — and let the owner free — mid-unwind). Cancellation is one request
 * per active op, guarded by per-op flags set BEFORE the hook; in_start / in_cancel are depth
 * counters so nested synchronous completions and reentrant cancels are bounded and safe.
 */
#include "connect_op.h"
#include <string.h>

/* Detach if the terminal has fired and NOTHING is outstanding — deferred while a start hook or the
 * terminal dispatch is on the stack (in_start / in_dispatch > 0), which the outermost frame
 * re-attempts on unwind. Timers count as outstanding until synchronously disarmed. Exactly-once
 * via `detached`. */
static void co_finalize(KlConnectOp *op) {
    if (!op->terminal) return;
    if (op->in_start || op->in_dispatch) return; /* defer past a start / terminal-dispatch frame */
    if (op->resolve_inflight) return;            /* resolve op still physically outstanding */
    if (op->pending) return;                     /* a racing attempt is still outstanding */
    if (op->delay_armed || op->deadline_armed) return; /* a timer could still fire into this op */
    if (op->detached) return;
    op->state    = KL_CONNECT_OP_STATE_DETACHED;
    op->detached = 1;
    if (op->on_detach) op->on_detach(op->ctx);   /* reuse/free legal only after this returns */
}

/* Synchronously disarm both timers (KEEL kl_timer_cancel retires immediately, so after this no
 * delay/deadline callback can fire into this op). Idempotent via the armed flags. */
static void co_disarm_timers(KlConnectOp *op) {
    if (op->delay_armed && op->cancel_delay) op->cancel_delay(op->ctx);
    op->delay_armed = 0;
    if (op->deadline_armed && op->cancel_deadline) op->cancel_deadline(op->ctx);
    op->deadline_armed = 0;
}

/* Guarded destructive tail for disposing a non-winner descriptor: dispose_fd is an adapter
 * callback that MAY reentrantly cancel/retire the op, so it runs under the in_dispatch depth guard
 * (detachment deferred) and finalization happens AFTER it returns. Callers must `return`
 * immediately — the op may be detached (and freed by on_detach) by the time this returns. */
static void co_dispose(KlConnectOp *op, KlSocketHandle fd) {
    op->in_dispatch++;
    op->dispose_fd(op->ctx, fd);      /* may reenter cancel/retire — cannot detach mid-hook */
    op->in_dispatch--;
    co_finalize(op);                  /* now safe to detach if fully retired */
}

/* Request cancellation of every still-outstanding op — AT MOST ONCE each (set-before-invoke so a
 * synchronous retirement is safe). in_dispatch is a depth counter so a reentrant kl_connect_op_cancel
 * from a hook keeps finalization deferred until the outermost frame unwinds. */
static void co_request_cancels(KlConnectOp *op) {
    op->in_dispatch++;
    if (op->resolve_inflight && op->cancel_resolve && !op->resolve_cancel_requested) {
        op->resolve_cancel_requested = 1;
        op->cancel_resolve(op->ctx);
    }
    for (int i = 0; i < KL_CONNECT_MAX_ADDRS; i++) {
        if (op->attempt_active[i] && op->cancel_attempt && !op->attempt_cancel_requested[i]) {
            op->attempt_cancel_requested[i] = 1;
            op->cancel_attempt(op->ctx, i);
        }
    }
    op->in_dispatch--;
}

/* Fire the terminal EXACTLY ONCE. The ENTIRE terminal dispatch — timer disarm, on_done, and the
 * cancellation requests — runs under the in_dispatch depth guard, so a reentrant cancel/retire
 * from on_done (or a synchronous cancellation completion) cannot fire on_detach and let the owner
 * free the op while this frame is still touching it. Detachment is finalized by the public entry
 * point that reached here, once in_start/in_dispatch unwind. */
static void co_terminal(KlConnectOp *op, KlConnectResult result, KlSocketHandle fd, int error) {
    if (op->terminal) return;
    op->terminal = 1;
    op->state    = KL_CONNECT_OP_STATE_DONE;
    op->result   = result;
    op->win_fd   = fd;
    op->error    = error;
    op->in_dispatch++;                        /* guard the whole terminal dispatch */
    co_disarm_timers(op);                     /* no delay/deadline can fire after terminal */
    if (op->on_done) op->on_done(op->ctx, result, fd, error);   /* exactly once; may reenter */
    co_request_cancels(op);                   /* abort losers / resolve (idempotent per op) */
    op->in_dispatch--;
}

/* Start one attempt to address `idx`. Marks it active (and clears its cancel-request — a genuinely
 * new op) BEFORE the hook so a synchronous on_attempt_* sees a consistent state. Returns 0 if the
 * attempt is now in flight or was decided inline, -1 on a hard local failure that never became
 * pending (advance to the next address). */
static int co_start_one(KlConnectOp *op, int idx) {
    op->attempt_active[idx]           = 1;
    op->attempt_cancel_requested[idx] = 0;   /* new op — not yet cancel-requested */
    op->pending++;
    op->in_start++;
    int err = 0;
    int r = op->start_attempt(op->ctx, idx, &err);   /* may sync-complete via on_attempt_* */
    op->in_start--;
    if (!op->attempt_active[idx])              /* decided inline (connected/failed) */
        return 0;
    if (r < 0) {                               /* hard local failure — never pending */
        op->attempt_active[idx] = 0;
        op->pending--;
        op->last_error = err;                  /* carry the platform error for exhaustion reporting */
        return -1;
    }
    return 0;                                   /* in flight — await the completion */
}

/* Arm the Connection Attempt Delay with the same discipline as a start hook: mark armed BEFORE the
 * hook so an inline firing (on_delay called from inside arm_delay) is seen and consumes it; guard
 * the arm frame against detachment (in_start); detect inline consumption before interpreting the
 * return. Returns 0 (armed or consumed inline), -1 (arm failed — no timer exists). */
static int co_arm_delay(KlConnectOp *op) {
    op->delay_armed = 1;                      /* mark before the hook (inline fire must be seen) */
    op->in_start++;
    int r = op->arm_delay(op->ctx);           /* may fire inline → on_delay */
    op->in_start--;
    if (!op->delay_armed) return 0;           /* consumed inline — return value is moot */
    if (r < 0) { op->delay_armed = 0; return -1; }   /* arm failed — no timer armed */
    return 0;
}

/* Advance the cursor and start the next viable address; skip hard local failures without consuming
 * the delay (§5). Stagger the following address via arm_delay; if the delay cannot be armed,
 * fast-start the next address immediately (best-effort stagger). Terminal FAILED when the list is
 * exhausted and nothing is pending. */
static void co_start_next(KlConnectOp *op) {
    while (op->state == KL_CONNECT_OP_STATE_CONNECTING && !op->terminal &&
           op->next_idx < op->naddrs) {
        int idx = op->next_idx++;
        if (co_start_one(op, idx) == 0) {
            if (op->arm_delay && op->state == KL_CONNECT_OP_STATE_CONNECTING && !op->terminal &&
                op->next_idx < op->naddrs && !op->delay_armed) {
                if (co_arm_delay(op) < 0)
                    continue;                  /* delay-arm failed — fast-start the next address now */
            }
            return;
        }
        /* hard local fail — try the next address immediately (no delay) */
    }
    if (op->state == KL_CONNECT_OP_STATE_CONNECTING && !op->terminal && op->pending == 0)
        co_terminal(op, KL_CONNECT_FAILED, KL_INVALID_SOCKET, op->last_error);
}

/* Arm the overall deadline with the start-hook discipline (see co_arm_delay). Returns 0 (armed or
 * fired inline), -1 (arm failed — *out_err set). */
static int co_arm_deadline(KlConnectOp *op, int *out_err) {
    if (!op->arm_deadline) return 0;          /* no deadline configured */
    op->deadline_armed = 1;                   /* mark before the hook */
    op->in_start++;
    int err = 0;
    int r = op->arm_deadline(op->ctx, &err);  /* may fire inline → on_deadline */
    op->in_start--;
    if (!op->deadline_armed) return 0;        /* consumed inline — return value moot */
    if (r < 0) { op->deadline_armed = 0; *out_err = err; return -1; }
    return 0;
}

int kl_connect_op_init(KlConnectOp *op, const KlConnectOpHooks *hooks, void *ctx) {
    if (!op || !hooks) return -1;
    if (!hooks->start_resolve || !hooks->start_attempt || !hooks->on_done) return -1;
    if (!hooks->dispose_fd) return -1;         /* every non-winner descriptor must have an owner */
    if (hooks->arm_delay    && !hooks->cancel_delay)    return -1;  /* timer must be disarmable */
    if (hooks->arm_deadline && !hooks->cancel_deadline) return -1;
    memset(op, 0, sizeof(*op));
    op->start_resolve   = hooks->start_resolve;
    op->cancel_resolve  = hooks->cancel_resolve;
    op->start_attempt   = hooks->start_attempt;
    op->cancel_attempt  = hooks->cancel_attempt;
    op->dispose_fd      = hooks->dispose_fd;
    op->arm_delay       = hooks->arm_delay;
    op->cancel_delay    = hooks->cancel_delay;
    op->arm_deadline    = hooks->arm_deadline;
    op->cancel_deadline = hooks->cancel_deadline;
    op->on_done         = hooks->on_done;
    op->on_detach       = hooks->on_detach;
    op->ctx             = ctx;
    op->win_fd          = KL_INVALID_SOCKET;
    op->state           = KL_CONNECT_OP_STATE_IDLE;
    op->inited          = 1;
    return 0;
}

int kl_connect_op_start(KlConnectOp *op) {
    if (!op || !op->inited) return -1;
    if (op->state != KL_CONNECT_OP_STATE_IDLE) return -1;
    op->state            = KL_CONNECT_OP_STATE_RESOLVING;
    op->resolve_cancel_requested = 0;

    /* Arm the deadline BEFORE marking the resolve outstanding: if it fires inline (a 0ms deadline)
     * the op becomes terminal without a phantom resolve op (which would block detachment / trigger
     * a cancel of a resolve that never started). */
    int derr = 0;
    if (co_arm_deadline(op, &derr) < 0) { /* deadline is essential — arm failure fails the connect */
        co_terminal(op, KL_CONNECT_FAILED, KL_INVALID_SOCKET, derr);
        co_finalize(op);
        return 0;
    }
    if (op->terminal) {                   /* deadline fired inline — already terminal, no resolve */
        co_finalize(op);
        return 0;
    }

    op->resolve_inflight = 1;             /* now the resolve becomes an outstanding op */
    op->in_start++;
    int r = op->start_resolve(op->ctx);   /* may sync-complete via on_resolved/on_resolve_failed */
    op->in_start--;
    if (op->resolve_inflight && r < 0) {  /* could not start and no inline completion */
        op->resolve_inflight = 0;
        co_terminal(op, KL_CONNECT_FAILED, KL_INVALID_SOCKET, op->last_error);
    }
    co_finalize(op);
    return 0;
}

void kl_connect_op_on_resolved(KlConnectOp *op, int naddrs) {
    if (!op || !op->inited) return;
    if (!op->resolve_inflight) return;            /* duplicate/spurious — drop */
    op->resolve_inflight = 0;
    if (op->terminal) { co_finalize(op); return; }/* already cancelled/failed — resolve op retired */
    if (naddrs < 0) naddrs = 0;
    if (naddrs > KL_CONNECT_MAX_ADDRS) naddrs = KL_CONNECT_MAX_ADDRS;
    op->naddrs   = naddrs;
    op->next_idx = 0;
    op->state    = KL_CONNECT_OP_STATE_CONNECTING;
    if (naddrs == 0)
        co_terminal(op, KL_CONNECT_FAILED, KL_INVALID_SOCKET, op->last_error);
    else
        co_start_next(op);
    co_finalize(op);
}

void kl_connect_op_on_resolve_failed(KlConnectOp *op, int error) {
    if (!op || !op->inited) return;
    if (!op->resolve_inflight) return;
    op->resolve_inflight = 0;
    op->last_error = error;
    if (!op->terminal)
        co_terminal(op, KL_CONNECT_FAILED, KL_INVALID_SOCKET, error);
    co_finalize(op);
}

void kl_connect_op_on_attempt_connected(KlConnectOp *op, int idx, KlSocketHandle fd) {
    if (!op || !op->inited) return;
    /* EVERY connected descriptor that is not accepted as the unique winner is routed to dispose_fd
     * (required at init) — an out-of-range index, a duplicate/spurious completion for an inactive
     * slot, and a post-terminal straggler all carry a real socket that must be closed. */
    if (idx < 0 || idx >= KL_CONNECT_MAX_ADDRS) { co_dispose(op, fd); return; }
    if (!op->attempt_active[idx])               { co_dispose(op, fd); return; }  /* dup/spurious */
    op->attempt_active[idx] = 0;                  /* this attempt retired; clear BEFORE cancels */
    op->pending--;
    if (op->terminal) {                           /* the race was already decided — this is a straggler */
        co_dispose(op, fd);                       /* guarded close of the orphan socket, then finalize */
        return;
    }
    co_terminal(op, KL_CONNECT_SUCCESS, fd, 0);   /* winner: fd transfers through on_done exactly once */
    co_finalize(op);
}

void kl_connect_op_on_attempt_failed(KlConnectOp *op, int idx, int error) {
    if (!op || !op->inited) return;
    if (idx < 0 || idx >= KL_CONNECT_MAX_ADDRS) return;
    if (!op->attempt_active[idx]) return;
    op->attempt_active[idx] = 0;
    op->pending--;
    op->last_error = error;
    if (!op->terminal) {
        if (op->next_idx < op->naddrs)
            co_start_next(op);                    /* fast-start the next address (§5) */
        else if (op->pending == 0)
            co_terminal(op, KL_CONNECT_FAILED, KL_INVALID_SOCKET, error);
        /* else: other attempts still racing — wait */
    }
    co_finalize(op);
}

void kl_connect_op_on_delay(KlConnectOp *op) {
    if (!op || !op->inited) return;
    if (!op->delay_armed) return;                 /* stale/duplicate — consume exactly once */
    op->delay_armed = 0;
    if (op->state == KL_CONNECT_OP_STATE_CONNECTING && !op->terminal)
        co_start_next(op);                        /* may re-arm the delay for the following address */
    co_finalize(op);
}

void kl_connect_op_on_deadline(KlConnectOp *op, int error) {
    if (!op || !op->inited) return;
    if (!op->deadline_armed) return;              /* stale/duplicate — consume exactly once */
    op->deadline_armed = 0;
    if (!op->terminal)
        co_terminal(op, KL_CONNECT_FAILED, KL_INVALID_SOCKET, error);
    co_finalize(op);
}

int kl_connect_op_cancel(KlConnectOp *op) {
    if (!op || !op->inited) return -1;
    if (op->detached) return 0;                   /* idempotent */
    if (!op->terminal)
        co_terminal(op, KL_CONNECT_CANCELLED, KL_INVALID_SOCKET, op->last_error);
    else
        co_request_cancels(op);                   /* already terminal — ensure losers cancelled once */
    co_finalize(op);
    return 0;
}

KlConnectOpState kl_connect_op_state(const KlConnectOp *op) {
    return op ? (KlConnectOpState)op->state : KL_CONNECT_OP_STATE_DETACHED;
}

int kl_connect_op_is_detached(const KlConnectOp *op) {
    return (op && op->detached) ? 1 : 0;
}
