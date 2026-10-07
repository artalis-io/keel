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
#include <stdlib.h>

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

/* A gzip body that ends inside its own 10-byte header is truncated, not empty: at end of input
 * (flush) it fails. It was accepted as an empty body, which the client then reported as success. */
UTEST(gz_trailer, truncated_header_flush_rejected) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    int fed = d->dfeed(d, (const char *)GZ, 5, 0, sink_emit, &s);   /* half the header */
    int fin = d->dfeed(d, NULL, 0, 1, sink_emit, &s);               /* and then the end */
    done(d, ctx);
    ASSERT_EQ(fed, 0);
    ASSERT_EQ(fin, -1);                           /* was 0: an empty body, accepted */
}

/* But a gzip stream that received nothing at all is an empty body, not a truncated one: a streaming
 * client finishes the decompressor at the end of every response that said Content-Encoding: gzip, and
 * a HEAD, a 204 or a 304 (or a 200 with Content-Length: 0) has no body bytes. That must not fail. */
UTEST(gz_trailer, empty_stream_flush_ok) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    int fin = d->dfeed(d, NULL, 0, 1, sink_emit, &s);               /* the end, and nothing before */
    done(d, ctx);
    ASSERT_EQ(fin, 0);                            /* was -1: the response failed as a decode error */
    ASSERT_EQ(s.len, (size_t)0);
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

/* ── Compression output size (single shot) ─────────────────────────────────────────────────
 * The caller frees the compressed buffer with *out_len (KlCompress contract; core does exactly
 * that, and stores it as body_owned_size). The buffer must therefore be exactly that size, not the
 * worst-case bound it was compressed into: a sized allocator would otherwise be handed the wrong
 * size. A size-checking allocator records each block and counts frees whose size differs. */
#define SZ_MAX_BLOCKS 64
static struct { void *p; size_t n; } g_blk[SZ_MAX_BLOCKS];
static int g_size_mismatch;
static void sz_note(void *p, size_t n) {
    for (int i = 0; i < SZ_MAX_BLOCKS; i++)
        if (!g_blk[i].p) { g_blk[i].p = p; g_blk[i].n = n; return; }
}
static void sz_drop(void *p, size_t n) {
    for (int i = 0; i < SZ_MAX_BLOCKS; i++)
        if (g_blk[i].p == p) { if (g_blk[i].n != n) g_size_mismatch++; g_blk[i].p = NULL; return; }
}
static void *sz_malloc(void *c, size_t n) { (void)c; void *p = malloc(n ? n : 1); if (p) sz_note(p, n); return p; }
static void *sz_realloc(void *c, void *p, size_t o, size_t n) {
    (void)c;
    if (p) sz_drop(p, o);
    void *q = realloc(p, n ? n : 1);
    if (q) sz_note(q, n);
    return q;
}
static void sz_free(void *c, void *p, size_t n) { (void)c; if (!p) return; sz_drop(p, n); free(p); }

UTEST(gz_compress, output_is_freed_at_its_own_size) {
    KlAllocator al = { sz_malloc, sz_realloc, sz_free, NULL };
    memset(g_blk, 0, sizeof g_blk);
    g_size_mismatch = 0;
    KlCompressCtx *ctx = kl_compress_miniz_ctx_create(6, &al);
    ASSERT_TRUE(ctx != NULL);
    KlCompress *c = kl_compress_miniz_create(ctx, &al);
    ASSERT_TRUE(c != NULL);
    static char in[4096];
    for (size_t i = 0; i < sizeof in; i++) in[i] = (char)('a' + (i % 7));   /* compressible */
    char *out = NULL;
    size_t out_len = 0;
    int rc = c->compress(c, in, sizeof in, &out, &out_len, &al);
    if (rc == 0) kl_free(&al, out, out_len);   /* exactly as the server frees it */
    c->destroy(c);
    kl_compress_miniz_ctx_destroy(ctx);
    ASSERT_EQ(rc, 0);
    ASSERT_LT(out_len, sizeof in);
    ASSERT_EQ(g_size_mismatch, 0);
}

/* A stream whose back-references reach further than 4 KiB. dfeed decompressed into a 4 KiB buffer
 * used as a wrapping dictionary that restarted at every call, but deflate distances reach 32 KiB:
 * any match more than 4 KiB back read garbage and the CRC check failed. Real responses (HTML, JSON)
 * repeat at such distances, and the buffered client path decompresses through dfeed. The body is a
 * repeating 8 KiB pattern, so the compressor emits 8 KiB-distance matches; it must come back whole,
 * fed in one call and fed in pieces. */
typedef struct { char *buf; size_t len, cap; } BigSink;
static int big_emit(void *ctx, const char *d, size_t n) {
    BigSink *s = ctx;
    if (s->len + n > s->cap) return -1;
    memcpy(s->buf + s->len, d, n);
    s->len += n;
    return 0;
}

UTEST(gz_stream, long_distance_matches_round_trip) {
    KlAllocator al = kl_allocator_default();
    enum { PERIOD = 8192, N = 200000 };
    char *body = malloc(N);
    ASSERT_TRUE(body != NULL);
    unsigned x = 12345;
    char pat[PERIOD];
    for (size_t i = 0; i < PERIOD; i++) { x = x * 1103515245u + 12345u; pat[i] = (char)('a' + (x >> 16) % 26); }
    for (size_t i = 0; i < N; i++) body[i] = pat[i % PERIOD];

    KlCompressCtx *cctx = kl_compress_miniz_ctx_create(6, &al);
    ASSERT_TRUE(cctx != NULL);
    KlCompress *c = kl_compress_miniz_create(cctx, &al);
    ASSERT_TRUE(c != NULL);
    char *gz = NULL;
    size_t gz_len = 0;
    ASSERT_EQ(0, c->compress(c, body, N, &gz, &gz_len, &al));
    c->destroy(c);
    ASSERT_LT(gz_len, (size_t)N / 4);             /* the 8 KiB matches were used */

    int whole_rc, piece_rc = 0, whole_ok, piece_ok;
    BigSink s = { malloc(N), 0, N };
    {
        KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
        whole_rc = d->dfeed(d, gz, gz_len, 1, big_emit, &s);
        done(d, ctx);
        whole_ok = whole_rc == 0 && s.len == N && memcmp(s.buf, body, N) == 0;
    }
    s.len = 0;
    {
        KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
        for (size_t off = 0; off < gz_len && piece_rc == 0; off += 1000) {
            size_t n = gz_len - off < 1000 ? gz_len - off : 1000;
            piece_rc = d->dfeed(d, gz + off, n, 0, big_emit, &s);
        }
        if (piece_rc == 0) piece_rc = d->dfeed(d, NULL, 0, 1, big_emit, &s);
        done(d, ctx);
        piece_ok = piece_rc == 0 && s.len == N && memcmp(s.buf, body, N) == 0;
    }
    kl_free(&al, gz, gz_len);
    kl_compress_miniz_ctx_destroy(cctx);
    free(s.buf);
    free(body);
    ASSERT_EQ(whole_rc, 0);                       /* was -1: CRC mismatch */
    ASSERT_TRUE(whole_ok);
    ASSERT_EQ(piece_rc, 0);
    ASSERT_TRUE(piece_ok);
}

/* ── Streaming compression ──────────────────────────────────────────────────────────────────
 * The streaming compressor (feed) ends with feed(NULL, 0, 1). That call ran tdefl_compress once,
 * with a 4 KiB output buffer: when the final block needs more, the rest stayed inside tdefl and the
 * gzip trailer followed an incomplete deflate stream, so any streamed compressed response whose last
 * block is larger than 4 KiB arrived corrupt. 200 KB of varied text, fed in 1000-byte pieces, must
 * decompress back to itself. */
static int stream_emit(void *ctx, const char *d, size_t n) { return big_emit(ctx, d, n); }

UTEST(gz_stream, streamed_compression_round_trips) {
    KlAllocator al = kl_allocator_default();
    const size_t body_len = 200000;
    char *body = malloc(body_len);
    ASSERT_TRUE(body != NULL);
    uint32_t x = 12345;                           /* words from an LCG: compressible, not trivially */
    static const char *words[] = { "alpha ", "bravo ", "charlie ", "delta ", "echo ", "foxtrot ",
                                   "golf ", "hotel ", "india ", "juliet ", "kilo ", "lima " };
    size_t at = 0;
    while (at < body_len) {
        x = x * 1103515245u + 12345u;
        const char *w = words[(x >> 16) % 12];
        size_t wl = strlen(w);
        if (wl > body_len - at) wl = body_len - at;
        memcpy(body + at, w, wl);
        at += wl;
    }

    KlCompressCtx *cctx = kl_compress_miniz_ctx_create(6, &al);
    ASSERT_TRUE(cctx != NULL);
    KlCompress *c = kl_compress_miniz_create(cctx, &al);
    ASSERT_TRUE(c != NULL);
    BigSink gz = { malloc(body_len * 2), 0, body_len * 2 };
    int crc = 0;
    for (size_t off = 0; off < body_len && crc == 0; off += 1000) {
        size_t n = body_len - off < 1000 ? body_len - off : 1000;
        crc = c->feed(c, body + off, n, 0, stream_emit, &gz);
    }
    if (crc == 0) crc = c->feed(c, NULL, 0, 1, stream_emit, &gz);
    c->destroy(c);
    kl_compress_miniz_ctx_destroy(cctx);

    KlCompressCtx *dctx;
    KlDecompress *d = mk(&dctx, &al);
    BigSink out = { malloc(body_len + 1), 0, body_len + 1 };
    int drc = d->dfeed(d, gz.buf, gz.len, 1, big_emit, &out);
    done(d, dctx);
    int same = drc == 0 && out.len == body_len && memcmp(out.buf, body, body_len) == 0;
    free(out.buf);
    free(gz.buf);
    free(body);
    ASSERT_EQ(crc, 0);
    ASSERT_EQ(drc, 0);                            /* was -1: the deflate stream stopped short */
    ASSERT_TRUE(same);
}

/* Bytes after the 8-byte trailer (trailing garbage, or a second gzip member this decoder does not
 * read) were ignored, and the body still counted as verified. At end of input they fail. */
UTEST(gz_trailer, bytes_after_the_trailer_flush_rejected) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    unsigned char buf[GZ_LEN + 3];
    memcpy(buf, GZ, GZ_LEN);
    memcpy(buf + GZ_LEN, "XYZ", 3);
    int rc = d->dfeed(d, (const char *)buf, sizeof buf, 1, sink_emit, &s);
    done(d, ctx);
    ASSERT_EQ(rc, -1);                            /* was 0: the extra bytes were dropped */
}

/* ── Optional gzip header fields split across feeds ──────────────────────────────────────────
 * The streaming header parser required FEXTRA / FNAME / FCOMMENT / FHCRC to arrive in the same
 * dfeed call as the fixed 10 bytes, so a split there failed a valid body. FNAME is what gzip writes
 * by default. The same "hello world" deflate data + trailer as GZ, behind a header carrying every
 * optional field: FLG = FHCRC|FEXTRA|FNAME|FCOMMENT, XLEN 4 "XY\0\0", FNAME "hello.txt",
 * FCOMMENT "c", and the header CRC16 (30 header bytes, 51 in all). */
static const unsigned char GZ_OPT[] = {
    0x1f,0x8b,0x08,0x1e,0x00,0x00,0x00,0x00,0x00,0x03,0x04,0x00,
    0x58,0x59,0x00,0x00,0x68,0x65,0x6c,0x6c,0x6f,0x2e,0x74,0x78,
    0x74,0x00,0x63,0x00,0xd8,0xbb,0xcb,0x48,0xcd,0xc9,0xc9,0x57,
    0x28,0xcf,0x2f,0xca,0x49,0x01,0x00,0x85,0x11,0x4a,0x0d,0x0b,
    0x00,0x00,0x00
};
#define GZ_OPT_LEN (sizeof(GZ_OPT))
#define GZ_OPT_HDR 30

/* Baseline: fed whole, the optional fields are skipped and the body decodes. */
UTEST(gz_header, optional_fields_whole_ok) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    int rc = d->dfeed(d, (const char *)GZ_OPT, GZ_OPT_LEN, 1, sink_emit, &s);
    done(d, ctx);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((size_t)11, s.len);
    ASSERT_EQ(0, memcmp(s.buf, "hello world", 11));
}

/* Every byte in its own feed: each optional field is split at every possible point. */
UTEST(gz_header, optional_fields_byte_by_byte_ok) {
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    int rc = 0;
    for (size_t i = 0; i < GZ_OPT_LEN && rc == 0; i++)
        rc = d->dfeed(d, (const char *)GZ_OPT + i, 1, 0, sink_emit, &s);
    if (rc == 0) rc = d->dfeed(d, NULL, 0, 1, sink_emit, &s);
    done(d, ctx);
    ASSERT_EQ(rc, 0);                             /* was -1: the header fields had to be in one feed */
    ASSERT_EQ((size_t)11, s.len);
    ASSERT_EQ(0, memcmp(s.buf, "hello world", 11));
}

/* gzip's default shape: FNAME only, with the cut right after the fixed 10 bytes. */
UTEST(gz_header, fname_split_after_fixed_header_ok) {
    static const unsigned char g[] = {
        0x1f,0x8b,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x03,
        'h','e','l','l','o','.','t','x','t',0x00,
        0xcb,0x48,0xcd,0xc9,0xc9,0x57,0x28,0xcf,0x2f,0xca,0x49,0x01,0x00,
        0x85,0x11,0x4a,0x0d,0x0b,0x00,0x00,0x00
    };
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    int a = d->dfeed(d, (const char *)g, 10, 0, sink_emit, &s);
    int b = d->dfeed(d, (const char *)g + 10, 4, 0, sink_emit, &s);       /* inside FNAME */
    int c = d->dfeed(d, (const char *)g + 14, sizeof g - 14, 1, sink_emit, &s);
    done(d, ctx);
    ASSERT_EQ(a, 0);
    ASSERT_EQ(b, 0);                              /* was -1: FNAME was not terminated in this feed */
    ASSERT_EQ(c, 0);
    ASSERT_EQ((size_t)11, s.len);
    ASSERT_EQ(0, memcmp(s.buf, "hello world", 11));
}

/* FEXTRA's length is honoured across feeds: a 300-byte extra field (bytes that would be a valid
 * deflate prefix are not mistaken for data) arrives in pieces, and the body still decodes. */
UTEST(gz_header, long_fextra_across_feeds_ok) {
    enum { XLEN = 300 };
    static const unsigned char tail[] = {
        0xcb,0x48,0xcd,0xc9,0xc9,0x57,0x28,0xcf,0x2f,0xca,0x49,0x01,0x00,
        0x85,0x11,0x4a,0x0d,0x0b,0x00,0x00,0x00
    };
    unsigned char g[10 + 2 + XLEN + sizeof tail];
    static const unsigned char fixed[10] = { 0x1f,0x8b,0x08,0x04,0,0,0,0,0,0x03 };
    memcpy(g, fixed, 10);
    g[10] = (unsigned char)(XLEN & 0xff);
    g[11] = (unsigned char)(XLEN >> 8);
    memset(g + 12, 0xcb, XLEN);
    memcpy(g + 12 + XLEN, tail, sizeof tail);
    KlAllocator al = kl_allocator_default();
    KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
    Sink s = {0};
    int rc = 0;
    for (size_t off = 0; off < sizeof g && rc == 0; off += 7) {
        size_t n = sizeof g - off < 7 ? sizeof g - off : 7;
        rc = d->dfeed(d, (const char *)g + off, n, 0, sink_emit, &s);
    }
    if (rc == 0) rc = d->dfeed(d, NULL, 0, 1, sink_emit, &s);
    done(d, ctx);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ((size_t)11, s.len);
    ASSERT_EQ(0, memcmp(s.buf, "hello world", 11));
}

/* A body that ends inside the optional fields is truncated: no error mid-stream, but the final
 * (flush) call fails, at every cut point inside them. */
UTEST(gz_header, truncated_in_optional_fields_flush_rejected) {
    for (size_t cut = 10; cut < GZ_OPT_HDR; cut++) {
        KlAllocator al = kl_allocator_default();
        KlCompressCtx *ctx; KlDecompress *d = mk(&ctx, &al);
        Sink s = {0};
        int fed = d->dfeed(d, (const char *)GZ_OPT, cut, 0, sink_emit, &s);
        int fin = d->dfeed(d, NULL, 0, 1, sink_emit, &s);
        done(d, ctx);
        ASSERT_EQ(fed, 0);
        ASSERT_EQ(fin, -1);
        ASSERT_EQ((size_t)0, s.len);
    }
}

UTEST_MAIN()
