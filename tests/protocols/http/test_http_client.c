#include "utest.h"
#include "net_compat.h"
#include <keel/http_client.h>
#include <keel/resolver.h>
#include <keel/allocator.h>
#include <keel/http_server.h>
#include <keel/event_ctx.h>
#include <keel/socket.h>
#include "../../../src/socket.h"   /* kl_sockdef_*: the built-in ops a wrapper delegates to; KL_SOCK_CAP_OVERLAPPED */
#include <string.h>
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#if !defined(_MSC_VER)
#include <unistd.h>
#endif   /* MSVC has no <unistd.h>; the harness helpers cover it */

/* ── kl_http_client_response_free tests ───────────────────────────────── */

UTEST(client, response_free_null) {
    kl_http_client_response_free(NULL);
    ASSERT_TRUE(1);  /* should not crash */
}

UTEST(client, response_free_zeroed) {
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    /* alloc is zeroed: should be a no-op */
    kl_http_client_response_free(&resp);
    ASSERT_EQ(resp.status, 0);
}

UTEST(client, response_free_with_data) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientResponse resp;
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
    resp.headers = kl_malloc(&a, sizeof(KlHttpClientHeader));
    ASSERT_TRUE(resp.headers != NULL);
    resp.num_headers = 1;

    char *name = kl_malloc(&a, 5);
    memcpy(name, "Host", 5);
    char *value = kl_malloc(&a, 12);
    memcpy(value, "example.com", 12);
    resp.headers[0].name = name;
    resp.headers[0].value = value;

    kl_http_client_response_free(&resp);
    ASSERT_EQ(resp.status, 0);
    ASSERT_TRUE(resp.body == NULL);
    ASSERT_TRUE(resp.headers == NULL);
    ASSERT_EQ(resp.num_headers, 0);
}

/* ── Sync client input validation ────────────────────────────────── */

UTEST(client, request_null_args) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_client_request(NULL, NULL, "GET", "http://x", NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_http_client_request(&a, NULL, NULL, "http://x", NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_http_client_request(&a, NULL, "GET", NULL, NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_http_client_request(&a, NULL, "GET", "http://x", NULL, 0, NULL, 0, NULL), -1);
}

UTEST(client, request_bad_url) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_client_request(&a, NULL, "GET", "ftp://example.com", NULL, 0, NULL, 0, &resp), -1);
    ASSERT_EQ(kl_http_client_request(&a, NULL, "GET", "garbage", NULL, 0, NULL, 0, &resp), -1);
}

UTEST(client, request_too_many_headers) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_client_request(&a, NULL, "GET", "http://example.com",
                                 NULL, KL_HTTP_CLIENT_MAX_REQ_HEADERS + 1,
                                 NULL, 0, &resp), -1);
}

UTEST(client, request_negative_headers) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientResponse resp;

    ASSERT_EQ(kl_http_client_request(&a, NULL, "GET", "http://example.com",
                                 NULL, -1, NULL, 0, &resp), -1);
}

UTEST(client, request_https_no_tls) {
    KlAllocator a = kl_allocator_default();
    KlHttpClientResponse resp;

    /* HTTPS without TLS config should fail */
    ASSERT_EQ(kl_http_client_request(&a, NULL, "GET", "https://example.com",
                                 NULL, 0, NULL, 0, &resp), -1);
}

/* ── Async client input validation ───────────────────────────────── */

UTEST(client, async_null_args) {
    ASSERT_TRUE(kl_http_client_start(NULL, NULL, NULL, "GET", "http://x",
                                 NULL, 0, NULL, 0, NULL, NULL) == NULL);
}

UTEST(client, async_bad_url) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    ASSERT_TRUE(kl_http_client_start(&ev, &a, NULL, "GET", "ftp://x",
                                 NULL, 0, NULL, 0, NULL, NULL) == NULL);

    kl_event_ctx_free(&ev);
}

UTEST(client, async_https_no_tls) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    ASSERT_TRUE(kl_http_client_start(&ev, &a, NULL, "GET", "https://example.com",
                                 NULL, 0, NULL, 0, NULL, NULL) == NULL);

    kl_event_ctx_free(&ev);
}

/* A TLS config without a factory cannot secure the connection: refuse the https request rather
 * than send it in plaintext. */
UTEST(client, async_https_tls_without_factory) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlTlsConfig tls = { .ctx = NULL, .factory = NULL };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tls = &tls;

    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", "https://127.0.0.1:1/",
                                           NULL, 0, NULL, 0, NULL, NULL);
    int refused = (c == NULL);
    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    ASSERT_TRUE(refused);
}

/* A start whose configured provider the ctx's loop cannot drive is refused, and the refusal leaves
 * the caller's shared ctx as it was. The provider was written to ctx->sockets before the check, so
 * every later start on the ctx without a provider of its own was refused too (and every other ctx
 * user went through the wrong provider). No NATIVE_FD: a readiness loop cannot watch its handles. */
static const KlSocketOps g_unwatchable_ops = { .name = "unwatchable" };
static const KlSocketProvider g_unwatchable_provider = {
    &g_unwatchable_ops, NULL, KL_SOCK_CAP_WRITEV, NULL,
};

UTEST(client, refused_provider_leaves_ctx_sockets_unchanged) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    const KlSocketProvider *before = ev.sockets;
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sockets = &g_unwatchable_provider;

    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", "http://127.0.0.1:1/",
                                           NULL, 0, NULL, 0, NULL, NULL);
    int refused = (c == NULL);
    /* A completion loop adopts its native provider instead of refusing; only a refusal must leave
     * the ctx untouched. */
    const KlSocketProvider *after = ev.sockets;
    kl_http_client_free(c);

    /* A following start that brings no provider of its own runs on the ctx's provider. */
    memset(&cfg, 0, sizeof cfg);
    KlHttpClient *c2 = kl_http_client_start(&ev, &a, &cfg, "GET", "http://127.0.0.1:1/",
                                            NULL, 0, NULL, 0, NULL, NULL);
    int second_ok = (c2 != NULL);
    kl_http_client_free(c2);
    kl_event_ctx_free(&ev);
    if (refused) ASSERT_TRUE(after == before);   /* was: the unwatchable provider */
    ASSERT_TRUE(after != &g_unwatchable_provider);
    ASSERT_TRUE(second_ok);                      /* was: refused, the ctx kept the bad provider */
}

/* ── The configured provider belongs to the client, not to the shared ctx ──────────────────────────
 * A counting wrapper over the built-in socket ops: each instance counts the sockets made, closed and
 * written through it. It advertises both a native fd (readiness loops) and the overlapped capability
 * (completion loops), so every backend accepts it as is. */
typedef struct { int sockets, closes, sends; } CountProv;
static CountProv g_cnt[2];

static KlSocketHandle cnt_socket(void *ctx, int d, int t, int p) {
    ((CountProv *)ctx)->sockets++;
    return kl_sockdef_socket(d, t, p);
}
static int cnt_close(void *ctx, KlSocketHandle fd) {
    ((CountProv *)ctx)->closes++;
    return kl_sockdef_close(fd);
}
static kl_ssize_t cnt_send(void *ctx, KlSocketHandle fd, const void *buf, size_t len) {
    ((CountProv *)ctx)->sends++;
    return kl_sockdef_send(fd, buf, len);
}
static const KlSocketOps g_cnt_ops = {
    .socket = cnt_socket, .close = cnt_close, .send = cnt_send, .name = "counting",
};
static KlSocketProvider g_cnt_prov[2] = {
    { &g_cnt_ops, &g_cnt[0], KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_OVERLAPPED, NULL },
    { &g_cnt_ops, &g_cnt[1], KL_SOCK_CAP_NATIVE_FD | KL_SOCK_CAP_OVERLAPPED, NULL },
};

typedef struct { int done, status; } ProvDone;
static void prov_done(KlHttpClient *client, void *ud) {
    ProvDone *d = ud;
    const KlHttpClientResponse *r = kl_http_client_error(client) == 0 ? kl_http_client_response(client)
                                                                       : NULL;
    d->status = r ? r->status : -1;
    d->done = 1;
}
static void prov_hello(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}
static void prov_server_thread(void *arg) { kl_http_server_run((KlHttpServer *)arg); }
static KlHttpServer g_prov_srv;
static KlPlatThread g_prov_tid;

/* Two clients on one ctx, each with its own provider. Starting a client wrote its provider into the
 * shared ctx, so the second start rerouted the first client's later I/O (its close among it) through
 * the second client's provider, and every other user of the ctx (a server sharing it) with it. */
UTEST(client, configured_provider_is_per_client_and_leaves_ctx_unchanged) {
    KlHttpServerConfig scfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&g_prov_srv, &scfg));
    kl_http_server_route(&g_prov_srv, "GET", "/", prov_hello, NULL, NULL);
    ASSERT_EQ(0, kl_plat_thread_create(&g_prov_tid, prov_server_thread, &g_prov_srv));
    for (int i = 0; i < 200 && g_prov_srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    int port = g_prov_srv.bound_port;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);

    memset(g_cnt, 0, sizeof g_cnt);
    KlAllocator a = kl_allocator_default();
    static KlEventCtx ev;
    int ev_ok = kl_event_ctx_init(&ev, &a) == 0;
    const KlSocketProvider *before = ev.sockets;
    KlHttpClientConfig ca, cb;
    memset(&ca, 0, sizeof ca);
    memset(&cb, 0, sizeof cb);
    ca.sockets = &g_cnt_prov[0];
    ca.system_dns = 1;           /* resolve inline: the connect starts inside the start call */
    cb.sockets = &g_cnt_prov[1];
    cb.system_dns = 1;
    static ProvDone da, db;
    memset(&da, 0, sizeof da);
    memset(&db, 0, sizeof db);

    KlHttpClient *cla = NULL, *clb = NULL;
    const KlSocketProvider *after_a = NULL, *after_b = NULL;
    if (ev_ok && port > 0) {
        cla = kl_http_client_start(&ev, &a, &ca, "GET", url, NULL, 0, NULL, 0, prov_done, &da);
        after_a = ev.sockets;
        clb = kl_http_client_start(&ev, &a, &cb, "GET", url, NULL, 0, NULL, 0, prov_done, &db);
        after_b = ev.sockets;
        for (int i = 0; i < 300 && cla && clb && !(da.done && db.done); i++)
            kl_event_ctx_run(&ev, 16, 10);
    }
    kl_http_client_free(cla);
    kl_http_client_free(clb);
    if (ev_ok) kl_event_ctx_free(&ev);
    kl_http_server_stop(&g_prov_srv);
    kl_plat_thread_join(&g_prov_tid);
    kl_http_server_free(&g_prov_srv);

    ASSERT_TRUE(ev_ok);
    ASSERT_TRUE(port > 0);
    ASSERT_TRUE(cla != NULL);
    ASSERT_TRUE(clb != NULL);
    ASSERT_TRUE(after_a == before);   /* was: the first client's provider */
    ASSERT_TRUE(after_b == before);   /* was: the second client's provider */
    ASSERT_EQ(200, da.status);
    ASSERT_EQ(200, db.status);
    /* Each client made, wrote and closed its socket through its own provider. */
    ASSERT_EQ(1, g_cnt[0].sockets);
    ASSERT_EQ(1, g_cnt[0].closes);    /* was: 0, closed through the second client's provider */
    ASSERT_TRUE(g_cnt[0].sends >= 1);
    ASSERT_EQ(1, g_cnt[1].sockets);
    ASSERT_EQ(1, g_cnt[1].closes);
    ASSERT_TRUE(g_cnt[1].sends >= 1);
}

/* A start that fails after the provider is chosen (here the resolver cannot start) leaves the
 * shared ctx's provider as it was. */
static KlResolveReq *nostart_resolve(KlResolver *self, KlEventCtx *ctx, const char *host, int port,
                                     KlResolveDoneFn done_fn, void *ud) {
    (void)self; (void)ctx; (void)host; (void)port; (void)done_fn; (void)ud;
    return NULL;                       /* could not start, no callback */
}
static void nostart_cancel(KlResolveReq *req) { (void)req; }

UTEST(client, failed_start_leaves_ctx_sockets_unchanged) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    const KlSocketProvider *before = ev.sockets;
    KlResolver r = { .resolve = nostart_resolve, .cancel = nostart_cancel };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sockets = &g_cnt_prov[0];
    cfg.resolver = &r;

    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", "http://example.invalid/",
                                           NULL, 0, NULL, 0, NULL, NULL);
    int refused = (c == NULL);
    const KlSocketProvider *after = ev.sockets;
    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    ASSERT_TRUE(refused);
    ASSERT_TRUE(after == before);      /* was: the failed client's provider */
}

/* The built-in resolver a client creates for itself makes its sockets through the client's provider
 * too, not through the ctx's: otherwise, on a ctx whose own provider is another handle domain (a
 * bring-your-own stack with ctx.sockets left NULL), the resolver opened a host socket and adopted it
 * into a loop that cannot drive it. A numeric target still creates the resolver (and its UDP
 * socket), and makes exactly one connection: two sockets in all, both through the client's provider. */
UTEST(client, default_resolver_uses_the_client_provider) {
    KlHttpServerConfig scfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&g_prov_srv, &scfg));
    kl_http_server_route(&g_prov_srv, "GET", "/", prov_hello, NULL, NULL);
    ASSERT_EQ(0, kl_plat_thread_create(&g_prov_tid, prov_server_thread, &g_prov_srv));
    for (int i = 0; i < 200 && g_prov_srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    int port = g_prov_srv.bound_port;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);

    memset(g_cnt, 0, sizeof g_cnt);
    g_cnt_prov[0].dgram = kl_sockdef_dgram();   /* the resolver needs the datagram data plane */
    KlAllocator a = kl_allocator_default();
    static KlEventCtx ev;
    int ev_ok = kl_event_ctx_init(&ev, &a) == 0;
    const KlSocketProvider *before = ev.sockets;
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.sockets = &g_cnt_prov[0];   /* no resolver, no system_dns: the built-in resolver */
    static ProvDone d;
    memset(&d, 0, sizeof d);

    KlHttpClient *cl = NULL;
    if (ev_ok && port > 0) {
        cl = kl_http_client_start(&ev, &a, &cfg, "GET", url, NULL, 0, NULL, 0, prov_done, &d);
        for (int i = 0; i < 300 && cl && !d.done; i++)
            kl_event_ctx_run(&ev, 16, 10);
    }
    kl_http_client_free(cl);          /* also frees the client's own resolver */
    const KlSocketProvider *after = ev.sockets;
    if (ev_ok) kl_event_ctx_free(&ev);
    g_cnt_prov[0].dgram = NULL;
    kl_http_server_stop(&g_prov_srv);
    kl_plat_thread_join(&g_prov_tid);
    kl_http_server_free(&g_prov_srv);

    ASSERT_TRUE(ev_ok);
    ASSERT_TRUE(port > 0);
    ASSERT_TRUE(cl != NULL);
    ASSERT_EQ(200, d.status);
    ASSERT_TRUE(after == before);
    ASSERT_EQ(2, g_cnt[0].sockets);   /* was: 1, the resolver's UDP socket came from the ctx's provider */
    ASSERT_EQ(2, g_cnt[0].closes);
}

/* ── kl_http_client_error/response on NULL ────────────────────────────── */

UTEST(client, error_null) {
    ASSERT_EQ(kl_http_client_error(NULL), -1);
}

UTEST(client, response_null) {
    ASSERT_TRUE(kl_http_client_response(NULL) == NULL);
}

/* ── kl_http_client_cancel/free NULL safety ───────────────────────────── */

UTEST(client, cancel_null) {
    kl_http_client_cancel(NULL);
    ASSERT_TRUE(1);  /* should not crash */
}

UTEST(client, free_null) {
    kl_http_client_free(NULL);
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

static void mock_done(KlHttpClient *client, void *user_data) {
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

    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &resolver;

    /* The mock resolver calls done_fn with error synchronously.
     * kl_http_client_start should still return a valid handle. */
    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", "http://example.com",
                                    NULL, 0, NULL, 0, mock_done, NULL);
    /* Handle may be NULL if done_fn triggered cleanup, or non-NULL if kept */
    if (c) {
        ASSERT_EQ(kl_http_client_error(c), -1);
        kl_http_client_free(c);
    }

    kl_event_ctx_free(&ev);
}

/* Regression (audit H1): a resolver that completes synchronously AND returns
 * NULL (both contract-permitted). The client must NOT treat the NULL as a start
 * failure and free itself out from under on_done: it must return a live handle
 * that the caller frees. And on_done is never called from inside
 * kl_http_client_start: the failure is reported on the next loop tick, so a
 * consumer that frees the client in on_done cannot be handed a dangling pointer. */
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

static void syncnull_done(KlHttpClient *client, void *user_data) {
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
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &resolver;

    syncnull_done_fired = 0;
    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", "http://example.com",
                                    NULL, 0, NULL, 0, syncnull_done, NULL);

    ASSERT_TRUE(c != NULL);                /* handle kept alive (not freed under us) */
    ASSERT_FALSE(syncnull_done_fired);     /* never inside kl_http_client_start ... */
    for (int i = 0; i < 10 && !syncnull_done_fired; i++)
        kl_event_ctx_run(&ev, 16, 5);
    ASSERT_TRUE(syncnull_done_fired);      /* ... but on the next loop tick */
    ASSERT_EQ(kl_http_client_error(c), -1);     /* completed with the resolver error */
    kl_http_client_free(c);                     /* caller owns it: no double-free/UAF */

    kl_event_ctx_free(&ev);
}

UTEST(client, sync_dns_fallback) {
    /* NULL resolver should use sync getaddrinfo: just verify it doesn't crash */
    KlAllocator a = kl_allocator_default();
    KlHttpClientResponse resp;

    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = NULL;

    /* This will fail to connect (no server), but exercises the code path */
    int rc = kl_http_client_request(&a, &cfg, "GET", "http://127.0.0.1:1",
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

    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &pending_resolver;

    mock_cancel_called = 0;

    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", "http://example.com",
                                    NULL, 0, NULL, 0, NULL, NULL);
    ASSERT_TRUE(c != NULL);

    /* Free should cancel the pending request */
    kl_http_client_free(c);
    ASSERT_TRUE(mock_cancel_called);

    kl_event_ctx_free(&ev);
}

/* ── #4: default resolver wiring (opt-out built-in async DNS) ─────── */

typedef struct { int done, error, status; } DnsWireCtx;

static void wire_done(KlHttpClient *client, void *ud) {
    DnsWireCtx *c = ud;
    c->error = kl_http_client_error(client);
    if (c->error == 0) {
        const KlHttpClientResponse *resp = kl_http_client_response(client);
        if (resp) c->status = resp->status;
    }
    c->done = 1;
}

static void wire_hello(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

static void wire_server_thread(void *arg) { kl_http_server_run((KlHttpServer *)arg); return; }

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
    KlHttpServer srv;
    KlHttpServerConfig scfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &scfg));
    kl_http_server_route(&srv, "GET", "/", wire_hello, NULL, NULL);
    KlPlatThread t;
    ASSERT_EQ(0, kl_plat_thread_create(&t, wire_server_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    ASSERT_TRUE(srv.bound_port > 0);

    char url[64];
    snprintf(url, sizeof(url), "http://localhost:%d/", srv.bound_port);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    DnsWireCtx c = {0};
    KlHttpClient *cl = kl_http_client_start(&ev, &a, NULL, "GET", url,
                                   NULL, 0, NULL, 0, wire_done, &c);
    ASSERT_TRUE(cl != NULL);
    ASSERT_EQ(0, wire_run(&ev, &c, 3000));
    ASSERT_EQ(0, c.error);
    ASSERT_EQ(200, c.status);
    kl_http_client_free(cl);            /* frees the auto-created resolver (ASan) */
    kl_event_ctx_free(&ev);

    kl_http_server_stop(&srv);
    kl_plat_thread_join(&t);
    kl_http_server_free(&srv);
}

/* system_dns=1 routes the async client through blocking getaddrinfo. */
UTEST(client, async_system_dns) {
    KlHttpServer srv;
    KlHttpServerConfig scfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &scfg));
    kl_http_server_route(&srv, "GET", "/", wire_hello, NULL, NULL);
    KlPlatThread t;
    ASSERT_EQ(0, kl_plat_thread_create(&t, wire_server_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    ASSERT_TRUE(srv.bound_port > 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", srv.bound_port);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.system_dns = 1;
    DnsWireCtx c = {0};
    KlHttpClient *cl = kl_http_client_start(&ev, &a, &cfg, "GET", url,
                                   NULL, 0, NULL, 0, wire_done, &c);
    ASSERT_TRUE(cl != NULL);
    ASSERT_EQ(0, wire_run(&ev, &c, 3000));
    ASSERT_EQ(0, c.error);
    ASSERT_EQ(200, c.status);
    kl_http_client_free(cl);
    kl_event_ctx_free(&ev);

    kl_http_server_stop(&srv);
    kl_plat_thread_join(&t);
    kl_http_server_free(&srv);
}

UTEST_MAIN();
