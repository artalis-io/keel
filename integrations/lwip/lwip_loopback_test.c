/*
 * lwip_loopback_test.c — a full Keel HTTP server running on the lwIP TCP/IP stack
 * (no kernel sockets), proving the socket + event providers end to end.
 *
 * Brings up lwIP (tcpip_init + loopback netif), starts a KlServer configured with
 * kl_socket_provider_lwip() + kl_event_provider_lwip(), then:
 *   (1) a raw lwIP client GETs it (proves the server axis), and
 *   (2) a Keel ASYNC CLIENT on the lwIP providers GETs it (proves the outbound
 *       client axis: connect + blocking name resolution via resolve_sync_lwip).
 * Both driven entirely by lwIP sockets + lwip_poll, linked against a STOCK
 * libkeel — no core recompile. Exit 0 on success.
 */
#include <keel/keel.h>
#include <keel/udp.h>
#include "keel_lwip.h"

#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LWT_PORT      8080
#define LWT_UDP_PORT  8081

static sys_sem_t g_ready;
static void tcpip_done(void *a) { (void)a; sys_sem_signal(&g_ready); }

static void handler(KlRequest *req, KlResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_response_json(res, 200, "{\"stack\":\"lwip\"}", 16);
}

static KlServer g_srv;
static void *srv_thread(void *a) { (void)a; kl_server_run(&g_srv); return NULL; }

static volatile int g_cli_done;
static void cli_done(KlClient *c, void *ud) { (void)c; (void)ud; g_cli_done = 1; }

/* Phase 2: a Keel async HTTP client on the lwIP providers → the server.
 * Proves the outbound axis (connect + resolve_sync_lwip name resolution). */
static int keel_client_on_lwip(uint16_t port) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx cev;
    if (kl_event_ctx_init_ex(&cev, &alloc, kl_event_provider_lwip()) != 0)
        return 0;
    cev.sockets = kl_socket_provider_lwip();

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", (unsigned)port);
    KlClientConfig ccfg = {
        .sockets    = kl_socket_provider_lwip(),
        .system_dns = 1,        /* blocking resolve → resolve_sync_lwip (lwip_getaddrinfo) */
        .timeout_ms = 5000,
    };
    g_cli_done = 0;
    KlClient *cli = kl_client_start(&cev, &alloc, &ccfg, "GET", url,
                                    NULL, 0, NULL, 0, cli_done, NULL);
    int ok = 0;
    if (cli) {
        for (int i = 0; i < 500 && !g_cli_done; i++)
            kl_event_ctx_run(&cev, 16, 20);
        if (g_cli_done && kl_client_error(cli) == 0) {
            const KlClientResponse *r = kl_client_response(cli);
            ok = (r && r->status == 200 && r->body &&
                  strstr(r->body, "\"stack\":\"lwip\"") != NULL);
        }
        kl_client_free(cli);
    }
    kl_event_ctx_free(&cev);
    return ok;
}

/* Phase 3: a Keel KlUdp echo server on the lwIP providers, exercised by a raw lwIP
 * UDP client. Proves the datagram axis (udp_io_lwip: recv_drain + raw_send) — the
 * foundation for udp_server and the built-in async DNS resolver on lwIP. */
static void udp_echo(KlUdp *udp, const void *data, size_t len,
                     const KlSockAddr *src, const KlSockAddr *local, void *ud) {
    (void)local; (void)ud;
    if (src) (void)kl_udp_send_to(udp, data, len, src);   /* bounce it back */
}

static int keel_udp_on_lwip(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx uev;
    if (kl_event_ctx_init_ex(&uev, &alloc, kl_event_provider_lwip()) != 0)
        return 0;
    uev.sockets = kl_socket_provider_lwip();

    KlUdp echo;
    KlUdpConfig ucfg = {
        .ctx = &uev, .family = AF_INET,
        .bind_addr = "127.0.0.1", .bind_port = LWT_UDP_PORT,
        .alloc = &alloc,
    };
    if (kl_udp_init(&echo, &ucfg) != 0) { kl_event_ctx_free(&uev); return 0; }
    if (kl_udp_recv_start(&echo, udp_echo, NULL) != 0) {
        kl_udp_free(&echo); kl_event_ctx_free(&uev); return 0;
    }

    /* Raw lwIP UDP client: send a datagram, expect it echoed back. */
    int c = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = lwip_htons(LWT_UDP_PORT);
    to.sin_addr.s_addr = lwip_htonl(INADDR_LOOPBACK);
    const char *msg = "ping-lwip";
    lwip_sendto(c, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof to);

    /* Make the client non-blocking so we can interleave draining the echo ctx. */
    lwip_fcntl(c, F_SETFL, O_NONBLOCK);

    int ok = 0;
    char buf[64];
    for (int i = 0; i < 500 && !ok; i++) {
        kl_event_ctx_run(&uev, 16, 20);       /* echo: recv the ping, send it back */
        int n = lwip_recvfrom(c, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n > 0) {
            buf[n] = 0;
            ok = (strcmp(buf, msg) == 0);
        }
    }
    lwip_close(c);
    kl_udp_free(&echo);
    kl_event_ctx_free(&uev);
    return ok;
}

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
    printf("lwIP loopback: raw client -> Keel server replied %s\n",
           ok ? "200 OK (correct)" : "UNEXPECTED");

    /* Phase 2: a Keel async client on lwIP → the same server. */
    int client_ok = keel_client_on_lwip((uint16_t)g_srv.bound_port);
    printf("lwIP loopback: Keel client on lwIP got %s\n",
           client_ok ? "200 (correct)" : "UNEXPECTED");

    /* Phase 3: a Keel KlUdp echo on lwIP, exercised by a raw lwIP UDP client. */
    int udp_ok = keel_udp_on_lwip();
    printf("lwIP loopback: Keel UDP echo on lwIP %s\n",
           udp_ok ? "round-tripped (correct)" : "UNEXPECTED");

    kl_server_stop(&g_srv);          /* loop wakes on its poll timeout, sees stop */
    pthread_join(tid, NULL);
    kl_server_free(&g_srv);
    return (ok && client_ok && udp_ok) ? 0 : 2;
}
