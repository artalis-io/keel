/*
 * test_redirect.c — Tests for URL resolution and redirect following
 */

#include "utest.h"
#include <keel/keel.h>

#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* ══════════════════════════════════════════════════════════════════════
 * Part 1: URL resolution tests (pure, no server needed)
 * ══════════════════════════════════════════════════════════════════════ */

UTEST(url_resolve, absolute_http) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://old.com/page", "http://other.com/path",
                              out, sizeof(out)), 0);
    ASSERT_STREQ(out, "http://other.com/path");
}

UTEST(url_resolve, absolute_https) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://old.com/page", "https://other.com/path",
                              out, sizeof(out)), 0);
    ASSERT_STREQ(out, "https://other.com/path");
}

UTEST(url_resolve, protocol_relative) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("https://old.com/page", "//other.com/path",
                              out, sizeof(out)), 0);
    ASSERT_STREQ(out, "https://other.com/path");
}

UTEST(url_resolve, protocol_relative_http) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://old.com/page", "//other.com/path",
                              out, sizeof(out)), 0);
    ASSERT_STREQ(out, "http://other.com/path");
}

UTEST(url_resolve, absolute_path) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://host:8080/old/path", "/new/path",
                              out, sizeof(out)), 0);
    ASSERT_STREQ(out, "http://host:8080/new/path");
}

UTEST(url_resolve, absolute_path_no_port) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://host/old", "/new",
                              out, sizeof(out)), 0);
    ASSERT_STREQ(out, "http://host/new");
}

UTEST(url_resolve, strip_fragment) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://host/old", "http://host/path#frag",
                              out, sizeof(out)), 0);
    ASSERT_STREQ(out, "http://host/path");
}

UTEST(url_resolve, strip_fragment_path) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://host/old", "/path#frag",
                              out, sizeof(out)), 0);
    ASSERT_STREQ(out, "http://host/path");
}

UTEST(url_resolve, null_args) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve(NULL, "/path", out, sizeof(out)), -1);
    ASSERT_EQ(kl_url_resolve("http://h", NULL, out, sizeof(out)), -1);
    ASSERT_EQ(kl_url_resolve("http://h", "/path", NULL, sizeof(out)), -1);
    ASSERT_EQ(kl_url_resolve("http://h", "/path", out, 0), -1);
}

UTEST(url_resolve, empty_location) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://host/old", "", out, sizeof(out)), -1);
}

UTEST(url_resolve, buffer_too_small) {
    char out[8];
    ASSERT_EQ(kl_url_resolve("http://host/old", "http://host/path",
                              out, sizeof(out)), -1);
}

UTEST(url_resolve, bare_relative) {
    char out[KL_URL_MAX];
    ASSERT_EQ(kl_url_resolve("http://host/old", "../foo", out, sizeof(out)), -1);
    ASSERT_EQ(kl_url_resolve("http://host/old", "foo", out, sizeof(out)), -1);
}

/* ══════════════════════════════════════════════════════════════════════
 * Part 2: Integration tests — redirect following with real test server
 *
 * Servers are started ONCE before all tests and stopped after.
 * This avoids per-test server lifecycle issues.
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Test server globals ─────────────────────────────────────────── */

static KlHttpServer redir_srv;
static pthread_t redir_tid;
static int redir_port;

static KlHttpServer redir_srv2;
static pthread_t redir_tid2;
static int redir_port2;

static int servers_started;

static void *server_thread(void *arg) {
    kl_http_server_run((KlHttpServer *)arg);
    return NULL;
}

static void wait_for_bind(KlHttpServer *s) {
    for (int i = 0; i < 200 && s->bound_port == 0; i++) usleep(10000);
}

static char url_buf[256];
static const char *test_url(const char *path) {
    snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d%s", redir_port, path);
    return url_buf;
}

/* ── Server handlers ─────────────────────────────────────────────── */

static void handle_dest(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "text/plain");
    kl_http_response_body_borrow(res, req->method, strlen(req->method));
}

static void handle_echo(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "text/plain");
    if (br && br->len > 0)
        kl_http_response_body_borrow(res, br->data, br->len);
    else
        kl_http_response_body_borrow(res, "no body", 7);
}

static void handle_auth_check(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)ctx;
    const char *auth = kl_http_request_header(req, "Authorization");
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "text/plain");
    if (auth)
        kl_http_response_body_borrow(res, auth, strlen(auth));
    else
        kl_http_response_body_borrow(res, "none", 4);
}

static void handle_301(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 301);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/dest", redir_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "moved", 5);
}

static void handle_302(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 302);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/dest", redir_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "found", 5);
}

static void handle_303(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 303);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/dest", redir_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "see other", 9);
}

static void handle_307(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 307);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/echo", redir_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "temp", 4);
}

static void handle_308(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 308);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/echo", redir_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "perm", 4);
}

static void handle_relative(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 301);
    kl_http_response_header(res, "Location", "/dest");
    kl_http_response_body_borrow(res, "moved", 5);
}

static void handle_no_location(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 301);
    kl_http_response_body_borrow(res, "no location", 11);
}

static void handle_chain(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 301);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/redir302", redir_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "chain", 5);
}

static void handle_loop(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 301);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/loop", redir_port);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "loop", 4);
}

static void handle_cross_origin(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_status(res, 301);
    char loc[128];
    snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/auth_check", redir_port2);
    kl_http_response_header(res, "Location", loc);
    kl_http_response_body_borrow(res, "cross", 5);
}

/* ── Server lifecycle (once per test run) ────────────────────────── */

static void ensure_servers(void) {
    if (servers_started) return;
    servers_started = 1;

    KlHttpServerConfig cfg = { .port = 0, .max_connections = 16, .max_body_size = 4096 };
    kl_http_server_init(&redir_srv, &cfg);

    kl_http_server_route(&redir_srv, "*", "/dest", handle_dest, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/echo", handle_echo, (void *)(size_t)4096,
                    kl_http_body_reader_buffer);
    kl_http_server_route(&redir_srv, "*", "/auth_check", handle_auth_check, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/redir301", handle_301, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/redir302", handle_302, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/redir303", handle_303, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/redir307", handle_307, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/redir308", handle_308, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/relative", handle_relative, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/no_location", handle_no_location, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/chain", handle_chain, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/loop", handle_loop, NULL, NULL);
    kl_http_server_route(&redir_srv, "*", "/cross_origin", handle_cross_origin, NULL, NULL);

    pthread_create(&redir_tid, NULL, server_thread, &redir_srv);
    wait_for_bind(&redir_srv);
    redir_port = redir_srv.bound_port;

    KlHttpServerConfig cfg2 = { .port = 0, .max_connections = 4 };
    kl_http_server_init(&redir_srv2, &cfg2);
    kl_http_server_route(&redir_srv2, "*", "/auth_check", handle_auth_check, NULL, NULL);

    pthread_create(&redir_tid2, NULL, server_thread, &redir_srv2);
    wait_for_bind(&redir_srv2);
    redir_port2 = redir_srv2.bound_port;
}

/* Called via atexit — stops servers once when process exits */
static void cleanup_servers(void) {
    if (!servers_started) return;
    kl_http_server_stop(&redir_srv);
    pthread_join(redir_tid, NULL);
    kl_http_server_free(&redir_srv);

    kl_http_server_stop(&redir_srv2);
    pthread_join(redir_tid2, NULL);
    kl_http_server_free(&redir_srv2);
    servers_started = 0;
}

/* ── Sync redirect tests ────────────────────────────────────────── */

#define TEST_TIMEOUT_MS 5000

UTEST(redirect, sync_301) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "GET", test_url("/redir301"),
                                   NULL, 0, NULL, 0, &resp), 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(resp.body_len, 3u);
    ASSERT_TRUE(memcmp(resp.body, "GET", 3) == 0);

    kl_http_client_response_free(&resp);
}

UTEST(redirect, sync_302_post_to_get) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "POST", test_url("/redir302"),
                                   NULL, 0, "data", 4, &resp), 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(resp.body_len, 3u);
    ASSERT_TRUE(memcmp(resp.body, "GET", 3) == 0);

    kl_http_client_response_free(&resp);
}

UTEST(redirect, sync_303) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;

    /* 303: PUT + body → GET (303 converts any non-GET/HEAD to GET) */
    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "PUT", test_url("/redir303"),
                                   NULL, 0, "data", 4, &resp), 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(resp.body_len, 3u);
    ASSERT_TRUE(memcmp(resp.body, "GET", 3) == 0);

    kl_http_client_response_free(&resp);
}

UTEST(redirect, sync_307_preserve) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "POST", test_url("/redir307"),
                                   NULL, 0, "hello", 5, &resp), 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(resp.body_len, 5u);
    ASSERT_TRUE(memcmp(resp.body, "hello", 5) == 0);

    kl_http_client_response_free(&resp);
}

UTEST(redirect, sync_308_preserve) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "POST", test_url("/redir308"),
                                   NULL, 0, "world", 5, &resp), 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(resp.body_len, 5u);
    ASSERT_TRUE(memcmp(resp.body, "world", 5) == 0);

    kl_http_client_response_free(&resp);
}

UTEST(redirect, sync_no_redirect) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "GET", test_url("/dest"),
                                   NULL, 0, NULL, 0, &resp), 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(resp.body_len, 3u);

    kl_http_client_response_free(&resp);
}

UTEST(redirect, sync_max_exceeded) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpRedirectConfig redir = { .max_redirects = 2 };
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, &redir, "GET", test_url("/loop"),
                                   NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(resp.error, KL_ERR_REDIRECT_LOOP);
}

UTEST(redirect, sync_missing_location) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "GET", test_url("/no_location"),
                                   NULL, 0, NULL, 0, &resp), 0);
    ASSERT_EQ(resp.status, 301);

    kl_http_client_response_free(&resp);
}

UTEST(redirect, sync_relative_path) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "GET", test_url("/relative"),
                                   NULL, 0, NULL, 0, &resp), 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(resp.body_len, 3u);

    kl_http_client_response_free(&resp);
}

UTEST(redirect, sync_cross_origin_drops_auth) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;
    KlHttpClientHeader headers[] = {
        { "Authorization", "Bearer secret123" }
    };

    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "GET",
                                   test_url("/cross_origin"),
                                   headers, 1, NULL, 0, &resp), 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(resp.body_len, 4u);
    ASSERT_TRUE(memcmp(resp.body, "none", 4) == 0);

    kl_http_client_response_free(&resp);
}

UTEST(redirect, sync_null_args) {
    KlHttpClientResponse resp;
    ASSERT_EQ(kl_http_redirect_request(NULL, NULL, NULL, "GET", "http://x",
                                   NULL, 0, NULL, 0, &resp), -1);
}

UTEST(redirect, sync_pooled) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, NULL), 0);
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_redirect_request_pooled(&pool, &a, &cfg, NULL, "GET",
                                          test_url("/redir301"),
                                          NULL, 0, NULL, 0, &resp), 0);
    ASSERT_EQ(resp.status, 200);

    kl_http_client_response_free(&resp);
    kl_http_client_pool_free(&pool);
}

UTEST(redirect, sync_chain) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpClientResponse resp;

    /* /chain → 301 → /redir302 → 302 → /dest → 200 */
    ASSERT_EQ(kl_http_redirect_request(&a, &cfg, NULL, "GET", test_url("/chain"),
                                   NULL, 0, NULL, 0, &resp), 0);
    ASSERT_EQ(resp.status, 200);
    ASSERT_TRUE(resp.body != NULL);
    ASSERT_EQ(resp.body_len, 3u);

    kl_http_client_response_free(&resp);
}

/* ── Async redirect tests ───────────────────────────────────────── */

typedef struct {
    int    done;
    int    error;
    int    status;
    char   body[512];
    size_t body_len;
    KlError last_error;
} AsyncRedirCtx;

static void async_redir_done(KlHttpRedirectClient *rc, void *user_data) {
    AsyncRedirCtx *ctx = user_data;
    ctx->error = kl_http_redirect_error(rc);
    ctx->last_error = kl_http_redirect_last_error(rc);
    if (ctx->error == 0) {
        const KlHttpClientResponse *resp = kl_http_redirect_response(rc);
        if (resp) {
            ctx->status = resp->status;
            if (resp->body && resp->body_len > 0) {
                size_t n = resp->body_len < sizeof(ctx->body) - 1
                         ? resp->body_len : sizeof(ctx->body) - 1;
                memcpy(ctx->body, resp->body, n);
                ctx->body[n] = '\0';
                ctx->body_len = n;
            }
        }
    }
    ctx->done = 1;
}

static int run_until_done(KlEventCtx *ev, AsyncRedirCtx *ctx, int timeout_ms) {
    int elapsed = 0;
    while (!ctx->done && elapsed < timeout_ms) {
        int n = kl_event_ctx_run(ev, 16, 50);
        if (n < 0) return -1;
        elapsed += 50;
    }
    return ctx->done ? 0 : -1;
}

UTEST(redirect, async_301) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };

    AsyncRedirCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlHttpRedirectClient *rc = kl_http_redirect_start(&ev, &a, &cfg, NULL,
                                             "GET", test_url("/redir301"),
                                             NULL, 0, NULL, 0,
                                             async_redir_done, &ctx);
    ASSERT_TRUE(rc != NULL);

    ASSERT_EQ(run_until_done(&ev, &ctx, TEST_TIMEOUT_MS), 0);
    ASSERT_TRUE(ctx.done);
    ASSERT_EQ(ctx.error, 0);
    ASSERT_EQ(ctx.status, 200);
    ASSERT_EQ(ctx.body_len, 3u);
    ASSERT_TRUE(memcmp(ctx.body, "GET", 3) == 0);

    kl_http_redirect_free(rc);
    kl_event_ctx_free(&ev);
}

UTEST(redirect, async_chain) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };

    AsyncRedirCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlHttpRedirectClient *rc = kl_http_redirect_start(&ev, &a, &cfg, NULL,
                                             "GET", test_url("/chain"),
                                             NULL, 0, NULL, 0,
                                             async_redir_done, &ctx);
    ASSERT_TRUE(rc != NULL);

    ASSERT_EQ(run_until_done(&ev, &ctx, TEST_TIMEOUT_MS), 0);
    ASSERT_TRUE(ctx.done);
    ASSERT_EQ(ctx.error, 0);
    ASSERT_EQ(ctx.status, 200);

    kl_http_redirect_free(rc);
    kl_event_ctx_free(&ev);
}

UTEST(redirect, async_max_exceeded) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };
    KlHttpRedirectConfig redir = { .max_redirects = 2 };

    AsyncRedirCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlHttpRedirectClient *rc = kl_http_redirect_start(&ev, &a, &cfg, &redir,
                                             "GET", test_url("/loop"),
                                             NULL, 0, NULL, 0,
                                             async_redir_done, &ctx);
    ASSERT_TRUE(rc != NULL);

    ASSERT_EQ(run_until_done(&ev, &ctx, TEST_TIMEOUT_MS), 0);
    ASSERT_TRUE(ctx.done);
    ASSERT_NE(ctx.error, 0);
    ASSERT_EQ(ctx.last_error, KL_ERR_REDIRECT_LOOP);

    kl_http_redirect_free(rc);
    kl_event_ctx_free(&ev);
}

UTEST(redirect, async_pooled) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };

    KlHttpClientPool pool;
    ASSERT_EQ(kl_http_client_pool_init(&pool, NULL, &a, &ev), 0);

    AsyncRedirCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    KlHttpRedirectClient *rc = kl_http_redirect_start_pooled(&pool, &ev, &a, &cfg, NULL,
                                                    "GET", test_url("/redir301"),
                                                    NULL, 0, NULL, 0,
                                                    async_redir_done, &ctx);
    ASSERT_TRUE(rc != NULL);

    int elapsed = 0;
    while (!ctx.done && elapsed < TEST_TIMEOUT_MS) {
        kl_event_ctx_run(&ev, 16, 50);
        kl_timer_fire(&ev);
        elapsed += 50;
    }

    ASSERT_TRUE(ctx.done);
    ASSERT_EQ(ctx.error, 0);
    ASSERT_EQ(ctx.status, 200);

    kl_http_redirect_free(rc);
    kl_http_client_pool_free(&pool);
    kl_event_ctx_free(&ev);
}

UTEST(redirect, async_cancel) {
    ensure_servers();

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlHttpClientConfig cfg = { .timeout_ms = TEST_TIMEOUT_MS };

    KlHttpRedirectClient *rc = kl_http_redirect_start(&ev, &a, &cfg, NULL,
                                             "GET", test_url("/loop"),
                                             NULL, 0, NULL, 0,
                                             async_redir_done, NULL);
    ASSERT_TRUE(rc != NULL);

    kl_http_redirect_cancel(rc);
    ASSERT_NE(kl_http_redirect_error(rc), 0);

    kl_http_redirect_free(rc);
    kl_event_ctx_free(&ev);
}

UTEST(redirect, async_null_args) {
    ASSERT_TRUE(kl_http_redirect_start(NULL, NULL, NULL, NULL,
                                   "GET", "http://x",
                                   NULL, 0, NULL, 0,
                                   NULL, NULL) == NULL);
}

UTEST(redirect, free_null) {
    kl_http_redirect_free(NULL);
    ASSERT_TRUE(1);
}

UTEST(redirect, cancel_null) {
    kl_http_redirect_cancel(NULL);
    ASSERT_TRUE(1);
}

/* ── Main with atexit cleanup ────────────────────────────────────── */

UTEST_STATE();

int main(int argc, const char *const argv[]) {
    atexit(cleanup_servers);
    return utest_main(argc, argv);
}
