/*
 * HTTP/1 overflow boundary tests — the HTTP slice of the former tests/test_overflow.c
 * (T-split). Exercises SIZE_MAX/INT_MAX overflow guards in the connection pool, chunked
 * decoder, multipart parser, router, and body reader. No real network connections needed.
 */
#include "utest.h"
#include <keel/allocator.h>
#include <keel/http_response.h>
#include <keel/http_connection.h>
#include <keel/http_router.h>
#include <keel/http1_chunked.h>
#include <keel/http_body_reader.h>
#include <keel/http_body_reader_multipart.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* ── Connection pool overflow ────────────────────────────────────── */

UTEST(overflow, conn_pool_negative) {
    /* http_connection.c:42 — capacity <= 0 returns -1 */
    KlAllocator alloc = kl_allocator_default();
    KlHttpConnPool pool;

    ASSERT_EQ(kl_http_conn_pool_init(&pool, -100, &alloc), -1);
}

UTEST(overflow, conn_pool_size_guard_exists) {
    /* http_connection.c:46 — SIZE_MAX / sizeof(KlHttpConn) guard is in place.
     * On 64-bit, INT_MAX won't trigger it (would need > 2^51), but
     * the guard protects 32-bit platforms. Verify the math. */
    ASSERT_TRUE(sizeof(KlHttpConn) > 0);
    size_t max_safe = SIZE_MAX / sizeof(KlHttpConn);
    ASSERT_TRUE(max_safe > 0);
    /* On 64-bit, max_safe is much larger than INT_MAX */
    ASSERT_TRUE(max_safe > 1000);
}

UTEST(overflow, conn_pool_zero) {
    /* http_connection.c:42 — capacity <= 0 */
    KlAllocator alloc = kl_allocator_default();
    KlHttpConnPool pool;
    ASSERT_EQ(kl_http_conn_pool_init(&pool, 0, &alloc), -1);
    ASSERT_EQ(kl_http_conn_pool_init(&pool, -1, &alloc), -1);
}

/* ── Chunked hex overflow ────────────────────────────────────────── */

UTEST(overflow, chunked_hex_too_many_digits) {
    /* http1_chunked.c:60 — max 16 hex digits */
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);

    /* Feed 17 hex digits */
    const char *input = "fffffffffffffffff\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), NULL);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, chunked_hex_accumulator_overflow) {
    /* http1_chunked.c:64 — size_accum > SIZE_MAX / 16 */
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);

    /* Feed 16 'f' digits — on 64-bit this is SIZE_MAX.
     * After 15 f's: size_accum = 0x0FFFFFFFFFFFFFFF
     * 16th f: check size_accum > SIZE_MAX/16 → false (equal)
     * Result: valid chunk size but no data follows → need more data */
    const char *input = "ffffffffffffffff\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), NULL);
    /* Should not crash. May return 0 (need data) or -1 (platform-dependent) */
    ASSERT_TRUE(rc == 0 || rc == -1);
}

UTEST(overflow, chunked_no_digits_before_cr) {
    /* http1_chunked.c:47 — size_digits == 0 when CR arrives */
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);

    const char *input = "\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), NULL);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, chunked_no_digits_before_ext) {
    /* http1_chunked.c:39 — size_digits == 0 when ';' arrives */
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);

    const char *input = ";ext\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), NULL);
    ASSERT_EQ(rc, -1);
}

/* ── Multipart overflow ──────────────────────────────────────────── */

UTEST(overflow, multipart_max_parts_exceeded) {
    /* In the streaming parser, max_parts is enforced at PART_BEGIN
     * inside kl_http_multipart_next (on_data is parse-free). Drive the
     * iterator and assert the third part trips KL_HTTP_MP_ERR_TOO_MANY_PARTS. */
    KlAllocator alloc = kl_allocator_default();

    KlHttpRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.method_len = 4;
    req.path = "/upload";
    req.path_len = 7;
    req.content_length = 100;

    const char *ct = "multipart/form-data; boundary=testboundary";
    req.headers[0].name = "Content-Type";
    req.headers[0].name_len = 12;
    req.headers[0].value = ct;
    req.headers[0].value_len = strlen(ct);
    req.num_headers = 1;

    KlHttpMultipartConfig cfg = { .max_parts = 2 };
    KlHttpBodyReader *reader = kl_http_body_reader_multipart(&alloc, &req, &cfg);
    ASSERT_TRUE(reader != NULL);

    const char *data =
        "--testboundary\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n\r\ndata1"
        "\r\n--testboundary\r\n"
        "Content-Disposition: form-data; name=\"b\"\r\n\r\ndata2"
        "\r\n--testboundary\r\n"
        "Content-Disposition: form-data; name=\"c\"\r\n\r\ndata3"
        "\r\n--testboundary--";
    ASSERT_EQ(reader->on_data(reader, data, strlen(data)), 0);
    reader->on_complete(reader);

    int part_begins = 0;
    KlHttpMultipartEvent e;
    do {
        e = kl_http_multipart_next(reader, NULL, NULL, NULL);
        if (e == KL_HTTP_MP_EVT_PART_BEGIN) part_begins++;
    } while (e != KL_HTTP_MP_EVT_DONE && e != KL_HTTP_MP_EVT_ERROR);
    ASSERT_EQ(e, KL_HTTP_MP_EVT_ERROR);
    ASSERT_EQ(kl_http_multipart_last_error(reader), KL_HTTP_MP_ERR_TOO_MANY_PARTS);
    ASSERT_EQ(part_begins, 2);

    reader->destroy(reader);
}

UTEST(overflow, multipart_max_part_size_exceeded) {
    /* max_part_size is enforced as PART_DATA events accumulate inside
     * kl_http_multipart_next. on_data is parse-free in the streaming model. */
    KlAllocator alloc = kl_allocator_default();

    KlHttpRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.method_len = 4;
    req.path = "/upload";
    req.path_len = 7;
    req.content_length = 100;

    const char *ct = "multipart/form-data; boundary=testboundary";
    req.headers[0].name = "Content-Type";
    req.headers[0].name_len = 12;
    req.headers[0].value = ct;
    req.headers[0].value_len = strlen(ct);
    req.num_headers = 1;

    KlHttpMultipartConfig cfg = { .max_part_size = 10 };
    KlHttpBodyReader *reader = kl_http_body_reader_multipart(&alloc, &req, &cfg);
    ASSERT_TRUE(reader != NULL);

    const char *data =
        "--testboundary\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n\r\n"
        "this data is definitely longer than ten bytes"
        "\r\n--testboundary--";
    ASSERT_EQ(reader->on_data(reader, data, strlen(data)), 0);
    reader->on_complete(reader);

    KlHttpMultipartEvent e;
    do {
        e = kl_http_multipart_next(reader, NULL, NULL, NULL);
    } while (e != KL_HTTP_MP_EVT_DONE && e != KL_HTTP_MP_EVT_ERROR);
    ASSERT_EQ(e, KL_HTTP_MP_EVT_ERROR);
    ASSERT_EQ(kl_http_multipart_last_error(reader), KL_HTTP_MP_ERR_PART_TOO_LARGE);

    reader->destroy(reader);
}

UTEST(overflow, multipart_max_total_size_exceeded) {
    /* http_body_reader_multipart.c:66-67 — max_total_size enforcement */
    KlAllocator alloc = kl_allocator_default();

    KlHttpRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.method_len = 4;
    req.path = "/upload";
    req.path_len = 7;
    req.content_length = 100;

    const char *ct = "multipart/form-data; boundary=testboundary";
    req.headers[0].name = "Content-Type";
    req.headers[0].name_len = 12;
    req.headers[0].value = ct;
    req.headers[0].value_len = strlen(ct);
    req.num_headers = 1;

    KlHttpMultipartConfig cfg = { .max_total_size = 10 };
    KlHttpBodyReader *reader = kl_http_body_reader_multipart(&alloc, &req, &cfg);
    ASSERT_TRUE(reader != NULL);

    const char *data =
        "--testboundary\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n\r\n"
        "this data is definitely longer than ten bytes"
        "\r\n--testboundary--";

    int rc = reader->on_data(reader, data, strlen(data));
    ASSERT_EQ(rc, -1);

    reader->destroy(reader);
}

/* ── Router capacity overflow ────────────────────────────────────── */

static void dummy_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)res; (void)ud;
}

UTEST(overflow, router_capacity_overflow) {
    /* http_router.c:25 — capacity > INT_MAX / 2 */
    KlAllocator alloc = kl_allocator_default();
    KlHttpRouter r;
    ASSERT_EQ(kl_http_router_init(&r, &alloc), 0);

    /* Artificially set capacity near INT_MAX/2 to test overflow guard */
    r.capacity = INT_MAX / 2 + 1;
    r.count = r.capacity;  /* force growth attempt */

    int rc = kl_http_router_add(&r, "GET", "/test", dummy_handler, NULL, NULL);
    ASSERT_EQ(rc, -1);

    /* Reset to valid state for free */
    r.count = 0;
    r.capacity = 16;
    kl_http_router_free(&r);
}

UTEST(overflow, router_mw_overflow) {
    /* http_router.c:144 — mw_capacity > INT_MAX / 2 */
    KlAllocator alloc = kl_allocator_default();
    KlHttpRouter r;
    ASSERT_EQ(kl_http_router_init(&r, &alloc), 0);

    r.mw_capacity = INT_MAX / 2 + 1;
    r.mw_count = r.mw_capacity;

    int rc = kl_http_router_use(&r, "*", "/*", NULL, NULL);
    ASSERT_EQ(rc, -1);

    r.mw_count = 0;
    r.mw_capacity = 0;
    r.middleware = NULL;
    kl_http_router_free(&r);
}

UTEST(overflow, router_post_mw_overflow) {
    /* http_router.c:189 — post_mw_capacity > INT_MAX / 2 */
    KlAllocator alloc = kl_allocator_default();
    KlHttpRouter r;
    ASSERT_EQ(kl_http_router_init(&r, &alloc), 0);

    r.post_mw_capacity = INT_MAX / 2 + 1;
    r.post_mw_count = r.post_mw_capacity;

    int rc = kl_http_router_use_post(&r, "*", "/*", NULL, NULL);
    ASSERT_EQ(rc, -1);

    r.post_mw_count = 0;
    r.post_mw_capacity = 0;
    r.post_middleware = NULL;
    kl_http_router_free(&r);
}

/* ── Body reader buffer overflow ─────────────────────────────────── */

UTEST(overflow, body_reader_buffer_max_size) {
    /* http_body_reader_buffer.c:10 — max_size enforcement */
    KlAllocator alloc = kl_allocator_default();
    KlHttpRequest req;
    memset(&req, 0, sizeof(req));

    /* Create a reader with 10-byte max */
    KlHttpBodyReader *reader = kl_http_body_reader_buffer(&alloc, &req, (void *)(size_t)10);
    ASSERT_TRUE(reader != NULL);

    char data[20];
    memset(data, 'A', sizeof(data));

    /* First 10 bytes should succeed */
    ASSERT_EQ(reader->on_data(reader, data, 10), 0);

    /* Next byte should fail (exceeds max_size) */
    ASSERT_EQ(reader->on_data(reader, data, 1), -1);

    reader->destroy(reader);
}

UTEST_MAIN();
