/*
 * test_socket_provider_vtable.c: a caller-supplied socket provider becomes active on the server and
 * client transports and drives all I/O through the kl_sock_* dispatchers, which deref provider->ops.
 * A NULL provider means "built-in default" (native), but a NON-NULL provider with a NULL ops table is
 * malformed and would fault on the first I/O. It must be rejected at the boundary where it becomes
 * active, before any I/O, with KL_ERR_INVALID_ARG (KL_ERR_SOCKET stays reserved for an actual
 * socket-operation failure). Individual ops stay optional (native fallback), so a valid provider with
 * a sparse ops table is still accepted.
 *
 * Public boundaries: kl_http_server_init, kl_http_client_request (sync), kl_http_client_start (async).
 */
#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include <keel/keel.h>
#include <string.h>

/* A malformed provider: non-NULL struct, NULL ops table. */
static const KlSocketProvider g_null_ops = { NULL, NULL, KL_SOCK_CAP_NATIVE_FD, NULL };

/* A valid, sparse provider: a non-NULL ops table with only a couple of ops set (the rest fall back
 * to the native default). This must be ACCEPTED (individual ops are optional). */
static const KlSocketOps g_sparse_ops = { .name = "sparse" };
static const KlSocketProvider g_sparse = { &g_sparse_ops, NULL, KL_SOCK_CAP_NATIVE_FD, NULL };

/* ── server boundary (kl_http_server_init) ──────────────────────────── */

UTEST(socket_provider_vtable, server_rejects_null_ops) {
    KlHttpServer s;
    KlHttpServerConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0; cfg.max_connections = 2;
    cfg.sockets = &g_null_ops;
    ASSERT_EQ(kl_http_server_init(&s, &cfg), -1);
    ASSERT_EQ((int)s.last_error, (int)KL_ERR_INVALID_ARG);
}

UTEST(socket_provider_vtable, server_accepts_null_provider) {
    KlHttpServer s;
    KlHttpServerConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0; cfg.max_connections = 2;
    cfg.sockets = NULL;   /* built-in default */
    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);
    kl_http_server_free(&s);
}

UTEST(socket_provider_vtable, server_accepts_valid_provider) {
    KlHttpServer s;
    KlHttpServerConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0; cfg.max_connections = 2;
    cfg.sockets = kl_socket_provider_posix();   /* a real, complete provider */
    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);
    kl_http_server_free(&s);
}

UTEST(socket_provider_vtable, server_accepts_sparse_ops) {
    KlHttpServer s;
    KlHttpServerConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0; cfg.max_connections = 2;
    cfg.sockets = &g_sparse;   /* non-NULL ops table, individual ops optional */
    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);
    kl_http_server_free(&s);
}

/* ── sync client boundary (kl_http_client_request) ──────────────────── */

UTEST(socket_provider_vtable, sync_client_rejects_null_ops) {
    KlAllocator alloc = kl_allocator_default();
    KlHttpClientConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sockets = &g_null_ops;
    KlHttpClientResponse resp;
    /* Validation fires before any connect, so the unreachable port is irrelevant. */
    int rc = kl_http_client_request(&alloc, &cfg, "GET", "http://127.0.0.1:9/",
                                    NULL, 0, NULL, 0, &resp);
    ASSERT_EQ(rc, -1);
    ASSERT_EQ((int)resp.error, (int)KL_ERR_INVALID_ARG);
}

/* ── async client boundary (kl_http_client_start) ───────────────────── */

UTEST(socket_provider_vtable, async_client_rejects_null_ops) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    KlHttpClientConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sockets = &g_null_ops;
    KlHttpClient *c = kl_http_client_start(&ev, &alloc, &cfg, "GET", "http://127.0.0.1:9/",
                                           NULL, 0, NULL, 0, NULL, NULL);
    ASSERT_TRUE(c == NULL);   /* rejected before allocation; no crash */
    kl_event_ctx_free(&ev);
}

UTEST_MAIN();
