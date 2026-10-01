/*
 * mock_tls.h: an identity (no-crypto) KlTls for tests.
 *
 * Implements the full KlTls vtable, INCLUDING the completion-mode feed_input/drain_output
 * ops, as a passthrough: "ciphertext" == plaintext. This lets the TLS-over-completion
 * driver run over the pollcomp backend in CI without mbedTLS; exercising comp_tls_drive,
 * the memory-BIO feed/drain plumbing, and comp_tls_send_response/file/stream/TLS-h2 for
 * real. It is the direct analogue of the pollcomp backend (a test double that makes an
 * axis runnable), one layer up.
 *
 * Role-agnostic + symmetric: the handshake is 0-RTT (immediate OK, no bytes on the wire),
 * so the SAME factory serves both the server (completion transport: feed/drain rings) and
 * the sync client (synchronous socket I/O). NOT a security construct; tests only.
 */
#ifndef KEEL_TEST_MOCK_TLS_H
#define KEEL_TEST_MOCK_TLS_H

#include <keel/tls.h>
#include <keel/allocator.h>
#include "../src/socket.h"    /* kl_sockdef_send / kl_sockdef_recv (sync mode) */
#include <string.h>

typedef struct {
    KlTls         base;
    KlAllocator  *alloc;
    int           comp_mode;   /* set once feed_input is called (completion transport) */
    unsigned char *in;  size_t in_len, in_pos, in_cap;  /* received bytes ring */
    unsigned char *out; size_t out_len, out_cap;         /* outgoing bytes ring */
    int           eof;         /* socket mode: the peer closed (read returned -1, at_eof says 1) */
    int           split_done;  /* socket mode: the one simulated split record has been served */
    size_t        served;      /* socket mode: plaintext bytes returned so far */
} MockTls;

/* Socket mode follows the KlTls read contract exactly: bytes (>0), 0 for WANT_READ (nothing
 * decryptable yet), -1 for error or a closed peer (at_eof tells which). A caller that reads a 0 as
 * end of stream is wrong under every real backend, and this mock now catches it.
 *
 * mock_tls_split_record > 0 simulates ONE TLS record split across TCP reads, the normal case on a
 * real network: the first read takes whatever the socket has into the engine's buffer and returns
 * 0 (WANT_READ) until it holds mock_tls_split_record bytes, then plaintext comes out of the buffer
 * (pending() reports what is still buffered). Static-per-TU, default 0 (off). */
static size_t mock_tls_split_record = 0;
/* Bytes served normally before the split record starts (so a test can aim the split at a request
 * body rather than its headers). The first read past this point gathers the record. */
static size_t mock_tls_split_after = 0;

static int mock_tls_grow(KlAllocator *a, unsigned char **buf, size_t *cap, size_t need) {
    if (need <= *cap) return 0;
    size_t ncap = *cap ? *cap : 4096;
    while (ncap < need) ncap *= 2;
    unsigned char *nb = kl_realloc(a, *buf, *cap, ncap);
    if (!nb) return -1;
    *buf = nb;
    *cap = ncap;
    return 0;
}

static KlTlsResult mock_tls_handshake(KlTls *self, KlSocketHandle fd) {
    (void)self; (void)fd;
    return KL_TLS_OK;   /* 0-RTT identity handshake; no bytes exchanged */
}

static kl_ssize_t mock_tls_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len) {
    MockTls *m = (MockTls *)self;
    if (m->comp_mode) {
        size_t avail = m->in_len - m->in_pos;
        if (avail == 0) return 0;   /* WANT_READ */
        size_t n = avail < len ? avail : len;
        memcpy(buf, m->in + m->in_pos, n);
        m->in_pos += n;
        if (m->in_pos == m->in_len) m->in_pos = m->in_len = 0;   /* drained; reset ring */
        return (ssize_t)n;
    }
    if (mock_tls_split_record && !m->split_done && m->served >= mock_tls_split_after) {
        /* Gather the record: take what the socket has, report WANT_READ until it is complete. */
        while (m->in_len < mock_tls_split_record) {
            if (mock_tls_grow(m->alloc, &m->in, &m->in_cap, mock_tls_split_record) < 0) return -1;
            kl_ssize_t r = kl_sockdef_recv(fd, m->in + m->in_len, mock_tls_split_record - m->in_len);
            if (r > 0) { m->in_len += (size_t)r; continue; }
            if (r == 0) { m->eof = 1; return -1; }
            return kl_sockdef_io_status() == KL_IO_WOULD_BLOCK ? 0 : -1;
        }
        m->split_done = 1;
    }
    if (m->in_pos < m->in_len) {                 /* serve the gathered record first */
        size_t avail = m->in_len - m->in_pos;
        size_t n = avail < len ? avail : len;
        memcpy(buf, m->in + m->in_pos, n);
        m->in_pos += n;
        if (m->in_pos == m->in_len) m->in_pos = m->in_len = 0;
        m->served += n;
        return (kl_ssize_t)n;
    }
    if (mock_tls_split_record && !m->split_done && m->served < mock_tls_split_after &&
        len > mock_tls_split_after - m->served)
        len = mock_tls_split_after - m->served;  /* stop at the split point */
    kl_ssize_t r = kl_sockdef_recv(fd, buf, len);
    if (r > 0) { m->served += (size_t)r; return r; }
    if (r == 0) { m->eof = 1; return -1; }       /* clean close: -1, and at_eof() says so */
    return kl_sockdef_io_status() == KL_IO_WOULD_BLOCK ? 0 : -1;   /* WANT_READ is 0, not -1 */
}

static kl_ssize_t mock_tls_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len) {
    MockTls *m = (MockTls *)self;
    if (m->comp_mode) {
        if (mock_tls_grow(m->alloc, &m->out, &m->out_cap, m->out_len + len) < 0) return -1;
        memcpy(m->out + m->out_len, buf, len);
        m->out_len += len;
        return (ssize_t)len;
    }
    kl_ssize_t r = kl_sockdef_send(fd, buf, len);
    if (r >= 0) return r;
    return kl_sockdef_io_status() == KL_IO_WOULD_BLOCK ? 0 : -1;   /* WANT_WRITE is 0, not -1 */
}

static int mock_tls_feed_input(KlTls *self, const void *cipher, size_t len) {
    MockTls *m = (MockTls *)self;
    m->comp_mode = 1;
    if (len == 0) return 0;
    if (m->in_pos > 0) {   /* compact the consumed prefix */
        memmove(m->in, m->in + m->in_pos, m->in_len - m->in_pos);
        m->in_len -= m->in_pos;
        m->in_pos = 0;
    }
    if (mock_tls_grow(m->alloc, &m->in, &m->in_cap, m->in_len + len) < 0) return -1;
    memcpy(m->in + m->in_len, cipher, len);
    m->in_len += len;
    return 0;
}

static kl_ssize_t mock_tls_drain_output(KlTls *self, void *buf, size_t cap) {
    MockTls *m = (MockTls *)self;
    size_t n = m->out_len < cap ? m->out_len : cap;
    if (n) memcpy(buf, m->out, n);
    m->out_len -= n;
    if (m->out_len) memmove(m->out, m->out + n, m->out_len);
    return (ssize_t)n;
}

static KlTlsResult mock_tls_shutdown(KlTls *self, KlSocketHandle fd) {
    (void)self; (void)fd;
    return KL_TLS_OK;
}
static size_t mock_tls_pending(KlTls *self) {
    MockTls *m = (MockTls *)self;
    if (!m->comp_mode && mock_tls_split_record && !m->split_done &&
        m->served >= mock_tls_split_after) return 0;                          /* record incomplete */
    return m->in_len - m->in_pos;
}
static int mock_tls_at_eof(KlTls *self) { return ((MockTls *)self)->eof; }
static void mock_tls_reset(KlTls *self) {   /* keep-alive: clear buffers, keep comp_mode */
    MockTls *m = (MockTls *)self;
    m->in_len = m->in_pos = m->out_len = 0;
}
static void mock_tls_destroy(KlTls *self) {
    MockTls *m = (MockTls *)self;
    kl_free(m->alloc, m->in, m->in_cap);
    kl_free(m->alloc, m->out, m->out_cap);
    kl_free(m->alloc, m, sizeof(*m));
}
/* When set to 1, mock_tls_set_hostname returns -1; simulating an SNI /
 * hostname-verification setup failure. Clients MUST fail closed (abort the
 * connection) rather than handshake without hostname verification. Static-per-TU,
 * default 0 (success), so it doesn't affect includers that don't opt in. */
static int mock_tls_set_hostname_fail = 0;
static int mock_tls_set_hostname(KlTls *self, const char *hostname) {
    (void)self; (void)hostname;
    return mock_tls_set_hostname_fail ? -1 : 0;
}

/* Optional peer-cert hook. Default NULL → the peer_cert vtable slot is left NULL (unchanged
 * for callers that don't set it). A test that exercises kl_http_request_peer_cert() installs its own
 * implementation (supplying the cert values) before creating the mock. Static-per-TU, so it
 * doesn't affect other includers. */
static int (*mock_tls_peer_cert_fn)(KlTls *self, KlPeerCert *out) = NULL;

/* Configurable negotiated ALPN protocol: a test sets this before the handshake to
 * simulate the protocol a real TLS server would have selected. NULL = no ALPN
 * negotiated (returns NULL, exactly as before this hook existed). Static-per-TU. */
static const char *mock_tls_alpn = NULL;
static const char *mock_tls_alpn_protocol(KlTls *self) {
    (void)self;
    return mock_tls_alpn;
}

/* KlTlsFactory: usable as both KlHttpServerConfig.tls->factory and KlHttpClientConfig.tls->factory. */
static KlTls *mock_tls_create(KlTlsCtx *ctx, KlAllocator *alloc) {
    (void)ctx;
    MockTls *m = kl_malloc(alloc, sizeof(*m));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->alloc = alloc;
    m->base.handshake     = mock_tls_handshake;
    m->base.read          = mock_tls_read;
    m->base.write         = mock_tls_write;
    m->base.shutdown      = mock_tls_shutdown;
    m->base.pending       = mock_tls_pending;
    m->base.reset         = mock_tls_reset;
    m->base.destroy       = mock_tls_destroy;
    m->base.alpn_protocol = mock_tls_alpn_protocol;
    m->base.set_hostname  = mock_tls_set_hostname;
    m->base.peer_cert     = mock_tls_peer_cert_fn;
    m->base.feed_input    = mock_tls_feed_input;
    m->base.drain_output  = mock_tls_drain_output;
    m->base.at_eof        = mock_tls_at_eof;
    return &m->base;
}

#endif /* KEEL_TEST_MOCK_TLS_H */
