#include "utest.h"
#include <keel/http_sse.h>
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

UTEST_MAIN();
