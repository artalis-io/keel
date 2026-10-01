/*
 * test_http_client_alloc_sizes.c: every block a client response owns is freed with the size it was
 * allocated with (audit L10, HTTP part).
 *
 * KlAllocator.free takes a size, and a sized allocator (an arena, a pool, a size-class allocator)
 * relies on it. Two response paths broke it:
 *   - kl_http_client_remove_header dropped num_headers without resizing the header array, and
 *     kl_http_client_response_free then freed the array at the new, smaller size;
 *   - a decompressed body was adopted as is: the decompressor allocates out_len bytes (its contract
 *     is "free with out_len"), but a response body is freed at body_len + 1, and it was not
 *     NUL-terminated like every other body.
 * The allocator here records the size of every live block and counts frees whose size differs.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/http_client.h>
#include <keel/http1_parser.h>
#include <keel/decompress.h>
#include "http_client_internal.h"
#include <stdlib.h>
#include <string.h>

/* ── Size-checking allocator ────────────────────────────────────────────────────────────────── */

#define TMAX 1024
static struct { void *p; size_t n; } g_live[TMAX];
static int g_bad_frees, g_unknown_frees;

static void track(void *p, size_t n) {
    for (int i = 0; i < TMAX; i++) if (!g_live[i].p) { g_live[i].p = p; g_live[i].n = n; return; }
}
static void untrack(void *p, size_t n) {
    for (int i = 0; i < TMAX; i++)
        if (g_live[i].p == p) {
            if (g_live[i].n != n) g_bad_frees++;
            g_live[i].p = NULL;
            return;
        }
    g_unknown_frees++;
}
static int live_blocks(void) {
    int n = 0;
    for (int i = 0; i < TMAX; i++) if (g_live[i].p) n++;
    return n;
}
static void *s_malloc(void *c, size_t n) { (void)c; void *p = malloc(n ? n : 1); if (p) track(p, n); return p; }
static void *s_realloc(void *c, void *p, size_t o, size_t n) {
    (void)c;
    if (p) untrack(p, o);
    void *q = realloc(p, n ? n : 1);
    if (q) track(q, n);
    return q;
}
static void s_free(void *c, void *p, size_t n) { (void)c; if (p) { untrack(p, n); free(p); } }
static KlAllocator g_sa = { s_malloc, s_realloc, s_free, NULL };

static void reset_tracking(void) { memset(g_live, 0, sizeof g_live); g_bad_frees = 0; g_unknown_frees = 0; }

/* Parse a canned response with the size-checking allocator. */
static int parse(const char *raw, KlHttpClientResponse *resp) {
    KlHttp1ResponseParser *p = kl_http1_response_parser_llhttp(0, &g_sa);
    if (!p) return -1;
    memset(resp, 0, sizeof(*resp));
    size_t consumed = 0;
    KlHttp1ParseResult r = p->parse(p, resp, raw, strlen(raw), &consumed);
    p->destroy(p);
    return r == KL_HTTP1_PARSE_OK ? 0 : -1;
}

UTEST(alloc_sizes, remove_header_then_free_uses_the_allocated_sizes) {
    reset_tracking();
    KlHttpClientResponse r;
    ASSERT_EQ(parse("HTTP/1.1 200 OK\r\nA: 1\r\nContent-Encoding: x\r\nB: 2\r\n"
                    "Content-Length: 2\r\n\r\nok", &r), 0);
    ASSERT_EQ(r.num_headers, 4);
    kl_http_client_remove_header(&r, "Content-Encoding");
    ASSERT_EQ(r.num_headers, 3);
    ASSERT_TRUE(kl_http_client_find_header_value(&r, "Content-Encoding") == NULL);
    ASSERT_STREQ(kl_http_client_find_header_value(&r, "B"), "2");
    kl_http_client_remove_header(&r, "A");
    kl_http_client_remove_header(&r, "B");
    kl_http_client_remove_header(&r, "Content-Length");      /* down to zero headers */
    ASSERT_EQ(r.num_headers, 0);
    kl_http_client_response_free(&r);
    ASSERT_EQ(g_bad_frees, 0);                                 /* was: the array freed short */
    ASSERT_EQ(g_unknown_frees, 0);
    ASSERT_EQ(live_blocks(), 0);
}

/* A mock decompressor: "decompresses" by doubling every byte, allocating exactly out_len bytes as
 * the KlDecompress contract says. */
static int md_decompress(KlDecompress *self, const char *in, size_t in_len, char **out,
                         size_t *out_len, KlAllocator *alloc) {
    (void)self;
    char *b = kl_malloc(alloc, in_len * 2);
    if (!b) return -1;
    for (size_t i = 0; i < in_len; i++) { b[2 * i] = in[i]; b[2 * i + 1] = in[i]; }
    *out = b;
    *out_len = in_len * 2;
    return 0;
}
static int md_dfeed(KlDecompress *self, const char *d, size_t n, int flush,
                    int (*emit)(void *, const char *, size_t), void *ctx) {
    (void)self; (void)d; (void)n; (void)flush; (void)emit; (void)ctx;
    return -1;
}
static const char *md_encoding(KlDecompress *self) { (void)self; return "x-double"; }
static void md_reset(KlDecompress *self) { (void)self; }
static void md_destroy(KlDecompress *self) { (void)self; }
static KlDecompress g_md = { md_decompress, md_dfeed, md_encoding, md_reset, md_destroy };
static KlDecompress *md_factory(KlCompressCtx *ctx, KlAllocator *alloc) { (void)ctx; (void)alloc; return &g_md; }

UTEST(alloc_sizes, decompressed_body_is_freed_with_its_size_and_terminated) {
    reset_tracking();
    KlHttpClientResponse r;
    ASSERT_EQ(parse("HTTP/1.1 200 OK\r\nContent-Encoding: x-double\r\nContent-Length: 3\r\n\r\nabc",
                    &r), 0);
    KlDecompressConfig dc;
    memset(&dc, 0, sizeof dc);
    dc.factory = md_factory;
    ASSERT_EQ(kl_http_client_decompress_response_body(&r, &dc), 0);
    ASSERT_EQ(r.body_len, (size_t)6);
    ASSERT_EQ(memcmp(r.body, "aabbcc", 6), 0);
    ASSERT_EQ(r.body[6], '\0');                                /* terminated like every body */
    ASSERT_TRUE(kl_http_client_find_header_value(&r, "Content-Encoding") == NULL);
    kl_http_client_response_free(&r);
    ASSERT_EQ(g_bad_frees, 0);                                 /* was: freed at 7, allocated at 6 */
    ASSERT_EQ(g_unknown_frees, 0);
    ASSERT_EQ(live_blocks(), 0);
}

UTEST_MAIN();
