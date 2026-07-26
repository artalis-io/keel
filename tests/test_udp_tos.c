/* UDP ECN/TOS/DSCP marking (IP_TOS / IPV6_TCLASS).
 *
 * Socket-level and per-packet marking + RX read. Delivery of the TOS byte over
 * loopback is platform-dependent (and per-packet cmsg support varies), so the
 * delivery tests probe and UTEST_SKIP where the byte isn't surfaced; the macro
 * and validation tests are deterministic everywhere.
 */
#include "utest.h"
#include <keel/udp.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <string.h>
#include "net_compat.h"

static int g_got;
static int g_tos;

static void on_recv(KlUdp *udp, const void *data, size_t len,
                    const struct sockaddr *src, socklen_t src_len,
                    const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)data; (void)len; (void)src; (void)src_len; (void)local; (void)local_len; (void)ud;
    g_tos = kl_udp_recv_tos(udp);
    g_got++;
}

static void dest_v4(struct sockaddr_in *a, uint16_t port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a->sin_addr);
}

static void pump(KlEventCtx *ctx, int ticks) {
    for (int i = 0; i < ticks && g_got == 0; i++)
        kl_event_ctx_run(ctx, 16, 10);
}

/* ── Deterministic: macro + validation ──────────────────────────────── */

UTEST(tos, kl_tos_macro) {
    ASSERT_EQ(KL_TOS(KL_DSCP_EF, KL_ECN_ECT0), 0xBA);        /* (46<<2)|2 = 186 */
    ASSERT_EQ(KL_TOS(KL_DSCP_CS0, KL_ECN_NOT_ECT), 0);
    ASSERT_EQ(KL_TOS(KL_DSCP_CS6, KL_ECN_CE), 195);          /* (48<<2)|3 */
    ASSERT_EQ(KL_TOS(KL_DSCP_CS5, KL_ECN_NOT_ECT), 160);     /* 40<<2 */
    ASSERT_EQ(KL_TOS(0xff, 0xff), 0xff);                     /* masked to one byte */
}

UTEST(tos, validation) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdp u;
    KlUdpConfig c = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&u, &c));
    struct sockaddr_in d; dest_v4(&d, 9999);
    struct sockaddr *dp = (struct sockaddr *)&d; socklen_t dl = sizeof(d);
    static char buf[16];

    ASSERT_EQ(-1, kl_udp_send_to_tos(&u, buf, sizeof(buf), dp, dl, -1));   /* tos < 0 */
    ASSERT_EQ(-1, kl_udp_send_to_tos(&u, buf, sizeof(buf), dp, dl, 256));  /* tos > 255 */
    ASSERT_EQ(-1, kl_udp_send_to_tos(&u, buf, sizeof(buf), NULL, 0, 40));  /* no dest */
    ASSERT_EQ(-1, kl_udp_set_tos(&u, -1));
    ASSERT_EQ(-1, kl_udp_set_tos(&u, 256));
    ASSERT_EQ(-1, kl_udp_recv_tos(NULL));
    ASSERT_EQ(-1, kl_udp_recv_tos(&u));   /* recv_tos disabled → -1 */

    kl_udp_free(&u);
    kl_event_ctx_free(&ctx);
}

/* ── Delivery (probe + skip) ─────────────────────────────────────────── */

/* Returns the DSCP (>>2) of a received datagram, or -1 to signal "skip"
 * (send failed or the TOS byte wasn't surfaced by this platform). */
static int send_and_read_dscp(int send_rc, KlEventCtx *ctx) {
    if (send_rc != 0)
        return -1;
    pump(ctx, 200);
    if (g_got == 0 || g_tos < 0)
        return -1;
    return g_tos >> 2;
}

UTEST(tos, socket_level_marking) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .recv_tos = 1 };
    KlUdpConfig tc = { .ctx = &ctx, .tos = KL_TOS(KL_DSCP_CS5, KL_ECN_NOT_ECT) };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_in d; dest_v4(&d, port);

    g_got = 0; g_tos = -1;
    int rc2 = kl_udp_send_to(&tx, "x", 1, (struct sockaddr *)&d, sizeof(d));
    int dscp = send_and_read_dscp(rc2, &ctx);
    if (dscp < 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("TOS not delivered over loopback in this environment");
    }
    ASSERT_EQ(KL_DSCP_CS5, dscp);

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(tos, per_packet_marking) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .recv_tos = 1 };
    KlUdpConfig tc = { .ctx = &ctx };   /* no socket-level mark */
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_in d; dest_v4(&d, port);

    /* First per-packet mark: CS6 */
    g_got = 0; g_tos = -1;
    int r1 = kl_udp_send_to_tos(&tx, "a", 1, (struct sockaddr *)&d, sizeof(d),
                                KL_TOS(KL_DSCP_CS6, KL_ECN_ECT0));
    int dscp1 = send_and_read_dscp(r1, &ctx);
    if (dscp1 < 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("per-packet TOS unsupported/undelivered in this environment");
    }
    ASSERT_EQ(KL_DSCP_CS6, dscp1);

    /* A different per-packet mark on the next datagram: CS3 */
    g_got = 0; g_tos = -1;
    int r2 = kl_udp_send_to_tos(&tx, "b", 1, (struct sockaddr *)&d, sizeof(d),
                                KL_TOS(KL_DSCP_CS3, KL_ECN_NOT_ECT));
    int dscp2 = send_and_read_dscp(r2, &ctx);
    ASSERT_EQ(KL_DSCP_CS3, dscp2);   /* per-packet override changed the mark */

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(tos, runtime_set_tos) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .recv_tos = 1 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));
    ASSERT_EQ(0, kl_udp_set_tos(&tx, KL_TOS(KL_DSCP_CS4, KL_ECN_NOT_ECT)));

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_in d; dest_v4(&d, port);

    g_got = 0; g_tos = -1;
    int rc2 = kl_udp_send_to(&tx, "x", 1, (struct sockaddr *)&d, sizeof(d));
    int dscp = send_and_read_dscp(rc2, &ctx);
    if (dscp < 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("TOS not delivered over loopback in this environment");
    }
    ASSERT_EQ(KL_DSCP_CS4, dscp);

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
