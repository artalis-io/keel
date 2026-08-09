/*
 * listener.h — INTERNAL. Phase-B accept-side listener state machine (step 5).
 *
 * A model-agnostic state machine for the inbound accept path, symmetric with KlConnectOp on the
 * outbound side. It keeps FOUR lifetimes structurally distinct — the reviewer's core requirement:
 *
 *   1. LISTENER LIFETIME — the KlListener itself: start → accept loop → close → confirmed
 *      detachment (on_close fires only after the outstanding accept AND the held slot reservation
 *      are retired; reuse/free legal only after).
 *   2. POOL-OWNED SLOT-RELEASE CAPABILITY — the credit reserve/release functions belong to the
 *      connection pool, not the listener. The listener BORROWS them to reserve a slot before each
 *      accept (backpressure) and BAKES the release fn+ctx BY VALUE into each KlSlotLease, so a
 *      committed slot can be released to the pool even after the listener is gone.
 *   3. NULLABLE CREDIT / LIVENESS TARGET — slot accounting is optional (reserve/release NULL =
 *      unbounded accept, no backpressure). A nullable `liveness` token (pool/owner-owned stable
 *      storage) is carried in each lease so a release that arrives after the pool is torn down is a
 *      safe no-op rather than a use-after-free.
 *   4. ACCEPTED-STREAM LIFETIME — an accepted connection outlives the listener. Its slot credit is
 *      handed off as a KlSlotLease through the accept callback; the accepted-stream owner releases
 *      it EXACTLY ONCE when the connection closes, independently of the listener.
 *
 * Discipline carried over from steps 2B/3/4: a reserved slot is taken BEFORE arming an accept
 * (never accept beyond capacity); the arm hook may complete synchronously (an iterative trampoline
 * bounds the stack); every reentrant adapter callback (on_accept, dispose_fd, on_close, cancel)
 * runs under a depth guard so it cannot detach-and-free mid-callback; cancellation of a posted
 * accept is requested once; and EVERY accepted descriptor that cannot be handed off as a live
 * connection (listener closing, spurious, out-of-range) is routed to dispose_fd — no fd leaks.
 *
 * The listener owns NO sockets, pool, or event loop: the adapter arms/cancels accepts, reserves/
 * releases pool credits, and drives the on_* entry points. Kept internal until Step 6 wiring.
 */
#ifndef KEEL_SRC_LISTENER_H
#define KEEL_SRC_LISTENER_H

#include <keel/handle.h>   /* KlSocketHandle */

/* Lifecycle phase (KlListener.state). */
enum {
    KL_LISTENER_IDLE = 0,   /* not started */
    KL_LISTENER_LISTENING,  /* a slot is reserved and an accept is armed */
    KL_LISTENER_PAUSED,     /* no slot credit available — backpressure (no accept armed) */
    KL_LISTENER_CLOSING,    /* close requested; awaiting accept + reservation retirement */
    KL_LISTENER_CLOSED      /* fully retired; on_close fired; reuse legal */
};

/* ── Slot credit (pool-owned capability, borrowed by the listener) ─────────────────────────── */

/* Reserve one connection-slot credit. Returns 1 (reserved), 0 (none available → backpressure),
 * -1 (pool error → the listener closes). NULL = no slot accounting (accept always permitted). */
typedef int  (*KlSlotReserveFn)(void *ctx);
/* Return one connection-slot credit to the pool. Used both for the listener's held-but-unused
 * reservation and, baked by value into a KlSlotLease, for a committed connection's slot. */
typedef void (*KlSlotReleaseFn)(void *ctx);

/* A committed slot handed (BY VALUE) to an accepted connection. Carries the POOL-OWNED release
 * capability plus a nullable liveness token — it does NOT reference the listener, so it stays valid
 * after the listener is closed/freed. MOVE-ONLY BY CONVENTION: the accepted-stream owner takes sole
 * ownership of the copy it receives and releases it EXACTLY ONCE (kl_slot_lease_release consumes it,
 * so a second release on that owned copy is a safe no-op). Do not copy a live lease and release
 * both copies — that would return two credits and overrun pool capacity.
 *
 * Liveness-token lifetime contract: `alive` (when non-NULL) MUST be independently stable storage
 * that OUTLIVES every issued lease — owned by a supervisor / slot arena, NOT freed together with
 * the pool it guards (the guard reads *alive before touching release_ctx, so the token must remain
 * valid even after the pool's release_ctx is torn down). Teardown order, exactly:
 *   1. set *alive = 0;
 *   2. destroy / invalidate release_ctx (the pool);
 *   3. keep the liveness-token storage alive until every issued lease has been released;
 *   4. finally free the token storage.
 * (Steps 1 before 2 are essential: a lease release in between must see *alive == 0 and no-op,
 * never invoke a freed release_ctx.) */
typedef struct {
    KlSlotReleaseFn release;      /* pool-owned release fn (copied by value; NULL = nothing to do) */
    void           *release_ctx;  /* pool-owned context */
    const int      *alive;        /* nullable, independently-stable liveness token (see contract) */
} KlSlotLease;

/* Release a committed slot back to the pool, EXACTLY ONCE, liveness-guarded. CONSUMES `*lease`
 * (clears its fields) so a repeated call on the same owned lease is a harmless no-op and the
 * release callback cannot re-enter and double-release through it. If the (stable) liveness token
 * says the pool is gone, this is a safe no-op. Call once from the accepted-stream owner on close. */
void kl_slot_lease_release(KlSlotLease *lease);

/* ── Accept hooks ──────────────────────────────────────────────────────────────────────────── */

/* Arm/post one accept. Readiness: add listen-fd READ interest. Completion: post_accept. May
 * complete synchronously (call kl_listener_on_accepted / on_accept_failed inside the call).
 * Returns 0 (armed/in flight, or decided inline), -1 on a hard failure that closes the listener. */
typedef int  (*KlListenerArmFn)(void *ctx);
/* Readiness: drop listen-fd READ interest. Completion: no-op (a posted accept still completes).
 * Required in readiness mode. */
typedef void (*KlListenerDisarmFn)(void *ctx);
/* Completion: request cancellation of an outstanding posted accept (invoked at most once). */
typedef void (*KlListenerCancelFn)(void *ctx);

/* An accepted connection: `fd` is the new socket; `lease` is the committed slot (pool-owned
 * release + nullable liveness), passed BY VALUE to express ownership transfer. The callback takes
 * ownership of `fd` and of the lease copy, and must release the lease exactly once
 * (kl_slot_lease_release) when the connection closes. It MAY reentrantly close the listener. */
typedef void (*KlListenerAcceptFn)(void *ctx, KlSocketHandle fd, KlSlotLease lease);

/* Dispose of an accepted `fd` that cannot become a live connection (listener closing, spurious/
 * duplicate, or out-of-range). REQUIRED: an accept can always land during teardown. */
typedef void (*KlListenerDisposeFn)(void *ctx, KlSocketHandle fd);

/* Detachment: fires EXACTLY ONCE after close AND the outstanding accept AND the held reservation
 * retire. Reuse/free of the listener is legal only after this returns. */
typedef void (*KlListenerCloseFn)(void *ctx);

typedef struct {
    KlSlotReserveFn      reserve;      /* optional (pairs with release); NULL = unbounded */
    KlSlotReleaseFn      release;      /* optional (pairs with reserve) */
    void                *credit_ctx;   /* pool context for reserve/release + baked into leases */
    const int           *liveness;     /* nullable stable liveness token (pool/owner-owned) */
    KlListenerArmFn      arm_accept;   /* required */
    KlListenerDisarmFn   disarm_accept;/* required in readiness mode */
    KlListenerCancelFn   cancel_accept;/* optional (completion) */
    KlListenerAcceptFn   on_accept;    /* required */
    KlListenerDisposeFn  dispose_fd;   /* required */
    KlListenerCloseFn    on_close;     /* optional */
} KlListenerHooks;

typedef struct KlListener {
    int  state;               /* KL_LISTENER_* */
    int  inited;
    int  completion_mode;     /* 1 = a posted accept completes even after disarm (→ dispose) */
    int  detached;            /* on_close fired (exactly-once) */
    int  last_error;          /* last accept/arm error (informational) */

    int  reserved;            /* 1 = a slot credit is held, awaiting an accept to commit it */
    int  accept_inflight;     /* an accept is armed/posted */
    int  accept_cancel_requested;

    /* sync-completion trampoline (bounds stack; mirrors stream_read) */
    int  arming;
    int  rearm_pending;
    int  accepted_inline;

    /* reentrancy guards */
    int  in_start;            /* DEPTH: inside an arm hook — defer detachment */
    int  in_dispatch;         /* DEPTH: inside on_accept/dispose/close/cancel — defer detachment */

    KlSlotReserveFn      reserve;
    KlSlotReleaseFn      release;
    void                *credit_ctx;
    const int           *liveness;
    KlListenerArmFn      arm_accept;
    KlListenerDisarmFn   disarm_accept;
    KlListenerCancelFn   cancel_accept;
    KlListenerAcceptFn   on_accept;
    KlListenerDisposeFn  dispose_fd;
    KlListenerCloseFn    on_close;
    void *ctx;
} KlListener;

/* Install hooks and reset state (zeroes the listener). `completion_mode` selects whether a posted
 * accept can still complete after disarm (completion) or is cancelled by disarm (readiness).
 * Requires arm_accept, on_accept, dispose_fd; requires disarm_accept in readiness mode; reserve
 * and release must be both-or-neither. Returns 0, or -1 on a bad/missing hook. Re-init is reuse. */
int kl_listener_init(KlListener *l, int completion_mode, const KlListenerHooks *hooks, void *ctx);

/* Begin accepting: reserve a slot and arm the first accept (or PAUSE if no slot). Returns 0, or
 * -1 if not inited / not IDLE. */
int kl_listener_start(KlListener *l);

/* An accept completed with a new connection `fd`. Commits the reserved slot and hands (fd, lease)
 * to on_accept, then reserves + arms the next. During teardown (or if spurious/out-of-range) the
 * fd is disposed instead. */
void kl_listener_on_accepted(KlListener *l, KlSocketHandle fd);

/* An accept attempt failed with `error` (e.g. ECONNABORTED): return the reserved slot and re-arm,
 * unless closing. A duplicate/spurious call (no accept in flight) is dropped. */
void kl_listener_on_accept_failed(KlListener *l, int error);

/* A connection slot was released back to the pool by someone else: if PAUSED, resume (reserve +
 * arm). No-op unless PAUSED. */
void kl_listener_notify_slot_free(KlListener *l);

/* Begin close: stop accepting (readiness disarms and retires the accept; completion requests
 * cancel once), release the held reservation, and detach once the outstanding accept retires.
 * Idempotent. Returns 0, or -1 if not inited. */
int kl_listener_close(KlListener *l);

/* Current lifecycle phase (KL_LISTENER_*). */
int kl_listener_state(const KlListener *l);

/* 1 once on_close has fired (fully retired; reusable), else 0. */
int kl_listener_is_detached(const KlListener *l);

#endif /* KEEL_SRC_LISTENER_H */
