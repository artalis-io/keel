/*
 * raw_client_test.c — LC-1: a plaintext numeric-IPv4 async KlClient over the lwIP-raw
 * completion backend, in-process over the loopback netif (NO_SYS=1, single-thread).
 *
 * The proof for the raw client (docs/phase10_lwip_raw_client_design.md §8 LC-1): ONE KlEventCtx
 * (== the one lwIP mainloop) runs BOTH a raw KlServer (route GET / -> a known body) AND a raw
 * async KlClient that GETs http://127.0.0.1:PORT/. The server drives that one loop via
 * kl_server_run() on a background thread; the KlClient is created + driven ENTIRELY on that same
 * loop thread (started from a KEEL timer that fires on the loop thread, exactly the single-thread
 * marshalling discipline the Phase-9 raw tests use). So the client's connected_cb + request send +
 * response recv and the server's accept + recv + send all fire inline on the ONE thread — the
 * request round-trips over 127.0.0.1 with no data race (NO_SYS=1 has one lwIP stack, no locks).
 *
 * How the connect maps: kl_client_start (numeric literal -> a synchronous numeric resolver, no
 * DNS) -> start_connect on the completion loop -> kl_comp_post_connect -> lwr_comp_post_connect ->
 * kl_lwr_connect -> tcp_connect + lwr_cli_connected -> per-slot pend_connect -> KL_LWR_CONNECT ->
 * KL_COMP_CONNECT (mask-encoded) -> kl_event_dispatch -> the client's connect watcher -> SENDING.
 * The request send + response recv then ride the client's readiness watcher, which the raw backend
 * relays as KL_COMP_WATCHER (writable = sndbuf headroom; readable = retained rx / peer close), with
 * the client's kl_sock_send/kl_sock_recv on the pcb made real by the glue.
 *
 * Cases:
 *   1. GET / to the raw server  -> 200 + body BYTE-EXACT.
 *   2. GET / to a CLOSED port   -> a clean connect error (no hang, no leak).
 *
 * DNS is LC-3, TLS is LC-4, Happy-Eyeballs is LC-2: this test is plaintext numeric-IP ONLY. The
 * built-in kl_dns_resolver is NOT usable here (it eagerly kl_datagram_socket_init's, which lwip-raw rejects),
 * so the client is handed an explicit numeric-only KlResolver via KlClientConfig.resolver — the
 * clean LC-1-scoped way to avoid any DNS machinery.
 *
 * Must be ASan+UBSan+LSan-clean.
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/resolver.h>
#include <keel/sockaddr.h>
#include <keel/timer.h>

#include "keel_lwip_raw.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── numeric-only KlResolver (LC-1: no DNS, no UDP) ────────────────────────────
 * Parses a numeric IPv4/IPv6 literal via kl_sockaddr_parse and completes SYNCHRONOUSLY inside
 * resolve() — the resolver sync-completion contract (CLAUDE.md). No UDP, no timers, no lwIP. This
 * keeps LC-1 strictly plaintext numeric-IP; DNS is LC-3. */
typedef struct { KlResolver base; KlResolveReq req; } NumResolver;

static KlResolveReq *num_resolve(KlResolver *self, KlEventCtx *ctx, const char *host, int port,
                                 KlResolveDoneFn done_fn, void *ud) {
    (void)ctx;
    NumResolver *nr = (NumResolver *)self;
    KlResolveResult res;
    memset(&res, 0, sizeof(res));
    if (kl_sockaddr_parse(&res.addrs[0], host, (uint16_t)port) != 0) {
        done_fn(&nr->req, NULL, -1, ud);   /* not a numeric literal — synchronous failure */
        return &nr->req;
    }
    res.naddrs = 1;
    done_fn(&nr->req, &res, 0, ud);         /* synchronous success */
    return &nr->req;
}
static void num_cancel(KlResolveReq *req) { (void)req; }
static void num_destroy(KlResolver *self) { (void)self; }

static NumResolver g_numres = {
    { num_resolve, num_cancel, num_destroy }, { NULL }
};

/* ── server handler: a known, non-trivial body (byte-exactness proof) ────────── */
#define WANT_BODY "{\"lc1\":true,\"raw\":\"loopback\",\"n\":12345}"

static void handle_root(KlRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/json");
    kl_http_response_body_copy(res, WANT_BODY, sizeof(WANT_BODY) - 1);   /* COPY: survives async send */
}

/* ── client completion state (all fields touched only on the loop thread) ─────── */
typedef struct {
    KlServer  *srv;
    const char *url;
    int         expect_ok;      /* 1 = expect 200+body; 0 = expect a clean connect error */
    KlClient   *client;
    atomic_int  done;
    atomic_int  pass;
} ClientCase;

static void on_done(KlClient *client, void *ud) {
    ClientCase *cc = ud;
    int err = kl_client_error(client);
    if (cc->expect_ok) {
        int ok = 0;
        if (err == 0) {
            const KlClientResponse *r = kl_client_response(client);
            ok = (r && r->status == 200 && r->body_len == sizeof(WANT_BODY) - 1 &&
                  r->body && memcmp(r->body, WANT_BODY, sizeof(WANT_BODY) - 1) == 0);
            if (!ok && r) printf("   [diag] status=%d body_len=%zu\n", r->status, r->body_len);
        } else {
            printf("   [diag] unexpected client error=%d last_error=%d\n",
                   err, (int)kl_client_last_error(client));
        }
        atomic_store(&cc->pass, ok);
    } else {
        /* closed-port: a non-zero error (connect refused / timeout) is the PASS condition. */
        atomic_store(&cc->pass, err != 0);
    }
    atomic_store(&cc->done, 1);
    kl_server_stop(cc->srv);
}

/* Fired on the loop thread: start the async KlClient on the server's shared ctx (== the one lwIP
 * mainloop). Everything the client does from here rides that ctx — its connect completion, its
 * data-plane watcher relay, its timers — all serviced by kl_server_run's completion tick. */
static void start_client(void *ud) {
    ClientCase *cc = ud;
    static KlAllocator alloc;   /* stable storage: the response stores the allocator by value */
    alloc = kl_allocator_default();
    KlClientConfig ccfg = { .timeout_ms = 5000, .resolver = &g_numres.base };
    cc->client = kl_client_start(&cc->srv->ev, &alloc, &ccfg, "GET", cc->url,
                                 NULL, 0, NULL, 0, on_done, cc);
    if (!cc->client) {
        /* An immediate start failure is a clean fail for the closed-port case; a failure for the
         * OK case is a test failure. */
        atomic_store(&cc->pass, cc->expect_ok ? 0 : 1);
        atomic_store(&cc->done, 1);
        kl_server_stop(cc->srv);
    }
}

static void *server_thread(void *a) { kl_server_run((KlServer *)a); return NULL; }

/* Run one case: bring up a raw server (kl_server_run on a bg thread) + start a raw KlClient on the
 * server's shared ctx (via a loop-thread timer); a bounded watchdog stops a hang. */
static int run_case(const char *label, int port, const char *url, int expect_ok) {
    KlConfig cfg = { .port = port, .bind_addr = "127.0.0.1",
                     .event_provider = kl_event_provider_lwip_raw() };
    KlServer srv;
    if (kl_server_init(&srv, &cfg) != 0) { printf("LC-1 FAIL: server_init (%s)\n", label); return 1; }
    kl_server_route(&srv, "GET", "/", handle_root, NULL, NULL);

    ClientCase cc;
    memset(&cc, 0, sizeof(cc));
    cc.srv = &srv;
    cc.url = url;
    cc.expect_ok = expect_ok;
    atomic_store(&cc.done, 0);
    atomic_store(&cc.pass, 0);

    /* Marshal the client start onto the loop thread (single-thread NO_SYS=1 discipline): the timer
     * fires inside kl_server_run's tick, so the client is created + driven on the loop thread. */
    kl_timer_add(&srv.ev, 20, start_client, &cc);

    /* Run the shared loop (server + client) on a bg thread; the main thread is a bounded watchdog
     * that stops the loop if the case hangs (a stall is a FAIL, never a wedge). */
    pthread_t srv_th;
    if (pthread_create(&srv_th, NULL, server_thread, &srv) != 0) {
        printf("LC-1 FAIL: pthread_create (%s)\n", label); kl_server_free(&srv); return 1;
    }
    for (int i = 0; i < 600 && !atomic_load(&cc.done); i++) {
        struct timespec sl = { 0, 10 * 1000000L };
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(&cc.done)) { kl_server_stop(&srv); }   /* hang guard */
    pthread_join(srv_th, NULL);

    int rc = 0;
    if (!atomic_load(&cc.done)) { printf("LC-1 FAIL: HANG (%s)\n", label); rc = 1; }
    else if (!atomic_load(&cc.pass)) { printf("LC-1 FAIL: (%s)\n", label); rc = 1; }
    else { printf("PASS LC-1 (%s)\n", label); }

    if (cc.client) kl_client_free(cc.client);
    kl_server_free(&srv);
    return rc;
}

int main(void) {
    /* Case 1: GET / -> 200 + byte-exact body over the raw loopback. */
    if (run_case("GET / -> 200, body byte-exact", 7810,
                 "http://127.0.0.1:7810/", 1) != 0) return 1;
    /* Case 2: connect to a port with no listener -> a clean connect error (no hang, no leak). The
     * server binds a DIFFERENT port (keeps the raw ctx alive + the loop turning); the client
     * targets the unlistened port. */
    if (run_case("closed-port -> clean connect error", 7852,
                 "http://127.0.0.1:7853/", 0) != 0) return 1;

    printf("LC-1 PASS\n");
    return 0;
}
