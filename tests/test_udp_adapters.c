/*
 * test_udp_adapters.c — M6.0b: the wrapper feature-adapters (KlDatagram callback contracts → KlUdp
 * callbacks) in ISOLATION, over a SOCKETLESS KlUdp (a zeroed struct with only the callback fields set —
 * no fd, no event loop, NO second live socket owner). Proves the pure signature translation the M6.1
 * cutover depends on before any KlDatagram is embedded: peer → src, `local` surfaced only under
 * KL_DGRAM_HAS_LOCAL, user_data forwarded, a NULL user callback a safe no-op. See udp_internal.h.
 */
#include "../vendor/utest.h"

#include <keel/udp.h>
#include <keel/datagram.h>       /* KL_DGRAM_HAS_LOCAL */
#include <keel/sockaddr.h>
#include "../src/udp_internal.h" /* kl_udp_dg_on_recv / _on_segments / _on_drain (whitebox) */

#include <string.h>

static KlSockAddr addr4(int a, int b, int c, int d, int port) {
    uint8_t ip[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    KlSockAddr s; kl_sockaddr_from_ipv4(&s, ip, (uint16_t)port); return s;
}

/* ── recorders ────────────────────────────────────────────────────────────────────────────────── */
static KlUdp *g_udp; static const void *g_data; static size_t g_len; static size_t g_seg;
static int g_recv_calls, g_seg_calls, g_drain_calls;
static int g_have_src, g_src_port, g_have_local, g_local_port; static void *g_ud;

static void rec_recv(KlUdp *u, const void *data, size_t len, const KlSockAddr *src,
                     const KlSockAddr *local, void *user_data) {
    g_recv_calls++; g_udp = u; g_data = data; g_len = len; g_ud = user_data;
    g_have_src   = (src != NULL);   g_src_port   = src   ? (int)kl_sockaddr_port(src)   : -1;
    g_have_local = (local != NULL); g_local_port = local ? (int)kl_sockaddr_port(local) : -1;
}
static void rec_segments(KlUdp *u, const void *data, size_t len, size_t seg,
                         const KlSockAddr *src, const KlSockAddr *local, void *user_data) {
    g_seg_calls++; g_udp = u; g_data = data; g_len = len; g_seg = seg; g_ud = user_data;
    g_have_src   = (src != NULL);   g_src_port   = src   ? (int)kl_sockaddr_port(src)   : -1;
    g_have_local = (local != NULL); g_local_port = local ? (int)kl_sockaddr_port(local) : -1;
}
static void rec_drain(KlUdp *u, void *user_data) { g_drain_calls++; g_udp = u; g_ud = user_data; }

static void reset(void) {
    g_udp = NULL; g_data = NULL; g_len = 0; g_seg = 0;
    g_recv_calls = g_seg_calls = g_drain_calls = 0;
    g_have_src = g_have_local = 0; g_src_port = g_local_port = -1; g_ud = NULL;
}

/* A socketless KlUdp with just the callbacks set — the adapters touch nothing else. */
static KlUdp mk_udp(void) { KlUdp u; memset(&u, 0, sizeof(u)); return u; }

/* recv adapter: forwards data/len/peer→src/user_data; local surfaced when HAS_LOCAL. */
UTEST(udp_adapters, recv_forwards_and_surfaces_local) {
    reset();
    KlUdp u = mk_udp();
    int ud_marker = 7;
    u.on_recv = rec_recv; u.recv_ud = &ud_marker;

    KlSockAddr peer = addr4(192,168,0,9, 40000), local = addr4(127,0,0,1, 5353);
    kl_udp_dg_on_recv(&u, "hello", 5, &peer, &local, KL_DGRAM_HAS_LOCAL);

    ASSERT_EQ(1, g_recv_calls);
    ASSERT_EQ(&u, g_udp);
    ASSERT_EQ((size_t)5, g_len);
    ASSERT_EQ(0, memcmp(g_data, "hello", 5));
    ASSERT_TRUE(g_have_src);   ASSERT_EQ(40000, g_src_port);
    ASSERT_TRUE(g_have_local); ASSERT_EQ(5353, g_local_port);   /* surfaced under HAS_LOCAL */
    ASSERT_EQ((void *)&ud_marker, g_ud);                        /* recv_ud forwarded */
}

/* recv adapter: WITHOUT HAS_LOCAL, `local` is dropped to NULL even when a pointer is supplied. */
UTEST(udp_adapters, recv_gates_local_on_has_local_flag) {
    reset();
    KlUdp u = mk_udp();
    u.on_recv = rec_recv;
    KlSockAddr peer = addr4(10,0,0,1, 53), local = addr4(127,0,0,1, 5353);
    kl_udp_dg_on_recv(&u, "x", 1, &peer, &local, 0 /* no HAS_LOCAL */);
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_TRUE(g_have_src);
    ASSERT_FALSE(g_have_local);   /* gated to NULL: the flag governs, not the pointer */
}

/* recv adapter: a NULL user callback is a safe no-op (no crash, no delivery). */
UTEST(udp_adapters, recv_null_callback_is_noop) {
    reset();
    KlUdp u = mk_udp();   /* on_recv == NULL */
    KlSockAddr peer = addr4(10,0,0,1, 53);
    kl_udp_dg_on_recv(&u, "x", 1, &peer, NULL, 0);
    ASSERT_EQ(0, g_recv_calls);
}

/* segments adapter: forwards the segment size + user_data; local gated the same way. */
UTEST(udp_adapters, segments_forwards_seg_and_surfaces_local) {
    reset();
    KlUdp u = mk_udp();
    int seg_marker = 9;
    u.on_recv_segments = rec_segments; u.recv_seg_ud = &seg_marker;

    KlSockAddr peer = addr4(192,168,0,9, 40000), local = addr4(127,0,0,1, 5353);
    kl_udp_dg_on_segments(&u, "AABBCC", 6, 2 /*seg*/, &peer, &local, KL_DGRAM_HAS_LOCAL);

    ASSERT_EQ(1, g_seg_calls);
    ASSERT_EQ(&u, g_udp);
    ASSERT_EQ((size_t)6, g_len);
    ASSERT_EQ((size_t)2, g_seg);                     /* segment size forwarded */
    ASSERT_TRUE(g_have_local); ASSERT_EQ(5353, g_local_port);
    ASSERT_EQ((void *)&seg_marker, g_ud);            /* recv_seg_ud forwarded */
}

UTEST(udp_adapters, segments_gates_local_and_null_is_noop) {
    reset();
    KlUdp u = mk_udp();
    u.on_recv_segments = rec_segments;
    KlSockAddr peer = addr4(10,0,0,1, 53), local = addr4(127,0,0,1, 5353);
    kl_udp_dg_on_segments(&u, "AB", 2, 2, &peer, &local, 0 /* no HAS_LOCAL */);
    ASSERT_EQ(1, g_seg_calls);
    ASSERT_FALSE(g_have_local);
    /* NULL segments callback → no-op */
    reset();
    KlUdp u2 = mk_udp();   /* on_recv_segments == NULL */
    kl_udp_dg_on_segments(&u2, "AB", 2, 2, &peer, &local, KL_DGRAM_HAS_LOCAL);
    ASSERT_EQ(0, g_seg_calls);
}

/* drain adapter: forwards drain_ud; NULL callback a safe no-op. */
UTEST(udp_adapters, drain_forwards_and_null_is_noop) {
    reset();
    KlUdp u = mk_udp();
    int d_marker = 3;
    u.on_drain = rec_drain; u.drain_ud = &d_marker;
    kl_udp_dg_on_drain(&u);
    ASSERT_EQ(1, g_drain_calls);
    ASSERT_EQ(&u, g_udp);
    ASSERT_EQ((void *)&d_marker, g_ud);
    /* NULL drain callback → no-op */
    reset();
    KlUdp u2 = mk_udp();   /* on_drain == NULL */
    kl_udp_dg_on_drain(&u2);
    ASSERT_EQ(0, g_drain_calls);
}

UTEST_MAIN();
