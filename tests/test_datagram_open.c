/*
 * test_datagram_open.c — provider-neutral datagram socket-preparation helper
 * (docs/archive/designs/datagram_consolidation_design.md §2).
 *
 * kl_datagram_open() reproduces the datagram socket create/configure/bind prep, minus the send/receive machine, and
 * hands the caller a prepared+bound fd plus the capture-option mask configure() enabled. This suite
 * proves what the helper owes:
 *
 *   (1) PER-STEP FAILURE CLEANUP — a failure at any step (bad arg, no-datagram provider, partial vtable,
 *       bad bind address, socket(), set_nonblocking, bind) closes exactly the fd it created (no leak, no
 *       double-close; nothing created when it fails before create), reports the right KlError, and
 *       leaves prep.fd invalid;
 *   (2) OWNERSHIP HANDOFF — on success it returns a valid, bound fd it did NOT close (the caller owns
 *       it), and configure() was invoked once; and
 *   (3) CAPTURE-MASK PASSTHROUGH — prep.rx_caps carries exactly the KL_DGRAM_RX_* bitmask configure()
 *       reported (it cannot be reconstructed later).
 *
 * The failure/mask matrix runs against a fully-virtual mock KlSocketProvider (fake fds, injectable step
 * failures + configure mask, exact close accounting) → deterministic, no real descriptors, no timing.
 * The success cases also use the built-in default provider (sockets=NULL) on a real loopback socket.
 */

#include "../vendor/utest.h"

#include <keel/datagram.h>   /* KlDatagramSocketConfig + KlDatagramOps */
#include <keel/sockaddr.h>     /* KlSockAddr, kl_sockaddr_family/_port */
#include <keel/error.h>        /* KlError */
#include <keel/socket_dgram.h> /* KL_DGRAM_RX_* */

#include "../src/datagram_open.h"   /* kl_datagram_open + KlDatagramPrep — the unit under test */
#include "../src/socket.h"          /* KlSocketProvider / KlSocketOps + kl_sock_* seam (real close) */

#include <string.h>
#include <unistd.h>            /* close() */

/* ── fully-virtual mock provider: fake fds, injectable per-step failures, exact close accounting ──── */
typedef struct {
    int socket_calls;      /* kl_sock_socket dispatched here */
    int socket_fail;       /* 1 → socket() returns KL_INVALID_SOCKET */
    int nonblock_calls;
    int nonblock_fail;     /* 1 → set_nonblocking() returns -1 */
    int cloexec_calls;
    int bind_calls;
    int bind_fail;         /* 1 → bind() returns -1 */
    int configure_calls;
    int configure_family;  /* family passed to configure() */
    uint32_t configure_ret;/* mask configure() reports back (accepted capture options) */
    int close_calls;
    KlSocketHandle last_socket_fd;   /* the fake fd socket() produced */
    KlSocketHandle last_close_fd;    /* the fd close() was asked to release */
} MockSock;
static MockSock g_ms;

#define MOCK_FAKE_FD ((KlSocketHandle)4242)

static KlSocketHandle ms_socket(void *ctx, int domain, int type, int protocol) {
    (void)ctx; (void)domain; (void)type; (void)protocol;
    g_ms.socket_calls++;
    if (g_ms.socket_fail) return KL_INVALID_SOCKET;
    g_ms.last_socket_fd = MOCK_FAKE_FD;
    return MOCK_FAKE_FD;
}
static int ms_set_nonblocking(void *ctx, KlSocketHandle fd) {
    (void)ctx; (void)fd; g_ms.nonblock_calls++;
    return g_ms.nonblock_fail ? -1 : 0;
}
static void ms_set_cloexec(void *ctx, KlSocketHandle fd) { (void)ctx; (void)fd; g_ms.cloexec_calls++; }
static int ms_bind(void *ctx, KlSocketHandle fd, const KlSockAddr *addr) {
    (void)ctx; (void)fd; (void)addr; g_ms.bind_calls++;
    return g_ms.bind_fail ? -1 : 0;
}
static int ms_close(void *ctx, KlSocketHandle fd) {
    (void)ctx; g_ms.close_calls++; g_ms.last_close_fd = fd; return 0;
}
static const KlSocketOps MS_OPS = {
    .socket = ms_socket, .set_nonblocking = ms_set_nonblocking,
    .set_cloexec = ms_set_cloexec, .bind = ms_bind, .close = ms_close,
};

static uint32_t ms_dg_configure(void *ctx, KlSocketHandle fd, int family,
                                const struct KlDatagramSocketConfig *cfg) {
    (void)ctx; (void)fd; (void)cfg; g_ms.configure_calls++; g_ms.configure_family = family;
    return g_ms.configure_ret;   /* the accepted capture-option mask the helper must surface */
}
static const KlDatagramOps MS_DG = { .configure = ms_dg_configure };
/* Provider WITH a complete datagram data-plane. */
static const KlSocketProvider MS_SP = { .ops = &MS_OPS, .capabilities = KL_SOCK_CAP_DATAGRAM, .dgram = &MS_DG };
/* Provider WITHOUT a datagram data-plane (stream-only) — must be rejected before any fd is created. */
static const KlSocketProvider MS_SP_NODG = { .ops = &MS_OPS, .capabilities = 0, .dgram = NULL };
/* Provider with a PARTIAL datagram vtable (configure missing) — must also be rejected before create. */
static const KlDatagramOps MS_DG_PARTIAL = { .configure = NULL };
static const KlSocketProvider MS_SP_PARTIAL = { .ops = &MS_OPS, .capabilities = KL_SOCK_CAP_DATAGRAM, .dgram = &MS_DG_PARTIAL };

static void ms_reset(void) { memset(&g_ms, 0, sizeof(g_ms)); }

/* An unbound-datagram config (family only). */
static KlDatagramSocketConfig cfg_unbound(int family) {
    KlDatagramSocketConfig c; memset(&c, 0, sizeof(c));
    c.family = family;
    return c;
}
/* A bound-datagram config (numeric address → drives the bind step). */
static KlDatagramSocketConfig cfg_bound(const char *addr, uint16_t port) {
    KlDatagramSocketConfig c; memset(&c, 0, sizeof(c));
    c.bind_addr = addr; c.bind_port = port;
    return c;
}

/* ── (1) per-step failure cleanup ────────────────────────────────────────────────────────────────── */

UTEST(datagram_open, null_out_rejected) {
    ms_reset();
    KlDatagramSocketConfig c = cfg_unbound(AF_INET);
    ASSERT_EQ(-1, kl_datagram_open(&MS_SP, &c, NULL));   /* no out → nothing to do */
    ASSERT_EQ(0, g_ms.socket_calls);
}

UTEST(datagram_open, null_cfg_rejected) {
    ms_reset();
    KlDatagramPrep p;
    ASSERT_EQ(-1, kl_datagram_open(&MS_SP, NULL, &p));
    ASSERT_FALSE(kl_handle_valid(p.fd));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)p.err);
    ASSERT_EQ((uint32_t)0, p.rx_caps);
    ASSERT_EQ(0, g_ms.socket_calls);   /* nothing created */
    ASSERT_EQ(0, g_ms.close_calls);
}

UTEST(datagram_open, provider_without_datagram_rejected) {
    ms_reset();
    KlDatagramSocketConfig c = cfg_unbound(AF_INET);
    KlDatagramPrep p;
    ASSERT_EQ(-1, kl_datagram_open(&MS_SP_NODG, &c, &p));
    ASSERT_FALSE(kl_handle_valid(p.fd));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)p.err);
    ASSERT_EQ(0, g_ms.socket_calls);   /* rejected before create */
    ASSERT_EQ(0, g_ms.close_calls);
}

UTEST(datagram_open, provider_with_partial_vtable_rejected) {
    ms_reset();
    KlDatagramSocketConfig c = cfg_unbound(AF_INET);
    KlDatagramPrep p;
    ASSERT_EQ(-1, kl_datagram_open(&MS_SP_PARTIAL, &c, &p));   /* .dgram non-NULL but .configure NULL */
    ASSERT_FALSE(kl_handle_valid(p.fd));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)p.err);
    ASSERT_EQ(0, g_ms.socket_calls);   /* rejected BEFORE create → no configure() crash */
    ASSERT_EQ(0, g_ms.configure_calls);
    ASSERT_EQ(0, g_ms.close_calls);
}

UTEST(datagram_open, bad_bind_address_rejected) {
    ms_reset();
    KlDatagramSocketConfig c = cfg_bound("not-an-ip-address", 53);   /* numeric parse fails */
    KlDatagramPrep p;
    ASSERT_EQ(-1, kl_datagram_open(&MS_SP, &c, &p));
    ASSERT_FALSE(kl_handle_valid(p.fd));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)p.err);
    ASSERT_EQ(0, g_ms.socket_calls);   /* rejected before create */
    ASSERT_EQ(0, g_ms.close_calls);
}

UTEST(datagram_open, socket_failure_creates_nothing) {
    ms_reset();
    g_ms.socket_fail = 1;
    KlDatagramSocketConfig c = cfg_unbound(AF_INET);
    KlDatagramPrep p;
    ASSERT_EQ(-1, kl_datagram_open(&MS_SP, &c, &p));
    ASSERT_FALSE(kl_handle_valid(p.fd));
    ASSERT_EQ((int)KL_ERR_SOCKET, (int)p.err);
    ASSERT_EQ(1, g_ms.socket_calls);
    ASSERT_EQ(0, g_ms.close_calls);   /* nothing to close */
    ASSERT_EQ(0, g_ms.configure_calls);
    ASSERT_EQ(0, g_ms.bind_calls);
}

UTEST(datagram_open, nonblocking_failure_closes_fd_once) {
    ms_reset();
    g_ms.nonblock_fail = 1;
    KlDatagramSocketConfig c = cfg_unbound(AF_INET);
    KlDatagramPrep p;
    ASSERT_EQ(-1, kl_datagram_open(&MS_SP, &c, &p));
    ASSERT_FALSE(kl_handle_valid(p.fd));
    ASSERT_EQ((int)KL_ERR_SOCKET, (int)p.err);
    ASSERT_EQ(1, g_ms.socket_calls);
    ASSERT_EQ(1, g_ms.close_calls);                    /* the created fd was closed */
    ASSERT_EQ(MOCK_FAKE_FD, g_ms.last_close_fd);       /* exactly the fd we created */
    ASSERT_EQ(0, g_ms.configure_calls);                /* failed before configure */
    ASSERT_EQ(0, g_ms.bind_calls);
}

UTEST(datagram_open, bind_failure_closes_fd_once) {
    ms_reset();
    g_ms.bind_fail = 1;
    g_ms.configure_ret = KL_DGRAM_RX_PKTINFO;          /* configure ran, but bind fails after it */
    KlDatagramSocketConfig c = cfg_bound("127.0.0.1", 0);
    KlDatagramPrep p;
    ASSERT_EQ(-1, kl_datagram_open(&MS_SP, &c, &p));
    ASSERT_FALSE(kl_handle_valid(p.fd));
    ASSERT_EQ((int)KL_ERR_BIND, (int)p.err);
    ASSERT_EQ((uint32_t)0, p.rx_caps);                 /* mask cleared on failure */
    ASSERT_EQ(1, g_ms.socket_calls);
    ASSERT_EQ(1, g_ms.configure_calls);                /* configure ran before bind */
    ASSERT_EQ(1, g_ms.bind_calls);
    ASSERT_EQ(1, g_ms.close_calls);                    /* the created fd was closed once */
    ASSERT_EQ(MOCK_FAKE_FD, g_ms.last_close_fd);
}

/* ── (2) ownership handoff + (3) capture-mask passthrough ────────────────────────────────────────── */

UTEST(datagram_open, success_hands_off_fd_without_closing) {
    ms_reset();
    KlDatagramSocketConfig c = cfg_bound("127.0.0.1", 0);
    KlDatagramPrep p; memset(&p, 0xEE, sizeof(p));     /* poison to prove every field is written */
    ASSERT_EQ(0, kl_datagram_open(&MS_SP, &c, &p));
    ASSERT_TRUE(kl_handle_valid(p.fd));
    ASSERT_EQ(MOCK_FAKE_FD, p.fd);                     /* the exact fd socket() produced */
    ASSERT_EQ((int)KL_ERR_NONE, (int)p.err);
    ASSERT_EQ(1, g_ms.socket_calls);
    ASSERT_EQ(1, g_ms.cloexec_calls);
    ASSERT_EQ(1, g_ms.nonblock_calls);
    ASSERT_EQ(1, g_ms.configure_calls);
    ASSERT_EQ(1, g_ms.bind_calls);
    ASSERT_EQ(0, g_ms.close_calls);                    /* helper did NOT close — the caller owns it */
    /* the caller now disposes of it → exactly one close, of exactly this fd */
    ASSERT_EQ(0, kl_sock_close(&MS_SP, p.fd));
    ASSERT_EQ(1, g_ms.close_calls);
    ASSERT_EQ(MOCK_FAKE_FD, g_ms.last_close_fd);
}

UTEST(datagram_open, surfaces_configure_capture_mask) {
    ms_reset();
    g_ms.configure_ret = KL_DGRAM_RX_PKTINFO | KL_DGRAM_RX_TOS;   /* provider enabled pktinfo + TOS */
    KlDatagramSocketConfig c = cfg_unbound(AF_INET);
    KlDatagramPrep p;
    ASSERT_EQ(0, kl_datagram_open(&MS_SP, &c, &p));
    ASSERT_TRUE(kl_handle_valid(p.fd));
    ASSERT_EQ((uint32_t)(KL_DGRAM_RX_PKTINFO | KL_DGRAM_RX_TOS), p.rx_caps);   /* surfaced verbatim */
    (void)kl_sock_close(&MS_SP, p.fd);
}

UTEST(datagram_open, configure_receives_bind_family) {
    ms_reset();
    KlDatagramSocketConfig c = cfg_unbound(AF_INET6);
    KlDatagramPrep p;
    ASSERT_EQ(0, kl_datagram_open(&MS_SP, &c, &p));
    ASSERT_TRUE(kl_handle_valid(p.fd));
    ASSERT_EQ(1, g_ms.configure_calls);
    ASSERT_EQ(AF_INET6, g_ms.configure_family);        /* family threaded through create→configure */
    (void)kl_sock_close(&MS_SP, p.fd);
}

/* ── (2b) real default provider (sockets=NULL) → a genuinely prepared, bound loopback socket ──────── */

UTEST(datagram_open, default_provider_binds_real_socket) {
    KlDatagramSocketConfig c = cfg_bound("127.0.0.1", 0);   /* ephemeral port */
    KlDatagramPrep p;
    ASSERT_EQ(0, kl_datagram_open(NULL, &c, &p));
    ASSERT_TRUE(kl_handle_valid(p.fd));
    ASSERT_EQ((int)KL_ERR_NONE, (int)p.err);
    /* prove it is actually bound: the kernel assigned a concrete local port */
    KlSockAddr la; memset(&la, 0, sizeof(la));
    ASSERT_EQ(0, kl_sock_get_local_addr(NULL, p.fd, &la));
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&la));
    ASSERT_NE((uint16_t)0, kl_sockaddr_port(&la));   /* bound → nonzero ephemeral port */
    ASSERT_EQ(0, kl_sock_close(NULL, p.fd));
}

UTEST(datagram_open, default_provider_unbound_socket) {
    KlDatagramSocketConfig c = cfg_unbound(AF_INET);        /* no bind address → unbound datagram socket */
    KlDatagramPrep p;
    ASSERT_EQ(0, kl_datagram_open(NULL, &c, &p));
    ASSERT_TRUE(kl_handle_valid(p.fd));
    ASSERT_EQ((int)KL_ERR_NONE, (int)p.err);
    ASSERT_EQ(0, kl_sock_close(NULL, p.fd));
}

UTEST_MAIN();
