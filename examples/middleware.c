/*
 * middleware.c: Pre/post-body middleware, CORS, and access logging
 *
 * Concepts: kl_http_server_use (pre-body), kl_http_server_use_post (post-body),
 * kl_http_cors_middleware, req->ctx for middleware-to-handler data passing.
 *
 * Middleware chain:
 *   1. request_log   (pre-body)  : logs method + path, stores start time
 *   2. CORS          (pre-body)  : allows http://localhost:3000
 *   3. auth_check    (pre-body)  : rejects /api/ without Authorization
 *   4. access_log    (post-body) : logs status + duration
 *
 * Build:  make examples
 * Run:    ./examples/middleware
 * Test:   curl localhost:8080/api/public
 *         curl -H "Authorization: Bearer token" localhost:8080/api/private
 *         curl -X OPTIONS -H "Origin: http://localhost:3000" localhost:8080/api/public
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Middleware ──────────────────────────────────────────────────────── */

/* Pre-body: log incoming request, store start time in req->ctx */
static int request_log(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    (void)res; (void)user_data;
    printf("→ %.*s %.*s\n",
           (int)req->method_len, req->method,
           (int)req->path_len, req->path);

    /* Store start time for duration calculation in access_log */
    struct timespec *ts = (struct timespec *)&req->ctx;
    clock_gettime(CLOCK_MONOTONIC, ts);
    return 0; /* continue */
}

/* Pre-body: reject /api/ requests without Authorization header */
static int auth_check(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    (void)user_data;
    const char *auth = kl_http_request_header(req, "Authorization");
    if (!auth) {
        kl_http_response_error(res, 401, "Authorization required");
        return 1; /* short-circuit */
    }
    return 0; /* continue */
}

/* Post-body: log status and duration */
static int access_log(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    (void)user_data;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    struct timespec *start = (struct timespec *)&req->ctx;
    double ms = (double)(now.tv_sec - start->tv_sec) * 1000.0
              + (double)(now.tv_nsec - start->tv_nsec) / 1e6;

    printf("← %d %.2fms\n", res->status, ms);
    return 0; /* continue */
}

/* ── Handlers ───────────────────────────────────────────────────────── */

static void handle_public(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"access\":\"public\"}", 19);
}

static void handle_private(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"access\":\"private\"}", 20);
}

static void handle_data(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        kl_http_response_error(res, 400, "Request body required");
        return;
    }
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/octet-stream");
    kl_http_response_body_borrow(res, br->data, br->len);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    KlHttpServer s;
    KlHttpServerConfig cfg = {
        .port = 8080,
        .install_signal_handlers = 1,
    };
    if (kl_http_server_init(&s, &cfg) < 0) return 1;

    /* CORS: allow requests from localhost:3000 */
    static KlHttpCorsConfig cors;
    kl_http_cors_init(&cors);
    kl_http_cors_add_origin(&cors, "http://localhost:3000");

    /* Pre-body middleware (runs before body is read) */
    kl_http_server_use(&s, "*", "/*", request_log, NULL);
    kl_http_server_use(&s, "*", "/*", kl_http_cors_middleware, &cors);
    kl_http_server_use(&s, "*", "/api/*", auth_check, NULL);

    /* Post-body middleware (runs after body is consumed) */
    kl_http_server_use_post(&s, "*", "/*", access_log, NULL);

    /* Routes */
    kl_http_server_route(&s, "GET",  "/api/public",  handle_public,  NULL, NULL);
    kl_http_server_route(&s, "GET",  "/api/private", handle_private, NULL, NULL);
    kl_http_server_route(&s, "POST", "/api/data",    handle_data,    NULL,
                    kl_http_body_reader_buffer);

    printf("middleware example listening on :8080\n");
    printf("  curl localhost:8080/api/public\n");
    printf("  curl -H 'Authorization: Bearer tok' localhost:8080/api/private\n");
    kl_http_server_run(&s);
    kl_http_server_free(&s);
    return 0;
}
