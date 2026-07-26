/*
 * tls_mbedtls.c — mbedTLS backend for Keel's KlTls vtable
 *
 * Implements server-side TLS (with optional mTLS) and client-side TLS
 * using mbedTLS 3.x. All I/O is non-blocking via custom BIO callbacks.
 *
 * SPDX-License-Identifier: MIT
 */

#include <keel/tls_mbedtls.h>

/* The socket seam: BIO I/O goes through kl_sockdef_send/recv (POSIX + Winsock,
 * with EINTR-retry, SIGPIPE suppression, and errno translation), and this also
 * supplies inet_ntop/AF_INET (SAN formatting) + KlSocketHandle. Included before
 * the mbedTLS headers so, on Windows, winsock2.h precedes any windows.h an
 * mbedTLS header might pull in. */
#include "socket.h"

#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>   /* MBEDTLS_ERR_NET_SEND/RECV_FAILED (BIO codes) */
#include <mbedtls/sha256.h>
#include <mbedtls/oid.h>
#include <mbedtls/asn1.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Secure zeroing — scrub key material before free.
 * Uses volatile pointer to prevent compiler from optimizing away the store.
 * Portable across all platforms without requiring platform-specific APIs
 * (explicit_bzero requires _DEFAULT_SOURCE on Linux, is unavailable in
 *  strict C11 mode on macOS). */
static void kl_secure_zero(void *ptr, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) *p++ = 0;
}

/* ── Internal context structure ──────────────────────────────────── */

typedef struct {
    mbedtls_ssl_config    conf;
    mbedtls_x509_crt      cert;       /* server/client certificate */
    mbedtls_pk_context     pkey;       /* private key */
    mbedtls_x509_crt      ca_cert;    /* CA cert for peer verification */
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
    KlAllocator           *alloc;     /* allocator used to create this context */
    int                    is_server;  /* 1 = server, 0 = client */
    int                    has_ca;     /* 1 = ca_cert loaded */
} KlMbedtlsCtx;

/* ── Per-connection TLS session ──────────────────────────────────── */

typedef struct {
    KlTls              base;       /* vtable — must be first */
    mbedtls_ssl_context ssl;
    KlMbedtlsCtx      *ctx;       /* shared context (not owned) */
    KlAllocator        *alloc;
    KlSocketHandle      fd;        /* cached for BIO callbacks */
    int                 handshake_done;
} KlMbedtlsTls;

/* ── Custom BIO callbacks (non-blocking I/O) ─────────────────────── */

/*
 * mbedTLS requires custom send/recv callbacks for non-blocking sockets.
 * These translate between mbedTLS error codes and POSIX.
 */

/* cppcheck-suppress constParameterCallback ; ctx must be void* to match the
 * mbedTLS f_send BIO function-pointer signature (mbedtls_ssl_set_bio) */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    const KlMbedtlsTls *t = (const KlMbedtlsTls *)ctx;
    /* kl_sockdef_send suppresses SIGPIPE (MSG_NOSIGNAL) and retries EINTR on
     * POSIX; on Windows it's the Winsock send with kl_wsa_set_errno — so the
     * EAGAIN/EWOULDBLOCK check below works on both. */
    ssize_t ret = kl_sockdef_send(t->fd, buf, len);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    return (int)ret;
}

/* cppcheck-suppress constParameterCallback ; ctx must be void* to match the
 * mbedTLS f_recv BIO function-pointer signature (mbedtls_ssl_set_bio) */
static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    const KlMbedtlsTls *t = (const KlMbedtlsTls *)ctx;
    ssize_t ret = kl_sockdef_recv(t->fd, buf, len);

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    if (ret == 0)
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;

    return (int)ret;
}

/* ── KlTls vtable implementation ─────────────────────────────────── */

static KlTlsResult tls_handshake(KlTls *self, KlSocketHandle fd)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    t->fd = fd;

    int ret = mbedtls_ssl_handshake(&t->ssl);

    if (ret == 0) {
        t->handshake_done = 1;
        return KL_TLS_OK;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ)
        return KL_TLS_WANT_READ;
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return KL_TLS_WANT_WRITE;

    return KL_TLS_ERROR;
}

static ssize_t tls_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    t->fd = fd;

    int ret = mbedtls_ssl_read(&t->ssl, (unsigned char *)buf, len);

    if (ret > 0)
        return ret;
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return -1;  /* clean shutdown or EOF */
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;   /* retry */

    return -1;  /* error */
}

static ssize_t tls_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    t->fd = fd;

    int ret = mbedtls_ssl_write(&t->ssl, (const unsigned char *)buf, len);

    if (ret > 0)
        return ret;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;   /* retry */

    return -1;  /* error */
}

static KlTlsResult tls_shutdown(KlTls *self, KlSocketHandle fd)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    t->fd = fd;

    int ret = mbedtls_ssl_close_notify(&t->ssl);

    if (ret == 0)
        return KL_TLS_OK;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ)
        return KL_TLS_WANT_READ;
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        return KL_TLS_WANT_WRITE;

    return KL_TLS_OK;  /* best-effort — don't fail on shutdown errors */
}

static size_t tls_pending(KlTls *self)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    return mbedtls_ssl_get_bytes_avail(&t->ssl);
}

static void tls_reset(KlTls *self)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    mbedtls_ssl_session_reset(&t->ssl);
    t->handshake_done = 0;
    t->fd = KL_INVALID_SOCKET;
}

static void tls_destroy(KlTls *self)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    mbedtls_ssl_free(&t->ssl);
    kl_free(t->alloc, t, sizeof(*t));
}

static const char *tls_alpn_protocol(KlTls *self)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    if (!t->handshake_done)
        return NULL;
    return mbedtls_ssl_get_alpn_protocol(&t->ssl);
}

/* ── mTLS peer-certificate extraction ────────────────────────────── */

/* Copy the CommonName RDN of an X.509 name into a NUL-terminated buffer. */
static void x509_extract_cn(const mbedtls_x509_name *name, char *out, size_t outlen)
{
    out[0] = '\0';
    for (const mbedtls_x509_name *n = name; n != NULL; n = n->next) {
        if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid) == 0) {
            size_t len = n->val.len;
            if (len >= outlen)
                len = outlen - 1;
            memcpy(out, n->val.p, len);
            out[len] = '\0';
            return;
        }
    }
}

/* Render the subjectAltName sequence as a comma-separated "DNS:x,IP:y" list. */
static void x509_extract_san(const mbedtls_x509_sequence *seq, char *out, size_t outlen)
{
    out[0] = '\0';
    size_t off = 0;
    for (const mbedtls_x509_sequence *cur = seq; cur != NULL; cur = cur->next) {
        char item[INET6_ADDRSTRLEN + 8];
        int tag = cur->buf.tag & MBEDTLS_ASN1_TAG_VALUE_MASK;

        if (tag == MBEDTLS_X509_SAN_DNS_NAME) {
            size_t len = cur->buf.len;
            if (len > sizeof(item) - 5)
                len = sizeof(item) - 5;
            memcpy(item, "DNS:", 4);
            memcpy(item + 4, cur->buf.p, len);
            item[4 + len] = '\0';
        } else if (tag == MBEDTLS_X509_SAN_IP_ADDRESS) {
            char ip[INET6_ADDRSTRLEN];
            if (cur->buf.len == 4 && inet_ntop(AF_INET, cur->buf.p, ip, sizeof(ip)))
                snprintf(item, sizeof(item), "IP:%s", ip);
            else if (cur->buf.len == 16 && inet_ntop(AF_INET6, cur->buf.p, ip, sizeof(ip)))
                snprintf(item, sizeof(item), "IP:%s", ip);
            else
                continue;
        } else {
            continue;   /* rfc822Name, URI, etc. — not surfaced */
        }

        size_t ilen = strlen(item);
        if (off + ilen + (off ? 1u : 0u) >= outlen)
            break;      /* out of room — stop cleanly */
        if (off)
            out[off++] = ',';
        memcpy(out + off, item, ilen);
        off += ilen;
        out[off] = '\0';
    }
}

/* Convert an X.509 UTC broken-down time to a Unix timestamp (timegm-style,
 * no TZ dependency; Howard Hinnant's days-from-civil algorithm). */
static int64_t x509_time_to_unix(const mbedtls_x509_time *t)
{
    int64_t y = t->year;
    int m = t->mon, d = t->day;
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + (int64_t)t->hour * 3600 + (int64_t)t->min * 60 + t->sec;
}

static int tls_peer_cert(KlTls *self, KlPeerCert *out)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    if (!t->handshake_done)
        return -1;

    const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&t->ssl);
    if (!crt)
        return -1;   /* no client certificate presented */

    memset(out, 0, sizeof(*out));
    out->verified = (mbedtls_ssl_get_verify_result(&t->ssl) == 0) ? 1 : 0;
    x509_extract_cn(&crt->subject, out->subject_cn, sizeof(out->subject_cn));
    x509_extract_cn(&crt->issuer, out->issuer_cn, sizeof(out->issuer_cn));
    x509_extract_san(&crt->subject_alt_names, out->san, sizeof(out->san));

    unsigned char hash[32];
    if (mbedtls_sha256(crt->raw.p, crt->raw.len, hash, 0) == 0) {
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            out->fingerprint_sha256[i * 2]     = hex[hash[i] >> 4];
            out->fingerprint_sha256[i * 2 + 1] = hex[hash[i] & 0x0F];
        }
        out->fingerprint_sha256[64] = '\0';
    }

    out->not_before = x509_time_to_unix(&crt->valid_from);
    out->not_after  = x509_time_to_unix(&crt->valid_to);
    out->der     = crt->raw.p;
    out->der_len = crt->raw.len;
    return 0;
}

/* ── Factory ─────────────────────────────────────────────────────── */

KlTls *kl_tls_mbedtls_create(KlTlsCtx *ctx, KlAllocator *alloc)
{
    if (!ctx || !alloc)
        return NULL;

    KlMbedtlsCtx *mctx = (KlMbedtlsCtx *)ctx;

    KlMbedtlsTls *t = kl_malloc(alloc, sizeof(*t));
    if (!t)
        return NULL;

    memset(t, 0, sizeof(*t));
    t->alloc = alloc;
    t->ctx = mctx;
    t->fd = KL_INVALID_SOCKET;

    /* Set up vtable */
    t->base.handshake    = tls_handshake;
    t->base.read         = tls_read;
    t->base.write        = tls_write;
    t->base.shutdown     = tls_shutdown;
    t->base.pending      = tls_pending;
    t->base.reset        = tls_reset;
    t->base.destroy      = tls_destroy;
    t->base.alpn_protocol = tls_alpn_protocol;
    t->base.set_hostname  = kl_tls_mbedtls_set_hostname;
    t->base.peer_cert     = tls_peer_cert;

    /* Initialize SSL context */
    mbedtls_ssl_init(&t->ssl);

    int ret = mbedtls_ssl_setup(&t->ssl, &mctx->conf);
    if (ret != 0) {
        mbedtls_ssl_free(&t->ssl);
        kl_free(alloc, t, sizeof(*t));
        return NULL;
    }

    /* Set BIO callbacks — pass 't' as context */
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);

    return &t->base;
}

/* ── Hostname for SNI (client mode) ──────────────────────────────── */

int kl_tls_mbedtls_set_hostname(KlTls *tls, const char *hostname)
{
    if (!tls || !hostname)
        return -1;

    KlMbedtlsTls *t = (KlMbedtlsTls *)tls;
    int ret = mbedtls_ssl_set_hostname(&t->ssl, hostname);
    return (ret == 0) ? 0 : -1;
}

/* ── Helper: read entire file into buffer ────────────────────────── */

static unsigned char *read_file(const char *path, size_t *out_len,
                                 KlAllocator *alloc)
{
    if (!path || !out_len)
        return NULL;

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long len = ftell(f);
    if (len < 0 || len > 1024 * 1024) {  /* 1 MB max for cert/key files */
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    /* +1 for null terminator (PEM parser needs it) */
    unsigned char *buf = kl_malloc(alloc, (size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);

    if (nread != (size_t)len) {
        kl_free(alloc, buf, (size_t)len + 1);
        return NULL;
    }

    buf[nread] = '\0';
    *out_len = nread + 1;  /* include null terminator for PEM */
    return buf;
}

/* ── Server context creation ─────────────────────────────────────── */

/* Build a server context from in-memory cert/key/(optional CA) buffers. PEM
 * buffers must be NUL-terminated with the length counting the NUL (mbedTLS PEM
 * rule); DER is auto-detected. Does not take ownership of or scrub the buffers —
 * the caller does. Shared by the file-path and _from_buf public entry points. */
static KlTlsCtx *server_ctx_from_mem(const unsigned char *cert_buf, size_t cert_len,
                                     const unsigned char *key_buf, size_t key_len,
                                     const unsigned char *ca_buf, size_t ca_len,
                                     int client_auth, KlAllocator *alloc)
{
    if (!cert_buf || !key_buf || !alloc)
        return NULL;

    KlMbedtlsCtx *ctx = kl_malloc(alloc, sizeof(*ctx));
    if (!ctx)
        return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    ctx->is_server = 1;

    /* Initialize all mbedTLS structures */
    mbedtls_ssl_config_init(&ctx->conf);
    mbedtls_x509_crt_init(&ctx->cert);
    mbedtls_pk_init(&ctx->pkey);
    mbedtls_x509_crt_init(&ctx->ca_cert);
    mbedtls_ctr_drbg_init(&ctx->drbg);
    mbedtls_entropy_init(&ctx->entropy);

    int ret;

    /* Seed RNG */
    ret = mbedtls_ctr_drbg_seed(&ctx->drbg, mbedtls_entropy_func,
                                 &ctx->entropy,
                                 (const unsigned char *)"keel-tls", 8);
    if (ret != 0)
        goto fail;

    ret = mbedtls_x509_crt_parse(&ctx->cert, cert_buf, cert_len);
    if (ret != 0)
        goto fail;

    ret = mbedtls_pk_parse_key(&ctx->pkey, key_buf, key_len,
                                NULL, 0, mbedtls_ctr_drbg_random, &ctx->drbg);
    if (ret != 0)
        goto fail;

    /* CA certificate for mTLS (optional) */
    if (ca_buf && ca_len) {
        ret = mbedtls_x509_crt_parse(&ctx->ca_cert, ca_buf, ca_len);
        if (ret != 0)
            goto fail;
        ctx->has_ca = 1;
    }

    /* Configure SSL */
    ret = mbedtls_ssl_config_defaults(&ctx->conf,
                                       MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        goto fail;

    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->drbg);
    mbedtls_ssl_conf_ca_chain(&ctx->conf, ctx->has_ca ? &ctx->ca_cert : NULL, NULL);

    ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->cert, &ctx->pkey);
    if (ret != 0)
        goto fail;

    /* mTLS: set client authentication mode */
    switch (client_auth) {
    case KL_MTLS_OPTIONAL:
        mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        break;
    case KL_MTLS_REQUIRED:
        mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        break;
    default:
        mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
        break;
    }

    return (KlTlsCtx *)ctx;

fail:
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->pkey);
    mbedtls_x509_crt_free(&ctx->ca_cert);
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    kl_free(alloc, ctx, sizeof(*ctx));
    return NULL;
}

KlTlsCtx *kl_tls_mbedtls_ctx_create(const char *cert_path,
                                      const char *key_path,
                                      const char *ca_path,
                                      int client_auth,
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
        kl_free(alloc, cert_buf, cert_len);
        return NULL;
    }
    unsigned char *ca_buf = NULL;
    if (ca_path) {
        ca_buf = read_file(ca_path, &ca_len, alloc);
        if (!ca_buf) {
            kl_secure_zero(key_buf, key_len);
            kl_free(alloc, key_buf, key_len);
            kl_free(alloc, cert_buf, cert_len);
            return NULL;
        }
    }

    KlTlsCtx *ctx = server_ctx_from_mem(cert_buf, cert_len, key_buf, key_len,
                                        ca_buf, ca_len, client_auth, alloc);

    kl_free(alloc, cert_buf, cert_len);
    kl_secure_zero(key_buf, key_len);
    kl_free(alloc, key_buf, key_len);
    if (ca_buf)
        kl_free(alloc, ca_buf, ca_len);
    return ctx;
}

KlTlsCtx *kl_tls_mbedtls_ctx_create_from_buf(const unsigned char *cert_buf, size_t cert_len,
                                              const unsigned char *key_buf, size_t key_len,
                                              const unsigned char *ca_buf, size_t ca_len,
                                              int client_auth, KlAllocator *alloc)
{
    return server_ctx_from_mem(cert_buf, cert_len, key_buf, key_len,
                               ca_buf, ca_len, client_auth, alloc);
}

/* ── Client context creation ─────────────────────────────────────── */

/* Shared client-ctx initialization. Caller passes either:
 *   - ca_buf/ca_len: in-memory CA bundle (parsed, copied), OR
 *   - both NULL/0 : no CA verification (verify_none)
 * Returns the ctx on success, NULL on failure (mbedTLS structures freed). */
static KlTlsCtx *client_ctx_create_from_mem(const unsigned char *ca_buf,
                                              size_t ca_len,
                                              KlAllocator *alloc)
{
    if (!alloc)
        return NULL;

    KlMbedtlsCtx *ctx = kl_malloc(alloc, sizeof(*ctx));
    if (!ctx)
        return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    ctx->is_server = 0;

    /* Initialize all mbedTLS structures */
    mbedtls_ssl_config_init(&ctx->conf);
    mbedtls_x509_crt_init(&ctx->cert);
    mbedtls_pk_init(&ctx->pkey);
    mbedtls_x509_crt_init(&ctx->ca_cert);
    mbedtls_ctr_drbg_init(&ctx->drbg);
    mbedtls_entropy_init(&ctx->entropy);

    int ret;

    /* Seed RNG */
    ret = mbedtls_ctr_drbg_seed(&ctx->drbg, mbedtls_entropy_func,
                                 &ctx->entropy,
                                 (const unsigned char *)"keel-client", 11);
    if (ret != 0)
        goto fail;

    /* Load CA certificates for server verification (optional) */
    if (ca_buf && ca_len > 0) {
        ret = mbedtls_x509_crt_parse(&ctx->ca_cert, ca_buf, ca_len);
        if (ret != 0)
            goto fail;
        ctx->has_ca = 1;
    }

    /* Configure as TLS client */
    ret = mbedtls_ssl_config_defaults(&ctx->conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        goto fail;

    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->drbg);

    if (ctx->has_ca) {
        mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_cert, NULL);
        mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* WARNING: No CA provided — TLS certificate verification DISABLED.
         * Connections are encrypted but vulnerable to MITM attacks.
         * Production deployments MUST provide a CA bundle. */
        mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    return (KlTlsCtx *)ctx;

fail:
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->pkey);
    mbedtls_x509_crt_free(&ctx->ca_cert);
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    kl_free(alloc, ctx, sizeof(*ctx));
    return NULL;
}

KlTlsCtx *kl_tls_mbedtls_client_ctx_create(const char *ca_path,
                                              KlAllocator *alloc)
{
    if (!alloc)
        return NULL;

    /* No CA path → no-verify client */
    if (!ca_path)
        return client_ctx_create_from_mem(NULL, 0, alloc);

    size_t ca_len;
    unsigned char *ca_buf = read_file(ca_path, &ca_len, alloc);
    if (!ca_buf)
        return NULL;

    KlTlsCtx *ctx = client_ctx_create_from_mem(ca_buf, ca_len, alloc);
    kl_free(alloc, ca_buf, ca_len);
    return ctx;
}

KlTlsCtx *kl_tls_mbedtls_client_ctx_create_from_buf(const unsigned char *ca_buf,
                                                      size_t ca_len,
                                                      KlAllocator *alloc)
{
    if (!ca_buf || ca_len == 0 || !alloc)
        return NULL;
    return client_ctx_create_from_mem(ca_buf, ca_len, alloc);
}

/* ── Context destruction ─────────────────────────────────────────── */

void kl_tls_mbedtls_ctx_destroy(KlTlsCtx *raw_ctx)
{
    if (!raw_ctx)
        return;

    KlMbedtlsCtx *ctx = (KlMbedtlsCtx *)raw_ctx;

    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->pkey);
    mbedtls_x509_crt_free(&ctx->ca_cert);
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);

    kl_free(ctx->alloc, ctx, sizeof(*ctx));
}
