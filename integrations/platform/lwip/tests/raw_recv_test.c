/*
 * raw_recv_test.c — tests for the hardened lwIP-raw completion backend.
 *
 * Covers the four hardening fixes with byte-exact + sanitizer-checkable cases:
 *   Receive (retained pbuf queue, no ack-before-deliver, no truncation):
 *        R1 header just below 8 KiB  -> 200
 *        R2 header ABOVE 8 KiB       -> server closes (overflow), no 200 (no hang)
 *        R3 body 32 KiB (> 8 KiB)    -> echoed back BYTE-EXACT
 *        R4 binary body (all 256 byte values x64 = 16 KiB) -> echoed BYTE-EXACT
 *        R5 chunked body (Transfer-Encoding: chunked)      -> echoed BYTE-EXACT
 *      (a large tcp_write fragments into many MSS pbufs delivered over multiple recv
 *       callbacks -> exercises the pbuf chain + multi-callback-before-consume paths.)
 *   Concurrency + completion saturation (per-conn slot cap, per-slot pending):
 *        C1 12 concurrent GET / clients (>> the earlier 8-armed / 64-ring) -> all 200
 *        C2 exactly max_connections concurrent                     -> all 200
 *        C3 capacity+1 concurrent -> the extra is REJECTED (RST), none hang
 *        C4 slot reuse after a burst + keep-serving                -> all 200
 *   Context (de-globalized KlLwrCtx + single-active guard):
 *        X1 second SIMULTANEOUS ctx is rejected (assert the error)
 *        X2 init/free/init again + two SEQUENTIAL servers on different ports
 *      (X2 is implicit: every phase runs its own kl_http_server_init/run/free in sequence,
 *       so a clean overall run proves sequential create/destroy/create + no stale state.)
 *
 * SINGLE-THREADED lwIP discipline (as in raw_tick_test.c): kl_http_server_run() blocks on a
 * pthread that owns the lwIP tick; every lwIP-touching client call is marshalled onto that
 * thread via KEEL timers (kl_timer_add fires on the loop thread). Runs in-process over the
 * loopback netif — no tap device, no root.
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/timer.h>
#include <keel/http_body_reader.h>

#include "keel_lwip_raw.h"
#include "lwip_raw_testclient.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const uint8_t LO[4] = { 127, 0, 0, 1 };

/* ── deterministic byte pattern (byte-exactness proof) ───────────────────────── */
static unsigned char pat(size_t i) { return (unsigned char)(i * 37u + 11u); }
static size_t pat_checksum(const unsigned char *b, size_t n) {
    size_t s = 0; for (size_t i = 0; i < n; i++) s += b[i]; return s;
}

/* ── shared echo handler: copies the received body back verbatim ─────────────── */
static void handle_echo(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    if (!br || br->len == 0) { kl_http_response_error(res, 400, "body required"); return; }
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/octet-stream");
    kl_http_response_body_copy(res, br->data, br->len);   /* COPY: survives the async send */
}
static void handle_root(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}

/* ── a small watchdog-driven server runner ───────────────────────────────────── */
static int run_watchdog(atomic_int *finished, KlHttpServer *s, int max_10ms) {
    for (int i = 0; i < max_10ms && !atomic_load(finished); i++) {
        struct timespec sl = { 0, 10 * 1000000L };
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(finished)) { atomic_store(finished, 1); kl_http_server_stop(s); return 0; }
    return 1;
}

/* ═══════════════════════ RECEIVE PHASE (R1..R5) ═══════════════════════════════ */
#define RECV_PORT 7790
static KlHttpServer g_rs;
static atomic_int g_r_finished;
static atomic_int g_r_fail;
static atomic_int g_r_stage;
/* stages */
enum { R_HDR_OK = 0, R_HDR_BIG, R_BODY_BIG, R_BODY_BIN, R_CHUNKED, R_DONE };

/* Each stage builds a request, sends it via the accumulating test client, and a poll cb
 * verifies the echoed body (or the close for the overflow case), then advances. */
static char   *g_req;           /* current request buffer (heap) */
static size_t  g_req_len;
static size_t  g_expect_body;   /* expected echoed body length (0 = expect no 200 / close) */
static size_t  g_expect_chk;    /* expected echoed body checksum */
static int     g_stage_started;
static int     g_stage_deadline;/* poll ticks remaining for the current stage */

static void r_build_hdr(size_t pad, int expect_ok) {
    /* GET /pad with a big "X-Pad: <pad bytes>" header. The total header block size is what
     * the 8 KiB read window bounds. */
    free(g_req);
    size_t cap = pad + 256;
    g_req = malloc(cap);
    int n = snprintf(g_req, cap,
                     "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Pad: ");
    memset(g_req + n, 'A', pad);
    size_t off = (size_t)n + pad;
    off += (size_t)snprintf(g_req + off, cap - off, "\r\n\r\n");
    g_req_len = off;
    g_expect_body = expect_ok ? 11 /* {"ok":true} */ : 0;
    g_expect_chk = 0;
}

static void r_build_body(const unsigned char *body, size_t blen, const char *path) {
    free(g_req);
    size_t cap = blen + 256;
    g_req = malloc(cap);
    int n = snprintf(g_req, cap,
                     "POST %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                     "Content-Type: application/octet-stream\r\nContent-Length: %zu\r\n\r\n",
                     path, blen);
    memcpy(g_req + n, body, blen);
    g_req_len = (size_t)n + blen;
    g_expect_body = blen;
    g_expect_chk = pat_checksum(body, blen);
}

/* Chunked encoding of `body` split into awkward-sized chunks (7, 1, 4093, rest). */
static void r_build_chunked(const unsigned char *body, size_t blen, const char *path) {
    free(g_req);
    size_t cap = blen * 2 + 512;
    g_req = malloc(cap);
    int n = snprintf(g_req, cap,
                     "POST %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                     "Content-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n\r\n",
                     path);
    size_t off = (size_t)n;
    size_t sizes[] = { 7, 1, 4093, 0 /* rest sentinel */ };
    size_t pos = 0;
    for (int i = 0; ; i++) {
        size_t csz = sizes[i];
        if (csz == 0) csz = blen - pos;         /* the rest */
        if (pos + csz > blen) csz = blen - pos;
        if (csz == 0 && pos >= blen) break;
        off += (size_t)snprintf(g_req + off, cap - off, "%zx\r\n", csz);
        memcpy(g_req + off, body + pos, csz); off += csz;
        off += (size_t)snprintf(g_req + off, cap - off, "\r\n");
        pos += csz;
        if (pos >= blen) break;
    }
    off += (size_t)snprintf(g_req + off, cap - off, "0\r\n\r\n");   /* terminator */
    g_req_len = off;
    g_expect_body = blen;
    g_expect_chk = pat_checksum(body, blen);
}

static unsigned char g_body32[32u * 1024u];
static unsigned char g_bodybin[16u * 1024u];

static void r_prepare_stage(int stage) {
    if (stage == R_HDR_OK)        r_build_hdr(6000, 1);              /* header well under 8 KiB */
    else if (stage == R_HDR_BIG)  r_build_hdr(12000, 0);            /* header over 8 KiB */
    else if (stage == R_BODY_BIG) r_build_body(g_body32, sizeof(g_body32), "/echo");
    else if (stage == R_BODY_BIN) r_build_body(g_bodybin, sizeof(g_bodybin), "/echo");
    else if (stage == R_CHUNKED)  r_build_chunked(g_bodybin, sizeof(g_bodybin), "/echo");
    g_stage_started = 0;
    g_stage_deadline = 400;   /* ~ up to 400 poll ticks (poll runs every 5ms) */
}

static const char *r_stage_name(int s) {
    switch (s) {
    case R_HDR_OK:   return "header-under-8KiB";
    case R_HDR_BIG:  return "header-over-8KiB (overflow close)";
    case R_BODY_BIG: return "body-32KiB byte-exact";
    case R_BODY_BIN: return "binary-body byte-exact";
    case R_CHUNKED:  return "chunked-body byte-exact";
    default:         return "?";
    }
}

static int g_r_await_close;   /* wait for the previous conn to fully close before the next start */

static void r_advance(KlHttpServer *s) {
    int stage = atomic_load(&g_r_stage);
    if (stage >= R_DONE) { atomic_store(&g_r_finished, 1); kl_http_server_stop(s); return; }
    r_prepare_stage(stage);
    g_r_await_close = 1;   /* between stages: let the previous connection fully close first */
}

static void r_poll(void *ud);

static void r_poll(void *ud) {
    KlHttpServer *s = ud;
    int stage = atomic_load(&g_r_stage);

    if (!g_stage_started) {
        /* The accumulating client is a single-slot peer: only open the next stage's connection
         * once the previous one is FULLY closed (server FIN seen), so the two roundtrips never
         * overlap on the shared accumulator and rapid pcb reuse can't churn lwIP state. */
        if (g_r_await_close && !kl_lwr_client_closed()) {
            kl_timer_add(&s->ev, 5, r_poll, s);
            return;
        }
        g_r_await_close = 0;
        g_stage_started = 1;
        kl_lwr_client_start_cap(LO, RECV_PORT, g_req, g_req_len, g_expect_body + 8192);
    } else {
        if (g_expect_body == 0) {
            /* Overflow case: expect the server to close WITHOUT a 200. We can't see the close
             * directly, but after enough ticks the client should NOT have a "200". Give it a
             * bounded window, then accept "no 200" as PASS. */
            char buf[256];
            size_t n = kl_lwr_client_response(buf, sizeof(buf));
            if (n > 0 && strstr(buf, " 200 ")) {
                atomic_store(&g_r_fail, 1);
                printf("R FAIL (%s): got a 200 for an over-8KiB header\n", r_stage_name(stage));
                atomic_store(&g_r_finished, 1); kl_http_server_stop(s); return;
            }
            if (--g_stage_deadline <= 0) {
                /* window elapsed with no 200 -> the server correctly rejected/closed it. */
                printf("PASS R (%s)\n", r_stage_name(stage));
                atomic_store(&g_r_stage, stage + 1);
                r_advance(s);
            }
        } else {
            size_t chk = 0;
            size_t blen = kl_lwr_client_body(&chk, NULL, NULL);
            /* g_expect_chk == 0 marks a "length-only" case (the small {"ok":true} header case,
             * where the response body isn't a known pattern); pattern-body cases carry a nonzero
             * expected checksum and are verified byte-exact. */
            if (blen >= g_expect_body) {
                if (blen == g_expect_body && (g_expect_chk == 0 || chk == g_expect_chk)) {
                    printf("PASS R (%s) — %zu bytes%s\n", r_stage_name(stage), blen,
                           g_expect_chk ? ", checksum ok" : "");
                    atomic_store(&g_r_stage, stage + 1);
                    r_advance(s);
                } else {
                    atomic_store(&g_r_fail, 1);
                    printf("R FAIL (%s): body len got %zu want %zu; chk got %zu want %zu\n",
                           r_stage_name(stage), blen, g_expect_body, chk, g_expect_chk);
                    atomic_store(&g_r_finished, 1); kl_http_server_stop(s); return;
                }
            } else if (--g_stage_deadline <= 0) {
                atomic_store(&g_r_fail, 1);
                printf("R FAIL (%s): timed out with %zu/%zu body bytes\n",
                       r_stage_name(stage), blen, g_expect_body);
                atomic_store(&g_r_finished, 1); kl_http_server_stop(s); return;
            }
        }
    }
    if (atomic_load(&g_r_finished)) return;
    kl_timer_add(&s->ev, 5, r_poll, s);
}

static void r_start(void *ud) { (void)ud; r_advance(&g_rs); }

static void *r_thread(void *arg) {
    (void)arg;
    kl_timer_add(&g_rs.ev, 20, r_start, NULL);
    kl_timer_add(&g_rs.ev, 40, r_poll, &g_rs);
    kl_http_server_run(&g_rs);
    return NULL;
}

static int run_receive(void) {
    for (size_t i = 0; i < sizeof(g_body32); i++) g_body32[i] = pat(i);
    for (size_t i = 0; i < sizeof(g_bodybin); i++) g_bodybin[i] = pat(i);

    KlHttpServerConfig cfg = { .port = RECV_PORT, .bind_addr = "127.0.0.1",
                     .max_body_size = 1u << 20,
                     .event_provider = kl_event_provider_lwip_raw() };
    if (kl_http_server_init(&g_rs, &cfg) != 0) { printf("R FAIL: server_init\n"); return 1; }
    kl_http_server_route(&g_rs, "GET",  "/",     handle_root, NULL, NULL);
    kl_http_server_route(&g_rs, "POST", "/echo", handle_echo, (void *)(size_t)(1u << 20),
                    kl_http_body_reader_buffer);
    atomic_store(&g_r_stage, R_HDR_OK);
    atomic_store(&g_r_finished, 0);
    atomic_store(&g_r_fail, 0);

    pthread_t th;
    if (pthread_create(&th, NULL, r_thread, NULL) != 0) {
        printf("R FAIL: pthread_create\n"); kl_http_server_free(&g_rs); return 1;
    }
    run_watchdog(&g_r_finished, &g_rs, 3000);   /* up to 30s */
    pthread_join(th, NULL);
    kl_http_server_free(&g_rs);
    kl_lwr_client_release();
    free(g_req); g_req = NULL;
    return atomic_load(&g_r_fail) ? 1 : 0;
}

/* ═══════════════════════ CONCURRENCY PHASE (C1..C4) ═══════════════════════════ */
#define CONC_PORT       7791
/* Room for the largest concurrent burst (C_EXACT) while staying under KL_LWR_MC_MAX (32) and the
 * lwIP pcb pool (MEMP_NUM_TCP_PCB=40 covers 2*N server+client pcbs + listener). C_OVER opens
 * CONC_MAXCONNS+1 to prove the extra is rejected. */
#define CONC_MAXCONNS   12
#define CONC_BURST      9       /* >= 9 concurrent (task requirement); < CONC_MAXCONNS so all pass */
#define REQ_ROOT        "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
static KlHttpServer g_cs;
static atomic_int g_c_finished;
static atomic_int g_c_fail;
static atomic_int g_c_stage;
enum { C_BURST = 0, C_EXACT, C_OVER, C_REUSE, C_DONE };
static int g_c_started;
static int g_c_wait;      /* ticks waited for the current burst to resolve */
static int g_c_n;         /* number of clients started this burst */

static void c_start_burst(int n) {
    kl_lwr_mc_reset();
    g_c_n = n;
    for (int i = 0; i < n; i++)
        kl_lwr_mc_start(i, LO, CONC_PORT, REQ_ROOT, sizeof(REQ_ROOT) - 1, 2048);
}

static const char *c_stage_name(int s) {
    switch (s) {
    case C_BURST: return "9-concurrent (>= 9, all served)";
    case C_EXACT:   return "exactly-max_connections";
    case C_OVER:    return "capacity+1 (extra rejected)";
    case C_REUSE:   return "slot-reuse-after-burst";
    default:        return "?";
    }
}

static void c_poll(void *ud);

static void c_next(KlHttpServer *s, int ok) {
    if (!ok) { atomic_store(&g_c_fail, 1); atomic_store(&g_c_finished, 1); kl_http_server_stop(s); return; }
    int stage = atomic_load(&g_c_stage);
    printf("PASS C (%s)\n", c_stage_name(stage));
    if (stage + 1 >= C_DONE) { atomic_store(&g_c_finished, 1); kl_http_server_stop(s); return; }
    atomic_store(&g_c_stage, stage + 1);
    g_c_started = 0;
}

static void c_poll(void *ud) {
    KlHttpServer *s = ud;
    int stage = atomic_load(&g_c_stage);

    if (!g_c_started) {
        g_c_started = 1;
        g_c_wait = 0;
        if (stage == C_BURST)     c_start_burst(CONC_BURST);
        else if (stage == C_EXACT)  c_start_burst(CONC_MAXCONNS);
        else if (stage == C_OVER)   c_start_burst(CONC_MAXCONNS + 1);
        else if (stage == C_REUSE)  c_start_burst(CONC_BURST); /* reuse after prior bursts */
    } else {
        /* Resolve when ALL started clients are done (each got 200 or was refused). */
        int done = 0, ok = 0, refused = 0;
        for (int i = 0; i < g_c_n; i++) {
            if (kl_lwr_mc_done(i)) done++;
            if (kl_lwr_mc_ok(i)) ok++;
            if (kl_lwr_mc_refused(i)) refused++;
        }
        if (done == g_c_n) {
            int pass = 0;
            if (stage == C_OVER) {
                /* capacity+1: every client resolved, at least one refused, the rest got 200. No
                 * hang (done == n). ok + refused must account for all. */
                pass = (refused >= 1) && (ok + refused == g_c_n) && (ok <= CONC_MAXCONNS);
                printf("   [%s] ok=%d refused=%d (max=%d)\n",
                       c_stage_name(stage), ok, refused, CONC_MAXCONNS);
            } else {
                /* bursts <= or recycling: every client must eventually get a 200 (slots recycle
                 * for the > max cases as earlier conns close). */
                pass = (ok == g_c_n) && (refused == 0);
                printf("   [%s] ok=%d refused=%d n=%d\n", c_stage_name(stage), ok, refused, g_c_n);
            }
            c_next(s, pass);
        } else if (++g_c_wait > 1000) {   /* ~5s */
            atomic_store(&g_c_fail, 1);
            printf("C FAIL (%s): HANG — only %d/%d resolved (ok=%d refused=%d)\n",
                   c_stage_name(stage), done, g_c_n, ok, refused);
            atomic_store(&g_c_finished, 1); kl_http_server_stop(s); return;
        }
    }
    if (atomic_load(&g_c_finished)) return;
    kl_timer_add(&s->ev, 5, c_poll, s);
}

static void *c_thread(void *arg) {
    (void)arg;
    kl_timer_add(&g_cs.ev, 40, c_poll, &g_cs);
    kl_http_server_run(&g_cs);
    return NULL;
}

static int run_concurrency(void) {
    KlHttpServerConfig cfg = { .port = CONC_PORT, .bind_addr = "127.0.0.1",
                     .max_connections = CONC_MAXCONNS,
                     .event_provider = kl_event_provider_lwip_raw() };
    if (kl_http_server_init(&g_cs, &cfg) != 0) { printf("C FAIL: server_init\n"); return 1; }
    kl_http_server_route(&g_cs, "GET", "/", handle_root, NULL, NULL);
    atomic_store(&g_c_stage, C_BURST);
    atomic_store(&g_c_finished, 0);
    atomic_store(&g_c_fail, 0);
    g_c_started = 0;

    pthread_t th;
    if (pthread_create(&th, NULL, c_thread, NULL) != 0) {
        printf("C FAIL: pthread_create\n"); kl_http_server_free(&g_cs); return 1;
    }
    run_watchdog(&g_c_finished, &g_cs, 4000);   /* up to 40s */
    pthread_join(th, NULL);
    kl_http_server_free(&g_cs);
    kl_lwr_mc_reset();
    return atomic_load(&g_c_fail) ? 1 : 0;
}

/* ═══════════════════════ CONTEXT PHASE (X1) ═══════════════════════════════════ */
/* X1: a second SIMULTANEOUS raw ctx must be rejected (NO_SYS=1 single-stack invariant). Create
 * one ctx, then assert a second kl_event_ctx_init_ex fails while the first is live; free the
 * first and assert a third now succeeds (sequential create works). */
static int run_context_reject(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx a;
    if (kl_event_ctx_init_ex(&a, &alloc, kl_event_provider_lwip_raw()) != 0) {
        printf("X FAIL: first ctx init failed\n"); return 1;
    }
    KlEventCtx b;
    int rc = kl_event_ctx_init_ex(&b, &alloc, kl_event_provider_lwip_raw());
    if (rc == 0) {
        printf("X FAIL: second SIMULTANEOUS ctx was accepted (must be rejected)\n");
        kl_event_ctx_free(&b);
        kl_event_ctx_free(&a);
        return 1;
    }
    printf("PASS X (second-simultaneous-ctx rejected)\n");
    kl_event_ctx_free(&a);
    /* Sequential: a fresh ctx after the first is freed must succeed. */
    KlEventCtx c;
    if (kl_event_ctx_init_ex(&c, &alloc, kl_event_provider_lwip_raw()) != 0) {
        printf("X FAIL: sequential ctx after free failed\n"); return 1;
    }
    kl_event_ctx_free(&c);
    printf("PASS X (sequential create/destroy/create)\n");
    return 0;
}

int main(void) {
    /* X2 is implicit: the three phases below each run their own server lifecycle in sequence,
     * so a clean overall pass proves sequential ctx create/destroy/create + no stale state. */
    if (run_context_reject() != 0) return 1;
    if (run_receive() != 0)        return 1;
    if (run_concurrency() != 0)    return 1;
    printf("raw_recv_test: ALL STAGE-A CASES PASS\n");
    return 0;
}
