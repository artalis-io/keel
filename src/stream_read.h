/*
 * stream_read.h — INTERNAL. Phase-B KlStream strict read pause/resume machinery (step 2B).
 *
 * A model-agnostic state machine over the raw receive path (docs/stream_contract.md §5). It
 * mediates between the transport (which lands received bytes in the stream's stable read_buf and
 * reports completions) and the consumer's deliver callback:
 *
 *   - Active: a completed receive is delivered immediately, then the next receive is re-armed.
 *   - Paused (STRICT): no read callback fires; a receive already in flight may complete into the
 *     stable read_buf but its bytes (or its terminal EOF/error condition) are HELD, undelivered,
 *     until resume. Readiness pause drops READ interest (disarm hook); completion pause simply
 *     posts no new receive (the in-flight one still completes → held).
 *   - Resume: delivers the held completion EXACTLY ONCE, then re-arms — unless the deliver
 *     callback synchronously paused or closed the stream, in which case nothing is re-armed
 *     (re-checked through the stream's stable state after the callback, like the accept rules).
 *
 * pause()/resume() are idempotent, including when called from inside the deliver callback. TLS
 * is above the raw stream: read_buf holds raw bytes here; the adapter's deliver hook decrypts as
 * needed. Kept internal until the Phase-B public surface is finalized; state is on KlStream
 * (connection.h), dormant unless kl_stream_read_init() is called, so HTTP behavior is unchanged.
 */
#ifndef KEEL_SRC_STREAM_READ_H
#define KEEL_SRC_STREAM_READ_H

#include <keel/connection.h>   /* KlStream */

/* Deliver a completed receive to the consumer. `ok` = 1: `len` data bytes are in `buf` (the
 * stream's stable read_buf). `ok` = 0: a terminal EOF/error — no data (len == 0). Callback
 * lifetime: it MAY synchronously pause, close, start, or write the stream — the machinery
 * re-checks state through stable fields afterward and honors it. It MUST NOT free/reuse the
 * stream; reuse becomes legal only after the later destructive-tail on_close detachment
 * callback (Step 3). */
typedef void (*KlStreamReadDeliverFn)(void *ctx, const char *buf, size_t len, int ok);

/* Arm/post the next receive. Readiness: add READ interest. Completion: post_recv. 0 ok, -1 fail
 * (a failure closes the read side). */
typedef int (*KlStreamReadArmFn)(void *ctx);

/* Drop a pending receive on pause. Readiness: remove READ interest. Completion: no-op (a posted
 * recv cannot be un-posted — it completes and is held). */
typedef void (*KlStreamReadDisarmFn)(void *ctx);

/* Install the read hooks and select the model. `completion_mode` = 1 means a receive posted
 * before pause still completes (into read_buf) and is held; 0 (readiness) means disarm cancels
 * the pending interest so no completion arrives. Rejects a double init (-1). */
int kl_stream_read_init(KlStream *s, int completion_mode,
                        KlStreamReadDeliverFn deliver, KlStreamReadArmFn arm,
                        KlStreamReadDisarmFn disarm, void *ctx);

/* Begin reading: arm the first receive (if active, none in flight, none held, not closed).
 * Returns 0, or -1 if arming failed. */
int kl_stream_read_start(KlStream *s);

/* The transport completed a receive: `len` bytes landed in read_buf, `ok` = 1 for data / 0 for
 * EOF/error. `len` > read_cap is a backend contract violation — the read side FAILS CLOSED (not
 * delivered as data; returns -1). A duplicate/spurious completion (none in flight) is dropped.
 * Active: deliver then re-arm (unless the callback paused/closed, or it was terminal). Paused:
 * hold the completion (bytes or terminal) for resume. A completion after close is dropped
 * (retirement). Returns 0, or -1 on a re-arm failure or an oversized completion. */
int kl_stream_on_recv(KlStream *s, size_t len, int ok);

/* STRICT pause: no further delivery/accumulation. Idempotent. Readiness disarms the pending
 * receive; completion leaves an in-flight recv to complete-then-hold. */
void kl_stream_pause(KlStream *s);

/* Resume: deliver a held completion exactly once (if any) then re-arm; if there is no held data,
 * re-arm to fetch more. Re-arm is suppressed if the deliver callback paused/closed the stream,
 * or if the held completion was terminal. Idempotent (no-op if not paused). Returns 0, or -1 if
 * a re-arm failed. */
int kl_stream_resume(KlStream *s);

/* LOGICALLY close the read side: no future delivery or re-arm, and any held completion is
 * discarded without delivering it. This does NOT claim retirement of a physically-outstanding
 * completion receive — in completion mode a posted recv may still be owned by the provider, so
 * recv_inflight stays set until its completion reaches kl_stream_on_recv (dropped) or a future
 * cancel-retirement path clears it. Readiness disarms and clears interest (nothing outstanding
 * after). Idempotent. (The logical/physical split is what Step-3 detachment builds on.) */
void kl_stream_read_close(KlStream *s);

/* Is a completed receive currently held (paused, undelivered)? */
int kl_stream_read_held(const KlStream *s);

#endif /* KEEL_SRC_STREAM_READ_H */
