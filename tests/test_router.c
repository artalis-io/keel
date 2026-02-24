#include "utest.h"
#include <keel/router.h>

static void dummy_handler(KlRequest *req, KlResponse *res, void *ctx) {
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

UTEST_MAIN();
