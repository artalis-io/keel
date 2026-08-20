/*
 * alpn_server.c — a real TLS server that advertises ALPN {h2, http/1.1} and
 * serves BOTH from one shared REST layer, for external-tool ALPN interop:
 *
 *   openssl s_client -alpn h2       -connect 127.0.0.1:18443   -> ALPN: h2
 *   openssl s_client -alpn http/1.1 -connect 127.0.0.1:18443   -> ALPN: http/1.1
 *   curl -k --http2   https://127.0.0.1:18443/                 -> HTTP/2 200
 *   curl -k --http1.1 https://127.0.0.1:18443/                 -> HTTP/1.1 200
 *
 * mbedTLS supplies TLS + ALPN advertisement (kl_tls_mbedtls_ctx_set_alpn); the
 * nghttp2 adapter supplies the HTTP/2 session. The GET / route + handler are
 * protocol-independent — the same handler answers over h2 and http/1.1. Runs
 * until killed. Requires BOTH mbedTLS and nghttp2 (BYO, out of CI).
 */
#include <keel/keel.h>
#include <keel_tls_mbedtls.h>
#include "keel_h2_nghttp2.h"
#include <stdio.h>

#define ALPN_PORT 18443

/* Embedded self-signed EC cert, CN=127.0.0.1 (same test material as smoke_tls). */
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

/* One protocol-independent handler, shared by h2 and http/1.1. */
static void handle_root(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    const char *host = kl_http_request_header(req, "host");
    char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"http\":%d,\"host\":\"%s\"}",
                     req->version_major, host ? host : "");
    /* body_copy (not kl_http_response_json, which borrows) — the buffer is on the
     * stack and the HTTP/1.1 path writes the body after this returns. */
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/json");
    kl_http_response_body_copy(res, body, (size_t)n);
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();

    KlTlsCtx *ctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)CERT_PEM, sizeof(CERT_PEM),
        (const unsigned char *)KEY_PEM, sizeof(KEY_PEM),
        NULL, 0, KL_MTLS_NONE, &alloc);
    if (!ctx) { fprintf(stderr, "tls ctx failed\n"); return 1; }

    /* Advertise both protocols; mbedTLS picks h2 when the peer offers it. */
    const char *protos[] = { "h2", "http/1.1", NULL };
    if (kl_tls_mbedtls_ctx_set_alpn(ctx, protos) != 0) {
        fprintf(stderr, "set_alpn failed\n");
        kl_tls_mbedtls_ctx_destroy(ctx);
        return 1;
    }

    KlTlsConfig tls = { .ctx = ctx, .factory = kl_tls_mbedtls_create,
                        .ctx_destroy = kl_tls_mbedtls_ctx_destroy };
    KlH2ServerConfig h2 = { .factory = kl_h2_nghttp2_server_session };
    KlHttpServerConfig cfg = { .port = ALPN_PORT, .bind_addr = "127.0.0.1",
                     .tls = &tls, .h2 = &h2 };

    KlHttpServer s;
    if (kl_http_server_init(&s, &cfg) < 0) {
        fprintf(stderr, "server init failed\n");
        kl_tls_mbedtls_ctx_destroy(ctx);
        return 1;
    }
    kl_http_server_route(&s, "GET", "/", handle_root, NULL, NULL);
    fprintf(stderr, "alpn_server: TLS h2+http/1.1 on 127.0.0.1:%d\n", ALPN_PORT);
    kl_http_server_run(&s);
    kl_http_server_free(&s);
    return 0;
}
