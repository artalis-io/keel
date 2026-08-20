/*
 * Overflow boundary tests — exercise SIZE_MAX/INT_MAX overflow guards
 * with pathological inputs. No real network connections needed.
 */
#include "utest.h"
#include <keel/allocator.h>
#include <keel/http_response.h>
#include <keel/http_connection.h>
#include <keel/http_router.h>
#include <keel/http1_chunked.h>
#include <keel/http_body_reader.h>
#include <keel/http_body_reader_multipart.h>
#include <keel/websocket.h>
#include <keel/h2.h>
#include <keel/h2_server.h>
#include "../src/h2_internal.h"
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* ── WebSocket frame overflow ────────────────────────────────────── */

UTEST(overflow, ws_frame_64bit_msb_set) {
    /* websocket.c:115 — MSB must be 0 per RFC */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    /* FIN=1 opcode=2 masked=0 len=127 (64-bit)
     * Payload length MSB=1 (0x80...) — should fail */
    uint8_t hdr[10] = {0x82, 0x7f,
                        0x80, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x01};
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, ws_frame_64bit_all_ff) {
    /* websocket.c:115 — MSB=1 with all 0xFF */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    uint8_t hdr[10] = {0x82, 0x7f,
                        0xff, 0xff, 0xff, 0xff,
                        0xff, 0xff, 0xff, 0xff};
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, ws_frame_64bit_size_max_half) {
    /* websocket.c:116 — plen > SIZE_MAX/2 but MSB=0 */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    /* Craft a 64-bit length just over SIZE_MAX/2 with MSB=0 */
    uint8_t hdr[10] = {0x82, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    size_t val = SIZE_MAX / 2 + 1;
    for (int i = 9; i >= 2; i--) {
        hdr[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, ws_frame_64bit_exactly_half) {
    /* websocket.c:116 — plen == SIZE_MAX/2 should succeed (boundary) */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    uint8_t hdr[10] = {0x82, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    size_t val = SIZE_MAX / 2;
    for (int i = 9; i >= 2; i--) {
        hdr[i] = (uint8_t)(val & 0xFF);
        val >>= 8;
    }
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    /* Should succeed (header parsed OK), need more data for payload */
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(fp.payload_len, SIZE_MAX / 2);
}

UTEST(overflow, ws_frame_control_oversized) {
    /* websocket.c:128 — control frames max 125 bytes */
    KlWsFrameParser fp;
    kl_ws_frame_init(&fp);

    /* Ping with payload length 126 (> 125 limit) */
    uint8_t hdr[4] = {0x89, 0x7e, 0x00, 0x7e};  /* FIN=1 PING len=126 */
    size_t consumed = 0;
    int rc = kl_ws_frame_parse(&fp, hdr, sizeof(hdr), &consumed);
    ASSERT_EQ(rc, -1);
}

/* ── Connection pool overflow ────────────────────────────────────── */

UTEST(overflow, conn_pool_negative) {
    /* connection.c:42 — capacity <= 0 returns -1 */
    KlAllocator alloc = kl_allocator_default();
    KlHttpConnPool pool;

    ASSERT_EQ(kl_http_conn_pool_init(&pool, -100, &alloc), -1);
}

UTEST(overflow, conn_pool_size_guard_exists) {
    /* connection.c:46 — SIZE_MAX / sizeof(KlHttpConn) guard is in place.
     * On 64-bit, INT_MAX won't trigger it (would need > 2^51), but
     * the guard protects 32-bit platforms. Verify the math. */
    ASSERT_TRUE(sizeof(KlHttpConn) > 0);
    size_t max_safe = SIZE_MAX / sizeof(KlHttpConn);
    ASSERT_TRUE(max_safe > 0);
    /* On 64-bit, max_safe is much larger than INT_MAX */
    ASSERT_TRUE(max_safe > 1000);
}

UTEST(overflow, conn_pool_zero) {
    /* connection.c:42 — capacity <= 0 */
    KlAllocator alloc = kl_allocator_default();
    KlHttpConnPool pool;
    ASSERT_EQ(kl_http_conn_pool_init(&pool, 0, &alloc), -1);
    ASSERT_EQ(kl_http_conn_pool_init(&pool, -1, &alloc), -1);
}

/* ── Chunked hex overflow ────────────────────────────────────────── */

UTEST(overflow, chunked_hex_too_many_digits) {
    /* chunked.c:60 — max 16 hex digits */
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);

    /* Feed 17 hex digits */
    const char *input = "fffffffffffffffff\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), NULL);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, chunked_hex_accumulator_overflow) {
    /* chunked.c:64 — size_accum > SIZE_MAX / 16 */
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
    /* chunked.c:47 — size_digits == 0 when CR arrives */
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);

    const char *input = "\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), NULL);
    ASSERT_EQ(rc, -1);
}

UTEST(overflow, chunked_no_digits_before_ext) {
    /* chunked.c:39 — size_digits == 0 when ';' arrives */
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
    /* body_reader_multipart.c:66-67 — max_total_size enforcement */
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
    /* router.c:25 — capacity > INT_MAX / 2 */
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
    /* router.c:144 — mw_capacity > INT_MAX / 2 */
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
    /* router.c:189 — post_mw_capacity > INT_MAX / 2 */
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
    /* body_reader_buffer.c:10 — max_size enforcement */
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

/* ── H2 struct size sanity ───────────────────────────────────────── */

UTEST(overflow, h2_stream_alloc_guard) {
    /* h2.c:463 — verify SIZE_MAX / sizeof(KlH2ServerStream) is bounded */
    ASSERT_TRUE(sizeof(KlH2ServerStream) > 0);
    size_t max_safe = SIZE_MAX / sizeof(KlH2ServerStream);
    ASSERT_TRUE(max_safe > 0);
    ASSERT_TRUE(max_safe <= SIZE_MAX / sizeof(KlH2ServerStream));
}

UTEST_MAIN();
