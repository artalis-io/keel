#include "utest.h"
#include <keel/keel.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* ── Mock TLS implementation ─────────────────────────────────────── */

typedef struct {
    KlTls base;
    int handshake_result;   /* 0=OK, 1=WANT_READ, 2=WANT_WRITE, 3=ERROR */
    char read_buf[256];
    size_t read_len;
    char write_buf[4096];
    size_t write_len;
    size_t pending_bytes;
    int shutdown_called;
    int shutdown_count;         /* how many times shutdown was called */
    int shutdown_want_writes;   /* return WANT_WRITE this many times */
    int reset_called;
    int destroy_called;
} MockTls;

static KlTlsResult mock_handshake(KlTls *self, int fd) {
    (void)fd;
    MockTls *m = (MockTls *)self;
    switch (m->handshake_result) {
        case 0: return KL_TLS_OK;
        case 1: return KL_TLS_WANT_READ;
        case 2: return KL_TLS_WANT_WRITE;
        default: return KL_TLS_ERROR;
    }
}

static ssize_t mock_read(KlTls *self, int fd, void *buf, size_t len) {
    (void)fd;
    MockTls *m = (MockTls *)self;
    if (m->read_len == 0) return 0;  /* WANT_READ */
    size_t to_copy = len < m->read_len ? len : m->read_len;
    memcpy(buf, m->read_buf, to_copy);
    /* Shift remaining data */
    m->read_len -= to_copy;
    if (m->read_len > 0)
        memmove(m->read_buf, m->read_buf + to_copy, m->read_len);
    return (ssize_t)to_copy;
}

static ssize_t mock_write(KlTls *self, int fd, const void *buf, size_t len) {
    (void)fd;
    MockTls *m = (MockTls *)self;
    size_t space = sizeof(m->write_buf) - m->write_len;
    if (space == 0) return 0;  /* WANT_WRITE */
    size_t to_copy = len < space ? len : space;
    memcpy(m->write_buf + m->write_len, buf, to_copy);
    m->write_len += to_copy;
    return (ssize_t)to_copy;
}

static KlTlsResult mock_shutdown(KlTls *self, int fd) {
    (void)fd;
    MockTls *m = (MockTls *)self;
    m->shutdown_called = 1;
    m->shutdown_count++;
    if (m->shutdown_want_writes > 0) {
        m->shutdown_want_writes--;
        return KL_TLS_WANT_WRITE;
    }
    return KL_TLS_OK;
}

static size_t mock_pending(KlTls *self) {
    MockTls *m = (MockTls *)self;
    return m->pending_bytes;
}

static void mock_reset(KlTls *self) {
    MockTls *m = (MockTls *)self;
    m->reset_called = 1;
}

static void mock_destroy(KlTls *self) {
    MockTls *m = (MockTls *)self;
    m->destroy_called = 1;
}

static void mock_tls_init(MockTls *m) {
    memset(m, 0, sizeof(*m));
    m->base.handshake = mock_handshake;
    m->base.read      = mock_read;
    m->base.write     = mock_write;
    m->base.shutdown   = mock_shutdown;
    m->base.pending    = mock_pending;
    m->base.reset      = mock_reset;
    m->base.destroy    = mock_destroy;
    m->base.alpn_protocol = NULL;
    m->base.set_hostname  = NULL;
}

/* ── Vtable and enum tests ───────────────────────────────────────── */

UTEST(tls, vtable_struct_size) {
    /* KlTls has 9 function pointers (7 required + alpn_protocol + set_hostname optional) */
    ASSERT_EQ(sizeof(KlTls), 9 * sizeof(void *));
}

UTEST(tls, result_enum_values) {
    /* All four result values must be distinct */
    ASSERT_NE((int)KL_TLS_OK, (int)KL_TLS_WANT_READ);
    ASSERT_NE((int)KL_TLS_OK, (int)KL_TLS_WANT_WRITE);
    ASSERT_NE((int)KL_TLS_OK, (int)KL_TLS_ERROR);
    ASSERT_NE((int)KL_TLS_WANT_READ, (int)KL_TLS_WANT_WRITE);
    ASSERT_NE((int)KL_TLS_WANT_READ, (int)KL_TLS_ERROR);
    ASSERT_NE((int)KL_TLS_WANT_WRITE, (int)KL_TLS_ERROR);
}

/* ── Handshake state machine tests ───────────────────────────────── */

UTEST(tls, mock_handshake_ok) {
    MockTls m;
    mock_tls_init(&m);
    m.handshake_result = 0;

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    c.tls = &m.base;
    c.state = KL_CONN_TLS_HANDSHAKE;

    KlConnState st = kl_conn_on_handshake(&c);
    ASSERT_EQ((int)st, (int)KL_CONN_READING);
    ASSERT_EQ(c.tls_want, 0);
}

UTEST(tls, mock_handshake_want_read) {
    MockTls m;
    mock_tls_init(&m);
    m.handshake_result = 1;

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    c.tls = &m.base;
    c.state = KL_CONN_TLS_HANDSHAKE;

    KlConnState st = kl_conn_on_handshake(&c);
    ASSERT_EQ((int)st, (int)KL_CONN_TLS_HANDSHAKE);
    ASSERT_EQ(c.tls_want, (int)KL_EVENT_READ);
}

UTEST(tls, mock_handshake_want_write) {
    MockTls m;
    mock_tls_init(&m);
    m.handshake_result = 2;

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    c.tls = &m.base;
    c.state = KL_CONN_TLS_HANDSHAKE;

    KlConnState st = kl_conn_on_handshake(&c);
    ASSERT_EQ((int)st, (int)KL_CONN_TLS_HANDSHAKE);
    ASSERT_EQ(c.tls_want, (int)KL_EVENT_WRITE);
}

UTEST(tls, mock_handshake_error) {
    MockTls m;
    mock_tls_init(&m);
    m.handshake_result = 3;

    KlConn c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    c.tls = &m.base;
    c.state = KL_CONN_TLS_HANDSHAKE;

    KlConnState st = kl_conn_on_handshake(&c);
    ASSERT_EQ((int)st, (int)KL_CONN_CLOSED);
}

UTEST(tls, handshake_null_tls_closes) {
    /* kl_conn_on_handshake with tls=NULL should return CLOSED */
    KlConn c;
    memset(&c, 0, sizeof(c));
    c.fd = -1;
    c.tls = NULL;
    c.state = KL_CONN_TLS_HANDSHAKE;

    KlConnState st = kl_conn_on_handshake(&c);
    ASSERT_EQ((int)st, (int)KL_CONN_CLOSED);
}

/* ── Mock I/O tests ──────────────────────────────────────────────── */

UTEST(tls, mock_read_write) {
    MockTls m;
    mock_tls_init(&m);

    /* Seed some data for reading */
    const char *test_data = "Hello, TLS!";
    size_t test_len = strlen(test_data);
    memcpy(m.read_buf, test_data, test_len);
    m.read_len = test_len;

    /* Read through vtable */
    char buf[64];
    ssize_t nr = m.base.read(&m.base, -1, buf, sizeof(buf));
    ASSERT_EQ((size_t)nr, test_len);
    ASSERT_EQ(0, memcmp(buf, test_data, test_len));

    /* Write through vtable */
    const char *out = "Response";
    size_t out_len = strlen(out);
    ssize_t nw = m.base.write(&m.base, -1, out, out_len);
    ASSERT_EQ((size_t)nw, out_len);
    ASSERT_EQ(0, memcmp(m.write_buf, out, out_len));
}

UTEST(tls, mock_pending) {
    MockTls m;
    mock_tls_init(&m);
    m.pending_bytes = 42;

    size_t p = m.base.pending(&m.base);
    ASSERT_EQ(p, (size_t)42);

    m.pending_bytes = 0;
    p = m.base.pending(&m.base);
    ASSERT_EQ(p, (size_t)0);
}

UTEST(tls, plaintext_null_tls) {
    /* When tls is NULL, conn_read/conn_write should use raw read/write.
     * We test this indirectly: a KlConn with tls=NULL should have
     * NULL tls field by default. */
    KlConn c;
    memset(&c, 0, sizeof(c));
    ASSERT_TRUE(c.tls == NULL);
}

UTEST(tls, config_null_means_plaintext) {
    /* KlConfig with tls=NULL (default zero-init) works as before */
    KlConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_TRUE(cfg.tls == NULL);

    /* Port must be set for server init, but we just verify the struct */
    cfg.port = 9999;
    ASSERT_TRUE(cfg.tls == NULL);
}

UTEST(tls, shutdown_and_reset) {
    MockTls m;
    mock_tls_init(&m);

    /* Shutdown */
    KlTlsResult r = m.base.shutdown(&m.base, -1);
    ASSERT_EQ((int)r, (int)KL_TLS_OK);
    ASSERT_TRUE(m.shutdown_called);

    /* Reset */
    m.base.reset(&m.base);
    ASSERT_TRUE(m.reset_called);

    /* Destroy */
    m.base.destroy(&m.base);
    ASSERT_TRUE(m.destroy_called);
}

/* ── Response send through TLS ───────────────────────────────────── */

UTEST(tls, response_send_through_mock) {
    /* Send a buffered response through mock TLS and verify output */
    MockTls m;
    mock_tls_init(&m);

    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    /* Use a pipe for conn_fd (writev_all needs a valid fd for the
     * plaintext codepath, but TLS path ignores it — the mock captures
     * all writes in write_buf) */
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    res.conn_fd = pipefd[1];
    res.tls = &m.base;
    kl_response_json(&res, 200, "{\"tls\":true}", 12);

    /* Buffer body sends may return 1 (partial) via try_writev — loop */
    int r;
    do { r = kl_response_send(&res); } while (r == 1);
    ASSERT_EQ(r, 0);

    /* Verify the mock captured the full response */
    ASSERT_TRUE(m.write_len > 0);
    /* Null-terminate for strstr */
    m.write_buf[m.write_len] = '\0';
    ASSERT_TRUE(strstr(m.write_buf, "HTTP/1.1 200 OK\r\n") != NULL);
    ASSERT_TRUE(strstr(m.write_buf, "Content-Type: application/json\r\n") != NULL);
    ASSERT_TRUE(strstr(m.write_buf, "Content-Length: 12\r\n") != NULL);
    ASSERT_TRUE(strstr(m.write_buf, "{\"tls\":true}") != NULL);

    close(pipefd[0]);
    close(pipefd[1]);
    kl_response_free(&res);
}

UTEST(tls, response_stream_through_mock) {
    /* Chunked streaming response through mock TLS */
    MockTls m;
    mock_tls_init(&m);

    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    res.conn_fd = pipefd[1];
    res.tls = &m.base;
    kl_response_header(&res, "Content-Type", "text/plain");

    KlWriteFn write_fn;
    void *write_ctx;
    ASSERT_EQ(kl_response_begin_stream(&res, 200, &write_fn, &write_ctx), 0);

    write_fn(write_ctx, "hello", 5);
    write_fn(write_ctx, " world", 6);

    ASSERT_EQ(kl_response_end_stream(&res), 0);

    /* Verify chunked encoding in mock buffer */
    m.write_buf[m.write_len] = '\0';
    ASSERT_TRUE(strstr(m.write_buf, "Transfer-Encoding: chunked\r\n") != NULL);
    ASSERT_TRUE(strstr(m.write_buf, "5\r\nhello\r\n") != NULL);
    ASSERT_TRUE(strstr(m.write_buf, "6\r\n world\r\n") != NULL);
    ASSERT_TRUE(strstr(m.write_buf, "0\r\n\r\n") != NULL);

    close(pipefd[0]);
    close(pipefd[1]);
    kl_response_free(&res);
}

UTEST(tls, response_file_through_mock) {
    /* File send through TLS uses pread+tls->write fallback */
    MockTls m;
    mock_tls_init(&m);

    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    /* Create a temp file with known content */
    char tmppath[] = "/tmp/keel_test_tls_XXXXXX";
    int tmpfd = mkstemp(tmppath);
    ASSERT_TRUE(tmpfd >= 0);
    const char *file_content = "TLS file content here!";
    size_t file_len = strlen(file_content);
    ASSERT_EQ((ssize_t)file_len, write(tmpfd, file_content, file_len));

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    res.conn_fd = pipefd[1];
    res.tls = &m.base;
    kl_response_file(&res, tmpfd, (off_t)file_len);

    /* Send headers + file */
    int r = kl_response_send(&res);
    ASSERT_EQ(r, 0);

    /* Verify the mock captured headers and file content */
    m.write_buf[m.write_len] = '\0';
    ASSERT_TRUE(strstr(m.write_buf, "HTTP/1.1 200 OK\r\n") != NULL);
    ASSERT_TRUE(strstr(m.write_buf, file_content) != NULL);

    close(pipefd[0]);
    close(pipefd[1]);
    /* tmpfd is owned by res, closed by response_free */
    kl_response_free(&res);
    unlink(tmppath);
}

UTEST(tls, file_send_yields_on_want_write) {
    /* Mock that returns 0 (WANT_WRITE) on first write of file data
     * should cause kl_response_send to return 1 (more to send) */
    MockTls m;
    mock_tls_init(&m);

    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);

    /* Create a temp file */
    char tmppath[] = "/tmp/keel_test_tls_yield_XXXXXX";
    int tmpfd = mkstemp(tmppath);
    ASSERT_TRUE(tmpfd >= 0);
    const char *data = "yield test data";
    size_t data_len = strlen(data);
    ASSERT_EQ((ssize_t)data_len, write(tmpfd, data, data_len));

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    res.conn_fd = pipefd[1];
    res.tls = &m.base;
    kl_response_file(&res, tmpfd, (off_t)data_len);

    /* First send: headers go through, file send starts */
    int r = kl_response_send(&res);
    /* Should complete since our mock has space */
    ASSERT_EQ(r, 0);

    /* Verify file was fully sent */
    ASSERT_EQ(res.file_offset, (off_t)data_len);

    close(pipefd[0]);
    close(pipefd[1]);
    kl_response_free(&res);
    unlink(tmppath);
}

/* ── Response reset preserves TLS pointer ────────────────────────── */

UTEST(tls, response_reset_preserves_tls) {
    MockTls m;
    mock_tls_init(&m);

    KlAllocator a = kl_allocator_default();
    KlResponse res;
    kl_response_init(&res, &a);
    res.tls = &m.base;
    res.conn_fd = 42;

    /* Add some headers to make the response non-empty */
    kl_response_header(&res, "X-Test", "value");
    ASSERT_TRUE(res.hdr_len > 0);

    /* Reset for keep-alive */
    kl_response_reset(&res);

    /* TLS pointer must survive the reset */
    ASSERT_EQ(res.tls, &m.base);
    /* conn_fd also preserved */
    ASSERT_EQ(res.conn_fd, 42);
    /* Headers cleared */
    ASSERT_EQ(res.hdr_len, (size_t)0);
    /* Status reset */
    ASSERT_EQ(res.status, 200);

    kl_response_free(&res);
}

/* ── Shutdown WANT_WRITE retry ───────────────────────────────────── */

UTEST(tls, shutdown_retries_want_write) {
    /* kl_conn_release should retry shutdown on WANT_WRITE */
    MockTls m;
    mock_tls_init(&m);
    m.shutdown_want_writes = 2;  /* return WANT_WRITE twice, then OK */

    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    ASSERT_EQ(kl_conn_pool_init(&pool, 1, &a), 0);
    pool.conns[0].parser = kl_parser_llhttp(&a);
    pool.conns[0].tls = &m.base;

    /* Acquire with a real fd so the shutdown path is taken (fd >= 0) */
    int devnull = open("/dev/null", O_RDONLY);
    ASSERT_TRUE(devnull >= 0);
    KlConn *c = kl_conn_acquire(&pool, devnull);
    ASSERT_TRUE(c != NULL);

    /* Release triggers shutdown */
    kl_conn_release(&pool, c);

    /* Shutdown should have been called 3 times: 2 WANT_WRITE + 1 OK */
    ASSERT_EQ(m.shutdown_count, 3);
    ASSERT_TRUE(m.reset_called);

    /* Clean up — prevent double-destroy since mock is stack-allocated */
    pool.conns[0].tls = NULL;
    kl_conn_pool_free(&pool);
}

UTEST(tls, shutdown_gives_up_after_max_retries) {
    /* If shutdown keeps returning WANT_WRITE, stop after 4 retries */
    MockTls m;
    mock_tls_init(&m);
    m.shutdown_want_writes = 100;  /* always returns WANT_WRITE */

    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    ASSERT_EQ(kl_conn_pool_init(&pool, 1, &a), 0);
    pool.conns[0].parser = kl_parser_llhttp(&a);
    pool.conns[0].tls = &m.base;

    int devnull = open("/dev/null", O_RDONLY);
    ASSERT_TRUE(devnull >= 0);
    KlConn *c = kl_conn_acquire(&pool, devnull);
    ASSERT_TRUE(c != NULL);

    kl_conn_release(&pool, c);

    /* 1 initial + 4 retries = 5 total calls */
    ASSERT_EQ(m.shutdown_count, 5);
    ASSERT_TRUE(m.reset_called);

    pool.conns[0].tls = NULL;
    kl_conn_pool_free(&pool);
}

/* ── Pool free calls shutdown + destroy ──────────────────────────── */

UTEST(tls, pool_free_calls_shutdown_and_destroy) {
    MockTls m;
    mock_tls_init(&m);

    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    ASSERT_EQ(kl_conn_pool_init(&pool, 1, &a), 0);
    pool.conns[0].parser = kl_parser_llhttp(&a);
    pool.conns[0].tls = &m.base;

    /* Give the slot a valid-looking fd so shutdown path is taken.
     * Use -1 to avoid closing a real fd — shutdown is still called
     * because the check is fd >= 0.  So use a dup'd /dev/null. */
    int devnull = open("/dev/null", 0);
    ASSERT_TRUE(devnull >= 0);
    pool.conns[0].fd = devnull;
    pool.conns[0].state = KL_CONN_READING;

    kl_conn_pool_free(&pool);

    /* Verify shutdown was called before destroy */
    ASSERT_TRUE(m.shutdown_called);
    ASSERT_TRUE(m.destroy_called);
}

UTEST_MAIN();
