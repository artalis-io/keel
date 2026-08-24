/*
 * keel/connect_op.h: Outbound-connect terminal-once state machine (KlConnectOp).
 *
 * ┌───────────────────────────────────────────────────────────────────────────────────────────┐
 * │ STABLE transport API. The function signatures + ownership contracts below are the            │
 * │ committed public surface. The struct LAYOUT is NOT part of the ABI: it lives in              │
 * │ <keel/connect_op_detail.h> (opt-in, for embedders that stack/embed a KlConnectOp) and may     │
 * │ change between releases; embedders recompile. Use the accessors, never the detail fields.    │
 * └───────────────────────────────────────────────────────────────────────────────────────────┘
 *
 * A model-agnostic state machine for establishing ONE outbound connection: name resolution
 * followed by Happy Eyeballs (RFC 8305) racing connect over the resolved address list. It owns no
 * timers, sockets, or event loop; the adapter arms the timers and drives the on_* entry points.
 *
 * Guarantees:
 *   - TERMINAL EXACTLY ONCE. on_done fires once: SUCCESS (winning fd), FAILED, or CANCELLED.
 *   - SYNCHRONOUS COMPLETION SAFE. resolve(), a connect attempt, and the arm hooks may complete
 *     inline; the machine bounds the C stack and never re-fires or detaches mid-callback.
 *   - CANCELLATION ONCE PER ACTIVE OP. resolve + each racing attempt are cancel-requested at most
 *     once; a reentrant cancel from a hook is safe.
 *   - TOTAL DESCRIPTOR OWNERSHIP. The winning fd transfers exactly once through on_done; every
 *     other connected fd (straggler, duplicate, out-of-range) is routed to dispose_fd (required).
 *   - TIMERS AS FIRST-CLASS RESOURCES. delay/deadline are armed via hooks (each paired with a
 *     required synchronous cancel), disarmed at terminal, and gate detachment; events consume once.
 *   - NO REUSE UNTIL CONFIRMED DETACHMENT. on_detach fires only after the terminal AND every
 *     outstanding op AND both timers retire. Re-init (kl_connect_op_init, which zeroes the op) is
 *     the reuse reset.
 */
#ifndef KEEL_CONNECT_OP_H
#define KEEL_CONNECT_OP_H

#include <keel/handle.h>   /* KlSocketHandle, KL_INVALID_SOCKET */

/** @brief Opaque connect operation. Full layout in <keel/connect_op_detail.h> (opt-in). */
typedef struct KlConnectOp KlConnectOp;

/** @brief Max racing addresses (matches KL_RESOLVE_MAX_ADDRS without coupling to resolver.h). */
#define KL_CONNECT_MAX_ADDRS 8

/** @brief Lifecycle phase (kl_connect_op_state). */
typedef enum {
    KL_CONNECT_OP_STATE_IDLE = 0,   /**< not started */
    KL_CONNECT_OP_STATE_RESOLVING,  /**< a name resolution is in flight */
    KL_CONNECT_OP_STATE_CONNECTING, /**< racing connect over the resolved list */
    KL_CONNECT_OP_STATE_DONE,       /**< terminal decided (on_done fired) */
    KL_CONNECT_OP_STATE_DETACHED    /**< all ops retired; on_detach fired; reuse legal */
} KlConnectOpState;

/** @brief Terminal result. */
typedef enum {
    KL_CONNECT_SUCCESS = 0,   /**< a connect attempt won; fd is the connected socket */
    KL_CONNECT_FAILED,        /**< resolve failed / all attempts failed / deadline */
    KL_CONNECT_CANCELLED      /**< kl_connect_op_cancel() before a natural terminal */
} KlConnectResult;

/* ── Adapter hooks ─────────────────────────────────────────────────────────────────────────── */

/** Begin name resolution. May complete synchronously (call kl_connect_op_on_resolved /
 *  on_resolve_failed inline). Returns 0 if in flight (or inline-decided), -1 if it could not start
 *  (→ terminal FAILED). */
typedef int (*KlConnectResolveFn)(void *ctx);
/** Request cancellation of the in-flight resolve (invoked at most once per resolve op). */
typedef void (*KlConnectResolveCancelFn)(void *ctx);
/** Begin one connect attempt to address `idx`. May complete inline. Returns 0 (in flight / inline
 *  decided), -1 on a HARD LOCAL failure that never became pending (advance to the next address); on
 *  -1 the hook MUST write the platform error to *out_err. */
typedef int (*KlConnectAttemptFn)(void *ctx, int idx, int *out_err);
/** Request cancellation of racing attempt `idx` (invoked at most once per attempt op). */
typedef void (*KlConnectAttemptCancelFn)(void *ctx, int idx);
/** Dispose of a connected socket that will NOT be the winner (straggler / duplicate / out-of-range).
 *  REQUIRED: descriptor ownership must never be dropped. */
typedef void (*KlConnectDisposeFn)(void *ctx, KlSocketHandle fd);
/** Arm the Connection Attempt Delay (RFC 8305 §5 stagger). Returns 0 armed, -1 arm failed (the
 *  machine fast-starts the next address). May fire inline (call kl_connect_op_on_delay). Optional;
 *  if set, cancel_delay MUST also be set. */
typedef int (*KlConnectArmDelayFn)(void *ctx);
/** Disarm the delay. MUST synchronously retire the timer. Required iff arm_delay is set. */
typedef void (*KlConnectCancelDelayFn)(void *ctx);
/** Arm the overall connect deadline at start. Returns 0 armed, -1 arm failed (a deadline is
 *  essential → the connect FAILS with *out_err). May fire inline. Optional; if set, cancel_deadline
 *  MUST also be set. */
typedef int (*KlConnectArmDeadlineFn)(void *ctx, int *out_err);
/** Disarm the deadline. MUST synchronously retire the timer. Required iff arm_deadline is set. */
typedef void (*KlConnectCancelDeadlineFn)(void *ctx);
/** Terminal callback: fires EXACTLY ONCE. On SUCCESS, `fd` is the winning socket (ownership
 *  transfers) and `error` 0; else `fd` is KL_INVALID_SOCKET and `error` the failure code. The op is
 *  NOT yet reusable; wait for on_detach. It MAY reentrantly cancel/retire. */
typedef void (*KlConnectDoneFn)(void *ctx, KlConnectResult result, KlSocketHandle fd, int error);
/** Detachment: fires EXACTLY ONCE after the terminal AND all ops AND both timers retire. Reuse/
 *  free legal only after this returns. */
typedef void (*KlConnectDetachFn)(void *ctx);

typedef struct {
    KlConnectResolveFn        start_resolve;    /**< required */
    KlConnectResolveCancelFn  cancel_resolve;   /**< optional */
    KlConnectAttemptFn        start_attempt;    /**< required */
    KlConnectAttemptCancelFn  cancel_attempt;   /**< optional */
    KlConnectDisposeFn        dispose_fd;       /**< required */
    KlConnectArmDelayFn       arm_delay;         /**< optional (pairs with cancel_delay) */
    KlConnectCancelDelayFn    cancel_delay;      /**< required iff arm_delay */
    KlConnectArmDeadlineFn    arm_deadline;      /**< optional (pairs with cancel_deadline) */
    KlConnectCancelDeadlineFn cancel_deadline;   /**< required iff arm_deadline */
    KlConnectDoneFn           on_done;           /**< required */
    KlConnectDetachFn         on_detach;         /**< optional */
} KlConnectOpHooks;

/* ── Contract (the public surface) ─────────────────────────────────────────────────────────── */

/** Install hooks and reset state (zeroes the op). Requires start_resolve, start_attempt, on_done,
 *  dispose_fd; arm/cancel timer hooks must be paired. Returns 0, or -1 on a bad/missing hook. */
int  kl_connect_op_init(KlConnectOp *op, const KlConnectOpHooks *hooks, void *ctx);
/** Begin: enter RESOLVING and start the resolve. Returns 0, or -1 if not inited / not IDLE. */
int  kl_connect_op_start(KlConnectOp *op);
/** Resolution completed with `naddrs` addresses (clamped to KL_CONNECT_MAX_ADDRS). */
void kl_connect_op_on_resolved(KlConnectOp *op, int naddrs);
/** Resolution failed with `error` (terminal FAILED unless already terminal). */
void kl_connect_op_on_resolve_failed(KlConnectOp *op, int error);
/** Racing attempt `idx` connected with `fd` (first to do so WINS; later stragglers are disposed). */
void kl_connect_op_on_attempt_connected(KlConnectOp *op, int idx, KlSocketHandle fd);
/** Racing attempt `idx` failed with `error` (fast-start next / terminal on exhaustion). */
void kl_connect_op_on_attempt_failed(KlConnectOp *op, int idx, int error);
/** The Connection Attempt Delay fired (start the next address). Consumed exactly once. */
void kl_connect_op_on_delay(KlConnectOp *op);
/** The overall deadline fired (terminal FAILED, cancel all outstanding). Consumed exactly once. */
void kl_connect_op_on_deadline(KlConnectOp *op, int error);
/** Request an abortive cancel (terminal CANCELLED if not already terminal; cancel every op once).
 *  Safe reentrantly; idempotent once detached. Returns 0, or -1 if not inited. */
int  kl_connect_op_cancel(KlConnectOp *op);
/** Current lifecycle phase (KlConnectOpState). */
KlConnectOpState kl_connect_op_state(const KlConnectOp *op);
/** 1 once on_detach has fired (fully retired; reusable), else 0. */
int  kl_connect_op_is_detached(const KlConnectOp *op);

#endif /* KEEL_CONNECT_OP_H */
