/*
 * test_http_client_hostname_fail.c — Finding 1 regression (HTTP client slice; T-split of
 * the former tests/test_tls_set_hostname_fail.c). Clients MUST fail closed when
 * KlTls::set_hostname() returns -1.
 *
 * The adapter's set_hostname sets up SNI *and* hostname verification; if it fails,
 * proceeding would complete the CA-chain check but SKIP hostname verification, so a cert
 * for the wrong host would be accepted. Every client path must destroy the TLS session and
 * abort the connection instead.
 *
 * Strategy: a plain loopback TCP listener (loopback_listener.h) accepts and holds
 * connections. The client is pointed at it over "https" with the identity mock TLS
 * (mock_tls.h) whose set_hostname is forced to return -1. With the fix the client surfaces
 * an error and NEVER reaches a successful handshake/response. A control run with
 * set_hostname succeeding proves the harness would otherwise progress past TLS setup.
 *
 * Covers: sync client, async client (+ the attributability control).
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/http_client.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"
#include "loopback_listener.h"
#include <string.h>

/* ── SYNC client ───────────────────────────────────────────────────── */

UTEST(tls_hostname_fail, sync_client_aborts)
{
    Listener l;
    ASSERT_EQ(listener_start(&l), 0);
    l.reply_http = 1;   /* would yield a 200 if the client got past the handshake */

    KlAllocator a = kl_allocator_default();
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpClientConfig cfg = { .timeout_ms = 1000, .tls = &tls_cfg };

    char url[128];
    make_url(url, sizeof(url), "https", l.port, "/");

    mock_tls_set_hostname_fail = 1;   /* set_hostname() returns -1 */
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    int rc = kl_http_client_request(&a, &cfg, "GET", url, NULL, 0, NULL, 0, &resp);
    mock_tls_set_hostname_fail = 0;

    /* Must fail — never a successful handshake / 200 (which the listener would
     * otherwise deliver over the passthrough TLS). */
    ASSERT_EQ(rc, -1);
    ASSERT_NE(resp.status, 200);
    kl_http_client_response_free(&resp);

    listener_stop(&l);
}

/* ── ASYNC client ──────────────────────────────────────────────────── */

typedef struct { int done; int error; int status; } AsyncCtx;

static void async_on_done(KlHttpClient *client, void *ud)
{
    AsyncCtx *c = ud;
    c->error = kl_http_client_error(client);
    const KlHttpClientResponse *r = kl_http_client_response(client);
    c->status = (c->error == 0 && r) ? r->status : -1;
    c->done = 1;
}

UTEST(tls_hostname_fail, async_client_aborts)
{
    Listener l;
    ASSERT_EQ(listener_start(&l), 0);
    l.reply_http = 1;   /* would yield a 200 if the client got past the handshake */

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpClientConfig cfg = { .timeout_ms = 1000, .tls = &tls_cfg };

    char url[128];
    make_url(url, sizeof(url), "https", l.port, "/");

    AsyncCtx ctx = {0};
    mock_tls_set_hostname_fail = 1;
    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", url, NULL, 0, NULL, 0,
                                  async_on_done, &ctx);
    ASSERT_TRUE(c != NULL);

    for (int i = 0; i < 500 && !ctx.done; i++)
        kl_event_ctx_run(&ev, 16, 10);
    mock_tls_set_hostname_fail = 0;

    ASSERT_TRUE(ctx.done);
    ASSERT_NE(ctx.error, 0);     /* connection aborted */
    ASSERT_NE(ctx.status, 200);  /* never the 200 the listener would deliver */

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    listener_stop(&l);
}

/* Control (attributability): with set_hostname SUCCEEDING, the very same harness
 * + passthrough TLS delivers a 200. So the abort in the fail case above is caused
 * by the set_hostname failure, not by a broken transport. */
UTEST(tls_hostname_fail, async_control_succeeds_when_hostname_ok)
{
    Listener l;
    ASSERT_EQ(listener_start(&l), 0);
    l.reply_http = 1;

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpClientConfig cfg = { .timeout_ms = 2000, .tls = &tls_cfg };

    char url[128];
    make_url(url, sizeof(url), "https", l.port, "/");

    AsyncCtx ctx = {0};
    mock_tls_set_hostname_fail = 0;   /* succeeds */
    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", url, NULL, 0, NULL, 0,
                                  async_on_done, &ctx);
    ASSERT_TRUE(c != NULL);

    for (int i = 0; i < 500 && !ctx.done; i++)
        kl_event_ctx_run(&ev, 16, 10);

    ASSERT_TRUE(ctx.done);
    ASSERT_EQ(ctx.error, 0);      /* got past TLS + a full response */
    ASSERT_EQ(ctx.status, 200);

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    listener_stop(&l);
}

UTEST_MAIN();
