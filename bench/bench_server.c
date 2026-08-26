/*
 * bench_server.c: Dedicated benchmark server (4 endpoints)
 *
 * Endpoints:
 *   GET  /hello       - baseline: minimal JSON, no params, no middleware
 *   GET  /users/:id   - router: param extraction + snprintf response
 *   GET  /mw/hello    - middleware: same response through 2 pass-through middleware
 *   POST /echo        - body reading: KlHttpBufReader + echo body back
 *
 * Build:  make bench
 * Run:    ./bench/bench_server [port]
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>   /* fork, getpid: multi-worker (SO_REUSEPORT) mode */

/* Backend-agnostic: over a completion loop (BACKEND=iouring) kl_http_server_init auto-adopts the
 * backend's overlapped provider, so this default-provider server serves every backend
 * unchanged; no explicit provider needed. */

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
}

static void handle_user(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    size_t id_len;
    const char *id = kl_http_request_param(req, "id", &id_len);
    if (!id) { kl_http_response_error(res, 400, "Missing id"); return; }
    static char json[128];
    int n = snprintf(json, sizeof(json),
                     "{\"id\":%.*s,\"name\":\"Alice\"}", (int)id_len, id);
    if (n < 0) n = 0;
    kl_http_response_json(res, 200, json, (size_t)n);
}

static int noop_mw(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)res; (void)ctx;
    return 0;
}

static void handle_echo(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        kl_http_response_json(res, 200, "{\"echo\":\"\"}", 11);
        return;
    }
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/json");
    kl_http_response_body_borrow(res, br->data, br->len);
}

int main(int argc, char **argv) {
    int port = 9090;
    int workers = 1;
    if (argc > 1) {
        char *end;
        long val = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || val < 1 || val > 65535) {
            fprintf(stderr, "usage: %s [port] [workers]\n", argv[0]);
            return 1;
        }
        port = (int)val;
    }
    if (argc > 2) {
        char *end;
        long val = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || val < 1 || val > 256) {
            fprintf(stderr, "usage: %s [port] [workers]\n", argv[0]);
            return 1;
        }
        workers = (int)val;
    }

    /* Multi-worker: fork (workers-1) children; every process runs its own KlHttpServer on the
     * same port. Each listen socket sets SO_REUSEPORT (http_server.c), so the kernel load-balances
     * incoming connections across the workers, Keel's horizontal-scaling model (one
     * single-threaded accept loop per core), demonstrated here for the connection-churn bench. */
    for (int w = 1; w < workers; w++) {
        pid_t pid = fork();
        if (pid == 0) break;              /* child: stop forking, run a server */
        if (pid < 0) perror("fork");      /* parent: keep going with fewer workers */
    }

    KlHttpServer s;
    KlHttpServerConfig cfg = {.port = port, .install_signal_handlers = 1};
    if (kl_http_server_init(&s, &cfg) < 0) return 1;

    kl_http_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);
    kl_http_server_route(&s, "GET", "/users/:id", handle_user, NULL, NULL);

    kl_http_server_use(&s, "*", "/mw/*", noop_mw, NULL);
    kl_http_server_use(&s, "*", "/mw/*", noop_mw, NULL);
    kl_http_server_route(&s, "GET", "/mw/hello", handle_hello, NULL, NULL);

    kl_http_server_route(&s, "POST", "/echo", handle_echo, NULL, kl_http_body_reader_buffer);

    printf("bench server listening on :%d (worker pid %d of %d)\n",
           port, (int)getpid(), workers);
    kl_http_server_run(&s);
    kl_http_server_free(&s);
    return 0;
}
