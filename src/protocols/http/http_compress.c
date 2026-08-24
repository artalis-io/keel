#include <keel/http_compress.h>
#include <string.h>

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
    if (kl_http_response_header(res, "Content-Encoding", enc) < 0 ||
        kl_http_response_header(res, "Vary", "Accept-Encoding") < 0) {
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

    KlAllocator *alloc = res->alloc;
    memset(cs, 0, sizeof(*cs));

    /* Create compression session */
    KlCompress *comp = cfg->factory(cfg->ctx, alloc);
    if (!comp) return -1;

    /* Set Content-Encoding and Vary headers */
    const char *enc = comp->encoding(comp);
    if (kl_http_response_header(res, "Content-Encoding", enc) < 0 ||
        kl_http_response_header(res, "Vary", "Accept-Encoding") < 0) {
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

    /* Final flush: emit remaining compressed data + trailer */
    if (!cs->error) {
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

    /* End chunked stream */
    if (rc == 0) {
        if (kl_http_response_end_stream(cs->res) < 0)
            rc = -1;
    }

    return rc;
}
