/* UDP recvmmsg/sendmmsg batching (Phase A).
 *
 * Batching is transparent: on Linux it uses recvmmsg/sendmmsg, elsewhere it
 * falls back to the per-datagram loop. These tests assert the observable
 * behavior (every datagram delivered, correct content, counts) so they validate
 * both paths — the batched path on the Linux CI runners, the fallback locally.
 */
#include "utest.h"
#include <keel/udp.h>
#include <keel/udp_server.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <string.h>
#include <stdint.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define NSEND 40

static uint8_t g_seen[256];
static int     g_count;
static int     g_bad;

static void on_recv(KlUdp *udp, const void *data, size_t len,
                    const struct sockaddr *src, socklen_t src_len,
                    const struct sockaddr *local, socklen_t local_len, void *ud) {
    (void)udp; (void)src; (void)src_len; (void)local; (void)local_len; (void)ud;
    if (len != 3) { g_bad++; return; }            /* payload = { seq, 0xAB, seq^0xFF } */
    const uint8_t *p = data;
    uint8_t seq = p[0];
    if (p[1] != 0xAB || p[2] != (uint8_t)(seq ^ 0xFF)) { g_bad++; return; }
    if (!g_seen[seq]) { g_seen[seq] = 1; g_count++; }
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

/* Send NSEND sequenced datagrams from tx to (127.0.0.1:port). */
static void blast(KlUdp *tx, uint16_t port) {
    struct sockaddr_in d;
    dest_v4(&d, port);
    for (int i = 0; i < NSEND; i++) {
        uint8_t buf[3] = { (uint8_t)i, 0xAB, (uint8_t)(i ^ 0xFF) };
        kl_udp_send_to(tx, buf, sizeof(buf), (struct sockaddr *)&d, sizeof(d));
    }
}

/* Run a receiver with the given batch size; returns the received count (or -1 on
 * a setup error). g_bad is left set for the caller to assert. ASSERT_* macros
 * can't be used outside a UTEST body, so results come back as return/globals. */
static int run_batch(int batch) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init(&ctx, &alloc) != 0) return -1;

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .mmsg_batch = batch, .so_rcvbuf = 1 << 20 };
    KlUdpConfig tc = { .ctx = &ctx, .mmsg_batch = batch };
    if (kl_udp_init(&rx, &rc) != 0) { kl_event_ctx_free(&ctx); return -1; }
    if (kl_udp_init(&tx, &tc) != 0) { kl_udp_free(&rx); kl_event_ctx_free(&ctx); return -1; }
    if (kl_udp_recv_start(&rx, on_recv, NULL) != 0) {
        kl_udp_free(&tx); kl_udp_free(&rx); kl_event_ctx_free(&ctx); return -1;
    }

    uint16_t port = kl_udp_local_port(&rx);
    reset_capture();
    blast(&tx, port);
    pump_until(&ctx, NSEND, 300);
    int c = g_count;

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
    return c;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

UTEST(bat, recv_batch_default) { ASSERT_EQ(NSEND, run_batch(0));  ASSERT_EQ(0, g_bad); }
UTEST(bat, recv_batch_large)   { ASSERT_EQ(NSEND, run_batch(64)); ASSERT_EQ(0, g_bad); }
UTEST(bat, batch_one_parity)   { ASSERT_EQ(NSEND, run_batch(1));  ASSERT_EQ(0, g_bad); }

/* Backpressure: a tiny send buffer forces queuing, so the send drain (batched
 * on Linux) runs; all datagrams still arrive. */
UTEST(bat, send_queue_drains_all) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .so_rcvbuf = 1 << 20 };
    KlUdpConfig tc = { .ctx = &ctx, .so_sndbuf = 4096 };   /* small → backpressure */
    ASSERT_EQ(0, kl_udp_init(&rx, &rc));
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));
    ASSERT_EQ(0, kl_udp_recv_start(&rx, on_recv, NULL));

    uint16_t port = kl_udp_local_port(&rx);
    reset_capture();
    blast(&tx, port);
    /* Pump both sockets: tx flushes its queue on writability, rx receives. */
    for (int i = 0; i < 300 && g_count < NSEND; i++)
        kl_event_ctx_run(&ctx, 16, 10);

    ASSERT_EQ(NSEND, g_count);
    ASSERT_EQ(0, g_bad);
    ASSERT_EQ((size_t)0, kl_udp_send_queued(&tx));   /* fully drained */

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);
}

/* Batching composes with the server dispatch surface. */
static void srv_handler(KlUdpServer *s, const void *data, size_t len,
                        const struct sockaddr *src, socklen_t src_len, void *ud) {
    (void)s; (void)ud;
    on_recv(NULL, data, len, src, src_len, NULL, 0, NULL);
}

UTEST(bat, server_batch) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    KlUdpServerConfig cfg = { .bind_addr = "127.0.0.1", .port = 0,
                              .mmsg_batch = 16, .so_rcvbuf = 1 << 20 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &cfg, srv_handler, NULL));

    KlUdp tx;
    KlUdpConfig tc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&tx, &tc));

    uint16_t port = kl_udp_server_local_port(&srv);
    reset_capture();
    blast(&tx, port);
    for (int i = 0; i < 300 && g_count < NSEND; i++)
        kl_event_ctx_run(&ctx, 16, 10);

    ASSERT_EQ(NSEND, g_count);
    ASSERT_EQ(0, g_bad);

    kl_udp_free(&tx);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
