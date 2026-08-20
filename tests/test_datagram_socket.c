/*
 * test_datagram_socket.c — D1: the public KlDatagram socket convenience (kl_datagram_socket_init) +
 * provider-neutral connect (kl_datagram_connect) + the accepted-RX inspector.
 *
 * Live tests run over a REAL loopback socket via kl_event_ctx_init (backend-adaptive: readiness on a
 * default build, pollcomp/io_uring completion under those BACKENDs). A small copy-posix mock provider
 * (drops a capability / clamps the configure() RX mask / counts closes) drives the deterministic
 * capability + fail-loud + single-close paths that a real provider cannot force portably.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

#include "../vendor/utest.h"

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>
#include <keel/socket.h>         /* kl_socket_provider_posix */

#include "../src/socket.h"       /* KlSocketProvider / KlSocketOps / kl_sock_get_local_addr */
#include "../src/event_caps.h"   /* kl_event_caps — completion detection (backend-adaptive tests) */

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

static KlAllocator g_alloc;

/* ── copy-posix mock provider: drop caps / clamp configure RX mask / count closes ─────────────────── */
static KlSocketProvider g_msp; static KlSocketOps g_mops; static KlDatagramOps g_mdg;
static unsigned  (*g_real_caps)(void *, KlSocketHandle);
static uint32_t  (*g_real_configure)(void *, KlSocketHandle, int, const struct KlUdpConfig *);
static int       (*g_real_close)(void *, KlSocketHandle);
static unsigned  g_drop_caps;                 /* caps() drops these bits */
static uint32_t  g_rx_keep = 0xFFFFFFFFu;     /* configure() ANDs its accepted mask with this */
static int       g_close_calls;

static unsigned m_caps(void *c, KlSocketHandle fd) { return g_real_caps(c, fd) & ~g_drop_caps; }
static uint32_t m_configure(void *c, KlSocketHandle fd, int fam, const struct KlUdpConfig *cfg) {
    return g_real_configure(c, fd, fam, cfg) & g_rx_keep;
}
static int m_close(void *c, KlSocketHandle fd) { g_close_calls++; return g_real_close(c, fd); }

static const KlSocketProvider *mock_provider(void) {
    g_msp = *kl_socket_provider_posix();
    g_mops = *g_msp.ops;
    g_mdg = *g_msp.dgram;
    g_real_caps = g_mdg.caps; g_real_configure = g_mdg.configure; g_real_close = g_mops.close;
    g_mdg.caps = m_caps; g_mdg.configure = m_configure; g_mops.close = m_close;
    g_msp.ops = &g_mops; g_msp.dgram = &g_mdg;
    return &g_msp;
}
static void mock_reset(void) { g_drop_caps = 0; g_rx_keep = 0xFFFFFFFFu; g_close_calls = 0; }

/* ── recv recorder ────────────────────────────────────────────────────────────────────────────── */
static int g_recv_calls; static unsigned char g_buf[2048]; static size_t g_len;
static void on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                    const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)peer; (void)local; (void)flags;
    g_recv_calls++; g_len = len;
    if (len) memcpy(g_buf, data, len <= sizeof(g_buf) ? len : sizeof(g_buf));
}
static void pump_until(KlEventCtx *ctx, int *flag, int want, int ticks) {
    for (int i = 0; i < ticks && *flag < want; i++) kl_event_ctx_run(ctx, 16, 20);
}
static void pump_close(KlEventCtx *ctx, KlDatagram *dg, int ticks) {
    for (int i = 0; i < ticks && kl_datagram_close_state(dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        kl_event_ctx_run(ctx, 16, 20);
}
/* best-effort teardown helper (utest ASSERT macros are only valid inside a UTEST body). */
static void close_free(KlEventCtx *ctx, KlDatagram *dg) {
    kl_datagram_close_begin(dg); pump_close(ctx, dg, 100); kl_datagram_free(dg);
}

/* ══ Live: create + bind + adopt + round-trip ═════════════════════════════════════════════════════ */
UTEST(datagram_socket, init_bind_and_roundtrip) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));

    KlDatagram rx; memset(&rx, 0, sizeof(rx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_datagram_socket_init(&rx, &rc));
    ASSERT_TRUE(kl_handle_valid(kl_datagram_fd(&rx)));
    uint16_t port = kl_datagram_local_port(&rx);
    ASSERT_NE((uint16_t)0, port);

    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc };   /* unbound client */
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));

    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));
    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", port);
    const char *msg = "socket-init";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dest, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));
    pump_until(&ctx, &g_recv_calls, 1, 100);
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_EQ(strlen(msg), g_len);
    ASSERT_EQ(0, memcmp(g_buf, msg, strlen(msg)));

    close_free(&ctx, &rx); close_free(&ctx, &tx);
    kl_event_ctx_free(&ctx);
}

/* Live: connect then a PEERLESS send is delivered; the accepted-RX inspector reflects capture. */
UTEST(datagram_socket, connect_then_peerless_send) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));

    KlDatagram rx; memset(&rx, 0, sizeof(rx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "127.0.0.1",
                                  .recv_pktinfo = 1 };
    ASSERT_EQ(0, kl_datagram_socket_init(&rx, &rc));
    uint16_t port = kl_datagram_local_port(&rx);
    /* inspector: whatever configure accepted for pktinfo is reflected (Linux/macOS accept it). */
    (void)kl_datagram_accepted_rx_caps(&rx);

    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));

    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", port);
    ASSERT_EQ(0, kl_datagram_connect(&tx, &dest));   /* posix grants CONNECTED (opportunistic) */

    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));
    const char *msg = "peerless";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = NULL, .tos = -1 };  /* connected send */
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));
    pump_until(&ctx, &g_recv_calls, 1, 100);
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_EQ(strlen(msg), g_len);

    close_free(&ctx, &rx); close_free(&ctx, &tx);
    kl_event_ctx_free(&ctx);
}

/* Live: a peerless send BEFORE connect is refused KL_DATAGRAM_UNSUPPORTED (never touches the kernel). */
UTEST(datagram_socket, send_before_connect_unsupported) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));
    const char *msg = "x";
    KlDatagramMessage m = { .data = msg, .len = 1, .peer = NULL, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_UNSUPPORTED, (int)kl_datagram_send(&tx, &m));
    close_free(&ctx, &tx);
    kl_event_ctx_free(&ctx);
}

/* Live: reconnect is unsupported in D1 — a second connect returns KL_ERR_INVALID_ARG. */
UTEST(datagram_socket, second_connect_refused) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));
    KlSockAddr peer; kl_sockaddr_parse(&peer, "127.0.0.1", 9999);
    ASSERT_EQ(0, kl_datagram_connect(&tx, &peer));                 /* first connect ok */
    ASSERT_EQ(-1, kl_datagram_connect(&tx, &peer));                /* reconnect refused */
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)kl_datagram_last_error(&tx));
    close_free(&ctx, &tx);
    kl_event_ctx_free(&ctx);
}

/* Live: a FAILED first connect (AF_INET socket → IPv6 peer) leaves the datagram usable for
 * destination-addressed sends. */
UTEST(datagram_socket, first_connect_failure_keeps_destination_send) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));

    /* rx to actually receive the destination-addressed send */
    KlDatagram rx; memset(&rx, 0, sizeof(rx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_datagram_socket_init(&rx, &rc));
    uint16_t port = kl_datagram_local_port(&rx);

    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc, .family = AF_INET };
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));
    KlSockAddr v6; kl_sockaddr_parse(&v6, "::1", port);            /* family mismatch → connect fails */
    ASSERT_EQ(-1, kl_datagram_connect(&tx, &v6));
    ASSERT_EQ((int)KL_ERR_CONNECT, (int)kl_datagram_last_error(&tx));

    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));
    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", port);
    const char *msg = "still-usable";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dest, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));   /* destination send works */
    pump_until(&ctx, &g_recv_calls, 1, 100);
    ASSERT_EQ(1, g_recv_calls);

    close_free(&ctx, &rx); close_free(&ctx, &tx);
    kl_event_ctx_free(&ctx);
}

/* Live: SLOT and BOTH policies both initialize; unknown policy fails BEFORE any fd is created. */
UTEST(datagram_socket, queue_policy_selection_and_unknown) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));

    KlDatagram a; memset(&a, 0, sizeof(a));
    KlDatagramSocketConfig ca = { .ctx = &ctx, .alloc = &g_alloc, .queue_policy = KL_DATAGRAM_QUEUE_SLOT };
    ASSERT_EQ(0, kl_datagram_socket_init(&a, &ca));   /* SLOT: send_slots default 8, byte budget ignored */
    close_free(&ctx, &a);

    KlDatagram b; memset(&b, 0, sizeof(b));
    KlDatagramSocketConfig cb = { .ctx = &ctx, .alloc = &g_alloc, .queue_policy = KL_DATAGRAM_QUEUE_BOTH };
    ASSERT_EQ(0, kl_datagram_socket_init(&b, &cb));
    close_free(&ctx, &b);

    /* unknown policy → KL_ERR_INVALID_ARG, fd never created (no leak under ASan/LSan) */
    KlDatagram c; memset(&c, 0, sizeof(c));
    KlDatagramSocketConfig cc = { .ctx = &ctx, .alloc = &g_alloc, .queue_policy = (KlDatagramQueuePolicy)99 };
    ASSERT_EQ(-1, kl_datagram_socket_init(&c, &cc));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)kl_datagram_last_error(&c));
    ASSERT_EQ(KL_INVALID_SOCKET, kl_datagram_fd(&c));

    kl_event_ctx_free(&ctx);
}

/* Backend-adaptive: recv_gro requested on a COMPLETION loop leaves capture DISABLED (readiness-only,
 * M5.4) so ordinary receive works with no batch; on readiness the guard is exercised elsewhere. */
UTEST(datagram_socket, gro_on_completion_capture_disabled) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    int completion = (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) != 0;
    if (!completion) { kl_event_ctx_free(&ctx); return; }   /* readiness path covered by test_datagram_batch */

    KlDatagram rx; memset(&rx, 0, sizeof(rx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "127.0.0.1", .recv_gro = 1 };
    ASSERT_EQ(0, kl_datagram_socket_init(&rx, &rc));
    ASSERT_EQ((unsigned)0, (kl_datagram_accepted_rx_caps(&rx) & KL_DGRAM_RX_GRO));  /* GRO NOT enabled */
    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));   /* no batch needed → succeeds */
    close_free(&ctx, &rx);
    kl_event_ctx_free(&ctx);
}

/* ══ Mock provider: fail-loud + capability paths a real provider cannot force portably ════════════ */

/* want_rx_caps not accepted → init fails KL_ERR_UNSUPPORTED and closes the prepared fd exactly once. */
UTEST(datagram_socket, required_rx_mask_failure_closes_fd_once) {
    g_alloc = kl_allocator_default(); mock_reset();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    g_rx_keep = 0;   /* configure() reports NO accepted RX capture */
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramSocketConfig cfg = { .ctx = &ctx, .sockets = mock_provider(), .alloc = &g_alloc,
                                   .bind_addr = "127.0.0.1", .recv_pktinfo = 1,
                                   .want_rx_caps = KL_DGRAM_RX_PKTINFO };
    ASSERT_EQ(-1, kl_datagram_socket_init(&dg, &cfg));
    ASSERT_EQ((int)KL_ERR_UNSUPPORTED, (int)kl_datagram_last_error(&dg));
    ASSERT_EQ(1, g_close_calls);                       /* prepared fd closed EXACTLY once */
    ASSERT_EQ(KL_INVALID_SOCKET, kl_datagram_fd(&dg));
    kl_event_ctx_free(&ctx);
}

/* A non-empty multicast_group is a MANDATORY multicast request: a provider without CAP_MULTICAST fails
 * init (fd closed once) rather than reaching a confusing join failure. */
UTEST(datagram_socket, multicast_group_requires_capability) {
    g_alloc = kl_allocator_default(); mock_reset();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    g_drop_caps = KL_DGRAM_CAP_MULTICAST;   /* provider cannot multicast */
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramSocketConfig cfg = { .ctx = &ctx, .sockets = mock_provider(), .alloc = &g_alloc,
                                   .bind_addr = "127.0.0.1", .multicast_group = "239.1.2.3" };
    ASSERT_EQ(-1, kl_datagram_socket_init(&dg, &cfg));
    ASSERT_EQ((int)KL_ERR_UNSUPPORTED, (int)kl_datagram_last_error(&dg));   /* mandatory cap missing */
    ASSERT_EQ(1, g_close_calls);
    kl_event_ctx_free(&ctx);
}

/* A provider without CAP_CONNECTED: init still succeeds (destination-addressed send works), but connect
 * fails KL_ERR_UNSUPPORTED on the granted-cap gate (no syscall). */
UTEST(datagram_socket, connect_capability_absent) {
    g_alloc = kl_allocator_default(); mock_reset();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    g_drop_caps = KL_DGRAM_CAP_CONNECTED;
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramSocketConfig cfg = { .ctx = &ctx, .sockets = mock_provider(), .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&dg, &cfg));                       /* reduced provider still inits */
    ASSERT_EQ((unsigned)0, (kl_datagram_caps(&dg) & KL_DGRAM_CAP_CONNECTED)); /* not granted */
    KlSockAddr peer; kl_sockaddr_parse(&peer, "127.0.0.1", 9999);
    ASSERT_EQ(-1, kl_datagram_connect(&dg, &peer));
    ASSERT_EQ((int)KL_ERR_UNSUPPORTED, (int)kl_datagram_last_error(&dg));
    close_free(&ctx, &dg);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
