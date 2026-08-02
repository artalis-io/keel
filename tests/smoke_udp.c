/*
 * smoke_udp.c — UDP datagram link + roundtrip smoke test.
 *
 * NOT a utest suite (not tests/test_*.c) — a standalone program built by the
 * `smoke-udp` Makefile target. It links the UDP datagram core (KlUdp over
 * KlEventCtx) and drives one real datagram over loopback, proving the
 * platform's datagram I/O both links and runs (the Windows CI gate for
 * udp_io_win.c: WSARecvMsg/WSASendMsg + cmsg). Runs on POSIX too.
 *
 * Single-threaded: an event-loop tick pump drives both the send flush and the
 * receive. recv_pktinfo is enabled so the WSARecvMsg/cmsg path runs (recovering
 * the datagram's local/destination address) rather than plain recvfrom.
 */

#include <keel/udp.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/net.h>       /* winsock (Windows) provides sockaddr_in + inet_pton */
#ifndef _WIN32
#include <netinet/in.h>     /* struct sockaddr_in */
#include <arpa/inet.h>      /* inet_pton, htons */
#endif
#include <string.h>
#include <stdio.h>

#define SMOKE_MSG "keel-udp-smoke"

static int    g_got;
static char   g_buf[256];
static size_t g_len;
static int    g_has_local;

static void on_recv(KlUdp *udp, const void *data, size_t len,
                    const KlSockAddr *src, const KlSockAddr *local, void *ud) {
    (void)udp; (void)src; (void)ud;
    if (len > sizeof(g_buf)) len = sizeof(g_buf);
    memcpy(g_buf, data, len);
    g_len = len;
    g_has_local = (local != NULL);
    g_got++;
}

int main(void) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    if (kl_event_ctx_init(&ctx, &alloc) != 0) {
        fprintf(stderr, "smoke-udp: event ctx init failed\n");
        return 1;
    }

    KlUdp rx, tx;
    KlUdpConfig rc = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0,
                       .recv_pktinfo = 1 };
    KlUdpConfig tc = { .ctx = &ctx };
    if (kl_udp_init(&rx, &rc) != 0 || kl_udp_init(&tx, &tc) != 0) {
        fprintf(stderr, "smoke-udp: udp init failed\n");
        return 1;
    }
    if (kl_udp_recv_start(&rx, on_recv, NULL) != 0) {
        fprintf(stderr, "smoke-udp: recv_start failed\n");
        return 1;
    }

    uint16_t port = kl_udp_local_port(&rx);
    if (port == 0) {
        fprintf(stderr, "smoke-udp: local_port == 0\n");
        return 1;
    }

    KlSockAddr dst;
    unsigned char _ipb[4]; inet_pton(AF_INET, "127.0.0.1", _ipb); kl_sockaddr_from_ipv4(&dst, _ipb, port);

    if (kl_udp_send_to(&tx, SMOKE_MSG, sizeof(SMOKE_MSG) - 1,
                       &dst) != 0) {
        fprintf(stderr, "smoke-udp: send_to failed\n");
        return 1;
    }

    for (int i = 0; i < 200 && g_got < 1; i++)
        kl_event_ctx_run(&ctx, 16, 10);

    int ok = (g_got == 1 &&
              g_len == sizeof(SMOKE_MSG) - 1 &&
              memcmp(g_buf, SMOKE_MSG, sizeof(SMOKE_MSG) - 1) == 0);

    kl_udp_free(&tx);
    kl_udp_free(&rx);
    kl_event_ctx_free(&ctx);

    if (!ok) {
        fprintf(stderr, "smoke-udp: roundtrip FAILED (got=%d len=%zu)\n",
                g_got, g_len);
        return 1;
    }
    /* recv_pktinfo was requested; on platforms that support it (Linux/Windows
     * WSARecvMsg), the local address should have been recovered. macOS/BSD may
     * not deliver it for unconnected v4 — so this is reported, not asserted. */
    printf("smoke-udp: datagram roundtrip OK (local_addr=%s)\n",
           g_has_local ? "recovered" : "n/a");
    return 0;
}
