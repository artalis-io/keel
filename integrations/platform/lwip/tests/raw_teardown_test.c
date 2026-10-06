/*
 * raw_teardown_test.c: connection teardown on the lwIP-raw completion backend.
 *
 * Three ways a connection can die under a posted op, each deterministic over the in-process
 * loopback netif:
 *
 *   T1 abort inside an lwIP callback. A file response whose file shrinks after the response was
 *      started: the send pump's pread runs dry inside the tcp_sent callback (the first pump, outside
 *      any callback, covers at most TCP_SND_BUF bytes, far less than the surviving file), and the
 *      pump aborts the pcb there. lwIP's tcp_input skips its post-callback work only when the
 *      callback returns ERR_ABRT; on any other return it keeps using the freed pcb. The memp pools
 *      keep that memory mapped, so ASan cannot see it: the callback-contract guard
 *      (kl_lwr_guard_server_pcbs) wraps the server pcb's callbacks and counts a callback that freed
 *      its pcb without returning ERR_ABRT. Asserts: no violation, at least one guarded abort (so
 *      the abort really ran inside a callback), the connection is released, and the server serves
 *      the next request.
 *
 *   T2 peer reset with a send AND a receive posted. "Expect: 100-continue" makes the server post
 *      the 100 Continue send and then the body receive in one dispatch. The client resets on the
 *      first response bytes without acknowledging them, so both ops are outstanding when the RST
 *      arrives. Each must complete (failed): the server releases a connection only after its last
 *      posted op completes. Asserts: the client saw "100 Continue" (the precondition), the server's
 *      active connection count returns to 0, and the next request is served.
 *
 *   T3 a dead connection's close after its pcb address was reused. Connection A is accepted (its
 *      receive posted), then reset by the client, and connection B is opened in the same timer
 *      callback. lwIP's memp pools are LIFO, so B's server pcb is A's freed one. The RST and the
 *      SYN are handled in one loop tick, before the driver sees A's failure, so A's close runs with
 *      B live at the same address. Asserts: B's server pcb IS A's old one (the precondition: the
 *      test fails rather than pass vacuously if the reuse did not happen), B gets its 200, the
 *      server's active count returns to 0, and a further connection is served.
 *
 * SINGLE-THREADED lwIP discipline (as in raw_send_test.c): kl_http_server_run() blocks on a pthread
 * that owns the lwIP tick; every lwIP-touching client call is marshalled onto that thread via KEEL
 * timers (kl_timer_add fires on the loop thread). The main thread only waits.
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
#include <fcntl.h>

static const uint8_t LO[4] = { 127, 0, 0, 1 };

#define POLL_MS        5
#define DEADLINE_POLLS 600          /* 600 * 5 ms: generous; every case settles in a few ticks */

static unsigned char pat(size_t i) { return (unsigned char)(i * 37u + 11u); }
static size_t pat_checksum_of(size_t n) {
    size_t s = 0; for (size_t i = 0; i < n; i++) s += pat(i); return s;
}

static int active_conns(const KlHttpServer *s) {
    KlHttpServerStats st;
    kl_http_server_stats(s, &st);
    return st.active_connections;
}

/* Small buffered response (a COPY: survives the async send). */
#define SMALL_LEN 1024u
static void handle_small(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    unsigned char buf[SMALL_LEN];
    for (size_t i = 0; i < SMALL_LEN; i++) buf[i] = pat(i);
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/octet-stream");
    kl_http_response_body_copy(res, (const char *)buf, SMALL_LEN);
}

static const char REQ_SMALL[] = "GET /s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

static int run_watchdog(atomic_int *finished, KlHttpServer *s, int max_10ms) {
    for (int i = 0; i < max_10ms && !atomic_load(finished); i++) {
        struct timespec sl = { 0, 10 * 1000000L };
        nanosleep(&sl, NULL);
    }
    if (!atomic_load(finished)) { atomic_store(finished, 1); kl_http_server_stop(s); return 0; }
    return 1;
}

/* One case = one server on its own loop thread, driven by a repeating poll timer. */
typedef struct {
    KlHttpServer srv;
    atomic_int   finished;
    atomic_int   fail;
    int          stage;
    int          deadline;
    void       (*poll)(void *ud);
} Case;

static void case_end(Case *c, int failed) {
    if (failed) atomic_store(&c->fail, 1);
    atomic_store(&c->finished, 1);
    kl_http_server_stop(&c->srv);
}

static void case_rearm(Case *c) {
    if (!atomic_load(&c->finished)) kl_timer_add(&c->srv.ev, POLL_MS, c->poll, c);
}

static void *case_thread(void *arg) {
    Case *c = arg;
    kl_timer_add(&c->srv.ev, 20, c->poll, c);
    kl_http_server_run(&c->srv);
    return NULL;
}

static int case_run(Case *c, const char *name) {
    pthread_t th;
    if (pthread_create(&th, NULL, case_thread, c) != 0) {
        printf("%s FAIL: pthread_create\n", name);
        kl_http_server_free(&c->srv);
        return 1;
    }
    if (!run_watchdog(&c->finished, &c->srv, 3000)) {
        atomic_store(&c->fail, 1);
        printf("%s FAIL: watchdog expired (stage %d)\n", name, c->stage);
    }
    pthread_join(th, NULL);
    kl_http_server_free(&c->srv);
    return atomic_load(&c->fail) ? 1 : 0;
}

/* Healthy follow-up roundtrip on the accumulating client: 1 = done ok, 0 = pending, -1 = failed. */
static int small_roundtrip_result(void) {
    size_t chk = 0, blen = kl_lwr_client_body(&chk, NULL, NULL);
    if (blen >= SMALL_LEN)
        return (blen == SMALL_LEN && chk == pat_checksum_of(SMALL_LEN)) ? 1 : -1;
    return 0;
}

/* ═══════════════════════ T1: abort inside the tcp_sent callback ═══════════════ */
#define T1_PORT      7840
#define T1_DECLARED  (1024u * 1024u)       /* Content-Length the response promises */
#define T1_SURVIVES  (256u * 1024u)        /* what is left on disk once the send has started */

static Case g_t1;
static char g_t1_path[512];
static int  g_t1_handler_ran, g_t1_guarded;

static int t1_make_file(void) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(g_t1_path, sizeof(g_t1_path), "%s/keel_raw_teardown_%d.bin", dir, (int)getpid());
    int fd = open(g_t1_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    unsigned char chunk[65536];
    size_t off = 0;
    while (off < T1_DECLARED) {
        size_t n = T1_DECLARED - off < sizeof(chunk) ? T1_DECLARED - off : sizeof(chunk);
        for (size_t i = 0; i < n; i++) chunk[i] = pat(off + i);
        if (write(fd, chunk, n) != (ssize_t)n) { close(fd); return -1; }
        off += n;
    }
    close(fd);
    return 0;
}

static void t1_handle_file(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    int fd = open(g_t1_path, O_RDONLY);
    if (fd < 0) { kl_http_response_error(res, 500, "open"); return; }
    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "application/octet-stream");
    kl_http_response_file(res, fd, T1_DECLARED);
    /* The file shrinks under the response: the pump runs dry once it passes T1_SURVIVES, which is
     * many tcp_sent rounds after the first (post-path) pump. */
    if (truncate(g_t1_path, T1_SURVIVES) != 0) printf("     T1: truncate failed\n");
    /* Wrap this connection's server pcb before the send is posted (still in the handler). */
    g_t1_guarded = kl_lwr_guard_server_pcbs(T1_PORT);
    g_t1_handler_ran = 1;
}

enum { T1_FILE = 0, T1_HEALTHY, T1_DONE };

static void t1_poll(void *ud) {
    Case *c = ud;
    KlHttpServer *s = &c->srv;
    if (c->stage == T1_FILE) {
        if (c->deadline == 0) {          /* first entry: start the file request */
            kl_lwr_client_start_cap(LO, T1_PORT, "GET /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
                                    sizeof("GET /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") - 1,
                                    T1_DECLARED + 8192);
            c->deadline = DEADLINE_POLLS;
            case_rearm(c);
            return;
        }
        if (!(g_t1_handler_ran && active_conns(s) == 0)) {
            if (--c->deadline <= 1) {
                printf("T1 FAIL: file connection not released (handler ran %d, active %d, "
                       "guard violations %d)\n", g_t1_handler_ran, active_conns(s),
                       kl_lwr_guard_violations());
                case_end(c, 1);
                return;
            }
            case_rearm(c);
            return;
        }
        size_t blen = kl_lwr_client_body(NULL, NULL, NULL);
        int bad = 0;
        if (g_t1_guarded < 1) { printf("T1 FAIL: guard wrapped no server pcb\n"); bad = 1; }
        if (kl_lwr_guard_violations() != 0) {
            printf("T1 FAIL: %d lwIP callback(s) freed the pcb without returning ERR_ABRT\n",
                   kl_lwr_guard_violations());
            bad = 1;
        } else if (kl_lwr_guard_aborts() < 1) {
            printf("T1 FAIL: the pump abort did not run inside a guarded callback "
                   "(calls %d): the scenario was not exercised\n", kl_lwr_guard_calls());
            bad = 1;
        }
        if (blen >= T1_DECLARED) { printf("T1 FAIL: complete body from a truncated file\n"); bad = 1; }
        if (bad) { case_end(c, 1); return; }
        printf("PASS T1 (file shrank mid-send: abort in tcp_sent returned ERR_ABRT; %d guarded calls, "
               "%zu body bytes, connection released)\n", kl_lwr_guard_calls(), blen);
        c->stage = T1_HEALTHY;
        c->deadline = 0;
    }
    if (c->stage == T1_HEALTHY) {
        if (c->deadline == 0) {
            kl_lwr_client_start_cap(LO, T1_PORT, REQ_SMALL, sizeof(REQ_SMALL) - 1, 1u << 16);
            c->deadline = DEADLINE_POLLS;
            case_rearm(c);
            return;
        }
        int r = small_roundtrip_result();
        if (r == 0 && --c->deadline > 1) { case_rearm(c); return; }
        if (r != 1) { printf("T1 FAIL: server did not serve the next request (%d)\n", r); case_end(c, 1); return; }
        printf("PASS T1 (server healthy afterwards)\n");
        c->stage = T1_DONE;
        case_end(c, 0);
    }
}

static int run_t1(void) {
    if (t1_make_file() != 0) { printf("T1 FAIL: cannot create %s\n", g_t1_path); return 1; }
    memset(&g_t1, 0, sizeof(g_t1));
    g_t1.poll = t1_poll;
    kl_lwr_guard_reset();
    KlHttpServerConfig cfg = { .port = T1_PORT, .bind_addr = "127.0.0.1", .max_connections = 8,
                               .event_provider = kl_event_provider_lwip_raw() };
    if (kl_http_server_init(&g_t1.srv, &cfg) != 0) { printf("T1 FAIL: server_init\n"); unlink(g_t1_path); return 1; }
    kl_http_server_route(&g_t1.srv, "GET", "/f", t1_handle_file, NULL, NULL);
    kl_http_server_route(&g_t1.srv, "GET", "/s", handle_small, NULL, NULL);
    int rc = case_run(&g_t1, "T1");
    kl_lwr_client_release();
    kl_lwr_guard_reset();
    unlink(g_t1_path);
    return rc;
}

/* ═══════════════════════ T2: reset with a send and a receive posted ═══════════ */
#define T2_PORT 7841
static Case g_t2;
static const char T2_REQ[] =
    "POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 16\r\nExpect: 100-continue\r\n\r\n";

static void t2_handle_up(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_error(res, 200, "ok");   /* never reached: the client resets first */
}

enum { T2_RESET = 0, T2_HEALTHY, T2_DONE };

static void t2_poll(void *ud) {
    Case *c = ud;
    KlHttpServer *s = &c->srv;
    if (c->stage == T2_RESET) {
        if (c->deadline == 0) {
            kl_lwr_rd_start(LO, T2_PORT, T2_REQ, sizeof(T2_REQ) - 1);
            c->deadline = DEADLINE_POLLS;
            case_rearm(c);
            return;
        }
        if (!kl_lwr_rd_done() || active_conns(s) != 0) {
            if (--c->deadline <= 1) {
                char head[256];
                kl_lwr_rd_head(head, sizeof(head));
                printf("T2 FAIL: connection never released after the reset (client done %d, "
                       "reset %d, active %d)\n", kl_lwr_rd_done(), kl_lwr_rd_reset_sent(),
                       active_conns(s));
                case_end(c, 1);
                return;
            }
            case_rearm(c);
            return;
        }
        char head[256];
        kl_lwr_rd_head(head, sizeof(head));
        if (!kl_lwr_rd_reset_sent() || strstr(head, "100 Continue") == NULL) {
            printf("T2 FAIL: precondition: the client did not reset on the 100 Continue "
                   "(reset %d, got \"%.40s\")\n", kl_lwr_rd_reset_sent(), head);
            case_end(c, 1);
            return;
        }
        printf("PASS T2 (reset with the 100 Continue send and the body receive posted: "
               "connection released)\n");
        c->stage = T2_HEALTHY;
        c->deadline = 0;
    }
    if (c->stage == T2_HEALTHY) {
        if (c->deadline == 0) {
            kl_lwr_client_start_cap(LO, T2_PORT, REQ_SMALL, sizeof(REQ_SMALL) - 1, 1u << 16);
            c->deadline = DEADLINE_POLLS;
            case_rearm(c);
            return;
        }
        int r = small_roundtrip_result();
        if (r == 0 && --c->deadline > 1) { case_rearm(c); return; }
        if (r != 1) { printf("T2 FAIL: server did not serve the next request (%d)\n", r); case_end(c, 1); return; }
        printf("PASS T2 (server healthy afterwards)\n");
        c->stage = T2_DONE;
        case_end(c, 0);
    }
}

static int run_t2(void) {
    memset(&g_t2, 0, sizeof(g_t2));
    g_t2.poll = t2_poll;
    KlHttpServerConfig cfg = { .port = T2_PORT, .bind_addr = "127.0.0.1", .max_connections = 8,
                               .event_provider = kl_event_provider_lwip_raw() };
    if (kl_http_server_init(&g_t2.srv, &cfg) != 0) { printf("T2 FAIL: server_init\n"); return 1; }
    kl_http_server_route(&g_t2.srv, "POST", "/up", t2_handle_up, NULL, kl_http_body_reader_buffer);
    kl_http_server_route(&g_t2.srv, "GET", "/s", handle_small, NULL, NULL);
    int rc = case_run(&g_t2, "T2");
    kl_lwr_client_release();
    return rc;
}

/* ═══════════════════════ T3: a dead connection's close after pcb reuse ════════ */
#define T3_PORT 7842
static Case g_t3;
/* Incomplete headers: the server reads them and posts the next receive. */
static const char T3_REQ_PARTIAL[] = "GET /s HTTP/1.1\r\nHost: x\r\n";
static const void *g_t3_pcb_a;
static int g_t3_settle;

enum { T3_OPEN_A = 0, T3_RESET_A_OPEN_B, T3_WAIT_B, T3_NEXT, T3_DONE };

static void t3_poll(void *ud) {
    Case *c = ud;
    KlHttpServer *s = &c->srv;
    switch (c->stage) {
    case T3_OPEN_A:
        kl_lwr_mc_reset();
        if (kl_lwr_mc_start(0, LO, T3_PORT, T3_REQ_PARTIAL, sizeof(T3_REQ_PARTIAL) - 1, 1024) != 0) {
            printf("T3 FAIL: start A\n"); case_end(c, 1); return;
        }
        c->stage = T3_RESET_A_OPEN_B;
        c->deadline = DEADLINE_POLLS;
        g_t3_settle = 0;
        break;
    case T3_RESET_A_OPEN_B:
        /* A accepted and its partial request read (a receive is posted again): a few quiet ticks. */
        if (active_conns(s) != 1 || ++g_t3_settle < 4) {
            if (--c->deadline <= 1) { printf("T3 FAIL: A never accepted\n"); case_end(c, 1); return; }
            break;
        }
        g_t3_pcb_a = kl_lwr_server_pcb_of(T3_PORT, kl_lwr_mc_local_port(0));
        if (!g_t3_pcb_a) { printf("T3 FAIL: no server pcb for A\n"); case_end(c, 1); return; }
        /* Same callback, so the same lwIP tick handles both: A's RST frees A's server pcb, then
         * B's SYN takes it back (LIFO memp) before the driver sees A's failure. */
        kl_lwr_mc_abort(0);
        if (kl_lwr_mc_start(1, LO, T3_PORT, REQ_SMALL, sizeof(REQ_SMALL) - 1, 4096) != 0) {
            printf("T3 FAIL: start B\n"); case_end(c, 1); return;
        }
        c->stage = T3_WAIT_B;
        c->deadline = DEADLINE_POLLS;
        break;
    case T3_WAIT_B:
        if (!kl_lwr_mc_done(1)) {
            if (--c->deadline <= 1) { printf("T3 FAIL: B never resolved\n"); case_end(c, 1); return; }
            break;
        }
        if (kl_lwr_mc_server_pcb(1) != g_t3_pcb_a) {
            printf("T3 FAIL: precondition: B's server pcb %p is not A's freed %p (no reuse; the "
                   "scenario was not exercised)\n", kl_lwr_mc_server_pcb(1), g_t3_pcb_a);
            case_end(c, 1);
            return;
        }
        if (!kl_lwr_mc_ok(1)) {
            printf("T3 FAIL: B (on A's reused pcb) was torn down by A's close: no 200\n");
            case_end(c, 1);
            return;
        }
        size_t chk = 0;
        size_t blen = kl_lwr_mc_body(1, &chk);
        if (blen != SMALL_LEN || chk != pat_checksum_of(SMALL_LEN)) {
            printf("T3 FAIL: B body %zu bytes, checksum mismatch\n", blen);
            case_end(c, 1);
            return;
        }
        printf("PASS T3 (A reset, B on A's reused pcb, A's close left B alone: 200 served)\n");
        if (kl_lwr_mc_start(2, LO, T3_PORT, REQ_SMALL, sizeof(REQ_SMALL) - 1, 4096) != 0) {
            printf("T3 FAIL: start C\n"); case_end(c, 1); return;
        }
        c->stage = T3_NEXT;
        c->deadline = DEADLINE_POLLS;
        break;
    case T3_NEXT:
        if (!kl_lwr_mc_done(2) || active_conns(s) != 0) {
            if (--c->deadline <= 1) {
                printf("T3 FAIL: next connection not served / not released (done %d, ok %d, active %d)\n",
                       kl_lwr_mc_done(2), kl_lwr_mc_ok(2), active_conns(s));
                case_end(c, 1);
                return;
            }
            break;
        }
        if (!kl_lwr_mc_ok(2)) { printf("T3 FAIL: next connection got no 200\n"); case_end(c, 1); return; }
        printf("PASS T3 (next connection served, every connection released)\n");
        c->stage = T3_DONE;
        case_end(c, 0);
        return;
    default:
        return;
    }
    case_rearm(c);
}

static int run_t3(void) {
    memset(&g_t3, 0, sizeof(g_t3));
    g_t3.poll = t3_poll;
    KlHttpServerConfig cfg = { .port = T3_PORT, .bind_addr = "127.0.0.1", .max_connections = 8,
                               .event_provider = kl_event_provider_lwip_raw() };
    if (kl_http_server_init(&g_t3.srv, &cfg) != 0) { printf("T3 FAIL: server_init\n"); return 1; }
    kl_http_server_route(&g_t3.srv, "GET", "/s", handle_small, NULL, NULL);
    int rc = case_run(&g_t3, "T3");
    kl_lwr_mc_reset();
    return rc;
}

int main(void) {
    int fails = 0;
    printf("── teardown tests (lwIP-raw) ──\n");
    fails += run_t1();
    fails += run_t2();
    fails += run_t3();
    if (fails) { printf("raw_teardown_test: %d FAILED\n", fails); return 1; }
    printf("raw_teardown_test: ALL PASS\n");
    return 0;
}
