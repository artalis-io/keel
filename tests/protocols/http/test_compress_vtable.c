/*
 * test_compress_vtable.c: a caller-supplied compression/decompression factory returns a session whose
 * required ops core calls unconditionally. A session missing a required op is rejected right after the
 * factory returns it, with its destroy guarded (a malformed table may omit destroy, so generic cleanup
 * is not guaranteed).
 *
 * Required (accepted contract): KlCompress -> compress, feed, encoding, destroy; KlDecompress ->
 * decompress, dfeed, encoding, destroy. reset is optional on both. Sessions here are shared statics
 * with stateless ops, so no case leaks even when destroy is the omitted op.
 *
 * Public boundaries: kl_http_response_body_compress + kl_http_compress_stream_begin (compress);
 * kl_decompress_body + kl_decompress_stream_init (decompress).
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/http_compress.h>
#include <keel/decompress.h>
#include <string.h>

/* ── mock compressor ──────── */
static int mc_compress(KlCompress *s, const char *in, size_t n, char **out, size_t *outn, KlAllocator *a) {
    (void)s; (void)in; (void)n;
    *out = kl_malloc(a, 1); if (!*out) return -1;
    (*out)[0] = 'x'; *outn = 1; return 0;   /* 1 byte: smaller than any real input */
}
static int mc_feed(KlCompress *s, const char *d, size_t n, int flush,
                   int (*emit)(void *, const char *, size_t), void *ectx) {
    (void)s; (void)d; (void)n; (void)flush; (void)emit; (void)ectx; return 0;
}
static const char *mc_encoding(KlCompress *s) { (void)s; return "mock"; }
static void mc_destroy(KlCompress *s) { (void)s; }

enum { C_NONE = -1, C_OMIT_COMPRESS, C_OMIT_FEED, C_OMIT_ENCODING, C_OMIT_DESTROY };
static int         g_c_omit;
static KlCompress  g_comp;
static KlCompress *comp_factory(KlCompressCtx *ctx, KlAllocator *a) {
    (void)ctx; (void)a;
    memset(&g_comp, 0, sizeof(g_comp));
    g_comp.compress = mc_compress; g_comp.feed = mc_feed;
    g_comp.encoding = mc_encoding; g_comp.destroy = mc_destroy;
    switch (g_c_omit) {
        case C_OMIT_COMPRESS: g_comp.compress = NULL; break;
        case C_OMIT_FEED:     g_comp.feed     = NULL; break;
        case C_OMIT_ENCODING: g_comp.encoding = NULL; break;
        case C_OMIT_DESTROY:  g_comp.destroy  = NULL; break;
        default: break;
    }
    return &g_comp;
}

/* ── mock decompressor ──────── */
static int md_decompress(KlDecompress *s, const char *in, size_t n, char **out, size_t *outn, KlAllocator *a) {
    (void)s; (void)in; (void)n;
    *out = kl_malloc(a, 1); if (!*out) return -1;
    (*out)[0] = 'y'; *outn = 1; return 0;
}
static int md_dfeed(KlDecompress *s, const char *d, size_t n, int flush,
                    int (*emit)(void *, const char *, size_t), void *ectx) {
    (void)s; (void)d; (void)n; (void)flush; (void)emit; (void)ectx; return 0;
}
static const char *md_encoding(KlDecompress *s) { (void)s; return "mock"; }
static void md_destroy(KlDecompress *s) { (void)s; }

enum { D_NONE = -1, D_OMIT_DECOMPRESS, D_OMIT_DFEED, D_OMIT_ENCODING, D_OMIT_DESTROY };
static int           g_d_omit;
static KlDecompress  g_decomp;
static KlDecompress *decomp_factory(KlCompressCtx *ctx, KlAllocator *a) {
    (void)ctx; (void)a;
    memset(&g_decomp, 0, sizeof(g_decomp));
    g_decomp.decompress = md_decompress; g_decomp.dfeed = md_dfeed;
    g_decomp.encoding = md_encoding; g_decomp.destroy = md_destroy;
    switch (g_d_omit) {
        case D_OMIT_DECOMPRESS: g_decomp.decompress = NULL; break;
        case D_OMIT_DFEED:      g_decomp.dfeed      = NULL; break;
        case D_OMIT_ENCODING:   g_decomp.encoding   = NULL; break;
        case D_OMIT_DESTROY:    g_decomp.destroy    = NULL; break;
        default: break;
    }
    return &g_decomp;
}

/* ── compress: kl_http_response_body_compress ──────── */
UTEST(compress_vtable, body_compress_valid_ok) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res; kl_http_response_init(&res, &a);
    KlCompressConfig cfg = { .ctx = NULL, .factory = comp_factory };
    g_c_omit = C_NONE;
    ASSERT_EQ(kl_http_response_body_compress(&res, &cfg, "hello world", 11), 0);
    kl_http_response_free(&res);
}
UTEST(compress_vtable, body_compress_missing_required_rejected) {
    int omits[] = { C_OMIT_COMPRESS, C_OMIT_FEED, C_OMIT_ENCODING, C_OMIT_DESTROY };
    for (size_t i = 0; i < sizeof(omits) / sizeof(omits[0]); i++) {
        KlAllocator a = kl_allocator_default();
        KlHttpResponse res; kl_http_response_init(&res, &a);
        KlCompressConfig cfg = { .ctx = NULL, .factory = comp_factory };
        g_c_omit = omits[i];
        ASSERT_EQ(kl_http_response_body_compress(&res, &cfg, "hello world", 11), -1);
        kl_http_response_free(&res);
    }
}
/* the second compress boundary: a malformed session is rejected before streaming starts. */
UTEST(compress_vtable, stream_begin_missing_required_rejected) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res; kl_http_response_init(&res, &a);
    KlCompressConfig cfg = { .ctx = NULL, .factory = comp_factory };
    KlHttpCompressStream cs;
    g_c_omit = C_OMIT_ENCODING;
    ASSERT_EQ(kl_http_compress_stream_begin(&res, &cfg, 200, &cs), -1);
    kl_http_response_free(&res);
}

/* ── decompress: kl_decompress_body + kl_decompress_stream_init ──────── */
UTEST(compress_vtable, decompress_body_valid_ok) {
    KlAllocator a = kl_allocator_default();
    KlDecompressConfig cfg = { .ctx = NULL, .factory = decomp_factory };
    g_d_omit = D_NONE;
    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(kl_decompress_body(&cfg, "in", 2, &out, &out_len, &a), 0);
    if (out) kl_free(&a, out, out_len);
}
UTEST(compress_vtable, decompress_body_missing_required_rejected) {
    int omits[] = { D_OMIT_DECOMPRESS, D_OMIT_DFEED, D_OMIT_ENCODING, D_OMIT_DESTROY };
    for (size_t i = 0; i < sizeof(omits) / sizeof(omits[0]); i++) {
        KlAllocator a = kl_allocator_default();
        KlDecompressConfig cfg = { .ctx = NULL, .factory = decomp_factory };
        g_d_omit = omits[i];
        char *out = NULL; size_t out_len = 0;
        ASSERT_EQ(kl_decompress_body(&cfg, "in", 2, &out, &out_len, &a), -1);
        ASSERT_TRUE(out == NULL);   /* rejected before decompress ran */
    }
}
UTEST(compress_vtable, decompress_stream_init_missing_required_rejected) {
    KlAllocator a = kl_allocator_default();
    KlDecompressConfig cfg = { .ctx = NULL, .factory = decomp_factory };
    KlDecompressStream ds;
    g_d_omit = D_OMIT_DFEED;
    ASSERT_EQ(kl_decompress_stream_init(&ds, &cfg, &a), -1);
}
UTEST(compress_vtable, decompress_stream_init_valid_ok) {
    KlAllocator a = kl_allocator_default();
    KlDecompressConfig cfg = { .ctx = NULL, .factory = decomp_factory };
    KlDecompressStream ds;
    g_d_omit = D_NONE;
    ASSERT_EQ(kl_decompress_stream_init(&ds, &cfg, &a), 0);
    kl_decompress_stream_free(&ds);
}

UTEST_MAIN();
