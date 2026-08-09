/*
 * keel/listener.h — Accept-side listener state machine (KlListener).
 *
 * ┌───────────────────────────────────────────────────────────────────────────────────────────┐
 * │ EXPERIMENTAL / UNSTABLE CANDIDATE API (Phase-B transport, step 6A).                          │
 * │ The FUNCTION + OWNERSHIP CONTRACTS below are the public surface. The struct layout is NOT —  │
 * │ it lives in <keel/listener_detail.h> for embedders (opt-in) and may change without notice.   │
 * └───────────────────────────────────────────────────────────────────────────────────────────┘
 *
 * A model-agnostic accept-path state machine, symmetric with KlConnectOp. It keeps FOUR lifetimes
 * structurally distinct:
 *   1. LISTENER LIFETIME — start → accept loop → close → confirmed detachment (on_close after the
 *      outstanding accept AND the held reservation retire; reuse/free legal only after).
 *   2. POOL-OWNED SLOT-RELEASE CAPABILITY — reserve/release belong to the pool; the listener
 *      borrows them for backpressure and bakes the release fn+ctx BY VALUE into each KlSlotLease.
 *   3. NULLABLE CREDIT / LIVENESS TARGET — slot accounting is optional; a nullable liveness token
 *      makes a lease release after pool teardown a safe no-op.
 *   4. ACCEPTED-STREAM LIFETIME — an accepted connection outlives the listener; its credit is
 *      handed off as a KlSlotLease and released once by the accepted-stream owner on close.
 *
 * The listener owns no sockets/pool/event loop — the adapter arms/cancels accepts, reserves/
 * releases pool credits, and drives the on_* entry points.
 */
#ifndef KEEL_LISTENER_H
#define KEEL_LISTENER_H

#include <keel/handle.h>   /* KlSocketHandle */

/** @brief Opaque listener. Full layout in <keel/listener_detail.h> (opt-in). */
typedef struct KlListener KlListener;

/** @brief Lifecycle phase (kl_listener_state). */
enum {
    KL_LISTENER_IDLE = 0,   /**< not started */
    KL_LISTENER_LISTENING,  /**< a slot is reserved and an accept is armed */
    KL_LISTENER_PAUSED,     /**< no slot credit — backpressure (no accept armed) */
    KL_LISTENER_CLOSING,    /**< close requested; awaiting accept + reservation retirement */
    KL_LISTENER_CLOSED      /**< fully retired; on_close fired; reuse legal */
};

/* ── Slot credit (pool-owned capability, borrowed by the listener) ─────────────────────────── */

/** Reserve one connection-slot credit. Returns 1 (reserved), 0 (none → backpressure), -1 (pool
 *  error → the listener closes). NULL = no slot accounting (accept always permitted). */
typedef int  (*KlSlotReserveFn)(void *ctx);
/** Return one connection-slot credit to the pool. */
typedef void (*KlSlotReleaseFn)(void *ctx);

/** A committed slot handed (BY VALUE) to an accepted connection. Carries the POOL-OWNED release
 *  capability plus a nullable liveness token — it does NOT reference the listener, so it stays
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

/** Arm/post one accept. May complete inline (call kl_listener_on_accepted / on_accept_failed).
 *  Returns 0 (armed/in flight, or inline-decided), -1 on a hard failure that closes the listener. */
typedef int  (*KlListenerArmFn)(void *ctx);
/** Drop the accept interest. Readiness: remove the persistent listen-fd READ interest — MUST be
 *  IDEMPOTENT (it is called both on close AND on backpressure PAUSE, i.e. after an accept
 *  completion, whenever the persistent level-triggered interest must be removed). Completion:
 *  no-op. Required in readiness mode. */
typedef void (*KlListenerDisarmFn)(void *ctx);
/** Completion: request cancellation of an outstanding posted accept (invoked at most once). */
typedef void (*KlListenerCancelFn)(void *ctx);
/** An accepted connection: `fd` is the new socket; `lease` (by value → ownership transfer) is the
 *  committed slot. The callback takes ownership of fd and the lease and releases the lease exactly
 *  once on close. It MAY reentrantly close the listener. */
typedef void (*KlListenerAcceptFn)(void *ctx, KlSocketHandle fd, KlSlotLease lease);
/** Dispose of an accepted `fd` that cannot become a live connection (closing/spurious/out-of-range).
 *  REQUIRED. */
typedef void (*KlListenerDisposeFn)(void *ctx, KlSocketHandle fd);
/** Detachment — fires EXACTLY ONCE after close AND the outstanding accept AND the held reservation
 *  retire. Reuse/free legal only after this returns. */
typedef void (*KlListenerCloseFn)(void *ctx);

typedef struct {
    KlSlotReserveFn      reserve;      /**< optional (pairs with release); NULL = unbounded */
    KlSlotReleaseFn      release;      /**< optional (pairs with reserve) */
    void                *credit_ctx;   /**< pool context for reserve/release + baked into leases */
    const int           *liveness;     /**< nullable stable liveness token (pool/owner-owned) */
    KlListenerArmFn      arm_accept;   /**< required */
    KlListenerDisarmFn   disarm_accept;/**< required in readiness mode */
    KlListenerCancelFn   cancel_accept;/**< optional (completion) */
    KlListenerAcceptFn   on_accept;    /**< required */
    KlListenerDisposeFn  dispose_fd;   /**< required */
    KlListenerCloseFn    on_close;     /**< optional */
} KlListenerHooks;

/* ── Contract (the public surface) ─────────────────────────────────────────────────────────── */

/** Install hooks and reset state (zeroes the listener). `completion_mode` selects whether a posted
 *  accept survives disarm (completion) or is cancelled by disarm (readiness). Requires arm_accept,
 *  on_accept, dispose_fd; requires disarm_accept in readiness mode; reserve/release both-or-neither.
 *  Returns 0, or -1 on a bad/missing hook. */
int  kl_listener_init(KlListener *l, int completion_mode, const KlListenerHooks *hooks, void *ctx);
/** Begin accepting: reserve a slot and arm the first accept (or PAUSE if none). Returns 0, or -1
 *  if not inited / not IDLE. */
int  kl_listener_start(KlListener *l);
/** An accept completed with a new connection `fd` (handed off, or disposed during teardown). */
void kl_listener_on_accepted(KlListener *l, KlSocketHandle fd);
/** An accept attempt failed with `error` (return the slot and re-arm, unless closing). */
void kl_listener_on_accept_failed(KlListener *l, int error);
/** A connection slot was released back to the pool elsewhere: resume if PAUSED. */
void kl_listener_notify_slot_free(KlListener *l);
/** Begin close: stop accepting, release the held reservation, detach once the accept retires.
 *  Idempotent. Returns 0, or -1 if not inited. */
int  kl_listener_close(KlListener *l);
/** Current lifecycle phase (KL_LISTENER_*). */
int  kl_listener_state(const KlListener *l);
/** 1 once on_close has fired (fully retired; reusable), else 0. */
int  kl_listener_is_detached(const KlListener *l);

#endif /* KEEL_LISTENER_H */
