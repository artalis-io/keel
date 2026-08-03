/*
 * raw_tick_test.c — Phase 9 lwIP-raw COMPLETION backend tests.
 *
 * P9-1 (retained): an lwIP-raw KlEventCtx ticks without a server — inits a ctx on the
 * lwIP-raw completion backend, asserts the loop's caps + socket provider are compatible,
 * drives it ~20 times via kl_event_ctx_run (each run = one lwIP mainloop tick), and frees
 * cleanly. Prints "P9-1 PASS".
 *
 * P9-2 (retained): a RAW-BACKED KlServer answers GET / over loopback. A real KlServer is
 * pinned to the lwIP-raw completion backend + its overlapped socket provider; the connection
 * state machine runs over the tcp_accept/tcp_recv/tcp_sent callbacks surfaced as
 * KL_COMP_ACCEPT/READ/WRITE. A raw-API test client (in the glue, like raw_loopback_spike.c)
 * connects to 127.0.0.1:PORT, writes "GET / HTTP/1.1...", and captures the response.
 *
 * P9-3 (this stage): full-payload send + backpressure + file send. Two cases FORCE multi-
 * round send (a response WELL larger than tcp_sndbuf ≈ 8*TCP_MSS ≈ 11-12 KB):
 *   (buffered) GET /big → a 64 KB body built with a known byte pattern. The client reads
 *              until it has the full Content-Length, then asserts body length + an additive
 *              checksum + first/last byte match. This spans many tcp_sent rounds and hits
 *              tcp_sndbuf-full (ERR_MEM) backpressure, resumed on tcp_sent. → "P9-3 PASS (buffered)"
 *   (file)     GET /file → a temp file (64 KB, same pattern) served via kl_response_file
 *              (FILE mode → kl_comp_post_sendfile → the glue preads it into the send buffer +
 *              reuses the send-pump). Client verifies the whole file body. → "P9-3 PASS (file)"
 *
 * SINGLE-THREADED lwIP discipline: NO_SYS=1 raw lwIP is not thread-safe and must run on ONE
 * thread. kl_server_run() blocks, so the server runs on a pthread that owns the lwIP tick —
 * and EVERY lwIP-touching client call (start + response poll) is scheduled onto THAT thread
 * via KEEL timers (kl_timer_add fires on the loop thread). The main thread only waits. So all
 * tcp_* calls (server + client) execute on the single tick thread; no lwIP data race.
 *
 * Runs in-process over the loopback netif — no tap device, no root — like the spike.
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/timer.h>

#include "keel_lwip_raw.h"
#include "lwip_raw_glue.h"   /* the raw-API test client (glue-owned, runs on the tick thread) */
#include "event_caps.h"      /* kl_event_ctx_sockets_compatible (internal negotiation) */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* ── P9-1 ─────────────────────────────────────────────────────────────────── */
#define TICK_ITERATIONS  20
#define TICK_MAX_EVENTS  16
#define TICK_TIMEOUT_MS  10

static int run_p9_1(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw()) != 0) {
        printf("P9-1 FAIL: kl_event_ctx_init_ex(lwip_raw) failed\n");
        return 1;
    }
    if (ctx.sockets == NULL)
        ctx.sockets = kl_event_native_provider(&ctx.loop);
    if (!kl_event_ctx_sockets_compatible(&ctx)) {
        printf("P9-1 FAIL: lwip-raw completion loop + socket provider not compatible\n");
        kl_event_ctx_free(&ctx);
        return 1;
    }
    for (int i = 0; i < TICK_ITERATIONS; i++) {
        int n = kl_event_ctx_run(&ctx, TICK_MAX_EVENTS, TICK_TIMEOUT_MS);
        if (n < 0) {
            printf("P9-1 FAIL: kl_event_ctx_run returned %d on tick %d\n", n, i);
            kl_event_ctx_free(&ctx);
            return 1;
        }
    }
    kl_event_ctx_free(&ctx);
    printf("P9-1 PASS\n");
    return 0;
}

/* ── shared large-body pattern (P9-3) ────────────────────────────────────────
 * A deterministic, non-trivial byte pattern so a checksum + first/last byte proves the
 * bytes arrived intact and in order (not just the right count). */
#define P9_3_BODY_LEN  (64u * 1024u)   /* WELL larger than tcp_sndbuf (~11-12 KB) */
static unsigned char pattern_byte(size_t i) { return (unsigned char)(i * 31u + 7u); }
static void fill_pattern(unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) buf[i] = pattern_byte(i);
}
static size_t pattern_checksum(size_t len) {
    size_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += pattern_byte(i);
    return sum;
}

/* ── P9-2 ─────────────────────────────────────────────────────────────────── */
#define P9_2_PORT       7780
#define P9_2_REQUEST    "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
#define P9_2_BODY       "{\"ok\":true}"

static KlServer g_srv;
static atomic_int g_done;       /* client saw "200" */
static atomic_int g_srv_stop;   /* stop signal for the server thread */

static void handle_root(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_response_json(res, 200, P9_2_BODY, sizeof(P9_2_BODY) - 1);
}

/* Fired on the tick thread once the server is bound + listening: start the raw client. */
static void start_client_cb(void *ud) {
    (void)ud;
    static const uint8_t lo[4] = { 127, 0, 0, 1 };
    kl_lwr_client_start(lo, P9_2_PORT, P9_2_REQUEST, sizeof(P9_2_REQUEST) - 1);
}

/* Repeating check (tick thread): has the client captured a "200" response yet? */
static void poll_client_cb(void *ud) {
    KlServer *s = ud;
    char buf[1024];
    size_t n = kl_lwr_client_response(buf, sizeof(buf));
    if (n > 0 && strstr(buf, "200") && strstr(buf, P9_2_BODY)) {
        atomic_store(&g_done, 1);
        kl_server_stop(s);
        return;
    }
    if (atomic_load(&g_srv_stop)) return;   /* timed out — stop re-arming */
    kl_timer_add(&s->ev, 10, poll_client_cb, s);   /* re-arm */
}

static void *server_thread(void *arg) {
    (void)arg;
    /* Schedule the client start + response polling onto THIS (tick) thread. The timers fire
     * from kl_server_run's completion loop, so all lwIP raw calls stay single-threaded.
     * Start slightly after the loop begins ticking so the listener is primed. */
    kl_timer_add(&g_srv.ev, 20, start_client_cb, NULL);
    kl_timer_add(&g_srv.ev, 40, poll_client_cb, &g_srv);
    kl_server_run(&g_srv);
    return NULL;
}

static int run_p9_2(void) {
    KlConfig cfg = { .port = P9_2_PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_lwip_raw() };
    if (kl_server_init(&g_srv, &cfg) != 0) {
        printf("P9-2 FAIL: kl_server_init (err=%d)\n", g_srv.last_error);
        return 1;
    }
    kl_server_route(&g_srv, "GET", "/", handle_root, NULL, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        printf("P9-2 FAIL: pthread_create\n");
        kl_server_free(&g_srv);
        return 1;
    }

    /* Watchdog: if the roundtrip doesn't complete in ~5s, stop the server and fail. */
    for (int i = 0; i < 500 && !atomic_load(&g_done); i++) {
        struct timespec sl = { 0, 10 * 1000000L };   /* 10ms */
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(&g_done)) {
        atomic_store(&g_srv_stop, 1);
        kl_server_stop(&g_srv);
    }
    pthread_join(th, NULL);
    kl_server_free(&g_srv);

    if (!atomic_load(&g_done)) {
        char buf[1024];
        size_t n = kl_lwr_client_response(buf, sizeof(buf));
        printf("P9-2 FAIL: raw-backed KlServer did not answer GET / with 200 over loopback"
               " (client got %zu bytes: \"%s\")\n", n, buf);
        return 1;
    }
    kl_lwr_client_release();
    printf("P9-2 PASS\n");
    return 0;
}

/* ── P9-3 ─────────────────────────────────────────────────────────────────────
 * One server, two routes (/big buffered, /file). Each case runs the same
 * server-thread + marshalled-client machinery as P9-2, parameterised by request +
 * verification. */
#define P9_3_PORT           7781
#define P9_3_REQ_BIG        "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
#define P9_3_REQ_FILE       "GET /file HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
#define P9_3_CLI_CAP        (P9_3_BODY_LEN + 4096u)   /* body + headers headroom */

static KlServer g_srv3;
/* Two cases run inside ONE kl_server_run (a single lwIP-raw loop can't be re-run cleanly,
 * and the glue's listen pcb + test client are process singletons). A small state machine
 * in the poll callback drives: buffered → file → done. */
enum { P3_BUFFERED = 0, P3_FILE = 1, P3_DONE = 2 };
static atomic_int g_p3_stage;   /* current case */
static atomic_int g_p3_pass;    /* bitmask: bit0 buffered ok, bit1 file ok */
static atomic_int g_p3_fail;    /* a case's body arrived but mismatched */
static atomic_int g_p3_finished;/* both cases resolved (pass or fail) */

static char g_file_path[256];   /* temp file path for the /file route (created in main) */

static void handle_big(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    /* Build the 64 KB body on the stack; kl_response_body_copy takes an owned copy. */
    static unsigned char body[P9_3_BODY_LEN];   /* static: 64 KB is large for a stack frame */
    fill_pattern(body, sizeof(body));
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "application/octet-stream");
    kl_response_body_copy(res, (const char *)body, sizeof(body));
}

static void handle_file(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    int fd = open(g_file_path, O_RDONLY);
    if (fd < 0) { kl_response_error(res, 500, "open failed"); return; }
    off_t sz = (off_t)P9_3_BODY_LEN;
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "application/octet-stream");
    /* FILE mode → the completion driver posts kl_comp_post_sendfile. The response layer
     * OWNS closing fd (kl_response_reset/free) — we do NOT close it here. */
    kl_response_file(res, fd, sz);
}

/* Verify the accumulated response body against the known pattern. Runs on the tick thread. */
static int verify_body(void) {
    size_t chk = 0; unsigned char first = 0, last = 0;
    size_t body_len = kl_lwr_client_body(&chk, &first, &last);
    if (body_len < P9_3_BODY_LEN) return 0;   /* not all here yet */
    if (body_len == P9_3_BODY_LEN &&
        chk == pattern_checksum(P9_3_BODY_LEN) &&
        first == pattern_byte(0) &&
        last == pattern_byte(P9_3_BODY_LEN - 1)) {
        return 1;   /* verified */
    }
    return -1;      /* wrong length/content */
}

/* Start the client for the current stage's request/route (runs on the tick thread). */
static void p3_start_stage(void) {
    static const uint8_t lo[4] = { 127, 0, 0, 1 };
    int stage = atomic_load(&g_p3_stage);
    if (stage == P3_BUFFERED)
        kl_lwr_client_start_cap(lo, P9_3_PORT, P9_3_REQ_BIG, sizeof(P9_3_REQ_BIG) - 1,
                                P9_3_CLI_CAP);
    else
        kl_lwr_client_start_cap(lo, P9_3_PORT, P9_3_REQ_FILE, sizeof(P9_3_REQ_FILE) - 1,
                                P9_3_CLI_CAP);
}

static void p3_poll_cb(void *ud) {
    KlServer *s = ud;
    int v = verify_body();
    if (v == 1) {
        int stage = atomic_load(&g_p3_stage);
        atomic_fetch_or(&g_p3_pass, 1 << stage);
        printf("P9-3 PASS (%s)\n", stage == P3_BUFFERED ? "buffered" : "file");
        if (stage == P3_BUFFERED) {
            atomic_store(&g_p3_stage, P3_FILE);
            p3_start_stage();                 /* kick the next case's connection */
        } else {
            atomic_store(&g_p3_finished, 1);
            kl_server_stop(s);
            return;
        }
    } else if (v < 0) {
        atomic_store(&g_p3_fail, 1);
        atomic_store(&g_p3_finished, 1);
        kl_server_stop(s);
        return;
    }
    if (atomic_load(&g_p3_finished)) return;
    kl_timer_add(&s->ev, 5, p3_poll_cb, s);
}

static void p3_start_cb(void *ud) {
    (void)ud;
    p3_start_stage();
}

static void *p3_server_thread(void *arg) {
    (void)arg;
    kl_timer_add(&g_srv3.ev, 20, p3_start_cb, NULL);
    kl_timer_add(&g_srv3.ev, 40, p3_poll_cb, &g_srv3);
    kl_server_run(&g_srv3);
    return NULL;
}

static int write_temp_file(void) {
    snprintf(g_file_path, sizeof(g_file_path), "/tmp/keel_p9_3_XXXXXX");
    int fd = mkstemp(g_file_path);
    if (fd < 0) return -1;
    unsigned char *body = malloc(P9_3_BODY_LEN);
    if (!body) { close(fd); return -1; }
    fill_pattern(body, P9_3_BODY_LEN);
    size_t off = 0;
    while (off < P9_3_BODY_LEN) {
        ssize_t w = write(fd, body + off, P9_3_BODY_LEN - off);
        if (w <= 0) { free(body); close(fd); return -1; }
        off += (size_t)w;
    }
    free(body);
    close(fd);
    return 0;
}

static int run_p9_3(void) {
    if (write_temp_file() != 0) {
        printf("P9-3 FAIL: could not create temp file\n");
        return 1;
    }

    KlConfig cfg = { .port = P9_3_PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_lwip_raw() };
    if (kl_server_init(&g_srv3, &cfg) != 0) {
        printf("P9-3 FAIL: kl_server_init (err=%d)\n", g_srv3.last_error);
        unlink(g_file_path);
        return 1;
    }
    kl_server_route(&g_srv3, "GET", "/big",  handle_big,  NULL, NULL);
    kl_server_route(&g_srv3, "GET", "/file", handle_file, NULL, NULL);

    atomic_store(&g_p3_stage, P3_BUFFERED);
    atomic_store(&g_p3_pass, 0);
    atomic_store(&g_p3_fail, 0);
    atomic_store(&g_p3_finished, 0);

    pthread_t th;
    if (pthread_create(&th, NULL, p3_server_thread, NULL) != 0) {
        printf("P9-3 FAIL: pthread_create\n");
        kl_server_free(&g_srv3);
        unlink(g_file_path);
        return 1;
    }
    /* Watchdog: ~15s for both cases to complete over loopback. */
    for (int i = 0; i < 1500 && !atomic_load(&g_p3_finished); i++) {
        struct timespec sl = { 0, 10 * 1000000L };
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(&g_p3_finished)) {
        atomic_store(&g_p3_finished, 1);
        kl_server_stop(&g_srv3);
    }
    pthread_join(th, NULL);

    kl_server_free(&g_srv3);
    kl_lwr_client_release();
    unlink(g_file_path);

    int pass = atomic_load(&g_p3_pass);
    int both = (1 << P3_BUFFERED) | (1 << P3_FILE);
    if ((pass & both) == both && !atomic_load(&g_p3_fail)) return 0;

    /* Report which stage failed + a content diff to aid debugging. */
    int stage = atomic_load(&g_p3_stage);
    size_t dchk = 0; unsigned char dfirst = 0, dlast = 0;
    size_t body_len = kl_lwr_client_body(&dchk, &dfirst, &dlast);
    printf("P9-3 FAIL (%s): expected %u body bytes, got %zu (total recv %zu); "
           "chk got %zu want %zu; first got 0x%02x want 0x%02x; last got 0x%02x want 0x%02x\n",
           stage == P3_BUFFERED ? "buffered" : "file",
           P9_3_BODY_LEN, body_len, kl_lwr_client_len(),
           dchk, pattern_checksum(P9_3_BODY_LEN),
           dfirst, pattern_byte(0), dlast, pattern_byte(P9_3_BODY_LEN - 1));
    for (size_t off = 0; off < body_len; ) {
        unsigned char chunk[4096];
        size_t n = kl_lwr_client_body_peek(off, chunk, sizeof(chunk));
        if (n == 0) break;
        for (size_t j = 0; j < n; j++)
            if (chunk[j] != pattern_byte(off + j)) {
                printf("  first mismatch at body[%zu]: got 0x%02x expected 0x%02x\n",
                       off + j, chunk[j], pattern_byte(off + j));
                return 1;
            }
        off += n;
    }
    return 1;
}

/* ── P9-4 — close / cancel / idle-timeout LIFETIME + MEMORY SAFETY ──────────────
 * All four cases run inside ONE kl_server_run over ONE lwIP-raw loop (a raw NO_SYS=1 loop +
 * the glue's listen pcb + test client are process singletons — like P9-3). A state machine in
 * the poll callback drives them sequentially on the tick thread. Cases:
 *   C1 many-short-lived  — N (>> 8) sequential open/serve/close roundtrips to GET / : all get
 *                          200, slots recycle, no leak / overflow of the bounded slot table.
 *   C2 close-with-outstanding — GET /big (64 KB); client reads a little then FIN (mode 2). The
 *                          server tears down the in-flight send cleanly (send buffer freed).
 *   C3 reset-mid-send    — GET /big; client reads a little then RST (mode 1) → server tcp_err →
 *                          free-by-owner (pcb already dead) + exactly-one close, no UAF.
 *   C4 idle-timeout      — a raw client connects, sends a partial request (no CRLFCRLF) and
 *                          never completes it; the server's idle sweep (short read_timeout_ms)
 *                          calls kl_comp_cancel → tcp_abort → terminal completion → one release.
 * Each case prints "P9-4 PASS (...)". The whole run is under ASan/UBSan/LSan in CI. */
#define P9_4_PORT           7782
#define P9_4_REQ_ROOT       "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
#define P9_4_REQ_BIG        "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
/* A deliberately INCOMPLETE request (no terminating CRLFCRLF) so the server never routes it and
 * the idle sweep fires. */
#define P9_4_REQ_PARTIAL    "GET / HTTP/1.1\r\nHost: x\r\n"
#define P9_4_MANY_CONNS     50            /* N >> KL_LWR_MAX_CONNS (8/32) — forces recycling */
#define P9_4_PARTIAL_READ   2048u         /* bytes the partial-read client takes before tearing down */
#define P9_4_CLI_CAP        (P9_3_BODY_LEN + 4096u)

static KlServer g_srv4;
enum { P4_MANY = 0, P4_CWO = 1, P4_RST = 2, P4_IDLE = 3, P4_DONE = 4 };
static atomic_int g_p4_stage;
static atomic_int g_p4_pass;            /* bitmask of passed cases */
static atomic_int g_p4_fail;
static atomic_int g_p4_finished;
static int        g_p4_many_started;    /* how many roundtrips in C1 have been kicked */

static void handle4_root(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_response_json(res, 200, P9_2_BODY, sizeof(P9_2_BODY) - 1);
}
static void handle4_big(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    static unsigned char body[P9_3_BODY_LEN];
    fill_pattern(body, sizeof(body));
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "application/octet-stream");
    kl_response_body_copy(res, (const char *)body, sizeof(body));
}

static const uint8_t g_lo4[4] = { 127, 0, 0, 1 };

static void p4_pass(int stage, const char *name) {
    atomic_fetch_or(&g_p4_pass, 1 << stage);
    printf("P9-4 PASS (%s)\n", name);
}

static void p4_poll_cb(void *ud) {
    KlServer *s = ud;
    int stage = atomic_load(&g_p4_stage);

    if (stage == P4_MANY) {
        /* Kick roundtrips one at a time; advance only when the previous one resolved. */
        if (g_p4_many_started == 0) {
            g_p4_many_started = 1;
            kl_lwr_lc_reset_counter();
            kl_lwr_lc_start(g_lo4, P9_4_PORT, P9_4_REQ_ROOT, sizeof(P9_4_REQ_ROOT) - 1,
                            0 /*full*/, 0);
        } else if (kl_lwr_lc_done()) {
            int completed = kl_lwr_lc_completed();
            if (!kl_lwr_lc_saw_200()) { atomic_store(&g_p4_fail, 1); goto stop; }
            if (completed >= P9_4_MANY_CONNS) {
                p4_pass(P4_MANY, "many-short-lived");
                atomic_store(&g_p4_stage, P4_CWO);
                g_p4_many_started = 0;
            } else {
                kl_lwr_lc_start(g_lo4, P9_4_PORT, P9_4_REQ_ROOT, sizeof(P9_4_REQ_ROOT) - 1,
                                0, 0);
            }
        }
    } else if (stage == P4_CWO) {
        if (!g_p4_many_started) {
            g_p4_many_started = 1;
            kl_lwr_lc_start(g_lo4, P9_4_PORT, P9_4_REQ_BIG, sizeof(P9_4_REQ_BIG) - 1,
                            2 /*partial+FIN*/, P9_4_PARTIAL_READ);
        } else if (kl_lwr_lc_done()) {
            p4_pass(P4_CWO, "close-with-outstanding");
            atomic_store(&g_p4_stage, P4_RST);
            g_p4_many_started = 0;
        }
    } else if (stage == P4_RST) {
        if (!g_p4_many_started) {
            g_p4_many_started = 1;
            kl_lwr_lc_start(g_lo4, P9_4_PORT, P9_4_REQ_BIG, sizeof(P9_4_REQ_BIG) - 1,
                            1 /*partial+RST*/, P9_4_PARTIAL_READ);
        } else if (kl_lwr_lc_done()) {
            p4_pass(P4_RST, "reset-mid-send");
            atomic_store(&g_p4_stage, P4_IDLE);
            g_p4_many_started = 0;
        }
    } else if (stage == P4_IDLE) {
        /* Send a partial (never-completed) request; the server's short read_timeout_ms drives
         * the idle sweep → kl_comp_cancel → tcp_abort → terminal completion → one release. The
         * client's lc_err resolves when the server RSTs it. */
        if (!g_p4_many_started) {
            g_p4_many_started = 1;
            /* mode 0 but the request never completes, so the server never responds; the idle
             * sweep tears it down and RSTs us → lc_err resolves g_lc_done. */
            kl_lwr_lc_start(g_lo4, P9_4_PORT, P9_4_REQ_PARTIAL, sizeof(P9_4_REQ_PARTIAL) - 1,
                            0, 0);
        } else if (kl_lwr_lc_done()) {
            p4_pass(P4_IDLE, "idle-timeout/kl_comp_cancel");
            atomic_store(&g_p4_stage, P4_DONE);
            atomic_store(&g_p4_finished, 1);
            kl_server_stop(s);
            return;
        }
    }

    if (atomic_load(&g_p4_finished)) return;
    kl_timer_add(&s->ev, 5, p4_poll_cb, s);
    return;
stop:
    atomic_store(&g_p4_finished, 1);
    kl_server_stop(s);
}

static void *p4_server_thread(void *arg) {
    (void)arg;
    kl_timer_add(&g_srv4.ev, 40, p4_poll_cb, &g_srv4);
    kl_server_run(&g_srv4);
    return NULL;
}

static int run_p9_4(void) {
    /* Short read timeout so the idle-timeout case (C4) fires quickly. */
    KlConfig cfg = { .port = P9_4_PORT, .bind_addr = "127.0.0.1",
                     .read_timeout_ms = 200,
                     .sockets = kl_socket_provider_lwip_raw() };
    if (kl_server_init(&g_srv4, &cfg) != 0) {
        printf("P9-4 FAIL: kl_server_init (err=%d)\n", g_srv4.last_error);
        return 1;
    }
    kl_server_route(&g_srv4, "GET", "/",    handle4_root, NULL, NULL);
    kl_server_route(&g_srv4, "GET", "/big", handle4_big,  NULL, NULL);

    atomic_store(&g_p4_stage, P4_MANY);
    atomic_store(&g_p4_pass, 0);
    atomic_store(&g_p4_fail, 0);
    atomic_store(&g_p4_finished, 0);
    g_p4_many_started = 0;

    pthread_t th;
    if (pthread_create(&th, NULL, p4_server_thread, NULL) != 0) {
        printf("P9-4 FAIL: pthread_create\n");
        kl_server_free(&g_srv4);
        return 1;
    }
    /* Watchdog: ~30s for all four cases (many-conns dominates). */
    for (int i = 0; i < 3000 && !atomic_load(&g_p4_finished); i++) {
        struct timespec sl = { 0, 10 * 1000000L };
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(&g_p4_finished)) {
        atomic_store(&g_p4_finished, 1);
        kl_server_stop(&g_srv4);
    }
    pthread_join(th, NULL);
    kl_server_free(&g_srv4);

    int pass = atomic_load(&g_p4_pass);
    int all = (1 << P4_MANY) | (1 << P4_CWO) | (1 << P4_RST) | (1 << P4_IDLE);
    if ((pass & all) == all && !atomic_load(&g_p4_fail)) return 0;

    printf("P9-4 FAIL: stage=%d pass_mask=0x%x fail=%d (last roundtrip: completed=%d recv=%zu"
           " saw200=%d)\n",
           atomic_load(&g_p4_stage), pass, atomic_load(&g_p4_fail),
           kl_lwr_lc_completed(), kl_lwr_lc_recv(), kl_lwr_lc_saw_200());
    return 1;
}

int main(void) {
    if (run_p9_1() != 0) return 1;
    if (run_p9_2() != 0) return 1;
    if (run_p9_3() != 0) return 1;
    if (run_p9_4() != 0) return 1;
    return 0;
}
