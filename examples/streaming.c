/*
 * streaming.c — Streaming chunked responses
 *
 * Concepts: kl_http_response_begin_stream, KlHttpResponseWriteFn, chunked transfer-encoding.
 * Demonstrates how to stream JSON (or any data) without intermediate
 * buffering. Any serializer that accepts a write callback can plug in.
 *
 * Build:  make examples
 * Run:    ./examples/streaming
 * Test:   curl localhost:8080/stream
 */

#include <keel/keel.h>
#include <stdio.h>

/* Simple streaming writer — writes directly through KEEL's chunked response */
static int write_json_key(KlHttpResponseWriteFn write_fn, void *ctx,
                          const char *key, const char *value) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "\"%s\":\"%s\"", key, value);
    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
    return write_fn(ctx, buf, (size_t)n);
}

static void handle_stream(KlRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;

    kl_http_response_header(res, "Content-Type", "application/json");

    KlHttpResponseWriteFn write_fn;
    void *write_ctx;
    kl_http_response_begin_stream(res, 200, &write_fn, &write_ctx);

    write_fn(write_ctx, "{", 1);

    write_json_key(write_fn, write_ctx, "name", "keel");
    write_fn(write_ctx, ",", 1);
    write_json_key(write_fn, write_ctx, "version", "0.1.0");
    write_fn(write_ctx, ",", 1);

    /* Array of items */
    write_fn(write_ctx, "\"items\":[", 9);
    for (int i = 0; i < 5; i++) {
        if (i > 0) write_fn(write_ctx, ",", 1);
        char item[64];
        int n = snprintf(item, sizeof(item), "{\"id\":%d}", i);
        if (n > 0) write_fn(write_ctx, item, (size_t)n);
    }
    write_fn(write_ctx, "]", 1);

    write_fn(write_ctx, "}", 1);

    kl_http_response_end_stream(res);
}

int main(void) {
    KlServer s;
    KlConfig cfg = {
        .port = 8080,
        .install_signal_handlers = 1,
    };
    if (kl_server_init(&s, &cfg) < 0) return 1;
    kl_server_route(&s, "GET", "/stream", handle_stream, NULL, NULL);

    printf("streaming example listening on :8080\n");
    printf("  curl localhost:8080/stream\n");
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
