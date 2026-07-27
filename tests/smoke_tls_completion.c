/*
 * smoke_tls_completion.c — buffered-BIO (completion-mode) TLS validation (8b-5a).
 *
 * NOT a utest suite — a standalone program built by the `smoke-tls-completion`
 * Makefile target (needs KEEL_TLS=mbedtls). It drives a full mbedTLS handshake +
 * bidirectional app data between a client and server session using ONLY the new
 * optional KlTls ops feed_input/drain_output — no socket, no event loop. This is the
 * platform-neutral proof that the memory-BIO the IOCP completion driver relies on
 * works (mbedTLS is BYO/out-of-CI, so this is a local validation gate).
 *
 * Ciphertext is shuttled between the two sessions in memory: each session's
 * drain_output is fed to the other's feed_input; handshake()/read()/write() operate
 * on those buffers (their fd argument is ignored in completion mode).
 */
#include <keel/keel.h>
#include <keel/tls_mbedtls.h>
#include <keel/tls.h>
#include <string.h>
#include <stdio.h>

/* Embedded self-signed EC cert + key (CN=127.0.0.1, SAN DNS:localhost) — same
 * test-only material as smoke_tls.c. */
static const char CERT_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIBmjCCAUGgAwIBAgIUOugIDeF4cZCY5f4MN+sk1xFwncIwCgYIKoZIzj0EAwIw\n"
"FDESMBAGA1UEAwwJMTI3LjAuMC4xMCAXDTI2MDcyNjA5MDEwMVoYDzIxMjYwNzAy\n"
"MDkwMTAxWjAUMRIwEAYDVQQDDAkxMjcuMC4wLjEwWTATBgcqhkjOPQIBBggqhkjO\n"
"PQMBBwNCAAQaZEdp4OpJZgbYIuvQiCDZE86uRuj+HQSP88xCcQ17ZSg3dWoDMRGH\n"
"DXznyJNlQ0vtbNr9Wcg4+/DAC/SLNu7Io28wbTAdBgNVHQ4EFgQUUW6CpMp5FGyE\n"
"eHRZT3D2XQUkJwowHwYDVR0jBBgwFoAUUW6CpMp5FGyEeHRZT3D2XQUkJwowDwYD\n"
"VR0TAQH/BAUwAwEB/zAaBgNVHREEEzARhwR/AAABgglsb2NhbGhvc3QwCgYIKoZI\n"
"zj0EAwIDRwAwRAIga5AdkBoyr0QJLwDzXXBKl/S4T7aplF32UlZDC3bkuusCIBNM\n"
"md2wiK5Cszs6q1wjkLLKoGtsrtjkNcFnKFdAw+zB\n"
"-----END CERTIFICATE-----\n";
static const char KEY_PEM[] =
"-----BEGIN PRIVATE KEY-----\n"
"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgZKkZoDmfdcTJNMty\n"
"gSicJ0RHBVOrlx0ISej/z1cGczKhRANCAAQaZEdp4OpJZgbYIuvQiCDZE86uRuj+\n"
"HQSP88xCcQ17ZSg3dWoDMRGHDXznyJNlQ0vtbNr9Wcg4+/DAC/SLNu7I\n"
"-----END PRIVATE KEY-----\n";

#define APP_MSG "hello-over-buffered-bio"
#define IGNORED_FD ((KlSocketHandle)-1)   /* fd is unused in completion mode */

/* Move all of `from`'s pending ciphertext into `to`'s input. Returns -1 on error. */
static int pump(KlTls *from, KlTls *to) {
    unsigned char buf[16384];
    ssize_t n;
    while ((n = from->drain_output(from, buf, sizeof(buf))) > 0)
        if (to->feed_input(to, buf, (size_t)n) < 0)
            return -1;
    return 0;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();

    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)CERT_PEM, sizeof(CERT_PEM),
        (const unsigned char *)KEY_PEM,  sizeof(KEY_PEM),
        NULL, 0, KL_MTLS_NONE, &alloc);
    KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create_from_buf(
        (const unsigned char *)CERT_PEM, sizeof(CERT_PEM), &alloc);
    if (!sctx || !cctx) { fprintf(stderr, "tls-comp: ctx create failed\n"); return 1; }

    KlTls *s = kl_tls_mbedtls_create(sctx, &alloc);
    KlTls *c = kl_tls_mbedtls_create(cctx, &alloc);
    if (!s || !c) { fprintf(stderr, "tls-comp: session create failed\n"); return 1; }

    /* The optional completion ops must be present on this backend. */
    if (!s->feed_input || !s->drain_output || !c->feed_input || !c->drain_output) {
        fprintf(stderr, "tls-comp: feed_input/drain_output not implemented\n");
        return 1;
    }
    if (c->set_hostname) c->set_hostname(c, "localhost");

    /* Enable completion (memory-BIO) mode on both before the handshake. */
    s->feed_input(s, NULL, 0);
    c->feed_input(c, NULL, 0);

    int sd = 0, cd = 0;
    for (int i = 0; i < 100 && !(sd && cd); i++) {
        if (!cd) {
            KlTlsResult r = c->handshake(c, IGNORED_FD);
            if (r == KL_TLS_OK) cd = 1;
            else if (r == KL_TLS_ERROR) { fprintf(stderr, "tls-comp: client handshake error\n"); return 1; }
        }
        if (pump(c, s) < 0) { fprintf(stderr, "tls-comp: pump c→s failed\n"); return 1; }
        if (!sd) {
            KlTlsResult r = s->handshake(s, IGNORED_FD);
            if (r == KL_TLS_OK) sd = 1;
            else if (r == KL_TLS_ERROR) { fprintf(stderr, "tls-comp: server handshake error\n"); return 1; }
        }
        if (pump(s, c) < 0) { fprintf(stderr, "tls-comp: pump s→c failed\n"); return 1; }
    }
    if (!(sd && cd)) { fprintf(stderr, "tls-comp: handshake did not complete\n"); return 1; }

    /* App data server → client. */
    char rb[64];
    if (s->write(s, IGNORED_FD, APP_MSG, sizeof(APP_MSG) - 1) != (ssize_t)(sizeof(APP_MSG) - 1) ||
        pump(s, c) < 0) { fprintf(stderr, "tls-comp: server write failed\n"); return 1; }
    ssize_t rn = c->read(c, IGNORED_FD, rb, sizeof(rb));
    if (rn != (ssize_t)(sizeof(APP_MSG) - 1) || memcmp(rb, APP_MSG, (size_t)rn) != 0) {
        fprintf(stderr, "tls-comp: client read mismatch (rn=%zd)\n", rn);
        return 1;
    }

    /* And client → server (exercises the other direction). */
    if (c->write(c, IGNORED_FD, APP_MSG, sizeof(APP_MSG) - 1) != (ssize_t)(sizeof(APP_MSG) - 1) ||
        pump(c, s) < 0) { fprintf(stderr, "tls-comp: client write failed\n"); return 1; }
    ssize_t rn2 = s->read(s, IGNORED_FD, rb, sizeof(rb));
    if (rn2 != (ssize_t)(sizeof(APP_MSG) - 1) || memcmp(rb, APP_MSG, (size_t)rn2) != 0) {
        fprintf(stderr, "tls-comp: server read mismatch (rn=%zd)\n", rn2);
        return 1;
    }

    s->destroy(s);
    c->destroy(c);
    kl_tls_mbedtls_ctx_destroy(sctx);
    kl_tls_mbedtls_ctx_destroy(cctx);
    printf("smoke-tls-completion: buffered-BIO handshake + bidirectional app data OK\n");
    return 0;
}
