#ifndef KEEL_SRC_DATAGRAM_LIFE_H
#define KEEL_SRC_DATAGRAM_LIFE_H

/*
 * datagram_life.h: TRANSPORT-NEUTRAL liveness + refcount token for datagram completion ops.
 *
 * A datagram completion (KL_COMP_DGRAM_RECV / _SEND) posted on a completion backend outlives the
 * transport owner (the KlDgramCore) that started it: the backend op is kernel-/backend-owned and its
 * completion may be reaped only when the loop is driven, possibly AFTER the owner began teardown (and
 * after the caller freed/reused the transport). The op therefore must NOT retain or dereference the
 * owner nor its socket state. Instead each posted op carries a reference to this token, which:
 *
 *  - is allocated from the EVENT-CONTEXT / backend allocator (outlives the transport), before the
 *    first receive is posted;
 *  - holds a NULLABLE live `target` (the owner); teardown marks it dead and clears the target FIRST;
 *    every completion path recovers the target through kl_dgram_life_target() and touches the owner
 *    ONLY while it is non-NULL;
 *  - owns the receive-side storage (the inbound slot + receive machine) via `on_final`, so that
 *    storage outlives every posted receive op that pins it;
 *  - is reference-counted: one OWNER ref (held by the transport) plus one ref per posted backend op. A
 *    posted op releases its ref exactly once, on genuine retirement/dequeue: post failure,
 *    cancellation (silent drop OR dispatched error), normal completion, callback-free teardown,
 *    backend shutdown. If a completion EVENT is emitted, ownership TRANSFERS from the backend op to the
 *    event and is released after dispatch. Teardown drops the owner ref; the FINAL release runs
 *    on_final (freeing the inbound storage + machine) and frees the token.
 *
 * This is the frozen "backend-owned stable token" mechanism (docs/contracts/datagram.md §6), NOT a
 * generation check on a freed object. It is transport-neutral: at post time the backend reads the
 * neutral by-value op descriptor to copy the socket/context/buffer/token into the op, and thereafter
 * touches only the token.
 *
 * SINGLE-THREADED: every create/retain/release/mark_dead/target call MUST run on the event-loop
 * thread. The refcount is a plain int; there is no atomic/locking. (A datagram is single-loop-affine.)
 *
 * INTERNAL header: not installed, no ABI commitment.
 */

#include <keel/allocator.h>

typedef struct KlDgramLife KlDgramLife;

struct KlCompletionEvent;   /* completion.h: the dispatch handler's event (fwd; avoids a header cycle) */

/* Which side of a datagram op a completion-axis cancel/retire query targets. Shared by the
 * completion backend seam (KlCompletionOps.cancel_dgram/retire_dgram) and the close coordinator
 * (datagram_close.h KlDgramRetireFn), so it lives on this transport-neutral token header. */
typedef enum { KL_DGRAM_OP_RECV = 0, KL_DGRAM_OP_SEND } KlDgramOpKind;

/* Per-op retirement classification a completion backend reports to the close coordinator.
 * PENDING keeps the object CLOSING (a cancelled completion op not yet drained); RETIRED = physically
 * done (storage safe to free); QUARANTINED = could NOT be confirmed retired (EFI unconfirmed op →
 * fail-closed, ref abandoned). `transport_err` (out) is 1 iff a terminal transport error occurred on
 * the op; it becomes CLOSE_ERROR ONLY when every op is RETIRED (never under quarantine). */
typedef enum {
    KL_DGRAM_RETIRE_PENDING = 0,
    KL_DGRAM_RETIRE_RETIRED,
    KL_DGRAM_RETIRE_QUARANTINED
} KlDgramRetireResult;

/* The owner's completion handler. `target` is the live owner (kl_dgram_life_target(), NULL once dead);
 * the handler routes the event and RELEASES the event's transferred token ref after dispatch. */
typedef void (*KlDgramDispatchFn)(void *target, const struct KlCompletionEvent *ev);

/* Create a token with ONE owner reference (refs=1, live=1, target set). `on_final` runs on the FINAL
 * release; it frees the owner-side receive storage (inbound slot + machine) via `final_ctx`; the
 * token then frees itself. `dispatch` is the completion routing identity (the driver calls
 * `dispatch(target, ev)` for this token's completions). `alloc` MUST outlive every posted op (the
 * event-ctx / backend allocator, never storage that disappears with the transport). Returns NULL
 * on allocation failure. */
KlDgramLife *kl_dgram_life_create(KlAllocator *alloc, void *target,
                                  void (*on_final)(void *final_ctx), void *final_ctx,
                                  KlDgramDispatchFn dispatch);

/* The token's completion-routing handler. */
KlDgramDispatchFn kl_dgram_life_dispatch(const KlDgramLife *l);

/* Take a reference (a backend op, at post time). No-op on NULL. */
void kl_dgram_life_retain(KlDgramLife *l);

/* Release a reference. On the FINAL release, run on_final (owner storage) then free the token.
 * No-op on NULL. Must be called exactly once per retain (+ once for the owner ref). */
void kl_dgram_life_release(KlDgramLife *l);

/* The owner (the transport) is going away (teardown): clear the live target so later completions see a
 * dead token. Does NOT drop the owner reference (the caller releases that separately). Idempotent. */
void kl_dgram_life_mark_dead(KlDgramLife *l);

/* The live target (the owner), or NULL once dead. A completion path MUST check this before touching
 * the owner / its socket state. */
void *kl_dgram_life_target(const KlDgramLife *l);

#endif /* KEEL_SRC_DATAGRAM_LIFE_H */
