/*
 * tls_mbedtls.c — mbedTLS backend for Keel's KlTls vtable
 *
 * Implements server-side TLS (with optional mTLS) and client-side TLS
 * using mbedTLS 3.x. All I/O is non-blocking via custom BIO callbacks.
 *
 * SPDX-License-Identifier: MIT
 */

#include <keel/tls_mbedtls.h>

#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

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
    int                    is_server;  /* 1 = server, 0 = client */
    int                    has_ca;     /* 1 = ca_cert loaded */
} KlMbedtlsCtx;

/* ── Per-connection TLS session ──────────────────────────────────── */

typedef struct {
    KlTls              base;       /* vtable — must be first */
    mbedtls_ssl_context ssl;
    KlMbedtlsCtx      *ctx;       /* shared context (not owned) */
    KlAllocator        *alloc;
    int                 fd;        /* cached for BIO callbacks */
    int                 handshake_done;
} KlMbedtlsTls;

/* ── Custom BIO callbacks (non-blocking I/O) ─────────────────────── */

/*
 * mbedTLS requires custom send/recv callbacks for non-blocking sockets.
 * These translate between mbedTLS error codes and POSIX.
 */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)ctx;
    ssize_t ret;

#ifdef MSG_NOSIGNAL
    do { ret = send(t->fd, buf, len, MSG_NOSIGNAL); } while (ret < 0 && errno == EINTR);
#else
    do { ret = write(t->fd, buf, len); } while (ret < 0 && errno == EINTR);
#endif

    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    return (int)ret;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)ctx;
    ssize_t ret;

    do {
        ret = read(t->fd, buf, len);
    } while (ret < 0 && errno == EINTR);

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

static KlTlsResult tls_handshake(KlTls *self, int fd)
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

static ssize_t tls_read(KlTls *self, int fd, void *buf, size_t len)
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

static ssize_t tls_write(KlTls *self, int fd, const void *buf, size_t len)
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

static KlTlsResult tls_shutdown(KlTls *self, int fd)
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
    t->fd = -1;
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
    t->fd = -1;

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

static unsigned char *read_file(const char *path, size_t *out_len)
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
    unsigned char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);

    if (nread != (size_t)len) {
        free(buf);
        return NULL;
    }

    buf[nread] = '\0';
    *out_len = nread + 1;  /* include null terminator for PEM */
    return buf;
}

/* ── Server context creation ─────────────────────────────────────── */

/*
 * ctx_create functions use raw malloc/free intentionally:
 *   - Called once at startup, not per-request
 *   - Run before KlAllocator-tracked lifetime begins
 *   - Per-connection TLS sessions (factory) use kl_malloc/kl_free
 *   - read_file() uses raw malloc for temporary file I/O buffers
 */

KlTlsCtx *kl_tls_mbedtls_ctx_create(const char *cert_path,
                                      const char *key_path,
                                      const char *ca_path,
                                      int client_auth)
{
    if (!cert_path || !key_path)
        return NULL;

    KlMbedtlsCtx *ctx = malloc(sizeof(*ctx));  /* startup-only, see note above */
    if (!ctx)
        return NULL;

    memset(ctx, 0, sizeof(*ctx));
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

    /* Load server certificate */
    size_t cert_len;
    unsigned char *cert_buf = read_file(cert_path, &cert_len);
    if (!cert_buf)
        goto fail;

    ret = mbedtls_x509_crt_parse(&ctx->cert, cert_buf, cert_len);
    free(cert_buf);
    if (ret != 0)
        goto fail;

    /* Load server private key */
    size_t key_len;
    unsigned char *key_buf = read_file(key_path, &key_len);
    if (!key_buf)
        goto fail;

    ret = mbedtls_pk_parse_key(&ctx->pkey, key_buf, key_len,
                                NULL, 0, mbedtls_ctr_drbg_random, &ctx->drbg);
    kl_secure_zero(key_buf, key_len);
    free(key_buf);
    if (ret != 0)
        goto fail;

    /* Load CA certificate for mTLS (optional) */
    if (ca_path) {
        size_t ca_len;
        unsigned char *ca_buf = read_file(ca_path, &ca_len);
        if (!ca_buf)
            goto fail;

        ret = mbedtls_x509_crt_parse(&ctx->ca_cert, ca_buf, ca_len);
        free(ca_buf);
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
    free(ctx);
    return NULL;
}

/* ── Client context creation ─────────────────────────────────────── */

KlTlsCtx *kl_tls_mbedtls_client_ctx_create(const char *ca_path)
{
    KlMbedtlsCtx *ctx = malloc(sizeof(*ctx));  /* startup-only, see note above */
    if (!ctx)
        return NULL;

    memset(ctx, 0, sizeof(*ctx));
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
    if (ca_path) {
        size_t ca_len;
        unsigned char *ca_buf = read_file(ca_path, &ca_len);
        if (!ca_buf)
            goto fail;

        ret = mbedtls_x509_crt_parse(&ctx->ca_cert, ca_buf, ca_len);
        free(ca_buf);
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
         * Production deployments MUST provide a CA bundle path. */
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
    free(ctx);
    return NULL;
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

    free(ctx);
}
