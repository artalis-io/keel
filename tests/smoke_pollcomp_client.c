/*
 * smoke_pollcomp_client.c — async KlClient CONNECT over a completion loop (LC-0 proof).
 *
 * The proof for the completion CONNECT contract (KL_COMP_CONNECT / kl_comp_post_connect):
 * an async KlClient does GET / to a KlServer, BOTH on the pollcomp completion axis, and the
 * client's connect is driven over the completion loop (kl_comp_post_connect → real
 * connect()+POLLOUT+SO_ERROR → KL_COMP_CONNECT → he_on_writable) rather than the readiness
 * WRITE-watcher shim. Before LC-0 the async client's connect only "worked by luck" on
 * pollcomp; this makes it correct by design (and mirrors the io_uring / IOCP backends).
 *
 * Topology: the server runs its own completion loop on a background thread; the async client
 * runs on a SEPARATE pollcomp KlEventCtx on the main thread, driven by kl_event_ctx_run
 * (→ kl_comp_run). The client adopts the pollcomp overlapped socket provider automatically.
 * Runs in CI and under ASan+UBSan+LSan.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   /* nanosleep (the Makefile may already pass -D_DEFAULT_SOURCE) */
#endif
#include <keel/keel.h>
#include "../src/socket.h"     /* internal kl_socket_provider_pollcomp() */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PORT 18101
#define WANT "{\"lc0\":true,\"ok\":42}"

static KlServer g_srv;

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void handle_root(KlRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, WANT, sizeof(WANT) - 1);
}

static void *server_thread(void *arg) { (void)arg; kl_server_run(&g_srv); return NULL; }

/* Async client completion state. */
typedef struct { int done; int ok; } ClientState;

static void on_done(KlClient *client, void *user_data) {
    ClientState *cs = user_data;
    cs->done = 1;
    if (kl_client_error(client) == 0) {
        const KlClientResponse *r = kl_client_response(client);
        cs->ok = (r && r->status == 200 && r->body_len == sizeof(WANT) - 1 &&
                  r->body && memcmp(r->body, WANT, sizeof(WANT) - 1) == 0);
    }
}

int main(void) {
    KlConfig cfg = { .port = PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_pollcomp() };
    if (kl_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-pollcomp-client: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_server_route(&g_srv, "GET", "/", handle_root, NULL, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        kl_server_free(&g_srv);
        return 1;
    }
    nap_ms(100);   /* let the server bind + start accepting */

    /* Client-side completion event ctx (pollcomp is the compiled-in default here). */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) {
        fprintf(stderr, "smoke-pollcomp-client: client ctx init failed\n");
        kl_server_stop(&g_srv); pthread_join(th, NULL); kl_server_free(&g_srv);
        return 1;
    }

    ClientState cs = { 0, 0 };
    KlClientConfig ccfg = { .timeout_ms = 3000 };
    KlClient *client = kl_client_start(&ev, &alloc, &ccfg, "GET",
                                       "http://127.0.0.1:18101/",
                                       NULL, 0, NULL, 0, on_done, &cs);
    if (!client) {
        fprintf(stderr, "smoke-pollcomp-client: kl_client_start failed\n");
        kl_event_ctx_free(&ev);
        kl_server_stop(&g_srv); pthread_join(th, NULL); kl_server_free(&g_srv);
        return 1;
    }

    /* Drive the client's completion loop until the request finishes (or a bounded cap). */
    for (int i = 0; i < 2000 && !cs.done; i++) {
        if (kl_event_ctx_run(&ev, 16, 50) < 0) break;
    }

    kl_client_free(client);
    kl_event_ctx_free(&ev);

    kl_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_server_free(&g_srv);

    if (!cs.done || !cs.ok) {
        fprintf(stderr, "smoke-pollcomp-client: async client over completion FAILED "
                        "(done=%d ok=%d)\n", cs.done, cs.ok);
        return 1;
    }
    printf("smoke-pollcomp-client: async KlClient connect+GET over completion loop OK\n");
    return 0;
}
