/* Happy Eyeballs (RFC 8305) — async client connect racing over the resolved
 * address list, plus the overall request deadline timer.
 *
 * A mock resolver returns a configurable list of 127.0.0.1 addresses (live
 * server ports, closed ports, or a connect-stalling port). The client is driven
 * by a KlEventCtx pump. Timing-dependent tests (stalled first address, deadline)
 * probe the environment first and UTEST_SKIP where a reliable connect-stall is
 * unavailable (some macOS configs), so they still pass on CI (Linux) where the
 * stall is deterministic.
 */
#include "../vendor/utest.h"
#include <keel/client.h>
#include <keel/resolver.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/server.h>
#include <keel/allocator.h>

#include "net_compat.h"
#include <pthread.h>
#include <string.h>

/* A blackhole address (RFC 5737 TEST-NET-1): a connect there stays pending
 * (SYN dropped by the default gateway) until the deadline — used to exercise the
 * slow-first race and the overall deadline. Guarded by blackhole_stalls(). */
#define BLACKHOLE_IP "192.0.2.1"

/* ── Mock resolver: returns a configurable v4 list (per-entry IP + port) ─ */

static const char  *g_res_ip[KL_RESOLVE_MAX_ADDRS];
static int          g_res_ports[KL_RESOLVE_MAX_ADDRS];
static int          g_res_n;
static KlResolveReq g_mock_req;

static void fill_v4(KlSockAddr *a, const char *ip, int port) {
    uint8_t b[4];
    inet_pton(AF_INET, ip, b);
    kl_sockaddr_from_ipv4(a, b, (uint16_t)port);
}

static KlResolveReq *he_resolve(KlResolver *self, KlEventCtx *ctx,
                                 const char *host, int port,
                                 KlResolveDoneFn done_fn, void *ud) {
    (void)ctx; (void)host; (void)port;
    g_mock_req.resolver = self;
    KlResolveResult r;
    memset(&r, 0, sizeof(r));
    r.ai_socktype = SOCK_STREAM;
    r.ai_protocol = 0;
    r.naddrs = g_res_n;
    for (int i = 0; i < g_res_n; i++)
        fill_v4(&r.addrs[i],
                g_res_ip[i] ? g_res_ip[i] : "127.0.0.1", g_res_ports[i]);
    done_fn(&g_mock_req, &r, 0, ud);       /* synchronous completion */
    return &g_mock_req;
}
static void he_resolve_cancel(KlResolveReq *req) { (void)req; }
static void he_resolve_destroy(KlResolver *self) { (void)self; }

static KlResolver g_mock_resolver = {
    .resolve = he_resolve,
    .cancel  = he_resolve_cancel,
    .destroy = he_resolve_destroy,
};

/* ── Real one-route server on a background thread ────────────────────── */

static void handle_ok(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

static void *server_thread_fn(void *arg) {
    kl_server_run((KlServer *)arg);
    return NULL;
}

static int start_server(KlServer *srv, pthread_t *tid) {
    KlConfig cfg = { .port = 0, .max_connections = 8 };
    if (kl_server_init(srv, &cfg) != 0)
        return -1;
    kl_server_route(srv, "GET", "/ok", handle_ok, NULL, NULL);
    if (pthread_create(tid, NULL, server_thread_fn, srv) != 0)
        return -1;
    for (int i = 0; i < 200 && srv->bound_port == 0; i++)
        usleep(10000);
    return srv->bound_port > 0 ? 0 : -1;
}

static void stop_server(KlServer *srv, pthread_t tid) {
    kl_server_stop(srv);
    pthread_join(tid, NULL);
    kl_server_free(srv);
}

/* Reserve a loopback port then close it — connecting there yields a fast
 * ECONNREFUSED (nothing listening). */
static int reserve_closed_port(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &l);
    int port = ntohs(a.sin_port);
    kl_test_closesock(s);
    return port;
}

/* Confirm a connect to BLACKHOLE_IP actually stalls in this environment (needs
 * a default route; on a fully offline host it may error immediately). Timing
 * tests UTEST_SKIP when this returns 0, keeping them green everywhere. */
static int blackhole_stalls(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    kl_test_set_nonblock(fd);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(80);
    inet_pton(AF_INET, BLACKHOLE_IP, &a.sin_addr);
    connect(fd, (struct sockaddr *)&a, sizeof(a));
    int pr = kl_test_poll1(fd, 1, 250);
    int stalled = 1;
    if (pr != 0) {                       /* completed or errored → not a stall */
        stalled = 0;
    }
    kl_test_closesock(fd);
    return stalled;
}

/* ── Client driver ───────────────────────────────────────────────────── */

typedef struct { int done; int status; KlError err; } HeCtx;

static void he_done(KlClient *cl, void *ud) {
    HeCtx *x = ud;
    const KlClientResponse *r = kl_client_response(cl);
    x->status = r ? r->status : -1;
    x->err = kl_client_last_error(cl);
    x->done = 1;
}

static void pump(KlEventCtx *ev, int *done, int max_iters) {
    for (int i = 0; i < max_iters && !*done; i++) {
        kl_event_ctx_run(ev, 16, 10);
        kl_timer_fire(ev);
    }
}

static KlClientConfig he_cfg(int delay_ms, int timeout_ms) {
    KlClientConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &g_mock_resolver;
    cfg.connect_attempt_delay_ms = delay_ms;
    cfg.timeout_ms = timeout_ms;
    return cfg;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

UTEST(he, first_wins) {
    KlServer srv; pthread_t tid;
    ASSERT_EQ(0, start_server(&srv, &tid));

    memset(g_res_ip, 0, sizeof(g_res_ip));
    g_res_n = 2;
    g_res_ports[0] = srv.bound_port;     /* both live → first wins the race */
    g_res_ports[1] = srv.bound_port;

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    KlClientConfig cfg = he_cfg(30, 2000);

    HeCtx x = { 0, 0, 0 };
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://host.test/ok",
                                   NULL, 0, NULL, 0, he_done, &x);
    ASSERT_TRUE(c != NULL);
    pump(&ev, &x.done, 200);

    ASSERT_TRUE(x.done);
    ASSERT_EQ(200, x.status);
    ASSERT_EQ(KL_ERR_NONE, x.err);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_server(&srv, tid);
}

UTEST(he, fallback_on_refused) {
    KlServer srv; pthread_t tid;
    ASSERT_EQ(0, start_server(&srv, &tid));

    memset(g_res_ip, 0, sizeof(g_res_ip));
    g_res_n = 2;
    g_res_ports[0] = reserve_closed_port();  /* ECONNREFUSED → fast failover */
    g_res_ports[1] = srv.bound_port;         /* live: wins */

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    KlClientConfig cfg = he_cfg(30, 2000);

    HeCtx x = { 0, 0, 0 };
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://host.test/ok",
                                   NULL, 0, NULL, 0, he_done, &x);
    ASSERT_TRUE(c != NULL);
    pump(&ev, &x.done, 200);

    ASSERT_TRUE(x.done);
    ASSERT_EQ(200, x.status);      /* recovered on the second address */

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_server(&srv, tid);
}

/* WSAPoll does not report a failed non-blocking connect via a writable event
 * (design doc §B.2), so a refused-connect race resolves only via the deadline —
 * this all-fail expectation is POSIX connect-error semantics. */
#if !defined(_WIN32)
UTEST(he, all_fail) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));

    memset(g_res_ip, 0, sizeof(g_res_ip));
    g_res_n = 2;
    g_res_ports[0] = reserve_closed_port();
    g_res_ports[1] = reserve_closed_port();

    KlClientConfig cfg = he_cfg(30, 2000);
    HeCtx x = { 0, 0, 0 };
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://host.test/ok",
                                   NULL, 0, NULL, 0, he_done, &x);
    ASSERT_TRUE(c != NULL);
    pump(&ev, &x.done, 200);

    ASSERT_TRUE(x.done);
    ASSERT_EQ(KL_ERR_CONNECT, x.err);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
}
#endif

UTEST(he, single_address) {
    KlServer srv; pthread_t tid;
    ASSERT_EQ(0, start_server(&srv, &tid));

    memset(g_res_ip, 0, sizeof(g_res_ip));
    g_res_n = 1;
    g_res_ports[0] = srv.bound_port;

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    KlClientConfig cfg = he_cfg(30, 2000);

    HeCtx x = { 0, 0, 0 };
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://host.test/ok",
                                   NULL, 0, NULL, 0, he_done, &x);
    ASSERT_TRUE(c != NULL);
    pump(&ev, &x.done, 200);

    ASSERT_TRUE(x.done);
    ASSERT_EQ(200, x.status);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    stop_server(&srv, tid);
}

UTEST(he, second_wins_on_slow_first) {
    if (!blackhole_stalls())
        UTEST_SKIP("no default route: " BLACKHOLE_IP " does not stall here");

    KlServer srv; pthread_t tid;
    ASSERT_EQ(0, start_server(&srv, &tid));

    memset(g_res_ip, 0, sizeof(g_res_ip));
    g_res_n = 2;
    g_res_ip[0] = BLACKHOLE_IP; g_res_ports[0] = 80;   /* stalls */
    g_res_ports[1] = srv.bound_port;                   /* live: wins after delay */

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    KlClientConfig cfg = he_cfg(30, 3000);   /* short delay so addr[1] fires */

    HeCtx x = { 0, 0, 0 };
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://host.test/ok",
                                   NULL, 0, NULL, 0, he_done, &x);
    ASSERT_TRUE(c != NULL);
    pump(&ev, &x.done, 400);

    ASSERT_TRUE(x.done);
    ASSERT_EQ(200, x.status);      /* second address wins while first stalls */

    kl_client_free(c);             /* cancels the still-pending first attempt */
    kl_event_ctx_free(&ev);
    stop_server(&srv, tid);
}

UTEST(he, deadline_fires_on_blackhole) {
    if (!blackhole_stalls())
        UTEST_SKIP("no default route: " BLACKHOLE_IP " does not stall here");

    memset(g_res_ip, 0, sizeof(g_res_ip));
    g_res_n = 1;
    g_res_ip[0] = BLACKHOLE_IP; g_res_ports[0] = 80;   /* single stalling address */

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    KlClientConfig cfg = he_cfg(30, 120);    /* short overall deadline */

    HeCtx x = { 0, 0, 0 };
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://host.test/ok",
                                   NULL, 0, NULL, 0, he_done, &x);
    ASSERT_TRUE(c != NULL);
    pump(&ev, &x.done, 400);         /* deadline should fire at ~120ms */

    ASSERT_TRUE(x.done);
    ASSERT_EQ(KL_ERR_TIMEOUT, x.err);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
}

UTEST_MAIN();
