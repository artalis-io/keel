#include "utest.h"
#include <keel/udp_server.h>
#include <keel/udp.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <string.h>
#include "net_compat.h"

/* ── Server side: echo handler ───────────────────────────────────────── */

static int g_srv_hits;
static char g_srv_src_ip[INET6_ADDRSTRLEN];

static void echo_handler(KlUdpServer *s, const void *data, size_t len,
                         const KlSockAddr *src, void *ud) {
    (void)ud;
    g_srv_hits++;
    g_srv_src_ip[0] = '\0';
    if (src && kl_sockaddr_family(src) == KL_AF_INET)
        inet_ntop(AF_INET, src->u.ip,
                  g_srv_src_ip, sizeof(g_srv_src_ip));
    kl_udp_server_reply(s, data, len, src);
}

/* ── Client side: capture ────────────────────────────────────────────── */

static char   g_cli_buf[512];
static size_t g_cli_len;
static int    g_cli_got;
static char   g_cli_src_ip[INET6_ADDRSTRLEN];

static void cli_recv(KlUdp *u, const void *data, size_t len,
                     const KlSockAddr *src, const KlSockAddr *local, void *ud) {
    (void)u; (void)local; (void)ud;
    if (len > sizeof(g_cli_buf)) len = sizeof(g_cli_buf);
    memcpy(g_cli_buf, data, len);
    g_cli_len = len;
    g_cli_src_ip[0] = '\0';
    if (src && kl_sockaddr_family(src) == KL_AF_INET)
        inet_ntop(AF_INET, src->u.ip,
                  g_cli_src_ip, sizeof(g_cli_src_ip));
    g_cli_got++;
}

static void reset(void) {
    g_srv_hits = 0; g_srv_src_ip[0] = '\0';
    g_cli_len = 0; g_cli_got = 0; memset(g_cli_buf, 0, sizeof(g_cli_buf));
    g_cli_src_ip[0] = '\0';
}

static void pump_until(KlEventCtx *ctx, int *flag, int want, int ticks) {
    for (int i = 0; i < ticks && *flag < want; i++)
        kl_event_ctx_run(ctx, 16, 10);
}

static void dest_v4(KlSockAddr *a, uint16_t port) {
    unsigned char _ipb[4]; inet_pton(AF_INET, "127.0.0.1", _ipb); kl_sockaddr_from_ipv4(a, _ipb, port);
}

/* ── Tests ───────────────────────────────────────────────────────────── */

UTEST(udp_server, echo_roundtrip) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    uint16_t port = kl_udp_server_local_port(&srv);
    ASSERT_TRUE(port > 0);

    KlUdp cli;
    KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, cli_recv, NULL));

    KlSockAddr dst;
    dest_v4(&dst, port);
    const char *msg = "discover";
    ASSERT_EQ(0, kl_udp_send_to(&cli, msg, strlen(msg),
                                &dst));

    /* client -> server (handler) -> reply -> client */
    pump_until(&ctx, &g_cli_got, 1, 300);

    ASSERT_EQ(1, g_srv_hits);
    ASSERT_STREQ("127.0.0.1", g_srv_src_ip);   /* server saw the client's addr */
    ASSERT_EQ(1, g_cli_got);
    ASSERT_EQ(strlen(msg), g_cli_len);
    ASSERT_EQ(0, memcmp(g_cli_buf, msg, g_cli_len));

    kl_udp_free(&cli);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

UTEST(udp_server, shared_loop_two_servers) {
    /* Two UDP servers on one event context — proves the shared-loop model. */
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer a, b;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    ASSERT_EQ(0, kl_udp_server_init(&a, &ctx, &sc, echo_handler, NULL));
    ASSERT_EQ(0, kl_udp_server_init(&b, &ctx, &sc, echo_handler, NULL));
    ASSERT_TRUE(kl_udp_server_local_port(&a) != kl_udp_server_local_port(&b));

    KlUdp cli;
    KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, cli_recv, NULL));

    KlSockAddr dst;
    dest_v4(&dst, kl_udp_server_local_port(&b));   /* hit server B */
    ASSERT_EQ(0, kl_udp_send_to(&cli, "hi", 2, &dst));
    pump_until(&ctx, &g_cli_got, 1, 300);

    ASSERT_EQ(1, g_srv_hits);        /* exactly one server handled it */
    ASSERT_EQ(1, g_cli_got);
    ASSERT_EQ((size_t)2, g_cli_len);

    kl_udp_free(&cli);
    kl_udp_server_free(&a);
    kl_udp_server_free(&b);
    kl_event_ctx_free(&ctx);
}

UTEST(udp_server, null_and_arg_guards) {
    ASSERT_EQ(-1, kl_udp_server_init(NULL, NULL, NULL, NULL, NULL));
    ASSERT_EQ(-1, kl_udp_server_reply(NULL, "x", 1, NULL));
    ASSERT_EQ((uint16_t)0, kl_udp_server_local_port(NULL));
    ASSERT_EQ(-1, kl_udp_server_fd(NULL));

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    /* NULL handler rejected. */
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1" };
    ASSERT_EQ(-1, kl_udp_server_init(&srv, &ctx, &sc, NULL, NULL));
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_udp_server_last_error(&srv));

    /* Valid init, then double-free safety. */
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    kl_udp_server_free(&srv);
    kl_udp_server_free(&srv);

    kl_event_ctx_free(&ctx);
}

#if defined(__linux__)
/* Linux: a wildcard-bound server auto-enables pktinfo and replies FROM the
 * address the client hit. Client sends to 127.0.0.2 → reply comes from 127.0.0.2
 * (not the routing-table default), which the client observes as the source. */
UTEST(udp_server, reply_from_hit_address) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "0.0.0.0", .port = 0 };  /* wildcard */
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    uint16_t port = kl_udp_server_local_port(&srv);

    KlUdp cli;
    KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, cli_recv, NULL));

    KlSockAddr dst;
    unsigned char _ipb[4]; inet_pton(AF_INET, "127.0.0.2", _ipb); kl_sockaddr_from_ipv4(&dst, _ipb, port);   /* hit a non-primary loopback */
    ASSERT_EQ(0, kl_udp_send_to(&cli, "ping", 4, &dst));
    pump_until(&ctx, &g_cli_got, 1, 300);

    ASSERT_EQ(1, g_cli_got);
    ASSERT_STREQ("127.0.0.2", g_cli_src_ip);   /* reply egressed from the hit addr */

    kl_udp_free(&cli);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}
#endif

UTEST_MAIN();
