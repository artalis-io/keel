/*
 * test_http_client_deadline.c: the HTTP client's request deadline and its resolved address list.
 *
 * Sync client: cfg.timeout_ms bounds the whole exchange (connect, TLS handshake, send, receive),
 * whatever the server does. A server that accepts and then says nothing, never reads, sends part of
 * a TLS record, or trickles its response one byte at a time must still get a KL_ERR_TIMEOUT back
 * near timeout_ms. Each sync request runs on its own thread under a watchdog: a call that does not
 * return in time fails the test, after which the peer closes its socket to unstick it.
 *
 * Both clients: when a name resolves to several addresses, a refused connect to one moves on to the
 * next, and a resolver's socket type never makes the HTTP connection anything but TCP.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/http_client.h>
#include <keel/http_client_pool.h>
#include <keel/resolver.h>
#include <keel/clock.h>
#include "net_compat.h"
#include "platform_thread.h"
#include "platform_socket.h"
#include "resolve_sync.h"
#include "mock_tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Scripted peer ──────────────────────────────────────────────────────────────────────────── */

enum {
    PEER_HOLD,       /* accept, then neither read nor write */
    PEER_PARTIAL,    /* read the request head, send three bytes, then nothing */
    PEER_TRICKLE,    /* read the request head, then one byte of a response every 50 ms */
    PEER_REPLY       /* read the request head, send a complete 200 */
};

typedef struct {
    KlSocketHandle lfd;
    int port;
    int mode;
    volatile int release;   /* set by the test: close the connection and leave */
    KlPlatThread t;
} Peer;

static int peer_listen_family(Peer *p, int family) {
    if (kl_plat_socket_runtime_init() != 0) return -1;
    p->lfd = (KlSocketHandle)socket(family, SOCK_STREAM, 0);
    if (!kl_handle_valid(p->lfd)) return -1;
    if (family == AF_INET6) {
        struct sockaddr_in6 a;
        memset(&a, 0, sizeof a);
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_loopback;
        socklen_t al = sizeof a;
        if (bind((int)p->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen((int)p->lfd, 4) != 0 ||
            getsockname((int)p->lfd, (struct sockaddr *)&a, &al) != 0) return -1;
        p->port = ntohs(a.sin6_port);
        return 0;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind((int)p->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen((int)p->lfd, 4) != 0 ||
        getsockname((int)p->lfd, (struct sockaddr *)&a, &al) != 0) return -1;
    p->port = ntohs(a.sin_port);
    return 0;
}

static int peer_listen(Peer *p) { return peer_listen_family(p, AF_INET); }

static int peer_read_head(int fd) {
    char h[4096];
    size_t n = 0;
    while (n < sizeof h) {
        if (kl_test_poll1(fd, 0, 100) <= 0) return -1;
        if (kl_test_sockread(fd, h + n, 1) != 1) return -1;
        n++;
        if (n >= 4 && memcmp(h + n - 4, "\r\n\r\n", 4) == 0) return 0;
    }
    return -1;
}

static void peer_thread(void *arg) {
    Peer *p = arg;
    if (kl_test_poll1(p->lfd, 0, 5000) <= 0) return;
    int fd = (int)accept((int)p->lfd, NULL, NULL);
    if (fd < 0) return;
    static const char trickle[] = "HTTP/1.1 200 OK\r\nX-Slow: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    size_t sent = 0;
    int head_ok = 1;
    if (p->mode != PEER_HOLD) {
        for (int i = 0; i < 50 && !p->release; i++)   /* the head may take a moment to arrive */
            if ((head_ok = peer_read_head(fd) == 0) != 0) break;
    }
    if (head_ok && p->mode == PEER_PARTIAL) (void)kl_test_sockwrite(fd, "abc", 3);
    if (head_ok && p->mode == PEER_REPLY) (void)kl_test_sockwrite(fd, reply, sizeof reply - 1);
    for (int i = 0; i < 1000 && !p->release; i++) {   /* at most 10 s, then let go regardless */
        if (head_ok && p->mode == PEER_TRICKLE && sent < sizeof trickle - 1 && i % 5 == 0)
            sent += kl_test_sockwrite(fd, trickle + sent, 1) == 1 ? 1 : 0;
        kl_test_sleep_ms(10);
    }
    kl_test_closesock(fd);
}

static void peer_start(Peer *p) { (void)kl_plat_thread_create(&p->t, peer_thread, p); }
static void peer_join(Peer *p) {
    p->release = 1;
    kl_plat_thread_join(&p->t);
    kl_test_closesock((int)p->lfd);
}

/* ── Sync request on a thread, under a watchdog ─────────────────────────────────────────────── */

typedef struct {
    char url[96];
    const char *method;
    const char *body;
    size_t body_len;
    KlHttpClientConfig cfg;
    int pooled;
    volatile int done;
    int rc;
    KlError err;
    int status;
    KlPlatThread t;
} Run;

static void run_thread(void *arg) {
    Run *r = arg;
    KlAllocator a = kl_allocator_default();
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof resp);
    if (r->pooled) {
        KlHttpClientPool pool;
        if (kl_http_client_pool_init(&pool, NULL, &a, NULL) == 0) {
            r->rc = kl_http_client_request_pooled(&pool, &a, &r->cfg, r->method, r->url, NULL, 0,
                                                  r->body, r->body_len, &resp);
            r->err = resp.error;
            r->status = resp.status;
            kl_http_client_response_free(&resp);
            kl_http_client_pool_free(&pool);
        }
    } else {
        r->rc = kl_http_client_request(&a, &r->cfg, r->method, r->url, NULL, 0, r->body, r->body_len,
                                       &resp);
        r->err = resp.error;
        r->status = resp.status;
        kl_http_client_response_free(&resp);
    }
    r->done = 1;
}

/* Run r against p; returns the milliseconds the call took, or -1 if it did not return within
 * watchdog_ms (the peer is then released so the call can finish and the thread be joined). */
static long run_watched(Run *r, Peer *p, unsigned watchdog_ms) {
    r->rc = 0;
    r->err = KL_ERR_NONE;
    r->done = 0;
    uint64_t t0 = kl_monotonic_ms();
    peer_start(p);
    if (kl_plat_thread_create(&r->t, run_thread, r) != 0) { peer_join(p); return -2; }
    while (!r->done && kl_monotonic_ms() - t0 < watchdog_ms)
        kl_test_sleep_ms(5);
    long took = r->done ? (long)(kl_monotonic_ms() - t0) : -1;
    peer_join(p);
    kl_plat_thread_join(&r->t);
    return took;
}

static void run_init(Run *r, const char *scheme, int port, int timeout_ms) {
    memset(r, 0, sizeof *r);
    snprintf(r->url, sizeof r->url, "%s://127.0.0.1:%d/", scheme, port);
    r->method = "GET";
    r->cfg.timeout_ms = timeout_ms;
}

/* A TLS engine whose handshake waits for the server's first flight: it reads the socket, and only
 * a would-block read is WANT_READ. On a blocking socket that read never returns. */
static KlTlsResult waiting_handshake(KlTls *self, KlSocketHandle fd) {
    (void)self;
    char b[16];
    kl_ssize_t r = kl_sockdef_recv(fd, b, sizeof b);
    if (r > 0) return KL_TLS_ERROR;              /* this test's server never sends one */
    if (r == 0) return KL_TLS_ERROR;
    return kl_sockdef_io_status() == KL_IO_WOULD_BLOCK ? KL_TLS_WANT_READ : KL_TLS_ERROR;
}
static KlTls *waiting_tls_create(KlTlsCtx *ctx, KlAllocator *alloc) {
    KlTls *t = mock_tls_create(ctx, alloc);
    if (t) t->handshake = waiting_handshake;
    return t;
}
static KlTlsConfig g_waiting_tls = { .ctx = NULL, .factory = waiting_tls_create };
static KlTlsConfig g_mock_tls = { .ctx = NULL, .factory = mock_tls_create };

enum { TIMEOUT_MS = 300, WATCHDOG_MS = 2500, BOUND_MS = 1500 };

UTEST(client_deadline, sync_tls_handshake_with_silent_server_times_out) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    p.mode = PEER_HOLD;
    static Run r;
    run_init(&r, "https", p.port, TIMEOUT_MS);
    r.cfg.tls = &g_waiting_tls;
    long took = run_watched(&r, &p, WATCHDOG_MS);
    ASSERT_GE(took, 0);              /* -1: still blocked in the handshake at the watchdog */
    ASSERT_LT(took, (long)BOUND_MS);
    ASSERT_EQ(r.rc, -1);
    ASSERT_EQ(r.err, KL_ERR_TIMEOUT);
}

UTEST(client_deadline, sync_pooled_tls_handshake_with_silent_server_times_out) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    p.mode = PEER_HOLD;
    static Run r;
    run_init(&r, "https", p.port, TIMEOUT_MS);
    r.cfg.tls = &g_waiting_tls;
    r.pooled = 1;
    long took = run_watched(&r, &p, WATCHDOG_MS);
    ASSERT_GE(took, 0);
    ASSERT_LT(took, (long)BOUND_MS);
    ASSERT_EQ(r.rc, -1);
    ASSERT_EQ(r.err, KL_ERR_TIMEOUT);
}

/* The response's TLS record arrives in part and the rest never comes. */
UTEST(client_deadline, sync_tls_partial_record_times_out) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    p.mode = PEER_PARTIAL;
    static Run r;
    run_init(&r, "https", p.port, TIMEOUT_MS);
    r.cfg.tls = &g_mock_tls;
    mock_tls_split_record = 64;      /* the 3 bytes sent are part of a 64-byte record */
    long took = run_watched(&r, &p, WATCHDOG_MS);
    mock_tls_split_record = 0;
    ASSERT_GE(took, 0);
    ASSERT_LT(took, (long)BOUND_MS);
    ASSERT_EQ(r.rc, -1);
    ASSERT_EQ(r.err, KL_ERR_TIMEOUT);
}

/* An upload far larger than the socket buffers, to a server that never reads. */
UTEST(client_deadline, sync_upload_to_non_reading_server_times_out) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    p.mode = PEER_HOLD;
    enum { BODY = 64 * 1024 * 1024 };
    char *body = malloc(BODY);
    ASSERT_TRUE(body != NULL);
    memset(body, 'u', BODY);
    static Run r;
    run_init(&r, "http", p.port, TIMEOUT_MS);
    r.method = "POST";
    r.body = body;
    r.body_len = BODY;
    long took = run_watched(&r, &p, WATCHDOG_MS);
    free(body);
    ASSERT_GE(took, 0);              /* -1: still blocked in send at the watchdog */
    ASSERT_LT(took, (long)BOUND_MS);
    ASSERT_EQ(r.rc, -1);
    ASSERT_EQ(r.err, KL_ERR_TIMEOUT);
}

/* Each byte arrives well inside timeout_ms, but the response never completes. */
UTEST(client_deadline, sync_trickling_response_hits_the_request_deadline) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    p.mode = PEER_TRICKLE;
    static Run r;
    run_init(&r, "http", p.port, TIMEOUT_MS);
    long took = run_watched(&r, &p, WATCHDOG_MS);
    ASSERT_GE(took, 0);              /* -1: still reading the trickle at the watchdog */
    ASSERT_LT(took, (long)BOUND_MS);
    ASSERT_EQ(r.rc, -1);
    ASSERT_EQ(r.err, KL_ERR_TIMEOUT);
}

/* The deadline does not cut short a request that completes in time. */
UTEST(client_deadline, sync_prompt_response_succeeds) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    p.mode = PEER_REPLY;
    static Run r;
    run_init(&r, "https", p.port, 2000);
    r.cfg.tls = &g_mock_tls;
    long took = run_watched(&r, &p, 4000);
    ASSERT_GE(took, 0);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.status, 200);
}

/* ── Resolved address list ──────────────────────────────────────────────────────────────────── */

/* Listen on the LAST address "localhost" resolves to, so a client must get past a refused connect
 * to the first one. Returns 0 when set up, 1 when this host resolves localhost to a single family
 * (nothing to test), -1 on error. */
static int listen_on_second_localhost(Peer *p) {
    KlSockAddr addrs[KL_RESOLVE_MAX_ADDRS];
    int n = 0;
    if (kl_resolve_sync("localhost", 80, SOCK_STREAM, addrs, KL_RESOLVE_MAX_ADDRS, &n) != 0)
        return -1;
    if (n < 2) return 1;
    int first = kl_sockaddr_family(&addrs[0]);
    int last = kl_sockaddr_family(&addrs[n - 1]);
    if (first == last) return 1;
    return peer_listen_family(p, last == KL_AF_INET6 ? AF_INET6 : AF_INET);
}

UTEST(client_addrs, sync_tries_the_next_address) {
    static Peer p;
    memset(&p, 0, sizeof p);
    int lr = listen_on_second_localhost(&p);
    ASSERT_NE(lr, -1);
    if (lr == 1) UTEST_SKIP("localhost resolves to one address family here");
    p.mode = PEER_REPLY;
    static Run r;
    memset(&r, 0, sizeof r);
    snprintf(r.url, sizeof r.url, "http://localhost:%d/", p.port);
    r.method = "GET";
    r.cfg.timeout_ms = 3000;
    long took = run_watched(&r, &p, 5000);
    ASSERT_GE(took, 0);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.status, 200);
}

UTEST(client_addrs, sync_pooled_tries_the_next_address) {
    static Peer p;
    memset(&p, 0, sizeof p);
    int lr = listen_on_second_localhost(&p);
    ASSERT_NE(lr, -1);
    if (lr == 1) UTEST_SKIP("localhost resolves to one address family here");
    p.mode = PEER_REPLY;
    static Run r;
    memset(&r, 0, sizeof r);
    snprintf(r.url, sizeof r.url, "http://localhost:%d/", p.port);
    r.method = "GET";
    r.cfg.timeout_ms = 3000;
    r.pooled = 1;
    long took = run_watched(&r, &p, 5000);
    ASSERT_GE(took, 0);
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.status, 200);
}

typedef struct { int done; int error; int status; } AsyncDone;
static void async_done(KlHttpClient *c, void *ud) {
    AsyncDone *d = ud;
    d->done = 1;
    d->error = kl_http_client_error(c);
    const KlHttpClientResponse *r = kl_http_client_response(c);
    d->status = r ? r->status : 0;
}

static void run_async(const KlHttpClientConfig *cfg, const char *url, int pooled, AsyncDone *d) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &a) != 0) return;
    KlHttpClientPool pool;
    int pool_ok = pooled && kl_http_client_pool_init(&pool, NULL, &a, &ev) == 0;
    KlHttpClient *c = pool_ok
        ? kl_http_client_start_pooled(&pool, &ev, &a, cfg, "GET", url, NULL, 0, NULL, 0, async_done, d)
        : kl_http_client_start(&ev, &a, cfg, "GET", url, NULL, 0, NULL, 0, async_done, d);
    for (int i = 0; c && i < 400 && !d->done; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    kl_http_client_free(c);
    if (pool_ok) kl_http_client_pool_free(&pool);
    kl_event_ctx_free(&ev);
}

static void async_next_address_case(int *utest_result, int pooled) {
    static Peer p;
    memset(&p, 0, sizeof p);
    int lr = listen_on_second_localhost(&p);
    ASSERT_NE(lr, -1);
    if (lr == 1) UTEST_SKIP("localhost resolves to one address family here");
    p.mode = PEER_REPLY;
    peer_start(&p);
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 3000;
    cfg.system_dns = 1;              /* blocking name resolution: the list comes from the system */
    char url[64];
    snprintf(url, sizeof url, "http://localhost:%d/", p.port);
    AsyncDone d = { 0, 0, 0 };
    run_async(&cfg, url, pooled, &d);
    peer_join(&p);
    ASSERT_TRUE(d.done);
    ASSERT_EQ(d.error, 0);
    ASSERT_EQ(d.status, 200);
}

UTEST(client_addrs, async_system_dns_tries_the_next_address) {
    async_next_address_case(utest_result, 0);
}

UTEST(client_addrs, async_pooled_system_dns_tries_the_next_address) {
    async_next_address_case(utest_result, 1);
}

/* A resolver that reports a datagram socket type for its (TCP) result. */
static int g_dgram_port;
static KlResolveReq g_dgram_req;
static KlResolveReq *dgram_resolve(KlResolver *self, KlEventCtx *ctx, const char *host, int port,
                                   KlResolveDoneFn done_fn, void *ud) {
    (void)ctx; (void)host; (void)port;
    g_dgram_req.resolver = self;
    KlResolveResult r;
    memset(&r, 0, sizeof r);
    r.ai_socktype = SOCK_DGRAM;
    r.ai_protocol = IPPROTO_UDP;
    r.naddrs = 1;
    uint8_t lo[4] = { 127, 0, 0, 1 };
    kl_sockaddr_from_ipv4(&r.addrs[0], lo, (uint16_t)g_dgram_port);
    done_fn(&g_dgram_req, &r, 0, ud);
    return &g_dgram_req;
}
static void dgram_cancel(KlResolveReq *req) { (void)req; }
static void dgram_destroy(KlResolver *self) { (void)self; }
static KlResolver g_dgram_resolver = { .resolve = dgram_resolve, .cancel = dgram_cancel,
                                       .destroy = dgram_destroy };

UTEST(client_addrs, async_connection_is_tcp_whatever_the_resolver_says) {
    static Peer p;
    memset(&p, 0, sizeof p);
    ASSERT_EQ(peer_listen(&p), 0);
    p.mode = PEER_REPLY;
    g_dgram_port = p.port;
    peer_start(&p);
    KlHttpClientConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.timeout_ms = 2000;
    cfg.resolver = &g_dgram_resolver;
    AsyncDone d = { 0, 0, 0 };
    run_async(&cfg, "http://example.invalid/", 0, &d);
    peer_join(&p);
    ASSERT_TRUE(d.done);
    ASSERT_EQ(d.error, 0);
    ASSERT_EQ(d.status, 200);
}

UTEST_MAIN();
