/*
 * test_datagram_socket.c: the public KlDatagram socket convenience (kl_datagram_socket_init) +
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
#include <keel/datagram_batch.h>   /* KlDgramTxDesc / send_batch / send_gso: peerless gate */
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>
#include <keel/socket.h>         /* kl_socket_provider_posix */

#include "../src/socket.h"       /* KlSocketProvider / KlSocketOps / kl_sock_get_local_addr */
#include "../src/event_caps.h"   /* kl_event_caps: completion detection (backend-adaptive tests) */

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>    /* inet_pton: source-pinned-reply test (Linux) */

static KlAllocator g_alloc;

/* ── copy-posix mock provider: drop caps / clamp configure RX mask / count closes ─────────────────── */
static KlSocketProvider g_msp; static KlSocketOps g_mops; static KlDatagramOps g_mdg;
static unsigned  (*g_real_caps)(void *, KlSocketHandle);
static uint32_t  (*g_real_configure)(void *, KlSocketHandle, int, const struct KlDatagramSocketConfig *);
static int       (*g_real_close)(void *, KlSocketHandle);
static int       (*g_real_connect)(void *, KlSocketHandle, const KlSockAddr *);
static unsigned  g_drop_caps;                 /* caps() drops these bits */
static uint32_t  g_rx_keep = 0xFFFFFFFFu;     /* configure() ANDs its accepted mask with this */
static int       g_close_calls, g_configure_calls, g_connect_calls;

static unsigned m_caps(void *c, KlSocketHandle fd) { return g_real_caps(c, fd) & ~g_drop_caps; }
static uint32_t m_configure(void *c, KlSocketHandle fd, int fam, const struct KlDatagramSocketConfig *cfg) {
    g_configure_calls++; return g_real_configure(c, fd, fam, cfg) & g_rx_keep;
}
static int m_close(void *c, KlSocketHandle fd) { g_close_calls++; return g_real_close(c, fd); }
static int m_connect(void *c, KlSocketHandle fd, const KlSockAddr *a) { g_connect_calls++; return g_real_connect(c, fd, a); }

static const KlSocketProvider *mock_provider(void) {
    g_msp = *kl_socket_provider_posix();
    g_mops = *g_msp.ops;
    g_mdg = *g_msp.dgram;
    g_real_caps = g_mdg.caps; g_real_configure = g_mdg.configure; g_real_close = g_mops.close;
    g_real_connect = g_mops.connect;
    g_mdg.caps = m_caps; g_mdg.configure = m_configure; g_mops.close = m_close; g_mops.connect = m_connect;
    g_msp.ops = &g_mops; g_msp.dgram = &g_mdg;
    return &g_msp;
}
static void mock_reset(void) {
    g_drop_caps = 0; g_rx_keep = 0xFFFFFFFFu; g_close_calls = g_configure_calls = g_connect_calls = 0;
}

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

/* Live: reconnect is unsupported; a second connect returns KL_ERR_INVALID_ARG. */
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

/* Backend-adaptive: recv_gro requested on a COMPLETION loop leaves capture DISABLED (readiness-only)
 * so ordinary receive works with no batch; on readiness the guard is exercised elsewhere. */
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

/* cfg->sockets == NULL means the EVENT CONTEXT's provider, not the built-in default. A custom
 * provider on the ctx must be threaded through open/adopt/close. */
UTEST(datagram_socket, context_provider_threaded_when_sockets_null) {
    g_alloc = kl_allocator_default(); mock_reset();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    ctx.sockets = mock_provider();   /* the event context carries a custom provider */
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramSocketConfig cfg = { .ctx = &ctx, .sockets = NULL, .alloc = &g_alloc, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_datagram_socket_init(&dg, &cfg));
    ASSERT_TRUE(g_configure_calls > 0);   /* the CTX provider's configure ran: not the built-in default */
    close_free(&ctx, &dg);
    ASSERT_TRUE(g_close_calls > 0);        /* and its close op ran at teardown */
    kl_event_ctx_free(&ctx);
}

/* The actual-connected state gates ALL send admission paths (single/batch/GSO), not just the single
 * send. A peerless batch or GSO send before connect is refused; after connect it is admitted. */
UTEST(datagram_socket, peerless_batch_and_gso_require_connect) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram rx; memset(&rx, 0, sizeof(rx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_datagram_socket_init(&rx, &rc));
    uint16_t port = kl_datagram_local_port(&rx);
    KlDatagram tx; memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));

    KlDatagramBatch *sb = kl_datagram_batch_create(&tx, KL_DGRAM_BATCH_SEND, 4, 1500);
    ASSERT_TRUE(sb != NULL);
    KlDgramTxDesc d; memset(&d, 0, sizeof(d)); d.data = "x"; d.len = 1; d.tos = -1;  /* dest UNSPEC = peerless */
    char gbuf[4] = { 'a','b','c','d' };

    /* BEFORE connect: peerless batch + GSO both refused (never reach the provider). */
    KlDatagramSendStatus stop = KL_DATAGRAM_ACCEPTED;
    ASSERT_EQ(0, kl_datagram_send_batch(&tx, sb, &d, 1, &stop));         /* nothing accepted */
    ASSERT_EQ((int)KL_DATAGRAM_UNSUPPORTED, (int)stop);
    ASSERT_EQ((int)KL_DATAGRAM_UNSUPPORTED, (int)kl_datagram_send_gso(&tx, sb, gbuf, 4, 2, NULL));

    /* connect, then the SAME peerless sends are admitted. */
    KlSockAddr dest; kl_sockaddr_parse(&dest, "127.0.0.1", port);
    ASSERT_EQ(0, kl_datagram_connect(&tx, &dest));
    stop = KL_DATAGRAM_ERROR;
    ASSERT_EQ(1, kl_datagram_send_batch(&tx, sb, &d, 1, &stop));         /* admitted */
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)stop);
    ASSERT_NE((int)KL_DATAGRAM_UNSUPPORTED, (int)kl_datagram_send_gso(&tx, sb, gbuf, 4, 2, NULL));

    /* drain the send queue (incl. the GSO group) so the caller-owned batch frees cleanly. */
    for (int i = 0; i < 60; i++) {
        if (kl_datagram_batch_free(sb) == 0) { sb = NULL; break; }
        kl_event_ctx_run(&ctx, 8, 10);
    }
    ASSERT_TRUE(sb == NULL);
    close_free(&ctx, &rx); close_free(&ctx, &tx);
    kl_event_ctx_free(&ctx);
}

/* Connect must be refused once close has begun: without issuing a provider connect syscall. */
UTEST(datagram_socket, connect_after_close_begin_refused_no_syscall) {
    g_alloc = kl_allocator_default(); mock_reset();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramSocketConfig cfg = { .ctx = &ctx, .sockets = mock_provider(), .alloc = &g_alloc,
                                   .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_datagram_socket_init(&dg, &cfg));
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));   /* an in-flight recv → CLOSING on completion */
    ASSERT_EQ(0, kl_datagram_close_begin(&dg));                 /* close begun (CLOSING or CLOSED) */
    KlSockAddr peer; kl_sockaddr_parse(&peer, "127.0.0.1", 9999);
    ASSERT_EQ(-1, kl_datagram_connect(&dg, &peer));            /* refused: not OPEN */
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)kl_datagram_last_error(&dg));
    ASSERT_EQ(0, g_connect_calls);                             /* no provider connect syscall */
    pump_close(&ctx, &dg, 100); kl_datagram_free(&dg);
    kl_event_ctx_free(&ctx);
}

/* ══ Datagram socket unicast + sockopt live coverage (not exercised above) ═══════════════════════ */

/* recv_stop halts delivery. */
UTEST(datagram_socket, recv_stop_halts_delivery) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "127.0.0.1" };
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&rx, &rc));
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));
    uint16_t port = kl_datagram_local_port(&rx);
    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));
    kl_datagram_recv_stop(&rx);
    KlSockAddr dst; kl_sockaddr_parse(&dst, "127.0.0.1", port);
    KlDatagramMessage m = { .data = "ping", .len = 4, .peer = &dst, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));
    for (int i = 0; i < 30; i++) kl_event_ctx_run(&ctx, 8, 10);   /* short pump */
    ASSERT_EQ(0, g_recv_calls);                                   /* nothing delivered while stopped */
    close_free(&ctx, &rx); close_free(&ctx, &tx);
    kl_event_ctx_free(&ctx);
}

/* Oversized datagram delivered truncated + counted. */
static int g_trunc_flag;
static void on_recv_trunc(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                          const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)data; (void)peer; (void)local;
    g_recv_calls++; g_len = len; g_trunc_flag = (flags & KL_DGRAM_TRUNCATED) != 0;
}
UTEST(datagram_socket, truncation_counted) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "127.0.0.1", .recv_cap = 4 };
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&rx, &rc));
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));
    uint16_t port = kl_datagram_local_port(&rx);
    g_recv_calls = 0; g_trunc_flag = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv_trunc, NULL));
    KlSockAddr dst; kl_sockaddr_parse(&dst, "127.0.0.1", port);
    KlDatagramMessage m = { .data = "abcdefgh", .len = 8, .peer = &dst, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));
    pump_until(&ctx, &g_recv_calls, 1, 200);
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_TRUE(g_len <= 4);                        /* captured prefix, not overflowed */
    ASSERT_TRUE(g_trunc_flag);                      /* delivered with the TRUNCATED flag */
    ASSERT_EQ((uint64_t)1, kl_datagram_truncated(&rx));
    close_free(&ctx, &rx); close_free(&ctx, &tx);
    kl_event_ctx_free(&ctx);
}

/* A small so_rcvbuf/so_sndbuf shrinks the kernel buffers. */
UTEST(datagram_socket, so_bufsize_applied) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram def, small; memset(&def, 0, sizeof(def)); memset(&small, 0, sizeof(small));
    KlDatagramSocketConfig dc = { .ctx = &ctx, .alloc = &g_alloc };
    KlDatagramSocketConfig sc = { .ctx = &ctx, .alloc = &g_alloc, .so_rcvbuf = 8192, .so_sndbuf = 8192 };
    ASSERT_EQ(0, kl_datagram_socket_init(&def, &dc));
    ASSERT_EQ(0, kl_datagram_socket_init(&small, &sc));
    int drb = 0, srb = 0, dsb = 0, ssb = 0; socklen_t l = sizeof(int);
    ASSERT_EQ(0, getsockopt((int)kl_datagram_fd(&def),   SOL_SOCKET, SO_RCVBUF, (char *)&drb, &l));
    ASSERT_EQ(0, getsockopt((int)kl_datagram_fd(&small), SOL_SOCKET, SO_RCVBUF, (char *)&srb, &l));
    ASSERT_EQ(0, getsockopt((int)kl_datagram_fd(&def),   SOL_SOCKET, SO_SNDBUF, (char *)&dsb, &l));
    ASSERT_EQ(0, getsockopt((int)kl_datagram_fd(&small), SOL_SOCKET, SO_SNDBUF, (char *)&ssb, &l));
    ASSERT_TRUE(srb < drb);                          /* shrunk below default (deterministic across OSes) */
    ASSERT_TRUE(ssb < dsb);
    close_free(&ctx, &def); close_free(&ctx, &small);
    kl_event_ctx_free(&ctx);
}

/* A wildcard-bound datagram that REQUIRES source-pinned replies fails loud on a provider without
 * SOURCE_PIN. */
UTEST(datagram_socket, wildcard_source_pin_cap_gate) {
    g_alloc = kl_allocator_default(); mock_reset();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    g_drop_caps = KL_DGRAM_CAP_SOURCE_PIN;   /* provider cannot source-pin */
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramSocketConfig cfg = { .ctx = &ctx, .sockets = mock_provider(), .alloc = &g_alloc,
                                   .bind_addr = "0.0.0.0", .recv_pktinfo = 1,
                                   .want_caps = KL_DGRAM_CAP_SOURCE_PIN };
    ASSERT_EQ(-1, kl_datagram_socket_init(&dg, &cfg));   /* required send-side cap missing */
    ASSERT_EQ((int)KL_ERR_UNSUPPORTED, (int)kl_datagram_last_error(&dg));
    ASSERT_EQ(1, g_close_calls);                          /* prepared fd closed once */
    kl_event_ctx_free(&ctx);
}

/* Source-pinned reply: a wildcard-bound datagram captures the datagram's local (hit) address on recv
 * and replies FROM it, so the reply egresses from the address the client contacted: the source-pin
 * convenience expressed directly on KlDatagram. Linux-only: uses the 127.0.0.0/8 loopback range. */
#if defined(__linux__)
static KlSockAddr g_srv_local; static int g_srv_have_local; static KlSockAddr g_srv_peer;
static KlDatagram *g_srv_dg;
static void srv_on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                        const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)data; (void)len;
    g_srv_have_local = (local != NULL) && (flags & KL_DGRAM_HAS_LOCAL);
    if (g_srv_have_local) g_srv_local = *local;
    g_srv_peer = *peer;
    /* reply FROM the hit local address (source-pin) back to the sender */
    KlDatagramMessage m = { .data = "pong", .len = 4, .peer = &g_srv_peer,
                            .local = g_srv_have_local ? &g_srv_local : NULL, .tos = -1 };
    (void)kl_datagram_send(g_srv_dg, &m);
}
static char g_cli_src_ip[64]; static int g_cli_got;
static void cli_on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                        const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)data; (void)len; (void)local; (void)flags;
    g_cli_got++; if (peer) kl_sockaddr_format_ip(peer, g_cli_src_ip, sizeof(g_cli_src_ip));
}
UTEST(datagram_socket, source_pinned_reply_from_hit_address) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram srv; memset(&srv, 0, sizeof(srv));
    KlDatagramSocketConfig sc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "0.0.0.0",
                                  .recv_pktinfo = 1, .want_caps = KL_DGRAM_CAP_SOURCE_PIN,
                                  .want_rx_caps = KL_DGRAM_RX_PKTINFO };
    ASSERT_EQ(0, kl_datagram_socket_init(&srv, &sc));
    uint16_t port = kl_datagram_local_port(&srv);
    g_srv_dg = &srv;
    ASSERT_EQ(0, kl_datagram_recv_start(&srv, srv_on_recv, NULL));

    KlDatagram cli; memset(&cli, 0, sizeof(cli));
    KlDatagramSocketConfig cc = { .ctx = &ctx, .alloc = &g_alloc };
    ASSERT_EQ(0, kl_datagram_socket_init(&cli, &cc));
    g_cli_got = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&cli, cli_on_recv, NULL));

    KlSockAddr dst; unsigned char ip[4]; inet_pton(AF_INET, "127.0.0.2", ip);
    kl_sockaddr_from_ipv4(&dst, ip, port);           /* hit a non-primary loopback address */
    KlDatagramMessage m = { .data = "ping", .len = 4, .peer = &dst, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&cli, &m));
    pump_until(&ctx, &g_cli_got, 1, 300);
    ASSERT_EQ(1, g_cli_got);
    ASSERT_STREQ("127.0.0.2", g_cli_src_ip);          /* reply egressed FROM the hit address */

    close_free(&ctx, &srv); close_free(&ctx, &cli);
    kl_event_ctx_free(&ctx);
}
#endif /* __linux__ */

/* Live IPv6 unicast construction + roundtrip. */
UTEST(datagram_socket, ipv6_roundtrip) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram rx, tx; memset(&rx, 0, sizeof(rx)); memset(&tx, 0, sizeof(tx));
    KlDatagramSocketConfig rc = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "::1" };
    if (kl_datagram_socket_init(&rx, &rc) != 0) { kl_event_ctx_free(&ctx); UTEST_SKIP("IPv6 unavailable"); }
    KlDatagramSocketConfig tc = { .ctx = &ctx, .alloc = &g_alloc, .family = AF_INET6 };
    ASSERT_EQ(0, kl_datagram_socket_init(&tx, &tc));
    uint16_t port = kl_datagram_local_port(&rx);
    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&rx, on_recv, NULL));
    KlSockAddr dst; kl_sockaddr_parse(&dst, "::1", port);
    const char *msg = "v6-hello";
    KlDatagramMessage m = { .data = msg, .len = strlen(msg), .peer = &dst, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&tx, &m));
    pump_until(&ctx, &g_recv_calls, 1, 100);
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_EQ(strlen(msg), g_len);
    close_free(&ctx, &rx); close_free(&ctx, &tx);
    kl_event_ctx_free(&ctx);
}

/* SO_REUSEPORT shared bind. */
UTEST(datagram_socket, reuse_port_shared_bind) {
#ifdef SO_REUSEPORT
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram a; memset(&a, 0, sizeof(a));
    KlDatagramSocketConfig ca = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "127.0.0.1", .reuse_port = 1 };
    ASSERT_EQ(0, kl_datagram_socket_init(&a, &ca));
    uint16_t port = kl_datagram_local_port(&a);
    KlDatagram b; memset(&b, 0, sizeof(b));
    KlDatagramSocketConfig cb = { .ctx = &ctx, .alloc = &g_alloc, .bind_addr = "127.0.0.1",
                                  .bind_port = port, .reuse_port = 1 };
    ASSERT_EQ(0, kl_datagram_socket_init(&b, &cb));   /* shares the port: SO_REUSEPORT */
    int ra = 0, rb = 0; socklen_t l = sizeof(int);
    ASSERT_EQ(0, getsockopt((int)kl_datagram_fd(&a), SOL_SOCKET, SO_REUSEPORT, (char *)&ra, &l));
    ASSERT_EQ(0, getsockopt((int)kl_datagram_fd(&b), SOL_SOCKET, SO_REUSEPORT, (char *)&rb, &l));
    ASSERT_TRUE(ra != 0); ASSERT_TRUE(rb != 0);
    close_free(&ctx, &a); close_free(&ctx, &b);
    kl_event_ctx_free(&ctx);
#else
    UTEST_SKIP("SO_REUSEPORT unavailable");
#endif
}

/* SO_BROADCAST gated by the broadcast flag (deterministic getsockopt check: a live
 * send-to-broadcast probe is environment-sensitive, so verify the sockopt was applied). */
UTEST(datagram_socket, broadcast_flag_applied) {
    g_alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &g_alloc));
    KlDatagram off, on; memset(&off, 0, sizeof(off)); memset(&on, 0, sizeof(on));
    KlDatagramSocketConfig co = { .ctx = &ctx, .alloc = &g_alloc };                    /* broadcast off */
    KlDatagramSocketConfig cb = { .ctx = &ctx, .alloc = &g_alloc, .broadcast = 1 };    /* broadcast on */
    ASSERT_EQ(0, kl_datagram_socket_init(&off, &co));
    ASSERT_EQ(0, kl_datagram_socket_init(&on, &cb));
    int bo = -1, bn = -1; socklen_t l = sizeof(int);
    ASSERT_EQ(0, getsockopt((int)kl_datagram_fd(&off), SOL_SOCKET, SO_BROADCAST, (char *)&bo, &l));
    ASSERT_EQ(0, getsockopt((int)kl_datagram_fd(&on),  SOL_SOCKET, SO_BROADCAST, (char *)&bn, &l));
    ASSERT_EQ(0, bo);            /* default: broadcast disabled */
    ASSERT_TRUE(bn != 0);        /* broadcast=1 → SO_BROADCAST set */
    close_free(&ctx, &off); close_free(&ctx, &on);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
