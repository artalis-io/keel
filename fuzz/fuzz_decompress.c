/*
 * fuzz_decompress.c: libFuzzer target for gzip/deflate decompression.
 *
 * The client decompresses UNTRUSTED server response bodies. This feeds arbitrary
 * bytes through both the single-shot (kl_decompress_body) and streaming
 * (kl_decompress_stream_*) paths of the miniz backend. It exercises:
 *   - malformed gzip header / trailer / CRC / ISIZE handling,
 *   - the growth loop and streaming emit path,
 *   - the anti-decompression-bomb output cap (a broken cap would let output grow
 *     unbounded; libFuzzer's malloc/rss limit flags that; ASan/UBSan catch OOB).
 *
 * Seed the corpus with a real gzip stream so mutations reach the inflate paths.
 *
 * Requires the miniz backend in the lib (not part of the default build):
 *   make fuzz-decompress KEEL_COMPRESS=miniz MINIZ_DIR=/path/to/miniz CC=clang
 * Run:    ./fuzz/fuzz_decompress fuzz/corpus_decompress/
 */
#include <keel/decompress.h>
#include "decompress_miniz.h"
#include "compress_miniz.h"
#include <keel/allocator.h>

#include <stddef.h>
#include <stdint.h>

static int emit_sink(void *ctx, const char *data, size_t len) {
    (void)ctx; (void)data; (void)len;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    KlAllocator alloc = kl_allocator_default();
    KlCompressCtx *ctx = kl_compress_miniz_ctx_create(6, &alloc);
    if (!ctx) return 0;
    KlDecompressConfig cfg = { .ctx = ctx, .factory = kl_decompress_miniz_create,
                               .ctx_destroy = NULL };

    /* Single-shot */
    char *out = NULL;
    size_t out_len = 0;
    if (kl_decompress_body(&cfg, (const char *)data, size, &out, &out_len, &alloc) == 0
        && out)
        kl_free(&alloc, out, out_len);

    /* Streaming: feed the bytes, then final-flush to validate the trailer. */
    KlDecompressStream ds;
    if (kl_decompress_stream_init(&ds, &cfg, &alloc) == 0) {
        kl_decompress_stream_feed(&ds, (const char *)data, size, 0, emit_sink, NULL);
        kl_decompress_stream_feed(&ds, NULL, 0, 1, emit_sink, NULL);
        kl_decompress_stream_free(&ds);
    }

    kl_compress_miniz_ctx_destroy(ctx);
    return 0;
}
