/*
 * raw_drain_test.c: graceful drain rejects autonomous lwIP-raw accepts.
 *
 * lwIP raw is unlike the post-driven completion backends: tcp_accept remains armed and an
 * incoming connection surfaces autonomously.  Hold one keep-alive connection open so graceful
 * drain cannot finish immediately, begin the drain, then connect a second client.  The first
 * request must have been served; the connection accepted after drain began must receive no HTTP
 * response.  This directly covers completion_http_server.c's autonomous-accept drain gate.
 *
 * Every lwIP raw call runs on the server/tick thread through timers.  The main thread only waits.
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/timer.h>

#include "keel_lwip_raw.h"
#include "lwip_raw_testclient.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DRAIN_PORT 7791
#define DRAIN_BODY "drain-ok"
#define DRAIN_REQ_KEEP "GET / HTTP/1.1\r\nHost: x\r\n\r\n"
#define DRAIN_REQ_LATE "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"

static const uint8_t g_loopback[4] = {127, 0, 0, 1};
static KlHttpServer g_server;
static atomic_int g_phase;
static atomic_int g_failed;
static atomic_int g_first_served;
static atomic_int g_late_started;

static void handle_root(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req;
    (void)ud;
    kl_http_response_json(res, 200, DRAIN_BODY, sizeof(DRAIN_BODY) - 1);
}

static void start_first(void *ud) {
    (void)ud;
    if (kl_lwr_mc_start(0, g_loopback, DRAIN_PORT, DRAIN_REQ_KEEP,
                        sizeof(DRAIN_REQ_KEEP) - 1, 1024) < 0)
        atomic_store(&g_failed, 1);
}

static void poll_clients(void *ud) {
    KlHttpServer *server = ud;

    if (atomic_load(&g_failed)) {
        kl_http_server_stop(server);
        return;
    }

    if (atomic_load(&g_phase) == 0) {
        size_t body_len = kl_lwr_mc_body(0, NULL);
        if (kl_lwr_mc_ok(0) && body_len == sizeof(DRAIN_BODY) - 1) {
            atomic_store(&g_first_served, 1);
            kl_http_server_stop(server); /* enters graceful drain; first connection stays alive */
            atomic_store(&g_phase, 1);
            if (kl_lwr_mc_start(1, g_loopback, DRAIN_PORT, DRAIN_REQ_LATE,
                                sizeof(DRAIN_REQ_LATE) - 1, 1024) < 0)
                atomic_store(&g_failed, 1);
            else
                atomic_store(&g_late_started, 1);
        }
    }

    /* Keep observing until the server's bounded graceful-drain deadline stops the loop. */
    if (atomic_load(&g_phase) < 1)
        kl_timer_add(&server->ev, 5, poll_clients, server);
}

static void *server_thread(void *ud) {
    KlHttpServer *server = ud;
    kl_timer_add(&server->ev, 20, start_first, NULL);
    kl_timer_add(&server->ev, 25, poll_clients, server);
    kl_http_server_run(server);
    return NULL;
}

int main(void) {
    KlHttpServerConfig cfg = {
        .port = DRAIN_PORT,
        .bind_addr = "127.0.0.1",
        .drain_timeout_ms = 500,
        .event_provider = kl_event_provider_lwip_raw(),
    };
    if (kl_http_server_init(&g_server, &cfg) != 0) {
        printf("DRAIN FAIL: kl_http_server_init (err=%d)\n", g_server.last_error);
        return 1;
    }
    if (kl_http_server_route(&g_server, "GET", "/", handle_root, NULL, NULL) != 0) {
        printf("DRAIN FAIL: route registration\n");
        kl_http_server_free(&g_server);
        return 1;
    }

    atomic_store(&g_phase, 0);
    atomic_store(&g_failed, 0);
    atomic_store(&g_first_served, 0);
    atomic_store(&g_late_started, 0);
    kl_lwr_mc_reset();

    pthread_t thread;
    if (pthread_create(&thread, NULL, server_thread, &g_server) != 0) {
        printf("DRAIN FAIL: pthread_create\n");
        kl_http_server_free(&g_server);
        return 1;
    }

    /* The server exits at the 500 ms drain deadline; allow ample scheduling headroom. */
    for (int i = 0; i < 300 && atomic_load(&g_phase) < 1 && !atomic_load(&g_failed); i++) {
        struct timespec delay = {0, 10 * 1000000L};
        nanosleep(&delay, NULL);
    }
    if (atomic_load(&g_phase) < 1) {
        atomic_store(&g_failed, 1);
        kl_http_server_stop(&g_server);
    }
    pthread_join(thread, NULL);

    int first_served = atomic_load(&g_first_served);
    int late_started = atomic_load(&g_late_started);
    int late_served = kl_lwr_mc_ok(1);
    kl_lwr_mc_reset();
    kl_http_server_free(&g_server);

    if (atomic_load(&g_failed) || !first_served || !late_started || late_served) {
        printf("DRAIN FAIL: failed=%d first_served=%d late_started=%d late_served=%d\n",
               atomic_load(&g_failed), first_served, late_started, late_served);
        return 1;
    }
    printf("DRAIN PASS (autonomous accept was not served after graceful drain began)\n");
    return 0;
}
