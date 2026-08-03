/*
 * raw_tick_test.c — Phase 9 lwIP-raw COMPLETION backend tests.
 *
 * P9-1 (retained): an lwIP-raw KlEventCtx ticks without a server — inits a ctx on the
 * lwIP-raw completion backend, asserts the loop's caps + socket provider are compatible,
 * drives it ~20 times via kl_event_ctx_run (each run = one lwIP mainloop tick), and frees
 * cleanly. Prints "P9-1 PASS".
 *
 * P9-2 (this stage): a RAW-BACKED KlServer answers GET / over loopback. A real KlServer is
 * pinned to the lwIP-raw completion backend + its overlapped socket provider; the connection
 * state machine runs over the tcp_accept/tcp_recv/tcp_sent callbacks surfaced as
 * KL_COMP_ACCEPT/READ/WRITE. A raw-API test client (in the glue, like raw_loopback_spike.c)
 * connects to 127.0.0.1:PORT, writes "GET / HTTP/1.1...", and captures the response.
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
#include <stdio.h>
#include <string.h>
#include <time.h>

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
    printf("P9-2 PASS\n");
    return 0;
}

int main(void) {
    if (run_p9_1() != 0) return 1;
    if (run_p9_2() != 0) return 1;
    return 0;
}
