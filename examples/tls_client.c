/*
 * tls_client.c — HTTPS client using mbedTLS backend
 *
 * Concepts: kl_client_request with TLS, kl_tls_mbedtls_client_ctx_create,
 * KlClientConfig.tls, HTTPS GET.
 *
 * Build:  make examples KEEL_TLS=mbedtls MBEDTLS_DIR=/path/to/mbedtls
 * Run:    ./examples/tls_client [url]
 *
 * Defaults to https://localhost:8443/hello (for use with ./examples/tls_server).
 * Pass NULL to kl_tls_mbedtls_client_ctx_create to skip CA verification
 * (suitable for self-signed certs in examples).
 */

#include <keel/keel.h>
#include <keel/tls_mbedtls.h>
#include <stdio.h>

int main(int argc, char **argv) {
    const char *url = "https://localhost:8443/hello";
    if (argc > 1) url = argv[1];

    /* Create client-side TLS context (NULL = skip CA verification) */
    KlAllocator alloc = kl_allocator_default();
    KlTlsCtx *tls_ctx = kl_tls_mbedtls_client_ctx_create(NULL, &alloc);
    if (!tls_ctx) {
        fprintf(stderr, "TLS context creation failed\n");
        return 1;
    }

    KlTlsConfig tls_cfg = {
        .ctx         = tls_ctx,
        .factory     = kl_tls_mbedtls_create,
        .ctx_destroy = kl_tls_mbedtls_ctx_destroy,
    };

    KlClientConfig cfg = {
        .timeout_ms = 5000,
        .tls        = &tls_cfg,
    };
    KlClientResponse resp;

    printf("HTTPS GET %s\n", url);
    int rc = kl_client_request(&alloc, &cfg, "GET", url,
                                NULL, 0, NULL, 0, &resp);
    if (rc < 0) {
        fprintf(stderr, "request failed\n");
        kl_tls_mbedtls_ctx_destroy(tls_ctx);
        return 1;
    }

    printf("  status: %d\n", resp.status);
    printf("  body:   %.*s\n", (int)resp.body_len, resp.body);

    kl_client_response_free(&resp);
    kl_tls_mbedtls_ctx_destroy(tls_ctx);
    return 0;
}
