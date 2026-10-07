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

/* --- Extensions and trailers: line endings, control bytes, length (the chunk-extension smuggling
 * class). A front end that ends a chunk-size line at a bare LF frames the body differently from a
 * decoder that skips to the next CR, so a bare LF, and any control byte, is an error; and the bytes
 * of an extension or a trailer section are capped (they count toward no body limit). --- */

/* Feed `s` whole, then again one byte at a time; both verdicts must equal `want`. */
static int decode_both_ways(const char *s, size_t n, int want) {
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    if (kl_http1_chunked_decode(&dec, s, n, NULL) != want) return 0;
    kl_http1_chunked_init(&dec);
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) rc = kl_http1_chunked_decode(&dec, s + i, 1, NULL);
    return rc == want;
}

UTEST(chunked, extension_bare_lf_rejected) {
    /* Decoded as "5;x" + LF here, the next line is the data; skipped to the CR, "hello" became
     * part of the extension and the data started after it. */
    const char *input = "5;x\nhello\r\n0\r\n\r\n";
    ASSERT_TRUE(decode_both_ways(input, strlen(input), -1));
}

UTEST(chunked, extension_control_byte_rejected) {
    const char in1[] = "5;a\x01" "b\r\nhello\r\n0\r\n\r\n";
    ASSERT_TRUE(decode_both_ways(in1, sizeof in1 - 1, -1));
    const char in2[] = "5;a\x7f\r\nhello\r\n0\r\n\r\n";
    ASSERT_TRUE(decode_both_ways(in2, sizeof in2 - 1, -1));
    const char in3[] = "5;a\0b\r\nhello\r\n0\r\n\r\n";
    ASSERT_TRUE(decode_both_ways(in3, sizeof in3 - 1, -1));
}

UTEST(chunked, extension_lone_cr_rejected) {
    const char *input = "5;a\rb\r\nhello\r\n0\r\n\r\n";
    ASSERT_TRUE(decode_both_ways(input, strlen(input), -1));
}

UTEST(chunked, valid_extensions_accepted) {
    /* BWS (SP / HTAB) inside, several extensions, a quoted value, obs-text. */
    const char input[] = "5;a=1;\tb = \"q v\";c\x80\r\nhello\r\n0;last\r\n\r\n";
    ASSERT_TRUE(decode_both_ways(input, sizeof input - 1, 1));
    KlHttp1ChunkedDecoder dec;
    kl_http1_chunked_init(&dec);
    MockReader m = make_mock();
    ASSERT_EQ(kl_http1_chunked_decode(&dec, input, sizeof input - 1, &m.base), 1);
    ASSERT_EQ(m.len, (size_t)5);
    ASSERT_TRUE(memcmp(m.data, "hello", 5) == 0);
}

UTEST(chunked, over_long_extension_rejected) {
    static char input[20000];
    size_t n = 0;
    input[n++] = '5';
    input[n++] = ';';
    memset(input + n, 'a', 16384);
    n += 16384;
    memcpy(input + n, "\r\nhello\r\n0\r\n\r\n", 14);
    n += 14;
    ASSERT_TRUE(decode_both_ways(input, n, -1));
}

UTEST(chunked, moderate_extension_accepted) {
    static char input[2000];
    size_t n = 0;
    input[n++] = '5';
    input[n++] = ';';
    memset(input + n, 'a', 1000);
    n += 1000;
    memcpy(input + n, "\r\nhello\r\n0\r\n\r\n", 14);
    n += 14;
    ASSERT_TRUE(decode_both_ways(input, n, 1));
}

UTEST(chunked, trailer_bare_lf_rejected) {
    const char *in1 = "5\r\nhello\r\n0\r\nX: y\n\r\n";
    ASSERT_TRUE(decode_both_ways(in1, strlen(in1), -1));
    const char *in2 = "5\r\nhello\r\n0\r\n\n";            /* a bare-LF final line */
    ASSERT_TRUE(decode_both_ways(in2, strlen(in2), -1));
}

UTEST(chunked, trailer_control_byte_rejected) {
    const char in1[] = "5\r\nhello\r\n0\r\nX: \x01y\r\n\r\n";
    ASSERT_TRUE(decode_both_ways(in1, sizeof in1 - 1, -1));
    const char in2[] = "5\r\nhello\r\n0\r\nX: y\x7f\r\n\r\n";
    ASSERT_TRUE(decode_both_ways(in2, sizeof in2 - 1, -1));
    const char in3[] = "5\r\nhello\r\n0\r\nX: y\rz\r\n\r\n";   /* a lone CR */
    ASSERT_TRUE(decode_both_ways(in3, sizeof in3 - 1, -1));
}

UTEST(chunked, valid_trailers_accepted) {
    const char input[] = "5\r\nhello\r\n0\r\nA: 1\r\nB:\tv w \x80\r\n\r\n";
    ASSERT_TRUE(decode_both_ways(input, sizeof input - 1, 1));
}

UTEST(chunked, over_long_trailer_line_rejected) {
    static char input[20000];
    size_t n = 0;
    memcpy(input, "5\r\nhello\r\n0\r\nX: ", 16);
    n += 16;
    memset(input + n, 'v', 16384);
    n += 16384;
    memcpy(input + n, "\r\n\r\n", 4);
    n += 4;
    ASSERT_TRUE(decode_both_ways(input, n, -1));
}

UTEST(chunked, over_long_trailer_section_rejected) {
    /* Many short lines: each is small, the section is not. */
    static char input[40000];
    size_t n = 0;
    memcpy(input, "5\r\nhello\r\n0\r\n", 13);
    n += 13;
    for (int i = 0; i < 600; i++) {
        memcpy(input + n, "X-Trailer: 0123456789abcdef0123456789\r\n", 39);
        n += 39;
    }
    memcpy(input + n, "\r\n", 2);
    n += 2;
    ASSERT_TRUE(decode_both_ways(input, n, -1));
}

UTEST_MAIN();
