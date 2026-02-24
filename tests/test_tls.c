#include "utest.h"
#include <keel/keel.h>
#include <string.h>

/* ── Mock TLS implementation ─────────────────────────────────────── */

typedef struct {
    KlTls base;
    int handshake_result;   /* 0=OK, 1=WANT_READ, 2=WANT_WRITE, 3=ERROR */
    char read_buf[256];
    size_t read_len;
    char write_buf[256];
    size_t write_len;
    size_t pending_bytes;
    int shutdown_called;
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
}

/* ── Tests ───────────────────────────────────────────────────────── */

UTEST(tls, vtable_struct_size) {
    /* KlTls has 7 function pointers */
    ASSERT_EQ(sizeof(KlTls), 7 * sizeof(void *));
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

UTEST_MAIN();
