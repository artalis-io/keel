/*
 * sse.c — Server-Sent Events (SSE) streaming
 *
 * Concepts: KlHttpSse, kl_http_sse_begin, kl_http_sse_event, kl_http_sse_comment, kl_http_sse_end.
 * Demonstrates the SSE helper for streaming events to a browser or curl.
 * Each connection receives a sequence of numbered events, then the stream ends.
 *
 * Build:  make examples
 * Run:    ./examples/sse
 * Test:   curl -N localhost:8080/events
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

static void handle_events(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;

    KlHttpSse sse;
    if (kl_http_sse_begin(res, &sse) < 0) return;

    /* Keep-alive comment (proxies won't close idle connections) */
    kl_http_sse_comment(&sse, "stream started", 14);

    /* Send 5 events with incrementing IDs */
    for (int i = 0; i < 5; i++) {
        char data[64];
        int n = snprintf(data, sizeof(data), "{\"count\":%d}", i);
        if (n < 0) break;

        char id[16];
        snprintf(id, sizeof(id), "%d", i);

        kl_http_sse_event(&sse, "tick", data, (size_t)n, id);
    }

    /* Multiline data example */
    const char *multi = "line one\nline two\nline three";
    kl_http_sse_event(&sse, "multi", multi, strlen(multi), NULL);

    kl_http_sse_end(&sse);
}

int main(void) {
    KlHttpServer s;
    KlHttpServerConfig cfg = {
        .port = 8080,
        .install_signal_handlers = 1,
    };
    if (kl_http_server_init(&s, &cfg) < 0) return 1;
    kl_http_server_route(&s, "GET", "/events", handle_events, NULL, NULL);

    printf("SSE example listening on :8080\n");
    printf("  curl -N localhost:8080/events\n");
    kl_http_server_run(&s);
    kl_http_server_free(&s);
    return 0;
}
