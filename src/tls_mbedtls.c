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
#include <mbedtls/rsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform.h>

#include <stdint.h>
#include <stdlib.h>

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

/* ── mbedTLS deep allocator seal: per-context arena hook ──────────
 *
 * Extends v2.3.3's top-level policy seal to also cover the deep heap
 * allocations mbedTLS makes during cert/pk/CA parse — the parsed DER
 * bytes, bignum limbs for RSA moduli + EC group points, cert chain
 * link nodes, etc.  Two pieces:
 *
 *   1. Allocator hook via mbedtls_platform_set_calloc_free.  While
 *      g_current_arena is non-NULL (set transiently by the swap
 *      around the parse-and-init body of client_ctx_create_from_mem),
 *      every mbedtls_calloc bumps from the per-context sh_seal_arena.
 *      When g_current_arena is NULL (every other time, including
 *      per-connection handshake allocations), calloc/free behave
 *      like heap calloc/free.  The free hook is permanently
 *      installed — it walks a registry of live arena ranges and
 *      no-ops for pointers inside any range; otherwise heap free.
 *
 *   2. Pre-warm walker (kl_mbed_prewarm_chain) drives mbedTLS's two
 *      verify-time lazy caches BEFORE seal so the first handshake
 *      doesn't fault on an mprotect-RO page:
 *        - RSA `ctx->RN` (Montgomery R^2 mod N) — written lazily by
 *          mbedtls_mpi_exp_mod on first call.  Pre-warm by calling
 *          mbedtls_rsa_public(rsa, zeros, output) once per pubkey.
 *        - ECP `grp->T` (precomputed comb table for the group's
 *          generator G) — written lazily by mbedtls_ecp_mul on first
 *          mul-by-G.  Pre-warm by calling mbedtls_ecp_mul(grp, R, 1,
 *          G, drbg) once per group.
 *
 * Scope: client_ctx_create_from_mem (HTTPS-out CA chain) only.  The
 * server path (kl_tls_mbedtls_ctx_create) CANNOT be deep-sealed
 * because rsa_prepare_blinding rewrites ctx->Vi / ctx->Vf on every
 * sign as a documented timing-attack defense.  Server stays on the
 * v2.3.3 shallow seal — see ctx->deep_sealed for the destroy
 * branch.
 *
 * Single-threaded boot invariant: ctx_create runs on the main thread
 * before the event loop starts.  The g_current_arena swap is plain
 * assignment.  The free hook is thread-safe (read-only registry walk
 * + heap free fallback); registry mutations are guarded by the same
 * boot-time invariant. */

typedef struct KlMbedArenaNode {
    struct ShSealArena *arena;
    struct KlMbedArenaNode *next;
} KlMbedArenaNode;

static struct ShSealArena *g_current_arena   = NULL;
static KlMbedArenaNode    *g_arena_registry  = NULL;
static int                 g_hook_installed  = 0;

static int kl_mbed_ptr_in_arena(const ShSealArena *a, const void *p)
{
    if (!a || !a->base) return 0;
    uintptr_t lo = (uintptr_t)a->base;
    uintptr_t hi = lo + a->capacity;
    uintptr_t pp = (uintptr_t)p;
    return pp >= lo && pp < hi;
}

static void *kl_mbed_arena_calloc(size_t n, size_t sz)
{
    if (n != 0 && sz > (size_t)-1 / n) return NULL;
    size_t total = n * sz;
    if (g_current_arena) {
        if (total == 0) total = 1;
        void *p = sh_seal_arena_alloc(g_current_arena, total, 16);
        if (!p) return NULL;
        memset(p, 0, total);
        return p;
    }
    return calloc(n, sz);
}

static void kl_mbed_arena_free(void *p)
{
    if (!p) return;
    for (KlMbedArenaNode *n = g_arena_registry; n; n = n->next) {
        if (kl_mbed_ptr_in_arena(n->arena, p)) return;
    }
    free(p);
}

static void kl_mbed_hook_install_once(void)
{
    if (g_hook_installed) return;
    mbedtls_platform_set_calloc_free(kl_mbed_arena_calloc, kl_mbed_arena_free);
    g_hook_installed = 1;
}

static int kl_mbed_arena_register(ShSealArena *arena)
{
    KlMbedArenaNode *node = (KlMbedArenaNode *)malloc(sizeof(*node));
    if (!node) return -1;
    node->arena = arena;
    node->next  = g_arena_registry;
    g_arena_registry = node;
    return 0;
}

static void kl_mbed_arena_unregister(ShSealArena *arena)
{
    KlMbedArenaNode **pp = &g_arena_registry;
    while (*pp) {
        if ((*pp)->arena == arena) {
            KlMbedArenaNode *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Pre-warm every mbedTLS verify-time lazy cache on the supplied cert
 * chain so the seal applied to its containing arena doesn't fault on
 * the first real verify.  Two known caches:
 *
 *   RSA pubkey → ctx->RN (Montgomery R^2 mod N).  Driven by an
 *     mbedtls_rsa_public call.  RSA(0) is meaningless cipher-wise
 *     but populates RN as a side effect.
 *
 *   EC pubkey → grp->T (precomputed comb table for generator G).
 *     Driven by ecp_mul of G by any scalar.  Computed once per
 *     group; on real verify, ECDSA computes u1*G and reuses T.
 *
 * Returns 0 on success, -1 on hard failure (arena OOM, ECP failure,
 * etc.).  RSA(0) may return MBEDTLS_ERR_RSA_PUBLIC_FAILED for keys
 * where 0 is special — that's not a hard failure because RN is
 * populated before the failure return.
 *
 * Returns 0 even for chains with no recognisable pubkey types — the
 * unknown-pk-type case just skips pre-warm for that cert; if mbedTLS
 * lazily writes through it during verify the seal will catch the
 * regression and surface it as a test failure, which is what we
 * want. */
static int kl_mbed_prewarm_chain(mbedtls_x509_crt *chain,
                                  mbedtls_ctr_drbg_context *drbg)
{
    for (mbedtls_x509_crt *c = chain; c != NULL; c = c->next) {
        mbedtls_pk_type_t t = mbedtls_pk_get_type(&c->pk);
        if (t == MBEDTLS_PK_RSA) {
            mbedtls_rsa_context *rsa = mbedtls_pk_rsa(c->pk);
            size_t klen = mbedtls_rsa_get_len(rsa);
            unsigned char *input  = calloc(1, klen);
            unsigned char *output = calloc(1, klen);
            if (!input || !output) {
                free(input); free(output);
                return -1;
            }
            /* Return value intentionally discarded — see doc-comment. */
            (void)mbedtls_rsa_public(rsa, input, output);
            free(input); free(output);
        } else if (t == MBEDTLS_PK_ECKEY || t == MBEDTLS_PK_ECDSA) {
            mbedtls_ecp_keypair *kp = mbedtls_pk_ec(c->pk);
            mbedtls_ecp_group *grp = &kp->MBEDTLS_PRIVATE(grp);
            mbedtls_ecp_point R;
            mbedtls_mpi one;
            mbedtls_ecp_point_init(&R);
            mbedtls_mpi_init(&one);
            int r = mbedtls_mpi_lset(&one, 1);
            if (r == 0) {
                r = mbedtls_ecp_mul(grp, &R, &one, &grp->G,
                                     mbedtls_ctr_drbg_random, drbg);
            }
            mbedtls_ecp_point_free(&R);
            mbedtls_mpi_free(&one);
            if (r != 0) return -1;
        }
    }
    return 0;
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
    /* 1 = client ctx with deep mbedTLS allocator seal in effect
     *     (every mbedtls_calloc during _create landed in policy_arena,
     *     arena owns ALL the deep heap chains, destroy skips
     *     mbedtls_*_free and just sh_seal_arena_destroy's wholesale).
     * 0 = server ctx with v2.3.3 shallow seal (top-level structs in
     *     arena, deep allocations on heap; destroy must call
     *     mbedtls_*_free via the stack-local pattern to release the
     *     heap chains AND avoid faulting on zero-on-sealed-page).
     * Set by _ctx_create / client_ctx_create_from_mem on the success
     * path; consumed by _ctx_destroy. */
    int                       deep_sealed;
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

    /* Arena sizing for client deep seal.
     *
     * Measured high-water from the pre-warm walker (mbedtls_rsa_public
     * for each RSA pubkey + mbedtls_ecp_mul for each EC pubkey):
     *   - Mozilla bundle (226 KB / 145 CAs, 101 RSA + 44 EC): 37 MB
     *   - macOS system store (333 KB / 128 CAs, 105 RSA + 23 EC): 19 MB
     *
     * EC pre-warm dominates (~600 KB per pubkey — comb table +
     * scratch).  RSA is cheap (~25 KB per pubkey — RR cache).
     * Without knowing the RSA/EC mix upfront, size from ca_len with
     * a ~300x factor (covers an all-EC worst case with margin).
     *
     * This is VIRTUAL address space; physical pages are only paid
     * for as pre-warm touches them.  Post-seal high-water for
     * Mozilla is 37 MB → that's the real RAM cost. */
    size_t arena_size = sizeof(KlMbedtlsCtxPolicy)
                      + ca_len * 300
                      + 32 * 1024 * 1024;
    if (sh_seal_arena_init(&ctx->policy_arena, arena_size,
                            "kl-tls-policy") != 0) {
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }
    if (kl_mbed_arena_register(&ctx->policy_arena) != 0) {
        sh_seal_arena_destroy(&ctx->policy_arena);
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }
    kl_mbed_hook_install_once();

    ctx->policy = sh_seal_arena_alloc(&ctx->policy_arena,
                                        sizeof(KlMbedtlsCtxPolicy),
                                        _Alignof(KlMbedtlsCtxPolicy));
    if (!ctx->policy) {
        kl_mbed_arena_unregister(&ctx->policy_arena);
        sh_seal_arena_destroy(&ctx->policy_arena);
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }
    memset(ctx->policy, 0, sizeof(*ctx->policy));
    ctx->policy->is_server = 0;

    /* ── Arena swap opens ──────────────────────────────────────── */
    g_current_arena = &ctx->policy_arena;

    mbedtls_ssl_config_init(&ctx->policy->conf);
    mbedtls_x509_crt_init(&ctx->policy->cert);
    mbedtls_pk_init(&ctx->policy->pkey);
    mbedtls_x509_crt_init(&ctx->policy->ca_cert);
    mbedtls_ctr_drbg_init(&ctx->drbg);
    mbedtls_entropy_init(&ctx->entropy);

    int ret;

    ret = mbedtls_ctr_drbg_seed(&ctx->drbg, mbedtls_entropy_func,
                                 &ctx->entropy,
                                 (const unsigned char *)"keel-client", 11);
    if (ret != 0) goto fail;

    if (ca_buf && ca_len > 0) {
        ret = mbedtls_x509_crt_parse(&ctx->policy->ca_cert, ca_buf, ca_len);
        if (ret != 0) goto fail;
        ctx->policy->has_ca = 1;
    }

    ret = mbedtls_ssl_config_defaults(&ctx->policy->conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) goto fail;

    mbedtls_ssl_conf_rng(&ctx->policy->conf, mbedtls_ctr_drbg_random, &ctx->drbg);

    if (ctx->policy->has_ca) {
        mbedtls_ssl_conf_ca_chain(&ctx->policy->conf, &ctx->policy->ca_cert, NULL);
        mbedtls_ssl_conf_authmode(&ctx->policy->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&ctx->policy->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    /* PRE-WARM: drive every known lazy cache mbedTLS populates on
     * first verify so the seal doesn't fault on the first handshake.
     * See kl_mbed_prewarm_chain doc for what's covered. */
    if (ctx->policy->has_ca) {
        if (kl_mbed_prewarm_chain(&ctx->policy->ca_cert, &ctx->drbg) != 0)
            goto fail;
    }

    /* ── Arena swap closes ─────────────────────────────────────── */
    g_current_arena = NULL;

    if (sh_seal_arena_seal(&ctx->policy_arena) != 0) {
        kl_mbed_arena_unregister(&ctx->policy_arena);
        sh_seal_arena_destroy(&ctx->policy_arena);
        mbedtls_ctr_drbg_free(&ctx->drbg);
        mbedtls_entropy_free(&ctx->entropy);
        kl_free(alloc, ctx, sizeof(*ctx));
        return NULL;
    }

    ctx->deep_sealed = 1;
    return (KlTlsCtx *)ctx;

fail:
    g_current_arena = NULL;
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    kl_mbed_arena_unregister(&ctx->policy_arena);
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

    if (ctx->deep_sealed) {
        /* Deep seal (client ctx, v2.5.0+): all of conf / cert / pkey
         * / ca_cert plus EVERY deep heap chain lives in the sealed
         * arena because mbedtls_calloc was hooked to it during the
         * swap window.  Skip mbedtls_*_free — they'd traverse arena
         * pointers (harmless via our free hook's no-op path) AND
         * try to zero the sealed root struct on exit (faults).
         * Arena destroy reclaims wholesale. */
    } else {
        /* Shallow seal (server ctx, v2.3.3-style): top-level structs
         * in arena, deep chains on heap.  Must call mbedtls_*_free to
         * release the heap chains.  Stack-local copy avoids the
         * zero-on-sealed-root-struct fault. */
        if (ctx->policy) {
            KlMbedtlsCtxPolicy local = *ctx->policy;
            mbedtls_ssl_config_free(&local.conf);
            mbedtls_x509_crt_free(&local.cert);
            mbedtls_pk_free(&local.pkey);
            mbedtls_x509_crt_free(&local.ca_cert);
        }
    }
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    kl_mbed_arena_unregister(&ctx->policy_arena);
    sh_seal_arena_destroy(&ctx->policy_arena);
    kl_free(ctx->alloc, ctx, sizeof(*ctx));
}
