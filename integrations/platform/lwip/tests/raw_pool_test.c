/*
 * raw_pool_test.c: the pooled async KlHttpClient over the lwIP-raw completion backend, in-process
 * over the loopback netif (NO_SYS=1, single loop thread), as raw_client_test.c sets it up.
 *
 * The pool tests a cached connection before handing it out (test-on-borrow: a non-consuming peek).
 * The raw provider's peek is real (kl_lwr_client_peek): would-block on an open, idle connection, 0
 * once the peer closed it. One pooled request, then, on the loop thread, the borrow the next pooled
 * request would make (kl_http_client_pool_acquire):
 *   1. right away, on an open and idle connection: a hit, the connection reused;
 *   2. after the server's idle timeout closed it: a miss, the closed connection dropped.
 * A peek that always failed would refuse every reuse (case 1 fails); one that never saw the close
 * would hand out a dead connection (case 2 fails).
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/resolver.h>
#include <keel/sockaddr.h>
#include <keel/timer.h>
#include <keel/http_client_pool.h>

#include "keel_lwip_raw.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* numeric-only KlResolver (no DNS, no UDP), as in raw_client_test.c */
typedef struct { KlResolver base; KlResolveReq req; } NumResolver;
static KlResolveReq *num_resolve(KlResolver *self, KlEventCtx *ctx, const char *host, int port,
                                 KlResolveDoneFn done_fn, void *ud) {
    (void)ctx;
    NumResolver *nr = (NumResolver *)self;
    KlResolveResult res;
    memset(&res, 0, sizeof(res));
    if (kl_sockaddr_parse(&res.addrs[0], host, (uint16_t)port) != 0) {
        done_fn(&nr->req, NULL, -1, ud);
        return &nr->req;
    }
    res.naddrs = 1;
    done_fn(&nr->req, &res, 0, ud);
    return &nr->req;
}
static void num_cancel(KlResolveReq *req) { (void)req; }
static void num_destroy(KlResolver *self) { (void)self; }
static NumResolver g_numres = { { num_resolve, num_cancel, num_destroy }, { NULL } };

static void handle_root(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_status(res, 200);
    kl_http_response_body_copy(res, "ok", 2);
}

typedef struct {
    KlHttpServer       *srv;
    KlHttpClientPool   *pool;
    const char         *url;
    int                 port;
    unsigned            gap_ms;      /* from the response to the borrow */
    KlHttpClient       *client;
    int                 status;
    int                 borrow;      /* kl_http_client_pool_acquire: 0 hit, 1 miss, -1 error */
    int                 reused;
    atomic_int          done;
} PoolCase;

static void finish(PoolCase *pc) {
    atomic_store(&pc->done, 1);
    kl_http_server_stop(pc->srv);
}
/* On the loop thread, after the gap: borrow the cached connection as the next pooled request would
 * (the pool peeks it first and drops it if the peer closed it). */
static void borrow(void *ud) {
    PoolCase *pc = ud;
    KlHttpClientPoolConn conn;
    memset(&conn, 0, sizeof conn);
    pc->borrow = kl_http_client_pool_acquire(pc->pool, "127.0.0.1", pc->port, 0, NULL, 0, &conn);
    pc->reused = conn.reused;
    if (pc->borrow == 0) kl_http_client_pool_discard(pc->pool, &conn);
    finish(pc);
}
static void on_done(KlHttpClient *client, void *ud) {
    PoolCase *pc = ud;
    const KlHttpClientResponse *r =
        kl_http_client_error(client) == 0 ? kl_http_client_response(client) : NULL;
    pc->status = r ? r->status : -1;
    (void)kl_timer_add(&pc->srv->ev, pc->gap_ms, borrow, pc);
}
static void start_client(void *ud) {
    PoolCase *pc = ud;
    static KlAllocator alloc;
    alloc = kl_allocator_default();
    KlHttpClientConfig ccfg = { .timeout_ms = 5000, .resolver = &g_numres.base };
    pc->client = kl_http_client_start_pooled(pc->pool, &pc->srv->ev, &alloc, &ccfg, "GET", pc->url,
                                             NULL, 0, NULL, 0, on_done, pc);
    if (!pc->client) { pc->status = -2; finish(pc); }
}

static void *server_thread(void *a) { kl_http_server_run((KlHttpServer *)a); return NULL; }

/* Returns the borrow's result (0 hit, 1 miss), or -2 on a failure. */
static int run_case(const char *label, int port, int idle_ms, unsigned gap_ms, int *reused) {
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);
    KlHttpServerConfig cfg = { .port = port, .bind_addr = "127.0.0.1", .read_timeout_ms = idle_ms,
                               .event_provider = kl_event_provider_lwip_raw() };
    KlHttpServer srv;
    if (kl_http_server_init(&srv, &cfg) != 0) { printf("POOL FAIL: server_init (%s)\n", label); return -2; }
    kl_http_server_route(&srv, "GET", "/", handle_root, NULL, NULL);
    static KlAllocator palloc;
    palloc = kl_allocator_default();
    KlHttpClientPool pool;
    if (kl_http_client_pool_init(&pool, NULL, &palloc, &srv.ev) != 0) {
        printf("POOL FAIL: pool_init (%s)\n", label);
        kl_http_server_free(&srv);
        return -2;
    }
    PoolCase pc;
    memset(&pc, 0, sizeof pc);
    pc.srv = &srv;
    pc.pool = &pool;
    pc.url = url;
    pc.port = port;
    pc.gap_ms = gap_ms;
    pc.borrow = -3;
    atomic_store(&pc.done, 0);
    kl_timer_add(&srv.ev, 20, start_client, &pc);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, &srv) != 0) {
        printf("POOL FAIL: pthread_create (%s)\n", label);
        kl_http_client_pool_free(&pool);
        kl_http_server_free(&srv);
        return -2;
    }
    for (int i = 0; i < 800 && !atomic_load(&pc.done); i++) {
        struct timespec sl = { 0, 10 * 1000000L };
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(&pc.done)) kl_http_server_stop(&srv);   /* hang guard */
    pthread_join(th, NULL);

    int rc = pc.borrow;
    if (!atomic_load(&pc.done)) { printf("POOL FAIL: HANG (%s)\n", label); rc = -2; }
    else if (pc.status != 200) { printf("POOL FAIL: (%s) status %d\n", label, pc.status); rc = -2; }
    *reused = pc.reused;
    if (pc.client) kl_http_client_free(pc.client);
    kl_http_client_pool_free(&pool);
    kl_http_server_free(&srv);
    return rc;
}

int main(void) {
    int reused = 0;
    /* 1. Open and idle: the peek would-blocks, so the cached connection is handed out again. */
    int r = run_case("open and idle", 7870, 5000, 10, &reused);
    printf("  open and idle: borrow %d, reused %d\n", r, reused);
    if (r != 0 || !reused) { printf("POOL FAIL: an open, idle pooled connection was refused\n"); return 1; }
    printf("PASS POOL (open and idle: reused)\n");
    /* 2. The server's idle timeout closed it first: the peek sees the close (0) and the pool drops
     *    it, so the borrow misses. */
    r = run_case("closed by the server", 7872, 200, 1500, &reused);
    printf("  closed by the server: borrow %d\n", r);
    if (r != 1) { printf("POOL FAIL: a connection the server closed was handed out\n"); return 1; }
    printf("PASS POOL (closed by the server: dropped)\n");
    printf("POOL PASS\n");
    return 0;
}
