/*
 * keel/stream.h - Raw-transport stream: write queue, strict read pause/resume, and graceful-close
 * lifecycle (KlStream).
 *
 * ┌───────────────────────────────────────────────────────────────────────────────────────────┐
 * │ STABLE transport API. The function signatures + ownership contracts below are the            │
 * │ committed public surface. The struct LAYOUT is NOT part of the ABI: it lives in              │
 * │ <keel/stream_detail.h> (opt-in, for embedders that stack/embed a KlStream) and may change    │
 * │ between releases; embedders recompile. Use the accessors, never the detail fields.           │
 * └───────────────────────────────────────────────────────────────────────────────────────────┘
 *
 * A model-agnostic raw byte transport, independent of readiness vs completion. Three cooperating
 * facets, all dormant until their _init is called (so a plain KlHttpConn is unaffected):
 *   - WRITE: atomic all-or-none over a bounded, preallocated queue; readiness direct-send + flush,
 *     or completion one-submit-at-a-time with WRITE-completion pumping.
 *   - READ: strict pause/resume; a completed receive is HELD undelivered while paused and
 *     delivered exactly once on resume; sync-completion-safe (iterative arm trampoline).
 *   - CLOSE: graceful (drain) or abortive (cancel) close with CONFIRMED DETACHMENT; on_close fires
 *     once, only after both the receive and send ops are physically retired.
 *
 * TLS lives ABOVE the raw stream: the adapter's writer/submit and read hooks encrypt/decrypt.
 */
#ifndef KEEL_STREAM_H
#define KEEL_STREAM_H

#include <stddef.h>
#include <keel/handle.h>      /* KlSocketHandle, kl_ssize_t */
#include <keel/allocator.h>   /* KlAllocator (write-queue init) */
#ifdef __cplusplus
extern "C" {
#endif


/** @brief Opaque raw-transport stream. Full layout in <keel/stream_detail.h> (opt-in). */
typedef struct KlStream KlStream;

/* ── Base initializer ──────────────────────────────────────────────────────────────────────── */

/** Base initializer. Validates the stable receive buffer, resets ALL state, and establishes the
 *  base invariants (an un-configured stream has an invalid handle and no facet installed). Call
 *  this FIRST: before the facet initializers (kl_stream_write_init / read_init / close_init) and
 *  before an internal adapter configures the platform handle/context. Consumers must not touch the
 *  detail fields; this is the whole-object initializer. `read_buffer` is the caller-owned stable
 *  buffer that completed receives land in.
 *
 *  STRICT PRECONDITION: the stream MUST be one of:
 *    - fresh / uninitialized storage (never passed to a facet initializer); or
 *    - a fully DETACHED stream (kl_stream_is_detached() == 1) whose write queue has been released
 *      with kl_stream_write_free() and all provider-owned resources have retired.
 *  Calling it on an ACTIVE or partially-initialized stream is INVALID (undefined): it would lose an
 *  allocated write queue and erase outstanding-operation ownership. This function does not, and
 *  cannot safely, detect that condition; it is the caller's contract.
 *
 *  Returns 0, or -1 if `stream`/`read_buffer` is NULL or `read_capacity` is 0. */
int kl_stream_init(KlStream *stream, char *read_buffer, size_t read_capacity);

/* ── Write ─────────────────────────────────────────────────────────────────────────────────── */

/** Atomic write result. */
typedef enum {
    KL_STREAM_ACCEPTED = 0,  /**< all bytes taken (sent inline and/or queued) */
    KL_STREAM_WOULD_BLOCK,   /**< len <= capacity but no room now; NOTHING taken */
    KL_STREAM_TOO_LARGE,     /**< len > capacity; permanent: caller must chunk */
    KL_STREAM_CLOSED,        /**< write side closing; new writes refused */
    KL_STREAM_ERROR          /**< fatal (writer/submission failure); bytes stay owned */
} KlStreamWriteStatus;

/** Readiness writer: send up to `len` bytes of `data`. Returns the number of bytes written
 *  (>= 0; a short write is fine, 0 = would-block), or -1 on a fatal error. Supplied by the
 *  transport provider or wrapper. */
typedef kl_ssize_t (*KlStreamWriteFn)(const char *data, size_t len, void *ctx);
/** Completion-mode submit hook: post one async send of [data,len]. 0 = submitted (a WRITE
 *  completion follows), non-zero = failed. Supplied by the transport provider or wrapper. */
typedef int (*KlStreamSubmitFn)(void *ctx, const char *data, size_t len);

/** Preallocate the write queue to `capacity` bytes (one alloc, init-time). Returns 0, or -1. */
int kl_stream_write_init(KlStream *s, KlAllocator *alloc, size_t capacity);
/** Install the READINESS writer (selects readiness mode). Init-time only. Returns 0, or -1. */
int kl_stream_set_writer(KlStream *s, KlStreamWriteFn fn, void *ctx);
/** Install the COMPLETION submit hook (selects completion mode). `copying`=1 if the backend copies
 *  at submit. Init-time only. Returns 0, or -1. */
int kl_stream_set_submit(KlStream *s, KlStreamSubmitFn fn, void *ctx, int copying);
/** Atomic, all-or-none write. Returns the mapped status. */
KlStreamWriteStatus kl_stream_write(KlStream *s, const char *data, size_t len);
/** Readiness: drain the queue on a writable signal. 0 drained / 1 pending / -1 error. */
int kl_stream_flush(KlStream *s);
/** Completion: the in-flight WRITE completed with result `ok`. 0 ok / -1 stream in error. */
int kl_stream_on_write_complete(KlStream *s, int ok);
/** Bytes currently queued (not yet fully sent / acknowledged). */
size_t kl_stream_write_pending(const KlStream *s);
/** Free the write queue (refuses while a send is in flight). Returns 0, or -1. */
int kl_stream_write_free(KlStream *s);

/* ── Read (strict pause/resume) ────────────────────────────────────────────────────────────── */

/** Deliver a completed receive. `ok`=1: `len` bytes in `buf` (the stream's stable read_buf).
 *  `ok`=0: terminal EOF/error (len 0). MUST NOT free/reuse the stream (reuse only after on_close). */
typedef void (*KlStreamReadDeliverFn)(void *ctx, const char *buf, size_t len, int ok);
/** Arm/post the next receive. Readiness: add READ interest. Completion: post_recv. 0 ok, -1 fail. */
typedef int  (*KlStreamReadArmFn)(void *ctx);
/** Drop a pending receive on pause. Readiness: remove READ interest. Completion: no-op. */
typedef void (*KlStreamReadDisarmFn)(void *ctx);

/** Install the read hooks and select the model. `completion_mode`=1: a recv posted before pause
 *  still completes (held); 0 (readiness): disarm cancels it. Rejects a double init (-1). */
int  kl_stream_read_init(KlStream *s, int completion_mode,
                         KlStreamReadDeliverFn deliver, KlStreamReadArmFn arm,
                         KlStreamReadDisarmFn disarm, void *ctx);
/** Begin reading: arm the first receive. Returns 0, or -1 if arming failed. */
int  kl_stream_read_start(KlStream *s);
/** The transport completed a receive: `len` bytes in read_buf, `ok`=1 data / 0 EOF-error. Returns
 *  0, or -1 on a re-arm failure or an oversized completion (fails closed). */
int  kl_stream_on_recv(KlStream *s, size_t len, int ok);
/** STRICT pause: no further delivery/accumulation. Idempotent. */
void kl_stream_pause(KlStream *s);
/** Resume: deliver a held completion exactly once (if any) then re-arm. Idempotent. Returns 0/-1. */
int  kl_stream_resume(KlStream *s);
/** Logically close the read side (discard any held completion). Idempotent. */
void kl_stream_read_close(KlStream *s);
/** Is a completed receive currently held (paused, undelivered)? */
int  kl_stream_read_held(const KlStream *s);

/* ── Close / confirmed-detachment lifecycle ────────────────────────────────────────────────── */

/** Close lifecycle phase (kl_stream_close_state). Distinct from the KlStreamWriteStatus
 *  KL_STREAM_CLOSED write-result enumerator. */
typedef enum {
    KL_STREAM_STATE_OPEN = 0,   /**< normal; no close requested */
    KL_STREAM_STATE_CLOSING,    /**< close requested; awaiting physical retirement of both ops */
    KL_STREAM_STATE_CLOSED      /**< both ops retired; on_close fired; reuse legal */
} KlStreamCloseState;

/** Detachment callback: invoked exactly once when the stream is fully retired (reuse legal after). */
typedef void (*KlStreamCloseFn)(void *ctx);
/** Optional cancellation hook: request the provider cancel a physically-outstanding recv/send
 *  (advisory; the lifecycle still waits for the op's terminal completion). May complete inline. */
typedef int  (*KlStreamCancelFn)(void *ctx);

/** Install the close lifecycle. `on_close` required. Rejects a double init (-1). Returns 0. */
int  kl_stream_close_init(KlStream *s, KlStreamCloseFn on_close, void *ctx);
/** Install optional cancel hooks (either NULL). FROZEN once closing begins (-1 unless OPEN). */
int  kl_stream_set_cancel(KlStream *s, KlStreamCancelFn cancel_recv, KlStreamCancelFn cancel_send);
/** Begin a GRACEFUL close (drain queued output). Idempotent. Returns 0, or -1 if not inited. */
int  kl_stream_close_begin(KlStream *s);
/** Begin an ABORTIVE close (cancel outstanding ops; drop queue). Idempotent once CLOSED. 0/-1. */
int  kl_stream_cancel(KlStream *s);
/** Current lifecycle phase (KlStreamCloseState). */
KlStreamCloseState kl_stream_close_state(const KlStream *s);
/** 1 once on_close has fired (fully detached; reusable), else 0. */
int  kl_stream_is_detached(const KlStream *s);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_STREAM_H */
