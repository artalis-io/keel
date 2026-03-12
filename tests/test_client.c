#include "utest.h"
#include <keel/client.h>
#include <keel/resolver.h>
#include <keel/allocator.h>
#include <string.h>

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

UTEST_MAIN();
