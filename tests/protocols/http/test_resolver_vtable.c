/*
 * test_resolver_vtable.c: a caller-supplied KlResolver (KlHttpClientConfig.resolver) is used by the
 * async client. Core calls resolve(), and cancel() whenever a pending resolution must stop (the
 * request timed out, or was cancelled or freed): without cancel, the resolver's later callback would
 * reach a freed client. So both are required (resolver.h requires all three). A BORROWED resolver's
 * destroy() is never called by Keel (the caller owns it). A malformed table is rejected at
 * acceptance as bad input, rather than being presented later as a DNS lookup failure.
 *
 * Public boundary: kl_http_client_start. A deferred mock resolve (returns a pending request without
 * completing) keeps the client alive so teardown exercises the borrowed-resolver contract.
 */
#include "utest.h"
#include <keel/keel.h>
#include <string.h>

static int g_destroy_calls, g_cancel_calls;
static KlResolveReq g_req;

static KlResolveReq *mock_resolve(KlResolver *self, KlEventCtx *ctx, const char *host, int port,
                                  KlResolveDoneFn done_fn, void *user_data) {
    (void)ctx; (void)host; (void)port; (void)done_fn; (void)user_data;
    g_req.resolver = self;
    return &g_req;   /* defer: do not call done_fn -> the client stays pending */
}
static void mock_cancel(KlResolveReq *req) { (void)req; g_cancel_calls++; }
static void mock_destroy(KlResolver *self) { (void)self; g_destroy_calls++; }

static void noop_done(KlHttpClient *c, void *ud) { (void)c; (void)ud; }

static KlHttpClient *start_with_resolver(KlEventCtx *ev, KlAllocator *alloc, KlResolver *r) {
    KlHttpClientConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = r;
    /* A non-numeric host so the async path goes through the resolver. */
    return kl_http_client_start(ev, alloc, &cfg, "GET", "http://example.test/",
                                NULL, 0, NULL, 0, noop_done, NULL);
}

/* A valid resolver (resolve present) is accepted; the borrowed resolver is NOT destroyed on free. */
UTEST(resolver_vtable, valid_resolver_accepted_and_not_destroyed) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    KlResolver r = { mock_resolve, mock_cancel, mock_destroy };
    g_destroy_calls = 0; g_cancel_calls = 0;
    KlHttpClient *c = start_with_resolver(&ev, &alloc, &r);
    ASSERT_TRUE(c != NULL);        /* resolution pending, not rejected */
    kl_http_client_free(c);
    ASSERT_EQ(g_destroy_calls, 0); /* borrowed: Keel never destroys it */
    ASSERT_GT(g_cancel_calls, 0);  /* the pending request was cancelled on free */
    kl_event_ctx_free(&ev);
}

/* A malformed resolver (no resolve op) is rejected up front (returns NULL), not a DNS failure. */
UTEST(resolver_vtable, missing_resolve_rejected) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    KlResolver r = { NULL, mock_cancel, mock_destroy };   /* resolve omitted */
    KlHttpClient *c = start_with_resolver(&ev, &alloc, &r);
    ASSERT_TRUE(c == NULL);
    kl_event_ctx_free(&ev);
}

/* cancel is required: without it a request freed (or timed out) while resolving cannot stop the
 * resolution, and the resolver's later callback reaches the freed client. Rejected up front. */
UTEST(resolver_vtable, missing_cancel_rejected) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    KlResolver r = { mock_resolve, NULL, mock_destroy };   /* cancel omitted */
    KlHttpClient *c = start_with_resolver(&ev, &alloc, &r);
    int rejected = (c == NULL);
    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    ASSERT_TRUE(rejected);         /* was: accepted */
}

/* The request deadline covers name resolution: a resolver that never answers must not leave the
 * request pending past timeout_ms (the deadline used to be armed only once resolution finished). */
typedef struct { int done; int err; } DeadlineDone;
static void deadline_done(KlHttpClient *c, void *ud) {
    DeadlineDone *d = ud;
    d->done++;
    d->err = (int)kl_http_client_last_error(c);
}
UTEST(resolver_vtable, deadline_covers_resolution) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    KlResolver r = { mock_resolve, mock_cancel, mock_destroy };   /* never calls done_fn */
    g_cancel_calls = 0;
    KlHttpClientConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &r;
    cfg.timeout_ms = 100;
    DeadlineDone d = { 0, 0 };
    KlHttpClient *c = kl_http_client_start(&ev, &alloc, &cfg, "GET", "http://example.test/",
                                           NULL, 0, NULL, 0, deadline_done, &d);
    for (int i = 0; c && i < 100 && !d.done; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(d.done, 1);                        /* was 0: the request never completed */
    ASSERT_EQ(d.err, (int)KL_ERR_TIMEOUT);
    ASSERT_GT(g_cancel_calls, 0);                /* the pending resolution was cancelled */
}

UTEST_MAIN();
