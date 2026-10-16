#include "utest.h"
#include <keel/http_sse.h>
#include <keel/http1_chunked.h>
#include <keel/allocator.h>
#include <string.h>
#include "net_compat.h"

/* ── Mock write function ─────────────────────────────────────────────── */

#define MOCK_BUF_SIZE 4096

typedef struct {
    char   buf[MOCK_BUF_SIZE];
    size_t len;
    int    fail_after;  /* -1 = never fail, 0+ = fail after N writes */
    int    write_count;
} MockCtx;

static int mock_write(void *ctx, const char *data, size_t len) {
    MockCtx *m = ctx;
    if (m->fail_after >= 0 && m->write_count >= m->fail_after)
        return -1;
    m->write_count++;
    if (m->len + len > MOCK_BUF_SIZE)
        return -1;
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return 0;
}

static void mock_init(MockCtx *m) {
    memset(m, 0, sizeof(*m));
    m->fail_after = -1;
}

/* Helper: set up an SSE handle with mock writer (bypasses kl_http_sse_begin) */
static void setup_mock_sse(KlHttpSse *sse, MockCtx *m) {
    mock_init(m);
    sse->write_fn = mock_write;
    sse->write_ctx = m;
    sse->res = NULL;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

UTEST(sse, basic_event) {
    KlHttpSse sse;
    MockCtx m;
    setup_mock_sse(&sse, &m);

    ASSERT_EQ(kl_http_sse_event(&sse, "update", "hello", 5, "42"), 0);

    const char *expected = "event: update\nid: 42\ndata: hello\n\n";
    ASSERT_EQ(m.len, strlen(expected));
    ASSERT_EQ(memcmp(m.buf, expected, m.len), 0);
}

UTEST(sse, data_only) {
    KlHttpSse sse;
    MockCtx m;
    setup_mock_sse(&sse, &m);

    ASSERT_EQ(kl_http_sse_event(&sse, NULL, "payload", 7, NULL), 0);

    const char *expected = "data: payload\n\n";
    ASSERT_EQ(m.len, strlen(expected));
    ASSERT_EQ(memcmp(m.buf, expected, m.len), 0);
}

UTEST(sse, multiline_data) {
    KlHttpSse sse;
    MockCtx m;
    setup_mock_sse(&sse, &m);

    ASSERT_EQ(kl_http_sse_event(&sse, NULL, "line1\nline2\nline3", 17, NULL), 0);

    const char *expected = "data: line1\ndata: line2\ndata: line3\n\n";
    ASSERT_EQ(m.len, strlen(expected));
    ASSERT_EQ(memcmp(m.buf, expected, m.len), 0);
}

UTEST(sse, empty_data) {
    KlHttpSse sse;
    MockCtx m;
    setup_mock_sse(&sse, &m);

    ASSERT_EQ(kl_http_sse_event(&sse, NULL, "", 0, NULL), 0);

    const char *expected = "data: \n\n";
    ASSERT_EQ(m.len, strlen(expected));
    ASSERT_EQ(memcmp(m.buf, expected, m.len), 0);
}

UTEST(sse, comment) {
    KlHttpSse sse;
    MockCtx m;
    setup_mock_sse(&sse, &m);

    ASSERT_EQ(kl_http_sse_comment(&sse, "keepalive", 9), 0);

    const char *expected = ": keepalive\n";
    ASSERT_EQ(m.len, strlen(expected));
    ASSERT_EQ(memcmp(m.buf, expected, m.len), 0);
}

UTEST(sse, write_error) {
    KlHttpSse sse;
    MockCtx m;
    setup_mock_sse(&sse, &m);
    m.fail_after = 0;  /* fail on first write */

    ASSERT_EQ(kl_http_sse_event(&sse, "test", "data", 4, NULL), -1);
}

UTEST(sse, begin_sets_headers) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);
    res.conn_fd = pipefd[1];

    KlHttpSse sse;
    ASSERT_EQ(kl_http_sse_begin(&res, &sse), 0);

    /* Verify headers were set */
    ASSERT_TRUE(strstr(res.hdr_buf, "Content-Type: text/event-stream\r\n") != NULL);
    ASSERT_TRUE(strstr(res.hdr_buf, "Cache-Control: no-cache\r\n") != NULL);
    ASSERT_TRUE(sse.write_fn != NULL);
    ASSERT_TRUE(sse.res == &res);

    kl_http_response_end_stream(&res);
    kl_test_closesock(pipefd[0]);
    kl_test_closesock(pipefd[1]);
    kl_http_response_free(&res);
}

/* kl_http_sse_end ends the underlying chunked stream: it emits the zero-length terminating chunk. */
UTEST(sse, end_terminates_stream) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    res.conn_fd = pfd[1];

    KlHttpSse sse;
    ASSERT_EQ(kl_http_sse_begin(&res, &sse), 0);
    /* drain the status line + headers that begin_stream sent */
    char buf[512];
    if (kl_test_poll1(pfd[0], 0, 500) > 0) kl_test_sockread(pfd[0], buf, sizeof(buf) - 1);

    ASSERT_EQ(kl_http_sse_end(&sse), 0);
    ASSERT_GT(kl_test_poll1(pfd[0], 0, 500), 0);
    long n = kl_test_sockread(pfd[0], buf, sizeof(buf) - 1);
    ASSERT_GT(n, (long)0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "0\r\n\r\n") != NULL);   /* chunked terminator */

    kl_test_closesock(pfd[0]);
    kl_test_closesock(pfd[1]);
    kl_http_response_free(&res);
}

UTEST(sse, end_null_is_invalid) {
    ASSERT_EQ(kl_http_sse_end(NULL), -1);
}

/* An SSE parser ends a line at CR, LF or CRLF, so a bare CR in data, or a CR or LF in an event name
 * or id, would start a field the caller never wrote (field injection). Data is split on all three;
 * an event name or id containing one is refused; a comment is written one line at a time. */
UTEST(sse, bare_cr_in_data_cannot_start_a_field) {
    KlHttpSse sse; MockCtx m; setup_mock_sse(&sse, &m);
    const char *data = "x\revent: admin\rid: 9";
    ASSERT_EQ(kl_http_sse_event(&sse, NULL, data, strlen(data), NULL), 0);
    m.buf[m.len] = '\0';
    ASSERT_STREQ(m.buf, "data: x\ndata: event: admin\ndata: id: 9\n\n");
}

UTEST(sse, crlf_in_data_is_one_line_break) {
    KlHttpSse sse; MockCtx m; setup_mock_sse(&sse, &m);
    ASSERT_EQ(kl_http_sse_event(&sse, NULL, "a\r\nb", 4, NULL), 0);
    m.buf[m.len] = '\0';
    ASSERT_STREQ(m.buf, "data: a\ndata: b\n\n");
}

UTEST(sse, line_break_in_event_or_id_is_refused) {
    KlHttpSse sse; MockCtx m; setup_mock_sse(&sse, &m);
    ASSERT_EQ(kl_http_sse_event(&sse, "a\nretry: 1", "d", 1, NULL), -1);
    ASSERT_EQ(kl_http_sse_event(&sse, "ok", "d", 1, "1\r2"), -1);
    ASSERT_EQ(m.len, (size_t)0);                 /* nothing written for a refused event */
}

UTEST(sse, multiline_comment_stays_a_comment) {
    KlHttpSse sse; MockCtx m; setup_mock_sse(&sse, &m);
    ASSERT_EQ(kl_http_sse_comment(&sse, "a\ndata: forged", 14), 0);
    m.buf[m.len] = '\0';
    ASSERT_STREQ(m.buf, ": a\n: data: forged\n");
}

/* Read all bytes already emitted by the synchronous socket-pair writer. */
static size_t read_all_now(int fd, char *buf, size_t cap) {
    size_t total = 0;
    int first = 1;
    while (total + 1 < cap) {
        if (kl_test_poll1(fd, 0, first ? 300 : 0) <= 0) break;
        first = 0;
        long n = kl_test_sockread(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return total;
}

typedef struct {
    KlHttpBodyReader base;
    char buf[16384];
    size_t len;
} DecodeReader;

static int decode_on_data(KlHttpBodyReader *base, const char *data, size_t len) {
    DecodeReader *reader = (DecodeReader *)base;
    if (len > sizeof(reader->buf) - reader->len) return -1;
    memcpy(reader->buf + reader->len, data, len);
    reader->len += len;
    return 0;
}

static void decode_noop(KlHttpBodyReader *base) { (void)base; }

static DecodeReader decode_reader(void) {
    DecodeReader reader;
    memset(&reader, 0, sizeof(reader));
    reader.base.on_data = decode_on_data;
    reader.base.on_complete = decode_noop;
    reader.base.on_error = decode_noop;
    reader.base.destroy = decode_noop;
    return reader;
}

static int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Count non-terminal chunks in a complete, extension-free chunked body. */
static int chunk_count(const char *body, size_t len) {
    size_t pos = 0;
    int count = 0;
    while (pos < len) {
        size_t chunk_len = 0;
        int digits = 0;
        while (pos < len && body[pos] != '\r') {
            int digit = hex_digit(body[pos++]);
            if (digit < 0 || chunk_len > (SIZE_MAX - (size_t)digit) / 16) return -1;
            chunk_len = chunk_len * 16 + (size_t)digit;
            digits++;
        }
        if (digits == 0 || pos + 2 > len || body[pos] != '\r' || body[pos + 1] != '\n')
            return -1;
        pos += 2;
        if (chunk_len == 0)
            return (pos + 2 <= len && body[pos] == '\r' && body[pos + 1] == '\n') ? count : -1;
        if (chunk_len > len - pos || len - pos - chunk_len < 2) return -1;
        pos += chunk_len;
        if (body[pos] != '\r' || body[pos + 1] != '\n') return -1;
        pos += 2;
        count++;
    }
    return -1;
}

static const char *chunk_body(char *wire, size_t wire_len, size_t *body_len) {
    char *body = strstr(wire, "\r\n\r\n");
    if (!body) return NULL;
    body += 4;
    *body_len = wire_len - (size_t)(body - wire);
    return body;
}

static int decoded_body_matches(const char *chunked, size_t chunked_len,
                                const char *expected) {
    KlHttp1ChunkedDecoder decoder;
    DecodeReader reader = decode_reader();
    kl_http1_chunked_init(&decoder);
    if (kl_http1_chunked_decode(&decoder, chunked, chunked_len, &reader.base) != 1)
        return 0;
    return reader.len == strlen(expected) && memcmp(reader.buf, expected, reader.len) == 0;
}

UTEST(sse, built_in_writer_batches_each_small_event_into_one_chunk) {
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &alloc);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    res.conn_fd = pfd[1];

    KlHttpSse sse;
    ASSERT_EQ(kl_http_sse_begin(&res, &sse), 0);
    ASSERT_EQ(kl_http_sse_event(&sse, "tick", "a\r\nb\rc\n", 7, "7"), 0);
    ASSERT_EQ(kl_http_sse_event(&sse, NULL, NULL, 0, NULL), 0);
    ASSERT_EQ(kl_http_sse_end(&sse), 0);
    kl_test_closesock(pfd[1]);

    char wire[16384];
    size_t wire_len = read_all_now(pfd[0], wire, sizeof(wire));
    kl_test_closesock(pfd[0]);
    kl_http_response_free(&res);

    size_t body_len = 0;
    const char *body = chunk_body(wire, wire_len, &body_len);
    ASSERT_TRUE(body != NULL);
    ASSERT_EQ(chunk_count(body, body_len), 2);
    ASSERT_TRUE(decoded_body_matches(
        body, body_len, "event: tick\nid: 7\ndata: a\ndata: b\ndata: c\n\ndata: \n\n"));
}

UTEST(sse, built_in_writer_flushes_events_and_comments_across_iovec_limit) {
    const char *lines = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11";
    const char *event_body =
        "data: 0\ndata: 1\ndata: 2\ndata: 3\ndata: 4\ndata: 5\n"
        "data: 6\ndata: 7\ndata: 8\ndata: 9\ndata: 10\ndata: 11\n\n";
    const char *comment_body =
        ": 0\n: 1\n: 2\n: 3\n: 4\n: 5\n: 6\n: 7\n: 8\n: 9\n: 10\n: 11\n";
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &alloc);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    res.conn_fd = pfd[1];

    KlHttpSse sse;
    ASSERT_EQ(kl_http_sse_begin(&res, &sse), 0);
    ASSERT_EQ(kl_http_sse_event(&sse, NULL, lines, strlen(lines), NULL), 0);
    ASSERT_EQ(kl_http_sse_comment(&sse, lines, strlen(lines)), 0);
    ASSERT_EQ(kl_http_sse_end(&sse), 0);
    kl_test_closesock(pfd[1]);

    char wire[16384];
    size_t wire_len = read_all_now(pfd[0], wire, sizeof(wire));
    kl_test_closesock(pfd[0]);
    kl_http_response_free(&res);

    size_t body_len = 0;
    const char *body = chunk_body(wire, wire_len, &body_len);
    ASSERT_TRUE(body != NULL);
    ASSERT_EQ(chunk_count(body, body_len), 4);
    char expected[1024];
    size_t event_len = strlen(event_body);
    size_t comment_len = strlen(comment_body);
    ASSERT_TRUE(event_len + comment_len < sizeof(expected));
    memcpy(expected, event_body, event_len);
    memcpy(expected + event_len, comment_body, comment_len + 1);
    ASSERT_TRUE(decoded_body_matches(body, body_len, expected));
}

UTEST(sse, built_in_writer_batches_without_a_drain) {
    const char *lines = "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11";
    const char *expected =
        "data: 0\ndata: 1\ndata: 2\ndata: 3\ndata: 4\ndata: 5\n"
        "data: 6\ndata: 7\ndata: 8\ndata: 9\ndata: 10\ndata: 11\n\n";
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &alloc);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    res.conn_fd = pfd[1];

    /* A response without an allocator uses the direct writev stream path. Restore it after begin
     * so response-owned header storage is still released normally. */
    res.alloc = NULL;
    KlHttpSse sse;
    ASSERT_EQ(kl_http_sse_begin(&res, &sse), 0);
    res.alloc = &alloc;
    ASSERT_EQ(res.drain_enabled, 0);
    ASSERT_EQ(kl_http_sse_event(&sse, NULL, lines, strlen(lines), NULL), 0);
    ASSERT_EQ(kl_http_sse_end(&sse), 0);
    kl_test_closesock(pfd[1]);

    char wire[16384];
    size_t wire_len = read_all_now(pfd[0], wire, sizeof(wire));
    kl_test_closesock(pfd[0]);
    kl_http_response_free(&res);

    size_t body_len = 0;
    const char *body = chunk_body(wire, wire_len, &body_len);
    ASSERT_TRUE(body != NULL);
    ASSERT_EQ(chunk_count(body, body_len), 2);
    ASSERT_TRUE(decoded_body_matches(body, body_len, expected));
}

UTEST(sse, built_in_writer_suppresses_head_body) {
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &alloc);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    res.conn_fd = pfd[1];
    res.head_request = 1;

    KlHttpSse sse;
    ASSERT_EQ(kl_http_sse_begin(&res, &sse), 0);
    ASSERT_EQ(kl_http_sse_event(&sse, "tick", "data", 4, "1"), 0);
    ASSERT_EQ(kl_http_sse_end(&sse), 0);
    kl_test_closesock(pfd[1]);

    char wire[4096];
    size_t wire_len = read_all_now(pfd[0], wire, sizeof(wire));
    kl_test_closesock(pfd[0]);
    kl_http_response_free(&res);

    size_t body_len = 1;
    ASSERT_TRUE(chunk_body(wire, wire_len, &body_len) != NULL);
    ASSERT_EQ(body_len, (size_t)0);
}

UTEST(sse, built_in_writer_rejects_write_after_stream_error) {
    KlAllocator alloc = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &alloc);
    int pfd[2];
    ASSERT_EQ(kl_test_socketpair(pfd), 0);
    res.conn_fd = pfd[1];

    KlHttpSse sse;
    ASSERT_EQ(kl_http_sse_begin(&res, &sse), 0);
    res.stream_error = 1;
    ASSERT_EQ(kl_http_sse_event(&sse, NULL, "data", 4, NULL), -1);
    kl_test_closesock(pfd[1]);

    char wire[4096];
    size_t wire_len = read_all_now(pfd[0], wire, sizeof(wire));
    kl_test_closesock(pfd[0]);
    kl_http_response_free(&res);

    size_t body_len = 1;
    ASSERT_TRUE(chunk_body(wire, wire_len, &body_len) != NULL);
    ASSERT_EQ(body_len, (size_t)0);
}

UTEST_MAIN();
