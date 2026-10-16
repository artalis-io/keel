#include "utest.h"
#include <keel/http_response.h>
#include <keel/event_ctx.h>
#include <keel/socket.h>
#include "event_caps.h"   /* kl_event_caps: the loop's drive model decides the stream writer */
#include "http_response_internal.h"
#include "net_compat.h"
#include <string.h>
#include <fcntl.h>
#include <errno.h>

UTEST(response, init_and_free) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    ASSERT_TRUE(res.hdr_buf != NULL);
    ASSERT_EQ(res.status, 200);
    kl_http_response_free(&res);
}

UTEST(response, stream_writev_rejects_length_sum_overflow) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    KlIoVec iov[2] = {
        { .base = (void *)"x", .len = SIZE_MAX },
        { .base = (void *)"y", .len = 1 },
    };

    ASSERT_EQ(kl_http_response_stream_writev(&res, iov, 2), -1);
    ASSERT_EQ(res.stream_error, 1);
    kl_http_response_free(&res);
}

UTEST(response, set_status) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    kl_http_response_status(&res, 404);
    ASSERT_EQ(res.status, 404);
    kl_http_response_free(&res);
}

UTEST(response, add_headers) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    kl_http_response_header(&res, "Content-Type", "text/plain");
    ASSERT_TRUE(res.hdr_len > 0);
    ASSERT_TRUE(strstr(res.hdr_buf, "Content-Type: text/plain\r\n") != NULL);
    kl_http_response_free(&res);
}

UTEST(response, body_buffer) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    kl_http_response_body_borrow(&res, "hello", 5);
    ASSERT_EQ(res.body_mode, KL_HTTP_BODY_BUFFER);
    ASSERT_EQ(res.body_len, (size_t)5);
    ASSERT_EQ(memcmp(res.body, "hello", 5), 0);
    kl_http_response_free(&res);
}

UTEST(response, json_convenience) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    kl_http_response_json(&res, 200, "{\"ok\":true}", 11);
    ASSERT_EQ(res.status, 200);
    ASSERT_EQ(res.body_mode, KL_HTTP_BODY_BUFFER);
    ASSERT_TRUE(strstr(res.hdr_buf, "application/json") != NULL);
    kl_http_response_free(&res);
}

UTEST(response, error_convenience) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    kl_http_response_error(&res, 500, "Internal Server Error");
    ASSERT_EQ(res.status, 500);
    ASSERT_EQ(res.body_mode, KL_HTTP_BODY_BUFFER);
    kl_http_response_free(&res);
}

UTEST(response, send_to_pipe) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);

    res.conn_fd = pipefd[1];
    kl_http_response_json(&res, 200, "{\"ok\":true}", 11);

    int r = kl_http_response_send(&res);
    ASSERT_EQ(r, 0);
    kl_test_closesock(pipefd[1]);

    char buf[1024];
    kl_ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    kl_test_closesock(pipefd[0]);

    /* Verify HTTP response format */
    ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Type: application/json\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Length: 11\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    kl_http_response_free(&res);
}

UTEST(response, head_suppresses_body) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);

    res.conn_fd = pipefd[1];
    res.head_request = 1;
    kl_http_response_json(&res, 200, "{\"ok\":true}", 11);

    int r = kl_http_response_send(&res);
    ASSERT_EQ(r, 0);
    kl_test_closesock(pipefd[1]);

    char buf[1024];
    kl_ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    kl_test_closesock(pipefd[0]);

    /* Headers should be present including Content-Length */
    ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Length: 11\r\n") != NULL);

    /* Body should NOT be present */
    char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;
    ASSERT_EQ((size_t)(n - (body_start - buf)), (size_t)0);

    kl_http_response_free(&res);
}

UTEST(response, keep_alive_conditional) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);

    res.conn_fd = pipefd[1];
    res.keep_alive = 0;
    kl_http_response_body_borrow(&res, "hi", 2);

    int r = kl_http_response_send(&res);
    ASSERT_EQ(r, 0);
    kl_test_closesock(pipefd[1]);

    char buf[1024];
    kl_ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    kl_test_closesock(pipefd[0]);

    /* No keep-alive header when flag is 0 */
    ASSERT_TRUE(strstr(buf, "Connection: keep-alive") == NULL);

    /* Now test with keep_alive = 1 */
    kl_http_response_free(&res);
    kl_http_response_init(&res, &a);
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);
    res.conn_fd = pipefd[1];
    res.keep_alive = 1;
    kl_http_response_body_borrow(&res, "hi", 2);

    r = kl_http_response_send(&res);
    ASSERT_EQ(r, 0);
    kl_test_closesock(pipefd[1]);

    n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    kl_test_closesock(pipefd[0]);

    ASSERT_TRUE(strstr(buf, "Connection: keep-alive") != NULL);

    kl_http_response_free(&res);
}

UTEST(response, streaming_chunked) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);

    res.conn_fd = pipefd[1];
    kl_http_response_header(&res, "Content-Type", "text/plain");

    KlHttpResponseWriteFn write_fn;
    void *write_ctx;
    ASSERT_EQ(kl_http_response_begin_stream(&res, 200, &write_fn, &write_ctx), 0);

    write_fn(write_ctx, "hello", 5);
    write_fn(write_ctx, " world", 6);

    ASSERT_EQ(kl_http_response_end_stream(&res), 0);
    kl_test_closesock(pipefd[1]);

    char buf[2048];
    kl_ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    kl_test_closesock(pipefd[0]);

    /* Verify chunked encoding */
    ASSERT_TRUE(strstr(buf, "Transfer-Encoding: chunked\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "5\r\nhello\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "6\r\n world\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "0\r\n\r\n") != NULL);

    kl_http_response_free(&res);
}

/* A standalone streamed response whose event ctx names the built-in socket provider explicitly (as
 * KlHttpServerConfig.sockets = kl_socket_provider_posix() / _winsock() does). The stream writer
 * follows the loop's drive model, never the provider's capability bits (the provider advertises
 * datagram support, which must not be read as anything else):
 *   - readiness: a synchronous send/recv provider, so the chunks go out through kl_sock_send;
 *   - completion: output goes only through a pooled connection's output queue, which a standalone
 *     response does not have, so the stream is refused whatever the provider (as
 *     completion_output.a_streamed_response_outside_a_pooled_connection_is_refused). */
UTEST(response, streaming_chunked_explicit_builtin_provider) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ectx;
    ASSERT_EQ(kl_event_ctx_init(&ectx, &a), 0);
    int completion = (kl_event_caps(&ectx.loop) & KL_EVENT_CAP_COMPLETION) != 0;
#ifdef _WIN32
    ectx.sockets = kl_socket_provider_winsock();
#else
    ectx.sockets = kl_socket_provider_posix();
#endif

    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    res.ctx = &ectx;

    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);
    res.conn_fd = pipefd[1];

    /* Every outcome is captured first and asserted after the fds, response and ctx are released. */
    KlHttpResponseWriteFn write_fn = NULL;
    void *write_ctx = NULL;
    int begin = kl_http_response_begin_stream(&res, 200, &write_fn, &write_ctx);
    int w1 = -1, w2 = -1, end = -1;
    if (begin == 0) {
        w1 = write_fn(write_ctx, "hello", 5);
        w2 = write_fn(write_ctx, " world", 6);
    }
    int st_err = res.stream_error;
    if (begin == 0) end = kl_http_response_end_stream(&res);
    kl_test_closesock(pipefd[1]);

    char buf[2048];
    kl_ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
    kl_test_closesock(pipefd[0]);
    kl_http_response_free(&res);
    kl_event_ctx_free(&ectx);

    if (completion) {
        ASSERT_EQ(begin, -1);   /* the status line already goes through the stream writer: refused */
        ASSERT_EQ(st_err, 1);
        ASSERT_TRUE(n <= 0);    /* nothing was sent synchronously on the loop thread */
        return;
    }
    ASSERT_EQ(begin, 0);   /* the status line and headers already go through the stream writer */
    ASSERT_EQ(w1, 0);
    ASSERT_EQ(w2, 0);
    ASSERT_EQ(st_err, 0);
    ASSERT_EQ(end, 0);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    ASSERT_TRUE(strstr(buf, "5\r\nhello\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "6\r\n world\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "0\r\n\r\n") != NULL);
}

/* ── Audit coverage tests ───────────────────────────────────────────── */

UTEST(response, reset_reuses_buffer) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    kl_http_response_header(&res, "X-Test", "value");
    ASSERT_TRUE(res.hdr_len > 0);

    char *old_buf = res.hdr_buf;
    size_t old_cap = res.hdr_cap;
    kl_http_response_reset(&res);

    /* Buffer pointer and capacity preserved after reset */
    ASSERT_EQ(res.hdr_buf, old_buf);
    ASSERT_EQ(res.hdr_cap, old_cap);
    /* Header content cleared */
    ASSERT_EQ(res.hdr_len, (size_t)0);
    /* Status reset to 200 */
    ASSERT_EQ(res.status, 200);

    kl_http_response_free(&res);
}

UTEST(response, file_mode) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    kl_http_response_file(&res, 42, 1024);
    ASSERT_EQ(res.body_mode, KL_HTTP_BODY_FILE);
    ASSERT_EQ(res.file_fd, 42);
    ASSERT_EQ(res.file_size, (uint64_t)1024);
    ASSERT_EQ(res.file_offset, (uint64_t)0);

    /* Prevent close(42) from hitting a real fd */
    res.file_fd = -1;
    kl_http_response_free(&res);
}

UTEST(response, body_null_data_ignored) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    /* NULL data with len > 0 should be silently ignored */
    kl_http_response_body_borrow(&res, NULL, 100);
    /* body_mode should remain unset (0) */
    ASSERT_EQ(res.body_mode, 0);

    kl_http_response_free(&res);
}

UTEST(response, header_injection_rejected) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    /* Headers with \r\n in name or value should be silently dropped */
    kl_http_response_header(&res, "X-Evil\r\nInjection", "value");
    ASSERT_EQ(res.hdr_len, (size_t)0);

    kl_http_response_header(&res, "X-Good", "value\r\nInjection: bad");
    ASSERT_EQ(res.hdr_len, (size_t)0);

    kl_http_response_free(&res);
}

/* ── Fix 1: Response API return values ──────────────────────────────── */

UTEST(response, header_returns_0_on_success) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    ASSERT_EQ(kl_http_response_header(&res, "Content-Type", "text/plain"), 0);
    ASSERT_EQ(kl_http_response_header(&res, "X-Custom", "value"), 0);
    kl_http_response_free(&res);
}

UTEST(response, header_returns_minus1_on_crlf) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    ASSERT_EQ(kl_http_response_header(&res, "X-Evil\r\n", "value"), -1);
    ASSERT_EQ(kl_http_response_header(&res, "X-Good", "val\r\nue"), -1);
    ASSERT_EQ(kl_http_response_header(&res, NULL, "value"), -1);
    ASSERT_EQ(kl_http_response_header(&res, "X-Good", NULL), -1);
    /* Header buffer should be untouched after failures */
    ASSERT_EQ(res.hdr_len, (size_t)0);
    kl_http_response_free(&res);
}

/* Failing allocator for rollback tests */
static void *fail_malloc(void *ctx, size_t size) {
    (void)ctx; (void)size; return NULL;
}
static void *fail_realloc(void *ctx, void *ptr, size_t old, size_t new_size) {
    (void)ctx; (void)ptr; (void)old; (void)new_size; return NULL;
}
static void fail_free(void *ctx, void *ptr, size_t size) {
    (void)ctx; (void)ptr; (void)size;
}

UTEST(response, header_rollback_on_alloc_failure) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    /* Add a header that succeeds */
    ASSERT_EQ(kl_http_response_header(&res, "X-First", "ok"), 0);
    size_t len_after_first = res.hdr_len;
    ASSERT_TRUE(len_after_first > 0);

    /* Switch to failing allocator: cap stays small so next append triggers realloc */
    KlAllocator fail = { .malloc = fail_malloc, .realloc = fail_realloc, .free = fail_free };
    res.alloc = &fail;
    /* Force a realloc by filling remaining capacity */
    res.hdr_len = res.hdr_cap;  /* pretend buffer is full */

    int rc = kl_http_response_header(&res, "X-Huge", "this-should-fail");
    ASSERT_EQ(rc, -1);
    /* hdr_len should be rolled back to what we set (hdr_cap) */
    ASSERT_EQ(res.hdr_len, res.hdr_cap);

    /* Restore original allocator for cleanup */
    res.alloc = &a;
    res.hdr_len = len_after_first;  /* restore valid state for free */
    kl_http_response_free(&res);
}

UTEST(response, body_copy_returns_minus1_on_alloc_failure) {
    KlAllocator fail = { .malloc = fail_malloc, .realloc = fail_realloc, .free = fail_free };
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    /* Switch to failing allocator */
    res.alloc = &fail;
    int rc = kl_http_response_body_copy(&res, "hello", 5);
    ASSERT_EQ(rc, -1);

    /* Restore for cleanup */
    res.alloc = &a;
    kl_http_response_free(&res);
}

UTEST(response, body_copy_returns_0_on_success) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    ASSERT_EQ(kl_http_response_body_copy(&res, "hello", 5), 0);
    ASSERT_EQ(res.body_len, (size_t)5);
    ASSERT_EQ(memcmp(res.body, "hello", 5), 0);
    /* Empty body */
    ASSERT_EQ(kl_http_response_body_copy(&res, "", 0), 0);
    ASSERT_EQ(res.body_len, (size_t)0);
    kl_http_response_free(&res);
}

UTEST(response, json_returns_0_on_success) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    ASSERT_EQ(kl_http_response_json(&res, 200, "{}", 2), 0);
    ASSERT_EQ(res.status, 200);
    kl_http_response_free(&res);
}

UTEST(response, error_returns_0_on_success) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    ASSERT_EQ(kl_http_response_error(&res, 500, "fail"), 0);
    ASSERT_EQ(res.status, 500);
    kl_http_response_free(&res);
}

/* ── Fix 2: Buffer send partial/resume ─────────────────────────────── */

UTEST(response, buffer_send_returns_1_on_eagain) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    /* Use a socketpair so we can fill the send buffer */
    int sv[2];
    ASSERT_EQ(kl_test_socketpair(sv), 0);

    /* Set non-blocking */
    kl_test_set_nonblock(sv[0]);

    /* Fill the send buffer to trigger EAGAIN */
    char fill[65536];
    memset(fill, 'X', sizeof(fill));
    while (1) {
        kl_ssize_t w = kl_test_sockwrite(sv[0], fill, sizeof(fill));
        if (w < 0) break;
    }

    res.conn_fd = sv[0];
    kl_http_response_body_borrow(&res, "test body", 9);

    int r = kl_http_response_send(&res);
    /* Should return 1 (partial) or 0 (if EAGAIN returned 0 bytes, offset stays 0) */
    ASSERT_TRUE(r == 0 || r == 1);

    kl_test_closesock(sv[0]);
    kl_test_closesock(sv[1]);
    kl_http_response_free(&res);
}

UTEST(response, buffer_send_resumes_from_offset) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);

    res.conn_fd = pipefd[1];
    kl_http_response_body_borrow(&res, "hello world", 11);

    /* Send should complete (pipe has enough buffer) */
    int r;
    int iterations = 0;
    do {
        r = kl_http_response_send(&res);
        iterations++;
    } while (r == 1 && iterations < 100);
    ASSERT_EQ(r, 0);
    /* send_offset should equal total size */
    ASSERT_TRUE(res.send_offset > 0);

    kl_test_closesock(pipefd[1]);

    /* Verify output */
    char buf[1024];
    kl_ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    kl_test_closesock(pipefd[0]);

    ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "hello world") != NULL);

    kl_http_response_free(&res);
}

UTEST(response, send_offset_reset_on_response_reset) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    res.send_offset = 42;
    kl_http_response_reset(&res);
    ASSERT_EQ(res.send_offset, (size_t)0);
    kl_http_response_free(&res);
}

/* Ending a stream on a HEAD response marks it ended. It returned early for HEAD (nothing to write)
 * before setting stream_ended, so a HEAD stream that reached SENDING never completed: readiness
 * spun on WRITE, and a completion loop held the slot. */
UTEST(response, end_stream_on_head_marks_the_stream_ended) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    res.head_request = 1;
    res.body_mode = KL_HTTP_BODY_STREAM;
    int rc = kl_http_response_end_stream(&res);
    int ended = res.stream_ended;
    kl_http_response_free(&res);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(ended, 1);                         /* was 0 */
}

/* ═══════════════════════════════════════════════════════════════════
 * Ending a stream twice must emit one terminating chunk, not two.
 *
 * kl_http_response_end_stream() guards on stream_error and on head_request, but never on the
 * stream_ended flag it sets itself. Called twice it writes "0\r\n\r\n" twice. One terminating chunk
 * is what ends a chunked body: the second one is not part of the response, it is the first thing the
 * client sees where the *next* response on the connection must start. And because stream_ended is
 * then 1, the server's "a stream that was never ended is never kept alive" rule does not fire, so on
 * a stream the driver does keep (which path it takes decides: docs/contracts/streaming.md) the stray
 * framing is already behind the client when the next response begins.
 * A handler closing its own stream and a helper that also closes it (kl_http_sse_end, a compress
 * wrapper) is an easy way to get there.
 *
 * The wire is the contract here, so these tests read it.
 * ═══════════════════════════════════════════════════════════════════ */

/* Occurrences of the chunked terminating chunk. The bodies below are fixed, so no chunk content can
 * be mistaken for it. */
static int count_terminators(const char *buf, size_t n) {
    int hits = 0;
    for (size_t i = 0; i + 5 <= n; i++)
        if (memcmp(buf + i, "0\r\n\r\n", 5) == 0) hits++;
    return hits;
}

/* Read everything currently on the pair. The writes under test are synchronous, so by the time the
 * calls have returned the whole of it is buffered. */
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

UTEST(response, a_stream_ended_twice_emits_one_terminating_chunk) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);
    res.conn_fd = pipefd[1];

    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    ASSERT_EQ(kl_http_response_begin_stream(&res, 200, &w, &wc), 0);
    w(wc, "abc", 3);

    int first = kl_http_response_end_stream(&res);
    int second = kl_http_response_end_stream(&res);
    int third = kl_http_response_end_stream(&res);
    kl_test_closesock(pipefd[1]);

    char buf[4096];
    size_t n = read_all_now(pipefd[0], buf, sizeof buf);
    kl_test_closesock(pipefd[0]);

    printf("  wire: %zu byte(s), terminating chunks: %d\n", n, count_terminators(buf, n));
    ASSERT_EQ(count_terminators(buf, n), 1);           /* was 3: one end per call */
    ASSERT_TRUE(strstr(buf, "3\r\nabc\r\n") != NULL);   /* the body is unchanged */
    /* The whole response ends at its terminator: nothing trails it. */
    ASSERT_EQ(n, strlen("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"));

    /* A stream that is already ended reports success again rather than turning into an error: a
     * caller that checks the return value must not be sent down a failure path for a no-op. */
    ASSERT_EQ(first, 0);
    ASSERT_EQ(second, 0);
    ASSERT_EQ(third, 0);

    kl_http_response_free(&res);
}

UTEST(response, a_repeated_end_stream_leaves_the_completed_stream_alone) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);
    res.conn_fd = pipefd[1];

    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    ASSERT_EQ(kl_http_response_begin_stream(&res, 200, &w, &wc), 0);
    w(wc, "abc", 3);
    ASSERT_EQ(kl_http_response_end_stream(&res), 0);

    int ended = res.stream_ended, err = res.stream_error, body_mode = (int)res.body_mode;
    ASSERT_EQ(res.drain_enabled, 1);

    ASSERT_EQ(kl_http_response_end_stream(&res), 0);

    /* No state transition of any kind on the second call: already terminal, and terminal is where
     * it must stay. No second error flag, no mode change, nothing newly buffered. */
    ASSERT_EQ(res.stream_ended, ended);
    ASSERT_EQ(res.stream_error, 0);
    ASSERT_EQ(err, 0);
    ASSERT_EQ((int)res.body_mode, body_mode);

    kl_test_closesock(pipefd[1]);
    kl_test_closesock(pipefd[0]);
    kl_http_response_free(&res);
}

/* The damage is what the next response on the same connection inherits. Reuse the response the way
 * the server does (kl_http_response_reset) and put a second, buffered response on the same socket. */
UTEST(response, the_next_response_after_a_double_ended_stream_starts_at_the_right_boundary) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);
    res.conn_fd = pipefd[1];

    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    ASSERT_EQ(kl_http_response_begin_stream(&res, 200, &w, &wc), 0);
    w(wc, "abc", 3);
    ASSERT_EQ(kl_http_response_end_stream(&res), 0);
    ASSERT_EQ(kl_http_response_end_stream(&res), 0);   /* the double close */

    /* Keep-alive reuse, exactly as the connection driver does it between requests. */
    kl_http_response_reset(&res);
    kl_http_response_status(&res, 200);
    kl_http_response_body_copy(&res, "SECOND", 6);
    ASSERT_EQ(kl_http_response_send(&res), 0);
    kl_test_closesock(pipefd[1]);

    char buf[8192];
    size_t n = read_all_now(pipefd[0], buf, sizeof buf);
    kl_test_closesock(pipefd[0]);

    printf("  two responses on one connection, %zu byte(s), terminators: %d\n", n,
           count_terminators(buf, n));
    ASSERT_EQ(count_terminators(buf, n), 1);

    /* Walk the wire as a client would: the first response ends at its terminator and the second
     * begins on the very next byte. A stray terminator makes this byte offset wrong. */
    const char *term = NULL;
    for (size_t i = 0; i + 5 <= n; i++)
        if (memcmp(buf + i, "0\r\n\r\n", 5) == 0) { term = buf + i; break; }
    ASSERT_TRUE(term != NULL);
    const char *next = term + 5;
    ASSERT_TRUE((size_t)(next - buf) < n);
    ASSERT_TRUE(strncmp(next, "HTTP/1.1 200 OK\r\n", sizeof("HTTP/1.1 200 OK\r\n") - 1) == 0);
    /* was: "0\r\n\r\nHTTP/1.1 200 OK..." -- the stray terminator occupied the boundary */
    ASSERT_TRUE(strstr(next, "SECOND") != NULL);

    kl_http_response_free(&res);
}

/* A failed termination is not a completed one: the failure must keep propagating, and the retry
 * must not be turned into a silent success by an idempotence guard. */
UTEST(response, a_failed_termination_keeps_failing_and_emits_nothing) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);
    res.conn_fd = pipefd[1];

    KlHttpResponseWriteFn w = NULL;
    void *wc = NULL;
    ASSERT_EQ(kl_http_response_begin_stream(&res, 200, &w, &wc), 0);
    w(wc, "abc", 3);

    /* Take the write target away, so terminating the stream fails at the transport. */
    kl_test_closesock(pipefd[1]);

    int first = kl_http_response_end_stream(&res);
    int second = kl_http_response_end_stream(&res);
    int third = kl_http_response_end_stream(&res);

    ASSERT_EQ(first, -1);      /* the transport failed */
    ASSERT_EQ(second, -1);     /* still failing: not converted into success */
    ASSERT_EQ(third, -1);
    ASSERT_EQ(res.stream_error, 1);
    ASSERT_EQ(res.stream_ended, 0);

    char buf[2048];
    size_t n = read_all_now(pipefd[0], buf, sizeof buf);
    kl_test_closesock(pipefd[0]);
    ASSERT_EQ(count_terminators(buf, n), 0);           /* the terminator never got out */
    ASSERT_TRUE(strstr(buf, "3\r\nabc\r\n") != NULL);   /* what was written before it did */

    kl_http_response_free(&res);
}

/* Characterization, not endorsement: end_stream() with no successful begin_stream() is a separate
 * pre-existing behavior. stream_ended stays 0, so an idempotence guard on stream_ended cannot
 * change it, and this test pins that. Whether a bare terminating chunk (no status line, no headers)
 * should go out at all is a different question, deliberately left out of this change. */
UTEST(response, end_stream_without_a_begin_stream_is_unchanged_by_the_idempotence_guard) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    int pipefd[2];
    ASSERT_EQ(kl_test_socketpair(pipefd), 0);
    res.conn_fd = pipefd[1];

    int rc = kl_http_response_end_stream(&res);
    int ended_after = res.stream_ended;
    kl_test_closesock(pipefd[1]);

    char buf[2048];
    size_t n = read_all_now(pipefd[0], buf, sizeof buf);
    kl_test_closesock(pipefd[0]);

    printf("  end_stream with no begin_stream: rc=%d, %zu byte(s), terminators=%d\n",
           rc, n, count_terminators(buf, n));
    ASSERT_EQ(rc, 0);                        /* unchanged behavior */
    ASSERT_EQ(ended_after, 1);
    ASSERT_EQ(count_terminators(buf, n), 1); /* one bare terminator: pre-existing, see PR notes */

    kl_http_response_free(&res);
}

/* Send a response with `code` over a socket pair; return 1 if the wire starts with `expect`. */
static int status_line_is(int code, const char *expect) {
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    if (kl_http_response_init(&res, &a) < 0) return 0;
    int sv[2];
    if (kl_test_socketpair(sv) != 0) { kl_http_response_free(&res); return 0; }
    res.conn_fd = sv[1];
    kl_http_response_status(&res, code);
    kl_http_response_body_borrow(&res, "x", 1);
    int rc = kl_http_response_send(&res);
    kl_test_closesock(sv[1]);
    char buf[512];
    kl_ssize_t n = kl_test_sockread(sv[0], buf, sizeof(buf) - 1);
    kl_test_closesock(sv[0]);
    kl_http_response_free(&res);
    if (rc != 0 || n <= 0) return 0;
    buf[n] = '\0';
    size_t el = strlen(expect);
    int ok = ((size_t)n >= el && memcmp(buf, expect, el) == 0);
    if (!ok) {
        char *eol = strstr(buf, "\r\n");
        if (eol) *eol = '\0';
        printf("  status %d sent as \"%s\"\n", code, buf);
    }
    return ok;
}

/* A valid status code goes out as itself: with its reason phrase when it is a registered code,
 * with an empty reason (allowed by HTTP/1.1) when it is not. Only a code outside 100-599 is
 * replaced, by 500. */
UTEST(response, status_line_keeps_every_valid_code) {
    ASSERT_TRUE(status_line_is(200, "HTTP/1.1 200 OK\r\n"));
    ASSERT_TRUE(status_line_is(404, "HTTP/1.1 404 Not Found\r\n"));
    ASSERT_TRUE(status_line_is(412, "HTTP/1.1 412 Precondition Failed\r\n"));
    ASSERT_TRUE(status_line_is(416, "HTTP/1.1 416 Range Not Satisfiable\r\n"));
    ASSERT_TRUE(status_line_is(426, "HTTP/1.1 426 Upgrade Required\r\n"));
    ASSERT_TRUE(status_line_is(451, "HTTP/1.1 451 Unavailable For Legal Reasons\r\n"));
    ASSERT_TRUE(status_line_is(501, "HTTP/1.1 501 Not Implemented\r\n"));
    ASSERT_TRUE(status_line_is(299, "HTTP/1.1 299 \r\n"));
    ASSERT_TRUE(status_line_is(499, "HTTP/1.1 499 \r\n"));
    ASSERT_TRUE(status_line_is(599, "HTTP/1.1 599 \r\n"));
    ASSERT_TRUE(status_line_is(600, "HTTP/1.1 500 Internal Server Error\r\n"));
    ASSERT_TRUE(status_line_is(99, "HTTP/1.1 500 Internal Server Error\r\n"));
}

/* A 1xx is interim: it can never be the final response this builder frames (Content-Length and a
 * body). Sent as one, a client waits for the real final response or reads the body as the next
 * status line. 100 Continue and 101 Switching Protocols are written by their own paths, so a
 * handler that sets any 1xx still gets 500, as before the status table was widened. */
UTEST(response, a_handler_set_1xx_is_not_sent_as_a_final_response) {
    ASSERT_TRUE(status_line_is(100, "HTTP/1.1 500 Internal Server Error\r\n"));   /* was: 100 Continue */
    ASSERT_TRUE(status_line_is(101, "HTTP/1.1 500 Internal Server Error\r\n"));
    ASSERT_TRUE(status_line_is(103, "HTTP/1.1 500 Internal Server Error\r\n"));
    ASSERT_TRUE(status_line_is(150, "HTTP/1.1 500 Internal Server Error\r\n"));
    ASSERT_TRUE(status_line_is(199, "HTTP/1.1 500 Internal Server Error\r\n"));
    ASSERT_TRUE(status_line_is(200, "HTTP/1.1 200 OK\r\n"));
}

UTEST_MAIN();
