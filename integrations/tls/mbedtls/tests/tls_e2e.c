/*
 * tls_e2e.c: end-to-end proof that the mbedTLS KlTls adapter satisfies BOTH
 * transport axes of the KlTls vtable, plus a hardening/regression suite.
 *
 *   Axis 1 (socket-BIO / readiness): a socketpair() carries ciphertext; client
 *     and server sessions are driven to handshake completion by alternating
 *     handshake/read/write on the two non-blocking fds, then a payload is
 *     round-tripped both directions. Asserts ALPN negotiation and reads back the
 *     server's verified identity via the client's peer_cert().
 *
 *   Axis 2 (memory-BIO / completion): NO sockets. Ciphertext is shuttled purely
 *     via feed_input()/drain_output() between the two sessions until both
 *     handshakes complete; app data is round-tripped through read/write +
 *     feed/drain; then a clean shutdown() is asserted to surface on the peer as
 *     read()==-1 with at_eof()==1.
 *
 * Certificates (a self-signed CA + a server leaf) are generated at runtime with
 * the mbedTLS library API; no filesystem, no `openssl` CLI.
 *
 * This mirrors the OpenSSL suite (integrations/tls/openssl/tests/tls_e2e.c) in
 * structure and style, adapted to the mbedTLS adapter's PUBLIC API surface.
 * The mbedTLS adapter has NO client-cert setter, NO insecure ctor and NO
 * truncation toggle, so those OpenSSL scenarios are intentionally absent; see
 * the notes printed by main() and the comments at each omission.
 *
 * Built under ASan+UBSan. SPDX-License-Identifier: MIT
 */

#include <keel_tls_mbedtls.h>
#include <keel/allocator.h>

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509.h>
#include <mbedtls/oid.h>

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── tiny cert factory (mbedTLS library API) ─────────────────────── */

typedef struct {
    char *cert_pem;   /* leaf cert PEM (NUL-terminated) */
    char *key_pem;    /* leaf private key PEM (NUL-terminated) */
} PemPair;

/* A shared RNG for all certificate generation in this suite. */
typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
} Rng;

static void rng_init(Rng *r) {
    mbedtls_entropy_init(&r->entropy);
    mbedtls_ctr_drbg_init(&r->drbg);
    const char *pers = "keel-mbedtls-e2e";
    assert(mbedtls_ctr_drbg_seed(&r->drbg, mbedtls_entropy_func, &r->entropy,
                                 (const unsigned char *)pers, strlen(pers)) == 0);
}

static void rng_free(Rng *r) {
    mbedtls_ctr_drbg_free(&r->drbg);
    mbedtls_entropy_free(&r->entropy);
}

/* Generate an EC P-256 keypair. Caller owns *out_key (mbedtls_pk_free). */
static void gen_key(Rng *rng, mbedtls_pk_context *out_key) {
    mbedtls_pk_init(out_key);
    assert(mbedtls_pk_setup(out_key,
        mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0);
    assert(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
        mbedtls_pk_ec(*out_key), mbedtls_ctr_drbg_random, &rng->drbg) == 0);
}

/* Duplicate a heap PEM string sized to strlen (used to store DER-vs-PEM output). */
static char *dup_pem(const unsigned char *buf) {
    size_t n = strlen((const char *)buf);
    char *out = malloc(n + 1);
    assert(out);
    memcpy(out, buf, n + 1);
    return out;
}

/*
 * Generate a cert for `cn`. When issuer_key/issuer_dn are NULL the cert is
 * self-signed (a CA, is_ca=1). Otherwise it is a leaf signed by the issuer.
 * A leaf additionally carries a single dNSName SAN equal to `san_dns` (may be
 * NULL to omit). The generated key PEM is written to `out->key_pem`, the cert
 * PEM to `out->cert_pem`. If out_key is non-NULL the caller receives the live
 * pk context (for signing children) and owns it; otherwise it is freed here.
 */
static void gen_cert(Rng *rng, const char *cn, int is_ca,
                     mbedtls_pk_context *issuer_key, const char *issuer_dn,
                     const char *san_dns,
                     PemPair *out, mbedtls_pk_context *out_key)
{
    mbedtls_pk_context key;
    gen_key(rng, &key);

    /* Emit the key PEM. */
    unsigned char keybuf[4096];
    assert(mbedtls_pk_write_key_pem(&key, keybuf, sizeof(keybuf)) == 0);
    out->key_pem = dup_pem(keybuf);

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    char subj[300];
    snprintf(subj, sizeof(subj), "CN=%s", cn);
    assert(mbedtls_x509write_crt_set_subject_name(&crt, subj) == 0);

    const char *iss = issuer_dn ? issuer_dn : subj;
    assert(mbedtls_x509write_crt_set_issuer_name(&crt, iss) == 0);

    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, issuer_key ? issuer_key : &key);

    /* Serial: a small fixed nonzero value (raw big-endian). */
    static unsigned char serial_seed = 1;
    unsigned char serial[1] = { serial_seed++ };
    assert(mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) == 0);

    assert(mbedtls_x509write_crt_set_validity(&crt,
        "20240101000000", "20340101000000") == 0);

    if (is_ca) {
        assert(mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1) == 0);
    } else {
        assert(mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) == 0);
        if (san_dns) {
            mbedtls_x509_san_list san;
            memset(&san, 0, sizeof(san));
            san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
            san.node.san.unstructured_name.tag = MBEDTLS_ASN1_IA5_STRING;
            san.node.san.unstructured_name.p   = (unsigned char *)san_dns;
            san.node.san.unstructured_name.len = strlen(san_dns);
            san.next = NULL;
            assert(mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san) == 0);
        }
    }

    unsigned char crtbuf[4096];
    int ret = mbedtls_x509write_crt_pem(&crt, crtbuf, sizeof(crtbuf),
                                        mbedtls_ctr_drbg_random, &rng->drbg);
    assert(ret == 0);
    out->cert_pem = dup_pem(crtbuf);

    mbedtls_x509write_crt_free(&crt);

    if (out_key) *out_key = key;   /* caller now owns it */
    else mbedtls_pk_free(&key);
}

static void pem_pair_free(PemPair *p) {
    free(p->cert_pem);
    free(p->key_pem);
    p->cert_pem = p->key_pem = NULL;
}

/* mbedTLS PEM ctors require the length to INCLUDE the trailing NUL. */
static size_t pemlen(const char *s) { return strlen(s) + 1; }

/* ── Axis 1: socket-BIO handshake + round-trip ───────────────────── */

static int nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Drive both sessions' handshakes to completion over the two fds. Returns 0. */
static int drive_handshake(KlTls *cli, int cfd, KlTls *srv, int sfd) {
    int cdone = 0, sdone = 0;
    for (int iter = 0; iter < 200 && (!cdone || !sdone); iter++) {
        if (!cdone) {
            KlTlsResult r = cli->handshake(cli, cfd);
            if (r == KL_TLS_OK) cdone = 1;
            else if (r == KL_TLS_ERROR) { fprintf(stderr, "client hs error\n"); return -1; }
        }
        if (!sdone) {
            KlTlsResult r = srv->handshake(srv, sfd);
            if (r == KL_TLS_OK) sdone = 1;
            else if (r == KL_TLS_ERROR) { fprintf(stderr, "server hs error\n"); return -1; }
        }
    }
    return (cdone && sdone) ? 0 : -1;
}

/* Write `msg` from `w` (on wfd), read it back on `r` (on rfd), assert equal. */
static void roundtrip(KlTls *w, int wfd, KlTls *r, int rfd, const char *msg) {
    size_t mlen = strlen(msg);
    size_t sent = 0;
    for (int i = 0; i < 200 && sent < mlen; i++) {
        kl_ssize_t n = w->write(w, wfd, msg + sent, mlen - sent);
        if (n > 0) sent += (size_t)n;
        else if (n < 0) { fprintf(stderr, "write err\n"); exit(1); }
    }
    assert(sent == mlen);

    char buf[512];
    size_t got = 0;
    for (int i = 0; i < 400 && got < mlen; i++) {
        kl_ssize_t n = r->read(r, rfd, buf + got, sizeof(buf) - got);
        if (n > 0) got += (size_t)n;
        else if (n < 0 && !r->at_eof(r)) { fprintf(stderr, "read err\n"); exit(1); }
    }
    assert(got == mlen);
    assert(memcmp(buf, msg, mlen) == 0);
}

static void axis1_socket_bio(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Axis 1: socket-BIO (readiness) ==\n");

    /* Server ctx: leaf + key, no client auth (the mbedTLS adapter has no client
     * cert setter, so plain server auth). ALPN advertises h2 then http/1.1. */
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    const char *sp[] = {"h2", "http/1.1", NULL};
    assert(kl_tls_mbedtls_ctx_set_alpn(sctx, sp) == 0);

    /* Client ctx: trusts the CA (verifies the server). ALPN offers http/1.1
     * then h2 (opposite preference from the server). */
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(cctx);
    const char *cp[] = {"http/1.1", "h2", NULL};
    assert(kl_tls_mbedtls_ctx_set_alpn(cctx, cp) == 0);

    KlTls *srv = kl_tls_mbedtls_create(sctx, alloc);
    KlTls *cli = kl_tls_mbedtls_create(cctx, alloc);
    assert(srv && cli);

    assert(kl_tls_mbedtls_set_hostname(cli, "server.local") == 0);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    int cfd = fds[0], sfd = fds[1];

    assert(drive_handshake(cli, cfd, srv, sfd) == 0);
    printf("  handshake complete\n");

    /* ALPN: mbedTLS selects the SERVER's first advertised protocol that the
     * client also offered; server preference wins → "h2" on both sides. */
    const char *cn_alpn = cli->alpn_protocol(cli);
    const char *sn_alpn = srv->alpn_protocol(srv);
    printf("  ALPN client=%s server=%s\n", cn_alpn ? cn_alpn : "(none)",
           sn_alpn ? sn_alpn : "(none)");
    assert(cn_alpn && strcmp(cn_alpn, "h2") == 0);
    assert(sn_alpn && strcmp(sn_alpn, "h2") == 0);
    printf("  PASS: ALPN negotiated h2 (server preference)\n");

    /* Round-trip both directions. */
    roundtrip(cli, cfd, srv, sfd, "hello from client");
    roundtrip(srv, sfd, cli, cfd, "hello from server");
    printf("  PASS: socket-BIO round-trip both directions\n");

    /* The CLIENT inspects the SERVER cert it just verified. */
    KlPeerCert pc;
    memset(&pc, 0, sizeof(pc));
    int have = cli->peer_cert(cli, &pc);
    assert(have == 0);
    printf("  peer_cert: verified=%d subject_cn='%s' issuer_cn='%s' san='%s'\n",
           pc.verified, pc.subject_cn, pc.issuer_cn, pc.san);
    printf("  peer_cert: fp=%.16s... not_before=%lld not_after=%lld der_len=%zu\n",
           pc.fingerprint_sha256, (long long)pc.not_before,
           (long long)pc.not_after, pc.der_len);
    assert(pc.verified == 1);
    assert(strcmp(pc.subject_cn, "server.local") == 0);
    assert(strstr(pc.san, "DNS:server.local") != NULL);
    assert(pc.der_len > 0 && pc.der != NULL);
    assert(strlen(pc.fingerprint_sha256) == 64);
    printf("  PASS: client peer_cert returns verified server identity\n");

    close(cfd); close(sfd);
    cli->destroy(cli);
    srv->destroy(srv);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

/* ── Axis 2: memory-BIO handshake + round-trip + clean-shutdown EOF ─ */

/* Shuttle all pending ciphertext from `from` into `to`. Returns bytes moved. */
static size_t pump(KlTls *from, KlTls *to) {
    size_t total = 0;
    unsigned char buf[4096];
    for (;;) {
        kl_ssize_t n = from->drain_output(from, buf, sizeof(buf));
        if (n <= 0) break;
        assert(to->feed_input(to, buf, (size_t)n) == 0);
        total += (size_t)n;
        if ((size_t)n < sizeof(buf)) break;
    }
    return total;
}

static void axis2_memory_bio(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Axis 2: memory-BIO (completion) ==\n");

    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(cctx);

    KlTls *srv = kl_tls_mbedtls_create(sctx, alloc);
    KlTls *cli = kl_tls_mbedtls_create(cctx, alloc);
    assert(srv && cli);
    assert(kl_tls_mbedtls_set_hostname(cli, "server.local") == 0);

    /* Enable memory-BIO mode by feeding an empty buffer to each first. */
    assert(cli->feed_input(cli, NULL, 0) == 0);
    assert(srv->feed_input(srv, NULL, 0) == 0);

    /* Drive both handshakes with NO sockets: step each, then pump ciphertext. */
    int cdone = 0, sdone = 0;
    for (int iter = 0; iter < 200 && (!cdone || !sdone); iter++) {
        if (!cdone) {
            KlTlsResult r = cli->handshake(cli, KL_INVALID_SOCKET);
            if (r == KL_TLS_OK) cdone = 1;
            else if (r == KL_TLS_ERROR) { fprintf(stderr, "cli hs err\n"); exit(1); }
        }
        if (!sdone) {
            KlTlsResult r = srv->handshake(srv, KL_INVALID_SOCKET);
            if (r == KL_TLS_OK) sdone = 1;
            else if (r == KL_TLS_ERROR) { fprintf(stderr, "srv hs err\n"); exit(1); }
        }
        pump(cli, srv);
        pump(srv, cli);
    }
    assert(cdone && sdone);
    printf("  handshake complete (no sockets)\n");

    /* App data client → server through read/write + feed/drain. */
    const char *msg = "memory-bio payload";
    size_t mlen = strlen(msg), sent = 0;
    while (sent < mlen) {
        kl_ssize_t n = cli->write(cli, KL_INVALID_SOCKET, msg + sent, mlen - sent);
        assert(n >= 0);
        sent += (size_t)n;
    }
    pump(cli, srv);
    char buf[256]; size_t got = 0;
    for (int i = 0; i < 100 && got < mlen; i++) {
        kl_ssize_t n = srv->read(srv, KL_INVALID_SOCKET, buf + got, sizeof(buf) - got);
        if (n > 0) got += (size_t)n;
    }
    assert(got == mlen && memcmp(buf, msg, mlen) == 0);
    /* Right after a successful read there is no EOF. */
    assert(srv->at_eof(srv) == 0);
    printf("  PASS: memory-BIO round-trip (client->server), at_eof()==0\n");

    /* And server → client. */
    const char *msg2 = "reply over memory-bio";
    size_t m2 = strlen(msg2); sent = 0;
    while (sent < m2) {
        kl_ssize_t n = srv->write(srv, KL_INVALID_SOCKET, msg2 + sent, m2 - sent);
        assert(n >= 0);
        sent += (size_t)n;
    }
    pump(srv, cli);
    got = 0;
    for (int i = 0; i < 100 && got < m2; i++) {
        kl_ssize_t n = cli->read(cli, KL_INVALID_SOCKET, buf + got, sizeof(buf) - got);
        if (n > 0) got += (size_t)n;
    }
    assert(got == m2 && memcmp(buf, msg2, m2) == 0);
    printf("  PASS: memory-BIO round-trip (server->client)\n");

    /* Clean shutdown from server → client's read() must report at_eof(). */
    srv->shutdown(srv, KL_INVALID_SOCKET);
    pump(srv, cli);
    kl_ssize_t rn = cli->read(cli, KL_INVALID_SOCKET, buf, sizeof(buf));
    assert(rn == -1);
    assert(cli->at_eof(cli) == 1);
    printf("  PASS: clean shutdown -> peer read()==-1 && at_eof()==1\n");

    cli->destroy(cli);
    srv->destroy(srv);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

/* ── Handshake helpers for negative/positive completion-mode tests ── */

/* Return 1 iff a client/server handshake COMPLETES (both OK), else 0. Destroys
 * the sessions before returning. Used for negative (must-fail) cases. */
static int mem_handshake_ok(KlAllocator *alloc, KlTlsCtx *sctx, KlTlsCtx *cctx,
                            const char *sni)
{
    KlTls *srv = kl_tls_mbedtls_create(sctx, alloc);
    KlTls *cli = kl_tls_mbedtls_create(cctx, alloc);
    assert(srv && cli);
    if (sni)
        assert(kl_tls_mbedtls_set_hostname(cli, sni) == 0);
    assert(cli->feed_input(cli, NULL, 0) == 0);
    assert(srv->feed_input(srv, NULL, 0) == 0);
    int cdone = 0, sdone = 0, failed = 0;
    for (int iter = 0; iter < 200 && (!cdone || !sdone) && !failed; iter++) {
        if (!cdone) {
            KlTlsResult r = cli->handshake(cli, KL_INVALID_SOCKET);
            if (r == KL_TLS_OK) cdone = 1; else if (r == KL_TLS_ERROR) failed = 1;
        }
        if (!sdone) {
            KlTlsResult r = srv->handshake(srv, KL_INVALID_SOCKET);
            if (r == KL_TLS_OK) sdone = 1; else if (r == KL_TLS_ERROR) failed = 1;
        }
        pump(cli, srv);
        pump(srv, cli);
    }
    int ok = (cdone && sdone && !failed);
    cli->destroy(cli);
    srv->destroy(srv);
    return ok;
}

/* Hostname mismatch → handshake fails (verify name not in the cert SAN). */
static void test_hostname_mismatch(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Hostname mismatch -> handshake fails ==\n");
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(cctx);
    /* Cert SAN is server.local; ask for a different name. */
    int ok = mem_handshake_ok(alloc, sctx, cctx, "wrong.example");
    assert(ok == 0);
    printf("  PASS: mismatched SNI/verify name fails the handshake\n");
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

/* Untrusted CA → verify fails (client trusts a DIFFERENT CA than signed server). */
static void test_untrusted_ca(KlAllocator *alloc, Rng *rng, PemPair *server)
{
    printf("== Untrusted CA -> verify fails ==\n");
    /* A fresh, unrelated CA the client will trust; but the server leaf was
     * signed by the real CA, so verification must fail. */
    PemPair other_ca = {0};
    gen_cert(rng, "Other CA", 1, NULL, NULL, NULL, &other_ca, NULL);

    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)other_ca.cert_pem, pemlen(other_ca.cert_pem), alloc);
    assert(cctx);
    int ok = mem_handshake_ok(alloc, sctx, cctx, "server.local");
    assert(ok == 0);
    printf("  PASS: server cert under an untrusted CA fails verification\n");
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
    pem_pair_free(&other_ca);
}

/* mTLS REQUIRED but client presents no cert → handshake fails.
 * The mbedTLS adapter has NO client-cert setter, so a client can never present
 * one; a REQUIRED server therefore must reject every client. We assert the
 * handshake does NOT both-complete. */
static void test_missing_client_cert(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== mTLS REQUIRED + no client cert -> handshake fails ==\n");
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem),
        KL_MTLS_REQUIRED, alloc);
    assert(sctx);
    /* Client trusts the CA but cannot present a client cert (no adapter API). */
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(cctx);
    int ok = mem_handshake_ok(alloc, sctx, cctx, "server.local");
    assert(ok == 0);
    printf("  PASS: REQUIRED client auth with no client cert fails\n");
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

/* feed_input boundary: a NULL cipher with a non-zero length and an oversized
 * chunk (> internal cap) are both rejected; len==0 is a no-op. */
static void test_feed_input_boundary(KlAllocator *alloc, PemPair *ca)
{
    printf("== feed_input boundary (NULL + oversize rejected) ==\n");
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(cctx);
    KlTls *cli = kl_tls_mbedtls_create(cctx, alloc);
    assert(cli);

    /* Enable completion mode; empty feed is a no-op success. */
    assert(cli->feed_input(cli, NULL, 0) == 0);
    assert(cli->feed_input(cli, NULL, 0) == 0);
    /* NULL buffer with a non-zero length is a caller bug → rejected, no deref. */
    assert(cli->feed_input(cli, NULL, 5) == -1);
    /* Oversized chunk (> 256 KiB cap) is rejected. */
    size_t huge = (size_t)(256 * 1024) + 1;
    unsigned char *blob = calloc(1, huge);
    assert(blob);
    assert(cli->feed_input(cli, blob, huge) == -1);
    free(blob);

    cli->destroy(cli);
    kl_tls_mbedtls_ctx_destroy(cctx);
    printf("  PASS: feed_input rejects NULL-cipher and oversized chunks\n");
}

/* from_buf boundary: a ctor rejects NULL cert/key/CA buffers. */
static void test_from_buf_boundary(KlAllocator *alloc, PemPair *server, PemPair *ca)
{
    printf("== from_buf boundary (NULL / empty PEM rejected) ==\n");
    const unsigned char *cert = (const unsigned char *)server->cert_pem;
    size_t clen = pemlen(server->cert_pem);
    const unsigned char *key = (const unsigned char *)server->key_pem;
    size_t klen = pemlen(server->key_pem);

    /* NULL cert buffer → ctor NULL. */
    assert(kl_tls_mbedtls_ctx_create_from_buf(NULL, 0, key, klen, NULL, 0,
                                              KL_MTLS_NONE, alloc) == NULL);
    /* NULL key buffer → ctor NULL. */
    assert(kl_tls_mbedtls_ctx_create_from_buf(cert, clen, NULL, 0, NULL, 0,
                                              KL_MTLS_NONE, alloc) == NULL);
    /* Zero-length cert buffer → mbedtls_x509_crt_parse fails → ctor NULL. */
    assert(kl_tls_mbedtls_ctx_create_from_buf(cert, 0, key, klen, NULL, 0,
                                              KL_MTLS_NONE, alloc) == NULL);
    /* Zero-length key buffer → mbedtls_pk_parse_key fails → ctor NULL. */
    assert(kl_tls_mbedtls_ctx_create_from_buf(cert, clen, key, 0, NULL, 0,
                                              KL_MTLS_NONE, alloc) == NULL);
    /* Client from_buf: NULL CA → NULL. */
    assert(kl_tls_mbedtls_client_ctx_create_from_buf(NULL, 0, alloc) == NULL);
    /* Client from_buf: non-NULL CA with len 0 → NULL. */
    assert(kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, 0, alloc) == NULL);
    printf("  PASS: from_buf ctors reject empty/NULL PEM buffers\n");
}

/* peer_cert edge cases: NULL out and before-handshake both return -1. */
static void test_peer_cert_edges(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== peer_cert edges (NULL out, pre-handshake) ==\n");
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(cctx);

    /* Before any handshake: peer_cert returns -1 regardless of out. The pre-
     * handshake path short-circuits on !handshake_done before touching `out`,
     * so a NULL out is safe here. */
    KlTls *cli0 = kl_tls_mbedtls_create(cctx, alloc);
    assert(cli0);
    KlPeerCert pc0;
    assert(cli0->peer_cert(cli0, &pc0) == -1);
    assert(cli0->peer_cert(cli0, NULL) == -1);
    printf("  PASS: peer_cert before handshake returns -1 (NULL out safe)\n");
    cli0->destroy(cli0);

    /* After a completed handshake: a NULL out is rejected (no deref) and a real
     * out is filled. */
    KlTls *srv = kl_tls_mbedtls_create(sctx, alloc);
    KlTls *cli = kl_tls_mbedtls_create(cctx, alloc);
    assert(srv && cli);
    assert(kl_tls_mbedtls_set_hostname(cli, "server.local") == 0);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    assert(drive_handshake(cli, fds[0], srv, fds[1]) == 0);
    assert(cli->peer_cert(cli, NULL) == -1);   /* NULL out rejected post-handshake */
    KlPeerCert pc;
    assert(cli->peer_cert(cli, &pc) == 0);
    printf("  PASS: post-handshake peer_cert rejects NULL out, fills a real struct\n");

    close(fds[0]); close(fds[1]);
    cli->destroy(cli); srv->destroy(srv);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

/* reset() + a second full handshake on the SAME sessions round-trips. */
static void test_second_handshake_after_reset(KlAllocator *alloc, PemPair *ca,
                                              PemPair *server)
{
    printf("== second handshake after reset() round-trips ==\n");
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(cctx);

    KlTls *srv = kl_tls_mbedtls_create(sctx, alloc);
    KlTls *cli = kl_tls_mbedtls_create(cctx, alloc);
    assert(srv && cli);

    /* First handshake + round-trip over sockets. */
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    assert(kl_tls_mbedtls_set_hostname(cli, "server.local") == 0);
    assert(drive_handshake(cli, fds[0], srv, fds[1]) == 0);
    roundtrip(cli, fds[0], srv, fds[1], "first handshake payload");
    printf("  first handshake OK\n");

    /* reset() both, close the old fds, and handshake again on fresh sockets. */
    cli->reset(cli);
    srv->reset(srv);
    close(fds[0]); close(fds[1]);

    int fds2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2) == 0);
    assert(nonblock(fds2[0]) == 0 && nonblock(fds2[1]) == 0);
    assert(kl_tls_mbedtls_set_hostname(cli, "server.local") == 0);
    assert(drive_handshake(cli, fds2[0], srv, fds2[1]) == 0);
    roundtrip(cli, fds2[0], srv, fds2[1], "second handshake payload");
    printf("  PASS: second handshake after reset() round-trips\n");

    close(fds2[0]); close(fds2[1]);
    cli->destroy(cli); srv->destroy(srv);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

/* Two simultaneous independent sessions interleaved over two socketpairs. */
static void test_two_simultaneous(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== two simultaneous sessions (interleaved) ==\n");
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(cctx);

    KlTls *srv1 = kl_tls_mbedtls_create(sctx, alloc);
    KlTls *cli1 = kl_tls_mbedtls_create(cctx, alloc);
    KlTls *srv2 = kl_tls_mbedtls_create(sctx, alloc);
    KlTls *cli2 = kl_tls_mbedtls_create(cctx, alloc);
    assert(srv1 && cli1 && srv2 && cli2);
    assert(kl_tls_mbedtls_set_hostname(cli1, "server.local") == 0);
    assert(kl_tls_mbedtls_set_hostname(cli2, "server.local") == 0);

    int a[2], b[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);
    assert(nonblock(a[0]) == 0 && nonblock(a[1]) == 0);
    assert(nonblock(b[0]) == 0 && nonblock(b[1]) == 0);

    assert(drive_handshake(cli1, a[0], srv1, a[1]) == 0);
    assert(drive_handshake(cli2, b[0], srv2, b[1]) == 0);

    /* Interleave two round-trips over the two independent session pairs. */
    roundtrip(cli1, a[0], srv1, a[1], "session one payload");
    roundtrip(cli2, b[0], srv2, b[1], "session two payload");
    roundtrip(srv1, a[1], cli1, a[0], "reply one");
    roundtrip(srv2, b[1], cli2, b[0], "reply two");
    printf("  PASS: two independent sessions round-tripped interleaved\n");

    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    cli1->destroy(cli1); srv1->destroy(srv1);
    cli2->destroy(cli2); srv2->destroy(srv2);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

/* ── Allocation-failure injection ────────────────────────────────── */

typedef struct {
    int fail_after;     /* number of successful allocs before failures begin */
    int count;
} FailAlloc;

static void *fa_malloc(void *ctx, size_t size) {
    FailAlloc *f = ctx;
    if (f->count++ >= f->fail_after) return NULL;
    return malloc(size);
}
static void *fa_realloc(void *ctx, void *ptr, size_t old, size_t nsz) {
    FailAlloc *f = ctx; (void)old;
    if (f->count++ >= f->fail_after) return NULL;
    return realloc(ptr, nsz);
}
static void fa_free(void *ctx, void *ptr, size_t size) { (void)ctx; (void)size; free(ptr); }

static KlAllocator fail_alloc_make(FailAlloc *f, int fail_after) {
    f->fail_after = fail_after;
    f->count = 0;
    KlAllocator a = { .malloc = fa_malloc, .realloc = fa_realloc, .free = fa_free, .ctx = f };
    return a;
}

/* Drive ctx creation + a handshake under an allocator that starts failing after
 * N allocations, for a sweep of N. Each iteration must fail gracefully (no
 * crash / no UAF under ASan); some N complete, some don't; either is fine, the
 * point is memory-safety on the failure paths. */
static void test_alloc_failure_injection(PemPair *ca, PemPair *server)
{
    printf("== allocation-failure injection (graceful, ASan-clean) ==\n");
    for (int n = 0; n < 40; n++) {
        FailAlloc f;
        KlAllocator a = fail_alloc_make(&f, n);

        KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
            (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
            (const unsigned char *)server->key_pem, pemlen(server->key_pem),
            NULL, 0, KL_MTLS_NONE, &a);
        KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
            (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), &a);

        if (sctx && cctx) {
            KlTls *srv = kl_tls_mbedtls_create(sctx, &a);
            KlTls *cli = kl_tls_mbedtls_create(cctx, &a);
            if (srv && cli) {
                if (kl_tls_mbedtls_set_hostname(cli, "server.local") == 0 &&
                    cli->feed_input(cli, NULL, 0) == 0 &&
                    srv->feed_input(srv, NULL, 0) == 0) {
                    int cdone = 0, sdone = 0, failed = 0;
                    for (int it = 0; it < 200 && (!cdone || !sdone) && !failed; it++) {
                        if (!cdone) {
                            KlTlsResult r = cli->handshake(cli, KL_INVALID_SOCKET);
                            if (r == KL_TLS_OK) cdone = 1; else if (r == KL_TLS_ERROR) failed = 1;
                        }
                        if (!sdone) {
                            KlTlsResult r = srv->handshake(srv, KL_INVALID_SOCKET);
                            if (r == KL_TLS_OK) sdone = 1; else if (r == KL_TLS_ERROR) failed = 1;
                        }
                        /* pump; feed/drain ring growth exercises comp_ensure under failure */
                        unsigned char pb[4096];
                        for (;;) {
                            kl_ssize_t k = cli->drain_output(cli, pb, sizeof(pb));
                            if (k <= 0) break;
                            if (srv->feed_input(srv, pb, (size_t)k) != 0) { failed = 1; break; }
                            if ((size_t)k < sizeof(pb)) break;
                        }
                        for (;;) {
                            kl_ssize_t k = srv->drain_output(srv, pb, sizeof(pb));
                            if (k <= 0) break;
                            if (cli->feed_input(cli, pb, (size_t)k) != 0) { failed = 1; break; }
                            if ((size_t)k < sizeof(pb)) break;
                        }
                    }
                    if (cdone && sdone && !failed) {
                        KlPeerCert pc;
                        (void)cli->peer_cert(cli, &pc);
                    }
                }
            }
            if (cli) cli->destroy(cli);
            if (srv) srv->destroy(srv);
        }
        if (sctx) kl_tls_mbedtls_ctx_destroy(sctx);
        if (cctx) kl_tls_mbedtls_ctx_destroy(cctx);
    }
    printf("  PASS: alloc-failure sweep completed without crash/UAF (ASan)\n");
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    Rng rng;
    rng_init(&rng);

    /* CA (self-signed) + a server leaf (CN=server.local, SAN DNS server.local)
     * signed by the CA. The mbedTLS adapter has no client-cert API, so there is
     * no client leaf. */
    PemPair ca = {0}, server = {0};
    mbedtls_pk_context ca_key;
    gen_cert(&rng, "Keel Test CA", 1, NULL, NULL, NULL, &ca, &ca_key);
    gen_cert(&rng, "server.local", 0, &ca_key, "CN=Keel Test CA", "server.local",
             &server, NULL);

    axis1_socket_bio(&alloc, &ca, &server);
    axis2_memory_bio(&alloc, &ca, &server);

    test_hostname_mismatch(&alloc, &ca, &server);
    test_untrusted_ca(&alloc, &rng, &server);
    test_missing_client_cert(&alloc, &ca, &server);
    test_feed_input_boundary(&alloc, &ca);
    test_from_buf_boundary(&alloc, &server, &ca);
    test_peer_cert_edges(&alloc, &ca, &server);
    test_second_handshake_after_reset(&alloc, &ca, &server);
    test_two_simultaneous(&alloc, &ca, &server);
    test_alloc_failure_injection(&ca, &server);

    /* Scenarios omitted because the mbedTLS adapter's public API cannot express
     * them (documented rather than faked):
     *   - client-cert mTLS SUCCESS: no client-cert setter on the adapter.
     *   - insecure (verify-none) client ctx: no insecure ctor.
     *   - strict/lenient truncation toggle: no allow_truncation setter.
     *   - TLS 1.1 version-floor rejection: mbedTLS 3.x speaks only TLS 1.2/1.3. */
    printf("\nNOTE: mTLS-success / insecure-ctx / truncation-toggle / TLS1.1-floor\n"
           "      omitted, not expressible via the mbedTLS adapter's public API.\n");

    mbedtls_pk_free(&ca_key);
    pem_pair_free(&ca);
    pem_pair_free(&server);
    rng_free(&rng);

    printf("\nALL PASS: both KlTls transport axes + hardening suite verified\n");
    return 0;
}
