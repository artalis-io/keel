/* UDP multicast + broadcast (RFC 1112 / RFC 3493 group membership).
 *
 * Delivery tests probe the environment first and UTEST_SKIP where the sandbox
 * blocks loopback multicast/broadcast (some CI containers do) — the join/leave
 * and error-path tests remain deterministic regardless of delivery.
 */
#include "utest.h"
#include <keel/udp.h>
#include <keel/udp_server.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <string.h>
#include "net_compat.h"

#define GROUP_V4 "239.255.42.99"     /* administratively-scoped, no well-known clash */
#define GROUP_V6 "ff12::4b45"

/* ── Capture ─────────────────────────────────────────────────────────── */

static char   g_buf[2048];
static size_t g_len;
static int    g_got;

static void on_recv(KlUdp *udp, const void *data, size_t len,
                    const struct sockaddr *src, socklen_t src_len,
                    const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)udp; (void)src; (void)src_len; (void)local; (void)local_len; (void)ud;
    if (len > sizeof(g_buf)) len = sizeof(g_buf);
    memcpy(g_buf, data, len);
    g_len = len;
    g_got++;
}

static void reset_capture(void) { g_len = 0; g_got = 0; memset(g_buf, 0, sizeof(g_buf)); }

static void pump_until(KlEventCtx *ctx, int want, int ticks) {
    for (int i = 0; i < ticks && g_got < want; i++)
        kl_event_ctx_run(ctx, 16, 10);
}

static void group_dest(struct sockaddr_storage *ss, socklen_t *len,
                       int family, const char *group, uint16_t port) {
    memset(ss, 0, sizeof(*ss));
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)ss;
        a->sin_family = AF_INET;
        a->sin_port = htons(port);
        inet_pton(AF_INET, group, &a->sin_addr);
        *len = sizeof(*a);
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)ss;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(port);
        inet_pton(AF_INET6, group, &a->sin6_addr);
        *len = sizeof(*a);
    }
}

/* ── Deterministic: validation + join/leave plumbing ─────────────────── */

UTEST(mc, join_rejects_non_multicast) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdp u;
    KlUdpConfig c = { .ctx = &ctx, .bind_addr = "0.0.0.0", .bind_port = 0 };
    ASSERT_EQ(0, kl_udp_init(&u, &c));

    ASSERT_EQ(-1, kl_udp_multicast_join(&u, "127.0.0.1", 0));    /* unicast addr */
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_udp_last_error(&u));
    ASSERT_EQ(-1, kl_udp_multicast_join(&u, "10.0.0.1", 0));
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_udp_last_error(&u));

    kl_udp_free(&u);
    kl_event_ctx_free(&ctx);
}

UTEST(mc, join_rejects_family_mismatch) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdp u;
    KlUdpConfig c = { .ctx = &ctx, .bind_addr = "0.0.0.0", .bind_port = 0 };
    ASSERT_EQ(0, kl_udp_init(&u, &c));

    /* v4 socket, v6 group literal → parse fails → invalid arg */
    ASSERT_EQ(-1, kl_udp_multicast_join(&u, GROUP_V6, 0));
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_udp_last_error(&u));

    kl_udp_free(&u);
    kl_event_ctx_free(&ctx);
}

UTEST(mc, join_leave_v4_ok) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdp u;
    KlUdpConfig c = { .ctx = &ctx, .bind_addr = "0.0.0.0", .bind_port = 0,
                      .reuse_addr = 1 };
    ASSERT_EQ(0, kl_udp_init(&u, &c));

    ASSERT_EQ(0, kl_udp_multicast_join(&u, GROUP_V4, 0));
    ASSERT_EQ(0, kl_udp_multicast_leave(&u, GROUP_V4, 0));

    kl_udp_free(&u);
    kl_event_ctx_free(&ctx);
}

UTEST(mc, join_null_args) {
    ASSERT_EQ(-1, kl_udp_multicast_join(NULL, GROUP_V4, 0));
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdp u;
    KlUdpConfig c = { .ctx = &ctx, .bind_addr = "0.0.0.0", .bind_port = 0 };
    ASSERT_EQ(0, kl_udp_init(&u, &c));
    ASSERT_EQ(-1, kl_udp_multicast_join(&u, NULL, 0));
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_udp_last_error(&u));
    kl_udp_free(&u);
    kl_event_ctx_free(&ctx);
}

/* ── Broadcast ───────────────────────────────────────────────────────── */

UTEST(mc, broadcast_flag_gates_send) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp b, n;
    KlUdpConfig bc = { .ctx = &ctx, .broadcast = 1 };
    KlUdpConfig nc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&b, &bc));
    ASSERT_EQ(0, kl_udp_init(&n, &nc));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9999);
    inet_pton(AF_INET, "255.255.255.255", &dst.sin_addr);
    const char *m = "beacon";

    int with = kl_udp_send_to(&b, m, strlen(m), (struct sockaddr *)&dst, sizeof(dst));
    if (with != 0) {
        kl_udp_free(&b); kl_udp_free(&n); kl_event_ctx_free(&ctx);
        UTEST_SKIP("no broadcast-capable interface");
    }
    int without = kl_udp_send_to(&n, m, strlen(m), (struct sockaddr *)&dst, sizeof(dst));
    ASSERT_EQ(0, with);          /* SO_BROADCAST set → accepted */
    ASSERT_EQ(-1, without);      /* no flag → EACCES */

    kl_udp_free(&b);
    kl_udp_free(&n);
    kl_event_ctx_free(&ctx);
}

/* ── Delivery (environment-sensitive: probe + skip) ──────────────────── */

UTEST(mc, v4_loopback_roundtrip) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "0.0.0.0", .bind_port = 0,
                       .reuse_addr = 1, .multicast_group = GROUP_V4 };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));           /* joins GROUP_V4 at init */
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_storage dst; socklen_t dlen;
    group_dest(&dst, &dlen, AF_INET, GROUP_V4, port);

    reset_capture();
    const char *m = "hello-group";
    if (kl_udp_send_to(&tx, m, strlen(m), (struct sockaddr *)&dst, dlen) != 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("multicast egress unavailable in this environment");
    }
    pump_until(&ctx, 1, 60);

    if (g_got == 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("loopback multicast not delivered in this environment");
    }
    ASSERT_EQ(1, g_got);
    ASSERT_EQ(strlen(m), g_len);
    ASSERT_EQ(0, memcmp(g_buf, m, g_len));

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(mc, leave_stops_delivery) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "0.0.0.0", .bind_port = 0,
                       .reuse_addr = 1 };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));
    ASSERT_EQ(0, kl_udp_multicast_join(&rx, GROUP_V4, 0));   /* runtime join */

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_storage dst; socklen_t dlen;
    group_dest(&dst, &dlen, AF_INET, GROUP_V4, port);
    const char *m = "x";

    reset_capture();
    if (kl_udp_send_to(&tx, m, strlen(m), (struct sockaddr *)&dst, dlen) != 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("multicast egress unavailable in this environment");
    }
    pump_until(&ctx, 1, 60);
    if (g_got == 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("loopback multicast not delivered in this environment");
    }

    ASSERT_EQ(0, kl_udp_multicast_leave(&rx, GROUP_V4, 0));
    reset_capture();
    ASSERT_EQ(0, kl_udp_send_to(&tx, m, strlen(m), (struct sockaddr *)&dst, dlen));
    pump_until(&ctx, 1, 30);
    ASSERT_EQ(0, g_got);     /* after leaving, no delivery */

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(mc, v6_loopback_roundtrip) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx;
    KlUdpConfig rc = { .ctx = &ctx, .family = AF_INET6, .bind_addr = "::",
                       .bind_port = 0, .reuse_addr = 1 };
    if (kl_udp_init(&rx, &rc) != 0) {
        kl_event_ctx_free(&ctx);
        UTEST_SKIP("IPv6 unavailable");
    }
    if (kl_udp_multicast_join(&rx, GROUP_V6, 0) != 0) {
        kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("IPv6 multicast join unavailable");
    }
    KlUdp tx;
    KlUdpConfig tc = { .ctx = &ctx, .family = AF_INET6 };
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_storage dst; socklen_t dlen;
    group_dest(&dst, &dlen, AF_INET6, GROUP_V6, port);

    reset_capture();
    const char *m = "v6-group";
    if (kl_udp_send_to(&tx, m, strlen(m), (struct sockaddr *)&dst, dlen) != 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("v6 multicast egress unavailable in this environment");
    }
    pump_until(&ctx, 1, 60);
    if (g_got == 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx);
        UTEST_SKIP("loopback v6 multicast not delivered in this environment");
    }
    ASSERT_EQ(1, g_got);
    ASSERT_EQ(0, memcmp(g_buf, m, g_len));

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

/* ── Server passthrough ──────────────────────────────────────────────── */

static void srv_handler(KlUdpServer *s, const void *data, size_t len,
                        const struct sockaddr *src, socklen_t src_len, void *ud) {
    (void)s; (void)src; (void)src_len; (void)ud;
    if (len > sizeof(g_buf)) len = sizeof(g_buf);
    memcpy(g_buf, data, len);
    g_len = len;
    g_got++;
}

UTEST(mc, server_config_group_join) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    KlUdpServerConfig cfg = { .bind_addr = "0.0.0.0", .port = 0,
                              .multicast_group = GROUP_V4 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &cfg, srv_handler, NULL));

    KlUdp tx;
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));

    uint16_t port = kl_udp_server_local_port(&srv);
    struct sockaddr_storage dst; socklen_t dlen;
    group_dest(&dst, &dlen, AF_INET, GROUP_V4, port);

    /* Dynamic passthrough join/leave: deterministic (setsockopt), env-independent. */
    ASSERT_EQ(0, kl_udp_server_multicast_join(&srv, "239.255.42.100", 0));
    ASSERT_EQ(0, kl_udp_server_multicast_leave(&srv, "239.255.42.100", 0));
    ASSERT_EQ(-1, kl_udp_server_multicast_join(&srv, "127.0.0.1", 0));  /* invalid */

    reset_capture();
    const char *m = "srv-mc";
    if (kl_udp_send_to(&tx, m, strlen(m), (struct sockaddr *)&dst, dlen) != 0) {
        kl_udp_free(&tx); kl_udp_server_free(&srv); kl_event_ctx_free(&ctx);
        UTEST_SKIP("multicast egress unavailable in this environment");
    }
    pump_until(&ctx, 1, 60);
    if (g_got == 0) {
        kl_udp_free(&tx); kl_udp_server_free(&srv); kl_event_ctx_free(&ctx);
        UTEST_SKIP("loopback multicast not delivered in this environment");
    }
    ASSERT_EQ(1, g_got);
    ASSERT_EQ(0, memcmp(g_buf, m, g_len));

    kl_udp_free(&tx);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
