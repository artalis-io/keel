/*
 * redirect_client.c: HTTP redirect following (sync + async)
 *
 * Self-contained: starts a local server with redirect routes, then
 * demonstrates the redirect client API with automatic 3xx following.
 *
 * Build:  make examples
 * Run:    ./examples/redirect_client
 */

#include <keel/keel.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* ── Test server (runs in background thread) ─────────────────────── */

static KlHttpServer srv;
static int srv_port;

static void handle_final(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/json");
    char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"method\":\"%s\",\"message\":\"arrived!\"}", req->method);
    kl_http_response_body_copy(res, body, (size_t)n);
}

static void handle_redirect(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 301);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/final", srv_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "moved", 5);
}

static void handle_chain(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 302);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/redirect", srv_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "chain", 5);
}

static void handle_post_redirect(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 303);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/final", srv_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "see other", 9);
}

static void *server_thread(void *arg) {
    kl_http_server_run((KlHttpServer *)arg);
    return NULL;
}

/* ── Sync demo ───────────────────────────────────────────────────── */

static void sync_demo(void) {
    KlAllocator alloc = kl_allocator_default();
    KlHttpClientConfig cfg = {.timeout_ms = 5000};
    KlHttpClientResponse resp;
    char url[256];

    /* 1. Simple 301 redirect */
    printf("--- Sync: 301 redirect ---\n");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/redirect", srv_port);
    if (kl_http_redirect_request(&alloc, &cfg, NULL, "GET", url,
                             NULL, 0, NULL, 0, &resp) == 0) {
        printf("  status: %d\n", resp.status);
        printf("  body:   %.*s\n", (int)resp.body_len, resp.body);
        kl_http_client_response_free(&resp);
    }

    /* 2. Chain: 302 → 301 → 200 */
    printf("\n--- Sync: redirect chain (302 → 301 → 200) ---\n");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/chain", srv_port);
    if (kl_http_redirect_request(&alloc, &cfg, NULL, "GET", url,
                             NULL, 0, NULL, 0, &resp) == 0) {
        printf("  status: %d\n", resp.status);
        printf("  body:   %.*s\n", (int)resp.body_len, resp.body);
        kl_http_client_response_free(&resp);
    }

    /* 3. POST with 303 → method changes to GET */
    printf("\n--- Sync: 303 POST → GET ---\n");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/post_redirect", srv_port);
    if (kl_http_redirect_request(&alloc, &cfg, NULL, "POST", url,
                             NULL, 0, "payload", 7, &resp) == 0) {
        printf("  status: %d (method changed to GET)\n", resp.status);
        printf("  body:   %.*s\n", (int)resp.body_len, resp.body);
        kl_http_client_response_free(&resp);
    }

    /* 4. Custom max_redirects */
    printf("\n--- Sync: max_redirects = 1 on a chain ---\n");
    KlHttpRedirectConfig redir = {.max_redirects = 1};
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/chain", srv_port);
    int rc = kl_http_redirect_request(&alloc, &cfg, &redir, "GET", url,
                                  NULL, 0, NULL, 0, &resp);
    if (rc < 0) {
        printf("  error: %s (expected: chain needs 2 hops, limit is 1)\n",
               kl_strerror(resp.error));
    } else {
        kl_http_client_response_free(&resp);
    }
}

/* ── Async demo ──────────────────────────────────────────────────── */

static int async_done;

static void on_redirect_done(KlHttpRedirectClient *rc, void *user_data) {
    (void)user_data;
    printf("--- Async: redirect completed ---\n");

    if (kl_http_redirect_error(rc) == 0) {
        const KlHttpClientResponse *r = kl_http_redirect_response(rc);
        printf("  status: %d\n", r->status);
        printf("  body:   %.*s\n", (int)r->body_len, r->body);
    } else {
        printf("  error: %s\n", kl_strerror(kl_http_redirect_last_error(rc)));
    }

    kl_http_redirect_free(rc);
    async_done = 1;
}

static void async_demo(void) {
    printf("\n--- Async: 301 redirect ---\n");

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) < 0) {
        fprintf(stderr, "event ctx init failed\n");
        return;
    }

    KlHttpClientConfig cfg = {.timeout_ms = 5000};
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/redirect", srv_port);

    async_done = 0;
    KlHttpRedirectClient *rc = kl_http_redirect_start(&ev, &alloc, &cfg, NULL,
                                              "GET", url,
                                              NULL, 0, NULL, 0,
                                              on_redirect_done, NULL);
    if (!rc) {
        fprintf(stderr, "async redirect start failed\n");
        kl_event_ctx_free(&ev);
        return;
    }

    while (!async_done) {
        if (kl_event_ctx_run(&ev, 8, 1000) < 0) break;
    }

    kl_event_ctx_free(&ev);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    printf("HTTP redirect client example\n\n");

    /* Start a local server with redirect routes */
    KlHttpServerConfig cfg = {.port = 0, .max_connections = 8, .max_body_size = 4096};
    kl_http_server_init(&srv, &cfg);
    kl_http_server_route(&srv, "GET", "/final", handle_final, NULL, NULL);
    kl_http_server_route(&srv, "*", "/redirect", handle_redirect, NULL, NULL);
    kl_http_server_route(&srv, "*", "/chain", handle_chain, NULL, NULL);
    kl_http_server_route(&srv, "*", "/post_redirect", handle_post_redirect, NULL, NULL);

    pthread_t tid;
    pthread_create(&tid, NULL, server_thread, &srv);
    for (int i = 0; i < 200 && kl_http_server_bound_port(&srv) == 0; i++) usleep(10000);
    srv_port = kl_http_server_bound_port(&srv);

    sync_demo();
    async_demo();

    /* Clean up */
    kl_http_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&srv);

    printf("\nDone.\n");
    return 0;
}
