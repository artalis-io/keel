#include "utest.h"
#include <keel/response.h>
#include <unistd.h>
#include <string.h>

UTEST(response, init_and_free) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    ASSERT_TRUE(res.hdr_buf != NULL);
    ASSERT_EQ(res.status, 200);
    kl_response_free(&res);
}

UTEST(response, set_status) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    kl_response_status(&res, 404);
    ASSERT_EQ(res.status, 404);
    kl_response_free(&res);
}

UTEST(response, add_headers) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    kl_response_header(&res, "Content-Type", "text/plain");
    ASSERT_TRUE(res.hdr_len > 0);
    ASSERT_TRUE(strstr(res.hdr_buf, "Content-Type: text/plain\r\n") != NULL);
    kl_response_free(&res);
}

UTEST(response, body_buffer) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    kl_response_body(&res, "hello", 5);
    ASSERT_EQ(res.body_mode, KL_BODY_BUFFER);
    ASSERT_EQ(res.body_len, (size_t)5);
    ASSERT_EQ(memcmp(res.body, "hello", 5), 0);
    kl_response_free(&res);
}

UTEST(response, json_convenience) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    kl_response_json(&res, 200, "{\"ok\":true}", 11);
    ASSERT_EQ(res.status, 200);
    ASSERT_EQ(res.body_mode, KL_BODY_BUFFER);
    ASSERT_TRUE(strstr(res.hdr_buf, "application/json") != NULL);
    kl_response_free(&res);
}

UTEST(response, error_convenience) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    kl_response_error(&res, 500, "Internal Server Error");
    ASSERT_EQ(res.status, 500);
    ASSERT_EQ(res.body_mode, KL_BODY_BUFFER);
    kl_response_free(&res);
}

UTEST(response, send_to_pipe) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    res.conn_fd = pipefd[1];
    kl_response_json(&res, 200, "{\"ok\":true}", 11);

    int r = kl_response_send(&res);
    ASSERT_EQ(r, 0);
    close(pipefd[1]);

    char buf[1024];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    close(pipefd[0]);

    /* Verify HTTP response format */
    ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Type: application/json\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Length: 11\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"ok\":true}") != NULL);

    kl_response_free(&res);
}

UTEST(response, head_suppresses_body) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    res.conn_fd = pipefd[1];
    res.head_request = 1;
    kl_response_json(&res, 200, "{\"ok\":true}", 11);

    int r = kl_response_send(&res);
    ASSERT_EQ(r, 0);
    close(pipefd[1]);

    char buf[1024];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    close(pipefd[0]);

    /* Headers should be present including Content-Length */
    ASSERT_TRUE(strstr(buf, "HTTP/1.1 200 OK\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "Content-Length: 11\r\n") != NULL);

    /* Body should NOT be present */
    char *body_start = strstr(buf, "\r\n\r\n");
    ASSERT_TRUE(body_start != NULL);
    body_start += 4;
    ASSERT_EQ((size_t)(n - (body_start - buf)), (size_t)0);

    kl_response_free(&res);
}

UTEST(response, keep_alive_conditional) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    res.conn_fd = pipefd[1];
    res.keep_alive = 0;
    kl_response_body(&res, "hi", 2);

    int r = kl_response_send(&res);
    ASSERT_EQ(r, 0);
    close(pipefd[1]);

    char buf[1024];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    close(pipefd[0]);

    /* No keep-alive header when flag is 0 */
    ASSERT_TRUE(strstr(buf, "Connection: keep-alive") == NULL);

    /* Now test with keep_alive = 1 */
    kl_response_free(&res);
    kl_response_init(&res, &a);
    ASSERT_EQ(pipe(pipefd), 0);
    res.conn_fd = pipefd[1];
    res.keep_alive = 1;
    kl_response_body(&res, "hi", 2);

    r = kl_response_send(&res);
    ASSERT_EQ(r, 0);
    close(pipefd[1]);

    n = read(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    close(pipefd[0]);

    ASSERT_TRUE(strstr(buf, "Connection: keep-alive") != NULL);

    kl_response_free(&res);
}

UTEST(response, streaming_chunked) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    res.conn_fd = pipefd[1];
    kl_response_header(&res, "Content-Type", "text/plain");

    KlWriteFn write_fn;
    void *write_ctx;
    ASSERT_EQ(kl_response_begin_stream(&res, 200, &write_fn, &write_ctx), 0);

    write_fn(write_ctx, "hello", 5);
    write_fn(write_ctx, " world", 6);

    ASSERT_EQ(kl_response_end_stream(&res), 0);
    close(pipefd[1]);

    char buf[2048];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    close(pipefd[0]);

    /* Verify chunked encoding */
    ASSERT_TRUE(strstr(buf, "Transfer-Encoding: chunked\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "5\r\nhello\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "6\r\n world\r\n") != NULL);
    ASSERT_TRUE(strstr(buf, "0\r\n\r\n") != NULL);

    kl_response_free(&res);
}

/* ── Audit coverage tests ───────────────────────────────────────────── */

UTEST(response, reset_reuses_buffer) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    kl_response_header(&res, "X-Test", "value");
    ASSERT_TRUE(res.hdr_len > 0);

    char *old_buf = res.hdr_buf;
    size_t old_cap = res.hdr_cap;
    kl_response_reset(&res);

    /* Buffer pointer and capacity preserved after reset */
    ASSERT_EQ(res.hdr_buf, old_buf);
    ASSERT_EQ(res.hdr_cap, old_cap);
    /* Header content cleared */
    ASSERT_EQ(res.hdr_len, (size_t)0);
    /* Status reset to 200 */
    ASSERT_EQ(res.status, 200);

    kl_response_free(&res);
}

UTEST(response, file_mode) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    kl_response_file(&res, 42, 1024);
    ASSERT_EQ(res.body_mode, KL_BODY_FILE);
    ASSERT_EQ(res.file_fd, 42);
    ASSERT_EQ(res.file_size, (off_t)1024);
    ASSERT_EQ(res.file_offset, (off_t)0);

    /* Prevent close(42) from hitting a real fd */
    res.file_fd = -1;
    kl_response_free(&res);
}

UTEST(response, body_null_data_ignored) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    /* NULL data with len > 0 should be silently ignored */
    kl_response_body(&res, NULL, 100);
    /* body_mode should remain unset (0) */
    ASSERT_EQ(res.body_mode, 0);

    kl_response_free(&res);
}

UTEST(response, header_injection_rejected) {
    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    /* Headers with \r\n in name or value should be silently dropped */
    kl_response_header(&res, "X-Evil\r\nInjection", "value");
    ASSERT_EQ(res.hdr_len, (size_t)0);

    kl_response_header(&res, "X-Good", "value\r\nInjection: bad");
    ASSERT_EQ(res.hdr_len, (size_t)0);

    kl_response_free(&res);
}

UTEST_MAIN();
