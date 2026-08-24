#include "utest.h"
#include <keel/keel.h>
#include <string.h>
#include "net_compat.h"

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

static KlTlsResult mock_handshake(KlTls *self, KlSocketHandle fd) {
    (void)fd;
    MockTls *m = (MockTls *)self;
    switch (m->handshake_result) {
        case 0: return KL_TLS_OK;
        case 1: return KL_TLS_WANT_READ;
        case 2: return KL_TLS_WANT_WRITE;
        default: return KL_TLS_ERROR;
    }
}

static kl_ssize_t mock_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len) {
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

static kl_ssize_t mock_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len) {
    (void)fd;
    MockTls *m = (MockTls *)self;
    size_t space = sizeof(m->write_buf) - m->write_len;
    if (space == 0) return 0;  /* WANT_WRITE */
    size_t to_copy = len < space ? len : space;
    memcpy(m->write_buf + m->write_len, buf, to_copy);
    m->write_len += to_copy;
    return (ssize_t)to_copy;
}

static KlTlsResult mock_shutdown(KlTls *self, KlSocketHandle fd) {
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
    /* KlTls has 14 function pointers (7 required + optional alpn_protocol +
     * set_hostname + peer_cert + feed_input + drain_output + set_socket_provider +
     * at_eof). The optional ops are additive/nullable: growth here is expected when
     * one is appended. */
    ASSERT_EQ(sizeof(KlTls), 14 * sizeof(void *));
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
