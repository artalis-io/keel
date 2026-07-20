#include "utest.h"
#include <keel/keel.h>
#include <keel/tls.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>

/* ═══════════════════════════════════════════════════════════════════
 * Passthrough TLS mock with a canned peer_cert
 *
 * The real mbedtls extraction is exercised by the mbedtls backend build;
 * here we validate the vtable plumbing: kl_request_peer_cert() must reach
 * conn->tls->peer_cert and pass its result through unchanged. A global
 * knob selects the mock's behaviour per test.
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    PT_CERT_PRESENT,   /* peer_cert fills a canned cert, returns 0 */
    PT_CERT_NONE,      /* peer_cert returns -1 (no client cert) */
    PT_CERT_NOSLOT     /* peer_cert vtable slot left NULL */
} PtCertMode;

static PtCertMode g_cert_mode = PT_CERT_PRESENT;
static const uint8_t g_der[] = { 0x30, 0x82, 0x01, 0x0a };  /* fake DER prefix */

typedef struct {
    KlTls        base;
    KlAllocator *alloc;
    int          handshake_done;
} PtTls;

static KlTlsResult pt_handshake(KlTls *self, KlSocketHandle fd) {
    PtTls *pt = (PtTls *)self;
    (void)fd;
    pt->handshake_done = 1;
    return KL_TLS_OK;
}
static ssize_t pt_read(KlTls *self, KlSocketHandle fd, void *buf, size_t len) {
    (void)self;
    ssize_t r;
    do { r = read(fd, buf, len); } while (r < 0 && errno == EINTR);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return r;
}
static ssize_t pt_write(KlTls *self, KlSocketHandle fd, const void *buf, size_t len) {
    (void)self;
    ssize_t r;
    do { r = write(fd, buf, len); } while (r < 0 && errno == EINTR);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return r;
}
static KlTlsResult pt_shutdown(KlTls *self, KlSocketHandle fd) { (void)self; (void)fd; return KL_TLS_OK; }
static size_t pt_pending(KlTls *self) { (void)self; return 0; }
static void pt_reset(KlTls *self) { ((PtTls *)self)->handshake_done = 0; }
static void pt_destroy(KlTls *self) {
    PtTls *pt = (PtTls *)self;
    kl_free(pt->alloc, pt, sizeof(*pt));
}

static int pt_peer_cert(KlTls *self, KlPeerCert *out) {
    (void)self;
    if (g_cert_mode == PT_CERT_NONE)
        return -1;
    out->verified = 1;
    snprintf(out->subject_cn, sizeof(out->subject_cn), "%s", "client.example.com");
    snprintf(out->san, sizeof(out->san), "%s", "DNS:client.example.com,IP:10.0.0.7");
    snprintf(out->issuer_cn, sizeof(out->issuer_cn), "%s", "Test CA");
    snprintf(out->fingerprint_sha256, sizeof(out->fingerprint_sha256),
             "%s", "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    out->not_before = 1000;
    out->not_after  = 2000;
    out->der     = g_der;
    out->der_len = sizeof(g_der);
    return 0;
}

static KlTls *pt_factory(KlTlsCtx *ctx, KlAllocator *alloc) {
    (void)ctx;
    PtTls *pt = kl_malloc(alloc, sizeof(*pt));
    if (!pt) return NULL;
    memset(pt, 0, sizeof(*pt));
    pt->alloc             = alloc;
    pt->base.handshake    = pt_handshake;
    pt->base.read         = pt_read;
    pt->base.write        = pt_write;
    pt->base.shutdown     = pt_shutdown;
    pt->base.pending      = pt_pending;
    pt->base.reset        = pt_reset;
    pt->base.destroy      = pt_destroy;
    pt->base.alpn_protocol = NULL;
    pt->base.set_hostname  = NULL;
    pt->base.peer_cert     = (g_cert_mode == PT_CERT_NOSLOT) ? NULL : pt_peer_cert;
    return &pt->base;
}

/* ── Captured handler results ────────────────────────────────────────── */

static int        g_rc = -2;
static KlPeerCert g_cert;

static void handle(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    memset(&g_cert, 0xAA, sizeof(g_cert));   /* poison to prove it gets filled */
    g_rc = kl_request_peer_cert(req, &g_cert);
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void *srv_thread(void *a) { kl_server_run((KlServer *)a); return NULL; }

static int connect_local(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    return fd;
}

static void drain(int fd) {
    char buf[512];
    struct pollfd p = { .fd = fd, .events = POLLIN };
    while (poll(&p, 1, 2000) > 0) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
    }
}

/* Run one request against a server configured via cfg; captures g_rc/g_cert.
 * Returns 0 on success, -1 if setup failed (bind/connect). Not a UTEST body,
 * so it uses plain error handling rather than ASSERT_* macros. */
static int run_once(KlConfig *cfg) {
    g_rc = -2;
    KlServer srv;
    if (kl_server_init(&srv, cfg) != 0)
        return -1;
    kl_server_route(&srv, "GET", "/x", handle, NULL, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, srv_thread, &srv) != 0) {
        kl_server_free(&srv);
        return -1;
    }
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) usleep(10000);

    int rc = -1;
    if (srv.bound_port > 0) {
        int fd = connect_local(srv.bound_port);
        if (fd >= 0) {
            const char *req = "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            if (write(fd, req, strlen(req)) == (ssize_t)strlen(req)) {
                drain(fd);
                rc = 0;
            }
            close(fd);
        }
    }

    kl_server_stop(&srv);
    pthread_join(t, NULL);
    kl_server_free(&srv);
    return rc;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

UTEST(peer_cert, present_over_tls) {
    g_cert_mode = PT_CERT_PRESENT;
    KlTlsConfig tls = { .ctx = (KlTlsCtx *)1, .factory = pt_factory, .ctx_destroy = NULL };
    KlConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4, .tls = &tls };
    ASSERT_EQ(0, run_once(&cfg));

    ASSERT_EQ(0, g_rc);
    ASSERT_EQ(1, g_cert.verified);
    ASSERT_STREQ("client.example.com", g_cert.subject_cn);
    ASSERT_STREQ("DNS:client.example.com,IP:10.0.0.7", g_cert.san);
    ASSERT_STREQ("Test CA", g_cert.issuer_cn);
    ASSERT_EQ((size_t)64, strlen(g_cert.fingerprint_sha256));
    ASSERT_EQ((int64_t)1000, g_cert.not_before);
    ASSERT_EQ((int64_t)2000, g_cert.not_after);
    ASSERT_EQ(sizeof(g_der), g_cert.der_len);
    ASSERT_TRUE(g_cert.der != NULL);
}

UTEST(peer_cert, none_presented) {
    /* TLS connection, but the client sent no certificate → -1. */
    g_cert_mode = PT_CERT_NONE;
    KlTlsConfig tls = { .ctx = (KlTlsCtx *)1, .factory = pt_factory, .ctx_destroy = NULL };
    KlConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4, .tls = &tls };
    ASSERT_EQ(0, run_once(&cfg));
    ASSERT_EQ(-1, g_rc);
}

UTEST(peer_cert, backend_without_support) {
    /* TLS backend that leaves the peer_cert vtable slot NULL → -1. */
    g_cert_mode = PT_CERT_NOSLOT;
    KlTlsConfig tls = { .ctx = (KlTlsCtx *)1, .factory = pt_factory, .ctx_destroy = NULL };
    KlConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4, .tls = &tls };
    ASSERT_EQ(0, run_once(&cfg));
    ASSERT_EQ(-1, g_rc);
}

UTEST(peer_cert, plaintext_connection) {
    /* No TLS at all → -1 (nothing to extract). */
    KlConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4 };
    ASSERT_EQ(0, run_once(&cfg));
    ASSERT_EQ(-1, g_rc);
}

UTEST(peer_cert, null_args) {
    ASSERT_EQ(-1, kl_request_peer_cert(NULL, &g_cert));
    /* req==NULL path; out==NULL guarded too (no crash). */
    ASSERT_EQ(-1, kl_request_peer_cert(NULL, NULL));
}

UTEST_MAIN();
