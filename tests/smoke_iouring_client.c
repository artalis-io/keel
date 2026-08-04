/*
 * smoke_iouring_client.c — async KlClient CONNECT over the io_uring completion loop (LC-0).
 *
 * The io_uring counterpart of smoke_pollcomp_client: an async KlClient does GET / to a
 * KlServer, BOTH on the io_uring completion axis, with the client's connect driven over the
 * completion loop (kl_comp_post_connect → IORING_OP_CONNECT → KL_COMP_CONNECT → he_on_writable)
 * rather than the readiness WRITE-watcher shim. Build with BACKEND=iouring first so the server
 * and the client's KlEventCtx both run on the io_uring completion loop; neither sets a socket
 * provider — the 8f-5a auto-wire adopts the backend's overlapped provider. Runs in the Apple
 * container under `make BACKEND=iouring` + ASan.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   /* nanosleep (the Makefile may already pass -D_DEFAULT_SOURCE) */
#endif
#include <keel/keel.h>

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PORT 18102
#define WANT "{\"lc0\":true,\"ring\":42}"

static KlServer g_srv;

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void handle_root(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_response_json(res, 200, WANT, sizeof(WANT) - 1);
}

static void *server_thread(void *arg) { (void)arg; kl_server_run(&g_srv); return NULL; }

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
    /* No socket provider: the completion loop auto-wires its overlapped provider (8f-5a). */
    KlConfig cfg = { .port = PORT, .bind_addr = "127.0.0.1" };
    if (kl_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-iouring-client: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_server_route(&g_srv, "GET", "/", handle_root, NULL, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        kl_server_free(&g_srv);
        return 1;
    }
    nap_ms(100);

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) {
        fprintf(stderr, "smoke-iouring-client: client ctx init failed\n");
        kl_server_stop(&g_srv); pthread_join(th, NULL); kl_server_free(&g_srv);
        return 1;
    }

    ClientState cs = { 0, 0 };
    KlClientConfig ccfg = { .timeout_ms = 3000 };
    KlClient *client = kl_client_start(&ev, &alloc, &ccfg, "GET",
                                       "http://127.0.0.1:18102/",
                                       NULL, 0, NULL, 0, on_done, &cs);
    if (!client) {
        fprintf(stderr, "smoke-iouring-client: kl_client_start failed\n");
        kl_event_ctx_free(&ev);
        kl_server_stop(&g_srv); pthread_join(th, NULL); kl_server_free(&g_srv);
        return 1;
    }

    for (int i = 0; i < 2000 && !cs.done; i++) {
        if (kl_event_ctx_run(&ev, 16, 50) < 0) break;
    }

    kl_client_free(client);
    kl_event_ctx_free(&ev);

    kl_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_server_free(&g_srv);

    if (!cs.done || !cs.ok) {
        fprintf(stderr, "smoke-iouring-client: async client over completion FAILED "
                        "(done=%d ok=%d)\n", cs.done, cs.ok);
        return 1;
    }
    printf("smoke-iouring-client: async KlClient connect+GET over io_uring completion OK\n");
    return 0;
}
