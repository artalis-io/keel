#include <keel/http_compress.h>
#include "http_response_internal.h"   /* kl_http_response_stream_admit */
#include <stdint.h>
#include <string.h>

/* True iff every REQUIRED KlCompress op is present. Core calls compress (single-shot),
 * feed (streaming), encoding, and destroy unconditionally; reset is optional (the
 * header documents it "may be NULL", and it is never called here). Validated once,
 * right after the factory returns the session (not in a hot path). A session missing
 * a required op is rejected; its destroy is guarded because a malformed table may
 * omit destroy, in which case the object cannot be freed generically. */
static int compress_vtable_valid(const KlCompress *c) {
    return c && c->compress && c->feed && c->encoding && c->destroy;
}

/* Append Content-Encoding and Vary as a pair. If either append fails, the header block is cut
 * back to where it was, so no Content-Encoding is left to label a body that is sent as is. */
static int add_encoding_headers(KlHttpResponse *res, const char *enc) {
    size_t mark = res->hdr_len;
    if (kl_http_response_header(res, "Content-Encoding", enc) < 0 ||
        kl_http_response_header(res, "Vary", "Accept-Encoding") < 0) {
        res->hdr_len = mark;
        return -1;
    }
    return 0;
}

int kl_http_response_body_compress(KlHttpResponse *res, KlCompressConfig *cfg,
                               const char *data, size_t len) {
    if (!res || !cfg || !cfg->factory) return -1;
    if (len > 0 && !data) return -1;

    KlAllocator *alloc = res->alloc;

    /* Empty body: just set empty buffer body */
    if (len == 0) {
        res->body_mode = KL_HTTP_BODY_BUFFER;
        res->body = "";
        res->body_len = 0;
        return 0;
    }

    /* Create compression session */
    KlCompress *comp = cfg->factory(cfg->ctx, alloc);
    if (!comp) return -1;
    if (!compress_vtable_valid(comp)) {
        if (comp->destroy) comp->destroy(comp);   /* guard: a malformed table may omit destroy */
        return -1;
    }

    /* Compress */
    char *out = NULL;
    size_t out_len = 0;
    int rc = comp->compress(comp, data, len, &out, &out_len, alloc);
    if (rc < 0) {
        comp->destroy(comp);
        return -1;
    }

    /* Expansion check: if compressed is not smaller, use original */
    if (out_len >= len) {
        kl_free(alloc, out, out_len);
        comp->destroy(comp);
        kl_http_response_body_borrow(res, data, len);
        return 0;
    }

    /* Set Content-Encoding and Vary headers */
    const char *enc = comp->encoding(comp);
    if (add_encoding_headers(res, enc) < 0) {
        kl_free(alloc, out, out_len);
        comp->destroy(comp);
        return -1;
    }

    /* Store as owned body */
    if (res->body_owned) {
        kl_free(alloc, res->body_owned, res->body_owned_size);
    }
    res->body_owned = out;
    res->body_owned_size = out_len;
    res->body_mode = KL_HTTP_BODY_BUFFER;
    res->body = res->body_owned;
    res->body_len = out_len;

    comp->destroy(comp);
    return 0;
}

/* ── Bounded streaming: one flush's output, staged, then admitted as one chunk ────────────── */

static void stage_release(KlHttpCompressStream *cs) {
    if (cs->stage) kl_free(cs->alloc, cs->stage, cs->stage_cap);
    cs->stage = NULL;
    cs->stage_cap = 0;
}

/* Grow the staging buffer to `need` before the codec is touched, so a failed allocation leaves the
 * codec as it was. It only grows (to the largest bound admitted), and is freed when the stream ends. */
static int stage_reserve(KlHttpCompressStream *cs, size_t need) {
    if (need <= cs->stage_cap) return 0;
    char *nb = kl_malloc(cs->alloc, need);
    if (!nb) return -1;
    stage_release(cs);
    cs->stage = nb;
    cs->stage_cap = need;
    return 0;
}

typedef struct {
    char  *buf;
    size_t cap;   /* the bound the output was admitted against */
    size_t len;
} StageSink;

/* Collect the codec's output for one feed. More than its bound is a codec defect: refuse it, which
 * fails the feed (and the stream), rather than let the chunk outgrow what was admitted. */
static int stage_emit(void *ctx, const char *data, size_t len) {
    StageSink *k = ctx;
    if (len > k->cap - k->len) return -1;
    if (len) memcpy(k->buf + k->len, data, len);
    k->len += len;
    return 0;
}

/* A bounded write that failed once admitted: the stream and the response's stream both fail, so the
 * connection closes rather than carry a body the client cannot decode (a later write through the
 * response itself is refused too). */
static KlHttpStreamStatus bounded_fail(KlHttpCompressStream *cs) {
    cs->error = 1;
    cs->res->stream_error = 1;
    return KL_HTTP_STREAM_ERROR;
}

/* Admit the codec's worst case for (len, final), then feed it, flushing, into the staging buffer and
 * hand the output to the response as one chunk. Everything that can refuse runs before the feed. */
static KlHttpStreamStatus bounded_feed(KlHttpCompressStream *cs, const char *data, size_t len,
                                       int final) {
    size_t bound = cs->comp->flush_bound(cs->comp, len, final);
    KlHttpStreamStatus st = kl_http_response_stream_admit(cs->res, bound);
    if (st != KL_HTTP_STREAM_ACCEPTED) {
        if (st == KL_HTTP_STREAM_ERROR) cs->error = 1;
        return st;
    }
    if (stage_reserve(cs, bound) < 0) return bounded_fail(cs);
    StageSink k = { cs->stage, bound, 0 };
    if (cs->comp->feed(cs->comp, data, len, final ? 1 : KL_COMPRESS_FLUSH_SYNC, stage_emit, &k) < 0)
        return bounded_fail(cs);             /* the codec may have advanced: nothing can resume it */
    /* At most `bound` bytes, which were admitted with nothing changed since: this is ACCEPTED (a
     * zero-length one emits nothing and disarms the edge, as an accepted write does). Anything else
     * means the output is lost while the codec has moved on. */
    st = kl_http_response_stream_try_write(cs->res, k.buf, k.len);
    if (st != KL_HTTP_STREAM_ACCEPTED) return bounded_fail(cs);
    return KL_HTTP_STREAM_ACCEPTED;
}

void kl_http_compress_stream_release(KlHttpCompressStream *cs) {
    if (!cs || !cs->comp) return;
    cs->comp->destroy(cs->comp);
    cs->comp = NULL;
    stage_release(cs);
}

int kl_http_compress_stream_bounded(const KlHttpCompressStream *cs) {
    return cs && cs->comp && cs->comp->flush_bound;
}

KlHttpStreamStatus kl_http_compress_stream_try_write(KlHttpCompressStream *cs, const char *data,
                                                      size_t len) {
    if (!cs) return KL_HTTP_STREAM_ERROR;
    if (!cs->comp) return KL_HTTP_STREAM_CLOSED;
    if (cs->error) return KL_HTTP_STREAM_ERROR;
    if ((len > 0 && !data) || !cs->comp->flush_bound) {
        cs->error = 1;
        return KL_HTTP_STREAM_ERROR;
    }
    KlHttpStreamStatus st;
    /* Nothing to send (an empty write, or a HEAD response, whose body never leaves): the response's
     * own answer for it (ACCEPTED, or CLOSED / ERROR), without running or sizing the codec. */
    if (len == 0 || cs->res->head_request)
        st = kl_http_response_stream_try_write(cs->res, NULL, 0);
    else
        st = bounded_feed(cs, data, len, 0);
    /* CLOSED is final: the response's stream ended or was reset or released, and no later call on
     * this stream can write again. Release the codec and staging now, as try_end does, rather than
     * hold them (miniz: about 300 KiB) until the caller thinks to. Releasing again is safe. */
    if (st == KL_HTTP_STREAM_CLOSED) kl_http_compress_stream_release(cs);
    return st;
}

KlHttpStreamStatus kl_http_compress_stream_try_end(KlHttpCompressStream *cs) {
    if (!cs) return KL_HTTP_STREAM_ERROR;
    if (!cs->comp) return KL_HTTP_STREAM_CLOSED;
    KlHttpStreamStatus st;
    if (cs->error || !cs->comp->flush_bound) {
        cs->error = 1;
        st = KL_HTTP_STREAM_ERROR;
    } else if (cs->res->head_request) {
        /* No body leaves a HEAD response: end it without sizing or running the codec (whatever
         * a legacy write left in it is never sent either). */
        st = kl_http_response_stream_try_write(cs->res, NULL, 0);
        if (st == KL_HTTP_STREAM_ACCEPTED && kl_http_response_end_stream(cs->res) < 0) {
            cs->error = 1;
            st = KL_HTTP_STREAM_ERROR;
        }
    } else {
        st = bounded_feed(cs, NULL, 0, 1);
        if (st == KL_HTTP_STREAM_WOULD_BLOCK || st == KL_HTTP_STREAM_TOO_LARGE) return st;
        /* The terminal chunk is never refused by the capacity (a reserved allowance). */
        if (st == KL_HTTP_STREAM_ACCEPTED && kl_http_response_end_stream(cs->res) < 0) {
            cs->error = 1;
            st = KL_HTTP_STREAM_ERROR;
        }
    }
    kl_http_compress_stream_release(cs);
    return st;
}

/* Emit callback for streaming: forwards compressed data to chunked stream */
static int stream_emit(void *ctx, const char *data, size_t len) {
    KlHttpCompressStream *cs = ctx;
    if (cs->error) return -1;
    if (len == 0) return 0;
    if (cs->write_fn(cs->write_ctx, data, len) < 0) {
        cs->error = 1;
        return -1;
    }
    return 0;
}

int kl_http_compress_stream_begin(KlHttpResponse *res, KlCompressConfig *cfg,
                              int status, KlHttpCompressStream *cs) {
    if (!res || !cfg || !cfg->factory || !cs) return -1;
    /* Refused before the codec is made or a header added: the response already has a stream. */
    if (!kl_http_response_stream_can_begin(res)) return -1;

    KlAllocator *alloc = res->alloc;
    memset(cs, 0, sizeof(*cs));

    /* Create compression session */
    KlCompress *comp = cfg->factory(cfg->ctx, alloc);
    if (!comp) return -1;
    if (!compress_vtable_valid(comp)) {
        if (comp->destroy) comp->destroy(comp);   /* guard: a malformed table may omit destroy */
        return -1;
    }

    /* Set Content-Encoding and Vary headers */
    const char *enc = comp->encoding(comp);
    if (add_encoding_headers(res, enc) < 0) {
        comp->destroy(comp);
        return -1;
    }

    /* Start chunked stream */
    KlHttpResponseWriteFn write_fn;
    void *write_ctx;
    if (kl_http_response_begin_stream(res, status, &write_fn, &write_ctx) < 0) {
        comp->destroy(comp);
        return -1;
    }

    cs->comp = comp;
    cs->write_fn = write_fn;
    cs->write_ctx = write_ctx;
    cs->res = res;
    cs->alloc = alloc;
    cs->error = 0;
    return 0;
}

int kl_http_compress_stream_write(KlHttpCompressStream *cs, const char *data,
                              size_t len) {
    if (!cs || !cs->comp) return -1;
    if (cs->error) return -1;
    if (len > 0 && !data) return -1;
    /* The response's stream ended (or was reset or released): nothing may be appended to it, and
     * the codec is not fed input whose output could never be sent. */
    if (!kl_http_response_stream_open(cs->res)) return -1;
    if (len == 0) return 0;

    if (cs->comp->feed(cs->comp, data, len, 0, stream_emit, cs) < 0) {
        cs->error = 1;
        return -1;
    }
    return 0;
}

int kl_http_compress_stream_end(KlHttpCompressStream *cs) {
    if (!cs || !cs->comp) return -1;

    int rc = 0;

    /* Final flush: emit remaining compressed data + trailer. Not once the response's stream has
     * ended (kl_http_response_end_stream called directly) or was reset or released: the codec's
     * finish would follow the terminating chunk on the wire, where the client reads it as the start
     * of the next response. The body the client got is then a truncated member, so this fails. */
    if (!kl_http_response_stream_open(cs->res)) {
        cs->error = 1;
        rc = -1;
    } else if (!cs->error) {
        if (cs->comp->feed(cs->comp, NULL, 0, 1, stream_emit, cs) < 0) {
            cs->error = 1;
            rc = -1;
        }
    } else {
        rc = -1;
    }

    /* Destroy compression session */
    cs->comp->destroy(cs->comp);
    cs->comp = NULL;
    stage_release(cs);

    /* End chunked stream */
    if (rc == 0) {
        if (kl_http_response_end_stream(cs->res) < 0)
            rc = -1;
    }

    return rc;
}
