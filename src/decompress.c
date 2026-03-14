#include <keel/decompress.h>
#include <string.h>

int kl_decompress_body(KlDecompressConfig *cfg, const char *in, size_t in_len,
                        char **out, size_t *out_len, KlAllocator *alloc) {
    if (!cfg || !cfg->factory || !out || !out_len || !alloc)
        return -1;
    if (in_len > 0 && !in)
        return -1;

    /* Empty input — empty output */
    if (in_len == 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }

    /* Create decompression session */
    KlDecompress *decomp = cfg->factory(cfg->ctx, alloc);
    if (!decomp)
        return -1;

    /* Decompress */
    int rc = decomp->decompress(decomp, in, in_len, out, out_len, alloc);
    decomp->destroy(decomp);
    return rc;
}

int kl_decompress_stream_init(KlDecompressStream *ds, KlDecompressConfig *cfg,
                               KlAllocator *alloc) {
    if (!ds || !cfg || !cfg->factory || !alloc)
        return -1;

    memset(ds, 0, sizeof(*ds));

    KlDecompress *decomp = cfg->factory(cfg->ctx, alloc);
    if (!decomp)
        return -1;

    ds->decomp = decomp;
    ds->alloc = alloc;
    ds->error = 0;
    return 0;
}

int kl_decompress_stream_feed(KlDecompressStream *ds,
                               const char *data, size_t len, int flush,
                               int (*emit)(void *ctx, const char *data,
                                           size_t len),
                               void *emit_ctx) {
    if (!ds || !ds->decomp)
        return -1;
    if (ds->error)
        return -1;

    if (ds->decomp->dfeed(ds->decomp, data, len, flush, emit, emit_ctx) < 0) {
        ds->error = 1;
        return -1;
    }
    return 0;
}

void kl_decompress_stream_free(KlDecompressStream *ds) {
    if (!ds || !ds->decomp)
        return;

    ds->decomp->destroy(ds->decomp);
    ds->decomp = NULL;
}
