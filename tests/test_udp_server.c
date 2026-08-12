/*
 * test_udp_server.c — KlUdpServer dispatch conformance.
 *
 * Phase-B step 6.2 (verification checkpoint): KlUdpServer is a thin wrapper over the PUBLIC KlUdp API
 * (kl_udp_init + kl_udp_recv_start + kl_udp_send_to_from), so its receive already rides the shared
 * serial-receive machine (KlDgramRecv over the dedicated inbound slot) that step 6.1 wired into
 * kl_udp_recv_start — no separate server-side wiring exists or is needed. These cases exercise the
 * server's Tier-1 couplings THROUGH that machine: serial multi-datagram one-packet-one-callback
 * dispatch validated per-payload with a duplicate-rejecting seen-set (no coalescing/substitution/
 * duplication/loss across the re-arm); source address on every recv with a reply to each sender's own
 * source; and (Linux) a getsockopt-verified SO_REUSEPORT shared bind with delivery conserved. Being
 * backend-agnostic (a default KlEventCtx loop), the same suite runs on the readiness backends
 * (kqueue/epoll/wsapoll/poll) AND the completion backends (pollcomp/io_uring/IOCP) — one file, both paths.
 *
 * The server's SEND queue (byte-budget) and CLOSE (kl_udp_free legacy teardown) remain the existing
 * KlUdp compatibility behavior; the fixed-slot atomic send + confirmed-detachment close machines are
 * deferred to the public KlDatagram path (Step 7) and are intentionally NOT exercised here.
 *
 * SPDX-License-Identifier: MIT
 */
#include "utest.h"
#include <keel/udp_server.h>
#include <keel/udp.h>
#include <keel/event_ctx.h>
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
    ASSERT_STREQ("127.0.0.2", g_cli_src_ip);   /* reply egressed from the hit addr */

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

UTEST_MAIN();
