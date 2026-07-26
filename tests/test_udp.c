#include "utest.h"
#include <keel/udp.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <string.h>
#include "net_compat.h"

/* ── Captured receive state ──────────────────────────────────────────── */

static char     g_buf[2048];
static size_t   g_len;
static int      g_got;
static char     g_src_ip[INET6_ADDRSTRLEN];
static char     g_local_ip[INET6_ADDRSTRLEN];
static int      g_has_local;

static void on_recv(KlUdp *udp, const void *data, size_t len,
                    const struct sockaddr *src, socklen_t src_len,
                    const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)udp; (void)ud; (void)src_len; (void)local_len;
    if (len > sizeof(g_buf)) len = sizeof(g_buf);
    memcpy(g_buf, data, len);
    g_len = len;
    g_src_ip[0] = '\0';
    if (src && src->sa_family == AF_INET)
        inet_ntop(AF_INET, &((const struct sockaddr_in *)src)->sin_addr,
                  g_src_ip, sizeof(g_src_ip));
    else if (src && src->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)src)->sin6_addr,
                  g_src_ip, sizeof(g_src_ip));
    g_local_ip[0] = '\0';
    g_has_local = (local != NULL);
    if (local && local->sa_family == AF_INET)
        inet_ntop(AF_INET, &((const struct sockaddr_in *)local)->sin_addr,
                  g_local_ip, sizeof(g_local_ip));
    else if (local && local->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)local)->sin6_addr,
                  g_local_ip, sizeof(g_local_ip));
    g_got++;
}

static void reset_capture(void) {
    g_len = 0; g_got = 0; g_src_ip[0] = '\0'; memset(g_buf, 0, sizeof(g_buf));
    g_local_ip[0] = '\0'; g_has_local = 0;
}

/* Pump the loop up to `ticks` times or until g_got reaches `want`. */
static void pump_until(KlEventCtx *ctx, int want, int ticks) {
    for (int i = 0; i < ticks && g_got < want; i++)
        kl_event_ctx_run(ctx, 16, 10);
}

static void dest_v4(struct sockaddr_in *a, uint16_t port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a->sin_addr);
}

/* ── Tests ───────────────────────────────────────────────────────────── */

UTEST(udp, echo_v4) {
    reset_capture();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_udp_local_port(&rx);
    ASSERT_TRUE(port > 0);

    struct sockaddr_in dst;
    dest_v4(&dst, port);
    const char *msg = "hello udp";
    ASSERT_EQ(0, kl_udp_send_to(&tx, msg, strlen(msg),
                                (struct sockaddr *)&dst, sizeof(dst)));

    pump_until(&ctx, 1, 200);
    ASSERT_EQ(1, g_got);
    ASSERT_EQ(strlen(msg), g_len);
    ASSERT_EQ(0, memcmp(g_buf, msg, g_len));
    ASSERT_STREQ("127.0.0.1", g_src_ip);   /* sender's source address */
    ASSERT_EQ((uint64_t)0, kl_udp_dropped(&tx));
    ASSERT_EQ((size_t)0, kl_udp_send_queued(&tx));

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(udp, echo_v6) {
    reset_capture();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "::1", .bind_port = 0 };
    if (kl_udp_init(&rx, &rc) != 0) {
        kl_event_ctx_free(&ctx);
        UTEST_SKIP("IPv6 unavailable");
    }
    KlUdp tx;
    KlUdpConfig tc = { .ctx = &ctx, .family = AF_INET6 };
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_in6 dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin6_family = AF_INET6;
    dst.sin6_port = htons(port);
    inet_pton(AF_INET6, "::1", &dst.sin6_addr);

    const char *msg = "v6 datagram";
    ASSERT_EQ(0, kl_udp_send_to(&tx, msg, strlen(msg),
                                (struct sockaddr *)&dst, sizeof(dst)));
    pump_until(&ctx, 1, 200);

    ASSERT_EQ(1, g_got);
    ASSERT_EQ(strlen(msg), g_len);
    ASSERT_EQ(0, memcmp(g_buf, msg, g_len));
    ASSERT_STREQ("::1", g_src_ip);

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(udp, connected_send) {
    reset_capture();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    /* send() before connect() must fail. */
    ASSERT_EQ(-1, kl_udp_send(&tx, "x", 1));
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_udp_last_error(&tx));

    struct sockaddr_in dst;
    dest_v4(&dst, kl_udp_local_port(&rx));
    ASSERT_EQ(0, kl_udp_connect(&tx, (struct sockaddr *)&dst, sizeof(dst)));

    const char *msg = "connected";
    ASSERT_EQ(0, kl_udp_send(&tx, msg, strlen(msg)));
    pump_until(&ctx, 1, 200);

    ASSERT_EQ(1, g_got);
    ASSERT_EQ(0, memcmp(g_buf, msg, g_len));

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(udp, truncation_counted) {
    reset_capture();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .recv_buf_size = 4 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    struct sockaddr_in dst;
    dest_v4(&dst, kl_udp_local_port(&rx));
    ASSERT_EQ(0, kl_udp_send_to(&tx, "abcdefgh", 8,
                                (struct sockaddr *)&dst, sizeof(dst)));
    pump_until(&ctx, 1, 200);

    ASSERT_EQ(1, g_got);
    ASSERT_TRUE(g_len <= 4);                       /* delivered truncated, not overflowed */
    ASSERT_EQ((uint64_t)1, kl_udp_truncated(&rx));

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(udp, recv_stop) {
    reset_capture();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1" };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));
    kl_udp_recv_stop(&rx);

    struct sockaddr_in dst;
    dest_v4(&dst, kl_udp_local_port(&rx));
    ASSERT_EQ(0, kl_udp_send_to(&tx, "ping", 4,
                                (struct sockaddr *)&dst, sizeof(dst)));
    pump_until(&ctx, 1, 30);                        /* short pump */
    ASSERT_EQ(0, g_got);                            /* not delivered while stopped */

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

/* Best-effort backpressure: on platforms where a tiny SO_SNDBUF forces EAGAIN,
 * exercise the queue → flush → on_drain path; where the kernel sends
 * immediately, the invariants still hold (nothing queued, no crash). */
static int  g_drained;
static void on_drain(KlUdp *udp, void *ud) { (void)udp; (void)ud; g_drained++; }

UTEST(udp, backpressure_best_effort) {
    reset_capture();
    g_drained = 0;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1" };
    KlUdpConfig tc = { .ctx = &ctx, .max_send_queue = 64 * 1024 };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));
    kl_udp_on_drain(&tx, on_drain, NULL);

    /* Shrink the send buffer to encourage EAGAIN. */
    int sndbuf = 2048;
    (void)setsockopt(kl_udp_fd(&tx), SOL_SOCKET, SO_SNDBUF, (char *)&sndbuf, sizeof(sndbuf));

    struct sockaddr_in dst;
    dest_v4(&dst, kl_udp_local_port(&rx));

    char payload[1400];
    memset(payload, 'z', sizeof(payload));
    for (int i = 0; i < 500; i++)
        (void)kl_udp_send_to(&tx, payload, sizeof(payload),
                             (struct sockaddr *)&dst, sizeof(dst));

    size_t queued_peak = kl_udp_send_queued(&tx);
    /* Drain whatever queued. */
    for (int i = 0; i < 300 && kl_udp_send_queued(&tx) > 0; i++)
        kl_event_ctx_run(&ctx, 16, 10);

    ASSERT_EQ((size_t)0, kl_udp_send_queued(&tx));   /* fully drained either way */
    if (queued_peak > 0)
        ASSERT_TRUE(g_drained >= 1);                 /* on_drain fired if we ever queued */

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

/* ── IP_PKTINFO local-address capture ────────────────────────────────── */

UTEST(udp, pktinfo_captures_local_addr) {
    /* Wildcard-bound socket with pktinfo → callback sees the local dest addr. */
    reset_capture();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "0.0.0.0", .recv_pktinfo = 1 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    struct sockaddr_in dst;
    dest_v4(&dst, kl_udp_local_port(&rx));    /* send to 127.0.0.1 */
    ASSERT_EQ(0, kl_udp_send_to(&tx, "hi", 2, (struct sockaddr *)&dst, sizeof(dst)));
    pump_until(&ctx, 1, 200);

    ASSERT_EQ(1, g_got);
    ASSERT_TRUE(g_has_local);                 /* pktinfo delivered a local addr */
    ASSERT_STREQ("127.0.0.1", g_local_ip);    /* the address we sent to */

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(udp, send_to_from_loopback) {
    /* Exercise the source-pinned send path (sendmsg + pktinfo cmsg) on every
     * platform — a regression guard for the CMSG_FIRSTHDR ordering. Delivery
     * from a pinned source is asserted only where the platform honors it. */
    reset_capture();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc_ = { .ctx = &ctx, .bind_addr = "0.0.0.0", .recv_pktinfo = 1 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc_));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    struct sockaddr_in dst, src;
    dest_v4(&dst, kl_udp_local_port(&rx));
    memset(&src, 0, sizeof(src));
    src.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &src.sin_addr);

    /* Must not crash constructing the control message (the bug this guards). */
    (void)kl_udp_send_to_from(&tx, "hi", 2, (struct sockaddr *)&dst, sizeof(dst),
                              (struct sockaddr *)&src, sizeof(src));
    pump_until(&ctx, 1, 200);
    if (g_got)                                    /* delivered → pinned source honored */
        ASSERT_STREQ("127.0.0.1", g_src_ip);

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(udp, pktinfo_off_no_local) {
    /* Without recv_pktinfo, the callback's local address is NULL. */
    reset_capture();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1" };   /* pktinfo off */
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    struct sockaddr_in dst;
    dest_v4(&dst, kl_udp_local_port(&rx));
    ASSERT_EQ(0, kl_udp_send_to(&tx, "hi", 2, (struct sockaddr *)&dst, sizeof(dst)));
    pump_until(&ctx, 1, 200);

    ASSERT_EQ(1, g_got);
    ASSERT_FALSE(g_has_local);

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

#if defined(__linux__)
/* Linux: the whole 127.0.0.0/8 is local, so we can exercise a genuine
 * multi-homed capture — send to 127.0.0.2 and confirm the local addr matches. */
UTEST(udp, pktinfo_multihomed_local) {
    reset_capture();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "0.0.0.0", .recv_pktinfo = 1 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(kl_udp_local_port(&rx));
    inet_pton(AF_INET, "127.0.0.2", &dst.sin_addr);
    ASSERT_EQ(0, kl_udp_send_to(&tx, "hi", 2, (struct sockaddr *)&dst, sizeof(dst)));
    pump_until(&ctx, 1, 200);

    ASSERT_EQ(1, g_got);
    ASSERT_STREQ("127.0.0.2", g_local_ip);    /* not 127.0.0.1 */

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}
#endif

UTEST(udp, so_bufsize_applied) {
    /* Setting a small SO_RCVBUF/SO_SNDBUF must shrink the kernel buffers below
     * the default. Shrinking is deterministic across platforms, unlike growth
     * (which the kernel caps at rmem_max/wmem_max). */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp def, small;
    KlUdpConfig dc = { .ctx = &ctx };
    KlUdpConfig sc = { .ctx = &ctx, .so_rcvbuf = 8192, .so_sndbuf = 8192 };
    ASSERT_EQ(0, kl_udp_init(&def, &dc));
    ASSERT_EQ(0, kl_udp_init(&small, &sc));

    int drb = 0, srb = 0, dsb = 0, ssb = 0;
    socklen_t l = sizeof(int);
    ASSERT_EQ(0, getsockopt(kl_udp_fd(&def),   SOL_SOCKET, SO_RCVBUF, (char *)&drb, &l));
    ASSERT_EQ(0, getsockopt(kl_udp_fd(&small), SOL_SOCKET, SO_RCVBUF, (char *)&srb, &l));
    ASSERT_EQ(0, getsockopt(kl_udp_fd(&def),   SOL_SOCKET, SO_SNDBUF, (char *)&dsb, &l));
    ASSERT_EQ(0, getsockopt(kl_udp_fd(&small), SOL_SOCKET, SO_SNDBUF, (char *)&ssb, &l));

    ASSERT_TRUE(srb > 0 && srb < drb);   /* rcvbuf knob shrank the buffer */
    ASSERT_TRUE(ssb > 0 && ssb < dsb);   /* sndbuf knob shrank the buffer */

    kl_udp_free(&small);
    kl_udp_free(&def);
    kl_event_ctx_free(&ctx);
}

UTEST(udp, null_and_arg_guards) {
    ASSERT_EQ(-1, kl_udp_init(NULL, NULL));
    ASSERT_EQ(-1, kl_udp_send(NULL, "x", 1));
    ASSERT_EQ(-1, kl_udp_send_to(NULL, "x", 1, NULL, 0));
    ASSERT_EQ((int)-1, kl_udp_fd(NULL));
    ASSERT_EQ((uint16_t)0, kl_udp_local_port(NULL));
    ASSERT_EQ((size_t)0, kl_udp_send_queued(NULL));

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdp u;
    KlUdpConfig c = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&u, &c));
    /* bad dest_len */
    ASSERT_EQ(-1, kl_udp_send_to(&u, "x", 1, (struct sockaddr *)&c, 0));
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_udp_last_error(&u));
    kl_udp_free(&u);
    kl_udp_free(&u);   /* double-free must be safe */
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
