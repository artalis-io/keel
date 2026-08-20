#include "utest.h"
#include <keel/router.h>

static void dummy_handler(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)res; (void)ctx;
}

UTEST(router, init_and_free) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    ASSERT_EQ(kl_router_init(&r, &a), 0);
    kl_router_free(&r);
}

UTEST(router, exact_match) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    kl_router_add(&r, "GET", "/hello", dummy_handler, NULL, NULL);

    KlRoute *matched;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int result = kl_router_match(&r, "GET", 3, "/hello", 6,
                                  &matched, params, &num_params);
    ASSERT_EQ(result, 200);
    ASSERT_TRUE(matched != NULL);
    ASSERT_EQ(num_params, 0);

    kl_router_free(&r);
}

UTEST(router, param_match) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    kl_router_add(&r, "GET", "/users/:id", dummy_handler, NULL, NULL);

    KlRoute *matched;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int result = kl_router_match(&r, "GET", 3, "/users/42", 9,
                                  &matched, params, &num_params);
    ASSERT_EQ(result, 200);
    ASSERT_TRUE(matched != NULL);
    ASSERT_EQ(num_params, 1);
    ASSERT_EQ(params[0].name_len, (size_t)2);
    ASSERT_EQ(memcmp(params[0].name, "id", 2), 0);
    ASSERT_EQ(params[0].value_len, (size_t)2);
    ASSERT_EQ(memcmp(params[0].value, "42", 2), 0);

    kl_router_free(&r);
}

UTEST(router, multi_param) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    kl_router_add(&r, "GET", "/users/:uid/posts/:pid", dummy_handler, NULL, NULL);

    KlRoute *matched;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int result = kl_router_match(&r, "GET", 3, "/users/5/posts/99", 17,
                                  &matched, params, &num_params);
    ASSERT_EQ(result, 200);
    ASSERT_EQ(num_params, 2);
    ASSERT_EQ(memcmp(params[0].value, "5", 1), 0);
    ASSERT_EQ(memcmp(params[1].value, "99", 2), 0);

    kl_router_free(&r);
}

UTEST(router, not_found) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    kl_router_add(&r, "GET", "/hello", dummy_handler, NULL, NULL);

    KlRoute *matched;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int result = kl_router_match(&r, "GET", 3, "/world", 6,
                                  &matched, params, &num_params);
    ASSERT_EQ(result, 404);
    ASSERT_TRUE(matched == NULL);

    kl_router_free(&r);
}

UTEST(router, method_not_allowed) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    kl_router_add(&r, "GET", "/hello", dummy_handler, NULL, NULL);

    KlRoute *matched;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int result = kl_router_match(&r, "POST", 4, "/hello", 6,
                                  &matched, params, &num_params);
    ASSERT_EQ(result, 405);

    kl_router_free(&r);
}

UTEST(router, wildcard_method) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    kl_router_add(&r, "*", "/api", dummy_handler, NULL, NULL);

    KlRoute *matched;
    KlParam params[KL_MAX_PARAMS];
    int num_params;

    ASSERT_EQ(kl_router_match(&r, "GET", 3, "/api", 4,
                               &matched, params, &num_params), 200);
    ASSERT_EQ(kl_router_match(&r, "POST", 4, "/api", 4,
                               &matched, params, &num_params), 200);
    ASSERT_EQ(kl_router_match(&r, "DELETE", 6, "/api", 4,
                               &matched, params, &num_params), 200);

    kl_router_free(&r);
}

UTEST(router, head_matches_get) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    kl_router_add(&r, "GET", "/hello", dummy_handler, NULL, NULL);

    KlRoute *matched;
    KlParam params[KL_MAX_PARAMS];
    int num_params;

    /* HEAD should match GET route */
    int result = kl_router_match(&r, "HEAD", 4, "/hello", 6,
                                  &matched, params, &num_params);
    ASSERT_EQ(result, 200);
    ASSERT_TRUE(matched != NULL);

    /* HEAD should NOT match POST-only routes */
    kl_router_add(&r, "POST", "/submit", dummy_handler, NULL, NULL);
    result = kl_router_match(&r, "HEAD", 4, "/submit", 7,
                              &matched, params, &num_params);
    ASSERT_EQ(result, 405);

    kl_router_free(&r);
}

UTEST(router, user_data_passed) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    int my_data = 42;
    kl_router_add(&r, "GET", "/data", dummy_handler, &my_data, NULL);

    KlRoute *matched;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    kl_router_match(&r, "GET", 3, "/data", 5, &matched, params, &num_params);
    ASSERT_EQ(*(int *)matched->user_data, 42);

    kl_router_free(&r);
}

/* ── Middleware tests ───────────────────────────────────────────────── */

static int mw_noop(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)res; (void)ctx;
    return 0;
}

/* Tracking middleware: appends a letter to a string buffer */
typedef struct { char buf[64]; int pos; } MwTracker;

static int mw_track_a(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)res;
    MwTracker *t = ctx;
    t->buf[t->pos++] = 'A';
    return 0;
}

static int mw_track_b(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)res;
    MwTracker *t = ctx;
    t->buf[t->pos++] = 'B';
    return 0;
}

static int mw_track_c_stop(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)res;
    MwTracker *t = ctx;
    t->buf[t->pos++] = 'C';
    return 1;  /* short-circuit */
}

UTEST(router, middleware_use) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    ASSERT_EQ(kl_router_use(&r, "*", "/*", mw_noop, NULL), 0);
    ASSERT_EQ(kl_router_use(&r, "GET", "/api/*", mw_noop, NULL), 0);
    ASSERT_EQ(r.mw_count, 2);

    kl_router_free(&r);
}

UTEST(router, middleware_run_prefix) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "*", "/*", mw_track_a, &t);

    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/anything"; req.path_len = 9;

    KlHttpResponse res = {0};
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);
    ASSERT_EQ(t.buf[0], 'A');

    /* Also matches root */
    req.path = "/"; req.path_len = 1;
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 2);

    kl_router_free(&r);
}

UTEST(router, middleware_run_exact) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "*", "/health", mw_track_a, &t);

    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    KlHttpResponse res = {0};

    req.path = "/health"; req.path_len = 7;
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);

    /* Should not match a different path */
    req.path = "/healthz"; req.path_len = 8;
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);  /* unchanged */

    kl_router_free(&r);
}

UTEST(router, middleware_api_prefix) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "*", "/api/*", mw_track_a, &t);

    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    KlHttpResponse res = {0};

    req.path = "/api/users"; req.path_len = 10;
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);

    req.path = "/api/users/123"; req.path_len = 14;
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 2);

    /* /api itself matches (path_len == prefix_len) */
    req.path = "/api"; req.path_len = 4;
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 3);

    /* /other should NOT match */
    req.path = "/other"; req.path_len = 6;
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 3);  /* unchanged */

    kl_router_free(&r);
}

UTEST(router, middleware_method_filter) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "GET", "/*", mw_track_a, &t);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};

    req.method = "GET"; req.method_len = 3;
    req.path = "/test"; req.path_len = 5;
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);

    /* POST should be skipped */
    req.method = "POST"; req.method_len = 4;
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);  /* unchanged */

    kl_router_free(&r);
}

UTEST(router, middleware_wildcard_method) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "*", "/*", mw_track_a, &t);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    req.path = "/x"; req.path_len = 2;

    req.method = "GET"; req.method_len = 3;
    kl_router_run_middleware(&r, &req, &res);
    req.method = "POST"; req.method_len = 4;
    kl_router_run_middleware(&r, &req, &res);
    req.method = "DELETE"; req.method_len = 6;
    kl_router_run_middleware(&r, &req, &res);

    ASSERT_EQ(t.pos, 3);

    kl_router_free(&r);
}

UTEST(router, middleware_head_falls_back) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "GET", "/*", mw_track_a, &t);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    req.method = "HEAD"; req.method_len = 4;
    req.path = "/test"; req.path_len = 5;

    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);  /* HEAD matched GET middleware */

    kl_router_free(&r);
}

UTEST(router, middleware_order) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "*", "/*", mw_track_a, &t);
    kl_router_use(&r, "*", "/*", mw_track_b, &t);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/x"; req.path_len = 2;

    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 2);
    ASSERT_EQ(t.buf[0], 'A');
    ASSERT_EQ(t.buf[1], 'B');

    kl_router_free(&r);
}

UTEST(router, middleware_short_circuit) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "*", "/*", mw_track_a, &t);
    kl_router_use(&r, "*", "/*", mw_track_c_stop, &t);
    kl_router_use(&r, "*", "/*", mw_track_b, &t);  /* should NOT run */

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/x"; req.path_len = 2;

    int rc = kl_router_run_middleware(&r, &req, &res);
    ASSERT_NE(rc, 0);
    ASSERT_EQ(t.pos, 2);  /* A ran, C ran, B skipped */
    ASSERT_EQ(t.buf[0], 'A');
    ASSERT_EQ(t.buf[1], 'C');

    kl_router_free(&r);
}

UTEST(router, middleware_no_match) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "POST", "/admin/*", mw_track_a, &t);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/public"; req.path_len = 7;

    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 0);  /* nothing matched */

    kl_router_free(&r);
}

static int mw_set_header(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_header(res, "X-Middleware", "applied");
    return 0;
}

UTEST(router, middleware_sets_header) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    kl_router_use(&r, "*", "/*", mw_set_header, NULL);

    KlHttpRequest req = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/test"; req.path_len = 5;

    KlHttpResponse res;
    kl_http_response_init(&res, &a);
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);

    /* Verify the header was written to the response buffer */
    ASSERT_TRUE(res.hdr_len > 0);

    kl_http_response_free(&res);
    kl_router_free(&r);
}

/* ── Audit coverage: param overflow ─────────────────────────────────── */

UTEST(router, param_overflow_clamped) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    /* Build a pattern with KL_MAX_PARAMS + 1 params */
    /* /a/:p0/b/:p1/.../q/:p16 — 17 param segments */
    kl_router_add(&r, "GET",
                  "/:a/:b/:c/:d/:e/:f/:g/:h/:i/:j/:k/:l/:m/:n/:o/:p/:q",
                  dummy_handler, NULL, NULL);

    KlRoute *matched;
    KlParam params[KL_MAX_PARAMS];
    int num_params;
    int result = kl_router_match(&r, "GET", 3,
        "/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17", 43,
        &matched, params, &num_params);
    ASSERT_EQ(result, 200);
    ASSERT_TRUE(matched != NULL);
    /* Only KL_MAX_PARAMS (16) captured, 17th silently dropped */
    ASSERT_EQ(num_params, KL_MAX_PARAMS);

    kl_router_free(&r);
}

/* ── Post-body middleware tests ─────────────────────────────────────── */

UTEST(router, post_middleware_use) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    ASSERT_EQ(kl_router_use_post(&r, "*", "/*", mw_noop, NULL), 0);
    ASSERT_EQ(kl_router_use_post(&r, "POST", "/api/*", mw_noop, NULL), 0);
    ASSERT_EQ(r.post_mw_count, 2);

    kl_router_free(&r);
}

UTEST(router, post_middleware_run) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use_post(&r, "*", "/*", mw_track_a, &t);

    KlHttpRequest req = {0};
    req.method = "POST"; req.method_len = 4;
    req.path = "/submit"; req.path_len = 7;

    KlHttpResponse res = {0};
    ASSERT_EQ(kl_router_run_post_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);
    ASSERT_EQ(t.buf[0], 'A');

    kl_router_free(&r);
}

UTEST(router, post_middleware_order) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use_post(&r, "*", "/*", mw_track_a, &t);
    kl_router_use_post(&r, "*", "/*", mw_track_b, &t);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/x"; req.path_len = 2;

    ASSERT_EQ(kl_router_run_post_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 2);
    ASSERT_EQ(t.buf[0], 'A');
    ASSERT_EQ(t.buf[1], 'B');

    kl_router_free(&r);
}

UTEST(router, post_middleware_short_circuit) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use_post(&r, "*", "/*", mw_track_a, &t);
    kl_router_use_post(&r, "*", "/*", mw_track_c_stop, &t);
    kl_router_use_post(&r, "*", "/*", mw_track_b, &t);  /* should NOT run */

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/x"; req.path_len = 2;

    int rc = kl_router_run_post_middleware(&r, &req, &res);
    ASSERT_NE(rc, 0);
    ASSERT_EQ(t.pos, 2);  /* A ran, C ran, B skipped */
    ASSERT_EQ(t.buf[0], 'A');
    ASSERT_EQ(t.buf[1], 'C');

    kl_router_free(&r);
}

UTEST(router, post_middleware_independent_of_pre) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use(&r, "*", "/*", mw_track_a, &t);
    kl_router_use_post(&r, "*", "/*", mw_track_b, &t);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};
    req.method = "GET"; req.method_len = 3;
    req.path = "/x"; req.path_len = 2;

    /* Pre-body middleware only runs A */
    ASSERT_EQ(kl_router_run_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);
    ASSERT_EQ(t.buf[0], 'A');

    /* Post-body middleware only runs B */
    ASSERT_EQ(kl_router_run_post_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 2);
    ASSERT_EQ(t.buf[1], 'B');

    kl_router_free(&r);
}

UTEST(router, post_middleware_method_filter) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);

    MwTracker t = {.pos = 0};
    kl_router_use_post(&r, "POST", "/*", mw_track_a, &t);

    KlHttpRequest req = {0};
    KlHttpResponse res = {0};

    /* GET should be skipped */
    req.method = "GET"; req.method_len = 3;
    req.path = "/test"; req.path_len = 5;
    ASSERT_EQ(kl_router_run_post_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 0);

    /* POST should match */
    req.method = "POST"; req.method_len = 4;
    ASSERT_EQ(kl_router_run_post_middleware(&r, &req, &res), 0);
    ASSERT_EQ(t.pos, 1);

    kl_router_free(&r);
}

/* ── Streaming-handler registration ───────────────────────────────── */

static KlBodyReader *dummy_body_factory(KlAllocator *alloc, const KlHttpRequest *req,
                                         void *user_data) {
    (void)alloc; (void)req; (void)user_data;
    return NULL;
}

UTEST(router, regular_add_clears_streaming_flag) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    ASSERT_EQ(kl_router_add(&r, "GET", "/x", dummy_handler, NULL, NULL), 0);
    ASSERT_EQ(r.count, 1);
    ASSERT_EQ(r.routes[0].streaming_handler, 0);
    kl_router_free(&r);
}

UTEST(router, streaming_add_sets_flag) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    ASSERT_EQ(kl_router_add_streaming(&r, "POST", "/upload", dummy_handler,
                                       NULL, dummy_body_factory), 0);
    ASSERT_EQ(r.count, 1);
    ASSERT_EQ(r.routes[0].streaming_handler, 1);
    ASSERT_TRUE(r.routes[0].body_reader == dummy_body_factory);
    kl_router_free(&r);
}

UTEST(router, streaming_add_rejects_null_body_factory) {
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    /* Streaming handlers MUST have a body reader to pump on_data into. */
    ASSERT_EQ(kl_router_add_streaming(&r, "POST", "/upload", dummy_handler,
                                       NULL, NULL), -1);
    ASSERT_EQ(r.count, 0);
    kl_router_free(&r);
}

UTEST(router, streaming_and_regular_routes_independent) {
    /* Mixing streaming + regular routes in the same router: each
     * retains its own flag, lookups stay independent. */
    KlAllocator a = kl_allocator_default();
    KlRouter r;
    kl_router_init(&r, &a);
    ASSERT_EQ(kl_router_add(&r, "GET", "/page", dummy_handler, NULL, NULL), 0);
    ASSERT_EQ(kl_router_add_streaming(&r, "POST", "/upload", dummy_handler,
                                       NULL, dummy_body_factory), 0);
    ASSERT_EQ(kl_router_add(&r, "GET", "/other", dummy_handler, NULL, NULL), 0);
    ASSERT_EQ(r.count, 3);
    ASSERT_EQ(r.routes[0].streaming_handler, 0);
    ASSERT_EQ(r.routes[1].streaming_handler, 1);
    ASSERT_EQ(r.routes[2].streaming_handler, 0);
    kl_router_free(&r);
}

UTEST_MAIN();
