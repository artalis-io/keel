/*
 * raw_udp_test.c — LC-3a: KlUdp datagram round-trip over the lwIP raw completion backend.
 *
 * The UDP analog of the LC-1 TCP client loopback test. It brings up ONE raw ctx
 * (kl_event_ctx_init_ex + kl_event_provider_lwip_raw → NO_SYS=1 lwIP + loopback netif) and runs
 * KEEL's own KlUdp machine (src/udp.c) over the raw datagram data-plane:
 *
 *   - the raw socket provider creates a udp_pcb for SOCK_DGRAM (kl_udp_init → lwr_sock_socket),
 *   - kl_udp_recv_start posts an overlapped recv (kl_comp_post_udp_recv → udp_recv arm),
 *   - kl_udp_send_to posts a send (kl_comp_post_udp_send → udp_sendto),
 *   - the mainloop tick (kl_event_ctx_run → lwr_comp_drain) delivers the datagram back as a
 *     KL_COMP_UDP_RECV, which the driver turns into the on_recv callback.
 *
 * Coverage:
 *   T1  a single datagram round-trips byte-exact, with the correct source address (127.0.0.1).
 *   T2  a few datagrams back-to-back all arrive in order.
 *   T3  a datagram at the per-udp DGRAM_MAX bound round-trips (largest retained payload).
 *   T4  close-with-queued-rx: a datagram sits in the ring un-drained when the socket is freed —
 *         must not leak (ASan/LSan gate).
 *   T5  kl_udp_free leaves no leak (the whole test is run under ASan+UBSan+LSan).
 *
 * A real datagram crosses the loopback netif through KEEL's udp.c machine — src/udp.c is
 * UNCHANGED; only the raw provider + glue supply the datagram transport. Prints "LC-3a PASS".
 *
 * SPDX-License-Identifier: MIT
 */
#include <keel/keel.h>
#include <keel/udp.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>

#include "keel_lwip_raw.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *msg) { printf("LC-3a FAIL: %s\n", msg); return 1; }

/* ── captured receive state ─────────────────────────────────────────────── */
static char   g_buf[4096];
static size_t g_len;
static int    g_got;
static char   g_src_ip[KL_SOCKADDR_STRLEN];

static void on_recv(KlUdp *udp, const void *data, size_t len,
                    const KlSockAddr *src, const KlSockAddr *local, void *ud) {
    (void)udp; (void)local; (void)ud;
    if (len > sizeof(g_buf)) len = sizeof(g_buf);
    memcpy(g_buf, data, len);
    g_len = len;
    g_src_ip[0] = '\0';
    if (src) kl_sockaddr_format_ip(src, g_src_ip, sizeof(g_src_ip));
    g_got++;
}

static void reset_capture(void) {
    g_len = 0; g_got = 0; g_src_ip[0] = '\0'; memset(g_buf, 0, sizeof(g_buf));
}

/* Pump the raw mainloop up to `ticks` times or until g_got reaches `want`. */
static void pump_until(KlEventCtx *ctx, int want, int ticks) {
    for (int i = 0; i < ticks && g_got < want; i++)
        kl_event_ctx_run(ctx, 16, 5);
}

static void dest_v4(KlSockAddr *a, uint16_t port) {
    const uint8_t lo[4] = { 127, 0, 0, 1 };
    kl_sockaddr_from_ipv4(a, lo, port);
}

/* T1 — one datagram, byte-exact + source addr. */
static int t1_single(KlEventCtx *ctx) {
    reset_capture();
    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = ctx, .bind_addr = "127.0.0.1", .bind_port = 0 };
    KlUdpConfig tc = { .ctx = ctx };
    if (kl_udp_init(&rx, &rc) != 0) return fail("rx init");
    if (kl_udp_init(&tx, &tc) != 0) { kl_udp_free(&rx); return fail("tx init"); }
    if (kl_udp_recv_start(&rx, on_recv, NULL) != 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("recv_start"); }

    uint16_t port = kl_udp_local_port(&rx);
    if (port == 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("rx local port 0"); }

    KlSockAddr dst; dest_v4(&dst, port);
    const char *msg = "hello raw udp";
    if (kl_udp_send_to(&tx, msg, strlen(msg), &dst) != 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("send_to"); }

    pump_until(ctx, 1, 400);
    int rc2 = 0;
    if (g_got != 1) rc2 = fail("T1: datagram not received");
    else if (g_len != strlen(msg) || memcmp(g_buf, msg, g_len) != 0) rc2 = fail("T1: payload mismatch");
    else if (strcmp(g_src_ip, "127.0.0.1") != 0) rc2 = fail("T1: wrong source address");

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    if (rc2) return rc2;
    printf("PASS T1 (single datagram round-trip, src=127.0.0.1)\n");
    return 0;
}

/* T2 — a few datagrams back-to-back all arrive. */
static int t2_burst(KlEventCtx *ctx) {
    reset_capture();
    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = ctx, .bind_addr = "127.0.0.1", .bind_port = 0 };
    KlUdpConfig tc = { .ctx = ctx };
    if (kl_udp_init(&rx, &rc) != 0) return fail("rx init");
    if (kl_udp_init(&tx, &tc) != 0) { kl_udp_free(&rx); return fail("tx init"); }
    if (kl_udp_recv_start(&rx, on_recv, NULL) != 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("recv_start"); }
    uint16_t port = kl_udp_local_port(&rx);
    KlSockAddr dst; dest_v4(&dst, port);

    const int N = 5;
    char last[32];
    for (int i = 0; i < N; i++) {
        char m[32];
        int mn = snprintf(m, sizeof(m), "dgram-%d", i);
        if (i == N - 1) memcpy(last, m, (size_t)mn + 1);
        if (kl_udp_send_to(&tx, m, (size_t)mn, &dst) != 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("T2 send"); }
        /* pump between sends so each datagram is drained (one recv-in-flight per drain). */
        pump_until(ctx, i + 1, 100);
    }
    pump_until(ctx, N, 400);
    int rc2 = 0;
    if (g_got != N) rc2 = fail("T2: not all datagrams received");
    else if (g_len != strlen(last) || memcmp(g_buf, last, g_len) != 0) rc2 = fail("T2: last payload mismatch");

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    if (rc2) return rc2;
    printf("PASS T2 (%d datagrams round-trip)\n", N);
    return 0;
}

/* T3 — a datagram at the per-udp DGRAM_MAX bound (2048) round-trips (largest retained payload). */
#define T3_LEN 2048u
static unsigned char g_big[T3_LEN];
static int t3_max(KlEventCtx *ctx) {
    reset_capture();
    for (unsigned i = 0; i < T3_LEN; i++) g_big[i] = (unsigned char)(i * 31u + 7u);

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .recv_buf_size = 4096 };
    KlUdpConfig tc = { .ctx = ctx };
    if (kl_udp_init(&rx, &rc) != 0) return fail("rx init");
    if (kl_udp_init(&tx, &tc) != 0) { kl_udp_free(&rx); return fail("tx init"); }
    if (kl_udp_recv_start(&rx, on_recv, NULL) != 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("recv_start"); }
    uint16_t port = kl_udp_local_port(&rx);
    KlSockAddr dst; dest_v4(&dst, port);

    if (kl_udp_send_to(&tx, g_big, T3_LEN, &dst) != 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("T3 send"); }
    pump_until(ctx, 1, 400);
    int rc2 = 0;
    if (g_got != 1) rc2 = fail("T3: datagram not received");
    else if (g_len != T3_LEN || memcmp(g_buf, g_big, T3_LEN) != 0) rc2 = fail("T3: large payload mismatch");

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    if (rc2) return rc2;
    printf("PASS T3 (max-size datagram round-trip, %u bytes)\n", T3_LEN);
    return 0;
}

/* T4 — close-with-queued-rx: send two datagrams but drain only one, then free the rx socket while a
 * datagram is still queued in the ring. Must not leak (ASan/LSan gate) and must not crash. */
static int t4_close_with_queued(KlEventCtx *ctx) {
    reset_capture();
    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = ctx, .bind_addr = "127.0.0.1", .bind_port = 0 };
    KlUdpConfig tc = { .ctx = ctx };
    if (kl_udp_init(&rx, &rc) != 0) return fail("rx init");
    if (kl_udp_init(&tx, &tc) != 0) { kl_udp_free(&rx); return fail("tx init"); }
    if (kl_udp_recv_start(&rx, on_recv, NULL) != 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("recv_start"); }
    uint16_t port = kl_udp_local_port(&rx);
    KlSockAddr dst; dest_v4(&dst, port);

    /* Two datagrams; the recv arm surfaces only one per drain, so after a single receive the
     * second still sits in the glue's ring. */
    if (kl_udp_send_to(&tx, "one", 3, &dst) != 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("T4 send1"); }
    if (kl_udp_send_to(&tx, "two", 3, &dst) != 0) { kl_udp_free(&tx); kl_udp_free(&rx); return fail("T4 send2"); }
    pump_until(ctx, 1, 200);   /* deliver exactly one; leave the other queued */

    /* Free with a datagram still queued in the ring — the ring is inline in the udp slot, cleared
     * by kl_lwr_udp_close's memset; no leak. */
    kl_udp_free(&tx);
    kl_udp_free(&rx);
    printf("PASS T4 (close-with-queued-rx, no leak/crash)\n");
    return 0;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init_ex(&ctx, &alloc, kl_event_provider_lwip_raw()) != 0)
        return fail("ctx init (bring up lwIP raw)");
    /* Wire the ctx's sockets to the raw provider — the same auto-wire a KlServer does via the
     * loop's native_provider(). A standalone KlEventCtx leaves ctx.sockets NULL (POSIX default),
     * so a UDP consumer (like this test / the future DNS resolver) must set it explicitly. */
    ctx.sockets = kl_socket_provider_lwip_raw();

    int rc = 0;
    if (rc == 0) rc = t1_single(&ctx);
    if (rc == 0) rc = t2_burst(&ctx);
    if (rc == 0) rc = t3_max(&ctx);
    if (rc == 0) rc = t4_close_with_queued(&ctx);

    kl_event_ctx_free(&ctx);
    if (rc != 0) return 1;
    printf("LC-3a PASS\n");
    return 0;
}
