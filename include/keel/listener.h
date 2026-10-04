/*
 * keel/listener.h: Accept-side listener state machine (KlListener).
 *
 * ┌───────────────────────────────────────────────────────────────────────────────────────────┐
 * │ STABLE transport API. The function signatures + ownership contracts below are the            │
 * │ committed public surface. The struct LAYOUT is NOT part of the ABI: it lives in              │
 * │ <keel/listener_detail.h> (opt-in, for embedders that stack/embed a KlListener) and may change │
 * │ between releases; embedders recompile. Use the accessors, never the detail fields.           │
 * └───────────────────────────────────────────────────────────────────────────────────────────┘
 *
 * A model-agnostic accept-path state machine, symmetric with KlConnectOp. It keeps FOUR lifetimes
 * structurally distinct:
 *   1. LISTENER LIFETIME: start → accept loop → close → confirmed detachment (on_close after
 *      EVERY posted accept has retired and its reservation returned; reuse/free legal only after).
 *      Up to `window` accepts are posted concurrently (window=1 readiness; backlog for IOCP), each
 *      holding one reserved credit and retiring exactly once via on_accepted/on_accept_failed.
 *   2. POOL-OWNED SLOT-RELEASE CAPABILITY: reserve/release belong to the pool; the listener
 *      borrows them for backpressure and bakes the release fn+ctx BY VALUE into each KlSlotLease.
 *   3. NULLABLE CREDIT / LIVENESS TARGET: slot accounting is optional; a nullable liveness token
 *      makes a lease release after pool teardown a safe no-op.
 *   4. ACCEPTED-STREAM LIFETIME: an accepted connection outlives the listener; its credit is
 *      handed off as a KlSlotLease and released once by the accepted-stream owner on close.
 *
 * The listener owns no sockets/pool/event loop; the adapter arms/cancels accepts, reserves/
 * releases pool credits, and drives the on_* entry points.
 */
#ifndef KEEL_LISTENER_H
#define KEEL_LISTENER_H

#include <keel/handle.h>   /* KlSocketHandle */
#ifdef __cplusplus
extern "C" {
#endif


/** @brief Opaque listener. Full layout in <keel/listener_detail.h> (opt-in). */
typedef struct KlListener KlListener;

/** @brief Lifecycle phase (kl_listener_state). The listener keeps up to `window` accepts posted
 *  concurrently, one reserved pool credit each (window=1 for readiness; up to the AcceptEx backlog
 *  for IOCP; see kl_listener_set_accept_window). */
typedef enum {
    KL_LISTENER_STATE_IDLE = 0,   /**< not started */
    KL_LISTENER_STATE_LISTENING,  /**< accepting: 1..window accepts posted (credit starvation while some
                                 remain posted STAYS LISTENING: it does not pause) */
    KL_LISTENER_STATE_PAUSED,     /**< backpressure: NO credit AND zero accepts posted (posted count
                                 reached 0); resumes via kl_listener_notify_slot_free */
    KL_LISTENER_STATE_CLOSING,    /**< close requested; awaiting retirement of every posted accept */
    KL_LISTENER_STATE_CLOSED      /**< fully retired; on_close fired; reuse legal */
} KlListenerState;

/* ── Slot credit (pool-owned capability, borrowed by the listener) ─────────────────────────── */

/** Reserve one connection-slot credit. Returns 1 (reserved), 0 (none → backpressure), -1 (pool
 *  error → the listener closes). NULL = no slot accounting (accept always permitted). */
typedef int  (*KlSlotReserveFn)(void *ctx);
/** Return one connection-slot credit to the pool. */
typedef void (*KlSlotReleaseFn)(void *ctx);

/** A committed slot handed (BY VALUE) to an accepted connection. Carries the POOL-OWNED release
 *  capability plus a nullable liveness token; it does NOT reference the listener, so it stays
 *  valid after the listener is closed/freed. MOVE-ONLY BY CONVENTION: the accepted-stream owner
 *  releases it EXACTLY ONCE (kl_slot_lease_release consumes it, so a second release is a no-op).
 *
 *  Liveness-token lifetime contract: `alive` (when non-NULL) MUST be independently stable storage
 *  that OUTLIVES every issued lease (supervisor / slot arena, NOT freed with the pool it guards).
 *  Teardown order, exactly: (1) set *alive = 0; (2) destroy/invalidate release_ctx; (3) keep the
 *  token storage until every lease is released; (4) free the token storage. */
typedef struct {
    KlSlotReleaseFn release;      /**< pool-owned release fn (by value; NULL = nothing to do) */
    void           *release_ctx;  /**< pool-owned context */
    const int      *alive;        /**< nullable, independently-stable liveness token */
} KlSlotLease;

/** Release a committed slot back to the pool EXACTLY ONCE, liveness-guarded. CONSUMES *lease so a
 *  repeated call on the same owned lease is a harmless no-op. Call once on connection close. */
void kl_slot_lease_release(KlSlotLease *lease);

/* ── Accept hooks ──────────────────────────────────────────────────────────────────────────── */

/** An arm hook's "nothing posted, try again later" (a transient failure: no memory for the op, a
 *  full submission queue). The listener returns the post's credit and, with nothing else posted,
 *  PAUSEs until kl_listener_notify_slot_free; it does not close. */
#define KL_LISTENER_ARM_RETRY (-2)

/** Arm/post ONE accept. Readiness: (re)assert the single persistent listen interest. Completion:
 *  post one discrete accept op. May complete inline. Returns 0 (posted / inline-decided),
 *  KL_LISTENER_ARM_RETRY when nothing could be posted for now, or -1 on a hard failure that closes
 *  the listener.
 *
 *  SYNCHRONOUS-COMPLETION ASSOCIATION (required for window > 1): if this hook completes inline, the
 *  inline kl_listener_on_accepted / on_accept_failed MUST retire the accept THIS call is posting,
 *  not an older still-outstanding one. The pump detects inline completion by the posted count
 *  returning to its pre-post value, which is only correct under that association; violating it would
 *  undercount an operation and lose its reserved credit. */
typedef int  (*KlListenerArmFn)(void *ctx);
/** Drop the accept interest. Readiness: remove the persistent listen-fd READ interest; MUST be
 *  IDEMPOTENT (it is called both on close AND on backpressure PAUSE, i.e. after an accept
 *  completion, whenever the persistent level-triggered interest must be removed). Completion:
 *  no-op. Required in readiness mode. */
typedef void (*KlListenerDisarmFn)(void *ctx);
/** Completion: request cancellation of the posted accepts. Invoked at most once; it is a SINGLE
 *  BATCH request covering EVERY currently-posted accept; each cancelled accept must still complete
 *  once through kl_listener_on_accept_failed (returning its credit). */
typedef void (*KlListenerCancelFn)(void *ctx);
/** An accepted connection: `fd` is the new socket; `lease` (by value → ownership transfer) is the
 *  committed slot. The callback takes ownership of fd and the lease and releases the lease exactly
 *  once on close. It MAY reentrantly close the listener. Every posted accept retires EXACTLY ONCE:
 *  through this callback (success) or kl_listener_on_accept_failed (failure/cancel). */
typedef void (*KlListenerAcceptFn)(void *ctx, KlSocketHandle fd, KlSlotLease lease);
/** Dispose of an accepted `fd` that cannot become a live connection (closing/spurious/out-of-range).
 *  REQUIRED. */
typedef void (*KlListenerDisposeFn)(void *ctx, KlSocketHandle fd);
/** Detachment: fires EXACTLY ONCE after close AND the outstanding accept AND the held reservation
 *  retire. Reuse/free legal only after this returns. */
typedef void (*KlListenerCloseFn)(void *ctx);

/* ── Object handoff family (a transport that is not a socket) ──────────────────────────────────
 * The listener never interprets what it hands off; it only passes the accepted value to on_accept or
 * back to dispose. For a socket that value is the fd (above). For a transport whose accepted
 * connection is NOT a socket (a Windows Named Pipe instance), the adapter that performed the accept
 * materializes its own connection object and hands THAT over instead: `conn` is that adapter's object,
 * opaque to the listener and typed by the adapter at both ends (it is never a native handle routed
 * through here). A listener uses exactly ONE family, fixed at init.
 *
 * OWNERSHIP of a `conn` passed to kl_listener_on_accepted_obj (the fd family is identical, with fd for
 * conn). From that call on, the listener owns it and releases it EXACTLY ONCE, by one of:
 *   - on_accept_obj(ctx, conn, lease): the listener is LISTENING and this retires a posted accept.
 *     Ownership of conn and the lease passes to the callback, unconditionally: there is no refusal
 *     return, so an owner that does not want the connection releases it itself. The callback may
 *     reentrantly close the listener; conn stays the owner's, and detachment waits until it returns.
 *   - dispose_obj(ctx, conn): nothing was posted (a spurious accept), or the listener is CLOSING /
 *     CLOSED. The accept's credit is returned first. The callback may reentrantly close the
 *     listener; detachment waits until it returns.
 * A conn handed to on_accept_obj is never disposed later, whatever happens next (a refill whose
 * arm fails, a close, a teardown). A synchronous accept from inside arm_accept follows the same rules
 * (the pump trampoline bounds the stack). The ONE exception: calling kl_listener_on_accepted_obj on a
 * listener initialised with the FD family is a contract violation. The call retires as a failed
 * accept and conn stays the caller's (neither callback runs). */

/** An accepted connection object: ownership of `conn` and the lease transfer to the callback, with
 *  the same exactly-once rules as KlListenerAcceptFn. */
typedef void (*KlListenerAcceptObjFn)(void *ctx, void *conn, KlSlotLease lease);
/** Dispose of an accepted `conn` that cannot be handed off (spurious / during teardown). Required in
 *  the object family. */
typedef void (*KlListenerDisposeObjFn)(void *ctx, void *conn);

typedef struct {
    KlSlotReserveFn      reserve;      /**< optional (pairs with release); NULL = unbounded */
    KlSlotReleaseFn      release;      /**< optional (pairs with reserve) */
    void                *credit_ctx;   /**< pool context for reserve/release + baked into leases */
    const int           *liveness;     /**< nullable stable liveness token (pool/owner-owned) */
    KlListenerArmFn      arm_accept;   /**< required */
    KlListenerDisarmFn   disarm_accept;/**< required in readiness mode */
    KlListenerCancelFn   cancel_accept;/**< optional (completion) */
    KlListenerAcceptFn   on_accept;    /**< fd family: required with dispose_fd */
    KlListenerDisposeFn  dispose_fd;   /**< fd family: required with on_accept */
    KlListenerCloseFn    on_close;     /**< optional */
    KlListenerAcceptObjFn  on_accept_obj; /**< object family: required with dispose_obj */
    KlListenerDisposeObjFn dispose_obj;   /**< object family: required with on_accept_obj */
} KlListenerHooks;

/* ── Contract (the public surface) ─────────────────────────────────────────────────────────── */

/** Install hooks and reset state (zeroes the listener). `completion_mode` selects whether a posted
 *  accept survives disarm (completion) or is cancelled by disarm (readiness). Requires arm_accept and
 *  EXACTLY ONE complete handoff family: on_accept + dispose_fd (sockets), or on_accept_obj +
 *  dispose_obj (a non-socket transport); requires disarm_accept in readiness mode; reserve/release
 *  both-or-neither. Returns 0, or -1 on a bad/missing/mixed hook set. */
int  kl_listener_init(KlListener *l, int completion_mode, const KlListenerHooks *hooks, void *ctx);
/** Set the bounded accept window: the max number of accepts kept posted concurrently, one reserved
 *  pool credit each (default 1). Use the IOCP AcceptEx backlog for IOCP so its multi-deep accept
 *  concurrency is preserved; other completion backends (io_uring / pollcomp) use 1. REJECTED for a
 *  readiness listener (window MUST be 1: a readiness arm is one persistent interest, not N posted
 *  ops). Init-time only (before start): returns 0, or -1 if not inited / not IDLE / window < 1 / a
 *  window != 1 in readiness mode. */
int  kl_listener_set_accept_window(KlListener *l, int window);
/** Begin accepting: fill the window (reserve + post up to `window` accepts); PAUSE only if NO credit
 *  is available at all. Returns 0, or -1 if not inited / not IDLE. */
int  kl_listener_start(KlListener *l);
/** A posted accept completed with a new connection `fd`: retires that accept (exactly-once) and
 *  commits its reserved credit into the handed-off lease, then refills the window. During teardown
 *  the fd is disposed and the credit returned instead. */
void kl_listener_on_accepted(KlListener *l, KlSocketHandle fd);
/** Object-family counterpart of kl_listener_on_accepted: a posted accept completed with the adapter's
 *  connection object `conn`. Identical retire / commit / dispose / refill semantics. Called on an
 *  fd-family listener it is a contract violation and is refused: the accept retires as FAILED (its
 *  credit returned, last_error set) and `conn` stays the caller's. The fd entry point on an
 *  object-family listener is refused the same way. */
void kl_listener_on_accepted_obj(KlListener *l, void *conn);
/** A posted accept failed/was cancelled with `error`: retires that accept (exactly-once) and
 *  returns its reserved credit, then refills the window (or, when closing, advances detachment). */
void kl_listener_on_accept_failed(KlListener *l, int error);
/** A connection slot was released to the pool elsewhere: top the window back up (resumes a PAUSED
 *  listener; also fills remaining window slots that were starved of credit). */
void kl_listener_notify_slot_free(KlListener *l);
/** Begin close: stop accepting and retire every posted accept's reservation; readiness disarms and
 *  returns all posted credits at once; completion issues ONE batch cancel and returns each credit as
 *  its accept completes as failed. Detaches (on_close) once every posted accept has retired.
 *  Idempotent. Returns 0, or -1 if not inited. */
int  kl_listener_close(KlListener *l);
/** Current lifecycle phase (KlListenerState). */
KlListenerState kl_listener_state(const KlListener *l);
/** 1 once on_close has fired (fully retired; reusable), else 0. */
int  kl_listener_is_detached(const KlListener *l);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_LISTENER_H */
