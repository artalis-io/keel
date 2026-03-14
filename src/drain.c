#include <keel/drain.h>
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
    ssize_t n = d->write_fn(data, len, d->write_ctx);
    if (n < 0) {
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

int kl_drain_flush(KlDrain *d) {
    if (!d) return -1;
    if (d->error) return -1;
    if (d->buf_len == 0) return 0;

    while (d->buf_len > 0) {
        ssize_t n = d->write_fn(d->buf, d->buf_len, d->write_ctx);
        if (n < 0) {
            d->error = 1;
            return -1;
        }
        if (n == 0) return 1;  /* would-block, more pending */

        size_t written = (size_t)n;
        d->buf_len -= written;
        if (d->buf_len > 0)
            memmove(d->buf, d->buf + written, d->buf_len);
    }

    /* Buffer drained — fire callback */
    if (d->on_drain)
        d->on_drain(d->drain_ctx);
    return 0;
}

int kl_drain_pending(const KlDrain *d) {
    if (!d) return 0;
    return d->buf_len > 0 ? 1 : 0;
}

size_t kl_drain_buffered(const KlDrain *d) {
    if (!d) return 0;
    return d->buf_len;
}

void kl_drain_free(KlDrain *d) {
    if (!d) return;
    if (d->buf) {
        kl_free(d->alloc, d->buf, d->buf_cap);
        d->buf     = NULL;
        d->buf_len = 0;
        d->buf_cap = 0;
    }
}
