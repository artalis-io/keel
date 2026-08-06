/*
 * tls_e2e.c — end-to-end proof that the OpenSSL KlTls adapter satisfies BOTH
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
 * at runtime with the OpenSSL library API — no filesystem, no `openssl` CLI.
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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

    /* Extensions via the programmatic X509_add1_ext_i2d — portable across OpenSSL,
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
    kl_tls_openssl_ctx_destroy(sctx);
    kl_tls_openssl_ctx_destroy(cctx);
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

    EVP_PKEY_free(ca_key);
    X509_free(ca_crt);
    pem_pair_free(&ca);
    pem_pair_free(&server);
    pem_pair_free(&client);

    printf("\nALL PASS: both KlTls transport axes verified with OpenSSL\n");
    return 0;
}
