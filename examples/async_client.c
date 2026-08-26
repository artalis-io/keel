/*
 * async_client.c: Multiple concurrent async HTTP requests
 *
 * Concepts: kl_http_client_start (async), KlEventCtx standalone event loop,
 * watcher-based completion, parallel request fan-out, error handling.
 *
 * Build:  make examples
 * Run:    ./examples/async_client
 *
 * Requires a running HTTP server on localhost:8080 (e.g. ./examples/hello_server).
 * Fires 3 concurrent GET requests and waits for all to complete.
 */

#include <keel/keel.h>
#include <stdio.h>

#define NUM_REQUESTS 3

static int completed;

static const char *urls[NUM_REQUESTS] = {
    "http://localhost:8080/hello",
    "http://localhost:8080/hello",
    "http://localhost:8080/hello",
};

static void on_done(KlHttpClient *client, void *user_data) {
    int idx = *(int *)user_data;
    printf("[%d] %s -> ", idx, urls[idx]);

    if (kl_http_client_error(client) < 0) {
        printf("ERROR\n");
    } else {
        const KlHttpClientResponse *r = kl_http_client_response(client);
        printf("status=%d body=%.*s\n", r->status,
               (int)r->body_len, r->body);
    }

    kl_http_client_free(client);
    completed++;
}

int main(void) {
    printf("async_client: firing %d concurrent requests\n\n", NUM_REQUESTS);

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) {
        fprintf(stderr, "event ctx init failed\n");
        return 1;
    }

    KlHttpClientConfig cfg = {.timeout_ms = 5000};
    int indices[NUM_REQUESTS];
    completed = 0;

    /* Fire all requests concurrently */
    for (int i = 0; i < NUM_REQUESTS; i++) {
        indices[i] = i;
        KlHttpClient *c = kl_http_client_start(&ev, &alloc, &cfg, "GET", urls[i],
                                        NULL, 0, NULL, 0, on_done, &indices[i]);
        if (!c) {
            fprintf(stderr, "[%d] failed to start request\n", i);
            completed++;
        }
    }

    /* Pump event loop until all requests complete */
    while (completed < NUM_REQUESTS) {
        if (kl_event_ctx_run(&ev, 16, 1000) < 0) break;
    }

    printf("\nAll %d requests completed.\n", NUM_REQUESTS);
    kl_event_ctx_free(&ev);
    return 0;
}
