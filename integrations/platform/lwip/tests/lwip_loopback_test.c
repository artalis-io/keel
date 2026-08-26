/*
 * lwip_loopback_test.c: a full Keel HTTP server running on the lwIP TCP/IP stack
 * (no kernel sockets), proving the socket + event providers end to end.
 *
 * Brings up lwIP (tcpip_init + loopback netif), starts a KlHttpServer configured with
 * kl_socket_provider_lwip() + kl_event_provider_lwip(), then:
 *   (1) a raw lwIP client GETs it (proves the server axis), and
 *   (2) a Keel ASYNC CLIENT on the lwIP providers GETs it (proves the outbound
 *       client axis: connect + blocking name resolution via resolve_sync_lwip).
 * Both driven entirely by lwIP sockets + lwip_poll, linked against a STOCK
 * libkeel: no core recompile. Exit 0 on success.
 */
#include <keel/keel.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>   /* complete KlDatagram type: the echo server embeds one */
#include "keel_lwip.h"
#ifdef LWT_TLS
#include <keel_tls_mbedtls.h>   /* HTTPS over lwIP (mbedTLS BIO → lwIP provider) */
#endif

#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LWT_PORT      8080
#define LWT_UDP_PORT  8081
#define LWT_TLS_PORT  8443

static sys_sem_t g_ready;
static void tcpip_done(void *a) { (void)a; sys_sem_signal(&g_ready); }

static void handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{\"stack\":\"lwip\"}", 16);
}

static KlHttpServer g_srv;
static void *srv_thread(void *a) { (void)a; kl_http_server_run(&g_srv); return NULL; }

static volatile int g_cli_done;
static void cli_done(KlHttpClient *c, void *ud) { (void)c; (void)ud; g_cli_done = 1; }

/* A Keel async HTTP client on the lwIP providers → the server.
 * Proves the outbound axis (connect + resolve_sync_lwip name resolution). */
static int keel_client_on_lwip(uint16_t port) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx cev;
    if (kl_event_ctx_init_ex(&cev, &alloc, kl_event_provider_lwip()) != 0)
        return 0;
    cev.sockets = kl_socket_provider_lwip();

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", (unsigned)port);
    KlHttpClientConfig ccfg = {
        .sockets    = kl_socket_provider_lwip(),
        .system_dns = 1,        /* blocking resolve → resolve_sync_lwip (lwip_getaddrinfo) */
        .timeout_ms = 5000,
    };
    g_cli_done = 0;
    KlHttpClient *cli = kl_http_client_start(&cev, &alloc, &ccfg, "GET", url,
                                    NULL, 0, NULL, 0, cli_done, NULL);
    int ok = 0;
    if (cli) {
        for (int i = 0; i < 500 && !g_cli_done; i++)
            kl_event_ctx_run(&cev, 16, 20);
        if (g_cli_done && kl_http_client_error(cli) == 0) {
            const KlHttpClientResponse *r = kl_http_client_response(cli);
            ok = (r && r->status == 200 && r->body &&
                  strstr(r->body, "\"stack\":\"lwip\"") != NULL);
        }
        kl_http_client_free(cli);
    }
    kl_event_ctx_free(&cev);
    return ok;
}

/* A Keel KlDatagram echo server on the lwIP providers, exercised by a raw lwIP
 * UDP client. Proves the datagram axis (socket_lwip's KlDatagramOps: recv/send):
 * the foundation for udp_server and the built-in async DNS resolver on lwIP. */
static void udp_echo(void *ud, const void *data, size_t len,
                     const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    (void)local; (void)flags;
    KlDatagram *echo = ud;
    if (peer)   /* bounce it back */
        (void)kl_datagram_send(echo, &(KlDatagramMessage){ .data = data, .len = len, .peer = peer, .tos = -1 });
}

/* Public close lifecycle over the lwIP readiness loop: abortive close, pump until CLOSED, free. */
static void dg_close(KlEventCtx *ctx, KlDatagram *dg) {
    if (kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED)
        (void)kl_datagram_close_cancel(dg);
    for (int i = 0; i < 300 && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 5);
    kl_datagram_free(dg);
}

static int keel_udp_on_lwip(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx uev;
    if (kl_event_ctx_init_ex(&uev, &alloc, kl_event_provider_lwip()) != 0)
        return 0;
    uev.sockets = kl_socket_provider_lwip();

    KlDatagram echo;
    KlDatagramSocketConfig ucfg = {
        .ctx = &uev, .family = AF_INET,
        .bind_addr = "127.0.0.1", .bind_port = LWT_UDP_PORT,
        .alloc = &alloc,
    };
    if (kl_datagram_socket_init(&echo, &ucfg) != 0) { kl_event_ctx_free(&uev); return 0; }
    if (kl_datagram_recv_start(&echo, udp_echo, &echo) != 0) {
        dg_close(&uev, &echo); kl_event_ctx_free(&uev); return 0;
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
    dg_close(&uev, &echo);
    kl_event_ctx_free(&uev);
    return ok;
}

#ifdef LWT_TLS
/* HTTPS on lwIP. A Keel TLS server (mbedTLS) + a Keel async TLS client,
 * both on the lwIP providers, with the mbedTLS socket-BIO routed through the lwIP
 * socket provider: which the framework auto-wires from KlHttpServerConfig.sockets /
 * KlHttpClientConfig.sockets via the KlTls.set_socket_provider hook (no explicit
 * per-ctx call needed): so a genuine TLS handshake + request runs over lwIP with
 * zero lwIP-specific TLS code. Embedded
 * self-signed EC cert (CN=127.0.0.1); the client skips CA verification. Certs are
 * the same test-only material as integrations/tls/mbedtls/tests/smoke_tls.c. */
static const char CERT_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIBmjCCAUGgAwIBAgIUOugIDeF4cZCY5f4MN+sk1xFwncIwCgYIKoZIzj0EAwIw\n"
"FDESMBAGA1UEAwwJMTI3LjAuMC4xMCAXDTI2MDcyNjA5MDEwMVoYDzIxMjYwNzAy\n"
"MDkwMTAxWjAUMRIwEAYDVQQDDAkxMjcuMC4wLjEwWTATBgcqhkjOPQIBBggqhkjO\n"
"PQMBBwNCAAQaZEdp4OpJZgbYIuvQiCDZE86uRuj+HQSP88xCcQ17ZSg3dWoDMRGH\n"
"DXznyJNlQ0vtbNr9Wcg4+/DAC/SLNu7Io28wbTAdBgNVHQ4EFgQUUW6CpMp5FGyE\n"
"eHRZT3D2XQUkJwowHwYDVR0jBBgwFoAUUW6CpMp5FGyEeHRZT3D2XQUkJwowDwYD\n"
"VR0TAQH/BAUwAwEB/zAaBgNVHREEEzARhwR/AAABgglsb2NhbGhvc3QwCgYIKoZI\n"
"zj0EAwIDRwAwRAIga5AdkBoyr0QJLwDzXXBKl/S4T7aplF32UlZDC3bkuusCIBNM\n"
"md2wiK5Cszs6q1wjkLLKoGtsrtjkNcFnKFdAw+zB\n"
"-----END CERTIFICATE-----\n";
static const char KEY_PEM[] =
"-----BEGIN PRIVATE KEY-----\n"
"MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgZKkZoDmfdcTJNMty\n"
"gSicJ0RHBVOrlx0ISej/z1cGczKhRANCAAQaZEdp4OpJZgbYIuvQiCDZE86uRuj+\n"
"HQSP88xCcQ17ZSg3dWoDMRGHDXznyJNlQ0vtbNr9Wcg4+/DAC/SLNu7I\n"
"-----END PRIVATE KEY-----\n";

static void https_handler(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{\"tls\":\"lwip\"}", 15);
}
static KlHttpServer g_tls_srv;
static void *tls_srv_thread(void *a) { (void)a; kl_http_server_run(&g_tls_srv); return NULL; }

static int keel_https_on_lwip(void) {
    KlAllocator alloc = kl_allocator_default();

    /* HTTPS server on the lwIP providers; mbedTLS BIO routed through lwIP. */
    KlTlsCtx *sctx = kl_tls_mbedtls_ctx_create_from_buf(
        (const unsigned char *)CERT_PEM, sizeof(CERT_PEM),
        (const unsigned char *)KEY_PEM,  sizeof(KEY_PEM),
        NULL, 0, KL_MTLS_NONE, &alloc);
    if (!sctx) return 0;
    /* No explicit provider call: the server auto-wires it from KlHttpServerConfig.sockets. */
    KlTlsConfig stls = { .ctx = sctx, .factory = kl_tls_mbedtls_create };  /* destroy manually */
    KlHttpServerConfig cfg = {
        .port = LWT_TLS_PORT, .bind_addr = "127.0.0.1",
        .sockets = kl_socket_provider_lwip(), .event_provider = kl_event_provider_lwip(),
        .tls = &stls,
    };
    if (kl_http_server_init(&g_tls_srv, &cfg) < 0) { kl_tls_mbedtls_ctx_destroy(sctx); return 0; }
    kl_http_server_route(&g_tls_srv, "GET", "/", https_handler, NULL, NULL);
    pthread_t tid;
    if (pthread_create(&tid, NULL, tls_srv_thread, NULL) != 0) {
        kl_http_server_free(&g_tls_srv); kl_tls_mbedtls_ctx_destroy(sctx); return 0;
    }
    for (int i = 0; i < 400 && g_tls_srv.bound_port == 0; i++) usleep(5000);

    /* Keel async HTTPS client on the lwIP providers; mbedTLS BIO routed through lwIP. */
    KlEventCtx cev;
    int ok = 0;
    if (kl_event_ctx_init_ex(&cev, &alloc, kl_event_provider_lwip()) == 0) {
        cev.sockets = kl_socket_provider_lwip();
        KlTlsCtx *cctx = kl_tls_mbedtls_client_ctx_create(NULL, &alloc);  /* NULL CA = skip verify */
        if (cctx) {
            /* No explicit provider call: the client auto-wires it from KlHttpClientConfig.sockets. */
            KlTlsConfig ctls = { .ctx = cctx, .factory = kl_tls_mbedtls_create };
            char url[64];
            snprintf(url, sizeof(url), "https://127.0.0.1:%u/", (unsigned)g_tls_srv.bound_port);
            KlHttpClientConfig ccfg = {
                .sockets = kl_socket_provider_lwip(), .system_dns = 1,
                .timeout_ms = 5000, .tls = &ctls,
            };
            g_cli_done = 0;
            KlHttpClient *cli = kl_http_client_start(&cev, &alloc, &ccfg, "GET", url,
                                            NULL, 0, NULL, 0, cli_done, NULL);
            if (cli) {
                for (int i = 0; i < 500 && !g_cli_done; i++)
                    kl_event_ctx_run(&cev, 16, 20);
                if (g_cli_done && kl_http_client_error(cli) == 0) {
                    const KlHttpClientResponse *r = kl_http_client_response(cli);
                    ok = (r && r->status == 200 && r->body &&
                          strstr(r->body, "\"tls\":\"lwip\"") != NULL);
                }
                kl_http_client_free(cli);
            }
            kl_tls_mbedtls_ctx_destroy(cctx);
        }
        kl_event_ctx_free(&cev);
    }

    kl_http_server_stop(&g_tls_srv);
    pthread_join(tid, NULL);
    kl_http_server_free(&g_tls_srv);
    kl_tls_mbedtls_ctx_destroy(sctx);
    return ok;
}
#endif /* LWT_TLS */

int main(void) {
    /* Bring up lwIP: tcpip thread + loopback netif (LWIP_HAVE_LOOPIF → 127.0.0.1). */
    sys_sem_new(&g_ready, 0);
    tcpip_init(tcpip_done, NULL);
    sys_sem_wait(&g_ready);
    usleep(100000);

    /* Keel server on the lwIP providers. */
    KlHttpServerConfig cfg = {
        .port = LWT_PORT, .bind_addr = "127.0.0.1",
        .sockets        = kl_socket_provider_lwip(),
        .event_provider = kl_event_provider_lwip(),
    };
    if (kl_http_server_init(&g_srv, &cfg) < 0) { fprintf(stderr, "server init failed\n"); return 1; }
    kl_http_server_route(&g_srv, "GET", "/", handler, NULL, NULL);

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

    /* A Keel async client on lwIP → the same server. */
    int client_ok = keel_client_on_lwip((uint16_t)g_srv.bound_port);
    printf("lwIP loopback: Keel client on lwIP got %s\n",
           client_ok ? "200 (correct)" : "UNEXPECTED");

    /* A Keel KlDatagram echo on lwIP, exercised by a raw lwIP UDP client. */
    int udp_ok = keel_udp_on_lwip();
    printf("lwIP loopback: Keel UDP echo on lwIP %s\n",
           udp_ok ? "round-tripped (correct)" : "UNEXPECTED");

    /* Optional (LWT_TLS): HTTPS on lwIP: Keel TLS server + client, mbedTLS
     * BIO routed through the lwIP socket provider. */
    int tls_ok = 1;
#ifdef LWT_TLS
    tls_ok = keel_https_on_lwip();
    printf("lwIP loopback: Keel HTTPS (mbedTLS) on lwIP %s\n",
           tls_ok ? "handshake + roundtrip OK (correct)" : "UNEXPECTED");
#endif

    kl_http_server_stop(&g_srv);          /* loop wakes on its poll timeout, sees stop */
    pthread_join(tid, NULL);
    kl_http_server_free(&g_srv);
    return (ok && client_ok && udp_ok && tls_ok) ? 0 : 2;
}
