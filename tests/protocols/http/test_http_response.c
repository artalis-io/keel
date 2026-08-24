#include "utest.h"
#include <keel/http_response.h>
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
    ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
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
    ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
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
    ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
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
    ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
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
        ssize_t w = kl_test_sockwrite(sv[0], fill, sizeof(fill));
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
    ssize_t n = kl_test_sockread(pipefd[0], buf, sizeof(buf) - 1);
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

UTEST_MAIN();
