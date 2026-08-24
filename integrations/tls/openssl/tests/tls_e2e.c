/*
 * tls_e2e.c: end-to-end proof that the OpenSSL KlTls adapter satisfies BOTH
 * transport axes of the KlTls vtable.
 *
 *   Axis 1 (socket-BIO / readiness): a socketpair() carries ciphertext; client
 *     and server sessions are driven to handshake completion by alternating
 *     handshake/read/write on the two non-blocking fds, then a payload is
 *     round-tripped both directions. Asserts ALPN negotiation (h2/http1.1) and
 *     (with mTLS REQUIRED) that peer_cert() returns the client identity.
 *
 *   Axis 2 (memory-BIO / completion): NO sockets. Ciphertext is shuttled purely
 *     via feed_input()/drain_output() between the two sessions until both
 *     handshakes complete; app data is round-tripped through read/write +
 *     feed/drain; then a clean shutdown() is asserted to surface on the peer as
 *     read()==-1 with at_eof()==1.
 *
 * Certificates (a self-signed CA + a server leaf + a client leaf) are generated
 * at runtime with the OpenSSL library API: no filesystem, no `openssl` CLI.
 *
 * Built under ASan+UBSan. SPDX-License-Identifier: MIT
 */

#include <keel_tls_openssl.h>
#include <keel/allocator.h>

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

/* ── tiny cert factory (library API) ─────────────────────────────── */

typedef struct {
    char *cert_pem;   /* leaf cert (self-signed by `ca`, or self-signed if ca==NULL) */
    char *key_pem;    /* leaf private key */
} PemPair;

static char *bio_to_str(BIO *b) {
    char *data = NULL;
    long n = BIO_get_mem_data(b, &data);
    char *out = malloc((size_t)n + 1);
    assert(out);
    memcpy(out, data, (size_t)n);
    out[n] = '\0';
    return out;
}

/* Generate an EC P-256 key + an X.509 cert for `cn`. If `issuer_key`/`issuer_crt`
 * are NULL the cert is self-signed (a CA); otherwise it is signed by them. */
static void gen_cert(const char *cn, int is_ca,
                     EVP_PKEY *issuer_key, X509 *issuer_crt,
                     PemPair *out, EVP_PKEY **out_key, X509 **out_crt)
{
    /* Portable EC P-256 keygen: EVP_EC_gen() is OpenSSL 3.0-only. The EVP_PKEY_CTX
     * form works across OpenSSL 1.1/3.x, BoringSSL, and LibreSSL, so the same e2e
     * runs against every backend the shared adapter supports. */
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    assert(kctx);
    assert(EVP_PKEY_keygen_init(kctx) == 1);
    assert(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) == 1);
    assert(EVP_PKEY_keygen(kctx, &pkey) == 1);
    EVP_PKEY_CTX_free(kctx);
    assert(pkey);

    X509 *crt = X509_new();
    assert(crt);
    X509_set_version(crt, 2); /* v3 */
    ASN1_INTEGER_set(X509_get_serialNumber(crt), (long)(rand() | 1));
    X509_gmtime_adj(X509_getm_notBefore(crt), -3600);
    X509_gmtime_adj(X509_getm_notAfter(crt), 3600 * 24 * 365);
    X509_set_pubkey(crt, pkey);

    X509_NAME *name = X509_get_subject_name(crt);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *)cn, -1, -1, 0);

    X509_NAME *iname = issuer_crt ? X509_get_subject_name(issuer_crt) : name;
    X509_set_issuer_name(crt, iname);

    /* Extensions via the programmatic X509_add1_ext_i2d, portable across OpenSSL,
     * LibreSSL, and BoringSSL (the CONF-string X509V3_EXT_conf_nid path is absent in
     * BoringSSL). CA basic constraints, or a dNSName SAN for leaves. */
    if (is_ca) {
        BASIC_CONSTRAINTS *bc = BASIC_CONSTRAINTS_new();
        assert(bc);
        bc->ca = 1;
        assert(X509_add1_ext_i2d(crt, NID_basic_constraints, bc, 1 /*critical*/, 0) == 1);
        BASIC_CONSTRAINTS_free(bc);
    } else {
        GENERAL_NAME    *gn  = GENERAL_NAME_new();
        ASN1_IA5STRING  *ia5 = ASN1_IA5STRING_new();
        assert(gn && ia5);
        assert(ASN1_STRING_set(ia5, cn, -1) == 1);
        GENERAL_NAME_set0_value(gn, GEN_DNS, ia5);      /* gn takes ownership of ia5 */
        GENERAL_NAMES *gens = sk_GENERAL_NAME_new_null();
        assert(gens && sk_GENERAL_NAME_push(gens, gn) > 0);
        assert(X509_add1_ext_i2d(crt, NID_subject_alt_name, gens, 0, 0) == 1);
        GENERAL_NAMES_free(gens);                        /* frees gn + ia5 */
    }

    EVP_PKEY *signer = issuer_key ? issuer_key : pkey;
    assert(X509_sign(crt, signer, EVP_sha256()) > 0);

    /* PEM out */
    BIO *cb = BIO_new(BIO_s_mem());
    assert(PEM_write_bio_X509(cb, crt) == 1);
    out->cert_pem = bio_to_str(cb);
    BIO_free(cb);

    BIO *kb = BIO_new(BIO_s_mem());
    assert(PEM_write_bio_PrivateKey(kb, pkey, NULL, NULL, 0, NULL, NULL) == 1);
    out->key_pem = bio_to_str(kb);
    BIO_free(kb);

    if (out_key) *out_key = pkey; else EVP_PKEY_free(pkey);
    if (out_crt) *out_crt = crt; else X509_free(crt);
}

static void pem_pair_free(PemPair *p) { free(p->cert_pem); free(p->key_pem); }

/* Generate a leaf cert signed by (issuer_key/issuer_crt) whose ONLY SAN is an
 * iPAddress (`ip_asc`, an IPv4/IPv6 literal). Used to prove IP-SAN verification.
 * Subject CN is set to `ip_asc` for readability. */
static void gen_cert_ip_san(const char *ip_asc,
                            EVP_PKEY *issuer_key, X509 *issuer_crt,
                            PemPair *out)
{
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    assert(kctx);
    assert(EVP_PKEY_keygen_init(kctx) == 1);
    assert(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) == 1);
    assert(EVP_PKEY_keygen(kctx, &pkey) == 1);
    EVP_PKEY_CTX_free(kctx);
    assert(pkey);

    X509 *crt = X509_new();
    assert(crt);
    X509_set_version(crt, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(crt), (long)(rand() | 1));
    X509_gmtime_adj(X509_getm_notBefore(crt), -3600);
    X509_gmtime_adj(X509_getm_notAfter(crt), 3600 * 24 * 365);
    X509_set_pubkey(crt, pkey);

    X509_NAME *name = X509_get_subject_name(crt);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *)ip_asc, -1, -1, 0);
    X509_set_issuer_name(crt, issuer_crt ? X509_get_subject_name(issuer_crt) : name);

    /* iPAddress SAN: pack the literal into 4 (v4) or 16 (v6) octets. */
    unsigned char raw[16];
    int rawlen = 0;
    if (inet_pton(AF_INET, ip_asc, raw) == 1) rawlen = 4;
    else if (inet_pton(AF_INET6, ip_asc, raw) == 1) rawlen = 16;
    assert(rawlen != 0);

    GENERAL_NAME   *gn  = GENERAL_NAME_new();
    ASN1_OCTET_STRING *os = ASN1_OCTET_STRING_new();
    assert(gn && os);
    assert(ASN1_OCTET_STRING_set(os, raw, rawlen) == 1);
    GENERAL_NAME_set0_value(gn, GEN_IPADD, os);   /* gn takes ownership of os */
    GENERAL_NAMES *gens = sk_GENERAL_NAME_new_null();
    assert(gens && sk_GENERAL_NAME_push(gens, gn) > 0);
    assert(X509_add1_ext_i2d(crt, NID_subject_alt_name, gens, 0, 0) == 1);
    GENERAL_NAMES_free(gens);                      /* frees gn + os */

    EVP_PKEY *signer = issuer_key ? issuer_key : pkey;
    assert(X509_sign(crt, signer, EVP_sha256()) > 0);

    BIO *cb = BIO_new(BIO_s_mem());
    assert(PEM_write_bio_X509(cb, crt) == 1);
    out->cert_pem = bio_to_str(cb);
    BIO_free(cb);
    BIO *kb = BIO_new(BIO_s_mem());
    assert(PEM_write_bio_PrivateKey(kb, pkey, NULL, NULL, 0, NULL, NULL) == 1);
    out->key_pem = bio_to_str(kb);
    BIO_free(kb);

    EVP_PKEY_free(pkey);
    X509_free(crt);
}

/* Push one dNSName SAN of exactly `len` bytes (may contain NUL/control) onto a
 * GENERAL_NAMES stack. Used to build hostile SAN entries the adapter must omit. */
static void push_dns_san(GENERAL_NAMES *gens, const unsigned char *bytes, int len)
{
    GENERAL_NAME   *gn  = GENERAL_NAME_new();
    ASN1_IA5STRING *ia5 = ASN1_IA5STRING_new();
    assert(gn && ia5);
    assert(ASN1_STRING_set(ia5, bytes, len) == 1);
    GENERAL_NAME_set0_value(gn, GEN_DNS, ia5);       /* gn takes ownership of ia5 */
    assert(sk_GENERAL_NAME_push(gens, gn) > 0);
}

/* Generate a CA-signed leaf whose textual identity fields are HOSTILE:
 *   - CN carries an embedded NUL ("trusted-user\0attacker")
 *   - SANs: one clean ("good.example") + one with a comma+control byte + one
 *     with an embedded NUL.
 * The cert is otherwise valid and chains to the CA, so it passes verification;
 * only the exposed KlPeerCert text must be canonicalized. */
static void gen_cert_evil(EVP_PKEY *ca_key, X509 *ca_crt, PemPair *out)
{
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    assert(kctx);
    assert(EVP_PKEY_keygen_init(kctx) == 1);
    assert(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) == 1);
    assert(EVP_PKEY_keygen(kctx, &pkey) == 1);
    EVP_PKEY_CTX_free(kctx);
    assert(pkey);

    X509 *crt = X509_new();
    assert(crt);
    X509_set_version(crt, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(crt), (long)(rand() | 1));
    X509_gmtime_adj(X509_getm_notBefore(crt), -3600);
    X509_gmtime_adj(X509_getm_notAfter(crt), 3600 * 24 * 365);
    X509_set_pubkey(crt, pkey);

    /* CN with an embedded NUL. UTF8String stores 0x00 verbatim; the length is
     * explicit so it is not treated as a C terminator. */
    static const unsigned char evil_cn[] = "trusted-user\0attacker";
    X509_NAME *name = X509_get_subject_name(crt);
    assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                      evil_cn, (int)(sizeof(evil_cn) - 1), -1, 0) == 1);
    X509_set_issuer_name(crt, X509_get_subject_name(ca_crt));

    GENERAL_NAMES *gens = sk_GENERAL_NAME_new_null();
    assert(gens);
    push_dns_san(gens, (const unsigned char *)"good.example", 12);
    static const unsigned char comma_ctrl[] = "evil,inj\x01" "ected";
    push_dns_san(gens, comma_ctrl, (int)(sizeof(comma_ctrl) - 1));
    static const unsigned char nul_dns[] = "a\0b";
    push_dns_san(gens, nul_dns, (int)(sizeof(nul_dns) - 1));
    assert(X509_add1_ext_i2d(crt, NID_subject_alt_name, gens, 0, 0) == 1);
    GENERAL_NAMES_free(gens);

    assert(X509_sign(crt, ca_key, EVP_sha256()) > 0);

    BIO *cb = BIO_new(BIO_s_mem());
    assert(PEM_write_bio_X509(cb, crt) == 1);
    out->cert_pem = bio_to_str(cb);
    BIO_free(cb);
    BIO *kb = BIO_new(BIO_s_mem());
    assert(PEM_write_bio_PrivateKey(kb, pkey, NULL, NULL, 0, NULL, NULL) == 1);
    out->key_pem = bio_to_str(kb);
    BIO_free(kb);

    EVP_PKEY_free(pkey);
    X509_free(crt);
}

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

static void axis1_socket_bio(KlAllocator *alloc,
                             PemPair *ca, PemPair *server, PemPair *client)
{
    printf("== Axis 1: socket-BIO (readiness) ==\n");

    /* Server ctx: leaf + key, trusting CA for mTLS, REQUIRED client auth. */
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem),
        KL_MTLS_REQUIRED, alloc);
    assert(sctx);
    const char *sp[] = {"h2", "http/1.1", NULL};
    assert(kl_tls_openssl_ctx_set_alpn(sctx, sp) == 0);

    /* Client ctx: trusts CA (verifies server) AND presents its own client cert
     * (for the server's mTLS REQUIRED). */
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    assert(kl_tls_openssl_client_ctx_set_cert(cctx,
        (const unsigned char *)client->cert_pem, strlen(client->cert_pem),
        (const unsigned char *)client->key_pem, strlen(client->key_pem)) == 0);
    const char *cp[] = {"http/1.1", "h2", NULL};
    assert(kl_tls_openssl_ctx_set_alpn(cctx, cp) == 0);

    KlTls *srv = kl_tls_openssl_create(sctx, alloc);
    KlTls *cli = kl_tls_openssl_create(cctx, alloc);
    assert(srv && cli);

    assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    int cfd = fds[0], sfd = fds[1];

    assert(drive_handshake(cli, cfd, srv, sfd) == 0);
    printf("  handshake complete\n");

    /* ALPN: server offered h2 first; client offered http/1.1 first. Server
     * preference wins → "h2". */
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

    /* mTLS: server sees the client's verified cert. */
    KlPeerCert pc;
    memset(&pc, 0, sizeof(pc));
    int have = srv->peer_cert(srv, &pc);
    assert(have == 0);
    printf("  peer_cert: verified=%d subject_cn='%s' issuer_cn='%s' san='%s'\n",
           pc.verified, pc.subject_cn, pc.issuer_cn, pc.san);
    printf("  peer_cert: fp=%.16s... not_before=%lld not_after=%lld der_len=%zu\n",
           pc.fingerprint_sha256, (long long)pc.not_before,
           (long long)pc.not_after, pc.der_len);
    assert(pc.verified == 1);
    assert(strcmp(pc.subject_cn, "client.local") == 0);
    assert(strstr(pc.san, "DNS:client.local") != NULL);
    assert(pc.der_len > 0 && pc.der != NULL);
    assert(strlen(pc.fingerprint_sha256) == 64);
    printf("  PASS: mTLS peer_cert returns verified client identity\n");

    cli->destroy(cli);
    srv->destroy(srv);
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
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

    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);

    KlTls *srv = kl_tls_openssl_create(sctx, alloc);
    KlTls *cli = kl_tls_openssl_create(cctx, alloc);
    assert(srv && cli);
    assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);

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
    assert(srv->at_eof(srv) == 0);   /* a successful read is NOT at_eof */
    printf("  PASS: memory-BIO round-trip (client->server)\n");

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
    assert(cli->at_eof(cli) == 0);   /* a successful read is NOT at_eof */
    printf("  PASS: memory-BIO round-trip (server->client)\n");

    /* Clean shutdown from server → client's read() must report at_eof(). Because
     * at_eof() reflects the LATEST read, the successful read just above cleared it
     * and this EOF read sets it; it is not sticky from an earlier state. */
    srv->shutdown(srv, KL_INVALID_SOCKET);
    pump(srv, cli);
    kl_ssize_t rn = cli->read(cli, KL_INVALID_SOCKET, buf, sizeof(buf));
    assert(rn == -1);
    assert(cli->at_eof(cli) == 1);
    printf("  PASS: clean shutdown -> peer read()==-1 && at_eof()==1\n");

    cli->destroy(cli);
    srv->destroy(srv);
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

/* ── Hardening regression tests ──────────────────────────────────── */

/* Handshake a client/server pair over the memory-BIO axis (no sockets). Both
 * sessions are returned to the caller (not destroyed). Verifies against `ca`. */
static void mem_handshake(KlAllocator *alloc, KlTlsCtx *sctx, KlTlsCtx *cctx,
                          const char *sni, KlTls **out_srv, KlTls **out_cli)
{
    KlTls *srv = kl_tls_openssl_create(sctx, alloc);
    KlTls *cli = kl_tls_openssl_create(cctx, alloc);
    assert(srv && cli);
    if (sni)
        assert(kl_tls_openssl_set_hostname(cli, sni) == 0);
    assert(cli->feed_input(cli, NULL, 0) == 0);
    assert(srv->feed_input(srv, NULL, 0) == 0);
    int cdone = 0, sdone = 0, err = 0;
    for (int iter = 0; iter < 200 && (!cdone || !sdone); iter++) {
        if (!cdone) {
            KlTlsResult r = cli->handshake(cli, KL_INVALID_SOCKET);
            if (r == KL_TLS_OK) cdone = 1; else if (r == KL_TLS_ERROR) { err = 1; break; }
        }
        if (!sdone) {
            KlTlsResult r = srv->handshake(srv, KL_INVALID_SOCKET);
            if (r == KL_TLS_OK) sdone = 1; else if (r == KL_TLS_ERROR) { err = 1; break; }
        }
        pump(cli, srv);
        pump(srv, cli);
    }
    (void)err;
    *out_srv = srv;
    *out_cli = cli;
}

/* Return 1 iff a client/server handshake COMPLETES (both OK), else 0. Destroys
 * the sessions before returning. Used for negative (must-fail) cases. */
static int mem_handshake_ok(KlAllocator *alloc, KlTlsCtx *sctx, KlTlsCtx *cctx,
                            const char *sni)
{
    KlTls *srv = kl_tls_openssl_create(sctx, alloc);
    KlTls *cli = kl_tls_openssl_create(cctx, alloc);
    assert(srv && cli);
    if (sni)
        assert(kl_tls_openssl_set_hostname(cli, sni) == 0);
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

/* A second session's peer_cert() must NOT clobber a first session's returned DER
 * (per-session storage, not shared thread-local scratch). */
static void test_der_alpn_lifetime(KlAllocator *alloc, PemPair *ca, PemPair *server,
                                   PemPair *client)
{
    printf("== Finding 1: DER + ALPN lifetime (per-session storage) ==\n");

    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem),
        KL_MTLS_REQUIRED, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    assert(kl_tls_openssl_client_ctx_set_cert(cctx,
        (const unsigned char *)client->cert_pem, strlen(client->cert_pem),
        (const unsigned char *)client->key_pem, strlen(client->key_pem)) == 0);

    /* Session A: extract the client cert, snapshot its DER. */
    KlTls *srvA, *cliA;
    mem_handshake(alloc, sctx, cctx, "server.local", &srvA, &cliA);
    KlPeerCert pcA;
    assert(srvA->peer_cert(srvA, &pcA) == 0);
    assert(pcA.der_len > 0 && pcA.der != NULL);
    unsigned char *snapshot = malloc(pcA.der_len);
    assert(snapshot);
    memcpy(snapshot, pcA.der, pcA.der_len);
    size_t snap_len = pcA.der_len;
    const uint8_t *A_der_ptr = pcA.der;

    /* Session B on the SAME thread: extract its own client cert. */
    KlTls *srvB, *cliB;
    mem_handshake(alloc, sctx, cctx, "server.local", &srvB, &cliB);
    KlPeerCert pcB;
    assert(srvB->peer_cert(srvB, &pcB) == 0);
    assert(pcB.der_len > 0 && pcB.der != NULL);
    /* Distinct backing storage: the pre-fix shared scratch would alias. */
    assert(pcB.der != A_der_ptr);

    /* A's DER (re-read from A's still-live buffer) is UNCHANGED. */
    assert(A_der_ptr[0] == snapshot[0]);
    assert(memcmp(A_der_ptr, snapshot, snap_len) == 0);
    printf("  PASS: session B peer_cert did not clobber session A's DER\n");

    /* reset() invalidates A's der_len (contract: valid only until reset). */
    srvA->reset(srvA);
    KlPeerCert pcA2;
    memset(&pcA2, 0, sizeof(pcA2));
    /* After reset the handshake is gone, so peer_cert() returns -1. */
    assert(srvA->peer_cert(srvA, &pcA2) == -1);
    printf("  PASS: reset() invalidates prior peer-cert extraction\n");

    free(snapshot);
    cliA->destroy(cliA); srvA->destroy(srvA);
    cliB->destroy(cliB); srvB->destroy(srvB);
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

/* Two simultaneous sessions handshaking/exchanging interleaved on one thread. */
static void test_two_simultaneous(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Finding 1: two simultaneous sessions (interleaved) ==\n");
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);

    KlTls *srv1, *cli1, *srv2, *cli2;
    mem_handshake(alloc, sctx, cctx, "server.local", &srv1, &cli1);
    mem_handshake(alloc, sctx, cctx, "server.local", &srv2, &cli2);

    /* Interleave two round-trips through the two independent session pairs. */
    const char *m1 = "session one payload", *m2 = "session two payload";
    size_t l1 = strlen(m1), l2 = strlen(m2), s1 = 0, s2 = 0;
    while (s1 < l1) { kl_ssize_t n = cli1->write(cli1, KL_INVALID_SOCKET, m1 + s1, l1 - s1); assert(n >= 0); s1 += (size_t)n; }
    while (s2 < l2) { kl_ssize_t n = cli2->write(cli2, KL_INVALID_SOCKET, m2 + s2, l2 - s2); assert(n >= 0); s2 += (size_t)n; }
    pump(cli1, srv1);
    pump(cli2, srv2);
    char b1[128] = {0}, b2[128] = {0}; size_t g1 = 0, g2 = 0;
    for (int i = 0; i < 100 && g1 < l1; i++) { kl_ssize_t n = srv1->read(srv1, KL_INVALID_SOCKET, b1 + g1, sizeof(b1) - g1); if (n > 0) g1 += (size_t)n; }
    for (int i = 0; i < 100 && g2 < l2; i++) { kl_ssize_t n = srv2->read(srv2, KL_INVALID_SOCKET, b2 + g2, sizeof(b2) - g2); if (n > 0) g2 += (size_t)n; }
    assert(g1 == l1 && memcmp(b1, m1, l1) == 0);
    assert(g2 == l2 && memcmp(b2, m2, l2) == 0);
    printf("  PASS: two independent sessions round-tripped interleaved\n");

    cli1->destroy(cli1); srv1->destroy(srv1);
    cli2->destroy(cli2); srv2->destroy(srv2);
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

/* Hostname mismatch → handshake fails (SNI/verify name not in the cert SAN). */
static void test_hostname_mismatch(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Findings 5/7: hostname mismatch -> handshake fails ==\n");
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    /* Cert SAN is server.local; ask for a different name. */
    int ok = mem_handshake_ok(alloc, sctx, cctx, "wrong.example");
    assert(ok == 0);
    printf("  PASS: mismatched SNI/verify name fails the handshake\n");
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

/* Untrusted CA → verify fails (client trusts a DIFFERENT CA than signed server). */
static void test_untrusted_ca(KlAllocator *alloc, PemPair *server)
{
    printf("== Finding 7: untrusted CA -> verify fails ==\n");
    /* A fresh, unrelated CA the client will trust; but the server leaf was
     * signed by the real CA, so verification must fail. */
    PemPair other_ca = {0};
    gen_cert("Other CA", 1, NULL, NULL, &other_ca, NULL, NULL);

    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)other_ca.cert_pem, strlen(other_ca.cert_pem), alloc);
    assert(cctx);
    int ok = mem_handshake_ok(alloc, sctx, cctx, "server.local");
    assert(ok == 0);
    printf("  PASS: server cert under an untrusted CA fails verification\n");
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
    pem_pair_free(&other_ca);
}

/* mTLS REQUIRED but client presents no cert → handshake fails. */
static void test_missing_client_cert(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Finding 6: mTLS REQUIRED + no client cert -> handshake fails ==\n");
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem),
        KL_MTLS_REQUIRED, alloc);
    assert(sctx);
    /* Client trusts the CA but does NOT set_cert: presents no client cert. */
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    int ok = mem_handshake_ok(alloc, sctx, cctx, "server.local");
    assert(ok == 0);
    printf("  PASS: REQUIRED client auth with no client cert fails\n");
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

/* mTLS config validation: OPTIONAL/REQUIRED without a CA, bad mode,
 * and mismatched ca_buf/ca_len are all rejected (ctor returns NULL). */
static void test_mtls_config_validation(KlAllocator *alloc, PemPair *server, PemPair *ca)
{
    printf("== Finding 6: mTLS config validation ==\n");
    const unsigned char *cert = (const unsigned char *)server->cert_pem;
    size_t clen = strlen(server->cert_pem);
    const unsigned char *key = (const unsigned char *)server->key_pem;
    size_t klen = strlen(server->key_pem);
    const unsigned char *cab = (const unsigned char *)ca->cert_pem;
    size_t calen = strlen(ca->cert_pem);

    /* REQUIRED but no CA → NULL. */
    assert(kl_tls_openssl_ctx_create_from_buf(cert, clen, key, klen, NULL, 0,
                                              KL_MTLS_REQUIRED, alloc) == NULL);
    /* OPTIONAL but no CA → NULL. */
    assert(kl_tls_openssl_ctx_create_from_buf(cert, clen, key, klen, NULL, 0,
                                              KL_MTLS_OPTIONAL, alloc) == NULL);
    /* Out-of-range mode → NULL. */
    assert(kl_tls_openssl_ctx_create_from_buf(cert, clen, key, klen, NULL, 0,
                                              (KlMtlsMode)99, alloc) == NULL);
    /* ca_buf set but ca_len 0 → NULL. */
    assert(kl_tls_openssl_ctx_create_from_buf(cert, clen, key, klen, cab, 0,
                                              KL_MTLS_NONE, alloc) == NULL);
    /* ca_buf NULL but ca_len non-zero → NULL. */
    assert(kl_tls_openssl_ctx_create_from_buf(cert, clen, key, klen, NULL, 5,
                                              KL_MTLS_NONE, alloc) == NULL);
    /* Valid REQUIRED + CA → non-NULL. */
    KlTlsCtx *ok = kl_tls_openssl_ctx_create_from_buf(cert, clen, key, klen,
                                                      cab, calen, KL_MTLS_REQUIRED, alloc);
    assert(ok != NULL);
    kl_tls_openssl_ctx_destroy(ok);
    printf("  PASS: invalid mTLS configs rejected, valid one accepted\n");
}

/* Completion-buffer boundary: feed_input rejects oversized + NULL. */
static void test_feed_input_boundary(KlAllocator *alloc, PemPair *ca)
{
    printf("== Finding 3: feed_input boundary (oversize + NULL) ==\n");
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    KlTls *cli = kl_tls_openssl_create(cctx, alloc);
    assert(cli);

    /* Enable completion mode. */
    assert(cli->feed_input(cli, NULL, 0) == 0);
    /* len==0 with NULL is a no-op success. */
    assert(cli->feed_input(cli, NULL, 0) == 0);
    /* NULL buffer with non-zero len is rejected. */
    assert(cli->feed_input(cli, NULL, 16) == -1);
    /* Oversized chunk (> 256 KiB cap) is rejected. */
    size_t huge = (size_t)(256 * 1024) + 1;
    unsigned char *blob = calloc(1, huge);
    assert(blob);
    assert(cli->feed_input(cli, blob, huge) == -1);
    free(blob);
    /* drain_output: NULL buf with non-zero cap is rejected; cap 0 is a no-op. */
    unsigned char tmp[16];
    assert(cli->drain_output(cli, NULL, 8) == -1);
    assert(cli->drain_output(cli, tmp, 0) == 0);

    cli->destroy(cli);
    kl_tls_openssl_ctx_destroy(cctx);
    printf("  PASS: feed_input/drain_output reject oversize + NULL args\n");
}

/* from_buf boundary: a ctor rejects len==0 / NULL buffers. */
static void test_from_buf_boundary(KlAllocator *alloc, PemPair *server, PemPair *ca)
{
    printf("== Finding 2: from_buf boundary (len==0 / NULL rejected) ==\n");
    const unsigned char *cert = (const unsigned char *)server->cert_pem;
    size_t clen = strlen(server->cert_pem);
    const unsigned char *key = (const unsigned char *)server->key_pem;
    size_t klen = strlen(server->key_pem);

    /* Zero-length cert buffer → bio_new_mem_checked rejects → ctor NULL. */
    assert(kl_tls_openssl_ctx_create_from_buf(cert, 0, key, klen, NULL, 0,
                                              KL_MTLS_NONE, alloc) == NULL);
    /* Zero-length key buffer → ctor NULL. */
    assert(kl_tls_openssl_ctx_create_from_buf(cert, clen, key, 0, NULL, 0,
                                              KL_MTLS_NONE, alloc) == NULL);
    /* NULL cert buffer → ctor NULL. */
    assert(kl_tls_openssl_ctx_create_from_buf(NULL, 0, key, klen, NULL, 0,
                                              KL_MTLS_NONE, alloc) == NULL);
    /* Client from_buf: non-NULL CA with len 0 → NULL. */
    assert(kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, 0, alloc) == NULL);
    printf("  PASS: from_buf ctors reject empty/NULL PEM buffers\n");
}

/* Insecure client ctx opt-out accepts an untrusted server. */
static void test_insecure_client(KlAllocator *alloc, PemPair *server)
{
    printf("== Finding 7: explicit insecure client ctx (verify-none) ==\n");
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    /* verify-none: no CA needed, no hostname check: handshake completes even
     * though the client trusts nothing. */
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_insecure(alloc);
    assert(cctx);
    int ok = mem_handshake_ok(alloc, sctx, cctx, NULL);
    assert(ok == 1);
    printf("  PASS: insecure ctx handshakes without CA/hostname verification\n");
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

/* peer_cert(NULL) must return -1 (no crash, no write through NULL). */
static void test_peer_cert_null(KlAllocator *alloc, PemPair *ca, PemPair *server,
                                PemPair *client)
{
    printf("== Finding 3: peer_cert(NULL) returns -1 ==\n");
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem),
        KL_MTLS_REQUIRED, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    assert(kl_tls_openssl_client_ctx_set_cert(cctx,
        (const unsigned char *)client->cert_pem, strlen(client->cert_pem),
        (const unsigned char *)client->key_pem, strlen(client->key_pem)) == 0);

    KlTls *srv, *cli;
    mem_handshake(alloc, sctx, cctx, "server.local", &srv, &cli);
    /* Handshake complete, a client cert is present, yet NULL out → -1. */
    assert(srv->peer_cert(srv, NULL) == -1);
    /* Sanity: with a real out it still succeeds. */
    KlPeerCert pc;
    assert(srv->peer_cert(srv, &pc) == 0);
    printf("  PASS: peer_cert(NULL) -> -1 (and non-NULL still works)\n");

    cli->destroy(cli); srv->destroy(srv);
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

/* A second full handshake after reset() on the SAME sessions round-trips. */
static void test_second_handshake_after_reset(KlAllocator *alloc, PemPair *ca,
                                               PemPair *server)
{
    printf("== Finding 5: second handshake after reset() round-trips ==\n");
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);

    KlTls *srv = kl_tls_openssl_create(sctx, alloc);
    KlTls *cli = kl_tls_openssl_create(cctx, alloc);
    assert(srv && cli);

    /* First handshake + round-trip over sockets (socket-BIO axis). */
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);
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
    assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);
    assert(drive_handshake(cli, fds2[0], srv, fds2[1]) == 0);
    roundtrip(cli, fds2[0], srv, fds2[1], "second handshake payload");
    printf("  PASS: second handshake after reset() round-trips\n");

    close(fds2[0]); close(fds2[1]);
    cli->destroy(cli); srv->destroy(srv);
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

/* Numeric-IP SAN: matching IP verifies, wrong IP fails. */
static void test_ip_san(KlAllocator *alloc, EVP_PKEY *ca_key, X509 *ca_crt,
                        PemPair *ca)
{
    printf("== Finding 4: numeric-IP SAN verification ==\n");
    /* A server leaf whose ONLY SAN is the IP 127.0.0.1, signed by the CA. */
    PemPair ipsrv = {0};
    gen_cert_ip_san("127.0.0.1", ca_key, ca_crt, &ipsrv);

    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)ipsrv.cert_pem, strlen(ipsrv.cert_pem),
        (const unsigned char *)ipsrv.key_pem, strlen(ipsrv.key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);

    /* Positive: verify against the matching IP literal. */
    int ok = mem_handshake_ok(alloc, sctx, cctx, "127.0.0.1");
    assert(ok == 1);
    printf("  PASS: matching IP-SAN verifies\n");

    /* Negative: a different IP literal must fail (IP-SAN mismatch). */
    ok = mem_handshake_ok(alloc, sctx, cctx, "127.0.0.2");
    assert(ok == 0);
    printf("  PASS: wrong IP-SAN fails verification\n");

    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
    pem_pair_free(&ipsrv);
}

/* Strict vs lenient truncation over the REAL socket-BIO readiness
 * path. The peer fd is closed WITHOUT a TLS close_notify (abrupt transport EOF).
 *   strict (default):     read() == -1 && at_eof() == 0  (truncation is an error)
 *   lenient (allow=1):    read() == -1 && at_eof() == 1  (treated as clean EOF)
 */
static void run_truncation_case(KlAllocator *alloc, PemPair *ca, PemPair *server,
                                int allow_truncation)
{
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    /* The truncation policy is a CLIENT-side reader concern; set it on the
     * client ctx so the client session inherits it. */
    assert(kl_tls_openssl_ctx_set_allow_truncation(cctx, allow_truncation) == 0);

    KlTls *srv = kl_tls_openssl_create(sctx, alloc);
    KlTls *cli = kl_tls_openssl_create(cctx, alloc);
    assert(srv && cli);
    assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    int cfd = fds[0], sfd = fds[1];
    assert(drive_handshake(cli, cfd, srv, sfd) == 0);

    /* Server writes one payload, then ABRUPTLY closes its fd: no shutdown(). */
    const char *msg = "pre-truncation body";
    size_t mlen = strlen(msg), sent = 0;
    for (int i = 0; i < 200 && sent < mlen; i++) {
        kl_ssize_t n = srv->write(srv, sfd, msg + sent, mlen - sent);
        if (n > 0) sent += (size_t)n;
    }
    assert(sent == mlen);
    close(sfd);                    /* abrupt transport EOF, no close_notify */
    srv->destroy(srv);             /* server session gone */

    /* Client drains the payload, then the next read hits the abrupt EOF. */
    char buf[256]; size_t got = 0; int saw_eof_result = 0;
    for (int i = 0; i < 400; i++) {
        kl_ssize_t n = cli->read(cli, cfd, buf + got, sizeof(buf) - got);
        if (n > 0) { got += (size_t)n; continue; }
        if (n == 0) continue;      /* WANT_READ (readiness): retry */
        /* n < 0: terminal. Consult at_eof(). */
        saw_eof_result = 1;
        if (allow_truncation) {
            assert(cli->at_eof(cli) == 1);
        } else {
            assert(cli->at_eof(cli) == 0);
        }
        break;
    }
    assert(got == mlen && memcmp(buf, msg, mlen) == 0);
    assert(saw_eof_result == 1);

    close(cfd);
    cli->destroy(cli);
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

static void test_truncation_modes(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Finding 5: truncation (strict vs lenient) over socket-BIO ==\n");
    run_truncation_case(alloc, ca, server, 0);
    printf("  PASS: strict, abrupt EOF is read()==-1 && at_eof()==0\n");
    run_truncation_case(alloc, ca, server, 1);
    printf("  PASS: lenient, abrupt EOF is read()==-1 && at_eof()==1\n");
}

/* ── Allocation-failure injection ────────────────────────────────── */

typedef struct {
    KlAllocator base;   /* forwards to real allocator */
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

/* Drive a handshake + peer-cert extraction under an allocator that starts failing
 * after N allocations, for a sweep of N. Each iteration must fail gracefully
 * (no crash / no UAF under ASan); some N will complete, some won't; either is
 * acceptable, the point is memory-safety on the failure paths (DER-buffer growth
 * in peer_cert, completion in/out ring growth in feed_input/drain). */
static void test_alloc_failure_injection(PemPair *ca, PemPair *server, PemPair *client)
{
    printf("== Finding 5: allocation-failure injection (graceful, ASan-clean) ==\n");
    for (int n = 0; n < 40; n++) {
        FailAlloc f;
        KlAllocator a = fail_alloc_make(&f, n);

        KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
            (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
            (const unsigned char *)server->key_pem, strlen(server->key_pem),
            (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem),
            KL_MTLS_REQUIRED, &a);
        KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
            (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), &a);
        if (cctx)
            (void)kl_tls_openssl_client_ctx_set_cert(cctx,
                (const unsigned char *)client->cert_pem, strlen(client->cert_pem),
                (const unsigned char *)client->key_pem, strlen(client->key_pem));

        if (sctx && cctx) {
            KlTls *srv = kl_tls_openssl_create(sctx, &a);
            KlTls *cli = kl_tls_openssl_create(cctx, &a);
            if (srv && cli) {
                if (kl_tls_openssl_set_hostname(cli, "server.local") == 0 &&
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
                        /* pump: feed/drain ring growth exercises comp_ensure under failure */
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
                        /* peer_cert exercises DER-buffer growth under failure. */
                        KlPeerCert pc;
                        (void)srv->peer_cert(srv, &pc);
                    }
                }
            }
            if (cli) cli->destroy(cli);
            if (srv) srv->destroy(srv);
        }
        if (sctx) kl_tls_openssl_ctx_destroy(sctx);
        if (cctx) kl_tls_openssl_ctx_destroy(cctx);
    }
    printf("  PASS: alloc-failure sweep completed without crash/UAF (ASan)\n");
}

/* Leave an unrelated error in OpenSSL's thread-local queue. Parsing garbage as
 * DER X.509 fails and queues an ASN.1 error on every OpenSSL-family library:
 * simulating a stale error left by app code or another session on this thread. */
static void poison_error_queue(void)
{
    static const unsigned char garbage[] = { 0x30, 0x01, 0xff };
    const unsigned char *p = garbage;
    X509 *x = d2i_X509(NULL, &p, (long)sizeof(garbage));
    assert(x == NULL);
    assert(ERR_peek_error() != 0);   /* the queue is now non-empty */
}

/* OpenSSL's error queue is thread-local and SSL_get_error() only
 * classifies correctly against an EMPTY queue. The adapter must ERR_clear_error()
 * before every SSL op so a stale error (from another session sharing the event
 * loop) cannot change the result. Proven two ways:
 *   (a) a bare transport EOF under lenient truncation must still classify as a
 *       clean EOF even with the queue poisoned (WITHOUT the fix the stale error
 *       makes SSL_get_error report SSL_ERROR_SSL and at_eof() would be 0), and
 *   (b) an independent session completes a full handshake + round-trip with the
 *       queue poisoned before each op: no cross-session contamination. */
static void test_error_queue_discipline(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Finding (r6): OpenSSL error-queue discipline ==\n");

    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    assert(kl_tls_openssl_ctx_set_allow_truncation(sctx, 1) == 0);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    assert(kl_tls_openssl_ctx_set_allow_truncation(cctx, 1) == 0);

    /* (a) bare transport EOF under a poisoned queue → still a clean EOF. */
    {
        KlTls *srv = kl_tls_openssl_create(sctx, alloc);
        KlTls *cli = kl_tls_openssl_create(cctx, alloc);
        assert(srv && cli);
        assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
        assert(drive_handshake(cli, fds[0], srv, fds[1]) == 0);

        close(fds[1]);          /* abrupt close: transport EOF, no close_notify */
        char b[16];
        kl_ssize_t n = 0;
        for (int i = 0; i < 20 && n == 0; i++) {
            poison_error_queue();               /* stale error before each read */
            n = cli->read(cli, fds[0], b, sizeof b);
        }
        assert(n < 0 && cli->at_eof(cli) == 1); /* clean EOF despite the poison */
        printf("  PASS: bare EOF classified clean despite a poisoned queue\n");
        close(fds[0]);
        cli->destroy(cli);
        srv->destroy(srv);
    }

    /* (b) an independent session, poisoned before every op, still handshakes and
     * round-trips: the stale error does not leak across sessions. */
    {
        KlTls *srv = kl_tls_openssl_create(sctx, alloc);
        KlTls *cli = kl_tls_openssl_create(cctx, alloc);
        assert(srv && cli);
        assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);

        int cdone = 0, sdone = 0;
        for (int it = 0; it < 200 && (!cdone || !sdone); it++) {
            poison_error_queue();
            if (!cdone) {
                KlTlsResult r = cli->handshake(cli, fds[0]);
                assert(r != KL_TLS_ERROR);
                if (r == KL_TLS_OK) cdone = 1;
            }
            poison_error_queue();
            if (!sdone) {
                KlTlsResult r = srv->handshake(srv, fds[1]);
                assert(r != KL_TLS_ERROR);
                if (r == KL_TLS_OK) sdone = 1;
            }
        }
        assert(cdone && sdone);
        poison_error_queue();
        roundtrip(cli, fds[0], srv, fds[1], "poisoned but fine");
        printf("  PASS: independent session unaffected by a poisoned queue\n");
        close(fds[0]);
        close(fds[1]);
        cli->destroy(cli);
        srv->destroy(srv);
    }

    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
}

/* KlPeerCert.verified must mean the chain was ACTUALLY verified,
 * not merely that a stored result reads X509_V_OK. A verifying client sees
 * verified==1 for a trusted server; an insecure (verify-none) client completes the
 * handshake but must report verified==0. (Untrusted-server / mTLS-verified paths
 * are covered by test_untrusted_ca and axis1_socket_bio.) */
static void test_client_verified_flag(KlAllocator *alloc, PemPair *ca, PemPair *server)
{
    printf("== Finding (r5): client peer_cert.verified honesty ==\n");

    /* Plain-TLS server (no mTLS). Reused for both client sessions. */
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);

    /* (a) verifying client trusting the CA → verified == 1. */
    {
        KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
            (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
        assert(cctx);
        KlTls *srv = kl_tls_openssl_create(sctx, alloc);
        KlTls *cli = kl_tls_openssl_create(cctx, alloc);
        assert(srv && cli);
        assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
        assert(drive_handshake(cli, fds[0], srv, fds[1]) == 0);

        KlPeerCert pc;
        memset(&pc, 0, sizeof(pc));
        assert(cli->peer_cert(cli, &pc) == 0);
        assert(pc.verified == 1);
        assert(strcmp(pc.subject_cn, "server.local") == 0);
        printf("  PASS: verifying client + trusted server -> verified==1\n");
        close(fds[0]); close(fds[1]);
        cli->destroy(cli); srv->destroy(srv);
        kl_tls_openssl_ctx_destroy(cctx);
    }

    /* (b) insecure client (SSL_VERIFY_NONE) → handshake OK but verified == 0. */
    {
        KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_insecure(alloc);
        assert(cctx);
        KlTls *srv = kl_tls_openssl_create(sctx, alloc);
        KlTls *cli = kl_tls_openssl_create(cctx, alloc);
        assert(srv && cli);
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
        assert(drive_handshake(cli, fds[0], srv, fds[1]) == 0);

        KlPeerCert pc;
        memset(&pc, 0, sizeof(pc));
        assert(cli->peer_cert(cli, &pc) == 0);   /* cert is present … */
        assert(pc.verified == 0);                /* … but was NOT verified */
        assert(pc.der_len > 0 && pc.der != NULL);
        printf("  PASS: insecure client -> handshake OK, verified==0\n");
        close(fds[0]); close(fds[1]);
        cli->destroy(cli); srv->destroy(srv);
        kl_tls_openssl_ctx_destroy(cctx);
    }

    kl_tls_openssl_ctx_destroy(sctx);
}

/* A CN that would not fit KlPeerCert.subject_cn[256] must be
 * OMITTED, not truncated: two long CNs sharing a 255-byte prefix must never
 * collapse to the same exposed string. The issuer CN (short) is unaffected. */
static void test_overlong_cn_omitted(KlAllocator *alloc, EVP_PKEY *ca_key,
                                     X509 *ca_crt, PemPair *ca, PemPair *server)
{
    printf("== Finding (r5): overlong CN omitted (not truncated) ==\n");

    char long_cn[300];
    memset(long_cn, 'a', sizeof(long_cn) - 1);
    long_cn[sizeof(long_cn) - 1] = '\0';        /* 299 'a's > subject_cn[256] */
    PemPair leaf = {0};
    gen_cert(long_cn, 0, ca_key, ca_crt, &leaf, NULL, NULL);

    /* mTLS: present the long-CN cert as the client cert to a REQUIRED server. */
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem),
        KL_MTLS_REQUIRED, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    assert(kl_tls_openssl_client_ctx_set_cert(cctx,
        (const unsigned char *)leaf.cert_pem, strlen(leaf.cert_pem),
        (const unsigned char *)leaf.key_pem, strlen(leaf.key_pem)) == 0);

    KlTls *srv = kl_tls_openssl_create(sctx, alloc);
    KlTls *cli = kl_tls_openssl_create(cctx, alloc);
    assert(srv && cli);
    assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);

    /* Accept both fail-closed outcomes (some backends cap CN length at the TLS
     * layer). Where the handshake completes, the overlong CN must be empty. */
    if (drive_handshake(cli, fds[0], srv, fds[1]) != 0) {
        printf("  PASS: overlong-CN cert rejected at handshake (fail closed)\n");
    } else {
        KlPeerCert pc;
        memset(&pc, 0, sizeof(pc));
        assert(srv->peer_cert(srv, &pc) == 0);
        assert(pc.subject_cn[0] == '\0');               /* omitted, not truncated */
        assert(strcmp(pc.issuer_cn, "Keel Test CA") == 0); /* short CN still shown */
        printf("  PASS: overlong CN omitted; short issuer CN preserved\n");
    }

    close(fds[0]); close(fds[1]);
    cli->destroy(cli); srv->destroy(srv);
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
    pem_pair_free(&leaf);
}

/* Textual peer-cert identities must be canonicalized. A client
 * cert whose CN embeds a NUL ("trusted-user\0attacker") and whose SANs include
 * comma/control/NUL bytes must NOT surface those bytes to the application: the CN
 * is omitted (empty) and the hostile SANs are dropped, leaving only the clean one.
 * Proves the fail-closed identity handling over a real mTLS handshake. */
static void test_identity_canonicalization(KlAllocator *alloc, EVP_PKEY *ca_key,
                                           X509 *ca_crt, PemPair *ca, PemPair *server)
{
    printf("== Finding (r4): CN/SAN identity canonicalization ==\n");
    PemPair evil = {0};
    gen_cert_evil(ca_key, ca_crt, &evil);

    /* Server REQUIRES a client cert; the client presents the crafted 'evil' one. */
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem),
        KL_MTLS_REQUIRED, alloc);
    assert(sctx);
    KlTlsCtx *cctx = kl_tls_openssl_client_ctx_create_from_buf(
        (const unsigned char *)ca->cert_pem, strlen(ca->cert_pem), alloc);
    assert(cctx);
    assert(kl_tls_openssl_client_ctx_set_cert(cctx,
        (const unsigned char *)evil.cert_pem, strlen(evil.cert_pem),
        (const unsigned char *)evil.key_pem, strlen(evil.key_pem)) == 0);

    KlTls *srv = kl_tls_openssl_create(sctx, alloc);
    KlTls *cli = kl_tls_openssl_create(cctx, alloc);
    assert(srv && cli);
    assert(kl_tls_openssl_set_hostname(cli, "server.local") == 0);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    int cfd = fds[0], sfd = fds[1];

    /* Two safe outcomes are acceptable, both fail-closed:
     *   (a) the backend rejects the malformed CN/SAN at the TLS layer outright
     *       (e.g. LibreSSL parses the hostile SAN strictly and aborts), or
     *   (b) the handshake completes and the adapter canonicalizes the identity:
     *       CN omitted, hostile SANs dropped (OpenSSL/BoringSSL).
     * What must NEVER happen is (b) surfacing the spoofable bytes. The normal-cert
     * mTLS path in axis1_socket_bio proves a well-formed client cert DOES complete
     * and yields its real identity, so a rejection here is specific to the hostile
     * content: not a handshake defect. */
    int hs = drive_handshake(cli, cfd, srv, sfd);
    if (hs != 0) {
        printf("  PASS: hostile-identity cert rejected at handshake (fail closed)\n");
    } else {
        KlPeerCert pc;
        memset(&pc, 0, sizeof(pc));
        assert(srv->peer_cert(srv, &pc) == 0);
        assert(pc.verified == 1);               /* still chains to the CA */

        printf("  subject_cn='%s' san='%s'\n", pc.subject_cn, pc.san);
        /* CN carried an embedded NUL -> rejected, surfaced empty (fail closed). */
        assert(pc.subject_cn[0] == '\0');
        /* Only the clean DNS SAN survives; comma/control/NUL entries are dropped. */
        assert(strstr(pc.san, "DNS:good.example") != NULL);
        assert(strstr(pc.san, "evil") == NULL);
        assert(strstr(pc.san, "inj") == NULL);
        assert(strchr(pc.san, ',') == NULL);    /* no ambiguous unescaped comma */
        /* The DER identity remains complete regardless of text canonicalization. */
        assert(pc.der_len > 0 && pc.der != NULL);
        assert(strlen(pc.fingerprint_sha256) == 64);
        printf("  PASS: embedded-NUL CN rejected; malformed SANs omitted\n");
    }

    close(cfd);
    close(sfd);
    cli->destroy(cli);
    srv->destroy(srv);
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
    pem_pair_free(&evil);
}

/* Prove the TLS 1.2 minimum floor is actually enforced: a raw
 * client capped at an obsolete protocol (TLS 1.1) must NOT negotiate with the
 * adapter server. Failure of SSL_CTX_set_min_proto_version(TLS1_2) is fatal during
 * context creation; this asserts the resulting floor holds on the wire. */
static void test_obsolete_protocol_rejected(KlAllocator *alloc, PemPair *server)
{
#ifdef TLS1_1_VERSION
    printf("== Finding (r3): TLS 1.1 client -> rejected by 1.2 floor ==\n");

    /* Adapter server (code under test) with its default TLS 1.2 minimum. */
    KlTlsCtx *sctx = kl_tls_openssl_ctx_create_from_buf(
        (const unsigned char *)server->cert_pem, strlen(server->cert_pem),
        (const unsigned char *)server->key_pem, strlen(server->key_pem),
        NULL, 0, KL_MTLS_NONE, alloc);
    assert(sctx);
    KlTls *srv = kl_tls_openssl_create(sctx, alloc);
    assert(srv);

    /* Raw OpenSSL client forced to offer at most TLS 1.1. Best-effort widen the
     * lower bound so it emits a genuine <=1.1 ClientHello; capping the max at 1.1
     * is the assertion that matters (a known constant, so this must succeed). If
     * the library lacks TLS 1.1 entirely the empty version range still fails the
     * handshake: the same outcome we assert. */
    SSL_CTX *rctx = SSL_CTX_new(TLS_client_method());
    assert(rctx);
    SSL_CTX_set_verify(rctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_min_proto_version(rctx, TLS1_VERSION);
    assert(SSL_CTX_set_max_proto_version(rctx, TLS1_1_VERSION) == 1);
    SSL *ssl = SSL_new(rctx);
    assert(ssl);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(nonblock(fds[0]) == 0 && nonblock(fds[1]) == 0);
    int cfd = fds[0], sfd = fds[1];
    SSL_set_fd(ssl, cfd);            /* BIO_NOCLOSE: we close cfd ourselves */
    SSL_set_connect_state(ssl);

    int cfail = 0, sfail = 0, sdone = 0;
    for (int iter = 0; iter < 400 && !cfail && !sfail && !sdone; iter++) {
        KlTlsResult r = srv->handshake(srv, sfd);
        if (r == KL_TLS_OK) sdone = 1;              /* must never happen */
        else if (r == KL_TLS_ERROR) sfail = 1;

        int rc = SSL_do_handshake(ssl);
        if (rc != 1) {
            int e = SSL_get_error(ssl, rc);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) cfail = 1;
        }
    }
    /* The obsolete protocol must be rejected: the adapter server never completes,
     * and the negotiation fails on at least one side. */
    assert(sdone == 0);
    assert(sfail == 1 || cfail == 1);
    printf("  PASS: obsolete-protocol (TLS 1.1) client rejected by 1.2 floor\n");

    SSL_free(ssl);
    SSL_CTX_free(rctx);
    close(cfd);
    close(sfd);
    srv->destroy(srv);
    kl_tls_openssl_ctx_destroy(sctx);
#else
    (void)alloc; (void)server;
    printf("== Finding (r3): TLS1_1_VERSION unavailable, skipping floor test ==\n");
#endif
}

int main(void) {
    srand(1234);
    KlAllocator alloc = kl_allocator_default();

    /* CA (self-signed), a server leaf, and a client leaf all signed by the CA. */
    PemPair ca = {0}, server = {0}, client = {0};
    EVP_PKEY *ca_key = NULL; X509 *ca_crt = NULL;
    gen_cert("Keel Test CA", 1, NULL, NULL, &ca, &ca_key, &ca_crt);
    gen_cert("server.local", 0, ca_key, ca_crt, &server, NULL, NULL);
    gen_cert("client.local", 0, ca_key, ca_crt, &client, NULL, NULL);

    axis1_socket_bio(&alloc, &ca, &server, &client);
    axis2_memory_bio(&alloc, &ca, &server);

    /* Hardening regression suite. */
    test_der_alpn_lifetime(&alloc, &ca, &server, &client);
    test_two_simultaneous(&alloc, &ca, &server);
    test_hostname_mismatch(&alloc, &ca, &server);
    test_untrusted_ca(&alloc, &server);
    test_missing_client_cert(&alloc, &ca, &server);
    test_mtls_config_validation(&alloc, &server, &ca);
    test_feed_input_boundary(&alloc, &ca);
    test_from_buf_boundary(&alloc, &server, &ca);
    test_insecure_client(&alloc, &server);

    /* Truncation / allocation-failure / protocol-floor coverage additions. */
    test_peer_cert_null(&alloc, &ca, &server, &client);
    test_second_handshake_after_reset(&alloc, &ca, &server);
    test_ip_san(&alloc, ca_key, ca_crt, &ca);
    test_truncation_modes(&alloc, &ca, &server);
    test_alloc_failure_injection(&ca, &server, &client);
    test_obsolete_protocol_rejected(&alloc, &server);
    test_identity_canonicalization(&alloc, ca_key, ca_crt, &ca, &server);
    test_client_verified_flag(&alloc, &ca, &server);
    test_overlong_cn_omitted(&alloc, ca_key, ca_crt, &ca, &server);
    test_error_queue_discipline(&alloc, &ca, &server);

    EVP_PKEY_free(ca_key);
    X509_free(ca_crt);
    pem_pair_free(&ca);
    pem_pair_free(&server);
    pem_pair_free(&client);

    printf("\nALL PASS: both KlTls transport axes + hardening suite verified\n");
    return 0;
}
