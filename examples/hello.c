#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>

void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
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

    KlServer s;
    KlConfig cfg = {.port = port};
    if (kl_server_init(&s, &cfg) < 0) return 1;
    kl_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
