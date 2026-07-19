#include "utest.h"
#include <keel/client.h>
#include <keel/resolver.h>
#include <keel/allocator.h>
#include <keel/server.h>
#include <keel/event_ctx.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

/* ── kl_client_response_free tests ───────────────────────────────── */

UTEST(client, response_free_null) {
    kl_client_response_free(NULL);
    ASSERT_TRUE(1);  /* should not crash */
}

UTEST(client, response_free_zeroed) {
    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    /* alloc is zeroed — should be a no-op */
    kl_client_response_free(&resp);
    ASSERT_EQ(resp.status, 0);
}

UTEST(client, response_free_with_data) {
    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));

    /* Simulate what the response parser produces */
    resp.alloc = a;
    resp.status = 200;

    /* Allocate body */
    resp.body = kl_malloc(&a, 6);
    ASSERT_TRUE(resp.body != NULL);
    memcpy(resp.body, "hello", 6);
    resp.body_len = 5;

    /* Allocate headers */
    resp.headers = kl_malloc(&a, sizeof(KlClientHeader));
    ASSERT_TRUE(resp.headers != NULL);
    resp.num_headers = 1;

    char *name = kl_malloc(&a, 5);
    memcpy(name, "Host", 5);
    char *value = kl_malloc(&a, 12);
    memcpy(value, "example.com", 12);
    resp.headers[0].name = name;
    resp.headers[0].value = value;

    kl_client_response_free(&resp);
    ASSERT_EQ(resp.status, 0);
    ASSERT_TRUE(resp.body == NULL);
    ASSERT_TRUE(resp.headers == NULL);
    ASSERT_EQ(resp.num_headers, 0);
}

/* ── Sync client input validation ────────────────────────────────── */

UTEST(client, request_null_args) {
    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;

    ASSERT_EQ(kl_client_request(NULL, NULL, "GET", "http://x", NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_client_request(&a, NULL, NULL, "http://x", NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_client_request(&a, NULL, "GET", NULL, NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_client_request(&a, NULL, "GET", "http://x", NULL, 0, NULL, 0, NULL), -1);
}

UTEST(client, request_bad_url) {
    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;

    ASSERT_EQ(kl_client_request(&a, NULL, "GET", "ftp://example.com", NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_client_request(&a, NULL, "GET", "garbage", NULL, 0, NULL, 0, &resp), -1);
}

UTEST(client, request_too_many_headers) {
    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;

    ASSERT_EQ(kl_client_request(&a, NULL, "GET", "http://example.com",
                                 NULL, KL_CLIENT_MAX_REQ_HEADERS + 1,
                                 NULL, 0, &resp), -1);
}

UTEST(client, request_negative_headers) {
    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;

    ASSERT_EQ(kl_client_request(&a, NULL, "GET", "http://example.com",
                                 NULL, -1, NULL, 0, &resp), -1);
}

UTEST(client, request_https_no_tls) {
    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;

    /* HTTPS without TLS config should fail */
    ASSERT_EQ(kl_client_request(&a, NULL, "GET", "https://example.com",
                                 NULL, 0, NULL, 0, &resp), -1);
}

/* ── Async client input validation ───────────────────────────────── */

UTEST(client, async_null_args) {
    ASSERT_TRUE(kl_client_start(NULL, NULL, NULL, "GET", "http://x",
                                 NULL, 0, NULL, 0, NULL, NULL) == NULL);
}

UTEST(client, async_bad_url) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    ASSERT_TRUE(kl_client_start(&ev, &a, NULL, "GET", "ftp://x",
                                 NULL, 0, NULL, 0, NULL, NULL) == NULL);

    kl_event_ctx_free(&ev);
}

UTEST(client, async_https_no_tls) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    ASSERT_TRUE(kl_client_start(&ev, &a, NULL, "GET", "https://example.com",
                                 NULL, 0, NULL, 0, NULL, NULL) == NULL);

    kl_event_ctx_free(&ev);
}

/* ── kl_client_error/response on NULL ────────────────────────────── */

UTEST(client, error_null) {
    ASSERT_EQ(kl_client_error(NULL), -1);
}

UTEST(client, response_null) {
    ASSERT_TRUE(kl_client_response(NULL) == NULL);
}

/* ── kl_client_cancel/free NULL safety ───────────────────────────── */

UTEST(client, cancel_null) {
    kl_client_cancel(NULL);
    ASSERT_TRUE(1);  /* should not crash */
}

UTEST(client, free_null) {
    kl_client_free(NULL);
    ASSERT_TRUE(1);  /* should not crash */
}

/* ── Fix 4: Resolver vtable tests ────────────────────────────────── */

/* Mock resolver that calls done_fn synchronously with an error */
static KlResolveReq mock_req;
static int mock_cancel_called;

static KlResolveReq *mock_resolve(KlResolver *self, KlEventCtx *ctx,
                                    const char *host, int port,
                                    KlResolveDoneFn done_fn, void *user_data) {
    (void)self; (void)ctx; (void)host; (void)port;
    mock_req.resolver = self;
    /* Call done_fn synchronously with error to test the callback path */
    done_fn(&mock_req, NULL, -1, user_data);
    return &mock_req;
}

static void mock_cancel(KlResolveReq *req) {
    (void)req;
    mock_cancel_called = 1;
}

static void mock_resolver_destroy(KlResolver *self) {
    (void)self;
}

static void mock_done(KlClient *client, void *user_data) {
    (void)user_data;
    /* Client should have error set */
    (void)client;
}

UTEST(client, async_dns_with_resolver_error) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlResolver resolver = {
        .resolve = mock_resolve,
        .cancel = mock_cancel,
        .destroy = mock_resolver_destroy,
    };

    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &resolver;

    /* The mock resolver calls done_fn with error synchronously.
     * kl_client_start should still return a valid handle. */
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://example.com",
                                    NULL, 0, NULL, 0, mock_done, NULL);
    /* Handle may be NULL if done_fn triggered cleanup, or non-NULL if kept */
    if (c) {
        ASSERT_EQ(kl_client_error(c), -1);
        kl_client_free(c);
    }

    kl_event_ctx_free(&ev);
}

/* Regression (audit H1): a resolver that completes synchronously AND returns
 * NULL (both contract-permitted). The client must NOT treat the NULL as a start
 * failure and free itself out from under on_done — it must return a live handle
 * that the caller frees. */
static KlResolveReq mock_req_syncnull;
static int          syncnull_done_fired;

static KlResolveReq *mock_resolve_sync_null(KlResolver *self, KlEventCtx *ctx,
                                            const char *host, int port,
                                            KlResolveDoneFn done_fn, void *ud) {
    (void)ctx; (void)host; (void)port;
    mock_req_syncnull.resolver = self;
    done_fn(&mock_req_syncnull, NULL, -1, ud);   /* synchronous completion */
    return NULL;                                  /* ...and NULL return */
}

static void syncnull_done(KlClient *client, void *user_data) {
    (void)client; (void)user_data;
    syncnull_done_fired = 1;
}

UTEST(client, async_resolver_sync_complete_null_return) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlResolver resolver = {
        .resolve = mock_resolve_sync_null,
        .cancel = mock_cancel,
        .destroy = mock_resolver_destroy,
    };
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &resolver;

    syncnull_done_fired = 0;
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://example.com",
                                    NULL, 0, NULL, 0, syncnull_done, NULL);

    ASSERT_TRUE(syncnull_done_fired);      /* on_done fired synchronously */
    ASSERT_TRUE(c != NULL);                /* handle kept alive (not freed under us) */
    ASSERT_EQ(kl_client_error(c), -1);     /* completed with the resolver error */
    kl_client_free(c);                     /* caller owns it — no double-free/UAF */

    kl_event_ctx_free(&ev);
}

UTEST(client, sync_dns_fallback) {
    /* NULL resolver should use sync getaddrinfo — just verify it doesn't crash */
    KlAllocator a = kl_allocator_default();
    KlClientResponse resp;

    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = NULL;

    /* This will fail to connect (no server), but exercises the code path */
    int rc = kl_client_request(&a, &cfg, "GET", "http://127.0.0.1:1",
                                NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(rc, -1);  /* connection refused */
}

/* Mock resolver that does NOT call done_fn (simulates pending async) */
static KlResolveReq pending_req;

static KlResolveReq *pending_resolve(KlResolver *self, KlEventCtx *ctx,
                                      const char *host, int port,
                                      KlResolveDoneFn done_fn, void *user_data) {
    (void)self; (void)ctx; (void)host; (void)port;
    (void)done_fn; (void)user_data;
    pending_req.resolver = self;
    return &pending_req;
}

UTEST(client, resolver_cancel_on_cleanup) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlResolver pending_resolver = {
        .resolve = pending_resolve,
        .cancel = mock_cancel,
        .destroy = mock_resolver_destroy,
    };

    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &pending_resolver;

    mock_cancel_called = 0;

    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://example.com",
                                    NULL, 0, NULL, 0, NULL, NULL);
    ASSERT_TRUE(c != NULL);

    /* Free should cancel the pending request */
    kl_client_free(c);
    ASSERT_TRUE(mock_cancel_called);

    kl_event_ctx_free(&ev);
}

/* ── #4: default resolver wiring (opt-out built-in async DNS) ─────── */

typedef struct { int done, error, status; } DnsWireCtx;

static void wire_done(KlClient *client, void *ud) {
    DnsWireCtx *c = ud;
    c->error = kl_client_error(client);
    if (c->error == 0) {
        const KlClientResponse *resp = kl_client_response(client);
        if (resp) c->status = resp->status;
    }
    c->done = 1;
}

static void wire_hello(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void *wire_server_thread(void *arg) { kl_server_run((KlServer *)arg); return NULL; }

static int wire_run(KlEventCtx *ev, DnsWireCtx *c, int timeout_ms) {
    int elapsed = 0;
    while (!c->done && elapsed < timeout_ms) {
        if (kl_event_ctx_run(ev, 16, 10) < 0) return -1;
        elapsed += 10;
    }
    return c->done ? 0 : -1;
}

/* Default config (no resolver, no system_dns): the async client auto-creates a
 * built-in resolver; "localhost" resolves via the shortcut → local server. */
UTEST(client, async_default_resolver_localhost) {
    KlServer srv;
    KlConfig scfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4 };
    ASSERT_EQ(0, kl_server_init(&srv, &scfg));
    kl_server_route(&srv, "GET", "/", wire_hello, NULL, NULL);
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, wire_server_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) usleep(10000);
    ASSERT_TRUE(srv.bound_port > 0);

    char url[64];
    snprintf(url, sizeof(url), "http://localhost:%d/", srv.bound_port);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    DnsWireCtx c = {0};
    KlClient *cl = kl_client_start(&ev, &a, NULL, "GET", url,
                                   NULL, 0, NULL, 0, wire_done, &c);
    ASSERT_TRUE(cl != NULL);
    ASSERT_EQ(0, wire_run(&ev, &c, 3000));
    ASSERT_EQ(0, c.error);
    ASSERT_EQ(200, c.status);
    kl_client_free(cl);            /* frees the auto-created resolver (ASan) */
    kl_event_ctx_free(&ev);

    kl_server_stop(&srv);
    pthread_join(t, NULL);
    kl_server_free(&srv);
}

/* system_dns=1 routes the async client through blocking getaddrinfo. */
UTEST(client, async_system_dns) {
    KlServer srv;
    KlConfig scfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4 };
    ASSERT_EQ(0, kl_server_init(&srv, &scfg));
    kl_server_route(&srv, "GET", "/", wire_hello, NULL, NULL);
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, wire_server_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) usleep(10000);
    ASSERT_TRUE(srv.bound_port > 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", srv.bound_port);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.system_dns = 1;
    DnsWireCtx c = {0};
    KlClient *cl = kl_client_start(&ev, &a, &cfg, "GET", url,
                                   NULL, 0, NULL, 0, wire_done, &c);
    ASSERT_TRUE(cl != NULL);
    ASSERT_EQ(0, wire_run(&ev, &c, 3000));
    ASSERT_EQ(0, c.error);
    ASSERT_EQ(200, c.status);
    kl_client_free(cl);
    kl_event_ctx_free(&ev);

    kl_server_stop(&srv);
    pthread_join(t, NULL);
    kl_server_free(&srv);
}

UTEST_MAIN();
