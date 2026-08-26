/*
 * Regression tests for the miniz streaming gzip trailer (CRC32 + ISIZE) verification
 * in decompress_miniz.c. Before the fix, verification only ran on a final flush=1 call
 * with a full 8-byte trailer, so a corrupt or truncated stream fed without a trailing
 * flush (or with a short trailer) was accepted as valid. These tests drive the public
 * KlDecompress.dfeed with a known "hello world" gzip stream in the scenarios that gap
 * allowed. Opt-in (bring-your-own miniz): built via `make decompress-miniz-test
 * MINIZ_DIR=...`; the miniz backend is otherwise not CI-gated.
 */
#include "utest.h"
#include <keel/allocator.h>
#include <keel/compress.h>
#include <keel/decompress.h>
#include "compress_miniz.h"     /* kl_compress_miniz_ctx_create/destroy */
#include "decompress_miniz.h"   /* kl_decompress_miniz_create */
#include <string.h>

/* `printf 'hello world' | gzip -n -c` -- 31 bytes: 10 header, 13 deflate, 8 trailer.
 * Trailer is bytes [23..30]: CRC32 (LE) 85 11 4a 0d, ISIZE (LE) 0b 00 00 00 (= 11). */
static const unsigned char GZ[] = {
    0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0xcb,0x48,
    0xcd,0xc9,0xc9,0x57,0x28,0xcf,0x2f,0xca,0x49,0x01,0x00,0x85,
    0x11,0x4a,0x0d,0x0b,0x00,0x00,0x00
};
#define GZ_LEN (sizeof(GZ))
#define TRAILER_CRC0 23   /* first CRC32 byte  */
#define TRAILER_ISIZE0 27 /* first ISIZE byte  */

typedef struct { char buf[256]; size_t len; } Sink;
static int sink_emit(void *ctx, const char *d, size_t n) {
    Sink *s = ctx;
    if (s->len + n > sizeof(s->buf)) return -1;
    memcpy(s->buf + s->len, d, n);
    s->len += n;
    return 0;
}

/* Fresh (ctx, decompressor); the decompressor OWNS neither -- caller destroys both. */
static KlDecompress *mk(KlCompressCtx **ctx_out, KlAllocator *al) {
    KlCompressCtx *ctx = kl_compress_miniz_ctx_create(6, al);
    *ctx_out = ctx;
    return kl_decompress_miniz_create(ctx, al);
}
static void done(KlDecompress *d, KlCompressCtx *ctx) {
    d->destroy(d);
    kl_compress_miniz_ctx_destroy(ctx);
}

/* Baseline: a valid stream fed whole with flush=1 decompresses and verifies. */
UTEST(gz_trailer, valid_full_flush_ok) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    ASSERT_EQ(0, d->dfeed(d, (const char *)GZ, GZ_LEN, 1, sink_emit, &s));
    ASSERT_EQ((size_t)11, s.len);
    ASSERT_EQ(0, memcmp(s.buf, "hello world", 11));
    done(d, ctx);
}

/* A valid stream fed whole with NO final flush is still integrity-checked (independent
 * of flush): the output emits and no error is returned. */
UTEST(gz_trailer, valid_full_no_flush_ok) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    ASSERT_EQ(0, d->dfeed(d, (const char *)GZ, GZ_LEN, 0, sink_emit, &s));
    ASSERT_EQ((size_t)11, s.len);
    ASSERT_EQ(0, memcmp(s.buf, "hello world", 11));
    done(d, ctx);
}

/* THE REGRESSION: a corrupt-CRC stream fed whole with flush=0 (no final flush) must be
 * REJECTED. Before the fix this returned 0 (verification was gated on flush). */
UTEST(gz_trailer, corrupt_crc_no_flush_rejected) {
    unsigned char g[GZ_LEN]; memcpy(g, GZ, GZ_LEN);
    g[TRAILER_CRC0] ^= 0xff;   /* flip the CRC32 */
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    ASSERT_EQ(-1, d->dfeed(d, (const char *)g, GZ_LEN, 0, sink_emit, &s));
    done(d, ctx);
}

/* Corrupt CRC on a final flush is rejected. */
UTEST(gz_trailer, corrupt_crc_flush_rejected) {
    unsigned char g[GZ_LEN]; memcpy(g, GZ, GZ_LEN);
    g[TRAILER_CRC0] ^= 0xff;
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    ASSERT_EQ(-1, d->dfeed(d, (const char *)g, GZ_LEN, 1, sink_emit, &s));
    done(d, ctx);
}

/* Corrupt ISIZE (uncompressed-length) is rejected. */
UTEST(gz_trailer, corrupt_isize_rejected) {
    unsigned char g[GZ_LEN]; memcpy(g, GZ, GZ_LEN);
    g[TRAILER_ISIZE0] ^= 0xff;
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    ASSERT_EQ(-1, d->dfeed(d, (const char *)g, GZ_LEN, 1, sink_emit, &s));
    done(d, ctx);
}

/* A truncated trailer (last 2 ISIZE bytes missing) on end-of-input is rejected rather
 * than accepted unverified. */
UTEST(gz_trailer, truncated_trailer_flush_rejected) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    ASSERT_EQ(-1, d->dfeed(d, (const char *)GZ, GZ_LEN - 2, 1, sink_emit, &s));
    done(d, ctx);
}

/* A trailer split across feeds (with no flush until the end) is captured and verified. */
UTEST(gz_trailer, split_trailer_across_feeds_ok) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    /* First chunk: everything but the final 2 trailer bytes (trailer arrives partial). */
    ASSERT_EQ(0, d->dfeed(d, (const char *)GZ, GZ_LEN - 2, 0, sink_emit, &s));
    /* Second chunk: the remaining 2 trailer bytes, still flush=0 -> completes + verifies. */
    ASSERT_EQ(0, d->dfeed(d, (const char *)(GZ + GZ_LEN - 2), 2, 0, sink_emit, &s));
    /* Final empty flush after a verified stream stays OK. */
    ASSERT_EQ(0, d->dfeed(d, NULL, 0, 1, sink_emit, &s));
    ASSERT_EQ((size_t)11, s.len);
    ASSERT_EQ(0, memcmp(s.buf, "hello world", 11));
    done(d, ctx);
}

UTEST_MAIN()
