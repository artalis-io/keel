/*
 * hello_server.c — Minimal KEEL HTTP server
 *
 * Concepts: KlHttpServer, KlHttpServerConfig, single route, JSON response.
 *
 * Build:  make examples
 * Run:    ./examples/hello_server [port]
 * Test:   curl localhost:8080/hello
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
}

int main(int argc, char **argv) {
    int port = 8080;
    if (argc > 1) {
        char *end;
        long val = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || val < 1 || val > 65535) {
            fprintf(stderr, "usage: %s [port]\n", argv[0]);
            return 1;
        }
        port = (int)val;
    }

    KlHttpServer s;
    KlHttpServerConfig cfg = {
        .port = port,
        .install_signal_handlers = 1,
    };
    if (kl_http_server_init(&s, &cfg) < 0) return 1;
    kl_http_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);

    printf("hello example listening on :%d\n", port);
    printf("  curl localhost:%d/hello\n", port);
    kl_http_server_run(&s);
    kl_http_server_free(&s);
    return 0;
}
