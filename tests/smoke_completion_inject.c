/*
 * smoke_completion_inject.c — the RC-2 proof: a DEFAULT (readiness-compiled: epoll on
 * Linux / kqueue on macOS) libkeel serving GET / over a RUNTIME-INJECTED pollcomp
 * COMPLETION backend. Impossible before RC-2.
 *
 * Before the provider/glue split, event_pollcomp.c defined the kl_event_*_builtin +
 * kl_comp_ops_builtin free functions, so it could ONLY be the compiled-in backend
 * (BACKEND=pollcomp) — linking it into a default lib duplicate-symbol-clashed with the
 * default backend's own _builtin. Now event_pollcomp.c is a PURE runtime provider (static
 * KlEventOps + KlCompletionOps + kl_event_provider_pollcomp(), no _builtin), so it links
 * as an extra object next to a default libkeel.a and injects its completion axis at runtime
 * via KlHttpServerConfig.event_provider.
 *
 * The server auto-wires the matched overlapped socket provider from the injected loop's
 * native_provider (server.c 8f-5a), exactly as the compiled-in completion path does — the
 * caller sets only event_provider. A blocking loopback client does GET / and asserts 200 +
 * the expected body. Prints COMPLETION-INJECT PASS on success.
 *
 * Built by `make smoke-completion-inject` (default lib + event_pollcomp.inject.o), and
 * under ASan by `make smoke-completion-inject-asan`.
 */
#include <keel/keel.h>
#include "event_pollcomp_internal.h"   /* internal: kl_event_provider_pollcomp() */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>          /* struct timeval (SO_RCVTIMEO) */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define SMOKE_PORT 18142
#define SMOKE_BODY "{\"inject\":\"pollcomp\"}"

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

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
    /* An OTHERWISE-DEFAULT server: the ONLY non-default knob is the injected completion
     * provider. No .sockets — the server auto-adopts the provider's overlapped native
     * provider (server.c 8f-5a) since the injected loop advertises KL_EVENT_CAP_COMPLETION. */
    KlHttpServerConfig cfg = { .port = SMOKE_PORT, .bind_addr = "127.0.0.1",
                     .event_provider = kl_event_provider_pollcomp() };
    if (kl_http_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-inject: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_http_server_route(&g_srv, "GET", "/", handle_ok, NULL, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "smoke-inject: pthread_create failed\n");
        kl_http_server_free(&g_srv);
        return 1;
    }
    for (int i = 0; i < 200 && g_srv.bound_port == 0; i++) nap_ms(5);

    KlAllocator alloc = kl_allocator_default();
    KlClientConfig ccfg = { .timeout_ms = 1000, .max_response_size = 256 * 1024 };
    int ok = 0, last_rc = -1, last_status = -1;
    for (int i = 0; i < 50 && !ok; i++) {
        nap_ms(50);
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        last_rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18142/",
                                    NULL, 0, NULL, 0, &resp);
        if (last_rc == 0) {
            last_status = resp.status;
            ok = (resp.status == 200 && resp.body_len == sizeof(SMOKE_BODY) - 1 &&
                  resp.body && memcmp(resp.body, SMOKE_BODY, sizeof(SMOKE_BODY) - 1) == 0);
            kl_client_response_free(&resp);
        }
    }

    kl_http_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_http_server_free(&g_srv);

    if (!ok) {
        fprintf(stderr, "smoke-inject: GET / over injected pollcomp FAILED (rc=%d status=%d)\n",
                last_rc, last_status);
        return 1;
    }
    printf("COMPLETION-INJECT PASS: default libkeel served GET / over a runtime-injected "
           "pollcomp completion backend\n");
    return 0;
}
