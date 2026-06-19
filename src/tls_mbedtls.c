/*
 * tls_mbedtls.c — mbedTLS backend for Keel's KlTls vtable
 *
 * Implements server-side TLS (with optional mTLS) and client-side TLS
 * using mbedTLS 3.x. All I/O is non-blocking via custom BIO callbacks.
 *
 * SPDX-License-Identifier: MIT
 */

#include <keel/tls_mbedtls.h>
#include <sh_seal_arena.h>

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

/*
 * Policy half — configured once at _create then sealed read-only.
 * Holds the SSL config (incl. authmode, cipher allowlist, RNG
 * callback pointer), the cert chain, private key, and CA chain.
 *
 * Why seal: a heap-write primitive into `conf.MBEDTLS_PRIVATE(authmode)`
 * (a single byte) would silently flip mTLS verification from
 * VERIFY_REQUIRED to VERIFY_NONE, disabling peer certificate
 * checks.  Equivalent one-byte primitives flip the cipher allowlist
 * head or replace the CA chain root pointer (a downgrade attack
 * against handshakes).  Sealing the policy struct turns any such
 * write into SIGSEGV instead of a quiet protocol downgrade.
 *
 * Per the audit, mbedtls treats `mbedtls_ssl_config` as const after
 * mbedtls_ssl_setup() — per-session state lives in mbedtls_ssl_context
 * (NOT here).  Cert and key parse trees are likewise immutable after
 * load.  Deep allocations inside these structures (cert chain link
 * list nodes) come from mbedTLS's own allocator and stay heap-
 * resident; the seal only protects the top-level fields.  Worthwhile
 * because the top-level fields are where the highest-value control-
 * flow + policy bytes live.
 */
typedef struct {
    mbedtls_ssl_config    conf;
    mbedtls_x509_crt      cert;       /* server/client certificate */
    mbedtls_pk_context    pkey;       /* private key */
    mbedtls_x509_crt      ca_cert;    /* CA cert for peer verification */
    int                   is_server;  /* 1 = server, 0 = client */
    int                   has_ca;     /* 1 = ca_cert loaded */
} KlMbedtlsCtxPolicy;

typedef struct {
    /* Pointer to policy struct, which lives in policy_arena.
     * Sealed RO after _create completes; reads only. */
    KlMbedtlsCtxPolicy       *policy;
    /* RNG state — MUST stay mutable.  Every random draw updates
     * the DRBG's internal state; sealing would break it. */
    mbedtls_ctr_drbg_context  drbg;
    mbedtls_entropy_context   entropy;
    KlAllocator              *alloc;
    /* Backs *policy.  Initialised RW by _create, mprotect-RO at
     * the end of _create / _client_create.  Destroyed by
     * _ctx_destroy. */
    ShSealArena               policy_arena;
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

    int ret = mbedtls_ssl_setup(&t->ssl, &mctx->policy->conf);
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

KlTlsCtx *kl_tls_mbedtls_ctx_create(const char *cert_path,
                                      const char *key_path,
                                      const char *ca_path,
                                      int client_auth,
                                      KlAllocator *alloc)
{
    if (!cert_path || !key_path || !alloc)
        return NULL;

    KlMbedtlsCtx *ctx = kl_malloc(alloc, sizeof(*ctx));
    if (!ctx)
        return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;

    /* Allocate the policy struct in a per-context seal arena.  The
     * arena starts RW so mbedtls_*_init / mbedtls_ssl_conf_* calls
     * below can populate fields normally; we seal at the end of the
     * success path. */
    if (sh_seal_arena_init(&ctx->policy_arena, sizeof(KlMbedtlsCtxPolicy),
                            "kl-tls-policy") != 0) {
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }
    ctx->policy = sh_seal_arena_alloc(&ctx->policy_arena,
                                        sizeof(KlMbedtlsCtxPolicy),
                                        _Alignof(KlMbedtlsCtxPolicy));
    if (!ctx->policy) {
        sh_seal_arena_destroy(&ctx->policy_arena);
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }
    memset(ctx->policy, 0, sizeof(*ctx->policy));
    ctx->policy->is_server = 1;

    /* Initialize all mbedTLS structures */
    mbedtls_ssl_config_init(&ctx->policy->conf);
    mbedtls_x509_crt_init(&ctx->policy->cert);
    mbedtls_pk_init(&ctx->policy->pkey);
    mbedtls_x509_crt_init(&ctx->policy->ca_cert);
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
    unsigned char *cert_buf = read_file(cert_path, &cert_len, alloc);
    if (!cert_buf)
        goto fail;

    ret = mbedtls_x509_crt_parse(&ctx->policy->cert, cert_buf, cert_len);
    kl_free(alloc, cert_buf, cert_len);
    if (ret != 0)
        goto fail;

    /* Load server private key */
    size_t key_len;
    unsigned char *key_buf = read_file(key_path, &key_len, alloc);
    if (!key_buf)
        goto fail;

    ret = mbedtls_pk_parse_key(&ctx->policy->pkey, key_buf, key_len,
                                NULL, 0, mbedtls_ctr_drbg_random, &ctx->drbg);
    kl_secure_zero(key_buf, key_len);
    kl_free(alloc, key_buf, key_len);
    if (ret != 0)
        goto fail;

    /* Load CA certificate for mTLS (optional) */
    if (ca_path) {
        size_t ca_len;
        unsigned char *ca_buf = read_file(ca_path, &ca_len, alloc);
        if (!ca_buf)
            goto fail;

        ret = mbedtls_x509_crt_parse(&ctx->policy->ca_cert, ca_buf, ca_len);
        kl_free(alloc, ca_buf, ca_len);
        if (ret != 0)
            goto fail;

        ctx->policy->has_ca = 1;
    }

    /* Configure SSL */
    ret = mbedtls_ssl_config_defaults(&ctx->policy->conf,
                                       MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        goto fail;

    mbedtls_ssl_conf_rng(&ctx->policy->conf, mbedtls_ctr_drbg_random, &ctx->drbg);
    mbedtls_ssl_conf_ca_chain(&ctx->policy->conf, ctx->policy->has_ca ? &ctx->policy->ca_cert : NULL, NULL);

    ret = mbedtls_ssl_conf_own_cert(&ctx->policy->conf, &ctx->policy->cert, &ctx->policy->pkey);
    if (ret != 0)
        goto fail;

    /* mTLS: set client authentication mode */
    switch (client_auth) {
    case KL_MTLS_OPTIONAL:
        mbedtls_ssl_conf_authmode(&ctx->policy->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        break;
    case KL_MTLS_REQUIRED:
        mbedtls_ssl_conf_authmode(&ctx->policy->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        break;
    default:
        mbedtls_ssl_conf_authmode(&ctx->policy->conf, MBEDTLS_SSL_VERIFY_NONE);
        break;
    }

    /* Policy fully populated.  Seal the arena RO — any heap-write
     * primitive that lands on conf.authmode, the cipher allowlist,
     * the CA chain head, the cert chain head, or any other field
     * faults instead of silently downgrading TLS. */
    if (sh_seal_arena_seal(&ctx->policy_arena) != 0)
        goto fail;

    return (KlTlsCtx *)ctx;

fail:
    if (ctx->policy) {
        mbedtls_ssl_config_free(&ctx->policy->conf);
        mbedtls_x509_crt_free(&ctx->policy->cert);
        mbedtls_pk_free(&ctx->policy->pkey);
        mbedtls_x509_crt_free(&ctx->policy->ca_cert);
    }
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    sh_seal_arena_destroy(&ctx->policy_arena);
    kl_free(alloc, ctx, sizeof(*ctx));
    return NULL;
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

    /* Allocate the policy struct in a per-context seal arena.
     * See kl_tls_mbedtls_ctx_create for the rationale. */
    if (sh_seal_arena_init(&ctx->policy_arena, sizeof(KlMbedtlsCtxPolicy),
                            "kl-tls-policy") != 0) {
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }
    ctx->policy = sh_seal_arena_alloc(&ctx->policy_arena,
                                        sizeof(KlMbedtlsCtxPolicy),
                                        _Alignof(KlMbedtlsCtxPolicy));
    if (!ctx->policy) {
        sh_seal_arena_destroy(&ctx->policy_arena);
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }
    memset(ctx->policy, 0, sizeof(*ctx->policy));
    ctx->policy->is_server = 0;

    /* Initialize all mbedTLS structures */
    mbedtls_ssl_config_init(&ctx->policy->conf);
    mbedtls_x509_crt_init(&ctx->policy->cert);
    mbedtls_pk_init(&ctx->policy->pkey);
    mbedtls_x509_crt_init(&ctx->policy->ca_cert);
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
        ret = mbedtls_x509_crt_parse(&ctx->policy->ca_cert, ca_buf, ca_len);
        if (ret != 0)
            goto fail;
        ctx->policy->has_ca = 1;
    }

    /* Configure as TLS client */
    ret = mbedtls_ssl_config_defaults(&ctx->policy->conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        goto fail;

    mbedtls_ssl_conf_rng(&ctx->policy->conf, mbedtls_ctr_drbg_random, &ctx->drbg);

    if (ctx->policy->has_ca) {
        mbedtls_ssl_conf_ca_chain(&ctx->policy->conf, &ctx->policy->ca_cert, NULL);
        mbedtls_ssl_conf_authmode(&ctx->policy->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* WARNING: No CA provided — TLS certificate verification DISABLED.
         * Connections are encrypted but vulnerable to MITM attacks.
         * Production deployments MUST provide a CA bundle. */
        mbedtls_ssl_conf_authmode(&ctx->policy->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    /* Policy fully populated.  Seal RO. */
    if (sh_seal_arena_seal(&ctx->policy_arena) != 0)
        goto fail;

    return (KlTlsCtx *)ctx;

fail:
    if (ctx->policy) {
        mbedtls_ssl_config_free(&ctx->policy->conf);
        mbedtls_x509_crt_free(&ctx->policy->cert);
        mbedtls_pk_free(&ctx->policy->pkey);
        mbedtls_x509_crt_free(&ctx->policy->ca_cert);
    }
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    sh_seal_arena_destroy(&ctx->policy_arena);
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

    /* mbedtls_*_free traverses internal heap-allocated chains
     * (cert.next list, cipher suite array, etc.) AND zeroes the
     * root struct on exit (mbedtls_platform_zeroize).  The root
     * struct here lives in our sealed arena — RO — so zeroing
     * would fault.  Copy the policy out to a stack-local first;
     * the copy aliases the same heap chains, so mbedtls_*_free
     * correctly releases the chains, then we destroy the arena. */
    if (ctx->policy) {
        KlMbedtlsCtxPolicy local = *ctx->policy;
        mbedtls_ssl_config_free(&local.conf);
        mbedtls_x509_crt_free(&local.cert);
        mbedtls_pk_free(&local.pkey);
        mbedtls_x509_crt_free(&local.ca_cert);
    }
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    sh_seal_arena_destroy(&ctx->policy_arena);
    kl_free(ctx->alloc, ctx, sizeof(*ctx));
}
