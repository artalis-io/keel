/*
 * stream_write.c — KlStream write machinery. See stream_write.h.
 *
 * Model-agnostic atomic write over a bounded, preallocated KlDrain queue. Readiness
 * drains via the queue's write_fn; completion posts one async send at a time via the submit
 * hook and pumps the next on the WRITE completion. TLS is above the raw stream (the adapter's
 * hooks encrypt); no TLS state here.
 */
#include "stream_write.h"
#include "drain_reserve.h"     /* kl_drain_prealloc / reserve_write / low-water */
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
    if (!s || s->wq_inited) return -1;
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

/* Completion pump: if no send is in flight and the queue has bytes, submit exactly one batch.
 * Returns 0 = ok (submitted, or nothing to do, or blocked by an in-flight send), -1 = the
 * submission failed (bytes remain owned in the queue; sticky error set). Ordering: send_inflight
 * blocks a second submit until kl_stream_on_write_complete clears it. */
static int stream_pump_completion(KlStream *s) {
    if (s->wq_err) return -1;
    if (s->send_inflight) return 0;              /* one in flight — wait for its completion */
    size_t len = kl_drain_buffered(&s->wq);
    if (len == 0) return 0;
    const char *data = kl_drain_data(&s->wq);
    int r = s->submit_fn(s->submit_ctx, data, len);
    if (r != 0) { s->wq_err = 1; return -1; }    /* 0 = submitted; anything else = failed. */
    s->send_inflight    = 1;
    s->send_cancel_requested = 0;                /* a genuinely new send op — not yet cancel-requested */
    s->inflight_len     = len;
    s->inflight_copying = s->submit_copying;     /* capture the policy WITH the op */
    if (s->inflight_copying)                     /* backend copied — free the queue now, but */
        kl_drain_consume(&s->wq, len);           /* send_inflight still blocks the next submit */
    return 0;
}

KlStreamWriteStatus kl_stream_write(KlStream *s, const char *data, size_t len) {
    if (!s || !s->wq_inited) return KL_STREAM_ERROR;
    if (s->wq_err) return KL_STREAM_ERROR;
    if (s->wq_closing) return KL_STREAM_CLOSED;   /* close in progress — refuse new writes */
    /* Fail closed if the adapter never installed a transport hook (readiness write_fn or a
     * completion submit) — otherwise the readiness path would call a NULL write_fn. Not sticky:
     * a missing hook is a setup ordering issue, not a permanent stream error. Nothing is taken. */
    if (!s->submit_fn && !s->wq.write_fn) return KL_STREAM_ERROR;
    if (len == 0) return KL_STREAM_ACCEPTED;
    if (!data) return KL_STREAM_ERROR;

    if (s->submit_fn) {
        /* Completion mode: buffer the whole write atomically (no synchronous direct send —
         * output rides the async submit), then pump one send if none is in flight. */
        KlDrainWriteStatus ds = kl_drain_reserve_buffer(&s->wq, data, len);
        if (ds != KL_DRAIN_ACCEPTED) return map_drain(ds);
        if (stream_pump_completion(s) < 0) return KL_STREAM_ERROR;  /* submit failed; bytes owned */
        return KL_STREAM_ACCEPTED;
    }

    /* Readiness mode: reserve + direct-send the prefix + buffer the remainder (atomic). */
    return map_drain(kl_drain_reserve_write(&s->wq, data, len));
}

int kl_stream_flush(KlStream *s) {
    if (!s || !s->wq_inited || s->wq_err) return -1;
    /* Readiness-only: completion output leaves via the submit hook, not a synchronous flush.
     * Fail closed in completion mode, and when no readiness writer is installed (kl_drain_flush
     * would call a NULL write_fn) — never crash on an accidental flush. */
    if (s->submit_fn || !s->wq.write_fn) return -1;
    int r = kl_drain_flush(&s->wq);   /* 0 drained / 1 pending / -1 error */
    /* Readiness graceful close drains via successive writable flushes; a fully-drained queue means
     * the write side is retired — notify so close can finalize (no-op unless closing). */
    if (r == 0 && s->on_retire) s->on_retire(s);
    return r;
}

int kl_stream_on_write_complete(KlStream *s, int ok) {
    if (!s || !s->wq_inited) return -1;
    if (!s->send_inflight) return s->wq_err ? -1 : 0;   /* spurious/duplicate completion */

    if (!ok) {
        /* Delivery failed. The completion has arrived, so the provider operation has RETIRED
         * and no longer references the queue. We still do NOT consume the queued bytes — but as
         * the stream's error/teardown policy (surface exactly what was unsent), not a
         * provider-lifetime requirement. (A copying backend already retired its copy at submit.)
         * Sticky error; do not pump. */
        s->send_inflight = 0;
        s->inflight_len  = 0;
        s->wq_err        = 1;
        if (s->on_retire) s->on_retire(s);       /* send op physically retired — let close finalize */
        return -1;
    }
    if (!s->inflight_copying)                     /* referencing backend: safe to free NOW */
        kl_drain_consume(&s->wq, s->inflight_len);
    s->send_inflight = 0;
    s->inflight_len  = 0;
    int r = stream_pump_completion(s);           /* send the next queued batch, if any */
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
     * still pending (unacknowledged) on the wire — count them too, with an overflow guard. */
    if (s->inflight_copying && s->send_inflight) {
        if (s->inflight_len > (size_t)-1 - q) return (size_t)-1;   /* saturate; not reachable */
        return q + s->inflight_len;
    }
    return q;
}

int kl_stream_write_free(KlStream *s) {
    if (!s) return 0;
    if (s->send_inflight) return -1;   /* op outstanding — freeing would UAF provider storage */
    kl_drain_free(&s->wq);
    s->wq_inited     = 0;
    s->inflight_len  = 0;
    s->wq_err        = 0;
    return 0;
}
