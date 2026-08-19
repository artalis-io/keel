/*
 * test_udp_server.c — KlUdpServer dispatch conformance.
 *
 * M4: KlUdpServer is re-implemented on the PUBLIC KlDatagram core (M0 prep + M1 BOTH send policy) + the
 * M2 extended layer (source-pin caps + multicast) — see docs/datagram_m4_udpserver_design.md. Its
 * receive rides the Tier-1 serial-receive machine (one datagram per handler call over the dedicated
 * inbound slot); its reply rides the fixed-slot atomic send with the BOTH byte-gate; teardown is the
 * confirmed-detachment / Option-A synchronous close. These cases exercise the server's couplings:
 * serial multi-datagram one-packet-one-callback dispatch (no coalescing/substitution/dup/loss across
 * the re-arm); source on every recv with reply-to-each-sender; (Linux) wildcard source-pin + a
 * getsockopt-verified SO_REUSEPORT shared bind; the M4-specifics — a wildcard init that FAILS LOUD +
 * closes prep.fd exactly once when a required cap (send SOURCE_PIN or RX pktinfo) is unavailable, the
 * reentrant free-from-handler lifecycle; and the M5.4 recv batch/GRO opt-in — mmsg_batch/recv_gro
 * attach a RECV KlDatagramBatch on a readiness+capable provider (per-segment GRO split still one
 * handler call each), with the completion single-flight fallback (no batch). Being backend-agnostic (a
 * default KlEventCtx loop), the suite runs on readiness (kqueue/epoll/wsapoll/poll) AND completion
 * (pollcomp/io_uring/IOCP) backends — one file, both paths.
 *
 * SPDX-License-Identifier: MIT
 */
#include "utest.h"
#include <keel/udp_server.h>
#include <keel/udp.h>
#include <keel/datagram.h>       /* KlDatagramOps + the KL_DGRAM_CAP_ and KL_DGRAM_RX_ bits — cap-gate mock */
#include <keel/datagram_detail.h>/* M5.4: whitebox — the attached RECV batch (dg.core->ext) */
#include <keel/socket.h>         /* KlSocketProvider + kl_socket_provider_posix — delegating mock */
#include <keel/event_ctx.h>
#include "../src/datagram_core.h"/* M5.4: KlDgramCore (core->ext) */
#include "../src/event_caps.h"   /* M5.4: kl_event_caps — completion vs readiness */
#include <keel/allocator.h>
#include <string.h>
#include <stdio.h>
#include "net_compat.h"

/* ── Server side: echo handler ───────────────────────────────────────── */

static int g_srv_hits;
static char g_srv_src_ip[INET6_ADDRSTRLEN];

static void echo_handler(KlUdpServer *s, const void *data, size_t len,
                         const KlSockAddr *src, void *ud) {
    (void)ud;
    g_srv_hits++;
    g_srv_src_ip[0] = '\0';
    if (src && kl_sockaddr_family(src) == KL_AF_INET)
        inet_ntop(AF_INET, src->u.ip,
                  g_srv_src_ip, sizeof(g_srv_src_ip));
    kl_udp_server_reply(s, data, len, src);
}

/* ── Client side: capture ────────────────────────────────────────────── */

static char     g_cli_buf[512];
static size_t   g_cli_len;
static int      g_cli_got;
static char     g_cli_src_ip[INET6_ADDRSTRLEN];

static void cli_recv(KlUdp *u, const void *data, size_t len,
                     const KlSockAddr *src, const KlSockAddr *local, void *ud) {
    (void)u; (void)local; (void)ud;
    if (len > sizeof(g_cli_buf)) len = sizeof(g_cli_buf);
    memcpy(g_cli_buf, data, len);
    g_cli_len = len;
    g_cli_src_ip[0] = '\0';
    if (src && kl_sockaddr_family(src) == KL_AF_INET)
        inet_ntop(AF_INET, src->u.ip,
                  g_cli_src_ip, sizeof(g_cli_src_ip));
    g_cli_got++;
}

/* Per-payload capture for the serial multi-datagram test: each echoed datagram must be exactly
 * "srv-<i>" for a distinct 0<=i<N. Sets bit i in `mask`; a malformed payload OR a duplicate index
 * flips `bad`. The test then asserts bad==0 (no corruption/substitution/dup) and mask==(1<<N)-1
 * (every distinct message arrived) — stronger than an aggregate over the bytes. */
static unsigned g_multi_mask;
static int      g_multi_bad;
static void multi_recv(KlUdp *u, const void *data, size_t len,
                       const KlSockAddr *src, const KlSockAddr *local, void *ud) {
    (void)u; (void)src; (void)local; (void)ud;
    g_cli_got++;
    const char *p = data;
    if (len != 5 || memcmp(p, "srv-", 4) != 0 || p[4] < '0' || p[4] > '9') { g_multi_bad = 1; return; }
    unsigned bit = 1u << (p[4] - '0');
    if (g_multi_mask & bit) { g_multi_bad = 1; return; }   /* duplicate index */
    g_multi_mask |= bit;
}

static void reset(void) {
    g_srv_hits = 0; g_srv_src_ip[0] = '\0';
    g_cli_len = 0; g_cli_got = 0; memset(g_cli_buf, 0, sizeof(g_cli_buf));
    g_cli_src_ip[0] = '\0';
    g_multi_mask = 0; g_multi_bad = 0;
}

/* Per-client capture (independent state) for the two-sender source-pinning test. */
typedef struct { char buf[64]; size_t len; int got; } CliCap;
static void cli_cap_recv(KlUdp *u, const void *data, size_t len,
                         const KlSockAddr *src, const KlSockAddr *local, void *ud) {
    (void)u; (void)src; (void)local;
    CliCap *c = ud;
    if (len > sizeof(c->buf)) len = sizeof(c->buf);
    memcpy(c->buf, data, len);
    c->len = len;
    c->got++;
}

static void pump_until(KlEventCtx *ctx, int *flag, int want, int ticks) {
    for (int i = 0; i < ticks && *flag < want; i++)
        kl_event_ctx_run(ctx, 16, 10);
}

static void dest_v4(KlSockAddr *a, uint16_t port) {
    unsigned char _ipb[4]; inet_pton(AF_INET, "127.0.0.1", _ipb); kl_sockaddr_from_ipv4(a, _ipb, port);
}

/* ── Tests ───────────────────────────────────────────────────────────── */

UTEST(udp_server, echo_roundtrip) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    uint16_t port = kl_udp_server_local_port(&srv);
    ASSERT_TRUE(port > 0);

    KlUdp cli;
    KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, cli_recv, NULL));

    KlSockAddr dst;
    dest_v4(&dst, port);
    const char *msg = "discover";
    ASSERT_EQ(0, kl_udp_send_to(&cli, msg, strlen(msg),
                                &dst));

    /* client -> server (handler) -> reply -> client */
    pump_until(&ctx, &g_cli_got, 1, 300);

    ASSERT_EQ(1, g_srv_hits);
    ASSERT_STREQ("127.0.0.1", g_srv_src_ip);   /* server saw the client's addr */
    ASSERT_EQ(1, g_cli_got);
    ASSERT_EQ(strlen(msg), g_cli_len);
    ASSERT_EQ(0, memcmp(g_cli_buf, msg, g_cli_len));

    kl_udp_free(&cli);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

UTEST(udp_server, shared_loop_two_servers) {
    /* Two UDP servers on one event context — proves the shared-loop model. */
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer a, b;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    ASSERT_EQ(0, kl_udp_server_init(&a, &ctx, &sc, echo_handler, NULL));
    ASSERT_EQ(0, kl_udp_server_init(&b, &ctx, &sc, echo_handler, NULL));
    ASSERT_TRUE(kl_udp_server_local_port(&a) != kl_udp_server_local_port(&b));

    KlUdp cli;
    KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, cli_recv, NULL));

    KlSockAddr dst;
    dest_v4(&dst, kl_udp_server_local_port(&b));   /* hit server B */
    ASSERT_EQ(0, kl_udp_send_to(&cli, "hi", 2, &dst));
    pump_until(&ctx, &g_cli_got, 1, 300);

    ASSERT_EQ(1, g_srv_hits);        /* exactly one server handled it */
    ASSERT_EQ(1, g_cli_got);
    ASSERT_EQ((size_t)2, g_cli_len);

    kl_udp_free(&cli);
    kl_udp_server_free(&a);
    kl_udp_server_free(&b);
    kl_event_ctx_free(&ctx);
}

UTEST(udp_server, null_and_arg_guards) {
    ASSERT_EQ(-1, kl_udp_server_init(NULL, NULL, NULL, NULL, NULL));
    ASSERT_EQ(-1, kl_udp_server_reply(NULL, "x", 1, NULL));
    ASSERT_EQ((uint16_t)0, kl_udp_server_local_port(NULL));
    ASSERT_EQ(-1, kl_udp_server_fd(NULL));

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    /* NULL handler rejected. */
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1" };
    ASSERT_EQ(-1, kl_udp_server_init(&srv, &ctx, &sc, NULL, NULL));
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_udp_server_last_error(&srv));

    /* Valid init, then double-free safety. */
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    kl_udp_server_free(&srv);
    kl_udp_server_free(&srv);

    kl_event_ctx_free(&ctx);
}

/* Serial multi-datagram dispatch: N distinct datagrams ("srv-0".."srv-<N-1>") sent back-to-back are
 * each delivered as ONE handler call (one-packet-one-callback) and each echoed — none coalesced,
 * substituted, duplicated, or lost across the receive machine's re-arm. multi_recv validates each
 * payload byte-for-byte and records its index in a seen-set that rejects duplicates; the test then
 * asserts every distinct message arrived exactly once. Runs on readiness and completion alike. */
UTEST(udp_server, serial_multi_datagram) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    uint16_t port = kl_udp_server_local_port(&srv);

    KlUdp cli;
    KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, multi_recv, NULL));

    KlSockAddr dst;
    dest_v4(&dst, port);

    enum { N = 4 };
    for (int i = 0; i < N; i++) {
        char m[8];
        int mn = snprintf(m, sizeof(m), "srv-%d", i);   /* exactly 5 bytes, distinct trailing digit */
        ASSERT_EQ(0, kl_udp_send_to(&cli, m, (size_t)mn, &dst));
    }

    pump_until(&ctx, &g_cli_got, N, 400);

    ASSERT_EQ(N, g_srv_hits);                         /* each datagram = exactly one handler call */
    ASSERT_EQ(N, g_cli_got);                          /* each echo delivered separately */
    ASSERT_EQ(0, g_multi_bad);                        /* no corruption/substitution/duplicate */
    ASSERT_EQ((unsigned)((1u << N) - 1), g_multi_mask); /* every distinct message arrived exactly once */

    kl_udp_free(&cli);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

/* Source addr on every recv + reply-to-source from inside the handler: two clients on distinct ports
 * each send a distinct payload; the echo handler replies to EACH sender's own source (the borrowed
 * `src`), never crossing them. Exercises the per-datagram source captured through the serial machine
 * under back-to-back arrivals — the coupling KlUdpServer depends on for reply-to-sender. */
UTEST(udp_server, reply_to_distinct_sources) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    uint16_t port = kl_udp_server_local_port(&srv);

    CliCap capA = {0}, capB = {0};
    KlUdp a, b;
    KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&a, &cc));
    ASSERT_EQ(0, kl_udp_init(&b, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&a, cli_cap_recv, &capA));
    ASSERT_EQ(0, kl_udp_recv_start(&b, cli_cap_recv, &capB));

    KlSockAddr dst;
    dest_v4(&dst, port);
    ASSERT_EQ(0, kl_udp_send_to(&a, "AAA", 3, &dst));
    ASSERT_EQ(0, kl_udp_send_to(&b, "BBBB", 4, &dst));

    for (int i = 0; i < 400 && (capA.got < 1 || capB.got < 1); i++)
        kl_event_ctx_run(&ctx, 16, 10);

    ASSERT_EQ(2, g_srv_hits);
    ASSERT_EQ(1, capA.got);
    ASSERT_EQ((size_t)3, capA.len);
    ASSERT_EQ(0, memcmp(capA.buf, "AAA", 3));    /* A got its OWN payload back */
    ASSERT_EQ(1, capB.got);
    ASSERT_EQ((size_t)4, capB.len);
    ASSERT_EQ(0, memcmp(capB.buf, "BBBB", 4));    /* B got its OWN payload — no cross-reply */

    kl_udp_free(&a);
    kl_udp_free(&b);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

#if defined(__linux__)
/* Linux: a wildcard-bound server auto-enables pktinfo and replies FROM the
 * address the client hit. Client sends to 127.0.0.2 → reply comes from 127.0.0.2
 * (not the routing-table default), which the client observes as the source. */
UTEST(udp_server, reply_from_hit_address) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "0.0.0.0", .port = 0 };  /* wildcard */
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    uint16_t port = kl_udp_server_local_port(&srv);

    KlUdp cli;
    KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, cli_recv, NULL));

    KlSockAddr dst;
    unsigned char _ipb[4]; inet_pton(AF_INET, "127.0.0.2", _ipb); kl_sockaddr_from_ipv4(&dst, _ipb, port);   /* hit a non-primary loopback */
    ASSERT_EQ(0, kl_udp_send_to(&cli, "ping", 4, &dst));
    pump_until(&ctx, &g_cli_got, 1, 300);

    ASSERT_EQ(1, g_cli_got);
    ASSERT_STREQ("127.0.0.2", g_cli_src_ip);   /* source-pinned reply egresses from the hit addr —
                                                * on readiness AND completion (native cmsg send + recv
                                                * pktinfo capture; the completion-source-pin increment) */

    kl_udp_free(&cli);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

/* SO_REUSEPORT shared bind (Linux): TWO servers bind the SAME port with reuse_port=1. Rather than
 * infer REUSEPORT from the bind merely succeeding (KlUdpServer also sets SO_REUSEADDR, which can by
 * itself permit a duplicate UDP bind on Linux), verify SO_REUSEPORT is actually set on each server's
 * socket via getsockopt on the native fd. Then confirm delivery is conserved — every datagram handled
 * exactly once across the group. We do NOT assert fan-out/distribution: a single client flow hashes
 * entirely to one server (REUSEPORT keys on the 4-tuple), so WHICH server handles each is not
 * observable deterministically. Capability-gated to Linux (SO_REUSEPORT semantics differ elsewhere). */
UTEST(udp_server, reuse_port_shared_bind) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));

    KlUdpServer a;
    KlUdpServerConfig sca = { .bind_addr = "127.0.0.1", .port = 0, .reuse_port = 1 };
    ASSERT_EQ(0, kl_udp_server_init(&a, &ctx, &sca, echo_handler, NULL));
    uint16_t port = kl_udp_server_local_port(&a);
    ASSERT_TRUE(port > 0);

    KlUdpServer b;
    KlUdpServerConfig scb = { .bind_addr = "127.0.0.1", .port = port, .reuse_port = 1 };
    ASSERT_EQ(0, kl_udp_server_init(&b, &ctx, &scb, echo_handler, NULL));   /* same port */

    /* Prove the shared bind is via SO_REUSEPORT, not merely SO_REUSEADDR. */
    int opt = 0; socklen_t olen = sizeof(opt);
    ASSERT_EQ(0, getsockopt((int)kl_udp_server_fd(&a), SOL_SOCKET, SO_REUSEPORT, &opt, &olen));
    ASSERT_TRUE(opt != 0);
    opt = 0; olen = sizeof(opt);
    ASSERT_EQ(0, getsockopt((int)kl_udp_server_fd(&b), SOL_SOCKET, SO_REUSEPORT, &opt, &olen));
    ASSERT_TRUE(opt != 0);

    KlUdp cli;
    KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, cli_recv, NULL));

    KlSockAddr dst;
    dest_v4(&dst, port);
    enum { N = 6 };
    for (int i = 0; i < N; i++)
        ASSERT_EQ(0, kl_udp_send_to(&cli, "x", 1, &dst));

    pump_until(&ctx, &g_cli_got, N, 500);

    ASSERT_EQ(N, g_srv_hits);   /* delivery conserved across the shared-port group */
    ASSERT_EQ(N, g_cli_got);

    kl_udp_free(&cli);
    kl_udp_server_free(&a);
    kl_udp_server_free(&b);
    kl_event_ctx_free(&ctx);
}
#endif

/* ── M4 regressions ──────────────────────────────────────────────────────────────────────────────
 * A delegating mock provider: wraps the real POSIX provider but (optionally) drops the send SOURCE_PIN
 * cap or the RX pktinfo capture, and counts closes — so a wildcard init fails loud (§4) and we can
 * assert prep.fd is closed exactly once (§10.3, blocker P1). Everything else delegates to POSIX (real
 * socket/bind/configure), so the failure is reached through the genuine open→rx-caps→init_ex path. */
static int g_mock_closes, g_mock_drop_srcpin, g_mock_drop_pktinfo;
static const KlDatagramOps *g_real_dg;
static KlSocketOps      g_mock_ops;
static KlDatagramOps    g_mock_dg;
static KlSocketProvider g_mock_sp;
static int mock_close(void *c, KlSocketHandle fd) {
    g_mock_closes++;
    return kl_socket_provider_posix()->ops->close(c, fd);
}
static unsigned mock_caps(void *c, KlSocketHandle fd) {
    unsigned caps = g_real_dg->caps ? g_real_dg->caps(c, fd) : 0;
    if (g_mock_drop_srcpin) caps &= ~(unsigned)KL_DGRAM_CAP_SOURCE_PIN;
    return caps;
}
static uint32_t mock_configure(void *c, KlSocketHandle fd, int family, const struct KlUdpConfig *cfg) {
    uint32_t rx = g_real_dg->configure(c, fd, family, cfg);
    if (g_mock_drop_pktinfo) rx &= ~(uint32_t)KL_DGRAM_RX_PKTINFO;
    return rx;
}
static void mock_provider_init(void) {
    const KlSocketProvider *p = kl_socket_provider_posix();
    g_real_dg = p->dgram;
    g_mock_ops = *p->ops;    g_mock_ops.close = mock_close;
    g_mock_dg  = *p->dgram;  g_mock_dg.caps = mock_caps; g_mock_dg.configure = mock_configure;
    g_mock_sp  = *p;         g_mock_sp.ops = &g_mock_ops; g_mock_sp.dgram = &g_mock_dg;
}

/* §10.3 — a wildcard bind missing the send SOURCE_PIN cap fails init and closes prep.fd exactly once. */
UTEST(udp_server, m4_missing_srcpin_fails_init_closes_fd) {
    reset(); mock_provider_init();
    g_mock_closes = 0; g_mock_drop_srcpin = 1; g_mock_drop_pktinfo = 0;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    ctx.sockets = &g_mock_sp;
    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "0.0.0.0", .port = 0 };   /* wildcard → want_caps=SOURCE_PIN */
    ASSERT_EQ(-1, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    ASSERT_EQ(KL_ERR_UNSUPPORTED, kl_udp_server_last_error(&srv));
    ASSERT_EQ(1, g_mock_closes);                                    /* prep.fd closed exactly once */
    g_mock_drop_srcpin = 0;
    kl_event_ctx_free(&ctx);
}

/* §10.3 — a wildcard bind whose RX pktinfo capture was NOT accepted fails init + closes prep.fd once. */
UTEST(udp_server, m4_missing_pktinfo_fails_init_closes_fd) {
    reset(); mock_provider_init();
    g_mock_closes = 0; g_mock_drop_srcpin = 0; g_mock_drop_pktinfo = 1;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    ctx.sockets = &g_mock_sp;
    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "0.0.0.0", .port = 0 };
    ASSERT_EQ(-1, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    ASSERT_EQ(KL_ERR_UNSUPPORTED, kl_udp_server_last_error(&srv));
    ASSERT_EQ(1, g_mock_closes);                                    /* prep.fd closed exactly once */
    g_mock_drop_pktinfo = 0;
    kl_event_ctx_free(&ctx);
}

/* §10.7 — reentrant free-from-handler: the handler frees the server; the transport reclamation defers
 * to the end of the current tick (no UAF under ASan), and a second free is an idempotent no-op. Runs on
 * readiness (default) and completion (BACKEND=pollcomp/iouring) — one file, both paths. */
static int g_freed_in_handler;
static void free_from_handler(KlUdpServer *s, const void *data, size_t len,
                              const KlSockAddr *src, void *ud) {
    (void)data; (void)len; (void)src; (void)ud;
    g_srv_hits++;
    kl_udp_server_free(s);        /* deferred to the tick's end (§9) — storage stays valid meanwhile */
    g_freed_in_handler = 1;
}
UTEST(udp_server, m4_free_from_handler) {
    reset(); g_freed_in_handler = 0;
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, free_from_handler, NULL));
    uint16_t port = kl_udp_server_local_port(&srv);
    KlUdp cli; KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    KlSockAddr dst; dest_v4(&dst, port);
    ASSERT_EQ(0, kl_udp_send_to(&cli, "bye", 3, &dst));
    pump_until(&ctx, &g_freed_in_handler, 1, 300);   /* handler runs → free (deferred, reclaimed at tick end) */
    ASSERT_EQ(1, g_srv_hits);
    ASSERT_EQ(1, g_freed_in_handler);
    kl_udp_server_free(&srv);      /* idempotent no-op after the deferred teardown already ran */
    kl_udp_free(&cli);
    kl_event_ctx_free(&ctx);
}

/* §10.8 — recv_gro forced off: a server configured with recv_gro=1 still delivers ONE datagram per
 * recv (the single-flight core cannot split a coalesced buffer, so GRO must not be enabled). */
UTEST(udp_server, m4_recv_gro_off_per_datagram) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0, .recv_gro = 1 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    uint16_t port = kl_udp_server_local_port(&srv);
    KlUdp cli; KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, cli_recv, NULL));
    KlSockAddr dst; dest_v4(&dst, port);
    ASSERT_EQ(0, kl_udp_send_to(&cli, "a", 1, &dst));
    ASSERT_EQ(0, kl_udp_send_to(&cli, "b", 1, &dst));
    pump_until(&ctx, &g_srv_hits, 2, 400);
    ASSERT_EQ(2, g_srv_hits);       /* two datagrams → two handler calls (not one coalesced buffer) */
    kl_udp_free(&cli);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

/* §P2 — an EMPTY multicast_group ("") is "no initial membership" (KlUdp parity), not an init failure. */
UTEST(udp_server, m4_empty_multicast_group_no_join) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0, .multicast_group = "" };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));   /* "" → no join, still OK */
    ASSERT_TRUE(kl_udp_server_local_port(&srv) > 0);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

/* §P1 — a large recv_buf_size is capped (min(value, 65535)); init succeeds and echoes normally. */
UTEST(udp_server, m4_recv_buf_size_capped) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0, .recv_buf_size = 4u * 1024 * 1024 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));   /* capped internally, no huge alloc */
    uint16_t port = kl_udp_server_local_port(&srv);
    KlUdp cli; KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, cli_recv, NULL));
    KlSockAddr dst; dest_v4(&dst, port);
    ASSERT_EQ(0, kl_udp_send_to(&cli, "z", 1, &dst));
    pump_until(&ctx, &g_cli_got, 1, 300);
    ASSERT_EQ(1, g_cli_got);
    kl_udp_free(&cli);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

/* ── M5.4: KlUdpServer recv batch / GRO opt-in ────────────────────────────────────────────────── */

/* A GRO-fabricating mock provider: wraps POSIX, forces RX_BATCH + CAP_GRO in caps(), reports RX_GRO
 * from configure(), and its recv_batch does a REAL recv then fabricates gro_seg — deterministic GRO
 * split at the server level on any platform. */
static int g_srv_mock_gro;
static unsigned char g_srv_rxbuf[8][2048];
static kl_ssize_t (*g_srv_real_recv)(void *, KlSocketHandle, void *, size_t, KlSockAddr *, KlDgramRxMeta *);
static const KlDatagramOps *g_srv_real_dg2;
static int g_srv_rxblk_fail;   /* 1 → rx_batch_new returns NULL (force create/attach failure) */
static int g_srv_last_n;       /* the n_slots the batch requested of rx_batch_new (mmsg normalization) */
static void *g_srv_rxblk_new(KlAllocator *a, int n, size_t bufsz) {
    (void)bufsz; g_srv_last_n = n;
    if (g_srv_rxblk_fail) return NULL;
    return kl_malloc(a, 1);
}
static void  g_srv_rxblk_free(KlAllocator *a, void *b) { kl_free(a, b, 1); }
static int   g_srv_recv_batch(void *c, KlSocketHandle fd, void *rb, KlDgramRxSlot *slots, int max) {
    (void)rb;
    int n = 0;
    while (n < max && n < 8) {
        KlSockAddr src; KlDgramRxMeta meta; memset(&meta, 0, sizeof(meta)); meta.tos = -1;
        kl_ssize_t r = g_srv_real_recv(c, fd, g_srv_rxbuf[n], sizeof(g_srv_rxbuf[n]), &src, &meta);
        if (r < 0) break;
        slots[n].data = g_srv_rxbuf[n]; slots[n].len = (size_t)r; slots[n].src = src;
        meta.gro_seg = g_srv_mock_gro; slots[n].meta = meta;
        n++;
    }
    if (n == 0) { errno = EAGAIN; return -1; }
    return n;
}
static int g_srv_drop_cap_gro;   /* 1 → caps() OMITS CAP_GRO (socket accepts RX_GRO but provider half missing) */
static unsigned g_srv_gro_caps(void *c, KlSocketHandle fd) {
    unsigned base = (g_srv_real_dg2->caps ? g_srv_real_dg2->caps(c, fd) : 0u) & ~(unsigned)KL_DGRAM_CAP_GRO;
    unsigned caps = base | KL_DGRAM_CAP_RX_BATCH;
    if (!g_srv_drop_cap_gro) caps |= KL_DGRAM_CAP_GRO;
    return caps;
}
static uint32_t g_srv_gro_configure(void *c, KlSocketHandle fd, int family, const struct KlUdpConfig *cfg) {
    uint32_t rx = g_srv_real_dg2->configure(c, fd, family, cfg);
    if (cfg->recv_gro) rx |= KL_DGRAM_RX_GRO;   /* claim GRO enabled ONLY when requested (like a real provider) */
    return rx;
}
static KlSocketOps      g_srv_gro_ops;
static KlDatagramOps    g_srv_gro_dg;
static KlSocketProvider g_srv_gro_sp;
static const KlSocketProvider *server_gro_provider(void) {
    const KlSocketProvider *p = kl_socket_provider_posix();
    g_srv_real_dg2 = p->dgram; g_srv_real_recv = p->dgram->recv;
    g_srv_gro_ops = *p->ops;
    g_srv_gro_dg  = *p->dgram;
    g_srv_gro_dg.caps = g_srv_gro_caps; g_srv_gro_dg.configure = g_srv_gro_configure;
    g_srv_gro_dg.rx_batch_new = g_srv_rxblk_new; g_srv_gro_dg.rx_batch_free = g_srv_rxblk_free;
    g_srv_gro_dg.recv_batch = g_srv_recv_batch;
    g_srv_gro_sp = *p; g_srv_gro_sp.ops = &g_srv_gro_ops; g_srv_gro_sp.dgram = &g_srv_gro_dg;
    return &g_srv_gro_sp;
}

/* Functional parity: with mmsg_batch set, N datagrams still reach the handler ONE at a time (the batch
 * splits nothing here) — batch path on a capable provider, single-flight fallback otherwise. */
UTEST(udp_server, m54_mmsg_batch_parity) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    int completion = (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) != 0;

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0, .mmsg_batch = 8 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    /* On a readiness build with a RX_BATCH-capable provider, the batch is attached; else single-flight. */
    unsigned pcaps = kl_datagram_provider_caps(&srv.dg);
    if (!completion && (pcaps & KL_DGRAM_CAP_RX_BATCH))
        ASSERT_TRUE(srv.dg.core->ext != NULL);   /* batch attached */
    else
        ASSERT_TRUE(srv.dg.core->ext == NULL);   /* single-flight fallback (completion / no cap) */
    uint16_t port = kl_udp_server_local_port(&srv);

    KlUdp cli; KlUdpConfig cc = { .ctx = &ctx };
    ASSERT_EQ(0, kl_udp_init(&cli, &cc));
    ASSERT_EQ(0, kl_udp_recv_start(&cli, multi_recv, NULL));
    KlSockAddr dst; dest_v4(&dst, port);
    enum { N = 4 };
    for (int i = 0; i < N; i++) { char m[8]; int mn = snprintf(m, sizeof(m), "srv-%d", i);
        ASSERT_EQ(0, kl_udp_send_to(&cli, m, (size_t)mn, &dst)); }
    pump_until(&ctx, &g_cli_got, N, 400);
    ASSERT_EQ(N, g_srv_hits);          /* one handler call per datagram */
    ASSERT_EQ(0, g_multi_bad);
    ASSERT_EQ((unsigned)((1u << N) - 1), g_multi_mask);

    kl_udp_free(&cli);
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

/* GRO split at the server: with recv_gro + a GRO-fabricating provider, a single coalesced datagram is
 * split into per-segment handler calls (the handler still sees one logical datagram each). */
UTEST(udp_server, m54_gro_splits_to_handler) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ctx); return; }  /* readiness (M5.4) */
    ctx.sockets = server_gro_provider();
    g_srv_mock_gro = 2;

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0, .recv_gro = 1 };
    ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
    ASSERT_TRUE(srv.dg.core->ext != NULL);   /* GRO → a RECV batch was attached */
    uint16_t port = kl_udp_server_local_port(&srv);

    /* raw client: send ONE 6-byte datagram; the mock claims gro_seg 2 → the server splits into 3. */
    int cfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in d; memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET; d.sin_addr.s_addr = htonl(0x7f000001); d.sin_port = htons(port);
    ASSERT_EQ((ssize_t)6, (ssize_t)sendto(cfd, "AABBCC", 6, 0, (struct sockaddr *)&d, sizeof(d)));
    for (int i = 0; i < 60 && g_srv_hits < 3; i++) kl_event_ctx_run(&ctx, 8, 10);
    ASSERT_EQ(3, g_srv_hits);   /* one coalesced datagram → 3 per-segment handler calls */
    close(cfd);

    g_srv_mock_gro = 0;
    kl_udp_server_free(&srv);
    kl_event_ctx_free(&ctx);
}

/* GRO is boundary-critical: if the splitting RECV batch can't be created (rx_batch_new fails), init
 * FAILS (no coalesced-unsplit single-flight fallback) — keyed to the accepted RX_GRO bit. */
UTEST(udp_server, m54_gro_batch_failure_fails_init) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ctx); return; }
    ctx.sockets = server_gro_provider();
    g_srv_mock_gro = 2; g_srv_rxblk_fail = 1;   /* the provider block alloc fails → attach can't happen */

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0, .recv_gro = 1 };
    ASSERT_EQ(-1, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));   /* GRO on + no split → fail */
    ASSERT_EQ(KL_ERR_UNSUPPORTED, kl_udp_server_last_error(&srv));

    g_srv_rxblk_fail = 0; g_srv_mock_gro = 0;
    kl_event_ctx_free(&ctx);   /* init failed → teardown already closed the fd; nothing to free */
}

/* Two-part gate (§6.2) at the server: the socket ACCEPTS RX_GRO (configure) but the provider caps OMIT
 * CAP_GRO — so an attached batch would NOT split. GRO capture is on with no active splitting → init must
 * fail (the boundary hazard), even though the batch itself could attach. */
UTEST(udp_server, m54_gro_accepted_but_no_provider_cap_fails) {
    reset();
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ctx); return; }
    ctx.sockets = server_gro_provider();
    g_srv_mock_gro = 2; g_srv_rxblk_fail = 0; g_srv_drop_cap_gro = 1;   /* RX_GRO accepted, CAP_GRO absent */

    KlUdpServer srv;
    KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0, .recv_gro = 1 };
    ASSERT_EQ(-1, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));   /* accepted GRO + inactive split → fail */
    ASSERT_EQ(KL_ERR_UNSUPPORTED, kl_udp_server_last_error(&srv));

    g_srv_drop_cap_gro = 0; g_srv_mock_gro = 0;
    kl_event_ctx_free(&ctx);
}

/* mmsg_batch normalization (KlUdp parity): 0 → default 16; 1 → batching disabled (no batch attached, no
 * rx_batch_new); a value > 64 → capped at 64. Verified via the n_slots the batch requests. */
UTEST(udp_server, m54_mmsg_normalization) {
    struct { int in; int expect_n; int expect_batch; } cases[] = {
        { 0,   16, 1 },   /* default */
        { 1,    0, 0 },   /* disabled — no batch (expect_n unused) */
        { 100, 64, 1 },   /* capped */
        { 32,  32, 1 },   /* passthrough */
    };
    for (unsigned k = 0; k < sizeof(cases)/sizeof(cases[0]); k++) {
        reset();
        KlAllocator alloc = kl_allocator_default();
        KlEventCtx ctx; ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
        if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ctx); return; }
        ctx.sockets = server_gro_provider();   /* reports RX_BATCH; no GRO requested → not mandatory */
        g_srv_mock_gro = 0; g_srv_rxblk_fail = 0; g_srv_last_n = -1;

        KlUdpServer srv;
        KlUdpServerConfig sc = { .bind_addr = "127.0.0.1", .port = 0, .mmsg_batch = cases[k].in };
        ASSERT_EQ(0, kl_udp_server_init(&srv, &ctx, &sc, echo_handler, NULL));
        if (cases[k].expect_batch) {
            ASSERT_TRUE(srv.dg.core->ext != NULL);          /* batch attached */
            ASSERT_EQ(cases[k].expect_n, g_srv_last_n);     /* normalized n_slots requested */
        } else {
            ASSERT_TRUE(srv.dg.core->ext == NULL);          /* mmsg_batch==1 → no batch */
        }
        kl_udp_server_free(&srv);
        kl_event_ctx_free(&ctx);
    }
}

UTEST_MAIN();
