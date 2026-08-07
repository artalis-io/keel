/*
 * test_tls_set_hostname_fail.c — Finding 1 regression: clients MUST fail closed
 * when KlTls::set_hostname() returns -1.
 *
 * The adapter's set_hostname sets up SNI *and* hostname verification; if it
 * fails, proceeding would complete the CA-chain check but SKIP hostname
 * verification, so a cert for the wrong host would be accepted. Every client
 * path must destroy the TLS session and abort the connection instead.
 *
 * Strategy: a plain loopback TCP listener accepts (and holds) connections. Each
 * client is pointed at it over "https"/"wss" with the identity mock TLS
 * (mock_tls.h) whose set_hostname is forced to return -1. With the fix the client
 * surfaces an error and NEVER reaches a successful handshake/response. A control
 * run with set_hostname succeeding proves the harness would otherwise progress
 * past the TLS setup (so the abort is attributable to the set_hostname failure).
 *
 * Covers: sync client, async client, HTTP/2 client, WebSocket client.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/client.h>
#include <keel/h2_client.h>
#include <keel/websocket_client.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

/* ── Loopback accept-only listener ─────────────────────────────────── */

typedef struct {
    int      listen_fd;
    int      port;
    pthread_t tid;
    _Atomic int stop;      /* written by the test thread, read by the listener */
    int      reply_http;   /* 1 = write a canned HTTP/1.1 200 to each conn */
    /* accepted[]/n_accepted are written only by the listener and read only AFTER
     * pthread_join (a synchronization point), so they need no atomics. */
    int      accepted[16];
    int      n_accepted;
} Listener;

/* A complete HTTP/1.1 200 response. With the passthrough mock TLS, a client that
 * gets PAST the (identity) handshake would parse this as a successful 200 — so a
 * client that instead aborts at set_hostname will NOT see it. This is what makes
 * the sync/async assertions discriminate the fail-closed behavior. */
static const char kHttp200[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";

static void *listener_thread(void *arg)
{
    Listener *l = arg;
    for (;;) {
        int fd = accept(l->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (l->stop) break;
            continue;
        }
        if (l->n_accepted < (int)(sizeof(l->accepted) / sizeof(l->accepted[0])))
            l->accepted[l->n_accepted++] = fd;
        if (l->reply_http) {
            /* Drain a bit of the request (bounded — the client may have aborted
             * and sent nothing), then reply 200 (best-effort). */
            kl_test_set_rcvtimeo(fd, 300);
            char tmp[512];
            (void)kl_test_sockread(fd, tmp, sizeof(tmp));
            (void)kl_test_sockwrite(fd, kHttp200, sizeof(kHttp200) - 1);
        }
        /* Hold the connection open until teardown. */
        if (l->stop) { kl_test_closesock(fd); break; }
    }
    return NULL;
}

static int listener_start(Listener *l)
{
    memset(l, 0, sizeof(*l));
    l->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (l->listen_fd < 0) return -1;
    struct sockaddr_in addr = { .sin_family = AF_INET };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = 0;
    if (bind(l->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return -1;
    if (listen(l->listen_fd, 8) < 0) return -1;
    socklen_t sl = sizeof(addr);
    if (getsockname(l->listen_fd, (struct sockaddr *)&addr, &sl) < 0) return -1;
    l->port = ntohs(addr.sin_port);
    pthread_create(&l->tid, NULL, listener_thread, l);
    return 0;
}

static void listener_stop(Listener *l)
{
    l->stop = 1;
    /* Kick accept() by connecting once. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in addr = { .sin_family = AF_INET };
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        addr.sin_port = htons((uint16_t)l->port);
        connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        kl_test_closesock(fd);
    }
    pthread_join(l->tid, NULL);
    for (int i = 0; i < l->n_accepted; i++) kl_test_closesock(l->accepted[i]);
    kl_test_closesock(l->listen_fd);
}

static void make_url(char *buf, size_t n, const char *scheme, int port, const char *path)
{
    snprintf(buf, n, "%s://127.0.0.1:%d%s", scheme, port, path);
}

/* ── SYNC client ───────────────────────────────────────────────────── */

UTEST(tls_hostname_fail, sync_client_aborts)
{
    Listener l;
    ASSERT_EQ(listener_start(&l), 0);
    l.reply_http = 1;   /* would yield a 200 if the client got past the handshake */

    KlAllocator a = kl_allocator_default();
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlClientConfig cfg = { .timeout_ms = 1000, .tls = &tls_cfg };

    char url[128];
    make_url(url, sizeof(url), "https", l.port, "/");

    mock_tls_set_hostname_fail = 1;   /* set_hostname() returns -1 */
    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    int rc = kl_client_request(&a, &cfg, "GET", url, NULL, 0, NULL, 0, &resp);
    mock_tls_set_hostname_fail = 0;

    /* Must fail — never a successful handshake / 200 (which the listener would
     * otherwise deliver over the passthrough TLS). */
    ASSERT_EQ(rc, -1);
    ASSERT_NE(resp.status, 200);
    kl_client_response_free(&resp);

    listener_stop(&l);
}

/* ── ASYNC client ──────────────────────────────────────────────────── */

typedef struct { int done; int error; int status; } AsyncCtx;

static void async_on_done(KlClient *client, void *ud)
{
    AsyncCtx *c = ud;
    c->error = kl_client_error(client);
    const KlClientResponse *r = kl_client_response(client);
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
    KlClientConfig cfg = { .timeout_ms = 1000, .tls = &tls_cfg };

    char url[128];
    make_url(url, sizeof(url), "https", l.port, "/");

    AsyncCtx ctx = {0};
    mock_tls_set_hostname_fail = 1;
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", url, NULL, 0, NULL, 0,
                                  async_on_done, &ctx);
    ASSERT_TRUE(c != NULL);

    for (int i = 0; i < 500 && !ctx.done; i++)
        kl_event_ctx_run(&ev, 16, 10);
    mock_tls_set_hostname_fail = 0;

    ASSERT_TRUE(ctx.done);
    ASSERT_NE(ctx.error, 0);     /* connection aborted */
    ASSERT_NE(ctx.status, 200);  /* never the 200 the listener would deliver */

    kl_client_free(c);
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
    KlClientConfig cfg = { .timeout_ms = 2000, .tls = &tls_cfg };

    char url[128];
    make_url(url, sizeof(url), "https", l.port, "/");

    AsyncCtx ctx = {0};
    mock_tls_set_hostname_fail = 0;   /* succeeds */
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", url, NULL, 0, NULL, 0,
                                  async_on_done, &ctx);
    ASSERT_TRUE(c != NULL);

    for (int i = 0; i < 500 && !ctx.done; i++)
        kl_event_ctx_run(&ev, 16, 10);

    ASSERT_TRUE(ctx.done);
    ASSERT_EQ(ctx.error, 0);      /* got past TLS + a full response */
    ASSERT_EQ(ctx.status, 200);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    listener_stop(&l);
}

/* ── HTTP/2 client ─────────────────────────────────────────────────── */

/* Minimal H2 session factory (never reached when set_hostname fails). */
typedef struct { KlH2ClientSession b; KlAllocator *a; } H2Sess;

static int   h2s_recv(KlH2ClientSession *s, const char *d, size_t n) { (void)s; (void)d; (void)n; return 0; }
static int32_t h2s_submit(KlH2ClientSession *s, const char *m, const char *p, const char *au,
                          const KlH2ClientHeader *h, int n, const char *b, size_t bl)
{ (void)s; (void)m; (void)p; (void)au; (void)h; (void)n; (void)b; (void)bl; return 1; }
static int   h2s_flush(KlH2ClientSession *s) { (void)s; return 0; }
static void  h2s_destroy(KlH2ClientSession *s)
{
    H2Sess *hs = (H2Sess *)s;
    kl_free(hs->a, hs, sizeof(*hs));
}

static KlH2ClientSession *h2_factory(KlAllocator *alloc)
{
    H2Sess *s = kl_malloc(alloc, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->a = alloc;
    s->b.recv = h2s_recv;
    s->b.submit_request = h2s_submit;
    s->b.flush = h2s_flush;
    s->b.destroy = h2s_destroy;
    return &s->b;
}

typedef struct { int errored; } H2Ctx;
static void h2_on_error(KlH2ClientConn *c, const char *msg, void *ud)
{
    (void)c; (void)msg;
    ((H2Ctx *)ud)->errored = 1;
}

UTEST(tls_hostname_fail, h2_client_aborts)
{
    Listener l;
    ASSERT_EQ(listener_start(&l), 0);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);

    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlH2ClientConfig cfg = { .timeout_ms = 1000, .tls = &tls_cfg, .session = h2_factory };

    char url[128];
    make_url(url, sizeof(url), "https", l.port, "/");

    H2Ctx ctx = {0};
    mock_tls_set_hostname_fail = 1;
    KlH2ClientConn *c = kl_h2_client_connect(&ev, &a, &cfg, url, h2_on_error, &ctx);
    /* connect returns a handle; the abort surfaces via on_error during the loop. */
    for (int i = 0; i < 500 && !ctx.errored; i++)
        kl_event_ctx_run(&ev, 16, 10);
    mock_tls_set_hostname_fail = 0;

    ASSERT_TRUE(ctx.errored);   /* connection aborted (fail closed) */

    if (c) kl_h2_client_free(c);
    kl_event_ctx_free(&ev);
    listener_stop(&l);
}

/* ── WebSocket client ──────────────────────────────────────────────── */

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
