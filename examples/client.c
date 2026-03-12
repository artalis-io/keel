/*
 * client.c — HTTP/1.1 sync + async client
 *
 * Concepts: kl_client_request (sync), kl_client_start (async),
 * KlClientConfig, KlClientResponse, KlEventCtx standalone event loop.
 *
 * Build:  make examples
 * Run:    ./examples/client
 *
 * Requires a running HTTP server on localhost:8080 (e.g. ./examples/hello_server).
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

/* ── Sync client ──────────────────────────────────────────────────── */

static void sync_demo(void) {
    printf("--- Sync GET ---\n");

    KlAllocator alloc = kl_allocator_default();
    KlClientConfig cfg = {.timeout_ms = 5000};
    KlClientResponse resp;

    int rc = kl_client_request(&alloc, &cfg, "GET",
                                "http://localhost:8080/hello",
                                NULL, 0, NULL, 0, &resp);
    if (rc < 0) {
        fprintf(stderr, "sync request failed\n");
        return;
    }

    printf("  status: %d\n", resp.status);
    printf("  body:   %.*s\n", (int)resp.body_len, resp.body);
    printf("  headers (%d):\n", resp.num_headers);
    for (int i = 0; i < resp.num_headers; i++) {
        printf("    %s: %s\n", resp.headers[i].name, resp.headers[i].value);
    }

    kl_client_response_free(&resp);
}

/* ── Async client ─────────────────────────────────────────────────── */

static int async_done_flag;

static void on_done(KlClient *client, void *user_data) {
    (void)user_data;
    printf("--- Async GET completed ---\n");

    if (kl_client_error(client) < 0) {
        fprintf(stderr, "  async request failed\n");
    } else {
        const KlClientResponse *r = kl_client_response(client);
        printf("  status: %d\n", r->status);
        printf("  body:   %.*s\n", (int)r->body_len, r->body);
    }

    kl_client_free(client);
    async_done_flag = 1;
}

static void async_demo(void) {
    printf("--- Async GET ---\n");

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) {
        fprintf(stderr, "event ctx init failed\n");
        return;
    }

    KlClientConfig cfg = {.timeout_ms = 5000};
    async_done_flag = 0;

    KlClient *c = kl_client_start(&ev, &alloc, &cfg, "GET",
                                    "http://localhost:8080/hello",
                                    NULL, 0, NULL, 0, on_done, NULL);
    if (!c) {
        fprintf(stderr, "async start failed\n");
        kl_event_ctx_free(&ev);
        return;
    }

    /* Pump the event loop until the request completes */
    while (!async_done_flag) {
        if (kl_event_ctx_run(&ev, 8, 1000) < 0) break;
    }

    kl_event_ctx_free(&ev);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    printf("HTTP client example\n");
    printf("Requires: ./examples/hello_server running on :8080\n\n");

    sync_demo();
    printf("\n");
    async_demo();

    return 0;
}
