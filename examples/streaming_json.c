#include <keel/keel.h>
#include <stdio.h>

/*
 * Demonstrates KEEL's streaming response API.
 * Any writer that accepts a (void *ctx, const char *data, size_t len)
 * callback can write directly through the response — zero intermediate
 * buffering. This is how sh_json or any custom serializer would integrate.
 */

/* Simple streaming writer — writes directly through KEEL's chunked response */
static int write_json_key(KlWriteFn write_fn, void *ctx,
                          const char *key, const char *value) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "\"%s\":\"%s\"", key, value);
    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
    return write_fn(ctx, buf, (size_t)n);
}

void handle_stream(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;

    kl_response_header(res, "Content-Type", "application/json");

    KlWriteFn write_fn;
    void *write_ctx;
    kl_response_begin_stream(res, 200, &write_fn, &write_ctx);

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

    kl_response_end_stream(res);
}

int main(void) {
    KlServer s;
    KlConfig cfg = {.port = 8080};
    if (kl_server_init(&s, &cfg) < 0) return 1;
    kl_server_route(&s, "GET", "/stream", handle_stream, NULL, NULL);
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
