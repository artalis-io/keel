#include "utest.h"
#include <keel/keel.h>
#include <keel/resolver_cache.h>

#include <string.h>
#include <time.h>
#include "net_compat.h"

/* ── Mock resolver ───────────────────────────────────────────────── */

static int mock_resolve_count;
static int mock_cancel_count;
static int mock_destroy_count;
static int mock_error_mode;       /* 1 = return error in done_fn */

/* Saved callback for deferred completion (pending mode) */
static KlResolveDoneFn  saved_done_fn;
static void            *saved_done_ud;
static KlResolveReq    *saved_req;

static KlResolveReq mock_req_storage;

static KlResolveResult make_result(void) {
    KlResolveResult r;
    memset(&r, 0, sizeof(r));
    struct sockaddr_in *sin = (struct sockaddr_in *)&r.addrs[0];
    sin->sin_family = AF_INET;
    sin->sin_port = htons(80);
    sin->sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */
    r.addrlens[0] = sizeof(struct sockaddr_in);
    r.naddrs = 1;
    r.ai_socktype = SOCK_STREAM;
    r.ai_protocol = 0;
    return r;
}

static KlResolveReq *mock_resolve(KlResolver *self, KlEventCtx *ctx,
                                    const char *host, int port,
                                    KlResolveDoneFn done_fn, void *user_data)
{
    (void)self; (void)ctx; (void)host; (void)port;
    mock_resolve_count++;
    mock_req_storage.resolver = self;

    if (mock_error_mode) {
        done_fn(&mock_req_storage, NULL, -1, user_data);
        return &mock_req_storage;
    }

    /* Sync completion with success */
    KlResolveResult r = make_result();
    done_fn(&mock_req_storage, &r, 0, user_data);
    return &mock_req_storage;
}

static void mock_cancel(KlResolveReq *req) {
    (void)req;
    mock_cancel_count++;
}

static void mock_destroy(KlResolver *self) {
    (void)self;
    mock_destroy_count++;
}

/* Pending mock: does NOT call done_fn (simulates async in-flight) */
static KlResolveReq *pending_resolve(KlResolver *self, KlEventCtx *ctx,
                                       const char *host, int port,
                                       KlResolveDoneFn done_fn, void *user_data)
{
    (void)self; (void)ctx; (void)host; (void)port;
    mock_resolve_count++;
    mock_req_storage.resolver = self;
    saved_done_fn = done_fn;
    saved_done_ud = user_data;
    saved_req = &mock_req_storage;
    return &mock_req_storage;
}

static void reset_mocks(void) {
    mock_resolve_count = 0;
    mock_cancel_count = 0;
    mock_destroy_count = 0;
    mock_error_mode = 0;
    saved_done_fn = NULL;
    saved_done_ud = NULL;
    saved_req = NULL;
}

/* ── Done callback for tests ─────────────────────────────────────── */

static int done_called;
static int done_error;
static KlResolveResult done_result;

static void test_done_fn(KlResolveReq *req, const KlResolveResult *result,
                           int error, void *user_data)
{
    (void)req; (void)user_data;
    done_called++;
    done_error = error;
    if (result)
        done_result = *result;
}

static void reset_done(void) {
    done_called = 0;
    done_error = 0;
    memset(&done_result, 0, sizeof(done_result));
}

/* Helper: resolve and immediately cancel (for tests that don't need the handle) */
static void resolve_fire(KlResolver *cache, const char *host, int port) {
    KlResolveReq *req = cache->resolve(cache, NULL, host, port,
                                         test_done_fn, NULL);
    if (req)
        cache->cancel(req);
}

/* ── Tests ───────────────────────────────────────────────────────── */

UTEST(rescache, cache_miss) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    KlResolveReq *req = cache->resolve(cache, NULL, "example.com", 80,
                                         test_done_fn, NULL);
    ASSERT_TRUE(req != NULL);
    ASSERT_EQ(mock_resolve_count, 1);
    ASSERT_EQ(done_called, 1);
    ASSERT_EQ(done_error, 0);

    cache->cancel(req);
    cache->destroy(cache);
}

UTEST(rescache, cache_hit) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    /* First resolve — cache miss, populates cache */
    resolve_fire(cache, "example.com", 80);
    ASSERT_EQ(mock_resolve_count, 1);

    /* Second resolve — cache hit, no inner call */
    reset_done();
    KlResolveReq *req2 = cache->resolve(cache, NULL, "example.com", 80,
                                          test_done_fn, NULL);
    ASSERT_TRUE(req2 != NULL);
    ASSERT_EQ(mock_resolve_count, 1);  /* still 1 — no new inner call */
    ASSERT_EQ(done_called, 1);
    ASSERT_EQ(done_error, 0);

    /* Cancel on cache hit should not call inner cancel */
    cache->cancel(req2);
    ASSERT_EQ(mock_cancel_count, 0);

    cache->destroy(cache);
}

UTEST(rescache, ttl_expiry) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    /* 1 ms TTL — will expire almost immediately */
    KlResolverCacheConfig cfg = { .ttl_ms = 1, .capacity = 4 };
    KlResolver *cache = kl_resolver_cache_create(&inner, &cfg, &a);
    ASSERT_TRUE(cache != NULL);

    /* Populate cache */
    resolve_fire(cache, "example.com", 80);
    ASSERT_EQ(mock_resolve_count, 1);

    /* Wait for TTL to expire */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 2000000 }; /* 2 ms */
    nanosleep(&ts, NULL);

    /* Should be a miss now */
    reset_done();
    resolve_fire(cache, "example.com", 80);
    ASSERT_EQ(mock_resolve_count, 2);  /* inner called again */

    cache->destroy(cache);
}

UTEST(rescache, capacity_eviction) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolverCacheConfig cfg = { .ttl_ms = 60000, .capacity = 2 };
    KlResolver *cache = kl_resolver_cache_create(&inner, &cfg, &a);
    ASSERT_TRUE(cache != NULL);

    /* Fill capacity */
    resolve_fire(cache, "a.com", 80);
    resolve_fire(cache, "b.com", 80);
    ASSERT_EQ(kl_resolver_cache_count(cache), 2);

    /* Third entry forces eviction */
    resolve_fire(cache, "c.com", 80);
    ASSERT_EQ(kl_resolver_cache_count(cache), 2);  /* still at capacity */
    ASSERT_EQ(mock_resolve_count, 3);

    cache->destroy(cache);
}

UTEST(rescache, different_hosts) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    resolve_fire(cache, "a.com", 80);
    resolve_fire(cache, "b.com", 80);
    ASSERT_EQ(mock_resolve_count, 2);  /* two separate lookups */
    ASSERT_EQ(kl_resolver_cache_count(cache), 2);

    /* Hit for each */
    reset_done();
    resolve_fire(cache, "a.com", 80);
    ASSERT_EQ(mock_resolve_count, 2);  /* no new calls */
    resolve_fire(cache, "b.com", 80);
    ASSERT_EQ(mock_resolve_count, 2);

    cache->destroy(cache);
}

UTEST(rescache, same_host_different_port) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    resolve_fire(cache, "example.com", 80);
    resolve_fire(cache, "example.com", 443);
    ASSERT_EQ(mock_resolve_count, 2);  /* different ports = separate entries */
    ASSERT_EQ(kl_resolver_cache_count(cache), 2);

    cache->destroy(cache);
}

UTEST(rescache, error_not_cached) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    /* Error mode — should not be cached */
    mock_error_mode = 1;
    resolve_fire(cache, "fail.com", 80);
    ASSERT_EQ(mock_resolve_count, 1);
    ASSERT_EQ(done_error, -1);
    ASSERT_EQ(kl_resolver_cache_count(cache), 0);

    /* Retry — should delegate again (not cached) */
    reset_done();
    resolve_fire(cache, "fail.com", 80);
    ASSERT_EQ(mock_resolve_count, 2);

    cache->destroy(cache);
}

UTEST(rescache, cancel_cache_hit) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    /* Populate */
    resolve_fire(cache, "example.com", 80);

    /* Hit — then cancel */
    KlResolveReq *req = cache->resolve(cache, NULL, "example.com", 80,
                                         test_done_fn, NULL);
    ASSERT_TRUE(req != NULL);
    cache->cancel(req);
    ASSERT_EQ(mock_cancel_count, 0);  /* no inner cancel for cache hit */

    cache->destroy(cache);
}

UTEST(rescache, cancel_cache_miss) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = pending_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    /* Miss — inner resolve is pending */
    KlResolveReq *req = cache->resolve(cache, NULL, "pending.com", 80,
                                         test_done_fn, NULL);
    ASSERT_TRUE(req != NULL);
    ASSERT_EQ(mock_resolve_count, 1);
    ASSERT_EQ(done_called, 0);  /* not completed yet */

    /* Cancel should delegate to inner */
    cache->cancel(req);
    ASSERT_EQ(mock_cancel_count, 1);

    cache->destroy(cache);
}

UTEST(rescache, destroy_preserves_inner) {
    reset_mocks();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    cache->destroy(cache);
    ASSERT_EQ(mock_destroy_count, 0);  /* inner NOT destroyed */
}

UTEST(rescache, null_config_defaults) {
    reset_mocks();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    /* Verify defaults by filling to default capacity */
    reset_done();
    for (int i = 0; i < 64; i++) {
        char host[32];
        snprintf(host, sizeof(host), "host%d.com", i);
        resolve_fire(cache, host, 80);
    }
    ASSERT_EQ(kl_resolver_cache_count(cache), 64);

    cache->destroy(cache);
}

UTEST(rescache, clear) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);

    resolve_fire(cache, "example.com", 80);
    ASSERT_EQ(kl_resolver_cache_count(cache), 1);

    kl_resolver_cache_clear(cache);
    ASSERT_EQ(kl_resolver_cache_count(cache), 0);

    /* Next resolve should be a miss */
    reset_done();
    resolve_fire(cache, "example.com", 80);
    ASSERT_EQ(mock_resolve_count, 2);  /* inner called again */

    cache->destroy(cache);
}

UTEST(rescache, count) {
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolverCacheConfig cfg = { .ttl_ms = 60000, .capacity = 8 };
    KlResolver *cache = kl_resolver_cache_create(&inner, &cfg, &a);
    ASSERT_TRUE(cache != NULL);

    ASSERT_EQ(kl_resolver_cache_count(cache), 0);

    resolve_fire(cache, "a.com", 80);
    ASSERT_EQ(kl_resolver_cache_count(cache), 1);

    resolve_fire(cache, "b.com", 80);
    ASSERT_EQ(kl_resolver_cache_count(cache), 2);

    /* Hit — count unchanged */
    resolve_fire(cache, "a.com", 80);
    ASSERT_EQ(kl_resolver_cache_count(cache), 2);

    cache->destroy(cache);
}

/* ── Re-entrant cancel from inside done_fn (audit H-1 regression test) ── */
/*
 * The user's done_fn calls cache->cancel(req) on the same request that
 * just completed.  Before the H-1 fix, this freed the request inside
 * cache_cancel, then inner_done_fn dereferenced cr->* on the freed
 * memory (UAF) — and on the sync path cache_resolve subsequently wrote
 * cr->inner_req on the freed slab.  The fix uses an in_user_done +
 * cancel_deferred flag pair so cancel() during user_done defers the
 * free to whichever caller (cache_resolve or inner_done_fn) is still
 * holding cr alive; the cancel is then honoured exactly once with no
 * UAF and no double-free.
 *
 * Verification: run under ASan (make debug); a regression would crash
 * with heap-use-after-free in the first variant and double-free in
 * the second.
 */
static KlResolver  *reentrant_cache_under_test;
static int          reentrant_cancel_inside_done;

static void reentrant_done_fn(KlResolveReq *req, const KlResolveResult *result,
                                int error, void *user_data)
{
    (void)result; (void)error; (void)user_data;
    if (reentrant_cancel_inside_done) {
        reentrant_cache_under_test->cancel(req);
    }
    done_called++;
}

UTEST(rescache, reentrant_cancel_in_sync_done_fn_cache_hit) {
    /* Sync completion via cache hit + user cancels inside done_fn. */
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);
    reentrant_cache_under_test = cache;

    /* Warm the cache with a no-cancel call. */
    reentrant_cancel_inside_done = 0;
    KlResolveReq *req = cache->resolve(cache, NULL, "example.com", 80,
                                         reentrant_done_fn, NULL);
    ASSERT_TRUE(req != NULL);
    cache->cancel(req);

    /* Now resolve the SAME host: cache hit → sync done_fn → user cancels. */
    reentrant_cancel_inside_done = 1;
    reset_done();
    req = cache->resolve(cache, NULL, "example.com", 80,
                          reentrant_done_fn, NULL);
    /* cache_resolve sees cancel_deferred and returns NULL (the user
     * already released the request); no leak, no UAF. */
    ASSERT_TRUE(req == NULL);
    ASSERT_EQ(done_called, 1);

    cache->destroy(cache);
}

UTEST(rescache, reentrant_cancel_in_sync_done_fn_cache_miss) {
    /* Sync completion via inner.resolve calling done_fn synchronously
     * + user cancels inside done_fn.  Distinct path from the cache-hit
     * variant: inner_done_fn rather than cache_resolve fires user_done. */
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = mock_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);
    reentrant_cache_under_test = cache;

    reentrant_cancel_inside_done = 1;
    KlResolveReq *req = cache->resolve(cache, NULL, "example.com", 80,
                                         reentrant_done_fn, NULL);
    /* Sync completion: inner_done_fn ran user_done which cancelled.
     * cache_resolve detects cancel_deferred and returns NULL. */
    ASSERT_TRUE(req == NULL);
    ASSERT_EQ(mock_resolve_count, 1);
    ASSERT_EQ(done_called, 1);

    cache->destroy(cache);
}

UTEST(rescache, reentrant_cancel_in_async_done_fn) {
    /* Async completion (pending_resolve) + user cancels inside done_fn. */
    reset_mocks();
    reset_done();
    KlAllocator a = kl_allocator_default();
    KlResolver inner = { .resolve = pending_resolve, .cancel = mock_cancel,
                          .destroy = mock_destroy };

    KlResolver *cache = kl_resolver_cache_create(&inner, NULL, &a);
    ASSERT_TRUE(cache != NULL);
    reentrant_cache_under_test = cache;

    reentrant_cancel_inside_done = 1;
    KlResolveReq *req = cache->resolve(cache, NULL, "example.com", 80,
                                         reentrant_done_fn, NULL);
    /* Async: cache_resolve returns the handle, inner hasn't fired yet. */
    ASSERT_TRUE(req != NULL);
    ASSERT_EQ(done_called, 0);

    /* Now fire the saved callback — user_done will cancel inside.
     * The fix defers, inner_done_fn frees on its async branch.
     * No double free, no UAF. */
    KlResolveResult r = make_result();
    saved_done_fn(saved_req, &r, 0, saved_done_ud);
    ASSERT_EQ(done_called, 1);

    cache->destroy(cache);
}

UTEST_MAIN();
