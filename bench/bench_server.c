/*
 * bench_server.c — Dedicated benchmark server (4 endpoints)
 *
 * Endpoints:
 *   GET  /hello       — baseline: minimal JSON, no params, no middleware
 *   GET  /users/:id   — router: param extraction + snprintf response
 *   GET  /mw/hello    — middleware: same response through 2 pass-through middleware
 *   POST /echo        — body reading: KlBufReader + echo body back
 *
 * Build:  make bench
 * Run:    ./bench/bench_server [port]
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>

/* A completion event loop (BACKEND=iouringcomp) requires an overlapped socket provider —
 * the Phase-7 negotiation rejects the default POSIX provider on a completion loop. The
 * bench harness (bench/bench_compare.sh) defines KEEL_BENCH_OVERLAPPED when building against
 * the io_uring completion backend so the same bench_server.c serves all backends. */
#ifdef KEEL_BENCH_OVERLAPPED
#include "../src/socket.h"   /* internal kl_socket_provider_iouringcomp() */
#endif

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
}

static void handle_user(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    size_t id_len;
    const char *id = kl_request_param(req, "id", &id_len);
    if (!id) { kl_response_error(res, 400, "Missing id"); return; }
    static char json[128];
    int n = snprintf(json, sizeof(json),
                     "{\"id\":%.*s,\"name\":\"Alice\"}", (int)id_len, id);
    if (n < 0) n = 0;
    kl_response_json(res, 200, json, (size_t)n);
}

static int noop_mw(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)res; (void)ctx;
    return 0;
}

static void handle_echo(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        kl_response_json(res, 200, "{\"echo\":\"\"}", 11);
        return;
    }
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "application/json");
    kl_response_body_borrow(res, br->data, br->len);
}

int main(int argc, char **argv) {
    int port = 9090;
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
    KlConfig cfg = {.port = port, .install_signal_handlers = 1};
#ifdef KEEL_BENCH_OVERLAPPED
    cfg.sockets = kl_socket_provider_iouringcomp();   /* completion loop needs overlapped */
#endif
    if (kl_server_init(&s, &cfg) < 0) return 1;

    kl_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);
    kl_server_route(&s, "GET", "/users/:id", handle_user, NULL, NULL);

    kl_server_use(&s, "*", "/mw/*", noop_mw, NULL);
    kl_server_use(&s, "*", "/mw/*", noop_mw, NULL);
    kl_server_route(&s, "GET", "/mw/hello", handle_hello, NULL, NULL);

    kl_server_route(&s, "POST", "/echo", handle_echo, NULL, kl_body_reader_buffer);

    printf("bench server listening on :%d\n", port);
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
