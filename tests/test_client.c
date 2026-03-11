#include "utest.h"
#include <keel/client.h>
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

UTEST_MAIN();
