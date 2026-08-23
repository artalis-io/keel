/*
 * test_datagram_multicast.c — LIVE multicast join/leave/delivery over KlDatagram. The
 * validation/routing/gating cases are covered deterministically by
 * test_datagram_public.m2_multicast_*; this file adds the live-delivery coverage. Delivery is
 * environment-sensitive: these probe the sandbox and UTEST_SKIP where loopback multicast is unavailable.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "../vendor/utest.h"

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>

#include <string.h>
#include <sys/socket.h>

#define GROUP_V4 "239.255.13.7"
#define GROUP_V6 "ff12::4b45:454c"

static KlAllocator g_alloc;
static int g_got; static unsigned char g_buf[512]; static size_t g_len;
static void on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                    const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)peer; (void)local; (void)flags;
    g_got++; g_len = len; if (len) memcpy(g_buf, data, len <= sizeof(g_buf) ? len : sizeof(g_buf));
}
static void pump(KlEventCtx *ctx, int ticks) { for (int i = 0; i < ticks && g_got == 0; i++) kl_event_ctx_run(ctx, 8, 10); }
static void group_dest(KlSockAddr *out, const char *group, uint16_t port) { kl_sockaddr_parse(out, group, port); }
static void done(KlEventCtx *ctx, KlDatagram *a, KlDatagram *b) {
    kl_datagram_close_begin(a); kl_datagram_close_begin(b);
    for (int i = 0; i < 100 && (kl_datagram_close_state(a) != KL_DGRAM_CLOSE_CLOSED ||
                                kl_datagram_close_state(b) != KL_DGRAM_CLOSE_CLOSED); i++)
        kl_event_ctx_run(ctx, 8, 10);
    kl_datagram_free(a); kl_datagram_free(b); kl_event_ctx_free(ctx);
}

/* join-at-init + loopback delivery to the group. */
UTEST(datagram_mc, v4_loopback_roundtrip) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "0.0.0.0",
                                  .reuse_addr = 1, .multicast_group = GROUP_V4 };  /* joins at init */
    ASSERT_EQ(0, kl_datagram_socket_init(&rx, &rc));
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_datagram_local_port(&rx);
    KlSockAddr dst; group_dest(&dst, GROUP_V4, port);
    g_got = 0;
    const char *m = "hello-group";
    KlDatagramMessage msg = { .data = m, .len = strlen(m), .peer = &dst, .tos = -1 };
    if (kl_datagram_send(&tx, &msg) != KL_DATAGRAM_ACCEPTED) { done(&ctx, &rx, &tx); UTEST_SKIP("multicast egress unavailable"); }
    pump(&ctx, 60);
    if (g_got == 0) { done(&ctx, &rx, &tx); UTEST_SKIP("loopback multicast not delivered here"); }
    ASSERT_EQ(1, g_got);
    ASSERT_EQ(strlen(m), g_len);
    ASSERT_EQ(0, memcmp(g_buf, m, g_len));
    done(&ctx, &rx, &tx);
}

/* runtime join → deliver → leave → no delivery. */
UTEST(datagram_mc, leave_stops_delivery) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    /* request MULTICAST opportunistically so the runtime join is granted (socket_init adds it only for a
     * join-at-init group; here we join at runtime, so ask via optional_caps). */
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "0.0.0.0",
                                  .reuse_addr = 1, .optional_caps = KL_DGRAM_CAP_MULTICAST };
    ASSERT_EQ(0, kl_datagram_socket_init(&rx, &rc));
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));
    ASSERT_EQ(0, kl_datagram_multicast_join(&rx, GROUP_V4, 0));

    uint16_t port = kl_datagram_local_port(&rx);
    KlSockAddr dst; group_dest(&dst, GROUP_V4, port);
    g_got = 0;
    KlDatagramMessage msg = { .data = "x", .len = 1, .peer = &dst, .tos = -1 };
    if (kl_datagram_send(&tx, &msg) != KL_DATAGRAM_ACCEPTED) { done(&ctx, &rx, &tx); UTEST_SKIP("multicast egress unavailable"); }
    pump(&ctx, 60);
    if (g_got == 0) { done(&ctx, &rx, &tx); UTEST_SKIP("loopback multicast not delivered here"); }

    ASSERT_EQ(0, kl_datagram_multicast_leave(&rx, GROUP_V4, 0));
    g_got = 0;
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &msg));
    for (int i = 0; i < 30; i++) kl_event_ctx_run(&ctx, 8, 10);
    ASSERT_EQ(0, g_got);   /* after leaving, no delivery */
    done(&ctx, &rx, &tx);
}

/* IPv6 loopback multicast (join at init + delivery). */
UTEST(datagram_mc, v6_loopback_roundtrip) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "::",
                                  .reuse_addr = 1, .multicast_group = GROUP_V6 };
    if (kl_datagram_socket_init(&rx, &rc) != 0) { kl_event_ctx_free(&ctx); UTEST_SKIP("IPv6 multicast unavailable"); }
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc, .family = AF_INET6 };
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_datagram_local_port(&rx);
    KlSockAddr dst; group_dest(&dst, GROUP_V6, port);
    g_got = 0;
    const char *m = "hi6";
    KlDatagramMessage msg = { .data = m, .len = strlen(m), .peer = &dst, .tos = -1 };
    if (kl_datagram_send(&tx, &msg) != KL_DATAGRAM_ACCEPTED) { done(&ctx, &rx, &tx); UTEST_SKIP("v6 multicast egress unavailable"); }
    pump(&ctx, 60);
    if (g_got == 0) { done(&ctx, &rx, &tx); UTEST_SKIP("v6 loopback multicast not delivered here"); }
    ASSERT_EQ(1, g_got);
    ASSERT_EQ(strlen(m), g_len);
    done(&ctx, &rx, &tx);
}

UTEST_MAIN();
