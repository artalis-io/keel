#include "utest.h"
#include <keel/compress.h>
#include <keel/allocator.h>
#include <string.h>
#include <unistd.h>

/* ── Mock compressor ─────────────────────────────────────────────── */

typedef struct {
    KlCompress base;
    int compress_fail;    /* -1 = succeed, 0+ = fail */
    int expand;           /* if set, output is larger than input */
    int destroy_called;
    int feed_count;
    int last_flush;
} MockComp;

static int mock_compress(KlCompress *self, const char *in, size_t in_len,
                          char **out, size_t *out_len, KlAllocator *alloc) {
    MockComp *m = (MockComp *)self;
    if (m->compress_fail == 0) return -1;

    if (m->expand) {
        /* Simulate expansion: output is input + 100 bytes */
        size_t total = in_len + 100;
        *out = kl_malloc(alloc, total);
        if (!*out) return -1;
        memset(*out, 'X', total);
        if (in_len > 0) memcpy(*out, in, in_len);
        *out_len = total;
    } else {
        /* Simulate compression: output is "COMPRESSED:" + first byte */
        const char *prefix = "COMPRESSED:";
        size_t plen = strlen(prefix);
        size_t total = plen + (in_len > 0 ? 1 : 0);
        *out = kl_malloc(alloc, total);
        if (!*out) return -1;
        memcpy(*out, prefix, plen);
        if (in_len > 0) (*out)[plen] = in[0];
        *out_len = total;
    }
    return 0;
}

static int mock_feed(KlCompress *self, const char *data, size_t len,
                      int flush,
                      int (*emit)(void *ctx, const char *data, size_t len),
                      void *emit_ctx) {
    MockComp *m = (MockComp *)self;
    m->feed_count++;
    m->last_flush = flush;

    /* Emit the input prefixed with "C:" */
    if (data && len > 0) {
        if (emit(emit_ctx, "C:", 2) < 0) return -1;
        if (emit(emit_ctx, data, len) < 0) return -1;
    }

    /* On final flush, emit trailer */
    if (flush) {
        if (emit(emit_ctx, "[END]", 5) < 0) return -1;
    }

    return 0;
}

static const char *mock_encoding(KlCompress *self) {
    (void)self;
    return "mock";
}

static void mock_reset(KlCompress *self) {
    MockComp *m = (MockComp *)self;
    m->feed_count = 0;
    m->last_flush = 0;
}

static void mock_destroy(KlCompress *self) {
    MockComp *m = (MockComp *)self;
    m->destroy_called = 1;
}

static void mock_comp_init(MockComp *m) {
    memset(m, 0, sizeof(*m));
    m->base.compress = mock_compress;
    m->base.feed = mock_feed;
    m->base.encoding = mock_encoding;
    m->base.reset = mock_reset;
    m->base.destroy = mock_destroy;
    m->compress_fail = -1;  /* succeed by default */
}

/* ── Mock factory ────────────────────────────────────────────────── */

static MockComp g_mock;
static int g_factory_fail;

static KlCompress *mock_factory(KlCompressCtx *ctx, KlAllocator *alloc) {
    (void)ctx;
    (void)alloc;
    if (g_factory_fail) return NULL;
    mock_comp_init(&g_mock);
    return &g_mock.base;
}

static KlCompress *mock_factory_expand(KlCompressCtx *ctx,
                                        KlAllocator *alloc) {
    (void)ctx;
    (void)alloc;
    mock_comp_init(&g_mock);
    g_mock.expand = 1;
    return &g_mock.base;
}

static KlCompress *mock_factory_compress_fail(KlCompressCtx *ctx,
                                               KlAllocator *alloc) {
    (void)ctx;
    (void)alloc;
    mock_comp_init(&g_mock);
    g_mock.compress_fail = 0;
    return &g_mock.base;
}

/* ── Buffer compression tests ────────────────────────────────────── */

UTEST(compress, buffer_basic) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    g_factory_fail = 0;

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    const char *data = "Hello, World! This is a test body with enough data.";

    ASSERT_EQ(kl_response_body_compress(&res, &cfg, data, strlen(data)), 0);

    /* Verify headers were set (null-terminate for strstr) */
    res.hdr_buf[res.hdr_len] = '\0';
    ASSERT_TRUE(strstr(res.hdr_buf, "Content-Encoding: mock\r\n") != NULL);
    ASSERT_TRUE(strstr(res.hdr_buf, "Vary: Accept-Encoding\r\n") != NULL);

    /* Verify body is compressed (mock: "COMPRESSED:" + first byte) */
    ASSERT_EQ(res.body_mode, KL_BODY_BUFFER);
    ASSERT_TRUE(res.body_len < strlen(data));
    ASSERT_EQ(memcmp(res.body, "COMPRESSED:H", 12), 0);

    /* Verify destroy was called */
    ASSERT_TRUE(g_mock.destroy_called);

    kl_response_free(&res);
}

UTEST(compress, buffer_expansion_fallback) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory_expand };
    const char *data = "short";

    ASSERT_EQ(kl_response_body_compress(&res, &cfg, data, strlen(data)), 0);

    /* No Content-Encoding header when expansion (null-terminate for strstr) */
    res.hdr_buf[res.hdr_len] = '\0';
    ASSERT_TRUE(strstr(res.hdr_buf, "Content-Encoding") == NULL);

    /* Original body preserved */
    ASSERT_EQ(res.body_mode, KL_BODY_BUFFER);
    ASSERT_EQ(res.body_len, strlen(data));
    ASSERT_EQ(memcmp(res.body, data, res.body_len), 0);

    /* Destroy still called */
    ASSERT_TRUE(g_mock.destroy_called);

    kl_response_free(&res);
}

UTEST(compress, buffer_empty) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };

    ASSERT_EQ(kl_response_body_compress(&res, &cfg, "", 0), 0);
    ASSERT_EQ(res.body_mode, KL_BODY_BUFFER);
    ASSERT_EQ(res.body_len, (size_t)0);

    kl_response_free(&res);
}

UTEST(compress, buffer_null_data) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };

    ASSERT_EQ(kl_response_body_compress(&res, &cfg, NULL, 10), -1);

    kl_response_free(&res);
}

UTEST(compress, buffer_null_config) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    ASSERT_EQ(kl_response_body_compress(&res, NULL, "data", 4), -1);

    kl_response_free(&res);
}

UTEST(compress, buffer_factory_failure) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    g_factory_fail = 1;
    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };

    ASSERT_EQ(kl_response_body_compress(&res, &cfg, "data", 4), -1);

    g_factory_fail = 0;
    kl_response_free(&res);
}

UTEST(compress, buffer_compress_failure) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    KlCompressConfig cfg = {
        .ctx = NULL,
        .factory = mock_factory_compress_fail
    };

    ASSERT_EQ(kl_response_body_compress(&res, &cfg, "data", 4), -1);

    /* Destroy still called on error */
    ASSERT_TRUE(g_mock.destroy_called);

    kl_response_free(&res);
}

/* ── Streaming compression tests ─────────────────────────────────── */

UTEST(compress, stream_basic) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    int pipefd[2];
    pipe(pipefd);
    res.conn_fd = pipefd[1];

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    KlCompressStream cs;

    ASSERT_EQ(kl_compress_stream_begin(&res, &cfg, 200, &cs), 0);
    res.hdr_buf[res.hdr_len] = '\0';
    ASSERT_TRUE(strstr(res.hdr_buf, "Content-Encoding: mock\r\n") != NULL);
    ASSERT_TRUE(strstr(res.hdr_buf, "Vary: Accept-Encoding\r\n") != NULL);
    ASSERT_TRUE(cs.comp != NULL);

    ASSERT_EQ(kl_compress_stream_write(&cs, "hello", 5), 0);
    ASSERT_EQ(kl_compress_stream_end(&cs), 0);

    /* Destroy called */
    ASSERT_TRUE(g_mock.destroy_called);

    close(pipefd[0]);
    close(pipefd[1]);
    kl_response_free(&res);
}

UTEST(compress, stream_multiple_writes) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    int pipefd[2];
    pipe(pipefd);
    res.conn_fd = pipefd[1];

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    KlCompressStream cs;

    ASSERT_EQ(kl_compress_stream_begin(&res, &cfg, 200, &cs), 0);
    ASSERT_EQ(kl_compress_stream_write(&cs, "chunk1", 6), 0);
    ASSERT_EQ(kl_compress_stream_write(&cs, "chunk2", 6), 0);
    ASSERT_EQ(kl_compress_stream_write(&cs, "chunk3", 6), 0);

    /* feed_count should reflect 3 writes */
    ASSERT_EQ(g_mock.feed_count, 3);

    ASSERT_EQ(kl_compress_stream_end(&cs), 0);

    /* Final flush called */
    ASSERT_EQ(g_mock.last_flush, 1);

    close(pipefd[0]);
    close(pipefd[1]);
    kl_response_free(&res);
}

UTEST(compress, stream_end_flushes) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    int pipefd[2];
    pipe(pipefd);
    res.conn_fd = pipefd[1];

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    KlCompressStream cs;

    ASSERT_EQ(kl_compress_stream_begin(&res, &cfg, 200, &cs), 0);
    ASSERT_EQ(kl_compress_stream_end(&cs), 0);

    /* flush=1 called even with no writes */
    ASSERT_EQ(g_mock.last_flush, 1);

    close(pipefd[0]);
    close(pipefd[1]);
    kl_response_free(&res);
}

UTEST(compress, stream_zero_len_write) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    int pipefd[2];
    pipe(pipefd);
    res.conn_fd = pipefd[1];

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    KlCompressStream cs;

    ASSERT_EQ(kl_compress_stream_begin(&res, &cfg, 200, &cs), 0);

    /* Zero-length write should be a no-op */
    ASSERT_EQ(kl_compress_stream_write(&cs, "", 0), 0);
    ASSERT_EQ(g_mock.feed_count, 0);

    ASSERT_EQ(kl_compress_stream_end(&cs), 0);

    close(pipefd[0]);
    close(pipefd[1]);
    kl_response_free(&res);
}

UTEST(compress, encoding_header) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    g_factory_fail = 0;

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    const char *data = "This is some data that gets compressed with mock.";

    ASSERT_EQ(kl_response_body_compress(&res, &cfg, data, strlen(data)), 0);

    /* Verify encoding() value used in Content-Encoding */
    res.hdr_buf[res.hdr_len] = '\0';
    ASSERT_TRUE(strstr(res.hdr_buf, "Content-Encoding: mock\r\n") != NULL);

    kl_response_free(&res);
}

UTEST(compress, destroy_called_on_success) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    g_factory_fail = 0;

    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };

    ASSERT_EQ(kl_response_body_compress(&res, &cfg, "test data here!", 15), 0);
    ASSERT_TRUE(g_mock.destroy_called);

    kl_response_free(&res);
}

UTEST(compress, destroy_called_on_error) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    KlCompressConfig cfg = {
        .ctx = NULL,
        .factory = mock_factory_compress_fail
    };

    ASSERT_EQ(kl_response_body_compress(&res, &cfg, "data", 4), -1);
    ASSERT_TRUE(g_mock.destroy_called);

    kl_response_free(&res);
}

UTEST(compress, null_response) {
    KlCompressConfig cfg = { .ctx = NULL, .factory = mock_factory };
    ASSERT_EQ(kl_response_body_compress(NULL, &cfg, "data", 4), -1);
}

UTEST(compress, stream_null_args) {
    ASSERT_EQ(kl_compress_stream_begin(NULL, NULL, 200, NULL), -1);
    ASSERT_EQ(kl_compress_stream_write(NULL, "data", 4), -1);
    ASSERT_EQ(kl_compress_stream_end(NULL), -1);
}

UTEST_MAIN();
