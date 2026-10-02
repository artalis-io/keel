#include "utest.h"
#include <keel/http_cors.h>
#include <string.h>

/* ── Config tests ───────────────────────────────────────────────────── */

UTEST(cors, init_defaults) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    ASSERT_EQ(c.origin_count, 0);
    ASSERT_STREQ(c.allowed_methods, "GET, POST, OPTIONS");
    ASSERT_STREQ(c.allowed_headers, "Content-Type, Authorization");
    ASSERT_EQ(c.allow_credentials, 0);
    ASSERT_EQ(c.max_age_seconds, 86400);
}

UTEST(cors, add_origin) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    ASSERT_EQ(kl_http_cors_add_origin(&c, "https://example.com"), 1);
    ASSERT_EQ(c.origin_count, 1);
    ASSERT_STREQ(c.allowed_origins[0], "https://example.com");
}

UTEST(cors, add_origin_rejects_empty) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    ASSERT_EQ(kl_http_cors_add_origin(&c, ""), 0);
    ASSERT_EQ(kl_http_cors_add_origin(&c, NULL), 0);
    ASSERT_EQ(c.origin_count, 0);
}

UTEST(cors, add_origin_full) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    for (int i = 0; i < KL_HTTP_CORS_MAX_ORIGINS; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "https://origin%d.com", i);
        ASSERT_EQ(kl_http_cors_add_origin(&c, buf), 1);
    }
    /* One more should fail */
    ASSERT_EQ(kl_http_cors_add_origin(&c, "https://overflow.com"), 0);
}

UTEST(cors, parse_origins) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    int n = kl_http_cors_parse_origins(&c, "https://a.com, https://b.com , https://c.com");
    ASSERT_EQ(n, 3);
    ASSERT_EQ(c.origin_count, 3);
    ASSERT_STREQ(c.allowed_origins[0], "https://a.com");
    ASSERT_STREQ(c.allowed_origins[1], "https://b.com");
    ASSERT_STREQ(c.allowed_origins[2], "https://c.com");
}

UTEST(cors, parse_origins_empty) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    ASSERT_EQ(kl_http_cors_parse_origins(&c, ""), 0);
    ASSERT_EQ(kl_http_cors_parse_origins(&c, NULL), 0);
    ASSERT_EQ(kl_http_cors_parse_origins(&c, "  , ,  "), 0);
}

/* ── Allowed check tests ────────────────────────────────────────────── */

UTEST(cors, is_allowed_wildcard) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    /* No origins = allow all */
    ASSERT_EQ(kl_http_cors_is_allowed(&c, "https://any.com", 15), 1);
}

UTEST(cors, is_allowed_whitelist) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    kl_http_cors_add_origin(&c, "https://good.com");

    ASSERT_EQ(kl_http_cors_is_allowed(&c, "https://good.com", 16), 1);
    ASSERT_EQ(kl_http_cors_is_allowed(&c, "https://evil.com", 16), 0);
}

UTEST(cors, is_allowed_no_origin) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    ASSERT_EQ(kl_http_cors_is_allowed(&c, NULL, 0), 0);
    ASSERT_EQ(kl_http_cors_is_allowed(&c, "", 0), 0);
}

/* ── Middleware tests ───────────────────────────────────────────────── */

UTEST(cors, middleware_no_origin) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);

    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/api"; req.path_len = 4;

    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    /* No Origin header: should pass through, no CORS headers added */
    int rc = kl_http_cors_middleware(&req, &res, &c);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(res.hdr_len, (size_t)0);

    kl_http_response_free(&res);
}

UTEST(cors, middleware_wildcard_origin) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);

    /* Simulate a request with Origin header */
    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/api"; req.path_len = 4;
    req.headers[0].name = "Origin"; req.headers[0].name_len = 6;
    req.headers[0].value = "https://app.com"; req.headers[0].value_len = 15;
    req.num_headers = 1;

    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int rc = kl_http_cors_middleware(&req, &res, &c);
    ASSERT_EQ(rc, 0);  /* continue */
    /* Should have Access-Control-Allow-Origin: * */
    ASSERT_TRUE(res.hdr_len > 0);

    kl_http_response_free(&res);
}

UTEST(cors, middleware_specific_origin) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    kl_http_cors_add_origin(&c, "https://allowed.com");

    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/api"; req.path_len = 4;
    req.headers[0].name = "Origin"; req.headers[0].name_len = 6;
    req.headers[0].value = "https://allowed.com"; req.headers[0].value_len = 19;
    req.num_headers = 1;

    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int rc = kl_http_cors_middleware(&req, &res, &c);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(res.hdr_len > 0);

    kl_http_response_free(&res);
}

/* Echoing the request's own Origin makes the response vary by Origin: without Vary, a shared cache
 * could serve one origin's Allow-Origin to another. A wildcard does not vary, and adds nothing. */
static int cors_has_vary_origin(int specific) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    if (specific) kl_http_cors_add_origin(&c, "https://allowed.com");
    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/api"; req.path_len = 4;
    req.headers[0].name = "Origin"; req.headers[0].name_len = 6;
    req.headers[0].value = "https://allowed.com"; req.headers[0].value_len = 19;
    req.num_headers = 1;
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    (void)kl_http_cors_middleware(&req, &res, &c);
    char hdrs[1024];
    size_t n = res.hdr_len < sizeof hdrs - 1 ? res.hdr_len : sizeof hdrs - 1;
    memcpy(hdrs, res.hdr_buf, n);
    hdrs[n] = '\0';
    kl_http_response_free(&res);
    return strstr(hdrs, "Vary: Origin") != NULL;
}

UTEST(cors, echoed_origin_varies_by_origin) {
    ASSERT_TRUE(cors_has_vary_origin(1));
    ASSERT_FALSE(cors_has_vary_origin(0));
}

/* With an origin allowlist every response depends on Origin, not only the ones that echo it: a
 * response to no Origin, or to a refused one, carries no Allow-Origin, and a shared cache that
 * stored it without Vary would serve that refusal to an allowed origin. */
static int cors_vary_for(const char *origin) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    kl_http_cors_add_origin(&c, "https://allowed.com");
    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/api"; req.path_len = 4;
    if (origin) {
        req.headers[0].name = "Origin"; req.headers[0].name_len = 6;
        req.headers[0].value = origin; req.headers[0].value_len = strlen(origin);
        req.num_headers = 1;
    }
    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    (void)kl_http_cors_middleware(&req, &res, &c);
    char hdrs[1024];
    size_t n = res.hdr_len < sizeof hdrs - 1 ? res.hdr_len : sizeof hdrs - 1;
    memcpy(hdrs, res.hdr_buf, n);
    hdrs[n] = '\0';
    kl_http_response_free(&res);
    return strstr(hdrs, "Vary: Origin") != NULL;
}

UTEST(cors, allowlist_varies_by_origin_on_every_response) {
    ASSERT_TRUE(cors_vary_for("https://allowed.com"));
    ASSERT_TRUE(cors_vary_for("https://evil.com"));     /* was: no Vary */
    ASSERT_TRUE(cors_vary_for(NULL));                   /* was: no Vary */
}

UTEST(cors, middleware_disallowed_origin) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    kl_http_cors_add_origin(&c, "https://allowed.com");

    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/api"; req.path_len = 4;
    req.headers[0].name = "Origin"; req.headers[0].name_len = 6;
    req.headers[0].value = "https://evil.com"; req.headers[0].value_len = 16;
    req.num_headers = 1;

    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int rc = kl_http_cors_middleware(&req, &res, &c);
    ASSERT_EQ(rc, 0);  /* continues, but grants nothing (only Vary: Origin, see below) */
    char hdrs[512];
    size_t hn = res.hdr_len < sizeof hdrs - 1 ? res.hdr_len : sizeof hdrs - 1;
    memcpy(hdrs, res.hdr_buf, hn);
    hdrs[hn] = '\0';
    ASSERT_TRUE(strstr(hdrs, "Access-Control-Allow-Origin") == NULL);

    kl_http_response_free(&res);
}

UTEST(cors, middleware_preflight) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);

    KlHttpRequest req = {0};
    req.method = "OPTIONS"; req.method_len = 7;
    req.path = "/api"; req.path_len = 4;
    req.headers[0].name = "Origin"; req.headers[0].name_len = 6;
    req.headers[0].value = "https://app.com"; req.headers[0].value_len = 15;
    req.num_headers = 1;

    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int rc = kl_http_cors_middleware(&req, &res, &c);
    ASSERT_EQ(rc, 1);  /* short-circuit */
    ASSERT_EQ(res.status, 204);
    ASSERT_TRUE(res.hdr_len > 0);

    kl_http_response_free(&res);
}

UTEST(cors, middleware_preflight_disallowed) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    kl_http_cors_add_origin(&c, "https://allowed.com");

    KlHttpRequest req = {0};
    req.method = "OPTIONS"; req.method_len = 7;
    req.path = "/api"; req.path_len = 4;
    req.headers[0].name = "Origin"; req.headers[0].name_len = 6;
    req.headers[0].value = "https://evil.com"; req.headers[0].value_len = 16;
    req.num_headers = 1;

    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    int rc = kl_http_cors_middleware(&req, &res, &c);
    ASSERT_EQ(rc, 0);  /* disallowed origin: no CORS grant, no preflight (only Vary: Origin) */
    char hdrs[512];
    size_t hn = res.hdr_len < sizeof hdrs - 1 ? res.hdr_len : sizeof hdrs - 1;
    memcpy(hdrs, res.hdr_buf, hn);
    hdrs[hn] = '\0';
    ASSERT_TRUE(strstr(hdrs, "Access-Control-") == NULL);

    kl_http_response_free(&res);
}

UTEST(cors, middleware_credentials) {
    KlHttpCorsConfig c;
    kl_http_cors_init(&c);
    kl_http_cors_add_origin(&c, "https://app.com");
    c.allow_credentials = 1;

    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/api"; req.path_len = 4;
    req.headers[0].name = "Origin"; req.headers[0].name_len = 6;
    req.headers[0].value = "https://app.com"; req.headers[0].value_len = 15;
    req.num_headers = 1;

    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    kl_http_cors_middleware(&req, &res, &c);
    /* Should contain both Allow-Origin and Allow-Credentials */
    ASSERT_TRUE(res.hdr_len > 0);

    kl_http_response_free(&res);
}

/* ── Audit coverage ────────────────────────────────────────────────── */

UTEST(cors, middleware_null_user_data) {
    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/api"; req.path_len = 4;

    KlAllocator a = kl_allocator_default();
    KlHttpResponse res;
    kl_http_response_init(&res, &a);

    /* NULL user_data should return 0 (pass-through), not crash */
    int rc = kl_http_cors_middleware(&req, &res, NULL);
    ASSERT_EQ(rc, 0);

    kl_http_response_free(&res);
}

UTEST_MAIN();
