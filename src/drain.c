#include <keel/drain.h>
#include "drain_reserve.h"   /* Phase-B internal reservation + low-water API */
#include <string.h>
#include <stdint.h>

#define DRAIN_INIT_CAP 4096

void kl_drain_init(KlDrain *d, KlDrainWriteFn write_fn, void *write_ctx,
                   KlAllocator *alloc) {
    if (!d) return;
    d->write_fn  = write_fn;
    d->write_ctx = write_ctx;
    d->on_drain  = NULL;
    d->drain_ctx = NULL;
    d->alloc     = alloc;
    d->buf       = NULL;
    d->buf_len   = 0;
    d->buf_cap   = 0;
    d->max_size  = 0;
    d->error     = 0;
    d->prealloc     = 0;
    d->low_water    = 0;
    d->on_writable  = NULL;
    d->writable_ctx = NULL;
}

void kl_drain_set_max_size(KlDrain *d, size_t max_size) {
    if (!d) return;
    d->max_size = max_size;
}

void kl_drain_on_drain(KlDrain *d, KlDrainCb cb, void *ctx) {
    if (!d) return;
    d->on_drain  = cb;
    d->drain_ctx = ctx;
}

/* Ensure buffer can hold at least `need` more bytes. Returns -1 on failure. */
static int drain_ensure(KlDrain *d, size_t need) {
    size_t required = d->buf_len + need;
    if (required < d->buf_len) return -1;  /* overflow */
    if (d->max_size > 0 && required > d->max_size) {
        d->error = 1;
        return -1;
    }
    if (required <= d->buf_cap) return 0;

    /* Grow: start at DRAIN_INIT_CAP, double until sufficient */
    size_t cap = d->buf_cap ? d->buf_cap : DRAIN_INIT_CAP;
    while (cap < required) {
        if (cap > SIZE_MAX / 2) {
            d->error = 1;
            return -1;
        }
        cap *= 2;
    }
    /* Clamp to max_size */
    if (d->max_size > 0 && cap > d->max_size)
        cap = d->max_size;

    char *nb = kl_realloc(d->alloc, d->buf, d->buf_cap, cap);
    if (!nb) {
        d->error = 1;
        return -1;
    }
    d->buf     = nb;
    d->buf_cap = cap;
    return 0;
}

int kl_drain_write(KlDrain *d, const char *data, size_t len) {
    if (!d) return -1;
    if (d->error) return -1;
    if (len == 0) return 0;
    if (!data) return -1;

    /* If buffer non-empty, must append to preserve ordering */
    if (d->buf_len > 0) {
        if (drain_ensure(d, len) < 0) return -1;
        memcpy(d->buf + d->buf_len, data, len);
        d->buf_len += len;
        return 0;
    }

    /* Try direct write */
    kl_ssize_t n = d->write_fn(data, len, d->write_ctx);
    if (n < 0) {
        d->error = 1;
        return -1;
    }
    if ((size_t)n > len) {
        d->error = 1;
        return -1;
    }
    if ((size_t)n == len) return 0;  /* all written */

    /* Buffer the remainder */
    size_t remain = len - (size_t)n;
    if (drain_ensure(d, remain) < 0) return -1;
    memcpy(d->buf + d->buf_len, data + n, remain);
    d->buf_len += remain;
    return 0;
}

/* Did buffered output cross from above the low-water mark to at or below it? */
static int drain_crossed_low_water(const KlDrain *d, size_t prev_len) {
    return d->on_writable && d->low_water > 0 &&
           prev_len > d->low_water && d->buf_len <= d->low_water;
}

/* Fire AT MOST ONE externally-reentrant callback after state is fully updated, per the
 * sequencing rule (docs/stream_contract.md §4). Precedence: a low-water writable crossing
 * (on_writable) suppresses on_drain for that transition — a caller using both gets the
 * writable signal, not a duplicate empty one. on_writable is a *writable* signal (it may
 * synchronously refill the buffer but must NOT free the drain), so d stays live and the
 * caller re-checks buf_len afterwards. on_drain is the legacy empty callback and is treated
 * as a destructive tail: nothing touches d after it. Returns the flush result recomputed
 * across a possible refill (0 = drained, 1 = more pending). */
static int drain_notify(KlDrain *d, size_t prev_len) {
    if (drain_crossed_low_water(d, prev_len)) {
        d->on_writable(d->writable_ctx);      /* may refill; must not free d */
        return d->buf_len > 0 ? 1 : 0;        /* re-check: honor a synchronous refill */
    }
    int drained = (d->buf_len == 0);
    if (drained && d->on_drain)
        d->on_drain(d->drain_ctx);            /* destructive tail — do NOT touch d after */
    return drained ? 0 : 1;
}

int kl_drain_flush(KlDrain *d) {
    if (!d) return -1;
    if (d->error) return -1;
    if (d->buf_len == 0) return 0;

    size_t prev_len = d->buf_len;
    while (d->buf_len > 0) {
        kl_ssize_t n = d->write_fn(d->buf, d->buf_len, d->write_ctx);
        if (n < 0) {
            d->error = 1;
            return -1;
        }
        if (n == 0) break;  /* would-block, more pending */

        size_t written = (size_t)n;
        if (written > d->buf_len) {
            d->error = 1;
            return -1;
        }
        d->buf_len -= written;
        if (d->buf_len > 0)
            memmove(d->buf, d->buf + written, d->buf_len);
    }

    /* State fully updated — now fire at most one callback (may refill on_writable). */
    return drain_notify(d, prev_len);
}

int kl_drain_pending(const KlDrain *d) {
    if (!d) return 0;
    return d->buf_len > 0 ? 1 : 0;
}

size_t kl_drain_buffered(const KlDrain *d) {
    if (!d) return 0;
    return d->buf_len;
}

const char *kl_drain_data(const KlDrain *d) {
    if (!d || d->buf_len == 0) return NULL;
    return d->buf;
}

void kl_drain_consume(KlDrain *d, size_t n) {
    if (!d || n == 0) return;
    size_t prev_len = d->buf_len;
    if (n >= d->buf_len) {
        d->buf_len = 0;
    } else {
        d->buf_len -= n;
        memmove(d->buf, d->buf + n, d->buf_len);
    }
    /* State fully updated — fire at most one callback (on_writable crossing precedes the
     * empty on_drain; see drain_notify). */
    (void)drain_notify(d, prev_len);
}

/* ── Phase-B reservation + low-water extension (drain_reserve.h) ─────────────── */

void kl_drain_set_low_water(KlDrain *d, size_t low_water) {
    if (!d) return;
    d->low_water = low_water;
}

void kl_drain_on_writable(KlDrain *d, KlDrainCb cb, void *ctx) {
    if (!d) return;
    d->on_writable  = cb;
    d->writable_ctx = ctx;
}

int kl_drain_prealloc(KlDrain *d, size_t capacity) {
    if (!d || !d->alloc || capacity == 0) return -1;
    /* Init-time only: a fresh, unused drain. No conversion/resize of an in-use drain — that
     * would make the logical capacity ambiguous. A later explicit resize op can add defined
     * grow/shrink semantics if ever needed. */
    if (d->buf || d->buf_len != 0 || d->buf_cap != 0 || d->prealloc) return -1;
    char *nb = kl_malloc(d->alloc, capacity);   /* exactly one allocation, here */
    if (!nb) return -1;                         /* failure — non-prealloc mode preserved */
    d->buf      = nb;
    d->buf_cap  = capacity;
    d->prealloc = 1;
    d->max_size = capacity;                     /* the reserved capacity IS the hard cap */
    return 0;
}

KlDrainWriteStatus kl_drain_reserve_write(KlDrain *d, const char *data, size_t len) {
    if (!d || d->error) return KL_DRAIN_WERROR;
    if (len == 0) return KL_DRAIN_ACCEPTED;
    if (!data) return KL_DRAIN_WERROR;

    /* Reservation requires the FULL invariant: preallocated fixed buffer. max_size alone must
     * NOT enable it (an ordinary growable drain has no reserved storage — copying a remainder
     * into d->buf == NULL would crash). Fail closed otherwise. */
    if (!d->prealloc || !d->buf || d->buf_cap == 0) return KL_DRAIN_WERROR;
    if (d->buf_len > d->buf_cap) { d->error = 1; return KL_DRAIN_WERROR; }  /* corrupt state */
    size_t capacity = d->buf_cap;

    if (len > capacity) return KL_DRAIN_TOO_LARGE;          /* permanent — caller must chunk */
    if (len > capacity - d->buf_len) return KL_DRAIN_WOULD_BLOCK;  /* no room now; nothing taken */

    /* Reservation succeeded: the whole remainder is guaranteed to fit. Preserve ordering —
     * append while the buffer is non-empty; otherwise try one direct send, then buffer the
     * remainder. Neither path allocates (space is reserved within buf_cap). */
    if (d->buf_len == 0) {
        kl_ssize_t n = d->write_fn(data, len, d->write_ctx);
        if (n < 0) { d->error = 1; return KL_DRAIN_WERROR; }
        if ((size_t)n > len) { d->error = 1; return KL_DRAIN_WERROR; }
        if ((size_t)n == len) return KL_DRAIN_ACCEPTED;    /* all sent inline — zero-copy fast path */
        size_t remain = len - (size_t)n;
        memcpy(d->buf, data + n, remain);                  /* into the reserved space; no alloc */
        d->buf_len = remain;
        return KL_DRAIN_ACCEPTED;
    }
    memcpy(d->buf + d->buf_len, data, len);                /* append; reserved, no alloc */
    d->buf_len += len;
    return KL_DRAIN_ACCEPTED;
}

KlDrainWriteStatus kl_drain_reserve_buffer(KlDrain *d, const char *data, size_t len) {
    if (!d || d->error) return KL_DRAIN_WERROR;
    if (len == 0) return KL_DRAIN_ACCEPTED;
    if (!data) return KL_DRAIN_WERROR;

    /* Same full reservation invariant + capacity checks as kl_drain_reserve_write, but no
     * direct send: the whole write is copied into the reserved queue (completion path). */
    if (!d->prealloc || !d->buf || d->buf_cap == 0) return KL_DRAIN_WERROR;
    if (d->buf_len > d->buf_cap) { d->error = 1; return KL_DRAIN_WERROR; }
    size_t capacity = d->buf_cap;
    if (len > capacity) return KL_DRAIN_TOO_LARGE;
    if (len > capacity - d->buf_len) return KL_DRAIN_WOULD_BLOCK;

    memcpy(d->buf + d->buf_len, data, len);
    d->buf_len += len;
    return KL_DRAIN_ACCEPTED;
}

void kl_drain_free(KlDrain *d) {
    if (!d) return;
    if (d->buf) {
        kl_free(d->alloc, d->buf, d->buf_cap);
        d->buf     = NULL;
        d->buf_len = 0;
        d->buf_cap = 0;
    }
    d->prealloc = 0;   /* buffer gone — no longer in reservation mode */
}
