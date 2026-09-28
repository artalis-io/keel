#ifndef KEEL_SRC_COMPLETION_LIFE_H
#define KEEL_SRC_COMPLETION_LIFE_H

/*
 * completion_life.h: TRANSPORT-NEUTRAL liveness + refcount token for completion ops.
 *
 * A completion op posted on a completion backend outlives the transport owner that started it: the
 * backend op is kernel-/backend-owned and its completion may be reaped only when the loop is driven,
 * possibly AFTER the owner began teardown (and after the caller freed/reused the transport). The op
 * therefore must NOT retain or dereference the owner nor its native handle. Instead each posted op
 * carries a reference to this token, which:
 *
 *  - is allocated from the EVENT-CONTEXT / backend allocator (outlives the transport), before the
 *    first op is posted;
 *  - holds a NULLABLE live `target` (the owner); an owner that tears down before its storage may mark
 *    it dead, clearing the target FIRST; every completion path recovers the target through
 *    kl_comp_life_target() and touches the owner ONLY while it is non-NULL;
 *  - owns storage a posted op still references (a receive buffer, or the whole owner object) via
 *    `on_final`, so that storage outlives every posted op that pins it;
 *  - is reference-counted: one OWNER ref (held by the transport) plus one ref per posted backend op. A
 *    posted op releases its ref exactly once, on genuine retirement/dequeue: post failure,
 *    cancellation (silent drop OR dispatched error), normal completion, callback-free teardown,
 *    backend shutdown. If a completion EVENT is emitted, ownership TRANSFERS from the backend op to the
 *    event and is released after dispatch. Teardown drops the owner ref; the FINAL release runs
 *    on_final and frees the token.
 *
 * Owners today: KlDatagram (KL_COMP_DGRAM_*; on_final frees the inbound slot + receive machine, and
 * teardown marks the token dead) and the Windows Named Pipe KlStream (KL_COMP_PIPE_*; on_final frees
 * the whole pipe object and closes its handle, and the token is never marked dead).
 *
 * This is the frozen "backend-owned stable token" mechanism (docs/contracts/datagram.md §5), NOT a
 * generation check on a freed object. At post time the backend reads the neutral by-value op
 * descriptor to copy the handle/context/buffer/token into the op, and thereafter touches only the
 * token.
 *
 * SINGLE-THREADED: every create/retain/release/mark_dead/target call MUST run on the event-loop
 * thread. The refcount is a plain int; there is no atomic/locking. (Owners are single-loop-affine.)
 *
 * INTERNAL header: not installed, no ABI commitment.
 */

#include <keel/allocator.h>

typedef struct KlCompLife KlCompLife;

struct KlCompletionEvent;   /* completion.h: the dispatch handler's event (fwd; avoids a header cycle) */

/* The owner's completion handler. `target` is the live owner (kl_comp_life_target(), NULL once dead);
 * the handler routes the event and RELEASES the event's transferred token ref after dispatch. */
typedef void (*KlCompLifeDispatchFn)(void *target, const struct KlCompletionEvent *ev);

/* Create a token with ONE owner reference (refs=1, live=1, target set). `on_final` runs on the FINAL
 * release; it frees the owner-side storage via `final_ctx`; the token then frees itself. `dispatch`
 * is the completion routing identity (the driver calls `dispatch(target, ev)` for this token's
 * completions). `alloc` MUST outlive every posted op (the event-ctx / backend allocator, never
 * storage that disappears with the transport). Returns NULL on allocation failure. */
KlCompLife *kl_comp_life_create(KlAllocator *alloc, void *target,
                                void (*on_final)(void *final_ctx), void *final_ctx,
                                KlCompLifeDispatchFn dispatch);

/* The token's completion-routing handler. */
KlCompLifeDispatchFn kl_comp_life_dispatch(const KlCompLife *l);

/* Take a reference (a backend op, at post time). No-op on NULL. */
void kl_comp_life_retain(KlCompLife *l);

/* Release a reference. On the FINAL release, run on_final (owner storage) then free the token.
 * No-op on NULL. Must be called exactly once per retain (+ once for the owner ref). */
void kl_comp_life_release(KlCompLife *l);

/* The owner (the transport) is going away (teardown): clear the live target so later completions see a
 * dead token. Does NOT drop the owner reference (the caller releases that separately). Idempotent. */
void kl_comp_life_mark_dead(KlCompLife *l);

/* The live target (the owner), or NULL once dead. A completion path MUST check this before touching
 * the owner / its native handle. */
void *kl_comp_life_target(const KlCompLife *l);

#endif /* KEEL_SRC_COMPLETION_LIFE_H */
