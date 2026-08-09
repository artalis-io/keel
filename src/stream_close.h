/*
 * stream_close.h — INTERNAL. Phase-B KlStream graceful-close / confirmed-detachment lifecycle
 * (step 3). Built on the step-2A write machinery (src/stream_write.h) and the step-2B strict
 * read machinery (src/stream_read.h).
 *
 * The single acceptance invariant: the detachment callback (on_close) fires EXACTLY ONCE, and
 * ONLY after BOTH outstanding operations are DEMONSTRABLY retired — the receive
 * (recv_inflight == 0) AND the send (send_inflight == 0). A merely LOGICAL close is never
 * sufficient: in completion mode a posted recv/send buffer is still owned by the provider until
 * its completion (or a cancellation's terminal completion) arrives, so reusing/freeing the
 * stream before then is a use-after-free. Reuse of the stream (or its buffers) becomes legal only
 * after on_close.
 *
 * Two entry modes, one invariant:
 *   - Graceful (kl_stream_close_begin): stop reading (logical read close), refuse NEW writes, but
 *     let the already-queued output DRAIN (the write machinery keeps pumping on completions /
 *     flush). Detaches when recv + send are retired and nothing is left queued.
 *   - Abortive (kl_stream_cancel): additionally request cancellation of any physically-outstanding
 *     recv/send via the cancel hooks, and do NOT wait for the queue to drain (queued-but-unsubmitted
 *     bytes are the stream's own memory — the owner frees them at on_close). Still waits for the
 *     in-flight ops' terminal completions.
 *
 * Synchronous-completion discipline (mirrors the step-2B arm trampoline): a cancel hook MAY
 * complete its op synchronously (calling kl_stream_on_recv / kl_stream_on_write_complete from
 * inside the hook). Those retirements route through on_retire back into finalize; an in_close_cancel
 * sentinel DEFERS finalization until the cancel calls unwind, so on_close cannot fire re-entrantly
 * or twice. read/write notify retirement through a generic on_retire hook installed here — they
 * carry no static dependency on this module (on_retire is NULL until kl_stream_close_init, so the
 * standalone 2A/2B behavior is unchanged).
 *
 * Kept internal until the Phase-B public KlStream surface is finalized. State lives on KlStream
 * (connection.h), dormant unless kl_stream_close_init() is called.
 */
#ifndef KEEL_SRC_STREAM_CLOSE_H
#define KEEL_SRC_STREAM_CLOSE_H

#include <keel/connection.h>   /* KlStream */

/* Lifecycle phase (KlStream.close_state). Distinct from the KlStreamWriteStatus KL_STREAM_CLOSED
 * write-result enumerator (src/stream_write.h) — these name the close STATE machine. */
enum {
    KL_STREAM_STATE_OPEN = 0,   /* normal; no close requested */
    KL_STREAM_STATE_CLOSING,    /* close requested; awaiting physical retirement of both ops */
    KL_STREAM_STATE_CLOSED      /* both ops retired; on_close fired; reuse legal */
};

/* Detachment callback: invoked exactly once when the stream is fully retired. After this returns,
 * the owner may reuse or free the stream and its buffers. It MUST NOT call kl_stream_close_begin/
 * cancel again (the stream is already CLOSED). */
typedef void (*KlStreamCloseFn)(void *ctx);

/* Optional cancellation hooks. Request that the provider cancel a physically-outstanding recv/send.
 * Returns 0 if the cancel was submitted/accepted, -1 otherwise (advisory only — the lifecycle still
 * waits for the op's terminal completion regardless). A hook MAY complete its op synchronously
 * (invoking kl_stream_on_recv / kl_stream_on_write_complete inline); finalize is deferred until the
 * cancel calls unwind (in_close_cancel), so this is safe. */
typedef int (*KlStreamCancelFn)(void *ctx);

/* Install the close lifecycle. `on_close` (required) is the detachment callback. `ctx` is shared by
 * on_close and the cancel hooks. Wires the generic on_retire notification so the write/read
 * machinery can drive finalization. Rejects a double init (-1); requires on_close. Returns 0. */
int kl_stream_close_init(KlStream *s, KlStreamCloseFn on_close, void *ctx);

/* Install optional cancellation hooks (either may be NULL). Only consulted by the abortive path
 * (kl_stream_cancel). FROZEN once closing begins — returns -1 if the stream is not OPEN (changing
 * provider hooks mid-cancel would weaken the lifecycle contract). Returns 0, or -1 if not inited
 * or already closing/closed. */
int kl_stream_set_cancel(KlStream *s, KlStreamCancelFn cancel_recv, KlStreamCancelFn cancel_send);

/* Begin a GRACEFUL close: logically close the read side (no more delivery/re-arm; readiness
 * physically retires the recv, completion leaves it to complete-and-drop), refuse new writes, and
 * let queued output drain. Fires on_close synchronously if both ops are already retired and the
 * queue is empty; otherwise on_close fires later from the final retirement. Idempotent: a second
 * begin (or begin after cancel/closed) is a no-op. Returns 0, or -1 if not inited. */
int kl_stream_close_begin(KlStream *s);

/* Begin an ABORTIVE close: as graceful, PLUS request cancellation of any physically-outstanding
 * recv/send (via the cancel hooks, which obey the synchronous-completion discipline) and detach
 * without waiting for the queue to drain. Still waits for both ops' terminal completions before
 * on_close. If already CLOSING gracefully, escalates to abortive (issues the cancels). Each cancel
 * hook is invoked AT MOST ONCE per outstanding op: a repeated kl_stream_cancel() while an op is
 * still outstanding does NOT re-request its cancellation (the flag clears only when a new op starts
 * or the stream is re-inited). Safe to call reentrantly from a cancel hook. Idempotent once CLOSED.
 * Returns 0, or -1 if not inited. */
int kl_stream_cancel(KlStream *s);

/* Current lifecycle phase (KL_STREAM_OPEN/_CLOSING/_CLOSED). */
int kl_stream_close_state(const KlStream *s);

/* 1 once on_close has fired (the stream is fully detached and reusable), else 0. */
int kl_stream_is_detached(const KlStream *s);

#endif /* KEEL_SRC_STREAM_CLOSE_H */
