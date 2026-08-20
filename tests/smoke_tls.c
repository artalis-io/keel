/*
 * smoke_tls.c — real mbedTLS handshake link + roundtrip smoke test.
 *
 * NOT a utest suite — a standalone program built by the `smoke-tls` Makefile
 * target (needs KEEL_TLS=mbedtls). It wires the mbedTLS backend into a KlHttpServer
 * and a sync KlClient and drives one real HTTPS request over loopback, proving
 * the backend both links and completes a genuine TLS handshake on the target
 * platform. This is the BYO/local validation gate for the mbedTLS backend on
 * POSIX and Windows (mbedTLS is not in CI). An embedded self-signed cert keeps
 * it filesystem-free; the client skips CA verification (self-signed loopback).
 *
 * A server runs on a background thread; main hits it with the sync HTTPS client
 * and checks the response, then stops the server and joins.
 */

#include <keel/keel.h>
#include <keel_tls_mbedtls.h>
#ifdef SMOKE_TLS_COMPLETION
#include "../src/event_caps.h"   /* kl_event_caps — assert the server runs on a completion loop */
#endif
#include <pthread.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
static void nap_ms(int ms) { Sleep(ms); }
#else
#include <time.h>
static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

#define SMOKE_PORT 18443
#define SMOKE_BODY "{\"tls\":true}"

/* Embedded self-signed EC (P-256) cert + key, CN=127.0.0.1,
 * SAN IP:127.0.0.1/DNS:localhost, valid ~100 years. Test-only material. */
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

static void handle_ok(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, SMOKE_BODY, sizeof(SMOKE_BODY) - 1);
}

static KlHttpServer g_srv;

static void *server_thread(void *arg) {
    (void)arg;
    kl_http_server_run(&g_srv);
    return NULL;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();

    /* Server-side TLS context from embedded cert/key (sizeof includes the NUL,
     * which mbedTLS's PEM parser requires). */
    KlTlsCtx *srv_ctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)CERT_PEM, sizeof(CERT_PEM),
        (const unsigned char *)KEY_PEM,  sizeof(KEY_PEM),
        NULL, 0, KL_MTLS_NONE, &alloc);
    if (!srv_ctx) {
        fprintf(stderr, "smoke-tls: server TLS ctx create failed\n");
        return 1;
    }
    KlTlsConfig srv_tls = {
        .ctx = srv_ctx, .factory = kl_tls_mbedtls_create,
        .ctx_destroy = kl_tls_mbedtls_ctx_destroy,
    };
    /* Default provider: on a completion backend (BACKEND=pollcomp|iouring) the server auto-adopts
     * the overlapped provider (5a), so this same smoke drives real mbedTLS over comp_tls_drive /
     * the memory-BIO feed_input/drain_output path on an actual event loop + socket — the full e2e
     * counterpart to the in-memory smoke-tls-completion. The SMOKE_TLS_COMPLETION build asserts the
     * loop really is completion (below). */
    KlHttpServerConfig cfg = { .port = SMOKE_PORT, .bind_addr = "127.0.0.1", .tls = &srv_tls };
    if (kl_http_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-tls: server init failed\n");
        kl_tls_mbedtls_ctx_destroy(srv_ctx);
        return 1;
    }
    kl_http_server_route(&g_srv, "GET", "/", handle_ok, NULL, NULL);

#ifdef SMOKE_TLS_COMPLETION
    /* Guard: this gate is meaningless unless the loop really is completion — fail loudly if built
     * against a readiness backend (build with BACKEND=pollcomp or BACKEND=iouring). */
    if (!(kl_event_caps(&g_srv.ev.loop) & KL_EVENT_CAP_COMPLETION)) {
        fprintf(stderr, "smoke-tls: not a completion loop — build with BACKEND=pollcomp|iouring\n");
        kl_http_server_free(&g_srv);
        kl_tls_mbedtls_ctx_destroy(srv_ctx);
        return 1;
    }
#endif

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "smoke-tls: pthread_create failed\n");
        kl_http_server_free(&g_srv);
        return 1;
    }

    /* Client-side TLS: NULL CA = skip verification (self-signed loopback). */
    int ok = 0, last_rc = -1, last_status = -1, last_err = 0;
    size_t last_len = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        nap_ms(50);
        KlTlsCtx *cli_ctx = kl_tls_mbedtls_client_ctx_create(NULL, &alloc);
        if (!cli_ctx) continue;
        KlTlsConfig cli_tls = {
            .ctx = cli_ctx, .factory = kl_tls_mbedtls_create,
            .ctx_destroy = kl_tls_mbedtls_ctx_destroy,
        };
        KlClientConfig ccfg = { .timeout_ms = 2000, .tls = &cli_tls };
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        char url[48];
        snprintf(url, sizeof(url), "https://127.0.0.1:%d/", SMOKE_PORT);
        last_rc = kl_client_request(&alloc, &ccfg, "GET", url,
                                    NULL, 0, NULL, 0, &resp);
        if (last_rc == 0) {
            last_status = resp.status;
            last_len = resp.body_len;
            ok = (resp.status == 200 &&
                  resp.body_len == sizeof(SMOKE_BODY) - 1 &&
                  resp.body && memcmp(resp.body, SMOKE_BODY, sizeof(SMOKE_BODY) - 1) == 0);
            kl_client_response_free(&resp);
        } else {
            last_err = (int)resp.error;
        }
        kl_tls_mbedtls_ctx_destroy(cli_ctx);
    }

    kl_http_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_http_server_free(&g_srv);

    if (!ok) {
        fprintf(stderr, "smoke-tls: HTTPS roundtrip FAILED (rc=%d status=%d body_len=%zu err=%d)\n",
                last_rc, last_status, last_len, last_err);
        return 1;
    }
#ifdef SMOKE_TLS_COMPLETION
    printf("smoke-tls: HTTPS handshake + roundtrip OK (over the completion loop)\n");
#else
    printf("smoke-tls: HTTPS handshake + roundtrip OK\n");
#endif
    return 0;
}
