#include "utest.h"
#include <keel/http1_chunked.h>
#include <string.h>

/* --- Mock body reader that accumulates data into a buffer --- */

typedef struct {
    KlHttpBodyReader base;
    char data[65536];
    size_t len;
    int reject_after;  /* return -1 after this many bytes; 0 = never */
} MockReader;

static int mock_on_data(KlHttpBodyReader *self, const char *data, size_t len) {
    MockReader *m = (MockReader *)self;
    if (m->reject_after > 0 && m->len + len > (size_t)m->reject_after)
        return -1;
    memcpy(m->data + m->len, data, len);
    m->len += len;
    return 0;
}

static void mock_noop(KlHttpBodyReader *self) { (void)self; }

static MockReader make_mock(void) {
    MockReader m;
    memset(&m, 0, sizeof(m));
    m.base.on_data = mock_on_data;
    m.base.on_complete = mock_noop;
    m.base.on_error = mock_noop;
    m.base.destroy = mock_noop;
    return m;
}

/* --- Tests --- */

UTEST(chunked, single_chunk) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    const char *input = "5\r\nhello\r\n0\r\n\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)5);
    ASSERT_TRUE(memcmp(m.data, "hello", 5) == 0);
}

UTEST(chunked, multi_chunk) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    const char *input = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)11);
    ASSERT_TRUE(memcmp(m.data, "hello world", 11) == 0);
}

UTEST(chunked, empty_body) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    const char *input = "0\r\n\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)0);
}

UTEST(chunked, large_hex) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    /* "a" = 10 decimal */
    const char *input = "a\r\n0123456789\r\n0\r\n\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)10);
    ASSERT_TRUE(memcmp(m.data, "0123456789", 10) == 0);
}

UTEST(chunked, uppercase_hex) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    /* "A" = 10 decimal */
    char body[32];
    memset(body, 'X', 10);
    char input[64];
    memcpy(input, "A\r\n", 3);
    memcpy(input + 3, body, 10);
    memcpy(input + 13, "\r\n0\r\n\r\n", 7);

    int rc = kl_http1_chunked_decode(&dec, input, 20, &m.base);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)10);
}

UTEST(chunked, chunk_extensions) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    const char *input = "5;ext=val\r\nhello\r\n0\r\n\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)5);
    ASSERT_TRUE(memcmp(m.data, "hello", 5) == 0);
}

UTEST(chunked, trailers) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    const char *input = "5\r\nhello\r\n0\r\n"
                        "Trailer-Key: value\r\n"
                        "\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)5);
    ASSERT_TRUE(memcmp(m.data, "hello", 5) == 0);
}

UTEST(chunked, byte_at_a_time) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    const char *input = "5\r\nhello\r\n3\r\nfoo\r\n0\r\n\r\n";
    size_t total = strlen(input);
    int rc = 0;

    for (size_t i = 0; i < total; i++) {
        rc = kl_http1_chunked_decode(&dec, input + i, 1, &m.base);
        if (rc != 0) break;
    }
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)8);
    ASSERT_TRUE(memcmp(m.data, "hellofoo", 8) == 0);
}

UTEST(chunked, invalid_hex) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    const char *input = "5G\r\nhello\r\n0\r\n\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, -1);
}

UTEST(chunked, overflow_hex) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    /* 17 hex digits: exceeds 16-digit max */
    const char *input = "12345678901234567\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, -1);
}

UTEST(chunked, missing_crlf) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    /* LF without CR after chunk size */
    const char *input = "5\nhello\r\n0\r\n\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, -1);
}

UTEST(chunked, reader_reject) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();
    m.reject_after = 3;  /* reject after 3 bytes */

    const char *input = "5\r\nhello\r\n0\r\n\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, -1);
}

UTEST(chunked, null_reader_discard) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);

    /* NULL reader: data is consumed but discarded */
    const char *input = "5\r\nhello\r\n0\r\n\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), NULL);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(dec.total_body, (size_t)5);
}

UTEST(chunked, multiple_trailers) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    const char *input = "3\r\nabc\r\n0\r\n"
                        "Trailer1: val1\r\n"
                        "Trailer2: val2\r\n"
                        "\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)3);
    ASSERT_TRUE(memcmp(m.data, "abc", 3) == 0);
}

UTEST(chunked, split_across_calls) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    /* Split in the middle of chunk data */
    const char *p1 = "5\r\nhel";
    const char *p2 = "lo\r\n0\r\n\r\n";

    int rc = kl_http1_chunked_decode(&dec, p1, strlen(p1), &m.base);
    ASSERT_EQ(rc, 0);

    rc = kl_http1_chunked_decode(&dec, p2, strlen(p2), &m.base);
    ASSERT_EQ(rc, 1);
    ASSERT_EQ(m.len, (size_t)5);
    ASSERT_TRUE(memcmp(m.data, "hello", 5) == 0);
}

UTEST(chunked, no_digits_before_cr) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    /* CR with no hex digits: error */
    const char *input = "\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, -1);
}

UTEST(chunked, no_digits_before_semicolon) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();

    /* Semicolon with no hex digits: error */
    const char *input = ";ext\r\n";
    int rc = kl_http1_chunked_decode(&dec, input, strlen(input), &m.base);
    ASSERT_EQ(rc, -1);
}

UTEST_MAIN();
