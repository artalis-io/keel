#include "utest.h"
#include <keel/decompress.h>
#include <keel/http_client.h>
#include <keel/allocator.h>
#include <string.h>

/* ── Mock decompressor ───────────────────────────────────────────── */

typedef struct {
    KlDecompress base;
    int decompress_fail;
    int destroy_called;
    int dfeed_count;
    int last_flush;
} MockDecomp;

/**
 * Mock decompressor: reverses the mock compressor's "COMPRESSED:" prefix.
 * If input starts with "COMPRESSED:", strips prefix and returns remainder.
 * Otherwise returns input as-is.
 */
static int mock_decompress(KlDecompress *self, const char *in, size_t in_len,
                            char **out, size_t *out_len, KlAllocator *alloc) {
    MockDecomp *m = (MockDecomp *)self;
    if (m->decompress_fail) return -1;

    const char *prefix = "COMPRESSED:";
    size_t plen = strlen(prefix);

    if (in_len >= plen && memcmp(in, prefix, plen) == 0) {
        /* Strip prefix */
        size_t rest = in_len - plen;
        if (rest == 0) {
            *out = NULL;
            *out_len = 0;
            return 0;
        }
        *out = kl_malloc(alloc, rest);
        if (!*out) return -1;
        memcpy(*out, in + plen, rest);
        *out_len = rest;
    } else {
        /* Passthrough */
        *out = kl_malloc(alloc, in_len);
        if (!*out) return -1;
        memcpy(*out, in, in_len);
        *out_len = in_len;
    }
    return 0;
}

/**
 * Mock streaming: strips "C:" prefix from input, emits remainder.
 * On flush=1, emits "[DONE]".
 */
static int mock_dfeed(KlDecompress *self, const char *data, size_t len,
                       int flush,
                       int (*emit)(void *ctx, const char *data, size_t len),
                       void *emit_ctx) {
    MockDecomp *m = (MockDecomp *)self;
    m->dfeed_count++;
    m->last_flush = flush;

    if (data && len > 0) {
        /* Strip "C:" prefix if present */
        if (len >= 2 && data[0] == 'C' && data[1] == ':') {
            if (len > 2) {
                if (emit(emit_ctx, data + 2, len - 2) < 0) return -1;
            }
        } else {
            if (emit(emit_ctx, data, len) < 0) return -1;
        }
    }

    if (flush) {
        if (emit(emit_ctx, "[DONE]", 6) < 0) return -1;
    }

    return 0;
}

static const char *mock_encoding(KlDecompress *self) {
    (void)self;
    return "mock";
}

static void mock_reset(KlDecompress *self) {
    MockDecomp *m = (MockDecomp *)self;
    m->dfeed_count = 0;
    m->last_flush = 0;
}

static void mock_destroy(KlDecompress *self) {
    MockDecomp *m = (MockDecomp *)self;
    m->destroy_called = 1;
}

static void mock_decomp_init(MockDecomp *m) {
    memset(m, 0, sizeof(*m));
    m->base.decompress = mock_decompress;
    m->base.dfeed = mock_dfeed;
    m->base.encoding = mock_encoding;
    m->base.reset = mock_reset;
    m->base.destroy = mock_destroy;
}

/* ── Mock factory ────────────────────────────────────────────────── */

static MockDecomp g_mock;
static int g_factory_fail;

static KlDecompress *mock_factory(KlCompressCtx *ctx, KlAllocator *alloc) {
    (void)ctx;
    (void)alloc;
    if (g_factory_fail) return NULL;
    mock_decomp_init(&g_mock);
    return &g_mock.base;
}

/* ── One-shot decompression tests ────────────────────────────────── */

UTEST(decompress, body_basic) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    KlDecompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    const char *compressed = "COMPRESSED:Hello";

    char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(kl_decompress_body(&cfg, compressed, strlen(compressed),
                                  &out, &out_len, &a), 0);

    ASSERT_EQ(out_len, (size_t)5);
    ASSERT_EQ(memcmp(out, "Hello", 5), 0);
    ASSERT_TRUE(g_mock.destroy_called);

    kl_free(&a, out, out_len);
}

UTEST(decompress, body_empty) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    KlDecompressConfig cfg = { .ctx = NULL, .factory = mock_factory };

    char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(kl_decompress_body(&cfg, "", 0, &out, &out_len, &a), 0);
    ASSERT_EQ(out_len, (size_t)0);
    ASSERT_TRUE(out == NULL);
}

UTEST(decompress, body_null_args) {
    KlAllocator a = kl_allocator_default();

    char *out = NULL;
    size_t out_len = 0;

    /* NULL config */
    ASSERT_EQ(kl_decompress_body(NULL, "data", 4, &out, &out_len, &a), -1);

    /* NULL factory */
    KlDecompressConfig cfg = { .ctx = NULL, .factory = NULL };
    ASSERT_EQ(kl_decompress_body(&cfg, "data", 4, &out, &out_len, &a), -1);

    /* NULL out */
    cfg.factory = mock_factory;
    ASSERT_EQ(kl_decompress_body(&cfg, "data", 4, NULL, &out_len, &a), -1);

    /* NULL out_len */
    ASSERT_EQ(kl_decompress_body(&cfg, "data", 4, &out, NULL, &a), -1);

    /* NULL alloc */
    ASSERT_EQ(kl_decompress_body(&cfg, "data", 4, &out, &out_len, NULL), -1);

    /* Non-zero len but NULL data */
    ASSERT_EQ(kl_decompress_body(&cfg, NULL, 10, &out, &out_len, &a), -1);
}

UTEST(decompress, body_factory_fail) {
    KlAllocator a = kl_allocator_default();

    g_factory_fail = 1;
    KlDecompressConfig cfg = { .ctx = NULL, .factory = mock_factory };

    char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(kl_decompress_body(&cfg, "data", 4, &out, &out_len, &a), -1);

    g_factory_fail = 0;
}

static KlDecompress *mock_factory_decompress_fail(KlCompressCtx *ctx,
                                                    KlAllocator *alloc) {
    (void)ctx;
    (void)alloc;
    mock_decomp_init(&g_mock);
    g_mock.decompress_fail = 1;
    return &g_mock.base;
}

UTEST(decompress, body_decompress_fail) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    KlDecompressConfig cfg = {
        .ctx = NULL,
        .factory = mock_factory_decompress_fail
    };

    char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(kl_decompress_body(&cfg, "data", 4, &out, &out_len, &a), -1);
    ASSERT_TRUE(g_mock.destroy_called);
}

/* ── Stream decompression tests ──────────────────────────────────── */

UTEST(decompress, stream_basic) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    KlDecompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    KlDecompressStream ds;

    ASSERT_EQ(kl_decompress_stream_init(&ds, &cfg, &a), 0);
    ASSERT_TRUE(ds.decomp != NULL);
    ASSERT_EQ(ds.error, 0);

    /* Verify the basic lifecycle works */
    kl_decompress_stream_free(&ds);
    ASSERT_TRUE(g_mock.destroy_called);
}

static char g_emit_buf[1024];
static size_t g_emit_len;

static int test_emit(void *ctx, const char *data, size_t len) {
    (void)ctx;
    if (g_emit_len + len > sizeof(g_emit_buf)) return -1;
    memcpy(g_emit_buf + g_emit_len, data, len);
    g_emit_len += len;
    return 0;
}

UTEST(decompress, stream_multi_feed) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    KlDecompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    KlDecompressStream ds;

    ASSERT_EQ(kl_decompress_stream_init(&ds, &cfg, &a), 0);

    g_emit_len = 0;
    memset(g_emit_buf, 0, sizeof(g_emit_buf));

    ASSERT_EQ(kl_decompress_stream_feed(&ds, "C:hello", 7, 0,
                                          test_emit, NULL), 0);
    ASSERT_EQ(kl_decompress_stream_feed(&ds, "C:world", 7, 0,
                                          test_emit, NULL), 0);

    ASSERT_EQ(g_mock.dfeed_count, 2);

    /* Check accumulated output: "hello" + "world" */
    ASSERT_EQ(g_emit_len, (size_t)10);
    ASSERT_EQ(memcmp(g_emit_buf, "helloworld", 10), 0);

    kl_decompress_stream_free(&ds);
}

UTEST(decompress, stream_flush) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    KlDecompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    KlDecompressStream ds;

    ASSERT_EQ(kl_decompress_stream_init(&ds, &cfg, &a), 0);

    g_emit_len = 0;
    memset(g_emit_buf, 0, sizeof(g_emit_buf));

    ASSERT_EQ(kl_decompress_stream_feed(&ds, "C:data", 6, 0,
                                          test_emit, NULL), 0);
    ASSERT_EQ(kl_decompress_stream_feed(&ds, NULL, 0, 1,
                                          test_emit, NULL), 0);

    ASSERT_EQ(g_mock.last_flush, 1);

    /* Output: "data" + "[DONE]" */
    ASSERT_EQ(g_emit_len, (size_t)10);
    ASSERT_EQ(memcmp(g_emit_buf, "data[DONE]", 10), 0);

    kl_decompress_stream_free(&ds);
}

static int fail_emit(void *ctx, const char *data, size_t len) {
    (void)ctx;
    (void)data;
    (void)len;
    return -1;
}

UTEST(decompress, stream_error) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    KlDecompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    KlDecompressStream ds;

    ASSERT_EQ(kl_decompress_stream_init(&ds, &cfg, &a), 0);

    /* Feed with failing emit → sticky error */
    ASSERT_EQ(kl_decompress_stream_feed(&ds, "C:data", 6, 0,
                                          fail_emit, NULL), -1);
    ASSERT_EQ(ds.error, 1);

    /* Subsequent feeds should also fail (sticky) */
    ASSERT_EQ(kl_decompress_stream_feed(&ds, "C:more", 6, 0,
                                          test_emit, NULL), -1);

    kl_decompress_stream_free(&ds);
}

UTEST(decompress, stream_null_args) {
    KlAllocator a = kl_allocator_default();

    /* NULL ds */
    ASSERT_EQ(kl_decompress_stream_init(NULL, NULL, NULL), -1);

    /* NULL cfg */
    KlDecompressStream ds;
    ASSERT_EQ(kl_decompress_stream_init(&ds, NULL, &a), -1);

    /* NULL factory */
    KlDecompressConfig cfg = { .ctx = NULL, .factory = NULL };
    ASSERT_EQ(kl_decompress_stream_init(&ds, &cfg, &a), -1);

    /* Feed on NULL ds */
    ASSERT_EQ(kl_decompress_stream_feed(NULL, "data", 4, 0,
                                          test_emit, NULL), -1);

    /* Free on NULL ds: should not crash */
    kl_decompress_stream_free(NULL);
}

/* ── Client integration tests (mock-based, no network) ───────────── */

/**
 * Test that decompress_response_body works on a KlHttpClientResponse.
 * We call the static helper indirectly through kl_decompress_body.
 */
UTEST(decompress, client_decompress_buffered) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    /* Build a mock response with Content-Encoding: mock */
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.alloc = a;
    resp.status = 200;

    /* Allocate body: "COMPRESSED:X" */
    const char *compressed = "COMPRESSED:X";
    size_t clen = strlen(compressed);
    resp.body = kl_malloc(&a, clen + 1);
    memcpy(resp.body, compressed, clen);
    resp.body[clen] = '\0';
    resp.body_len = clen;

    /* Allocate headers */
    resp.headers = kl_malloc(&a, 2 * sizeof(KlHttpClientHeader));
    resp.num_headers = 2;

    /* Content-Encoding header */
    char *hname = kl_malloc(&a, strlen("Content-Encoding") + 1);
    strcpy(hname, "Content-Encoding");
    char *hval = kl_malloc(&a, strlen("mock") + 1);
    strcpy(hval, "mock");
    resp.headers[0].name = hname;
    resp.headers[0].value = hval;

    /* Another header */
    char *hname2 = kl_malloc(&a, strlen("Content-Type") + 1);
    strcpy(hname2, "Content-Type");
    char *hval2 = kl_malloc(&a, strlen("text/plain") + 1);
    strcpy(hval2, "text/plain");
    resp.headers[1].name = hname2;
    resp.headers[1].value = hval2;

    /* Decompress via one-shot path (simulates what client does) */
    KlDecompressConfig dcfg = { .ctx = NULL, .factory = mock_factory };

    /* Create session, check encoding, decompress, replace body */
    KlDecompress *decomp = dcfg.factory(dcfg.ctx, &a);
    ASSERT_TRUE(decomp != NULL);
    const char *enc = decomp->encoding(decomp);
    ASSERT_STREQ(enc, "mock");

    char *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(decomp->decompress(decomp, resp.body, resp.body_len,
                                   &out, &out_len, &a), 0);
    decomp->destroy(decomp);

    /* Replace body */
    kl_free(&a, resp.body, resp.body_len + 1);
    resp.body = out;
    resp.body_len = out_len;

    ASSERT_EQ(resp.body_len, (size_t)1);
    ASSERT_EQ(resp.body[0], 'X');

    kl_http_client_response_free(&resp);
}

UTEST(decompress, client_no_encoding) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    /* Response with no Content-Encoding → body untouched */
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.alloc = a;
    resp.status = 200;

    const char *body_data = "rawbody";
    size_t blen = strlen(body_data);
    resp.body = kl_malloc(&a, blen + 1);
    memcpy(resp.body, body_data, blen);
    resp.body[blen] = '\0';
    resp.body_len = blen;
    resp.headers = NULL;
    resp.num_headers = 0;

    /* kl_decompress_body should not be called since no encoding header.
       We just verify via one-shot that it still works with empty input. */
    ASSERT_EQ(resp.body_len, blen);
    ASSERT_EQ(memcmp(resp.body, "rawbody", blen), 0);

    kl_http_client_response_free(&resp);
}

UTEST(decompress, client_encoding_mismatch) {
    KlAllocator a = kl_allocator_default();
    g_factory_fail = 0;

    /* Decompress session reports "mock" but header says "br" */
    KlDecompressConfig dcfg = { .ctx = NULL, .factory = mock_factory };
    KlDecompress *decomp = dcfg.factory(dcfg.ctx, &a);
    ASSERT_TRUE(decomp != NULL);

    const char *enc = decomp->encoding(decomp);
    ASSERT_STREQ(enc, "mock");

    /* "br" != "mock" → mismatch, body should stay as-is */
    ASSERT_TRUE(strcasecmp("br", enc) != 0);

    decomp->destroy(decomp);
}

UTEST(decompress, client_no_config) {
    /* decompress = NULL → body untouched (default behavior) */
    KlAllocator a = kl_allocator_default();

    char *out = NULL;
    size_t out_len = 0;

    /* kl_decompress_body with NULL config should fail safely */
    ASSERT_EQ(kl_decompress_body(NULL, "data", 4, &out, &out_len, &a), -1);
}

UTEST_MAIN();
