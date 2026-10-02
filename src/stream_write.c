/*
 * stream_write.c: KlStream write machinery. See stream_write.h.
 *
 * Model-agnostic atomic write over a bounded, preallocated KlDrain queue. Readiness
 * drains via the queue's write_fn; completion posts one async send at a time via the submit
 * hook and pumps the next on the WRITE completion. TLS is above the raw stream (the adapter's
 * hooks encrypt); no TLS state here.
 */
#include "stream_write.h"
#include "drain_reserve.h"     /* kl_drain_prealloc / reserve_write / low-water */
#include "allocator_validate.h"
#include <keel/drain.h>

/* Map an internal drain reservation result to the public-ish stream write status. */
static KlStreamWriteStatus map_drain(KlDrainWriteStatus s) {
    switch (s) {
    case KL_DRAIN_ACCEPTED:    return KL_STREAM_ACCEPTED;
    case KL_DRAIN_WOULD_BLOCK: return KL_STREAM_WOULD_BLOCK;
    case KL_DRAIN_TOO_LARGE:   return KL_STREAM_TOO_LARGE;
    case KL_DRAIN_WERROR:      default: return KL_STREAM_ERROR;
    }
}

int kl_stream_write_init(KlStream *s, KlAllocator *alloc, size_t capacity) {
    if (!s || s->wq_inited || !kl_allocator_ops_valid(alloc)) return -1;
    kl_drain_init(&s->wq, NULL, NULL, alloc);   /* writer installed later by the adapter */
    if (kl_drain_prealloc(&s->wq, capacity) < 0) return -1;
    s->wq_inited        = 1;
    s->wq_err           = 0;
    s->submit_fn        = NULL;
    s->submit_ctx       = NULL;
    s->submit_copying   = 0;
    s->send_inflight    = 0;
    s->inflight_len     = 0;
    s->inflight_copying = 0;
    s->wr_blocked       = 0;
    s->in_writable      = 0;
    s->on_writable      = NULL;
    s->writable_ctx     = NULL;
    return 0;
}

/* Transport config is init-time only: reject any change while a send is in flight or bytes are
 * queued, so the mode/ownership can't shift under an active operation (a later completion would
 * otherwise interpret ownership with the wrong policy). */
static int stream_config_locked(const KlStream *s) {
    return s->send_inflight || kl_drain_buffered(&s->wq) > 0;
}

int kl_stream_set_writer(KlStream *s, KlStreamWriteFn fn, void *ctx) {
    if (!s || !s->wq_inited) return -1;
    if (stream_config_locked(s)) return -1;
    s->wq.write_fn  = fn;
    s->wq.write_ctx = ctx;
    return 0;
}

int kl_stream_set_submit(KlStream *s, KlStreamSubmitFn fn, void *ctx, int copying) {
    if (!s || !s->wq_inited) return -1;
    if (stream_config_locked(s)) return -1;
    s->submit_fn      = fn;
    s->submit_ctx     = ctx;
    s->submit_copying = copying ? 1 : 0;
    return 0;
}

int kl_stream_on_writable(KlStream *s, KlStreamWritableFn fn, void *ctx) {
    if (!s || !s->wq_inited) return -1;
    s->on_writable  = fn;
    s->writable_ctx = ctx;
    if (!fn) s->wr_blocked = 0;
    return 0;
}

/* The writable edge, evaluated at a physical progress point AFTER all state is updated. Returns 1
 * (and disarms) when the blocked producer must be told it may write again: the total pending count,
 * in-flight bytes included, is below capacity, or the write side has failed so waiting is futile.
 * Never while closing: the close owns the stream's end. */
static int stream_writable_edge(KlStream *s) {
    if (!s->wr_blocked || !s->on_writable || s->wq_closing) return 0;
    if (!s->wq_err && kl_stream_write_pending(s) >= s->wq.max_size) return 0;
    s->wr_blocked = 0;
    return 1;
}

/* Run the edge callback with close finalization deferred (stream_close.c reads in_writable): a close
 * begun from inside it must not detach the stream, and so let its owner free it, while this frame is
 * still using it. The caller's trailing on_retire performs the deferred finalize. */
static void stream_fire_writable(KlStream *s) {
    s->in_writable++;
    s->on_writable(s->writable_ctx);
    s->in_writable--;
}

/* Retire the in-flight send's bytes and state after a successful completion. */
static void stream_send_retired(KlStream *s) {
    if (!s->inflight_copying)                     /* referencing backend: safe to free NOW */
        kl_drain_consume(&s->wq, s->inflight_len);
    s->send_inflight = 0;
    s->inflight_len  = 0;
}

/* Completion pump: if no send is in flight and the queue has bytes, submit exactly one batch.
 * Returns 0 = ok (submitted, or nothing to do, or blocked by an in-flight send), -1 = the
 * submission failed (bytes remain owned in the queue; sticky error set). Ordering: send_inflight
 * blocks a second submit until kl_stream_on_write_complete clears it.
 *
 * Synchronous-completion safe (docs/contracts/stream.md): the op is recorded as in flight BEFORE
 * submit_fn runs, so a kl_stream_on_write_complete from inside it is recognized, not dropped as
 * spurious. That inline completion is only noted (the backend's frame is still on the stack); the
 * pump retires it once submit_fn has returned and loops to submit the next batch, so a backend that
 * always completes inline drains the queue iteratively, without recursion. The writable edge and the
 * retire notification stay with the caller: kl_stream_on_write_complete fires both after pumping,
 * and kl_stream_write needs neither (it cleared wr_blocked, and a closing stream refuses writes). */
static int stream_pump_completion(KlStream *s) {
    for (;;) {
        if (s->wq_err) return -1;
        if (s->close_abort) return 0;            /* abortive close drops the queue: send nothing more */
        if (s->send_inflight) return 0;          /* one in flight; wait for its completion */
        size_t len = kl_drain_buffered(&s->wq);
        if (len == 0) return 0;
        const char *data = kl_drain_data(&s->wq);
        s->send_inflight    = 1;
        s->send_cancel_requested = 0;            /* a genuinely new send op; not yet cancel-requested */
        s->inflight_len     = len;
        s->inflight_copying = s->submit_copying; /* capture the policy WITH the op */
        s->inline_done      = 0;
        s->submitting       = 1;
        int r = s->submit_fn(s->submit_ctx, data, len);
        s->submitting       = 0;
        if (r != 0) {                            /* 0 = submitted; anything else = failed, and no */
            s->send_inflight = 0;                /* completion follows (one reported inline is */
            s->inflight_len  = 0;                /* void: the submission itself failed) */
            s->inline_done   = 0;
            s->wq_err        = 1;
            return -1;
        }
        if (s->inflight_copying)                 /* backend copied: free the queue now, but */
            kl_drain_consume(&s->wq, len);       /* send_inflight still blocks the next submit */
        if (!s->inline_done) return 0;           /* the usual case: the completion comes later */
        s->inline_done = 0;                      /* completed inside submit_fn: retire it here */
        if (!s->inline_ok) {                     /* delivery failed: as kl_stream_on_write_complete */
            s->send_inflight = 0;
            s->inflight_len  = 0;
            s->wq_err        = 1;
            return -1;
        }
        stream_send_retired(s);                  /* and pump the next batch */
    }
}

KlStreamWriteStatus kl_stream_write(KlStream *s, const char *data, size_t len) {
    if (!s || !s->wq_inited) return KL_STREAM_ERROR;
    if (s->wq_err) return KL_STREAM_ERROR;
    if (s->wq_closing) return KL_STREAM_CLOSED;   /* close in progress; refuse new writes */
    /* Fail closed if the adapter never installed a transport hook (readiness write_fn or a
     * completion submit); otherwise the readiness path would call a NULL write_fn. Not sticky:
     * a missing hook is a setup ordering issue, not a permanent stream error. Nothing is taken. */
    if (!s->submit_fn && !s->wq.write_fn) return KL_STREAM_ERROR;
    if (len == 0) return KL_STREAM_ACCEPTED;
    if (!data) return KL_STREAM_ERROR;

    if (s->submit_fn) {
        /* Completion mode: buffer the whole write atomically (no synchronous direct send;
         * output rides the async submit), then pump one send if none is in flight. */
        KlDrainWriteStatus ds = kl_drain_reserve_buffer(&s->wq, data, len);
        if (ds == KL_DRAIN_WOULD_BLOCK) s->wr_blocked = 1;
        if (ds != KL_DRAIN_ACCEPTED) return map_drain(ds);
        s->wr_blocked = 0;                         /* no longer blocked; before any inline completion */
        if (stream_pump_completion(s) < 0) return KL_STREAM_ERROR;  /* submit failed; bytes owned */
        return KL_STREAM_ACCEPTED;
    }

    /* Readiness mode: reserve + direct-send the prefix + buffer the remainder (atomic). */
    KlDrainWriteStatus ds = kl_drain_reserve_write(&s->wq, data, len);
    /* A writer failure is terminal for the write side, exactly as a failed completion send is:
     * record it so later writes report it and a graceful close does not wait on the queue. */
    if (ds == KL_DRAIN_WERROR) s->wq_err = 1;
    if (ds == KL_DRAIN_WOULD_BLOCK) s->wr_blocked = 1;
    if (ds == KL_DRAIN_ACCEPTED) s->wr_blocked = 0;
    return map_drain(ds);
}

int kl_stream_flush(KlStream *s) {
    if (!s || !s->wq_inited || s->wq_err) return -1;
    /* Readiness-only: completion output leaves via the submit hook, not a synchronous flush.
     * Fail closed in completion mode, and when no readiness writer is installed (kl_drain_flush
     * would call a NULL write_fn); never crash on an accidental flush. */
    if (s->submit_fn || !s->wq.write_fn) return -1;
    int r = kl_drain_flush(&s->wq);   /* 0 drained / 1 pending / -1 error */
    /* Readiness graceful close drains via successive writable flushes; a fully-drained queue means
     * the write side is retired; notify so close can finalize (no-op unless closing). A writer
     * failure is terminal: record it (sticky, like the completion path) and notify too, because
     * the queue can no longer drain and must stop holding a graceful close. */
    if (r < 0) s->wq_err = 1;
    int fired = stream_writable_edge(s);
    if (fired) {
        stream_fire_writable(s);
        /* The callback may have refilled the queue: report it, so the adapter keeps write interest. */
        if (r == 0 && kl_drain_buffered(&s->wq) > 0) r = 1;
    }
    /* Tail (may detach; on_close may free s). Also after the callback, which may have begun a close
     * whose finalize it deferred. */
    if ((r != 1 || fired) && s->on_retire) s->on_retire(s);
    return r;
}

int kl_stream_on_write_complete(KlStream *s, int ok) {
    if (!s || !s->wq_inited) return -1;
    if (!s->send_inflight) return s->wq_err ? -1 : 0;   /* spurious/duplicate completion */
    if (s->submitting) {                          /* inline, from inside submit_fn: the pump that */
        s->inline_done = 1;                       /* called it retires the op once it returns */
        s->inline_ok   = ok;
        return 0;
    }

    if (!ok) {
        /* Delivery failed. The completion has arrived, so the provider operation has RETIRED
         * and no longer references the queue. We still do NOT consume the queued bytes, but as
         * the stream's error/teardown policy (surface exactly what was unsent), not a
         * provider-lifetime requirement. (A copying backend already retired its copy at submit.)
         * Sticky error; do not pump. */
        s->send_inflight = 0;
        s->inflight_len  = 0;
        s->wq_err        = 1;
        if (stream_writable_edge(s)) stream_fire_writable(s);   /* wake a blocked producer */
        if (s->on_retire) s->on_retire(s);       /* send op physically retired; let close finalize */
        return -1;
    }
    stream_send_retired(s);
    int r = stream_pump_completion(s);           /* send the next queued batch, if any */
    if (stream_writable_edge(s)) stream_fire_writable(s);
    /* This send op retired. If pump re-submitted, send_inflight is set again and finalize will
     * see the write side as still busy (graceful drain continues); once the queue is empty and
     * nothing is re-submitted, this notification is what detaches the stream. */
    if (s->on_retire) s->on_retire(s);
    return r;
}

size_t kl_stream_write_pending(const KlStream *s) {
    if (!s || !s->wq_inited) return 0;
    size_t q = kl_drain_buffered(&s->wq);
    /* A copying backend consumed the in-flight bytes from the queue at submit, but they are
     * still pending (unacknowledged) on the wire; count them too, with an overflow guard. */
    if (s->inflight_copying && s->send_inflight) {
        if (s->inflight_len > (size_t)-1 - q) return (size_t)-1;   /* saturate; not reachable */
        return q + s->inflight_len;
    }
    return q;
}

int kl_stream_write_free(KlStream *s) {
    if (!s) return 0;
    if (s->send_inflight) return -1;   /* op outstanding; freeing would UAF provider storage */
    kl_drain_free(&s->wq);
    s->wq_inited     = 0;
    s->inflight_len  = 0;
    s->wq_err        = 0;
    s->wr_blocked    = 0;
    s->on_writable   = NULL;
    s->writable_ctx  = NULL;
    return 0;
}
