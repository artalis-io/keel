/* UDP GSO/GRO offload (Phase B).
 *
 * kl_udp_send_gso and recv_gro are transparent: on Linux they use UDP_SEGMENT /
 * UDP_GRO, elsewhere they fall back to per-segment sends / plain receives. These
 * tests assert observable per-segment delivery and content, so they validate on
 * every platform — the offload path on Linux, the fallback elsewhere. GRO
 * coalescing is non-deterministic (the kernel decides), so the GRO tests assert
 * correctness whether or not coalescing occurred.
 */
#include "utest.h"
#include <keel/udp.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <string.h>
#include <stdint.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define NSEG  6
#define SEGSZ 200

static uint8_t g_seen[256];
static int     g_count;
static int     g_bad;
static uint8_t g_sendbuf[NSEG * SEGSZ];

/* Segment s: byte 0 = s, byte j = (uint8_t)(s + j) — self-describing + checkable. */
static void build_segments(void) {
    for (int s = 0; s < NSEG; s++)
        for (int j = 0; j < SEGSZ; j++)
            g_sendbuf[s * SEGSZ + j] = (uint8_t)(j == 0 ? s : (s + j));
}

static void note_segment(const uint8_t *p, size_t len) {
    if (len < 1) { g_bad++; return; }
    uint8_t seq = p[0];
    for (size_t j = 1; j < len; j++)
        if (p[j] != (uint8_t)(seq + j)) { g_bad++; return; }
    if (!g_seen[seq]) { g_seen[seq] = 1; g_count++; }
}

static void on_recv(KlUdp *udp, const void *data, size_t len,
                    const struct sockaddr *src, socklen_t src_len,
                    const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)udp; (void)src; (void)src_len; (void)local; (void)local_len; (void)ud;
    note_segment(data, len);
}

/* Coalesced-GRO callback: split the buffer and note each segment. */
static void on_segments(KlUdp *udp, const void *data, size_t len, size_t seg,
                        const struct sockaddr *src, socklen_t src_len,
                        const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)udp; (void)src; (void)src_len; (void)local; (void)local_len; (void)ud;
    size_t off = 0;
    while (off < len) {
        size_t c = (len - off < seg) ? (len - off) : seg;
        note_segment((const uint8_t *)data + off, c);
        off += c;
    }
}

static void reset_capture(void) { memset(g_seen, 0, sizeof(g_seen)); g_count = 0; g_bad = 0; }

static void pump_until(KlEventCtx *ctx, int want, int ticks) {
    for (int i = 0; i < ticks && g_count < want; i++)
        kl_event_ctx_run(ctx, 16, 10);
}

static void dest_v4(struct sockaddr_in *a, uint16_t port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a->sin_addr);
}

/* ── GSO ─────────────────────────────────────────────────────────────── */

UTEST(gso, roundtrip) {
    build_segments();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .so_rcvbuf = 1 << 20 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_in d; dest_v4(&d, port);

    reset_capture();
    ASSERT_EQ(0, kl_udp_send_gso(&tx, g_sendbuf, NSEG * SEGSZ, SEGSZ,
                                 (struct sockaddr *)&d, sizeof(d)));
    pump_until(&ctx, NSEG, 300);

    ASSERT_EQ(NSEG, g_count);   /* all segments delivered as distinct datagrams */
    ASSERT_EQ(0, g_bad);        /* content intact */

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(gso, single_segment) {
    build_segments();
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
    struct sockaddr_in d; dest_v4(&d, port);

    reset_capture();
    /* segment_size == total_len → exactly one datagram */
    ASSERT_EQ(0, kl_udp_send_gso(&tx, g_sendbuf, SEGSZ, SEGSZ,
                                 (struct sockaddr *)&d, sizeof(d)));
    pump_until(&ctx, 1, 100);
    ASSERT_EQ(1, g_count);
    ASSERT_EQ(0, g_bad);

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST(gso, validation) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdp tx;
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    struct sockaddr_in d; dest_v4(&d, 9999);
    struct sockaddr *dp = (struct sockaddr *)&d;
    socklen_t dl = sizeof(d);
    static uint8_t buf[64];

    ASSERT_EQ(-1, kl_udp_send_gso(&tx, buf, 0, 10, dp, dl));        /* total 0 */
    ASSERT_EQ(-1, kl_udp_send_gso(&tx, buf, 64, 0, dp, dl));        /* seg 0 */
    ASSERT_EQ(-1, kl_udp_send_gso(&tx, buf, 64, 65, dp, dl));       /* seg > total */
    ASSERT_EQ(-1, kl_udp_send_gso(&tx, buf, 70000, 100, dp, dl));   /* total > 65507 */
    ASSERT_EQ(-1, kl_udp_send_gso(&tx, buf, 64, 10, NULL, 0));      /* no dest */
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_udp_last_error(&tx));

    kl_udp_free(&tx);
    kl_event_ctx_free(&ctx);
}

/* ── GRO ─────────────────────────────────────────────────────────────── */

/* recv_gro + transparent split: coalesced or not, all NSEG arrive intact. */
UTEST(gro, transparent_split) {
    build_segments();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .recv_gro = 1, .so_rcvbuf = 1 << 20 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_in d; dest_v4(&d, port);

    reset_capture();
    ASSERT_EQ(0, kl_udp_send_gso(&tx, g_sendbuf, NSEG * SEGSZ, SEGSZ,
                                 (struct sockaddr *)&d, sizeof(d)));
    pump_until(&ctx, NSEG, 300);

    ASSERT_EQ(NSEG, g_count);
    ASSERT_EQ(0, g_bad);

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

/* recv_gro + coalesced callback: whether datagrams arrive via on_segments
 * (coalesced) or on_recv (not), the total logical count is NSEG, intact. */
UTEST(gro, coalesced_callback) {
    build_segments();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .recv_gro = 1, .so_rcvbuf = 1 << 20 };
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));
    kl_udp_recv_segments(&rx, on_segments, NULL);   /* coalesced buffers → on_segments */

    uint16_t port = kl_udp_local_port(&rx);
    struct sockaddr_in d; dest_v4(&d, port);

    reset_capture();
    ASSERT_EQ(0, kl_udp_send_gso(&tx, g_sendbuf, NSEG * SEGSZ, SEGSZ,
                                 (struct sockaddr *)&d, sizeof(d)));
    pump_until(&ctx, NSEG, 300);

    ASSERT_EQ(NSEG, g_count);
    ASSERT_EQ(0, g_bad);

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
