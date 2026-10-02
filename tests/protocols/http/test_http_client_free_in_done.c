/*
 * test_http_client_free_in_done.c: freeing the async client from inside its own on_done is safe on
 * every failure path of the connect phase.
 *
 * examples/async_client.c frees the client in on_done, the natural pattern. A DNS failure, a refused
 * connect, or a TLS setup failure used to call on_done from inside the connect op's terminal dispatch;
 * the client (and the KlConnectOp embedded in it) was freed while that frame was still running, which
 * then read and wrote freed memory. The completion is now deferred to the next loop tick.
 *
 * A use-after-free does not reliably crash, so the client's allocator here POISONS and QUARANTINES
 * every block it frees: the bytes are overwritten with 0xDD and the block is kept, not returned. A
 * later read then sees garbage (typically a poisoned function pointer, which crashes), and a later
 * write changes the poison, which the final check catches. Either way the defect fails the test on
 * every platform, with or without a sanitizer.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/http_client.h>
#include <keel/resolver.h>
#include <keel/tls.h>
#include "net_compat.h"
#include "mock_tls.h"
#include "loopback_listener.h"
#include <stdlib.h>
#include <string.h>

/* ── Poisoning, quarantining allocator ──────────────────────────────────────────────────────── */

#define QMAX 4096
static struct { unsigned char *p; size_t n; } g_q[QMAX];
static int g_nq;

static void *q_malloc(void *c, size_t n) { (void)c; return malloc(n ? n : 1); }
static void *q_realloc(void *c, void *p, size_t o, size_t n) { (void)c; (void)o; return realloc(p, n ? n : 1); }
static void q_free(void *c, void *p, size_t n) {
    (void)c;
    if (!p) return;
    memset(p, 0xDD, n);
    if (g_nq < QMAX) { g_q[g_nq].p = p; g_q[g_nq].n = n; g_nq++; }
    else free(p);
}
static KlAllocator g_qa = { q_malloc, q_realloc, q_free, NULL };

/* Every quarantined block must still hold its poison, then release them. Returns the number of
 * blocks written after they were freed. */
static int quarantine_check_and_release(void) {
    int written = 0;
    for (int i = 0; i < g_nq; i++) {
        for (size_t k = 0; k < g_q[i].n; k++)
            if (g_q[i].p[k] != 0xDD) { written++; break; }
        free(g_q[i].p);
    }
    g_nq = 0;
    return written;
}

/* ── The consumer: frees the client inside on_done, as examples/async_client.c does ─────────── */

typedef struct { int calls; int error; } Done;

static void free_in_done(KlHttpClient *client, void *ud) {
    Done *d = ud;
    d->calls++;
    d->error = kl_http_client_error(client);
    kl_http_client_free(client);
}

static void run_until(KlEventCtx *ev, const Done *d, int ms) {
    for (int i = 0; i < ms / 5 && d->calls == 0; i++) kl_event_ctx_run(ev, 16, 5);
    for (int i = 0; i < 20; i++) kl_event_ctx_run(ev, 16, 5);   /* let any stale frame or timer run */
}

/* ── A resolver that fails: inline (inside resolve), or from a timer (asynchronously) ───────── */

typedef struct {
    KlResolveReq    base;
    KlEventCtx     *ctx;
    KlResolveDoneFn done;
    void           *ud;
    int64_t         timer;
} FailReq;

typedef struct { KlResolver base; int inline_fail; FailReq req; } FailResolver;

static void fail_fire(void *arg) {
    FailReq *r = arg;
    r->timer = -1;
    r->done(&r->base, NULL, KL_ERR_DNS, r->ud);
}
static KlResolveReq *fail_resolve(KlResolver *self, KlEventCtx *ctx, const char *host, int port,
                                  KlResolveDoneFn done_fn, void *user_data) {
    (void)host; (void)port;
    FailResolver *fr = (FailResolver *)self;
    FailReq *r = &fr->req;
    memset(r, 0, sizeof *r);
    r->base.resolver = self;
    r->ctx = ctx; r->done = done_fn; r->ud = user_data; r->timer = -1;
    if (fr->inline_fail) {
        done_fn(&r->base, NULL, KL_ERR_DNS, user_data);
        return &r->base;
    }
    r->timer = kl_timer_add(ctx, 0, fail_fire, r);
    return r->timer >= 0 ? &r->base : NULL;
}
static void fail_cancel(KlResolveReq *req) {
    FailReq *r = (FailReq *)req;
    if (r->timer >= 0) { kl_timer_cancel(r->ctx, r->timer); r->timer = -1; }
}
static void fail_destroy(KlResolver *self) { (void)self; }

static void dns_case(int *utest_result, int inline_fail) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    FailResolver fr;
    memset(&fr, 0, sizeof fr);
    fr.base.resolve = fail_resolve; fr.base.cancel = fail_cancel; fr.base.destroy = fail_destroy;
    fr.inline_fail = inline_fail;
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 2000;
    cfg.resolver = &fr.base;

    Done d = { 0, 0 };
    KlHttpClient *c = kl_http_client_start(&ev, &g_qa, &cfg, "GET", "http://no-such-host.test/",
                                           NULL, 0, NULL, 0, free_in_done, &d);
    ASSERT_TRUE(c != NULL);                                   /* on_done never runs inside start */
    ASSERT_EQ(d.calls, 0);
    run_until(&ev, &d, 2000);
    ASSERT_EQ(d.calls, 1);                                    /* exactly once */
    ASSERT_NE(d.error, 0);
    kl_event_ctx_free(&ev);
    ASSERT_EQ(quarantine_check_and_release(), 0);             /* nothing written after free */
}

UTEST(http_client_free_in_done, dns_failure_reported_inline) { dns_case(utest_result, 1); }
UTEST(http_client_free_in_done, dns_failure_reported_async)  { dns_case(utest_result, 0); }

/* ── Connect refused ────────────────────────────────────────────────────────────────────────── */

/* A loopback port with nobody listening: bind, learn the port, close. */
static int closed_port(void) {
    KlSocketHandle s = (KlSocketHandle)socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(s)) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    socklen_t al = sizeof a;
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || getsockname(s, (struct sockaddr *)&a, &al) < 0) {
        kl_test_closesock(s);
        return -1;
    }
    int port = ntohs(a.sin_port);
    kl_test_closesock(s);
    return port;
}

UTEST(http_client_free_in_done, connect_refused) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    int port = closed_port();
    ASSERT_GT(port, 0);
    char url[128];
    make_url(url, sizeof url, "http", port, "/");
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 8000;                                    /* Windows retries a refused SYN ~2 s */

    Done d = { 0, 0 };
    KlHttpClient *c = kl_http_client_start(&ev, &g_qa, &cfg, "GET", url, NULL, 0, NULL, 0,
                                           free_in_done, &d);
    ASSERT_TRUE(c != NULL);
    run_until(&ev, &d, 8000);
    ASSERT_EQ(d.calls, 1);
    ASSERT_NE(d.error, 0);
    kl_event_ctx_free(&ev);
    ASSERT_EQ(quarantine_check_and_release(), 0);
}

/* ── TLS setup failure inside the connect winner (he_win) ───────────────────────────────────── */

UTEST(http_client_free_in_done, tls_setup_failure) {
    Listener l;
    ASSERT_EQ(listener_start(&l), 0);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 2000;
    cfg.tls = &tls_cfg;
    char url[128];
    make_url(url, sizeof url, "https", l.port, "/");

    Done d = { 0, 0 };
    mock_tls_set_hostname_fail = 1;                           /* fails inside he_win */
    KlHttpClient *c = kl_http_client_start(&ev, &g_qa, &cfg, "GET", url, NULL, 0, NULL, 0,
                                           free_in_done, &d);
    ASSERT_TRUE(c != NULL);
    run_until(&ev, &d, 3000);
    mock_tls_set_hostname_fail = 0;
    ASSERT_EQ(d.calls, 1);
    ASSERT_NE(d.error, 0);
    kl_event_ctx_free(&ev);
    listener_stop(&l);
    ASSERT_EQ(quarantine_check_and_release(), 0);
}

/* ── The deferred error and the request deadline due in the same tick ───────────────────────── */

/* The TLS setup failure above is deferred to a 0 ms timer. If the loop runs late, the request
 * deadline is overdue by then: one tick dispatches the connect (deferring the error), then fires both
 * timers, the older deadline first. Exactly one completion may reach on_done. */
static void count_done(KlHttpClient *client, void *ud) {
    Done *d = ud;
    d->calls++;
    d->error = kl_http_client_error(client);
}

/* Resolves to 127.0.0.1:<port> inside resolve(), so the deadline is armed and the connect started
 * inside kl_http_client_start, and a stall before the first loop tick leaves the deadline overdue. */
typedef struct { KlResolver base; KlResolveReq req; int port; } NowResolver;

static KlResolveReq *now_resolve(KlResolver *self, KlEventCtx *ctx, const char *host, int port,
                                 KlResolveDoneFn done_fn, void *user_data) {
    (void)ctx; (void)host; (void)port;
    NowResolver *nr = (NowResolver *)self;
    nr->req.resolver = self;
    KlResolveResult res;
    memset(&res, 0, sizeof res);
    const uint8_t lo[4] = { 127, 0, 0, 1 };
    kl_sockaddr_from_ipv4(&res.addrs[0], lo, (uint16_t)nr->port);
    res.naddrs = 1;
    res.ai_socktype = SOCK_STREAM;
    done_fn(&nr->req, &res, 0, user_data);
    return NULL;                                              /* completed inline */
}
static void now_cancel(KlResolveReq *req) { (void)req; }
static void now_destroy(KlResolver *self) { (void)self; }

static void deadline_case(int *utest_result, int free_in_cb) {
    static Listener l;                                        /* its thread outlives an early ASSERT return */
    ASSERT_EQ(listener_start(&l), 0);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlTlsConfig tls_cfg = { .ctx = NULL, .factory = mock_tls_create };
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 50;
    cfg.tls = &tls_cfg;
    NowResolver nr;
    memset(&nr, 0, sizeof nr);
    nr.base.resolve = now_resolve; nr.base.cancel = now_cancel; nr.base.destroy = now_destroy;
    nr.port = l.port;
    cfg.resolver = &nr.base;
    char url[128];
    make_url(url, sizeof url, "https", l.port, "/");

    Done d = { 0, 0 };
    mock_tls_set_hostname_fail = 1;
    KlHttpClient *c = kl_http_client_start(&ev, &g_qa, &cfg, "GET", url, NULL, 0, NULL, 0,
                                           free_in_cb ? free_in_done : count_done, &d);
    ASSERT_TRUE(c != NULL);
    kl_test_sleep_ms(200);                                    /* the deadline lapses before the loop runs */
    run_until(&ev, &d, 3000);
    mock_tls_set_hostname_fail = 0;
    int calls = d.calls;
    if (!free_in_cb) kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    listener_stop(&l);
    ASSERT_EQ(calls, 1);
    ASSERT_EQ(quarantine_check_and_release(), 0);
}

UTEST(http_client_free_in_done, overdue_deadline_completes_once) { deadline_case(utest_result, 0); }
UTEST(http_client_free_in_done, overdue_deadline_then_free_in_done) { deadline_case(utest_result, 1); }

UTEST_MAIN();
