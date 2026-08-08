/*
 * stream_write.h — INTERNAL. Phase-B KlStream write machinery (step 2A).
 *
 * The generic, model-agnostic write side of a KlStream, built on the Step-1 KlDrain
 * reservation substrate (src/drain_reserve.h). kl_stream_write() is atomic all-or-none over a
 * bounded, preallocated queue and returns a KlStreamWriteStatus that maps 1:1 to the four
 * internal drain results.
 *
 * Two transport models, one queue:
 *   - Readiness: the queue's write_fn (adapter-supplied) does the direct send; the reservation
 *     path sends the prefix inline and buffers the remainder; kl_stream_flush() drains on a
 *     writable signal.
 *   - Completion: a submit hook (adapter-supplied) posts one async send at a time. A successful
 *     submission is NOT permission to post another — send_inflight stays set until the WRITE
 *     completion (kl_stream_on_write_complete), which then pumps the next queued batch. A copying
 *     backend may free the queued bytes at submission; a referencing backend keeps them until
 *     completion. Submission failure leaves the bytes owned in the queue and sets a sticky error.
 *
 * TLS stays ABOVE the raw stream: the adapter's writer/submit hooks encrypt as needed; no TLS
 * state lives on KlStream. Kept internal (not in public <keel/connection.h>) until the Phase-B
 * public KlStream surface is finalized. The storage is on KlStream (connection.h), dormant
 * unless kl_stream_write_init() is called; the HTTP response path is unchanged.
 */
#ifndef KEEL_SRC_STREAM_WRITE_H
#define KEEL_SRC_STREAM_WRITE_H

#include <keel/connection.h>   /* KlStream */
#include <keel/drain.h>        /* KlDrainWriteFn */

/* Completion-mode submit hook: post one async send of [data,len]. Returns 0 = submitted (a
 * WRITE completion will follow), non-zero = failed. Supplied by the interim HTTP/TLS adapter
 * (the completion analogue of KlDrain's readiness write_fn). Internal name — no public typedef
 * while the machinery is internal (matches the inline field type on KlStream). */
typedef int (*KlInternalStreamSubmitFn)(void *ctx, const char *data, size_t len);

/* Atomic write result — mirrors docs/stream_contract.md §2 (CLOSED is reserved for the Tier-3
 * half-close, not produced in 2A). Maps from the drain results: ACCEPTED<-ACCEPTED,
 * WOULD_BLOCK<-WOULD_BLOCK, TOO_LARGE<-TOO_LARGE, ERROR<-WERROR. */
typedef enum {
    KL_STREAM_ACCEPTED = 0,  /* all bytes taken (sent inline and/or queued) */
    KL_STREAM_WOULD_BLOCK,   /* len <= capacity but no room now; NOTHING taken */
    KL_STREAM_TOO_LARGE,     /* len > capacity; permanent — caller must chunk */
    KL_STREAM_CLOSED,        /* write side gone (Tier-3; not produced in 2A) */
    KL_STREAM_ERROR          /* fatal (writer/submission failure); bytes stay owned */
} KlStreamWriteStatus;

/* Preallocate the stream's bounded write queue to `capacity` bytes (one allocation, at init —
 * no hot-path alloc). Returns 0 on success, -1 on allocation failure. Idempotent-safe: a
 * second call on an already-inited stream is rejected (-1) rather than leaking. */
int kl_stream_write_init(KlStream *s, KlAllocator *alloc, size_t capacity);

/* Install the READINESS writer (the queue's direct-send function). Leaving the submit hook
 * unset selects readiness mode. Configuration is init-time only: returns -1 (no change) if a
 * send is in flight or bytes are queued — the transport mode/ownership must not change while
 * data is active. Returns 0 on success. */
int kl_stream_set_writer(KlStream *s, KlDrainWriteFn fn, void *ctx);

/* Install the COMPLETION submit hook (selects completion mode). `copying` = 1 if the backend
 * copies the bytes at submission (so the queue may be consumed immediately; ordering still
 * blocks the next submit until the WRITE completion). Same init-time-only rule as
 * kl_stream_set_writer: returns -1 if in-flight or queued, else 0. */
int kl_stream_set_submit(KlStream *s, KlInternalStreamSubmitFn fn, void *ctx, int copying);

/* Atomic, all-or-none write. Readiness: reserve + direct-send prefix + buffer remainder.
 * Completion: reserve (buffer) then pump one submit if none is in flight. Returns the mapped
 * status; a completion submission failure returns KL_STREAM_ERROR with the bytes still owned
 * in the queue (never silently discarded) and the stream marked errored. */
KlStreamWriteStatus kl_stream_write(KlStream *s, const char *data, size_t len);

/* Readiness: drain the queue on a writable signal. Returns 0 = drained, 1 = more pending
 * (would-block), -1 = error. Fires the queue's low-water writable callback per Step-1 rules. */
int kl_stream_flush(KlStream *s);

/* Completion: the in-flight WRITE completed with result `ok` (1 = success, 0 = failure).
 * On success: consume the delivered bytes (referencing backend; a copying backend consumed
 * them at submit), clear the in-flight op, and pump the next queued batch. On failure: the op
 * has retired (the completion arrived), so the provider no longer references the queue —
 * retire it, keep the unsent queued bytes (the stream's error/teardown policy, not a provider-
 * lifetime need), set a sticky write error, and do NOT pump. Returns
 * 0 = ok (idle or next submitted), -1 = the stream is in error (this failure, a later submit
 * failure, or a prior sticky error). */
int kl_stream_on_write_complete(KlStream *s, int ok);

/* Bytes currently queued (not yet fully sent / acknowledged). */
size_t kl_stream_write_pending(const KlStream *s);

/* Free the write queue. Refuses (returns -1, no change) while a send is in flight — freeing
 * provider-referenced storage would be a UAF; only free after confirmed op retirement (the
 * WRITE completion, or a future cancel path). Returns 0 when freed (or already free). Safe on
 * an uninitialized stream. */
int kl_stream_write_free(KlStream *s);

#endif /* KEEL_SRC_STREAM_WRITE_H */
