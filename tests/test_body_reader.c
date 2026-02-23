#include "utest.h"
#include <keel/keel.h>
#include <string.h>

/* ── Buffer reader tests ────────────────────────────────────────────── */

UTEST(buf, create_destroy) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);
    br->destroy(br);
}

UTEST(buf, single_chunk) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    ASSERT_EQ(br->on_data(br, "hello", 5), 0);
    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)5);
    ASSERT_TRUE(memcmp(b->data, "hello", 5) == 0);

    br->destroy(br);
}

UTEST(buf, multi_chunk) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);

    ASSERT_EQ(br->on_data(br, "abc", 3), 0);
    ASSERT_EQ(br->on_data(br, "def", 3), 0);
    ASSERT_EQ(br->on_data(br, "ghi", 3), 0);
    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)9);
    ASSERT_TRUE(memcmp(b->data, "abcdefghi", 9) == 0);

    br->destroy(br);
}

UTEST(buf, growth) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);

    /* Feed > 1024 bytes (initial cap) to trigger realloc */
    char chunk[256];
    memset(chunk, 'A', sizeof(chunk));
    for (int i = 0; i < 8; i++)
        ASSERT_EQ(br->on_data(br, chunk, sizeof(chunk)), 0);
    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)(256 * 8));
    ASSERT_TRUE(b->cap >= b->len);

    br->destroy(br);
}

UTEST(buf, max_size_reject) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, (void *)(size_t)100);

    char data[60];
    memset(data, 'X', sizeof(data));
    ASSERT_EQ(br->on_data(br, data, 60), 0);
    /* This should exceed the 100-byte limit */
    ASSERT_EQ(br->on_data(br, data, 60), -1);

    br->destroy(br);
}

UTEST(buf, exact_max_size) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, (void *)(size_t)100);

    char data[100];
    memset(data, 'Y', sizeof(data));
    ASSERT_EQ(br->on_data(br, data, 100), 0);
    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)100);

    br->destroy(br);
}

UTEST(buf, empty_body) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = {0};
    KlBodyReader *br = kl_body_reader_buffer(&a, &req, NULL);

    br->on_complete(br);

    KlBufReader *b = (KlBufReader *)br;
    ASSERT_EQ(b->len, (size_t)0);

    br->destroy(br);
}

/* ── Multipart reader tests ─────────────────────────────────────────── */

/* Helper: build a fake KlRequest with Content-Type header */
static KlRequest make_mp_request(const char *content_type) {
    KlRequest req = {0};
    req.method = "POST";
    req.method_len = 4;
    req.path = "/upload";
    req.path_len = 7;
    req.headers[0].name = "Content-Type";
    req.headers[0].name_len = 12;
    req.headers[0].value = content_type;
    req.headers[0].value_len = strlen(content_type);
    req.num_headers = 1;
    return req;
}

UTEST(mp, reject_json) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request("application/json");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br == NULL);
}

UTEST(mp, reject_no_boundary) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request("multipart/form-data");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br == NULL);
}

UTEST(mp, single_field) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=abc123");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--abc123\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n"
        "\r\n"
        "value1"
        "\r\n--abc123--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "field1") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)6);
    ASSERT_TRUE(memcmp(mr->parts[0].data, "value1", 6) == 0);
    ASSERT_TRUE(mr->parts[0].filename == NULL);

    br->destroy(br);
}

UTEST(mp, two_fields) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=sep");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--sep\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n"
        "\r\n"
        "alpha"
        "\r\n--sep\r\n"
        "Content-Disposition: form-data; name=\"b\"\r\n"
        "\r\n"
        "beta"
        "\r\n--sep--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 2);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "a") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)5);
    ASSERT_TRUE(memcmp(mr->parts[0].data, "alpha", 5) == 0);
    ASSERT_TRUE(strcmp(mr->parts[1].name, "b") == 0);
    ASSERT_EQ(mr->parts[1].data_len, (size_t)4);
    ASSERT_TRUE(memcmp(mr->parts[1].data, "beta", 4) == 0);

    br->destroy(br);
}

UTEST(mp, file_upload) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=fileBnd");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--fileBnd\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file contents here"
        "\r\n--fileBnd--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "file") == 0);
    ASSERT_TRUE(mr->parts[0].filename != NULL);
    ASSERT_TRUE(strcmp(mr->parts[0].filename, "test.txt") == 0);
    ASSERT_TRUE(mr->parts[0].content_type != NULL);
    ASSERT_TRUE(strcmp(mr->parts[0].content_type, "text/plain") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)18);
    ASSERT_TRUE(memcmp(mr->parts[0].data, "file contents here", 18) == 0);

    br->destroy(br);
}

UTEST(mp, boundary_spanning) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=BND");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    /* Full body split so boundary "\r\n--BND" spans two on_data calls */
    const char *full =
        "--BND\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n"
        "\r\n"
        "data_here"
        "\r\n--BND--\r\n";

    size_t total = strlen(full);
    /* Find where the closing boundary starts */
    const char *bnd = strstr(full + 10, "\r\n--BND");
    ASSERT_TRUE(bnd != NULL);
    /* Split in the middle of "\r\n--BND" */
    size_t split = (size_t)(bnd - full) + 3; /* after "\r\n-" */

    ASSERT_EQ(br->on_data(br, full, split), 0);
    ASSERT_EQ(br->on_data(br, full + split, total - split), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "x") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)9);
    ASSERT_TRUE(memcmp(mr->parts[0].data, "data_here", 9) == 0);

    br->destroy(br);
}

UTEST(mp, byte_at_a_time) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=B");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--B\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n"
        "\r\n"
        "val"
        "\r\n--B--\r\n";

    size_t total = strlen(body);
    for (size_t i = 0; i < total; i++) {
        int rc = br->on_data(br, body + i, 1);
        if (rc != 0) {
            ASSERT_EQ(rc, 0); /* force failure message */
            break;
        }
    }
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "f") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)3);
    ASSERT_TRUE(memcmp(mr->parts[0].data, "val", 3) == 0);

    br->destroy(br);
}

UTEST(mp, max_part_size) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=LIM");

    KlMultipartConfig cfg = {.max_part_size = 5};
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--LIM\r\n"
        "Content-Disposition: form-data; name=\"big\"\r\n"
        "\r\n"
        "toolongdata"
        "\r\n--LIM--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), -1);
    br->destroy(br);
}

UTEST(mp, max_total_size) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=TOT");

    KlMultipartConfig cfg = {.max_total_size = 10};
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--TOT\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n"
        "\r\n"
        "12345"
        "\r\n--TOT\r\n"
        "Content-Disposition: form-data; name=\"b\"\r\n"
        "\r\n"
        "678901" /* total = 11, exceeds 10 */
        "\r\n--TOT--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), -1);
    br->destroy(br);
}

UTEST(mp, max_parts) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=MP");

    KlMultipartConfig cfg = {.max_parts = 1};
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--MP\r\n"
        "Content-Disposition: form-data; name=\"first\"\r\n"
        "\r\n"
        "ok"
        "\r\n--MP\r\n"
        "Content-Disposition: form-data; name=\"second\"\r\n"
        "\r\n"
        "fail"
        "\r\n--MP--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), -1);
    br->destroy(br);
}

UTEST(mp, empty_part) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=E");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--E\r\n"
        "Content-Disposition: form-data; name=\"empty\"\r\n"
        "\r\n"
        "\r\n--E--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "empty") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)0);

    br->destroy(br);
}

UTEST(mp, quoted_boundary) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=\"qb123\"");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--qb123\r\n"
        "Content-Disposition: form-data; name=\"q\"\r\n"
        "\r\n"
        "quoted"
        "\r\n--qb123--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "q") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)6);

    br->destroy(br);
}

UTEST(mp, binary_data) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=BIN");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    /* Build body with null bytes and binary in the part data */
    char body[256];
    const char *hdr =
        "--BIN\r\n"
        "Content-Disposition: form-data; name=\"bin\"; filename=\"data.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n";
    size_t hdr_len = strlen(hdr);
    memcpy(body, hdr, hdr_len);

    /* 8 bytes of binary data including nulls */
    char bin_data[] = {'\x00', '\x01', '\xff', '\x00', '\xfe', 'A', '\x00', '\x80'};
    memcpy(body + hdr_len, bin_data, 8);

    const char *footer = "\r\n--BIN--\r\n";
    size_t footer_len = strlen(footer);
    memcpy(body + hdr_len + 8, footer, footer_len);
    size_t total = hdr_len + 8 + footer_len;

    ASSERT_EQ(br->on_data(br, body, total), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "bin") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)8);
    ASSERT_TRUE(memcmp(mr->parts[0].data, bin_data, 8) == 0);

    br->destroy(br);
}

/* T-1: Maximum boundary length (70 chars per RFC 2046) */
UTEST(mp, max_boundary_length) {
    KlAllocator a = kl_allocator_default();
    /* 70-char boundary */
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary="
        "0123456789012345678901234567890123456789012345678901234567890123456789");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--0123456789012345678901234567890123456789012345678901234567890123456789\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n"
        "\r\n"
        "ok"
        "\r\n--0123456789012345678901234567890123456789012345678901234567890123456789--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)2);

    br->destroy(br);
}

/* T-1b: Boundary > 70 chars rejected */
UTEST(mp, boundary_too_long) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary="
        "01234567890123456789012345678901234567890123456789012345678901234567890");
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br == NULL);
}

/* T-2: Part headers exceeding hdr_buf (2048 bytes) → error */
UTEST(mp, header_overflow) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=HO");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    /* Build body with a very long header line (>2048 bytes) */
    char body[4096];
    int off = snprintf(body, sizeof(body),
                       "--HO\r\nContent-Disposition: form-data; name=\"x");
    /* Pad the header with spaces to exceed 2048 */
    while (off < 2100)
        body[off++] = 'x';
    off += snprintf(body + off, sizeof(body) - (size_t)off,
                    "\"\r\n\r\ndata\r\n--HO--\r\n");

    int rc = br->on_data(br, body, (size_t)off);
    /* Should error because headers overflow hdr_buf */
    ASSERT_EQ(rc, -1);
    br->destroy(br);
}

/* T-3: Body data containing boundary-like substring */
UTEST(mp, boundary_like_in_body) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=XYZ");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    /* Body contains "--XY" which is a partial match, and "\r\n--XY" but not "\r\n--XYZ" */
    const char *body =
        "--XYZ\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n"
        "\r\n"
        "data--XYdata\r\n--XYdata"
        "\r\n--XYZ--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    /* "data--XYdata\r\n--XYdata" = 22 bytes */
    ASSERT_EQ(mr->parts[0].data_len, (size_t)22);
    ASSERT_TRUE(memcmp(mr->parts[0].data, "data--XYdata\r\n--XYdata", 22) == 0);

    br->destroy(br);
}

/* T-4: Preamble content before first boundary */
UTEST(mp, preamble_content) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=P");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    /* Preamble text before the first --P boundary */
    const char *body =
        "This is preamble text that should be ignored.\r\n"
        "--P\r\n"
        "Content-Disposition: form-data; name=\"x\"\r\n"
        "\r\n"
        "val"
        "\r\n--P--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "x") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)3);

    br->destroy(br);
}

/* T-5: Malformed Content-Disposition (no name=) → error */
UTEST(mp, malformed_disposition) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=MD");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--MD\r\n"
        "Content-Disposition: form-data\r\n"
        "\r\n"
        "data"
        "\r\n--MD--\r\n";

    ASSERT_EQ(br->on_data(br, body, strlen(body)), -1);
    br->destroy(br);
}

/* T-7: Total size limit across multiple on_data calls (chunked) */
UTEST(mp, chunked_total_size) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=CT");

    KlMultipartConfig cfg = {.max_total_size = 8};
    KlBodyReader *br = kl_body_reader_multipart(&a, &req, &cfg);
    ASSERT_TRUE(br != NULL);

    /* Feed header in first chunk */
    const char *chunk1 =
        "--CT\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n"
        "\r\n";
    ASSERT_EQ(br->on_data(br, chunk1, strlen(chunk1)), 0);

    /* 5 bytes of data — under limit */
    ASSERT_EQ(br->on_data(br, "12345", 5), 0);

    /* 5 more bytes — exceeds 8-byte total limit */
    const char *chunk3 = "67890\r\n--CT--\r\n";
    ASSERT_EQ(br->on_data(br, chunk3, strlen(chunk3)), -1);

    br->destroy(br);
}

/* T-8: Multipart split across many on_data calls (chunked boundary) */
UTEST(mp, chunked_multipart) {
    KlAllocator a = kl_allocator_default();
    KlRequest req = make_mp_request(
        "multipart/form-data; boundary=CM");

    KlBodyReader *br = kl_body_reader_multipart(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    /* Split into 3 chunks: preamble+headers, body+boundary start, rest */
    const char *c1 = "--CM\r\nContent-Disposition: form-data; name=\"k\"\r\n\r\n";
    const char *c2 = "value-data\r\n-";
    const char *c3 = "-CM--\r\n";

    ASSERT_EQ(br->on_data(br, c1, strlen(c1)), 0);
    ASSERT_EQ(br->on_data(br, c2, strlen(c2)), 0);
    ASSERT_EQ(br->on_data(br, c3, strlen(c3)), 0);
    br->on_complete(br);

    KlMultipartReader *mr = (KlMultipartReader *)br;
    ASSERT_EQ(mr->num_parts, 1);
    ASSERT_TRUE(strcmp(mr->parts[0].name, "k") == 0);
    ASSERT_EQ(mr->parts[0].data_len, (size_t)10);
    ASSERT_TRUE(memcmp(mr->parts[0].data, "value-data", 10) == 0);

    br->destroy(br);
}

UTEST_MAIN();
