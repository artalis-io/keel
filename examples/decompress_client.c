/*
 * decompress_client.c: Client-side response decompression with miniz
 *
 * Concepts: KlDecompressConfig, kl_decompress_miniz_create, automatic
 * Content-Encoding: gzip decompression in KlHttpClientConfig.
 *
 * Demonstrates:
 *   1. One-shot compress → decompress round-trip (via vtable)
 *   2. Streaming decompress in small chunks
 *   3. Client integration usage
 *
 * Build:  make examples KEEL_COMPRESS=miniz MINIZ_DIR=/path/to/miniz
 * Run:    ./examples/decompress_client
 *
 * No server required; compresses data locally, then decompresses
 * to verify the round-trip.
 */

#include <keel/keel.h>
#include "compress_miniz.h"
#include "decompress_miniz.h"
#include <stdio.h>
#include <string.h>

/* Collector for streaming output */
typedef struct {
    char buf[1024];
    size_t len;
} Collector;

static int collect_fn(void *ctx, const char *data, size_t len) {
    Collector *c = ctx;
    if (c->len + len > sizeof(c->buf)) return -1;
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    return 0;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();

    /* Create shared gzip context (level 6 = default) */
    KlCompressCtx *cctx = kl_compress_miniz_ctx_create(6, &alloc);
    if (!cctx) { fprintf(stderr, "compress ctx failed\n"); return 1; }

    /* ── 1. Compress data via vtable ───────────────────────────────── */

    KlCompress *comp = kl_compress_miniz_create(cctx, &alloc);
    if (!comp) {
        fprintf(stderr, "compress create failed\n");
        kl_compress_miniz_ctx_destroy(cctx);
        return 1;
    }

    const char *original = "Hello from Keel! This text will be gzip-compressed "
                           "and then decompressed to verify the round-trip works "
                           "correctly with the pluggable decompression vtable.";
    size_t original_len = strlen(original);

    char *compressed = NULL;
    size_t compressed_len = 0;

    if (comp->compress(comp, original, original_len,
                       &compressed, &compressed_len, &alloc) < 0) {
        fprintf(stderr, "compression failed\n");
        comp->destroy(comp);
        kl_compress_miniz_ctx_destroy(cctx);
        return 1;
    }
    comp->destroy(comp);

    printf("Original:   %zu bytes\n", original_len);
    printf("Compressed: %zu bytes\n", compressed_len);

    /* ── 2. Decompress data (one-shot) ─────────────────────────────── */

    KlDecompressConfig dcfg = {
        .ctx = cctx,
        .factory = kl_decompress_miniz_create,
        .ctx_destroy = kl_compress_miniz_ctx_destroy,
    };

    char *decompressed = NULL;
    size_t decompressed_len = 0;

    if (kl_decompress_body(&dcfg, compressed, compressed_len,
                            &decompressed, &decompressed_len, &alloc) < 0) {
        fprintf(stderr, "decompression failed\n");
        kl_free(&alloc, compressed, compressed_len);
        kl_compress_miniz_ctx_destroy(cctx);
        return 1;
    }

    printf("Decompressed: %zu bytes\n", decompressed_len);

    /* Verify round-trip */
    if (decompressed_len == original_len &&
        memcmp(decompressed, original, original_len) == 0) {
        printf("Round-trip: OK\n");
    } else {
        printf("Round-trip: FAIL\n");
        kl_free(&alloc, compressed, compressed_len);
        kl_free(&alloc, decompressed, decompressed_len);
        kl_compress_miniz_ctx_destroy(cctx);
        return 1;
    }

    /* ── 3. Streaming decompress ───────────────────────────────────── */

    printf("\nStreaming decompress:\n");

    KlDecompressStream ds;
    if (kl_decompress_stream_init(&ds, &dcfg, &alloc) < 0) {
        fprintf(stderr, "stream init failed\n");
        kl_free(&alloc, compressed, compressed_len);
        kl_free(&alloc, decompressed, decompressed_len);
        kl_compress_miniz_ctx_destroy(cctx);
        return 1;
    }

    size_t chunk_size = 16;
    Collector collector = { .len = 0 };

    for (size_t off = 0; off < compressed_len; off += chunk_size) {
        size_t n = compressed_len - off;
        if (n > chunk_size) n = chunk_size;
        int is_last = (off + n >= compressed_len);

        if (kl_decompress_stream_feed(&ds, compressed + off, n, is_last,
                                       collect_fn, &collector) < 0) {
            fprintf(stderr, "stream feed failed at offset %zu\n", off);
            kl_decompress_stream_free(&ds);
            kl_free(&alloc, compressed, compressed_len);
            kl_free(&alloc, decompressed, decompressed_len);
            kl_compress_miniz_ctx_destroy(cctx);
            return 1;
        }
    }

    printf("  Fed %zu bytes in %zu-byte chunks\n", compressed_len, chunk_size);
    printf("  Output: %zu bytes\n", collector.len);

    if (collector.len == original_len &&
        memcmp(collector.buf, original, original_len) == 0) {
        printf("  Streaming round-trip: OK\n");
    } else {
        printf("  Streaming round-trip: FAIL\n");
    }

    kl_decompress_stream_free(&ds);

    /* ── 4. Client usage note ──────────────────────────────────────── */

    printf("\nClient integration:\n");
    printf("  KlDecompressConfig decomp = { .ctx, .factory, .ctx_destroy };\n");
    printf("  KlHttpClientConfig cfg = { .decompress = &decomp };\n");
    printf("  // Responses with Content-Encoding: gzip are auto-decompressed\n");

    /* Cleanup */
    kl_free(&alloc, compressed, compressed_len);
    kl_free(&alloc, decompressed, decompressed_len);
    kl_compress_miniz_ctx_destroy(cctx);
    return 0;
}
