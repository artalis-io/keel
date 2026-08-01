/*
 * lwip_loopback_test.c — a full Keel HTTP server running on the lwIP TCP/IP stack
 * (no kernel sockets), proving the socket + event providers end to end.
 *
 * Brings up lwIP (tcpip_init + loopback netif), starts a KlServer configured with
 * kl_socket_provider_lwip() + kl_event_provider_lwip(), then connects an lwIP
 * client to it over 127.0.0.1 and does an HTTP/1.1 GET. The handler runs and
 * returns 200 — driven entirely by lwIP sockets + lwip_poll, with no core
 * recompile. Exit 0 on success.
 */
#include <keel/keel.h>
#include "keel_lwip.h"

#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LWT_PORT 8080

static sys_sem_t g_ready;
static void tcpip_done(void *a) { (void)a; sys_sem_signal(&g_ready); }

static void handler(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_response_json(res, 200, "{\"stack\":\"lwip\"}", 16);
}

static KlServer g_srv;
static void *srv_thread(void *a) { (void)a; kl_server_run(&g_srv); return NULL; }

int main(void) {
    /* Bring up lwIP: tcpip thread + loopback netif (LWIP_HAVE_LOOPIF → 127.0.0.1). */
    sys_sem_new(&g_ready, 0);
    tcpip_init(tcpip_done, NULL);
    sys_sem_wait(&g_ready);
    usleep(100000);

    /* Keel server on the lwIP providers. */
    KlConfig cfg = {
        .port = LWT_PORT, .bind_addr = "127.0.0.1",
        .sockets        = kl_socket_provider_lwip(),
        .event_provider = kl_event_provider_lwip(),
    };
    if (kl_server_init(&g_srv, &cfg) < 0) { fprintf(stderr, "server init failed\n"); return 1; }
    kl_server_route(&g_srv, "GET", "/", handler, NULL, NULL);

    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_thread, NULL) != 0) { fprintf(stderr, "thread\n"); return 1; }
    for (int i = 0; i < 400 && g_srv.bound_port == 0; i++) usleep(5000);
    if (g_srv.bound_port <= 0) { fprintf(stderr, "server never bound\n"); return 1; }

    /* lwIP client → the Keel server, over lwIP loopback. */
    int c = lwip_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in in; memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    in.sin_port = lwip_htons((uint16_t)g_srv.bound_port);
    in.sin_addr.s_addr = lwip_htonl(INADDR_LOOPBACK);
    if (lwip_connect(c, (struct sockaddr *)&in, sizeof in) != 0) { fprintf(stderr, "connect failed\n"); return 1; }

    const char *req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    lwip_send(c, req, strlen(req), 0);
    char buf[512]; int got = 0, n;
    while (got < (int)sizeof(buf) - 1 && (n = lwip_recv(c, buf + got, sizeof(buf) - 1 - got, 0)) > 0) {
        got += n;
        if (strstr(buf, "\r\n\r\n")) { buf[got] = 0; if (strstr(buf, "\"stack\":\"lwip\"")) break; }
    }
    buf[got > 0 ? got : 0] = 0;
    lwip_close(c);

    int ok = (strstr(buf, "200 OK") != NULL) && (strstr(buf, "\"stack\":\"lwip\"") != NULL);
    printf("lwIP loopback: Keel server on lwIP replied %s\n", ok ? "200 OK (correct)" : "UNEXPECTED");

    kl_server_stop(&g_srv);          /* loop wakes on its poll timeout, sees stop */
    pthread_join(tid, NULL);
    kl_server_free(&g_srv);
    return ok ? 0 : 2;
}
