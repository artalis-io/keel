/*
 * tls_mbedtls.c — mbedTLS backend for Keel's KlTls vtable
 *
 * Implements server-side TLS (with optional mTLS) and client-side TLS
 * using mbedTLS 3.x. All I/O is non-blocking via custom BIO callbacks.
 *
 * SPDX-License-Identifier: MIT
 */

#include "keel_tls_mbedtls.h"

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
    /* Optional socket provider the socket-BIO routes ciphertext through (NULL =
     * host kl_sockdef_*). Set via kl_tls_mbedtls_ctx_set_socket_provider so TLS can
     * run over a non-kernel stack (e.g. lwIP). Inherited by each session. */
    const KlSocketProvider *sp;
    /* ALPN: ctx-owned copy of the advertised (server) / offered (client) protocol
     * list. mbedTLS stores the pointer (does not copy), so it must outlive the
     * config — hence the backing buffer here. NULL-terminated `alpn` array. */
    const char            *alpn[8];
    char                   alpn_buf[256];
} KlMbedtlsCtx;

/* ── Per-connection TLS session ──────────────────────────────────── */

typedef struct {
    KlTls              base;       /* vtable — must be first */
    mbedtls_ssl_context ssl;
    KlMbedtlsCtx      *ctx;       /* shared context (not owned) */
    KlAllocator        *alloc;
    KlSocketHandle      fd;        /* cached for BIO callbacks (socket-BIO mode) */
    const KlSocketProvider *sp;    /* socket provider for BIO I/O (NULL = host default) */
    int                 handshake_done;
    /* Completion (memory-BIO) mode — 8b-5. Active once feed_input() is first called:
     * the BIO reads ciphertext from in_buf (fed by the caller) and appends outgoing
     * ciphertext to out_buf (drained by the caller) instead of the socket fd. */
    int                 comp_mode;
    unsigned char      *in_buf;   size_t in_cap, in_len, in_pos;
    unsigned char      *out_buf;  size_t out_cap, out_len;
} KlMbedtlsTls;

/* Cap on either completion ciphertext ring (a TLS record is <= ~16 KiB; cert-chain
 * handshake flights are larger — 256 KiB is generous, overflow → connection error). */
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

/* ── Custom BIO callbacks (non-blocking I/O) ─────────────────────── */

/*
 * mbedTLS requires custom send/recv callbacks for non-blocking sockets.
 * These translate between mbedTLS error codes and POSIX.
 */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)ctx;
    if (t->comp_mode) {   /* memory BIO: append to the outgoing-ciphertext ring */
        if (comp_ensure(&t->out_buf, &t->out_cap, t->out_len + len, t->alloc) < 0)
            return MBEDTLS_ERR_NET_SEND_FAILED;
        memcpy(t->out_buf + t->out_len, buf, len);
        t->out_len += len;
        return (int)len;
    }
    /* kl_sock_send routes through the configured provider (t->sp) — or, when NULL,
     * kl_sockdef_send: SIGPIPE-suppressed (MSG_NOSIGNAL) + EINTR-retried on POSIX,
     * Winsock send with kl_wsa_set_errno on Windows — so the EAGAIN/EWOULDBLOCK
     * check below works on both. A non-host provider (e.g. lwIP) supplies its own
     * would-block errno. */
    ssize_t ret = kl_sock_send(t->sp, t->fd, buf, len);

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
    if (t->comp_mode) {   /* memory BIO: consume from the fed-ciphertext ring */
        size_t avail = t->in_len - t->in_pos;
        if (avail == 0)
            return MBEDTLS_ERR_SSL_WANT_READ;   /* caller must feed_input more */
        size_t n = len < avail ? len : avail;
        memcpy(buf, t->in_buf + t->in_pos, n);
        t->in_pos += n;
        if (t->in_pos >= t->in_len)
            t->in_pos = t->in_len = 0;
        return (int)n;
    }
    ssize_t ret = kl_sock_recv(t->sp, t->fd, buf, len);

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

/* Completion mode (8b-5): feed received ciphertext to the engine's input ring. */
static int tls_feed_input(KlTls *self, const void *cipher, size_t len)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    t->comp_mode = 1;
    if (t->in_pos > 0) {   /* compact the partially-consumed prefix */
        memmove(t->in_buf, t->in_buf + t->in_pos, t->in_len - t->in_pos);
        t->in_len -= t->in_pos;
        t->in_pos = 0;
    }
    if (len == 0)
        return 0;
    if (comp_ensure(&t->in_buf, &t->in_cap, t->in_len + len, t->alloc) < 0)
        return -1;
    memcpy(t->in_buf + t->in_len, cipher, len);
    t->in_len += len;
    return 0;
}

/* Completion mode: drain the engine's pending outgoing ciphertext. */
static ssize_t tls_drain_output(KlTls *self, void *buf, size_t cap)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    size_t avail = t->out_len;
    size_t n = (avail < cap) ? avail : cap;
    if (n)
        memcpy(buf, t->out_buf, n);
    t->out_len = avail - n;               /* remaining ciphertext */
    if (t->out_len)
        memmove(t->out_buf, t->out_buf + n, t->out_len);
    return (ssize_t)n;
}

static void tls_reset(KlTls *self)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    mbedtls_ssl_session_reset(&t->ssl);
    t->handshake_done = 0;
    t->fd = KL_INVALID_SOCKET;
    /* Drop buffered ciphertext for the next request; keep the transport mode +
     * allocated rings for reuse across keep-alive. */
    t->in_len = t->in_pos = t->out_len = 0;
}

static void tls_destroy(KlTls *self)
{
    KlMbedtlsTls *t = (KlMbedtlsTls *)self;
    mbedtls_ssl_free(&t->ssl);
    kl_free(t->alloc, t->in_buf, t->in_cap);
    kl_free(t->alloc, t->out_buf, t->out_cap);
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
    if (outlen == 0)
        return;   /* defensive: no room even for the NUL */
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
    if (outlen == 0)
        return;   /* defensive: no room even for the NUL */
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

/* KlTls.set_socket_provider — the framework (connection.c/client.c) calls this
 * before the handshake with the connection's provider, overriding any ctx default
 * so the socket-BIO uses the same stack as the connection. */
static void tls_set_socket_provider(KlTls *self, const struct KlSocketProvider *sp)
{
    ((KlMbedtlsTls *)self)->sp = (const KlSocketProvider *)sp;
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
    t->sp = mctx->sp;   /* inherit the ctx's socket provider (NULL = host default) */

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
    t->base.feed_input    = tls_feed_input;      /* completion mode (8b-5) */
    t->base.drain_output  = tls_drain_output;
    t->base.set_socket_provider = tls_set_socket_provider;  /* framework auto-wires the provider */

    /* Completion (memory-BIO) mode starts off; feed_input() enables it. */
    t->comp_mode = 0;
    t->in_buf = t->out_buf = NULL;
    t->in_cap = t->in_len = t->in_pos = 0;
    t->out_cap = t->out_len = 0;

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

int kl_tls_mbedtls_ctx_set_alpn(KlTlsCtx *c, const char **protos)
{
    KlMbedtlsCtx *ctx = (KlMbedtlsCtx *)c;
    if (!ctx || !protos) return -1;

    /* Copy the protocol strings into ctx-owned storage (mbedTLS keeps the
     * pointer, not a copy) so a caller may pass a transient array. */
    size_t off = 0;
    int n = 0;
    for (int i = 0; protos[i]; i++) {
        if (n >= (int)(sizeof(ctx->alpn) / sizeof(ctx->alpn[0])) - 1) return -1;
        size_t l = strlen(protos[i]);
        if (off + l + 1 > sizeof(ctx->alpn_buf)) return -1;
        memcpy(ctx->alpn_buf + off, protos[i], l + 1);
        ctx->alpn[n++] = ctx->alpn_buf + off;
        off += l + 1;
    }
    ctx->alpn[n] = NULL;
    return mbedtls_ssl_conf_alpn_protocols(&ctx->conf, (const char **)ctx->alpn) == 0
               ? 0 : -1;
}

int kl_tls_mbedtls_ctx_set_socket_provider(KlTlsCtx *c, const struct KlSocketProvider *sp)
{
    KlMbedtlsCtx *ctx = (KlMbedtlsCtx *)c;
    if (!ctx) return -1;
    ctx->sp = (const KlSocketProvider *)sp;   /* NULL restores the host default */
    return 0;
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
