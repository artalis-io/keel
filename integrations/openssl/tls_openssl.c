/*
 * tls_openssl.c — OpenSSL / BoringSSL backend for Keel's KlTls vtable
 *
 * ONE adapter TU serving both OpenSSL 3.x and BoringSSL: the API is shared, the
 * few divergences are guarded with `#if defined(OPENSSL_IS_BORINGSSL)`. Builds
 * integrations/openssl/libkeel_openssl.a (OpenSSL) and, from
 * integrations/boringssl/, the same source against a BoringSSL prefix.
 *
 * Implements server-side TLS (with optional mTLS) and client-side TLS. Two
 * transport modes over one custom BIO_METHOD:
 *   - readiness / socket-BIO (default): ciphertext I/O via kl_sock_send/recv on
 *     the connection fd; would-block → BIO_set_retry + WANT_READ/WANT_WRITE.
 *   - completion / memory-BIO: once feed_input() is first called, the BIO reads
 *     ciphertext from an internal in-buffer (fed by feed_input) and appends
 *     outgoing ciphertext to an out-buffer (drained by drain_output); the fd is
 *     never touched. This is how io_uring/IOCP/pollcomp/lwip-raw/UEFI drive TLS.
 *
 * The adapter lives ABOVE the socket seam: it includes only <keel/tls.h>,
 * <keel/allocator.h>, the internal src/socket.h seam, and the TLS library
 * headers — never an event-engine or platform-networking header.
 *
 * SPDX-License-Identifier: MIT
 */

#include "keel_tls_openssl.h"

/* The socket seam: BIO I/O goes through kl_sock_send/kl_sock_recv (POSIX +
 * Winsock, with EINTR-retry, SIGPIPE suppression, and errno translation), and
 * this also supplies inet_ntop/AF_INET (SAN formatting) + KlSocketHandle.
 * Included before the OpenSSL headers so, on Windows, winsock2.h precedes any
 * windows.h an OpenSSL header might pull in. */
#include "socket.h"

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/asn1.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Secure zeroing — scrub key material before free. Volatile pointer prevents
 * the store being optimized away; portable, no platform-specific API. */
static void kl_secure_zero(void *ptr, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) *p++ = 0;
}

/* ── Internal context structure ──────────────────────────────────── */

typedef struct {
    SSL_CTX               *ssl_ctx;
    KlAllocator           *alloc;
    int                    is_server;
    /* When set, a bare transport EOF (no close_notify) counts as a clean EOF
     * (lenient). Default 0 = strict. Inherited by each session. */
    int                    allow_truncation;
    /* Optional socket provider the socket-BIO routes ciphertext through (NULL =
     * host kl_sockdef_*). Inherited by each session. */
    const KlSocketProvider *sp;
    /* ALPN wire-format list (len-prefixed protocol names), ctx-owned. Server:
     * candidate list the select callback picks from. Client: passed to
     * SSL_CTX_set_alpn_protos (which copies), kept for symmetry. */
    unsigned char          alpn_wire[256];
    size_t                 alpn_wire_len;
} KlOpensslCtx;

/* ── Per-connection TLS session ──────────────────────────────────── */

typedef struct {
    KlTls              base;       /* vtable — must be first */
    SSL               *ssl;
    KlOpensslCtx      *ctx;       /* shared context (not owned) */
    KlAllocator        *alloc;
    KlSocketHandle      fd;        /* cached for BIO callbacks (socket-BIO mode) */
    const KlSocketProvider *sp;    /* socket provider for BIO I/O (NULL = host default) */
    int                 handshake_done;
    int                 eof_seen;  /* set when read() hit a clean close_notify/EOF */
    int                 allow_truncation;  /* inherited from ctx (strict EOF policy) */
    /* Completion (memory-BIO) mode. Active once feed_input() is first called:
     * the BIO reads ciphertext from in_buf (fed by the caller) and appends
     * outgoing ciphertext to out_buf (drained by the caller). */
    int                 comp_mode;
    unsigned char      *in_buf;   size_t in_cap, in_len, in_pos;
    unsigned char      *out_buf;  size_t out_cap, out_len;
    /* Session-owned peer-cert DER (peer_cert()); valid until this session's next
     * reset()/destroy() per the KlPeerCert contract. Not shared across sessions. */
    unsigned char      *der_buf;  size_t der_cap, der_len;
    /* Session-owned NUL-terminated negotiated ALPN name (alpn_protocol()); an
     * ALPN protocol name is <= 255 bytes by the wire format. */
    char                alpn[256];
} KlOpensslTls;

/* Cap on either completion ciphertext ring (a TLS record is <= ~16 KiB; cert-
 * chain handshake flights are larger — 256 KiB is generous, overflow → error). */
#define KL_TLS_COMP_MAX (256u * 1024u)

/* Ensure *buf has capacity for `need` bytes (grow by doubling, capped). */
static int comp_ensure(unsigned char **buf, size_t *cap, size_t need,
                       KlAllocator *alloc) {
    if (need <= *cap)
        return 0;
    if (need > KL_TLS_COMP_MAX)
        return -1;
    size_t nc = *cap ? *cap : 4096;
    while (nc < need) nc = (nc > KL_TLS_COMP_MAX / 2) ? KL_TLS_COMP_MAX : nc * 2;
    unsigned char *nb = kl_realloc(alloc, *buf, *cap, nc);
    if (!nb)
        return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

/* ── Custom BIO_METHOD (non-blocking, socket-or-memory I/O) ───────── */

/*
 * OpenSSL (unlike mbedTLS's set_bio callbacks) drives ciphertext through a BIO
 * object. We register one custom BIO_METHOD whose read/write route to the
 * KlOpensslTls stored via BIO_set_data. In completion mode the callbacks use the
 * in/out rings; in readiness mode they use kl_sock_recv/kl_sock_send on t->fd,
 * mapping would-block to BIO_set_retry_read/write + return -1 (which OpenSSL
 * surfaces as SSL_ERROR_WANT_READ/WRITE), and a 0-byte recv to a clean EOF.
 */

static BIO_METHOD *kl_bio_method; /* created lazily, process-lifetime */

static int kl_bio_write(BIO *bio, const char *buf, int len)
{
    KlOpensslTls *t = (KlOpensslTls *)BIO_get_data(bio);
    BIO_clear_retry_flags(bio);
    if (len <= 0)
        return 0;
    size_t n = (size_t)len;

    if (t->comp_mode) {   /* memory BIO: append to the outgoing-ciphertext ring */
        /* Overflow-safe cap check BEFORE the add: n > MAX - out_len ⇒ over cap. */
        if (n > KL_TLS_COMP_MAX - t->out_len)
            return -1;   /* buffer full — fatal (no retry flag) */
        if (comp_ensure(&t->out_buf, &t->out_cap, t->out_len + n, t->alloc) < 0)
            return -1;   /* buffer full — fatal (no retry flag) */
        memcpy(t->out_buf + t->out_len, buf, n);
        t->out_len += n;
        return len;
    }

    /* readiness: kl_sock_send routes through t->sp (or kl_sockdef_send when
     * NULL): SIGPIPE-suppressed + EINTR-retried on POSIX, Winsock errno-mapped
     * on Windows — so the EAGAIN/EWOULDBLOCK check below works on both. */
    ssize_t ret = kl_sock_send(t->sp, t->fd, buf, n);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            BIO_set_retry_write(bio);
            return -1;
        }
        return -1;   /* fatal, no retry flag */
    }
    return (int)ret;
}

static int kl_bio_read(BIO *bio, char *buf, int len)
{
    KlOpensslTls *t = (KlOpensslTls *)BIO_get_data(bio);
    BIO_clear_retry_flags(bio);
    if (len <= 0)
        return 0;
    size_t cap = (size_t)len;

    if (t->comp_mode) {   /* memory BIO: consume from the fed-ciphertext ring */
        size_t avail = t->in_len - t->in_pos;
        if (avail == 0) {
            BIO_set_retry_read(bio);   /* caller must feed_input more */
            return -1;
        }
        size_t nn = cap < avail ? cap : avail;
        memcpy(buf, t->in_buf + t->in_pos, nn);
        t->in_pos += nn;
        if (t->in_pos >= t->in_len)
            t->in_pos = t->in_len = 0;
        return (int)nn;
    }

    ssize_t ret = kl_sock_recv(t->sp, t->fd, buf, cap);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            BIO_set_retry_read(bio);
            return -1;
        }
        return -1;   /* fatal, no retry flag */
    }
    /* ret == 0: peer closed the transport — clean EOF. A 0 return from a source
     * BIO signals EOF to OpenSSL (SSL_read → SSL_ERROR_ZERO_RETURN / -1). */
    return (int)ret;
}

static long kl_bio_ctrl(BIO *bio, int cmd, long larg, void *parg)
{
    (void)bio; (void)larg; (void)parg;
    switch (cmd) {
    case BIO_CTRL_FLUSH:   /* SSL_write flushes the BIO; nothing buffered here */
        return 1;
    case BIO_CTRL_PUSH:
    case BIO_CTRL_POP:
        return 0;
    default:
        return 0;
    }
}

static int kl_bio_create(BIO *bio)
{
    BIO_set_init(bio, 1);
    BIO_set_data(bio, NULL);
    return 1;
}

static int kl_bio_destroy(BIO *bio)
{
    if (!bio)
        return 0;
    BIO_set_data(bio, NULL);
    BIO_set_init(bio, 0);
    return 1;
}

/* Create the process-wide custom BIO_METHOD exactly once, thread-safely (via
 * pthread_once). Every BIO_meth_set_*() return is checked; on any failure the
 * partial method is freed and the global left NULL, so callers fail the session
 * create. */
static pthread_once_t g_bio_once = PTHREAD_ONCE_INIT;

static void kl_bio_method_init(void)
{
    int type = BIO_get_new_index() | BIO_TYPE_SOURCE_SINK;
    BIO_METHOD *m = BIO_meth_new(type, "keel-tls-bio");
    if (!m)
        return;
    if (BIO_meth_set_write(m, kl_bio_write) != 1 ||
        BIO_meth_set_read(m, kl_bio_read) != 1 ||
        BIO_meth_set_ctrl(m, kl_bio_ctrl) != 1 ||
        BIO_meth_set_create(m, kl_bio_create) != 1 ||
        BIO_meth_set_destroy(m, kl_bio_destroy) != 1) {
        BIO_meth_free(m);
        return;   /* leave kl_bio_method NULL — session create will fail cleanly */
    }
    kl_bio_method = m;
}

static BIO_METHOD *kl_bio_get_method(void)
{
    pthread_once(&g_bio_once, kl_bio_method_init);
    return kl_bio_method;
}

/* ── KlTls vtable implementation ─────────────────────────────────── */

static KlTlsResult tls_handshake(KlTls *self, KlSocketHandle fd)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    t->fd = fd;

    int ret = SSL_do_handshake(t->ssl);
    if (ret == 1) {
        t->handshake_done = 1;
        return KL_TLS_OK;
    }
    int err = SSL_get_error(t->ssl, ret);
    if (err == SSL_ERROR_WANT_READ)
        return KL_TLS_WANT_READ;
    if (err == SSL_ERROR_WANT_WRITE)
        return KL_TLS_WANT_WRITE;
    return KL_TLS_ERROR;
}

static kl_ssize_t tls_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    t->fd = fd;
    if (len == 0)
        return 0;

    int want = len > INT_MAX ? INT_MAX : (int)len;
    int ret = SSL_read(t->ssl, buf, want);
    if (ret > 0)
        return ret;

    int err = SSL_get_error(t->ssl, ret);
    if (err == SSL_ERROR_ZERO_RETURN) {
        t->eof_seen = 1;   /* clean close_notify — at_eof() reports it */
        return -1;
    }
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return 0;   /* retry */
    if (err == SSL_ERROR_SYSCALL) {
        /* A transport EOF without a close_notify surfaces here with ret==0 and no
         * queued error. STRICT (default): this is a truncation — return -1 with
         * eof_seen staying 0, so a truncated close-delimited response is NOT
         * accepted as complete. Only when allow_truncation is opted in do we treat
         * a clean transport EOF as at_eof(). A real close_notify goes through
         * SSL_ERROR_ZERO_RETURN above and always sets eof_seen. */
        if (ret == 0 && ERR_peek_error() == 0 && t->allow_truncation) {
            t->eof_seen = 1;
            return -1;
        }
        return -1;
    }
    return -1;   /* error */
}

/* at_eof: was the last read() -1 a clean TLS shutdown (close_notify/EOF)? */
static int tls_at_eof(KlTls *self)
{
    return ((KlOpensslTls *)self)->eof_seen;
}

static kl_ssize_t tls_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    t->fd = fd;
    if (len == 0)
        return 0;

    int want = len > INT_MAX ? INT_MAX : (int)len;
    int ret = SSL_write(t->ssl, buf, want);
    if (ret > 0)
        return ret;

    int err = SSL_get_error(t->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return 0;   /* retry */
    return -1;   /* error */
}

static KlTlsResult tls_shutdown(KlTls *self, KlSocketHandle fd)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    t->fd = fd;

    int ret = SSL_shutdown(t->ssl);
    if (ret >= 0)
        return KL_TLS_OK;   /* 0 = close_notify sent (peer's pending), 1 = done */
    int err = SSL_get_error(t->ssl, ret);
    if (err == SSL_ERROR_WANT_READ)
        return KL_TLS_WANT_READ;
    if (err == SSL_ERROR_WANT_WRITE)
        return KL_TLS_WANT_WRITE;
    return KL_TLS_OK;   /* best-effort — don't fail on shutdown errors */
}

static size_t tls_pending(KlTls *self)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    int p = SSL_pending(t->ssl);
    return p > 0 ? (size_t)p : 0;
}

/* Completion mode: feed received ciphertext to the engine's input ring. */
static int tls_feed_input(KlTls *self, const void *cipher, size_t len)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    if (!cipher && len != 0)
        return -1;   /* NULL buffer with a non-zero length is a caller bug */
    t->comp_mode = 1;
    if (t->in_pos > 0) {   /* compact the partially-consumed prefix */
        memmove(t->in_buf, t->in_buf + t->in_pos, t->in_len - t->in_pos);
        t->in_len -= t->in_pos;
        t->in_pos = 0;
    }
    if (len == 0)
        return 0;   /* no-op success (also the mode-enable call) */
    /* Overflow-safe cap check BEFORE the add: len > MAX - in_len ⇒ over cap. */
    if (len > KL_TLS_COMP_MAX - t->in_len)
        return -1;
    if (comp_ensure(&t->in_buf, &t->in_cap, t->in_len + len, t->alloc) < 0)
        return -1;
    memcpy(t->in_buf + t->in_len, cipher, len);
    t->in_len += len;
    return 0;
}

/* Completion mode: drain the engine's pending outgoing ciphertext. */
static kl_ssize_t tls_drain_output(KlTls *self, void *buf, size_t cap)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    if (!buf && cap != 0)
        return -1;   /* NULL buffer with a non-zero cap is a caller bug */
    if (cap == 0)
        return 0;    /* no-op success */
    size_t avail = t->out_len;
    size_t n = (avail < cap) ? avail : cap;
    if (n)
        memcpy(buf, t->out_buf, n);
    t->out_len = avail - n;
    if (t->out_len)
        memmove(t->out_buf, t->out_buf + n, t->out_len);
    return (ssize_t)n;
}

static void tls_reset(KlTls *self)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    /* SSL_clear resets the session for reuse; the BIO stays attached. */
    SSL_clear(t->ssl);
    t->handshake_done = 0;
    t->eof_seen = 0;
    t->fd = KL_INVALID_SOCKET;
    /* Drop buffered ciphertext; keep transport mode + allocated rings. */
    t->in_len = t->in_pos = t->out_len = 0;
    /* Invalidate any previously-returned peer-cert DER (contract: valid only
     * until reset()); keep the buffer allocated for reuse. */
    t->der_len = 0;
    t->alpn[0] = '\0';
}

static void tls_destroy(KlTls *self)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    if (t->ssl)
        SSL_free(t->ssl);   /* frees the attached BIO too (SSL_set_bio ownership) */
    kl_free(t->alloc, t->in_buf, t->in_cap);
    kl_free(t->alloc, t->out_buf, t->out_cap);
    kl_free(t->alloc, t->der_buf, t->der_cap);
    kl_free(t->alloc, t, sizeof(*t));
}

static const char *tls_alpn_protocol(KlTls *self)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    if (!t->handshake_done)
        return NULL;
    const unsigned char *proto = NULL;
    unsigned int plen = 0;
    SSL_get0_alpn_selected(t->ssl, &proto, &plen);
    if (!proto || plen == 0)
        return NULL;
    /* Return a NUL-terminated copy in this SESSION's own buffer (not a shared
     * thread-local), so a second session on the same thread never clobbers it.
     * ALPN names are <= 255 bytes by the wire format. */
    unsigned int n = plen < sizeof(t->alpn) - 1 ? plen : (unsigned int)sizeof(t->alpn) - 1;
    memcpy(t->alpn, proto, n);
    t->alpn[n] = '\0';
    return t->alpn;
}

/* ── mTLS peer-certificate extraction ────────────────────────────── */

/* Copy the CN of an X509_NAME into a NUL-terminated buffer. */
static void x509_name_cn(X509_NAME *name, char *out, size_t outlen)
{
    if (outlen == 0)
        return;
    out[0] = '\0';
    if (!name)
        return;
    int idx = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
    if (idx < 0)
        return;
    X509_NAME_ENTRY *e = X509_NAME_get_entry(name, idx);
    if (!e)
        return;
    ASN1_STRING *s = X509_NAME_ENTRY_get_data(e);
    if (!s)
        return;
    unsigned char *utf8 = NULL;
    int len = ASN1_STRING_to_UTF8(&utf8, s);
    if (len < 0 || !utf8)
        return;
    size_t n = (size_t)len < outlen - 1 ? (size_t)len : outlen - 1;
    memcpy(out, utf8, n);
    out[n] = '\0';
    OPENSSL_free(utf8);
}

/* Render the subjectAltName GENERAL_NAMES as a comma-separated "DNS:x,IP:y". */
static void x509_extract_san(X509 *crt, char *out, size_t outlen)
{
    if (outlen == 0)
        return;
    out[0] = '\0';
    GENERAL_NAMES *sans = X509_get_ext_d2i(crt, NID_subject_alt_name, NULL, NULL);
    if (!sans)
        return;

    size_t off = 0;
    int count = sk_GENERAL_NAME_num(sans);
    for (int i = 0; i < count; i++) {
        GENERAL_NAME *gn = sk_GENERAL_NAME_value(sans, i);
        if (!gn)
            continue;
        char item[INET6_ADDRSTRLEN + 8];
        item[0] = '\0';

        if (gn->type == GEN_DNS) {
            const unsigned char *dns = ASN1_STRING_get0_data(gn->d.dNSName);
            int dlen = ASN1_STRING_length(gn->d.dNSName);
            if (dlen < 0)
                continue;
            size_t dl = (size_t)dlen;
            if (dl > sizeof(item) - 5)
                dl = sizeof(item) - 5;
            memcpy(item, "DNS:", 4);
            memcpy(item + 4, dns, dl);
            item[4 + dl] = '\0';
        } else if (gn->type == GEN_IPADD) {
            const unsigned char *ip = ASN1_STRING_get0_data(gn->d.iPAddress);
            int iplen = ASN1_STRING_length(gn->d.iPAddress);
            char ipstr[INET6_ADDRSTRLEN];
            if (iplen == 4 && inet_ntop(AF_INET, ip, ipstr, sizeof(ipstr)))
                snprintf(item, sizeof(item), "IP:%s", ipstr);
            else if (iplen == 16 && inet_ntop(AF_INET6, ip, ipstr, sizeof(ipstr)))
                snprintf(item, sizeof(item), "IP:%s", ipstr);
            else
                continue;
        } else {
            continue;   /* rfc822Name, URI, etc. — not surfaced */
        }

        size_t ilen = strlen(item);
        if (off + ilen + (off ? 1u : 0u) >= outlen)
            break;
        if (off)
            out[off++] = ',';
        memcpy(out + off, item, ilen);
        off += ilen;
        out[off] = '\0';
    }
    GENERAL_NAMES_free(sans);
}

/* Convert an ASN1_TIME to a Unix timestamp. BoringSSL does not provide
 * ASN1_TIME_to_tm; it offers ASN1_TIME_to_time_t (a direct Unix seconds
 * conversion) instead. OpenSSL (1.1.1+) and LibreSSL use ASN1_TIME_to_tm plus a
 * TZ-free timegm-style conversion (Howard Hinnant's days-from-civil), avoiding a
 * dependency on the non-portable timegm(). */
static int64_t asn1_time_to_unix(const ASN1_TIME *at)
{
    if (!at)
        return 0;
#if defined(OPENSSL_IS_BORINGSSL)
    time_t tt = 0;
    if (ASN1_TIME_to_time_t(at, &tt) != 1)
        return 0;
    return (int64_t)tt;
#else
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (ASN1_TIME_to_tm(at, &tm) != 1)
        return 0;
    int64_t y = tm.tm_year + 1900;
    int m = tm.tm_mon + 1, d = tm.tm_mday;
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + (int64_t)tm.tm_hour * 3600 +
           (int64_t)tm.tm_min * 60 + tm.tm_sec;
#endif
}

static int tls_peer_cert(KlTls *self, KlPeerCert *out)
{
    KlOpensslTls *t = (KlOpensslTls *)self;
    if (!t->handshake_done)
        return -1;

    /* SSL_get1_peer_certificate is the OpenSSL 3.0 rename; OpenSSL 1.1.x, BoringSSL,
     * and LibreSSL keep the historical SSL_get_peer_certificate (all return a +1 ref
     * we must X509_free). Cover the whole OpenSSL API family. */
#if defined(OPENSSL_IS_BORINGSSL) || defined(LIBRESSL_VERSION_NUMBER) || \
    (defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x30000000L)
    X509 *crt = SSL_get_peer_certificate(t->ssl);
#else
    X509 *crt = SSL_get1_peer_certificate(t->ssl);
#endif
    if (!crt)
        return -1;   /* no client certificate presented */

    memset(out, 0, sizeof(*out));
    out->verified = (SSL_get_verify_result(t->ssl) == X509_V_OK) ? 1 : 0;
    x509_name_cn(X509_get_subject_name(crt), out->subject_cn, sizeof(out->subject_cn));
    x509_name_cn(X509_get_issuer_name(crt), out->issuer_cn, sizeof(out->issuer_cn));
    x509_extract_san(crt, out->san, sizeof(out->san));

    /* DER + SHA-256 fingerprint. Stash the DER in THIS session's own buffer (not a
     * shared thread-local) so `der` stays valid until this session's next
     * reset()/destroy() per the KlPeerCert contract — a second session on the same
     * thread never clobbers it. */
    int der_len = i2d_X509(crt, NULL);
    if (der_len > 0) {
        if ((size_t)der_len > t->der_cap) {
            unsigned char *nb = kl_realloc(t->alloc, t->der_buf, t->der_cap,
                                           (size_t)der_len);
            if (nb) {
                t->der_buf = nb;
                t->der_cap = (size_t)der_len;
            }
        }
        if (t->der_buf && (size_t)der_len <= t->der_cap) {
            unsigned char *p = t->der_buf;
            i2d_X509(crt, &p);
            t->der_len = (size_t)der_len;
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256(t->der_buf, (size_t)der_len, hash);
            static const char hex[] = "0123456789abcdef";
            for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
                out->fingerprint_sha256[i * 2]     = hex[hash[i] >> 4];
                out->fingerprint_sha256[i * 2 + 1] = hex[hash[i] & 0x0F];
            }
            out->fingerprint_sha256[SHA256_DIGEST_LENGTH * 2] = '\0';
            out->der = t->der_buf;
            out->der_len = (size_t)der_len;
        }
    }

    out->not_before = asn1_time_to_unix(X509_get0_notBefore(crt));
    out->not_after  = asn1_time_to_unix(X509_get0_notAfter(crt));

    X509_free(crt);
    return 0;
}

/* KlTls.set_socket_provider — framework wires the connection's provider here
 * before the handshake so the socket-BIO matches the connection's stack. */
static void tls_set_socket_provider(KlTls *self, const struct KlSocketProvider *sp)
{
    ((KlOpensslTls *)self)->sp = (const KlSocketProvider *)sp;
}

/* ── Factory ─────────────────────────────────────────────────────── */

KlTls *kl_tls_openssl_create(KlTlsCtx *ctx, KlAllocator *alloc)
{
    if (!ctx || !alloc)
        return NULL;

    KlOpensslCtx *octx = (KlOpensslCtx *)ctx;

    BIO_METHOD *meth = kl_bio_get_method();
    if (!meth)
        return NULL;

    KlOpensslTls *t = kl_malloc(alloc, sizeof(*t));
    if (!t)
        return NULL;

    memset(t, 0, sizeof(*t));
    t->alloc = alloc;
    t->ctx = octx;
    t->fd = KL_INVALID_SOCKET;
    t->sp = octx->sp;   /* inherit the ctx's socket provider (NULL = host) */
    t->allow_truncation = octx->allow_truncation;   /* inherit strict-EOF policy */

    t->base.handshake     = tls_handshake;
    t->base.read          = tls_read;
    t->base.write         = tls_write;
    t->base.shutdown      = tls_shutdown;
    t->base.pending       = tls_pending;
    t->base.reset         = tls_reset;
    t->base.destroy       = tls_destroy;
    t->base.alpn_protocol = tls_alpn_protocol;
    t->base.set_hostname  = kl_tls_openssl_set_hostname;
    t->base.peer_cert     = tls_peer_cert;
    t->base.feed_input    = tls_feed_input;
    t->base.drain_output  = tls_drain_output;
    t->base.set_socket_provider = tls_set_socket_provider;
    t->base.at_eof        = tls_at_eof;

    t->comp_mode = 0;

    t->ssl = SSL_new(octx->ssl_ctx);
    if (!t->ssl) {
        kl_free(alloc, t, sizeof(*t));
        return NULL;
    }

    if (octx->is_server)
        SSL_set_accept_state(t->ssl);
    else
        SSL_set_connect_state(t->ssl);

    /* One custom BIO for both read and write; SSL takes ownership (freed by
     * SSL_free). BIO_up_ref not needed for the single-BIO case per OpenSSL docs
     * — but SSL_set_bio with the same rbio==wbio consumes exactly one ref. */
    BIO *bio = BIO_new(meth);
    if (!bio) {
        SSL_free(t->ssl);
        kl_free(alloc, t, sizeof(*t));
        return NULL;
    }
    BIO_set_data(bio, t);
    BIO_set_init(bio, 1);
    SSL_set_bio(t->ssl, bio, bio);

    return &t->base;
}

/* ── Hostname for SNI + verification (client mode) ───────────────── */

int kl_tls_openssl_set_hostname(KlTls *tls, const char *hostname)
{
    if (!tls || !hostname)
        return -1;
    KlOpensslTls *t = (KlOpensslTls *)tls;

    /* SNI server_name extension. */
    if (SSL_set_tlsext_host_name(t->ssl, hostname) != 1)
        return -1;

    /* Enable hostname verification against the peer cert. Harmless when the ctx
     * is verify-none (no chain is checked, so the host param is simply unused).
     * SSL_set1_host both sets the expected name and turns on the check. */
    if (SSL_set1_host(t->ssl, hostname) != 1)
        return -1;

    return 0;
}

/* ── ALPN ────────────────────────────────────────────────────────── */

/* Server ALPN select callback: pick the server's first candidate that the
 * client also offered (server preference). `arg` is the KlOpensslCtx. */
static int alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen, void *arg)
{
    (void)ssl;
    KlOpensslCtx *ctx = (KlOpensslCtx *)arg;
    /* Walk our (server) candidate list; for each, scan the client's offered
     * list for a match. First server hit wins. */
    const unsigned char *srv = ctx->alpn_wire;
    size_t srv_end = ctx->alpn_wire_len;
    for (size_t i = 0; i < srv_end; ) {
        unsigned char slen = srv[i];
        const unsigned char *sname = &srv[i + 1];
        for (unsigned int j = 0; j + 1 <= inlen; ) {
            unsigned char clen = in[j];
            if (j + 1 + clen > inlen)
                break;
            const unsigned char *cname = &in[j + 1];
            if (clen == slen && memcmp(sname, cname, slen) == 0) {
                *out = sname;
                *outlen = slen;
                return SSL_TLSEXT_ERR_OK;
            }
            j += 1 + clen;
        }
        i += 1 + slen;
    }
    return SSL_TLSEXT_ERR_NOACK;   /* no overlap — proceed without ALPN */
}

int kl_tls_openssl_ctx_set_alpn(KlTlsCtx *c, const char **protos)
{
    KlOpensslCtx *ctx = (KlOpensslCtx *)c;
    if (!ctx || !protos)
        return -1;

    /* Build the len-prefixed wire list into ctx-owned storage. */
    size_t off = 0;
    for (int i = 0; protos[i]; i++) {
        size_t l = strlen(protos[i]);
        if (l == 0 || l > 255)
            return -1;
        if (off + 1 + l > sizeof(ctx->alpn_wire))
            return -1;
        ctx->alpn_wire[off++] = (unsigned char)l;
        memcpy(ctx->alpn_wire + off, protos[i], l);
        off += l;
    }
    ctx->alpn_wire_len = off;

    if (ctx->is_server) {
        SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, alpn_select_cb, ctx);
        return 0;
    }
    /* Client: OpenSSL copies the wire list. Returns 0 on success (note the
     * inverted convention of this one OpenSSL call). */
    return SSL_CTX_set_alpn_protos(ctx->ssl_ctx, ctx->alpn_wire,
                                   (unsigned int)ctx->alpn_wire_len) == 0 ? 0 : -1;
}

int kl_tls_openssl_ctx_set_socket_provider(KlTlsCtx *c, const struct KlSocketProvider *sp)
{
    KlOpensslCtx *ctx = (KlOpensslCtx *)c;
    if (!ctx)
        return -1;
    ctx->sp = (const KlSocketProvider *)sp;
    return 0;
}

int kl_tls_openssl_ctx_set_allow_truncation(KlTlsCtx *c, int allow)
{
    KlOpensslCtx *ctx = (KlOpensslCtx *)c;
    if (!ctx)
        return -1;
    ctx->allow_truncation = allow ? 1 : 0;
    return 0;
}

/* ── Helper: read entire file into buffer ────────────────────────── */

static unsigned char *read_file(const char *path, size_t *out_len, KlAllocator *alloc)
{
    if (!path || !out_len)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0 || len > 1024 * 1024) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *buf = kl_malloc(alloc, (size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (nread != (size_t)len) {
        kl_free(alloc, buf, (size_t)len + 1);
        return NULL;
    }
    buf[nread] = '\0';
    *out_len = nread;
    return buf;
}

/* ── Cert/key/CA loading from PEM buffers ────────────────────────── */

/* Safe wrapper around BIO_new_mem_buf: BIO_new_mem_buf takes an `int` length, so
 * reject anything that would narrow badly BEFORE the cast — empty/NULL buffers and
 * lengths that overflow int. Returns NULL on bad args (callers then fail cleanly). */
static BIO *bio_new_mem_checked(const void *buf, size_t len)
{
    if (len == 0 || len > INT_MAX)
        return NULL;
    if (!buf)
        return NULL;
    return BIO_new_mem_buf(buf, (int)len);
}

/* Load a PEM certificate chain into ssl_ctx (leaf + intermediates). */
static int load_cert_chain_pem(SSL_CTX *sc, const unsigned char *pem, size_t len)
{
    BIO *bio = bio_new_mem_checked(pem, len);
    if (!bio)
        return -1;
    int rc = -1;
    X509 *leaf = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    if (!leaf)
        goto out;
    if (SSL_CTX_use_certificate(sc, leaf) != 1) {
        X509_free(leaf);
        goto out;
    }
    X509_free(leaf);
    /* Any following certs are chain (intermediates). */
    SSL_CTX_clear_chain_certs(sc);
    X509 *ca;
    while ((ca = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        if (SSL_CTX_add0_chain_cert(sc, ca) != 1) {   /* takes ownership on success */
            X509_free(ca);
            goto out;
        }
    }
    ERR_clear_error();   /* the terminating PEM_read failure is expected EOF */
    rc = 0;
out:
    BIO_free(bio);
    return rc;
}

static int load_private_key_pem(SSL_CTX *sc, const unsigned char *pem, size_t len)
{
    BIO *bio = bio_new_mem_checked(pem, len);
    if (!bio)
        return -1;
    EVP_PKEY *pk = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pk)
        return -1;
    int rc = (SSL_CTX_use_PrivateKey(sc, pk) == 1 &&
              SSL_CTX_check_private_key(sc) == 1) ? 0 : -1;
    EVP_PKEY_free(pk);
    return rc;
}

/* Load PEM CA(s) into the ctx's trust store (for verifying the peer). */
static int load_ca_store_pem(SSL_CTX *sc, const unsigned char *pem, size_t len)
{
    X509_STORE *store = SSL_CTX_get_cert_store(sc);
    if (!store)
        return -1;
    BIO *bio = bio_new_mem_checked(pem, len);
    if (!bio)
        return -1;
    int added = 0, rc = -1;
    X509 *ca;
    while ((ca = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        if (X509_STORE_add_cert(store, ca) == 1)
            added++;
        X509_free(ca);
    }
    ERR_clear_error();
    BIO_free(bio);
    if (added > 0)
        rc = 0;
    return rc;
}

/* For a server doing mTLS, also advertise the acceptable client-CA names. */
static void load_client_ca_names(SSL_CTX *sc, const unsigned char *pem, size_t len)
{
    BIO *bio = bio_new_mem_checked(pem, len);
    if (!bio)
        return;
    X509 *ca;
    while ((ca = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
        X509_NAME *nm = X509_get_subject_name(ca);
        if (nm)
            SSL_CTX_add_client_CA(sc, ca);   /* copies the name */
        X509_free(ca);
    }
    ERR_clear_error();
    BIO_free(bio);
}

/* ── Server context creation ─────────────────────────────────────── */

static KlTlsCtx *server_ctx_from_mem(const unsigned char *cert_buf, size_t cert_len,
                                     const unsigned char *key_buf, size_t key_len,
                                     const unsigned char *ca_buf, size_t ca_len,
                                     KlMtlsMode client_auth, KlAllocator *alloc)
{
    if (!cert_buf || !key_buf || !alloc)
        return NULL;

    /* Validate the mTLS mode + CA arguments before touching OpenSSL. */
    if (client_auth != KL_MTLS_NONE &&
        client_auth != KL_MTLS_OPTIONAL &&
        client_auth != KL_MTLS_REQUIRED)
        return NULL;   /* out-of-range mode */
    /* ca_buf/ca_len must agree (both present or both absent). */
    if ((ca_buf == NULL) != (ca_len == 0))
        return NULL;
    /* OPTIONAL / REQUIRED client auth is meaningless without a CA to verify. */
    if ((client_auth == KL_MTLS_OPTIONAL || client_auth == KL_MTLS_REQUIRED) &&
        (ca_buf == NULL || ca_len == 0))
        return NULL;

    KlOpensslCtx *ctx = kl_malloc(alloc, sizeof(*ctx));
    if (!ctx)
        return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    ctx->is_server = 1;

    ctx->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx->ssl_ctx)
        goto fail;

    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);

    if (load_cert_chain_pem(ctx->ssl_ctx, cert_buf, cert_len) != 0)
        goto fail;
    if (load_private_key_pem(ctx->ssl_ctx, key_buf, key_len) != 0)
        goto fail;

    if (ca_buf && ca_len) {
        if (load_ca_store_pem(ctx->ssl_ctx, ca_buf, ca_len) != 0)
            goto fail;
        load_client_ca_names(ctx->ssl_ctx, ca_buf, ca_len);
    }

    switch (client_auth) {
    case KL_MTLS_OPTIONAL:
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
        break;
    case KL_MTLS_REQUIRED:
        SSL_CTX_set_verify(ctx->ssl_ctx,
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        break;
    default:
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);
        break;
    }

    return (KlTlsCtx *)ctx;

fail:
    if (ctx->ssl_ctx)
        SSL_CTX_free(ctx->ssl_ctx);
    kl_free(alloc, ctx, sizeof(*ctx));
    return NULL;
}

KlTlsCtx *kl_tls_openssl_ctx_create(const char *cert_path, const char *key_path,
                                    const char *ca_path, KlMtlsMode client_auth,
                                    KlAllocator *alloc)
{
    if (!cert_path || !key_path || !alloc)
        return NULL;

    size_t cert_len = 0, key_len = 0, ca_len = 0;
    unsigned char *cert_buf = read_file(cert_path, &cert_len, alloc);
    if (!cert_buf)
        return NULL;
    unsigned char *key_buf = read_file(key_path, &key_len, alloc);
    if (!key_buf) {
        kl_free(alloc, cert_buf, cert_len + 1);
        return NULL;
    }
    unsigned char *ca_buf = NULL;
    if (ca_path) {
        ca_buf = read_file(ca_path, &ca_len, alloc);
        if (!ca_buf) {
            kl_secure_zero(key_buf, key_len);
            kl_free(alloc, key_buf, key_len + 1);
            kl_free(alloc, cert_buf, cert_len + 1);
            return NULL;
        }
    }

    KlTlsCtx *ctx = server_ctx_from_mem(cert_buf, cert_len, key_buf, key_len,
                                        ca_buf, ca_len, client_auth, alloc);

    kl_free(alloc, cert_buf, cert_len + 1);
    kl_secure_zero(key_buf, key_len);
    kl_free(alloc, key_buf, key_len + 1);
    if (ca_buf)
        kl_free(alloc, ca_buf, ca_len + 1);
    return ctx;
}

KlTlsCtx *kl_tls_openssl_ctx_create_from_buf(const unsigned char *cert_buf, size_t cert_len,
                                             const unsigned char *key_buf, size_t key_len,
                                             const unsigned char *ca_buf, size_t ca_len,
                                             KlMtlsMode client_auth, KlAllocator *alloc)
{
    return server_ctx_from_mem(cert_buf, cert_len, key_buf, key_len,
                               ca_buf, ca_len, client_auth, alloc);
}

/* ── Client context creation ─────────────────────────────────────── */

/* Create a client ctx. Verification policy:
 *   insecure != 0            → SSL_VERIFY_NONE (explicit opt-out).
 *   ca_buf/ca_len provided   → load that CA bundle + SSL_VERIFY_PEER.
 *   ca_buf == NULL (secure)  → system default trust store + SSL_VERIFY_PEER. */
static KlTlsCtx *client_ctx_create_from_mem(const unsigned char *ca_buf, size_t ca_len,
                                            int insecure, KlAllocator *alloc)
{
    if (!alloc)
        return NULL;

    KlOpensslCtx *ctx = kl_malloc(alloc, sizeof(*ctx));
    if (!ctx)
        return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    ctx->is_server = 0;

    ctx->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx->ssl_ctx)
        goto fail;

    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);

    if (insecure) {
        /* WARNING: verify-none — encrypted but MITM-vulnerable. Explicit opt-in
         * via kl_tls_openssl_client_ctx_create_insecure(). */
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);
    } else if (ca_buf && ca_len > 0) {
        /* Verify against the caller-supplied CA bundle. */
        if (load_ca_store_pem(ctx->ssl_ctx, ca_buf, ca_len) != 0)
            goto fail;
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    } else {
        /* Secure default: verify against the system trust store. */
        if (SSL_CTX_set_default_verify_paths(ctx->ssl_ctx) != 1)
            goto fail;
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    }

    return (KlTlsCtx *)ctx;

fail:
    if (ctx->ssl_ctx)
        SSL_CTX_free(ctx->ssl_ctx);
    kl_free(alloc, ctx, sizeof(*ctx));
    return NULL;
}

KlTlsCtx *kl_tls_openssl_client_ctx_create(const char *ca_path, KlAllocator *alloc)
{
    if (!alloc)
        return NULL;
    if (!ca_path)   /* system default trust store, still verifying */
        return client_ctx_create_from_mem(NULL, 0, 0, alloc);

    size_t ca_len = 0;
    unsigned char *ca_buf = read_file(ca_path, &ca_len, alloc);
    if (!ca_buf)
        return NULL;
    KlTlsCtx *ctx = client_ctx_create_from_mem(ca_buf, ca_len, 0, alloc);
    kl_free(alloc, ca_buf, ca_len + 1);
    return ctx;
}

KlTlsCtx *kl_tls_openssl_client_ctx_create_from_buf(const unsigned char *ca_buf,
                                                    size_t ca_len, KlAllocator *alloc)
{
    if (!alloc)
        return NULL;
    if (ca_buf == NULL)   /* system default trust store, still verifying */
        return client_ctx_create_from_mem(NULL, 0, 0, alloc);
    if (ca_len == 0)
        return NULL;      /* non-NULL buffer must be non-empty */
    return client_ctx_create_from_mem(ca_buf, ca_len, 0, alloc);
}

KlTlsCtx *kl_tls_openssl_client_ctx_create_insecure(KlAllocator *alloc)
{
    return client_ctx_create_from_mem(NULL, 0, 1, alloc);
}

int kl_tls_openssl_client_ctx_set_cert(KlTlsCtx *c,
                                       const unsigned char *cert_buf, size_t cert_len,
                                       const unsigned char *key_buf, size_t key_len)
{
    KlOpensslCtx *ctx = (KlOpensslCtx *)c;
    if (!ctx || ctx->is_server || !cert_buf || !key_buf)
        return -1;
    if (load_cert_chain_pem(ctx->ssl_ctx, cert_buf, cert_len) != 0)
        return -1;
    if (load_private_key_pem(ctx->ssl_ctx, key_buf, key_len) != 0)
        return -1;
    return 0;
}

/* ── Context destruction ─────────────────────────────────────────── */

void kl_tls_openssl_ctx_destroy(KlTlsCtx *raw_ctx)
{
    if (!raw_ctx)
        return;
    KlOpensslCtx *ctx = (KlOpensslCtx *)raw_ctx;
    if (ctx->ssl_ctx)
        SSL_CTX_free(ctx->ssl_ctx);
    kl_free(ctx->alloc, ctx, sizeof(*ctx));
}
