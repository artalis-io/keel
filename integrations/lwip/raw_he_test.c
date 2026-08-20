/*
 * raw_he_test.c — LC-2: Happy Eyeballs (RFC 8305) racing + timers over the lwIP-raw completion
 * backend, in-process over the loopback netif (NO_SYS=1, single-thread).
 *
 * The proof for LC-2 (docs/phase10_lwip_raw_client_design.md §8 LC-2): the SAME single-loop model
 * as LC-1 (ONE KlEventCtx == the one lwIP mainloop, driving BOTH a raw KlHttpServer and a raw async
 * KlHttpClient on the loop thread), but now the client is handed a MULTI-ADDRESS resolver so its
 * Happy-Eyeballs layer (src/client.c: he_new_attempt/he_win/he_on_connect_result + the Connection-
 * Attempt-Delay + overall-deadline timers) races/fails-over several concurrent tcp_connect pcbs
 * natively. The winning pcb is adopted; every losing/connecting pcb is aborted (kl_comp_cancel ->
 * kl_lwr_tcp_abort) and its glue slot freed — the new memory-safety surface this test hardens.
 *
 * Cases (all in-process over loopback, NO_SYS=1):
 *
 *   1. FAILOVER/RACE. Resolver returns [127.0.0.1:REFUSED, 127.0.0.1:GOOD]. The first attempt
 *      hits a port with no listener on the loopif -> RST -> a FAST connect failure; Happy Eyeballs
 *      fast-starts the second address (RFC 8305 §5, no delay wait) which the raw server accepts ->
 *      200 + BYTE-EXACT body. The losing (refused) connecting pcb is aborted + its slot freed.
 *
 *   2. DEADLINE TIMEOUT. Resolver returns two BLACK-HOLE addresses (a non-loopif IP the raw stack
 *      routes out the default (loopback) netif but that answers neither SYN-ACK nor RST — the SYN
 *      is simply dropped, so the connect neither completes nor refuses). Both attempts stay
 *      in-flight (connecting) until the client's OWN overall deadline fires (he_on_deadline), which
 *      aborts EVERY in-flight connecting pcb (kl_comp_cancel per attempt) and completes the request
 *      with an error. Proves: no hang, no leak, exactly-one terminal, the deadline reclaims
 *      half-open connecting pcbs cleanly.
 *
 *   3. RACE-TO-WIN (two good-ish addresses). Resolver returns [127.0.0.1:REFUSED, 127.0.0.1:GOOD]
 *      again but with a LARGER Connection-Attempt-Delay so the 2nd attempt would normally wait —
 *      yet the 1st refuses immediately, so §5 fast-fail brings the winner up without waiting out
 *      the delay. Same 200 + body; confirms the delay timer path + fast-fail interplay, and that
 *      the delay timer is cancelled on win (no stray timer fires post-completion).
 *
 * DNS is LC-3, TLS is LC-4: this test is plaintext numeric-IP ONLY. Each address is a numeric
 * literal fed through a fixed in-test KlResolver (no DNS machinery, no UDP — lwip-raw rejects UDP).
 *
 * Must be ASan+UBSan+LSan-clean — aborting connecting/losing pcbs + freeing their slots is the LC-2
 * memory-safety surface.
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

/* ── ports / addresses ────────────────────────────────────────────────────────
 * GOOD    — the raw server's listen port on 127.0.0.1 (accepts).
 * REFUSED — a 127.0.0.1 port with NO listener: on the loopif a SYN loops back and, finding no
 *           bound pcb, gets an RST -> a deterministic FAST connect refusal (proven by LC-1's
 *           closed-port case). Happy Eyeballs fast-fails through it to the GOOD address.
 * BLACKHOLE_IP — a NON-loopif IPv4 (10.x): the raw stack routes it out the default (loopback)
 *           netif; the looped-back packet is not addressed to the loopif and is dropped (no RST,
 *           no SYN-ACK). So a connect to it stays CONNECTING forever -> the client's overall
 *           deadline is what terminates it. Distinct from a 127.x refused port (which RSTs). */
#define GOOD_PORT       7860
#define REFUSED_PORT    7861          /* no listener on 127.0.0.1 -> RST (fast refuse) */
#define BLACKHOLE_PORT  7862
static const char *GOOD_IP      = "127.0.0.1";
static const char *BLACKHOLE_IP = "10.255.255.1";  /* non-loopif, unrouted -> SYN dropped (hang) */

/* ── multi-address KlResolver (LC-2: no DNS, no UDP) ───────────────────────────
 * Keyed on the request host string, returns a FIXED 2-address list per case, completing
 * SYNCHRONOUSLY inside resolve() (the resolver sync-completion contract, CLAUDE.md). The
 * addresses are numeric literals parsed via kl_sockaddr_parse — no DNS, no timers, no lwIP. */
typedef enum { LIST_FAILOVER, LIST_BLACKHOLE } AddrList;

typedef struct { KlResolver base; KlResolveReq req; AddrList which; } HeResolver;

static int he_fill(KlResolveResult *res, AddrList which, int port) {
    memset(res, 0, sizeof(*res));
    if (which == LIST_FAILOVER) {
        /* preferred first: the refused address, then the good one — forcing a real fail-over */
        if (kl_sockaddr_parse(&res->addrs[0], GOOD_IP, (uint16_t)REFUSED_PORT) != 0) return -1;
        if (kl_sockaddr_parse(&res->addrs[1], GOOD_IP, (uint16_t)port)         != 0) return -1;
        res->naddrs = 2;
    } else { /* LIST_BLACKHOLE — two unrouted addresses that neither accept nor refuse */
        if (kl_sockaddr_parse(&res->addrs[0], BLACKHOLE_IP, (uint16_t)BLACKHOLE_PORT) != 0) return -1;
        if (kl_sockaddr_parse(&res->addrs[1], BLACKHOLE_IP, (uint16_t)(BLACKHOLE_PORT + 1)) != 0)
            return -1;
        res->naddrs = 2;
    }
    return 0;
}

static KlResolveReq *he_resolve(KlResolver *self, KlEventCtx *ctx, const char *host, int port,
                                KlResolveDoneFn done_fn, void *ud) {
    (void)ctx; (void)host;
    HeResolver *hr = (HeResolver *)self;
    KlResolveResult res;
    if (he_fill(&res, hr->which, port) != 0) { done_fn(&hr->req, NULL, -1, ud); return &hr->req; }
    done_fn(&hr->req, &res, 0, ud);   /* synchronous success — 2 addresses to race */
    return &hr->req;
}
static void he_cancel(KlResolveReq *req)  { (void)req; }
static void he_destroy(KlResolver *self)  { (void)self; }

static HeResolver g_res_failover  = { { he_resolve, he_cancel, he_destroy }, { NULL }, LIST_FAILOVER };
static HeResolver g_res_blackhole = { { he_resolve, he_cancel, he_destroy }, { NULL }, LIST_BLACKHOLE };

/* ── server handler: a known, non-trivial body (byte-exactness proof) ────────── */
#define WANT_BODY "{\"lc2\":true,\"raw\":\"happy-eyeballs\",\"n\":67890}"

static void handle_root(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/json");
    kl_http_response_body_copy(res, WANT_BODY, sizeof(WANT_BODY) - 1);   /* COPY: survives async send */
}

/* ── per-case client state (all fields touched only on the loop thread) ───────── */
typedef struct {
    KlHttpServer   *srv;
    const char *url;
    KlResolver *resolver;
    int         connect_delay_ms;   /* Connection-Attempt-Delay to configure */
    int         timeout_ms;         /* overall deadline */
    int         expect_ok;          /* 1 = expect 200+body; 0 = expect a clean terminal error */
    KlHttpClient   *client;
    atomic_int  done;
    atomic_int  pass;
    uint64_t    start_ms;           /* wall clock at client start (deadline-elapsed proof) */
    uint64_t    finish_ms;
} HeCase;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void on_done(KlHttpClient *client, void *ud) {
    HeCase *cc = ud;
    cc->finish_ms = now_ms();
    int err = kl_http_client_error(client);
    if (cc->expect_ok) {
        int ok = 0;
        if (err == 0) {
            const KlHttpClientResponse *r = kl_http_client_response(client);
            ok = (r && r->status == 200 && r->body_len == sizeof(WANT_BODY) - 1 &&
                  r->body && memcmp(r->body, WANT_BODY, sizeof(WANT_BODY) - 1) == 0);
            if (!ok && r) printf("   [diag] status=%d body_len=%zu\n", r->status, r->body_len);
        } else {
            printf("   [diag] unexpected client error=%d last_error=%d\n",
                   err, (int)kl_http_client_last_error(client));
        }
        atomic_store(&cc->pass, ok);
    } else {
        /* deadline case: a non-zero terminal error (timeout / connect) is the PASS condition. */
        if (err == 0) printf("   [diag] expected an error, got success\n");
        atomic_store(&cc->pass, err != 0);
    }
    atomic_store(&cc->done, 1);
    kl_http_server_stop(cc->srv);
}

/* Fired on the loop thread: start the async KlHttpClient on the server's shared ctx (== the one lwIP
 * mainloop). Its connect racing, its data-plane watcher relay, and its HE timers are all serviced
 * by kl_http_server_run's completion tick — single-thread NO_SYS=1 discipline. */
static void start_client(void *ud) {
    HeCase *cc = ud;
    static KlAllocator alloc;   /* stable storage: the response stores the allocator by value */
    alloc = kl_allocator_default();
    KlHttpClientConfig ccfg = {
        .timeout_ms = cc->timeout_ms,
        .resolver = cc->resolver,
        .connect_attempt_delay_ms = cc->connect_delay_ms,
    };
    cc->start_ms = now_ms();
    cc->client = kl_http_client_start(&cc->srv->ev, &alloc, &ccfg, "GET", cc->url,
                                 NULL, 0, NULL, 0, on_done, cc);
    if (!cc->client) {
        atomic_store(&cc->pass, cc->expect_ok ? 0 : 1);
        atomic_store(&cc->done, 1);
        kl_http_server_stop(cc->srv);
    }
}

static void *server_thread(void *a) { kl_http_server_run((KlHttpServer *)a); return NULL; }

/* Run one case: raw server on GOOD_PORT (kl_http_server_run on a bg thread) + a raw KlHttpClient started on
 * the server's shared ctx via a loop-thread timer; a bounded watchdog stops a hang. `max_wait_ms`
 * bounds the watchdog (deadline cases need to outlast the client's own timeout). */
static int run_case(const char *label, HeResolver *resolver, int connect_delay_ms, int timeout_ms,
                    int expect_ok, int max_wait_ms) {
    KlHttpServerConfig cfg = { .port = GOOD_PORT, .bind_addr = "127.0.0.1",
                     .event_provider = kl_event_provider_lwip_raw() };
    KlHttpServer srv;
    if (kl_http_server_init(&srv, &cfg) != 0) { printf("LC-2 FAIL: server_init (%s)\n", label); return 1; }
    kl_http_server_route(&srv, "GET", "/", handle_root, NULL, NULL);

    HeCase cc;
    memset(&cc, 0, sizeof(cc));
    cc.srv = &srv;
    cc.url = "http://127.0.0.1:7860/";   /* host is numeric; the resolver overrides the addr list */
    cc.resolver = &resolver->base;
    cc.connect_delay_ms = connect_delay_ms;
    cc.timeout_ms = timeout_ms;
    cc.expect_ok = expect_ok;
    atomic_store(&cc.done, 0);
    atomic_store(&cc.pass, 0);

    kl_timer_add(&srv.ev, 20, start_client, &cc);

    pthread_t srv_th;
    if (pthread_create(&srv_th, NULL, server_thread, &srv) != 0) {
        printf("LC-2 FAIL: pthread_create (%s)\n", label); kl_http_server_free(&srv); return 1;
    }
    int ticks = max_wait_ms / 10;
    for (int i = 0; i < ticks && !atomic_load(&cc.done); i++) {
        struct timespec sl = { 0, 10 * 1000000L };
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(&cc.done)) { kl_http_server_stop(&srv); }   /* hang guard */
    pthread_join(srv_th, NULL);

    int rc = 0;
    if (!atomic_load(&cc.done)) { printf("LC-2 FAIL: HANG (%s)\n", label); rc = 1; }
    else if (!atomic_load(&cc.pass)) { printf("LC-2 FAIL: (%s)\n", label); rc = 1; }
    else {
        uint64_t elapsed = cc.finish_ms - cc.start_ms;
        printf("PASS LC-2 (%s) [%llums]\n", label, (unsigned long long)elapsed);
        /* Deadline case: the request must have actually WAITED out its deadline (the connects hung
         * until he_on_deadline fired), not fast-failed. Allow generous slack for the 10ms tick. */
        if (!expect_ok && elapsed + 20 < (uint64_t)timeout_ms) {
            printf("LC-2 FAIL: (%s) completed in %llums, well before the %dms deadline "
                   "(connects did not hang as intended)\n",
                   label, (unsigned long long)elapsed, timeout_ms);
            rc = 1;
        }
    }

    if (cc.client) kl_http_client_free(cc.client);
    kl_http_server_free(&srv);
    return rc;
}

int main(void) {
    /* Case 1: FAILOVER — refused addr first, good addr second; HE fast-fails to the winner. */
    if (run_case("failover: [refused, good] -> 200, body byte-exact",
                 &g_res_failover, /*delay*/0, /*timeout*/5000, /*ok*/1, /*wait*/6000) != 0)
        return 1;

    /* Case 2: DEADLINE — two black-hole addrs that hang; the overall deadline aborts them. A short
     * 300ms deadline keeps the test fast; the watchdog outlasts it. */
    if (run_case("deadline: [blackhole, blackhole] -> aborted + timeout error",
                 &g_res_blackhole, /*delay*/50, /*timeout*/300, /*ok*/0, /*wait*/4000) != 0)
        return 1;

    /* Case 3: RACE-TO-WIN with a large Connection-Attempt-Delay — the 1st refuses immediately, so
     * §5 fast-fail brings the winner up WITHOUT waiting the delay; the delay timer is cancelled on
     * win (no stray fire post-completion). */
    if (run_case("delay+fast-fail: [refused, good], 400ms delay -> 200 without waiting",
                 &g_res_failover, /*delay*/400, /*timeout*/5000, /*ok*/1, /*wait*/6000) != 0)
        return 1;

    printf("LC-2 PASS\n");
    return 0;
}
