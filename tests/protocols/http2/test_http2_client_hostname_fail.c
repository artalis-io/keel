/*
 * test_http2_client_hostname_fail.c — guards that the HTTP/2 client fails closed on a
 * set_hostname failure (HTTP/2 client slice of the TLS set-hostname-fail tests). The HTTP/2 client MUST fail closed when
 * KlTls::set_hostname() returns -1 (see the HTTP slice for the full rationale).
 *
 * Strategy: a loopback listener (loopback_listener.h) accepts + holds; the h2 client is
 * pointed at it over "https" with the identity mock TLS whose set_hostname returns -1; the
 * abort surfaces via on_error and the session factory is never reached.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/http2_client.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"
#include "loopback_listener.h"
#include <string.h>

/* Minimal H2 session factory (never reached when set_hostname fails). */
typedef struct { KlHttp2ClientSession b; KlAllocator *a; } H2Sess;

static int   h2s_recv(KlHttp2ClientSession *s, const char *d, size_t n) { (void)s; (void)d; (void)n; return 0; }
static int32_t h2s_submit(KlHttp2ClientSession *s, const char *m, const char *p, const char *au,
                          const KlHttp2ClientHeader *h, int n, const char *b, size_t bl)
{ (void)s; (void)m; (void)p; (void)au; (void)h; (void)n; (void)b; (void)bl; return 1; }
static int   h2s_flush(KlHttp2ClientSession *s) { (void)s; return 0; }
static void  h2s_destroy(KlHttp2ClientSession *s)
{
    H2Sess *hs = (H2Sess *)s;
    kl_free(hs->a, hs, sizeof(*hs));
}

static KlHttp2ClientSession *h2_factory(KlAllocator *alloc)
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
static void h2_on_error(KlHttp2ClientConn *c, const char *msg, void *ud)
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
    KlHttp2ClientConfig cfg = { .timeout_ms = 1000, .tls = &tls_cfg, .session = h2_factory };

    char url[128];
    make_url(url, sizeof(url), "https", l.port, "/");

    H2Ctx ctx = {0};
    mock_tls_set_hostname_fail = 1;
    KlHttp2ClientConn *c = kl_http2_client_connect(&ev, &a, &cfg, url, h2_on_error, &ctx);
    /* connect returns a handle; the abort surfaces via on_error during the loop. */
    for (int i = 0; i < 500 && !ctx.errored; i++)
        kl_event_ctx_run(&ev, 16, 10);
    mock_tls_set_hostname_fail = 0;

    ASSERT_TRUE(ctx.errored);   /* connection aborted (fail closed) */

    if (c) kl_http2_client_free(c);
    kl_event_ctx_free(&ev);
    listener_stop(&l);
}

UTEST_MAIN();
