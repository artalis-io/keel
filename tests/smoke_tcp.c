/*
 * smoke_tcp.c — plaintext TCP link + roundtrip smoke test.
 *
 * NOT a utest suite (not tests/test_*.c) — a standalone program built by the
 * `smoke` Makefile target. It links the whole TCP core (KlServer + sync
 * KlClient) and drives one real request over loopback, proving the library
 * both links and runs on the target platform (the Windows CI gate). Runs on
 * POSIX too.
 *
 * A server runs on a background thread; main hits it with the sync client and
 * checks the response, then stops the server and joins.
 */

#include <keel/keel.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

/* Test-local sleep (this is a test program, not a library TU). */
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

#define SMOKE_PORT 18080
#define SMOKE_BODY "{\"ok\":true}"

static void handle_ok(KlRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, SMOKE_BODY, sizeof(SMOKE_BODY) - 1);
}

static KlServer g_srv;

static void *server_thread(void *arg) {
    (void)arg;
    kl_server_run(&g_srv);
    return NULL;
}

int main(void) {
    KlConfig cfg = { .port = SMOKE_PORT, .bind_addr = "127.0.0.1" };
    if (kl_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke: server init failed\n");
        return 1;
    }
    kl_server_route(&g_srv, "GET", "/", handle_ok, NULL, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "smoke: pthread_create failed\n");
        kl_server_free(&g_srv);
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlClientConfig ccfg = { .timeout_ms = 1000 };
    int ok = 0;
    int last_rc = -1, last_status = -1, last_err = 0;
    size_t last_len = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        nap_ms(50);   /* let the listener come up; retry if not yet bound */
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        last_rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18080/",
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
    }

    kl_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_server_free(&g_srv);

    if (!ok) {
        fprintf(stderr, "smoke: roundtrip FAILED (rc=%d status=%d body_len=%zu err=%d)\n",
                last_rc, last_status, last_len, last_err);
        return 1;
    }
    printf("smoke: TCP roundtrip OK\n");
    return 0;
}
