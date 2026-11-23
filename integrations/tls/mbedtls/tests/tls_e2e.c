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
 * The mbedTLS adapter has NO client-cert setter and NO
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

#include <keel/event_ctx.h>
#include <keel/http_client.h>
#include <keel/http_server.h>
#include <keel/async.h>

#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
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

/* No CA bundle must not mean "no verification". kl_tls_mbedtls_client_ctx_create(NULL) built a
 * client that accepted any certificate (VERIFY_NONE), where the OpenSSL adapter verifies against the
 * system store: a caller that forgot the CA path got a silently unauthenticated client. mbedTLS has
 * no system store to fall back on, so a NULL path is refused. The check records its result and the
 * suite fails at the end, so every other test still reports. */
static int g_null_ca_fail;
static void test_null_ca_fails_closed(KlAllocator *alloc)
{
    printf("== NULL CA path -> no context (fail closed) ==\n");
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create(NULL, alloc);
    if (cctx) {
        printf("  FAIL: a NULL CA path made a client that verifies nothing\n");   /* was */
        g_null_ca_fail = 1;
        kl_tls_mbedtls_ctx_destroy(cctx);
        return;
    }
    printf("  PASS: a NULL CA path is refused\n");

    /* The explicit opt-in still makes a working, unverified client. */
    KlTlsCtx *ictx = kl_tls_mbedtls_client_ctx_create_insecure(alloc);
    if (!ictx) {
        printf("  FAIL: the insecure constructor made no context\n");
        g_null_ca_fail = 1;
        return;
    }
    kl_tls_mbedtls_ctx_destroy(ictx);
    printf("  PASS: kl_tls_mbedtls_client_ctx_create_insecure makes a context\n");
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

/* Audit 18 tests record a failure and go on (the suite asserts elsewhere), so every one of
 * them reports its result in one run; main fails at the end if any did. */
static int g_a18_fail;
#define A18_CHECK(cond, what) do { if (!(cond)) { printf("  FAIL: %s\n", what); \
        g_a18_fail++; } } while (0)

/* ── Audit 18 I3: a TLS write retried the way KlDrain retries it ─────────────────────────────
 * After a write would block, KlDrain copies what was not taken into its own buffer, may append more,
 * and retries from that buffer: a different pointer and a larger length. The adapter must keep the
 * write-retry contract under that (OpenSSL: a moved buffer is a fatal BAD_WRITE_RETRY unless allowed;
 * mbedTLS: a larger retry length was acknowledged although only the pending record was sent). The
 * reader must get every byte, in order. */
static void test_drain_style_retry(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Audit 18 I3: drain-style write retry (moved buffer, larger length) ==\n");
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
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    int small = 8192;
    (void)setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    (void)setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    int cfd = fds[0], sfd = fds[1];
    assert(drive_handshake(cli, cfd, srv, sfd) == 0);

    const size_t total = 512 * 1024, piece = 16 * 1024;
    unsigned char *src = malloc(total), *got = malloc(total), *dbuf = malloc(total);
    assert(src && got && dbuf);
    for (size_t i = 0; i < total; i++) src[i] = (unsigned char)(i * 131 + (i >> 9));
    size_t produced = 0, dlen = 0, glen = 0;
    int failed = 0;
    for (int iter = 0; iter < 200000 && glen < total && !failed; iter++) {
        if (dlen > 0) {                                   /* flush the drain, as kl_drain_flush */
            kl_ssize_t n = srv->write(srv, sfd, dbuf, dlen);
            if (n < 0) { failed = 1; break; }
            if (n > 0) { memmove(dbuf, dbuf + n, dlen - (size_t)n); dlen -= (size_t)n; }
        } else if (produced < total) {                    /* a new write, as kl_drain_write */
            size_t len = total - produced < piece ? total - produced : piece;
            kl_ssize_t n = srv->write(srv, sfd, src + produced, len);
            if (n < 0) { failed = 1; break; }
            memcpy(dbuf, src + produced + n, len - (size_t)n);   /* the rest goes to the drain */
            dlen = len - (size_t)n;
            produced += len;
        }
        if (dlen > 0 && produced < total) {               /* more appended behind the stall */
            size_t len = total - produced < piece ? total - produced : piece;
            memcpy(dbuf + dlen, src + produced, len);
            dlen += len;
            produced += len;
        }
        for (int k = 0; k < 8; k++) {                     /* the reader drains a little */
            kl_ssize_t r = cli->read(cli, cfd, got + glen, total - glen);
            if (r > 0) glen += (size_t)r; else break;
        }
    }
    int intact = !failed && glen == total && memcmp(got, src, total) == 0;
    printf("  wrote %zu, read %zu, failed=%d, intact=%d\n", produced, glen, failed, intact);
    A18_CHECK(!failed, "a drain-style retry failed (I3; OpenSSL: BAD_WRITE_RETRY)");
    A18_CHECK(intact, "bytes were lost or reordered (I3; mbedTLS)");
    printf("  PASS: every byte arrived, in order\n");
    free(src); free(got); free(dbuf);
    cli->destroy(cli); srv->destroy(srv);
    close(fds[0]); close(fds[1]);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

/* ── Audit 18 I4: a bare TCP close is not a clean TLS close ──────────────────────────────────
 * The socket-BIO mapped a 0-byte recv to close_notify, so at_eof() reported a clean shutdown for a
 * connection that was simply cut (an on-path FIN): a close-delimited response was accepted as whole. */
static void test_truncation_is_not_eof(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Audit 18 I4: TCP FIN without close_notify ==\n");
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(sctx && cctx);
    KlTls *srv = kl_tls_mbedtls_create(sctx, alloc);
    KlTls *cli = kl_tls_mbedtls_create(cctx, alloc);
    assert(srv && cli);
    assert(kl_tls_mbedtls_set_hostname(cli, "server.local") == 0);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    int cfd = fds[0], sfd = fds[1];
    assert(drive_handshake(cli, cfd, srv, sfd) == 0);
    roundtrip(srv, sfd, cli, cfd, "partial body");
    shutdown(sfd, SHUT_WR);                               /* a FIN, no close_notify */
    char buf[64];
    kl_ssize_t r = 0;
    for (int i = 0; i < 100; i++) { r = cli->read(cli, cfd, buf, sizeof buf); if (r != 0) break; }
    printf("  read=%zd at_eof=%d\n", (ssize_t)r, cli->at_eof(cli));
    A18_CHECK(r < 0, "the read after the FIN did not fail (I4)");
    A18_CHECK(cli->at_eof(cli) == 0, "a bare FIN read as a clean TLS close (I4)");   /* was 1 */
    printf("  PASS: a bare FIN is not reported as a clean TLS close\n");
    cli->destroy(cli); srv->destroy(srv);
    close(fds[0]); close(fds[1]);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

/* ── A write retried with less than the blocked write's length ───────────────────────────────
 * After a write would block, mbedTLS has a whole record built from the length it was given. A retry
 * with a shorter length would flush that record and have only the shorter length acknowledged, so
 * the caller would send the rest again. The adapter refuses such a retry (-1); a retry with the
 * same length still just waits. */
static int g_short_retry_fail;
static void test_short_retry_rejected(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== write retry shorter than the blocked write ==\n");
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, pemlen(ca->cert_pem), alloc);
    assert(sctx && cctx);
    KlTls *srv = kl_tls_mbedtls_create(sctx, alloc);
    KlTls *cli = kl_tls_mbedtls_create(cctx, alloc);
    assert(srv && cli);
    assert(kl_tls_mbedtls_set_hostname(cli, "server.local") == 0);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    int small = 8192;
    (void)setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    (void)setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    int cfd = fds[0], sfd = fds[1];
    assert(drive_handshake(cli, cfd, srv, sfd) == 0);

    enum { PIECE = 16 * 1024 };
    static unsigned char data[PIECE];
    memset(data, 'w', sizeof data);
    kl_ssize_t w = 1;
    for (int i = 0; i < 10000 && w > 0; i++)              /* nobody reads: fill until it blocks */
        w = srv->write(srv, sfd, data, PIECE);
    int blocked = w == 0;
    kl_ssize_t same = blocked ? srv->write(srv, sfd, data, PIECE) : -2;
    kl_ssize_t shorter = blocked ? srv->write(srv, sfd, data, 100) : -2;
    printf("  blocked=%d same-length retry=%zd shorter retry=%zd\n", blocked,
           (ssize_t)same, (ssize_t)shorter);
    if (!blocked) { printf("  FAIL: the writes never blocked\n"); g_short_retry_fail++; }
    if (same != 0) { printf("  FAIL: a same-length retry did not keep waiting\n"); g_short_retry_fail++; }
    if (shorter != -1) {                                   /* was 0 or 100 */
        printf("  FAIL: a shorter retry was accepted\n");
        g_short_retry_fail++;
    }
    /* The refused retry leaves the record built from the blocked write inside mbedTLS. A later write
     * would flush it and be acknowledged for its own bytes, none of which went out: once refused,
     * every later write fails. The peer's socket is emptied first, so the flush could succeed. */
    if (shorter == -1) {
        static unsigned char sink[64 * 1024];
        while (read(cfd, sink, sizeof sink) > 0) {}
    }
    kl_ssize_t later = shorter == -1 ? srv->write(srv, sfd, data, PIECE) : -2;
    printf("  write after the refused retry=%zd\n", (ssize_t)later);
    if (later != -1) {                                     /* was the length, or 0 */
        printf("  FAIL: a write after a refused retry was accepted\n");
        g_short_retry_fail++;
    }
    if (!g_short_retry_fail)
        printf("  PASS: a shorter retry is refused, and so is every write after it\n");
    cli->destroy(cli); srv->destroy(srv);
    close(fds[0]); close(fds[1]);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
}

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

/* ── Async HTTP client: a paused body resumes from plaintext held inside mbedTLS ──
 *
 * A real mbedTLS server session sends one HTTP response in a single write, so mbedTLS cuts it into
 * 16 KiB records; the last record is 12000 bytes. The client reads at most 8 KiB per call, so after
 * its final read 3808 bytes of decrypted plaintext stay inside the client's mbedTLS session
 * (mbedtls_ssl_get_bytes_avail) with nothing left on the socket to make it readable. The consumer
 * pauses on every body fragment; the server then holds the connection open and sends nothing more.
 * The body can only complete if kl_http_client_resume delivers what the TLS engine holds without
 * waiting for socket readiness. Checked: plaintext really was pending at a pause, every byte arrives
 * exactly and in order, nothing is delivered while paused, and the request succeeds once. */

#define PAUSE_RECORD   16384
#define PAUSE_TAIL     12000

typedef struct {
    int         lfd;
    KlTlsCtx   *sctx;
    KlAllocator *alloc;
    const char *resp;
    size_t      resp_len;
} PauseSrv;

static void *pause_srv_main(void *arg)
{
    PauseSrv *p = arg;
    struct pollfd pf = { p->lfd, POLLIN, 0 };
    if (poll(&pf, 1, 10000) <= 0) return NULL;
    int fd = accept(p->lfd, NULL, NULL);
    if (fd < 0) return NULL;
    KlTls *t = kl_tls_mbedtls_create(p->sctx, p->alloc);   /* blocking fd: each call waits */
    if (!t) { close(fd); return NULL; }
    KlTlsResult hr;
    int spins = 0;
    while ((hr = t->handshake(t, (KlSocketHandle)fd)) != KL_TLS_OK)
        if (hr == KL_TLS_ERROR || ++spins > 1000) goto out;
    char req[2048];
    size_t got = 0;
    while (got < sizeof req - 1) {                          /* the request headers */
        kl_ssize_t n = t->read(t, (KlSocketHandle)fd, req + got, sizeof req - 1 - got);
        if (n < 0) goto out;
        got += (size_t)n;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    size_t off = 0;
    while (off < p->resp_len) {                             /* ONE logical write: full records */
        kl_ssize_t n = t->write(t, (KlSocketHandle)fd, p->resp + off, p->resp_len - off);
        if (n < 0) goto out;
        off += (size_t)n;
    }
    /* Hold the connection: no more bytes, so no readiness event, until the client closes. */
    pf.fd = fd;
    while (poll(&pf, 1, 10000) > 0) {
        char b[256];
        if (recv(fd, b, sizeof b, 0) <= 0) break;
    }
out:
    t->destroy(t);
    close(fd);
    return NULL;
}

typedef struct {
    KlHttpClient *client;
    KlTls        *tls;           /* the client's session, captured by the factory wrapper */
    char         *got;
    size_t        len;
    int           paused, pauses, pending_at_pause, cb_while_paused, done;
    KlError       err;
} PauseSink;

static PauseSink *g_pause_sink;

static KlTls *pause_tls_factory(KlTlsCtx *ctx, KlAllocator *alloc)
{
    KlTls *t = kl_tls_mbedtls_create(ctx, alloc);
    if (g_pause_sink) g_pause_sink->tls = t;
    return t;
}

static int pause_on_body(const char *d, size_t n, void *ud)
{
    PauseSink *k = ud;
    if (k->paused) k->cb_while_paused = 1;
    memcpy(k->got + k->len, d, n);
    k->len += n;
    k->paused = 1;
    k->pauses++;
    if (k->tls && k->tls->pending(k->tls) > 0)
        k->pending_at_pause++;                  /* decrypted bytes wait inside mbedTLS */
    return KL_HTTP_CLIENT_BODY_PAUSE;
}

static void pause_on_done(KlHttpClient *c, void *ud)
{
    PauseSink *k = ud;
    if (k->paused) k->cb_while_paused = 1;
    k->done++;
    k->err = kl_http_client_last_error(c);
}

static int g_pause_fail;
#define PAUSE_CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_pause_fail++; } } while (0)

static void test_client_pause_drains_tls_pending(KlAllocator *alloc, PemPair *server)
{
    printf("== Async HTTP client: pause/resume with plaintext pending in mbedTLS ==\n");

    /* Headers padded so headers + body = 4 full records + a 12000-byte last record. */
    static char resp[4 * PAUSE_RECORD + PAUSE_TAIL];
    const size_t total = sizeof resp;
    char hdr[128];
    int hl0 = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Length: %06zu\r\n\r\n", (size_t)0);
    size_t body_len = total - (size_t)hl0;
    int hl = snprintf(resp, sizeof resp, "HTTP/1.1 200 OK\r\nContent-Length: %06zu\r\n\r\n", body_len);
    assert(hl == hl0);
    for (size_t i = 0; i < body_len; i++) resp[(size_t)hl + i] = (char)('a' + (i * 7 + i / 251) % 26);

    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_insecure(alloc);   /* identity is not under test */
    assert(sctx && cctx);

    PauseSrv srv = { -1, sctx, alloc, resp, total };
    srv.lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    assert(srv.lfd >= 0 && bind(srv.lfd, (struct sockaddr *)&a, sizeof a) == 0 &&
           listen(srv.lfd, 1) == 0 && getsockname(srv.lfd, (struct sockaddr *)&a, &al) == 0);
    pthread_t tid;
    assert(pthread_create(&tid, NULL, pause_srv_main, &srv) == 0);

    PauseSink k;
    memset(&k, 0, sizeof k);
    k.got = malloc(body_len);
    g_pause_sink = &k;
    KlTlsConfig tc = { .ctx = cctx, .factory = pause_tls_factory, .ctx_destroy = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 15000;
    cfg.tls = &tc;
    KlHttpClientStreamCfg sc;
    memset(&sc, 0, sizeof sc);
    sc.on_body = pause_on_body;
    sc.user_data = &k;
    KlEventCtx ev;
    assert(kl_event_ctx_init(&ev, alloc) == 0);
    char url[64];
    snprintf(url, sizeof url, "https://127.0.0.1:%d/", ntohs(a.sin_port));
    k.client = kl_http_client_start_s(&ev, alloc, &cfg, "GET", url, NULL, 0, NULL, 0, &sc,
                                      pause_on_done, &k);
    PAUSE_CHECK(k.client != NULL);
    for (int i = 0; k.client && i < 100000 && !k.done; i++) {
        (void)kl_event_ctx_run(&ev, 16, 5);
        if (k.paused && !k.done) {
            for (int j = 0; j < 3 && !k.done; j++) (void)kl_event_ctx_run(&ev, 16, 2);
            int before = k.pauses;
            k.paused = 0;
            PAUSE_CHECK(kl_http_client_resume(k.client) == 0);
            PAUSE_CHECK(k.pauses == before);                /* never re-entrant */
        }
    }
    PAUSE_CHECK(k.done == 1);
    PAUSE_CHECK(k.err == KL_ERR_NONE);
    PAUSE_CHECK(k.len == body_len);
    PAUSE_CHECK(k.len == body_len && memcmp(k.got, resp + hl, body_len) == 0);
    PAUSE_CHECK(k.pending_at_pause > 0);
    PAUSE_CHECK(k.cb_while_paused == 0);
    printf("  %zu body bytes in %d paused fragments; plaintext pending in mbedTLS at %d pauses\n",
           k.len, k.pauses, k.pending_at_pause);

    kl_http_client_free(k.client);
    kl_event_ctx_free(&ev);
    pthread_join(tid, NULL);
    close(srv.lfd);
    g_pause_sink = NULL;
    free(k.got);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
    if (!g_pause_fail) printf("  PASS: resume drained TLS-held plaintext with no socket event\n");
}

/* ── Server: a TLS client hangs up while its request is suspended ──
 *
 * A real Keel server over mbedTLS suspends GET /park. The client (a real mbedTLS session) then goes
 * away, once after sending close_notify and once by just dropping TCP. Either way the server must
 * cancel the suspended op exactly once: detection is at the transport, beneath the TLS session, so a
 * close_notify (application data inside a record) is neither read nor needed. Default policy. */

static KlHttpServer g_hup_srv;
static KlAsyncOp g_hup_op;
static volatile int g_hup_parked, g_hup_cancels;
static int g_hup_fail;
#define HUP_CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_hup_fail++; } } while (0)

static void hup_cancel(KlAsyncOp *op, void *ud) { (void)op; (void)ud; g_hup_cancels++; }
static void hup_park(KlHttpRequest *req, KlHttpResponse *res, void *ud)
{
    (void)res; (void)ud;
    memset(&g_hup_op, 0, sizeof g_hup_op);
    g_hup_op.on_cancel = hup_cancel;
    if (kl_async_suspend(&g_hup_srv, kl_http_request_conn(req), &g_hup_op) == 0) g_hup_parked++;
}
static void *hup_srv_main(void *arg) { (void)arg; kl_http_server_run(&g_hup_srv); return NULL; }

static void hup_wait(volatile int *v, int want, int ms)
{
    for (int i = 0; i < ms / 10 && *v < want; i++) usleep(10 * 1000);
}

static void test_server_tls_hangup_while_suspended(KlAllocator *alloc, PemPair *server)
{
    printf("== Server: TLS client hangs up while its request is suspended ==\n");
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, pemlen(server->cert_pem),
        (const unsigned char *)server->key_pem, pemlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_insecure(alloc);
    assert(sctx && cctx);
    KlTlsConfig tc = { .ctx = sctx, .factory = kl_tls_mbedtls_create, .ctx_destroy = NULL };
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.bind_addr = "127.0.0.1";
    cfg.tls = &tc;
    assert(kl_http_server_init(&g_hup_srv, &cfg) == 0);
    kl_http_server_route(&g_hup_srv, "GET", "/park", hup_park, NULL, NULL);
    pthread_t tid;
    assert(pthread_create(&tid, NULL, hup_srv_main, NULL) == 0);
    for (int i = 0; i < 300 && g_hup_srv.bound_port == 0; i++) usleep(10 * 1000);

    static const char req[] = "GET /park HTTP/1.1\r\nHost: server.local\r\n\r\n";
    for (int variant = 0; variant < 2; variant++) {        /* 0: close_notify first; 1: TCP only */
        int before = g_hup_cancels;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)g_hup_srv.bound_port);
        HUP_CHECK(connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
        KlTls *t = kl_tls_mbedtls_create(cctx, alloc);     /* blocking fd: each call waits */
        HUP_CHECK(t != NULL);
        KlTlsResult hr;
        int spins = 0;
        while ((hr = t->handshake(t, (KlSocketHandle)fd)) != KL_TLS_OK && hr != KL_TLS_ERROR && ++spins < 1000) {}
        HUP_CHECK(hr == KL_TLS_OK);
        HUP_CHECK(t->write(t, (KlSocketHandle)fd, req, sizeof req - 1) == (kl_ssize_t)(sizeof req - 1));
        hup_wait(&g_hup_parked, variant + 1, 3000);
        HUP_CHECK(g_hup_parked == variant + 1);
        if (variant == 0) (void)t->shutdown(t, (KlSocketHandle)fd);   /* close_notify */
        t->destroy(t);
        close(fd);
        hup_wait(&g_hup_cancels, before + 1, 3500);
        HUP_CHECK(g_hup_cancels == before + 1);
        printf("  %s: suspended op cancelled %d time(s)\n",
               variant == 0 ? "close_notify + close" : "TCP close, no close_notify", g_hup_cancels - before);
    }
    kl_http_server_stop(&g_hup_srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_hup_srv);
    HUP_CHECK(g_hup_cancels == 2);                         /* free cancelled nothing more */
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
    if (!g_hup_fail) printf("  PASS: suspended op cancelled once per TLS hang-up, with or without close_notify\n");
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
    test_drain_style_retry(&alloc, &ca, &server);
    test_truncation_is_not_eof(&alloc, &ca, &server);
    test_short_retry_rejected(&alloc, &ca, &server);
    test_null_ca_fails_closed(&alloc);
    test_client_pause_drains_tls_pending(&alloc, &server);
    test_server_tls_hangup_while_suspended(&alloc, &server);

    /* Scenarios omitted because the mbedTLS adapter's public API cannot express
     * them (documented rather than faked):
     *   - client-cert mTLS SUCCESS: no client-cert setter on the adapter.
     *   - strict/lenient truncation toggle: no allow_truncation setter.
     *   - TLS 1.1 version-floor rejection: mbedTLS 3.x speaks only TLS 1.2/1.3. */
    printf("\nNOTE: mTLS-success / truncation-toggle / TLS1.1-floor\n"
           "      omitted, not expressible via the mbedTLS adapter's public API.\n");

    mbedtls_pk_free(&ca_key);
    pem_pair_free(&ca);
    pem_pair_free(&server);
    rng_free(&rng);

    if (g_a18_fail) { printf("\nFAIL: %d audit-18 check(s)\n", g_a18_fail); return 1; }
    if (g_null_ca_fail) { printf("\nFAIL: NULL CA path check\n"); return 1; }
    if (g_short_retry_fail) { printf("\nFAIL: shorter write retry check\n"); return 1; }
    if (g_pause_fail) { printf("\nFAIL: %d client pause/resume check(s)\n", g_pause_fail); return 1; }
    if (g_hup_fail) { printf("\nFAIL: %d server hang-up check(s)\n", g_hup_fail); return 1; }
    printf("\nALL PASS: both KlTls transport axes + hardening suite verified\n");
    return 0;
}
