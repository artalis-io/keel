/*
 * tls_server.c — HTTPS server using mbedTLS backend
 *
 * Concepts: KlTlsConfig, kl_tls_mbedtls_ctx_create, kl_tls_mbedtls_create,
 * cert/key loading, HTTPS on port 8443.
 *
 * Build:  make examples KEEL_TLS=mbedtls MBEDTLS_DIR=/path/to/mbedtls
 * Run:    ./examples/tls_server [cert.pem key.pem]
 * Test:   curl -k https://localhost:8443/hello
 *
 * Generate self-signed certs:
 *   openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
 *     -days 365 -nodes -subj "/CN=localhost"
 */

#include <keel/keel.h>
#include <keel_tls_mbedtls.h>
#include <stdio.h>

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"msg\":\"hello over TLS\"}", 23);
}

int main(int argc, char **argv) {
    const char *cert_path = "cert.pem";
    const char *key_path  = "key.pem";
    if (argc >= 3) {
        cert_path = argv[1];
        key_path  = argv[2];
    }

    /* Create TLS context (loads cert + key) */
    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *tls_ctx = kl_tls_mbedtls_ctx_create(cert_path, key_path,
                                                     NULL, 0, &alloc);
    if (!tls_ctx) {
        fprintf(stderr, "Failed to load %s / %s\n", cert_path, key_path);
        return 1;
    }

    KlTlsConfig tls_cfg = {
        .ctx         = tls_ctx,
        .factory     = kl_tls_mbedtls_create,
        .ctx_destroy = kl_tls_mbedtls_ctx_destroy,
    };

    KlServer s;
    KlConfig cfg = {
        .port = 8443,
        .tls  = &tls_cfg,
        .install_signal_handlers = 1,
    };
    if (kl_server_init(&s, &cfg) < 0) {
        kl_tls_mbedtls_ctx_destroy(tls_ctx);
        return 1;
    }

    kl_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);

    printf("tls example listening on :8443 (HTTPS)\n");
    printf("  curl -k https://localhost:8443/hello\n");
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
