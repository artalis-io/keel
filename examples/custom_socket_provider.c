/*
 * custom_socket_provider.c — bring-your-own socket stack (PAL Phase 4).
 *
 * Concepts: the KlSocketProvider / KlSocketOps vtable, the KL_SOCK_CAP_* flags,
 * and selecting a provider via KlConfig.sockets (server) + KlClientConfig.sockets
 * (client). This provider is a *decorator*: it wraps the built-in provider
 * (kl_socket_provider_posix) and counts sockets + bytes, forwarding each op it
 * intercepts to the wrapped provider. Ops it does not implement are left NULL —
 * Keel falls back to its built-in default for those. Uses only installed public
 * headers (no internal/POSIX types: KlIoVec, kl_ssize_t, KlSocketHandle).
 *
 * Build:  make examples
 * Run:    ./examples/custom_socket_provider
 */

#include <keel/keel.h>
#include <keel/client.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── A counting decorator over a wrapped provider ──────────────────── */

typedef struct {
    const KlSocketProvider *base;   /* the wrapped built-in provider */
    int  sockets, accepts;
    long tx_bytes, rx_bytes;
} CountingCtx;

static KlSocketHandle c_socket(void *ctx, int domain, int type, int protocol) {
    CountingCtx *c = ctx;
    c->sockets++;
    return c->base->ops->socket(c->base->context, domain, type, protocol);
}
static KlSocketHandle c_accept(void *ctx, KlSocketHandle fd, KlSockAddr *peer) {
    CountingCtx *c = ctx;
    c->accepts++;
    return c->base->ops->accept(c->base->context, fd, peer);
}
static kl_ssize_t c_send(void *ctx, KlSocketHandle fd, const void *buf, size_t len) {
    CountingCtx *c = ctx;
    kl_ssize_t r = c->base->ops->send(c->base->context, fd, buf, len);
    if (r > 0) c->tx_bytes += r;
    return r;
}
static kl_ssize_t c_recv(void *ctx, KlSocketHandle fd, void *buf, size_t len) {
    CountingCtx *c = ctx;
    kl_ssize_t r = c->base->ops->recv(c->base->context, fd, buf, len);
    if (r > 0) c->rx_bytes += r;
    return r;
}

/* Only intercept socket/accept/send/recv; the rest are NULL → Keel uses its
 * built-in default (identical to the wrapped provider) for them. */
static const KlSocketOps counting_ops = {
    .socket = c_socket, .accept = c_accept, .send = c_send, .recv = c_recv,
    .name = "counting",
};

/* ── A tiny server to exercise the provider ────────────────────────── */

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *u) {
    (void)req; (void)u;
    static const char body[] = "{\"msg\":\"hello via decorated provider\"}";
    kl_http_response_json(res, 200, body, sizeof(body) - 1);
}
static void *run_server(void *arg) { kl_server_run((KlServer *)arg); return NULL; }

#define DEMO_PORT 18099

int main(void) {
    printf("custom_socket_provider example\n\n");

    /* Server through a counting decorator over the built-in provider. It must
     * advertise KL_SOCK_CAP_NATIVE_FD — the readiness event loop polls real fds. */
    CountingCtx srv = { .base = kl_socket_provider_posix(), 0, 0, 0, 0 };
    KlSocketProvider srv_prov = { &counting_ops, &srv, KL_SOCK_CAP_NATIVE_FD, NULL };

    KlServer s;
    KlConfig cfg = { .port = DEMO_PORT, .bind_addr = "127.0.0.1", .sockets = &srv_prov };
    if (kl_server_init(&s, &cfg) < 0) {
        fprintf(stderr, "server init failed\n");
        return 1;
    }
    kl_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, run_server, &s) != 0) {
        kl_server_free(&s);
        return 1;
    }

    /* Client through its OWN counting decorator, selected via KlClientConfig. */
    CountingCtx cli = { .base = kl_socket_provider_posix(), 0, 0, 0, 0 };
    KlSocketProvider cli_prov = { &counting_ops, &cli, KL_SOCK_CAP_NATIVE_FD, NULL };
    KlAllocator alloc = kl_allocator_default();

    int ok = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        struct timespec ts = { 0, 30 * 1000000L };
        nanosleep(&ts, NULL);
        KlClientConfig ccfg = { .timeout_ms = 2000, .sockets = &cli_prov };
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        if (kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18099/hello",
                              NULL, 0, NULL, 0, &resp) == 0) {
            if (resp.status == 200) {
                ok = 1;
                printf("--- response ---\n  status: %d\n  body:   %.*s\n\n",
                       resp.status, (int)resp.body_len, resp.body);
            }
            kl_client_response_free(&resp);
        }
    }

    kl_server_stop(&s);
    pthread_join(th, NULL);
    kl_server_free(&s);

    printf("--- server provider stats ---\n");
    printf("  sockets created: %d\n  accepts:         %d\n", srv.sockets, srv.accepts);
    printf("  bytes sent/recv: %ld / %ld\n\n", srv.tx_bytes, srv.rx_bytes);
    printf("--- client provider stats ---\n");
    printf("  sockets created: %d\n", cli.sockets);
    printf("  bytes sent/recv: %ld / %ld\n\n", cli.tx_bytes, cli.rx_bytes);

    if (!ok) {
        fprintf(stderr, "request failed\n");
        return 1;
    }
    printf("both server and client drove the decorated provider.\n");
    return 0;
}
