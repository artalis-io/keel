/*
 * test_datagram_open.c — M0 provider-neutral datagram socket-preparation helper
 * (docs/datagram_consolidation_design.md §2 D-DNS-2).
 *
 * kl_datagram_open() reproduces kl_udp_init's create/configure/bind prep, minus the KlUdp machine, and
 * hands the caller a prepared+bound fd. This suite proves the two things M0 owes:
 *
 *   (1) PER-STEP FAILURE CLEANUP — a failure at any step (bad arg, no-datagram provider, bad bind
 *       address, socket(), set_nonblocking, bind) closes exactly the fd it created (no leak, no
 *       double-close, nothing created when it fails before create), reports the right KlError, and
 *       returns KL_INVALID_SOCKET; and
 *   (2) OWNERSHIP HANDOFF — on success it returns a valid, bound fd it did NOT close (the caller owns
 *       it), configure() was invoked once, and the real default provider yields a genuinely bound
 *       socket.
 *
 * The failure matrix runs against a fully-virtual mock KlSocketProvider (fake fds, injectable step
 * failures, exact close accounting) → deterministic, no real descriptors, no timing. The success cases
 * use the built-in default provider (sockets=NULL) on a real loopback socket.
 */

#include "../vendor/utest.h"

#include <keel/udp.h>          /* KlUdpConfig */
#include <keel/sockaddr.h>     /* KlSockAddr, kl_sockaddr_family/_port */
#include <keel/error.h>        /* KlError */

#include "../src/datagram_open.h"   /* kl_datagram_open — the unit under test */
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
                                const struct KlUdpConfig *cfg) {
    (void)ctx; (void)fd; (void)cfg; g_ms.configure_calls++; g_ms.configure_family = family;
    return 0;   /* best-effort; no capture caps */
}
static const KlDatagramOps MS_DG = { .configure = ms_dg_configure };
/* Provider WITH a datagram data-plane. */
static const KlSocketProvider MS_SP = { .ops = &MS_OPS, .capabilities = KL_SOCK_CAP_DATAGRAM, .dgram = &MS_DG };
/* Provider WITHOUT a datagram data-plane (stream-only) — must be rejected before any fd is created. */
static const KlSocketProvider MS_SP_NODG = { .ops = &MS_OPS, .capabilities = 0, .dgram = NULL };

static void ms_reset(void) { memset(&g_ms, 0, sizeof(g_ms)); }

/* An unbound-datagram config (family only). */
static KlUdpConfig cfg_unbound(int family) {
    KlUdpConfig c; memset(&c, 0, sizeof(c));
    c.family = family;
    return c;
}
/* A bound-datagram config (numeric address → drives the bind step). */
static KlUdpConfig cfg_bound(const char *addr, uint16_t port) {
    KlUdpConfig c; memset(&c, 0, sizeof(c));
    c.bind_addr = addr; c.bind_port = port;
    return c;
}

/* ── (1) per-step failure cleanup ────────────────────────────────────────────────────────────────── */

UTEST(datagram_open, null_cfg_rejected) {
    ms_reset();
    KlError err = KL_ERR_NONE;
    KlSocketHandle fd = kl_datagram_open(&MS_SP, NULL, &err);
    ASSERT_FALSE(kl_handle_valid(fd));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)err);
    ASSERT_EQ(0, g_ms.socket_calls);   /* nothing created */
    ASSERT_EQ(0, g_ms.close_calls);
}

UTEST(datagram_open, provider_without_datagram_rejected) {
    ms_reset();
    KlUdpConfig c = cfg_unbound(AF_INET);
    KlError err = KL_ERR_NONE;
    KlSocketHandle fd = kl_datagram_open(&MS_SP_NODG, &c, &err);
    ASSERT_FALSE(kl_handle_valid(fd));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)err);
    ASSERT_EQ(0, g_ms.socket_calls);   /* rejected before create */
    ASSERT_EQ(0, g_ms.close_calls);
}

UTEST(datagram_open, bad_bind_address_rejected) {
    ms_reset();
    KlUdpConfig c = cfg_bound("not-an-ip-address", 53);   /* numeric parse fails */
    KlError err = KL_ERR_NONE;
    KlSocketHandle fd = kl_datagram_open(&MS_SP, &c, &err);
    ASSERT_FALSE(kl_handle_valid(fd));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)err);
    ASSERT_EQ(0, g_ms.socket_calls);   /* rejected before create */
    ASSERT_EQ(0, g_ms.close_calls);
}

UTEST(datagram_open, socket_failure_creates_nothing) {
    ms_reset();
    g_ms.socket_fail = 1;
    KlUdpConfig c = cfg_unbound(AF_INET);
    KlError err = KL_ERR_NONE;
    KlSocketHandle fd = kl_datagram_open(&MS_SP, &c, &err);
    ASSERT_FALSE(kl_handle_valid(fd));
    ASSERT_EQ((int)KL_ERR_SOCKET, (int)err);
    ASSERT_EQ(1, g_ms.socket_calls);
    ASSERT_EQ(0, g_ms.close_calls);   /* nothing to close */
    ASSERT_EQ(0, g_ms.configure_calls);
    ASSERT_EQ(0, g_ms.bind_calls);
}

UTEST(datagram_open, nonblocking_failure_closes_fd_once) {
    ms_reset();
    g_ms.nonblock_fail = 1;
    KlUdpConfig c = cfg_unbound(AF_INET);
    KlError err = KL_ERR_NONE;
    KlSocketHandle fd = kl_datagram_open(&MS_SP, &c, &err);
    ASSERT_FALSE(kl_handle_valid(fd));
    ASSERT_EQ((int)KL_ERR_SOCKET, (int)err);
    ASSERT_EQ(1, g_ms.socket_calls);
    ASSERT_EQ(1, g_ms.close_calls);                    /* the created fd was closed */
    ASSERT_EQ(MOCK_FAKE_FD, g_ms.last_close_fd);       /* exactly the fd we created */
    ASSERT_EQ(0, g_ms.configure_calls);                /* failed before configure */
    ASSERT_EQ(0, g_ms.bind_calls);
}

UTEST(datagram_open, bind_failure_closes_fd_once) {
    ms_reset();
    g_ms.bind_fail = 1;
    KlUdpConfig c = cfg_bound("127.0.0.1", 0);
    KlError err = KL_ERR_NONE;
    KlSocketHandle fd = kl_datagram_open(&MS_SP, &c, &err);
    ASSERT_FALSE(kl_handle_valid(fd));
    ASSERT_EQ((int)KL_ERR_BIND, (int)err);
    ASSERT_EQ(1, g_ms.socket_calls);
    ASSERT_EQ(1, g_ms.configure_calls);                /* configure ran before bind */
    ASSERT_EQ(1, g_ms.bind_calls);
    ASSERT_EQ(1, g_ms.close_calls);                    /* the created fd was closed once */
    ASSERT_EQ(MOCK_FAKE_FD, g_ms.last_close_fd);
}

/* ── (2) ownership handoff ───────────────────────────────────────────────────────────────────────── */

UTEST(datagram_open, success_hands_off_fd_without_closing) {
    ms_reset();
    KlUdpConfig c = cfg_bound("127.0.0.1", 0);
    KlError err = KL_ERR_INVALID_ARG;   /* seed non-NONE to prove it is cleared on success */
    KlSocketHandle fd = kl_datagram_open(&MS_SP, &c, &err);
    ASSERT_TRUE(kl_handle_valid(fd));
    ASSERT_EQ(MOCK_FAKE_FD, fd);                       /* the exact fd socket() produced */
    ASSERT_EQ((int)KL_ERR_NONE, (int)err);
    ASSERT_EQ(1, g_ms.socket_calls);
    ASSERT_EQ(1, g_ms.cloexec_calls);
    ASSERT_EQ(1, g_ms.nonblock_calls);
    ASSERT_EQ(1, g_ms.configure_calls);
    ASSERT_EQ(1, g_ms.bind_calls);
    ASSERT_EQ(0, g_ms.close_calls);                    /* helper did NOT close — the caller owns it */
    /* the caller now disposes of it → exactly one close, of exactly this fd */
    ASSERT_EQ(0, kl_sock_close(&MS_SP, fd));
    ASSERT_EQ(1, g_ms.close_calls);
    ASSERT_EQ(MOCK_FAKE_FD, g_ms.last_close_fd);
}

UTEST(datagram_open, configure_receives_bind_family) {
    ms_reset();
    KlUdpConfig c = cfg_unbound(AF_INET6);
    KlError err = KL_ERR_NONE;
    KlSocketHandle fd = kl_datagram_open(&MS_SP, &c, &err);
    ASSERT_TRUE(kl_handle_valid(fd));
    ASSERT_EQ(1, g_ms.configure_calls);
    ASSERT_EQ(AF_INET6, g_ms.configure_family);        /* family threaded through create→configure */
    (void)kl_sock_close(&MS_SP, fd);
}

UTEST(datagram_open, null_out_err_tolerated) {
    ms_reset();
    KlUdpConfig c = cfg_unbound(AF_INET);
    KlSocketHandle fd = kl_datagram_open(&MS_SP, &c, NULL);   /* out_err NULL on the success path */
    ASSERT_TRUE(kl_handle_valid(fd));
    (void)kl_sock_close(&MS_SP, fd);
    g_ms.socket_fail = 1;
    KlSocketHandle bad = kl_datagram_open(&MS_SP, &c, NULL);  /* and on a failure path */
    ASSERT_FALSE(kl_handle_valid(bad));
}

/* ── (2b) real default provider (sockets=NULL) → a genuinely prepared, bound loopback socket ──────── */

UTEST(datagram_open, default_provider_binds_real_socket) {
    KlUdpConfig c = cfg_bound("127.0.0.1", 0);   /* ephemeral port */
    KlError err = KL_ERR_INVALID_ARG;
    KlSocketHandle fd = kl_datagram_open(NULL, &c, &err);
    ASSERT_TRUE(kl_handle_valid(fd));
    ASSERT_EQ((int)KL_ERR_NONE, (int)err);
    /* prove it is actually bound: the kernel assigned a concrete local port */
    KlSockAddr la; memset(&la, 0, sizeof(la));
    ASSERT_EQ(0, kl_sock_get_local_addr(NULL, fd, &la));
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&la));
    ASSERT_NE((uint16_t)0, kl_sockaddr_port(&la));   /* bound → nonzero ephemeral port */
    ASSERT_EQ(0, kl_sock_close(NULL, fd));
}

UTEST(datagram_open, default_provider_unbound_socket) {
    KlUdpConfig c = cfg_unbound(AF_INET);        /* no bind address → unbound datagram socket */
    KlError err = KL_ERR_INVALID_ARG;
    KlSocketHandle fd = kl_datagram_open(NULL, &c, &err);
    ASSERT_TRUE(kl_handle_valid(fd));
    ASSERT_EQ((int)KL_ERR_NONE, (int)err);
    ASSERT_EQ(0, kl_sock_close(NULL, fd));
}

UTEST_MAIN();
