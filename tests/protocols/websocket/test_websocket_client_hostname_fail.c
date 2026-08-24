/*
 * test_websocket_client_hostname_fail.c: guards that the WebSocket client fails closed on a
 * set_hostname failure (WebSocket client slice of the TLS set-hostname-fail tests). The WebSocket client MUST fail
 * closed when KlTls::set_hostname() returns -1 (see the HTTP slice for the full rationale).
 *
 * Strategy: a loopback listener (loopback_listener.h) accepts + holds; the ws client is
 * pointed at it over "wss" with the identity mock TLS whose set_hostname returns -1; the
 * abort surfaces via on_error and on_open is never reached.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/websocket_client.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"
#include "loopback_listener.h"
#include <string.h>

typedef struct { int opened; int errored; } WsCtx;
static void ws_on_open(KlWsClientConn *ws, void *ud) { (void)ws; ((WsCtx *)ud)->opened = 1; }
static void ws_on_msg(KlWsClientConn *ws, const char *d, size_t n, int b, void *ud)
{ (void)ws; (void)d; (void)n; (void)b; (void)ud; }
static void ws_on_close(KlWsClientConn *ws, uint16_t code, const char *r, size_t rn, void *ud)
{ (void)ws; (void)code; (void)r; (void)rn; (void)ud; }
static void ws_on_error(KlWsClientConn *ws, const char *msg, void *ud)
{ (void)ws; (void)msg; ((WsCtx *)ud)->errored = 1; }

UTEST(tls_hostname_fail, ws_client_aborts)
{
    Listener l;
    ASSERT_EQ(listener_start(&l), 0);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlWsClientConfig cfg = { .timeout_ms = 1000, .tls = &tls_cfg };
    KlWsClientCallbacks cbs = {
        .on_open = ws_on_open, .on_message = ws_on_msg,
        .on_close = ws_on_close, .on_error = ws_on_error,
    };

    char url[128];
    make_url(url, sizeof(url), "wss", l.port, "/");

    WsCtx ctx = {0};
    mock_tls_set_hostname_fail = 1;
    KlWsClientConn *c = kl_ws_client_connect(&ev, &a, &cfg, url, &cbs, &ctx);
    for (int i = 0; i < 500 && !ctx.errored; i++)
        kl_event_ctx_run(&ev, 16, 10);
    mock_tls_set_hostname_fail = 0;

    ASSERT_TRUE(ctx.errored);       /* connection aborted (fail closed) */
    ASSERT_FALSE(ctx.opened);       /* never reached WS open */

    if (c) kl_ws_client_free(c);
    kl_event_ctx_free(&ev);
    listener_stop(&l);
}

UTEST_MAIN();
