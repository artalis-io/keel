/*
 * test_datagram_live.c — the public KlDatagram over a REAL event loop + REAL loopback UDP (7B-4).
 *
 * The first LIVE backend binding: no mock. `kl_event_ctx_init` yields whatever the build's backend is —
 * a pollcomp COMPLETION loop under BACKEND=pollcomp (the 7B-4 target), a kqueue/epoll READINESS loop on
 * a default build — and the facade's §2.5 adapter builder selects the matching path. The caller prepares
 * the fd through the socket seam (KlDatagram contract), then does a real send→recv round-trip over
 * 127.0.0.1 and a clean close, so the live completion/readiness adapters are exercised end to end.
 *
 * sockets = NULL → the default POSIX provider; pollcomp drives overlapped ops on the ordinary fd, so no
 * provider plumbing is needed. The same test therefore runs green on both the completion and readiness
 * gates (the readiness §10 rows are only FLIPPED in 7B-6, but the adapter working here is a bonus).
 */

#include "../vendor/utest.h"

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>
#include <keel/udp.h>            /* KlUdpConfig — the dgram provider configure() arg */

#include "../src/socket.h"       /* kl_sock_socket / _bind / _set_nonblocking / _get_local_addr */

#include <string.h>
#include <sys/socket.h>

static KlAllocator g_alloc;

/* recv recorder */
static int g_recv_calls; static unsigned char g_buf[2048]; static size_t g_len; static int g_has_peer;
static void on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                    const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)local; (void)flags;
    g_recv_calls++; g_len = len; g_has_peer = (peer != NULL);
    if (len) memcpy(g_buf, data, len <= sizeof(g_buf) ? len : sizeof(g_buf));
}

/* prepare a datagram fd through the socket seam (socket + nonblocking + configure + optional bind) */
static KlSocketHandle prep_fd(const KlSocketProvider *sp, const char *bind_ip, int port) {
    KlSocketHandle fd = kl_sock_socket(sp, AF_INET, SOCK_DGRAM, 0);
    if (!kl_handle_valid(fd)) return fd;
    kl_sock_set_nonblocking(sp, fd);
    KlUdpConfig ucfg; memset(&ucfg, 0, sizeof(ucfg));
    const KlDatagramOps *dg = sp ? sp->dgram : kl_sockdef_dgram();
    (void)dg->configure(sp ? sp->context : NULL, fd, AF_INET, &ucfg);   /* best-effort sockopts */
    if (bind_ip) {
        KlSockAddr b; kl_sockaddr_parse(&b, bind_ip, (uint16_t)port);
        if (kl_sock_bind(sp, fd, &b) != 0) { kl_sock_close(sp, fd); return KL_INVALID_SOCKET; }
    }
    return fd;
}

static void pump_until(KlEventCtx *ctx, int *flag, int want, int ticks) {
    for (int i = 0; i < ticks && *flag < want; i++) kl_event_ctx_run(ctx, 16, 20);
}
static void pump_close(KlEventCtx *ctx, KlDatagram *dg, int ticks) {
    for (int i = 0; i < ticks && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 20);
}

UTEST(datagram_live, loopback_roundtrip_and_clean_close) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    const KlSocketProvider *sp = ctx.sockets;   /* NULL = default POSIX (pollcomp drives it overlapped) */

    KlSocketHandle rxfd = prep_fd(sp, "127.0.0.1", 0);
    ASSERT_TRUE(kl_handle_valid(rxfd));
    KlSockAddr local; ASSERT_EQ(0, kl_sock_get_local_addr(sp, rxfd, &local));
    int port = (int)kl_sockaddr_port(&local);
    ASSERT_NE(0, port);
    KlSocketHandle txfd = prep_fd(sp, NULL, 0);
    ASSERT_TRUE(kl_handle_valid(txfd));

    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .sockets = sp, .fd = rxfd,
                            .send_slots = 4, .send_slot_cap = 1500, .recv_cap = 2048 };
    KlDatagramConfig tc = rc; tc.fd = txfd;
    ASSERT_EQ(0, kl_datagram_init(&rx, &rc));
    ASSERT_EQ(0, kl_datagram_init(&tx, &tc));

    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", (uint16_t)port);
    const char *msg = "hello-datagram";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dest, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));

    pump_until(&ctx, &g_recv_calls, 1, 100);
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_EQ(strlen(msg), g_len);
    ASSERT_TRUE(g_has_peer);
    ASSERT_EQ(0, memcmp(g_buf, msg, strlen(msg)));

    /* clean close: graceful → drain → retire the outstanding recv → close the fd once → DETACHED */
    ASSERT_EQ(0, kl_datagram_close_begin(&rx));
    pump_close(&ctx, &rx, 100);
    ASSERT_EQ((int)KL_DGRAM_CLOSE_CLOSED, (int)kl_datagram_close_state(&rx));
    ASSERT_EQ((int)KL_DGRAM_DETACHED, (int)kl_datagram_close_result(&rx));
    ASSERT_EQ(0, kl_datagram_free(&rx));

    ASSERT_EQ(0, kl_datagram_close_begin(&tx));
    pump_close(&ctx, &tx, 100);
    ASSERT_EQ((int)KL_DGRAM_CLOSE_CLOSED, (int)kl_datagram_close_state(&tx));
    ASSERT_EQ(0, kl_datagram_free(&tx));

    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
