#include <keel/keel.h>
#include <stdio.h>
#include <string.h>

/*
 * Demonstrates streaming request body via the built-in buffer reader.
 * POST to /api/data with any payload — echoes it back.
 * Body size limited to 1 MB.
 */

#define MAX_BODY_SIZE (1 << 20)  /* 1 MB */

static void handle_data(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        kl_response_error(res, 400, "Request body required");
        return;
    }

    /* Echo the body back */
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "application/octet-stream");
    kl_response_body(res, br->data, br->len);
}

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
}

int main(void) {
    KlServer s;
    KlConfig cfg = {.port = 8080};
    if (kl_server_init(&s, &cfg) < 0) return 1;

    /* GET /hello — no body reader needed */
    kl_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);

    /* POST /api/data — buffer reader with 1MB limit */
    kl_server_route(&s, "POST", "/api/data", handle_data,
                    (void *)(size_t)MAX_BODY_SIZE, kl_body_reader_buffer);

    printf("stream_body example listening on :8080\n");
    printf("  curl -X POST -d 'hello world' localhost:8080/api/data\n");
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
