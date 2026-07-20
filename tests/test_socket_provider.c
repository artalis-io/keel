/*
 * test_socket_provider.c — the internal socket seam + provider vtable
 * (PAL Phase 2). Proves: POSIX default + fast path, provider dispatch, per-op
 * NULL fallback, deterministic fault injection (short write, EWOULDBLOCK,
 * ECONNRESET), a decorator over real socketpair I/O, and that a transport
 * (KlUdp) actually threads ctx->sockets to the seam.
 */
#include "utest.h"
#include "../src/socket.h"

#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/udp.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* ── Programmable mock provider ────────────────────────────────────── */

typedef struct {
    int     wrap;              /* 1 = perform real send/recv on the given fd */
    ssize_t short_send;        /* cap a send to this many bytes (-1 = no cap) */
    int     force_send_err;    /* errno to fail the NEXT send with (0 = none) */
    int     force_recv_err;    /* errno to fail the NEXT recv with (0 = none) */
    int     nb_calls, cloexec_calls, nosig_calls, send_calls, recv_calls;
} MockSock;

static int mock_set_nonblocking(void *ctx, int fd) {
    MockSock *m = ctx; (void)fd; m->nb_calls++; return 0;
}
static void mock_set_cloexec(void *ctx, int fd) {
    MockSock *m = ctx; (void)fd; m->cloexec_calls++;
}
static void mock_set_nosigpipe(void *ctx, int fd) {
    MockSock *m = ctx; (void)fd; m->nosig_calls++;
}
static ssize_t mock_send(void *ctx, int fd, const void *buf, size_t len) {
    MockSock *m = ctx;
    m->send_calls++;
    if (m->force_send_err) { int e = m->force_send_err; m->force_send_err = 0; errno = e; return -1; }
    size_t n = len;
    if (m->short_send >= 0 && (size_t)m->short_send < n) n = (size_t)m->short_send;
    if (m->wrap) return send(fd, buf, n, 0);
    return (ssize_t)n;
}
static ssize_t mock_recv(void *ctx, int fd, void *buf, size_t len) {
    MockSock *m = ctx;
    m->recv_calls++;
    if (m->force_recv_err) { int e = m->force_recv_err; m->force_recv_err = 0; errno = e; return -1; }
    if (m->wrap) return recv(fd, buf, len, 0);
    return 0;
}

static const KlSocketOps MOCK_OPS = {
    mock_set_nonblocking, mock_set_cloexec, mock_set_nosigpipe,
    mock_send, mock_recv, "mock",
};

static KlSocketProvider mock_provider(MockSock *m) {
    KlSocketProvider p = { &MOCK_OPS, m, 0 };
    return p;
}

/* ── POSIX default / fast path ─────────────────────────────────────── */

UTEST(sockprov, posix_identity_and_caps) {
    const KlSocketProvider *p = kl_socket_provider_posix();
    ASSERT_TRUE(p != NULL);
    ASSERT_STREQ("posix", p->ops->name);
    ASSERT_TRUE((p->capabilities & KL_SOCK_CAP_NATIVE_FD) != 0);
    ASSERT_TRUE(p->context == NULL);
}

/* NULL provider takes the inline POSIX path and moves real bytes. */
UTEST(sockprov, null_provider_real_io) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    ASSERT_EQ((ssize_t)5, kl_sock_send(NULL, sv[0], "hello", 5));
    char buf[8] = {0};
    ASSERT_EQ((ssize_t)5, kl_sock_recv(NULL, sv[1], buf, sizeof(buf)));
    ASSERT_STREQ("hello", buf);
    close(sv[0]); close(sv[1]);
}

/* Explicit POSIX provider dispatches through its ops to the same effect. */
UTEST(sockprov, explicit_posix_provider_real_io) {
    const KlSocketProvider *p = kl_socket_provider_posix();
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    ASSERT_EQ((ssize_t)3, kl_sock_send(p, sv[0], "abc", 3));
    char buf[4] = {0};
    ASSERT_EQ((ssize_t)3, kl_sock_recv(p, sv[1], buf, sizeof(buf)));
    ASSERT_STREQ("abc", buf);
    close(sv[0]); close(sv[1]);
}

/* ── Dispatch + fault injection ────────────────────────────────────── */

UTEST(sockprov, mock_dispatch_and_setup_hooks) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = -1;
    KlSocketProvider p = mock_provider(&m);
    ASSERT_EQ(0, kl_sock_set_nonblocking(&p, 7));
    kl_sock_set_cloexec(&p, 7);
    kl_sock_set_nosigpipe(&p, 7);
    ASSERT_EQ(1, m.nb_calls);
    ASSERT_EQ(1, m.cloexec_calls);
    ASSERT_EQ(1, m.nosig_calls);
}

UTEST(sockprov, mock_short_write) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = 3;
    KlSocketProvider p = mock_provider(&m);
    ASSERT_EQ((ssize_t)3, kl_sock_send(&p, 9, "0123456789", 10));  /* capped */
    ASSERT_EQ(1, m.send_calls);
}

UTEST(sockprov, mock_send_ewouldblock) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = -1; m.force_send_err = EWOULDBLOCK;
    KlSocketProvider p = mock_provider(&m);
    errno = 0;
    ASSERT_EQ((ssize_t)-1, kl_sock_send(&p, 9, "x", 1));
    ASSERT_EQ(EWOULDBLOCK, errno);
}

UTEST(sockprov, mock_recv_econnreset) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = -1; m.force_recv_err = ECONNRESET;
    KlSocketProvider p = mock_provider(&m);
    char buf[4];
    errno = 0;
    ASSERT_EQ((ssize_t)-1, kl_sock_recv(&p, 9, buf, sizeof(buf)));
    ASSERT_EQ(ECONNRESET, errno);
}

/* A provider whose send/recv ops are NULL falls back to the POSIX path. */
UTEST(sockprov, per_op_null_fallback) {
    static const KlSocketOps partial = { NULL, NULL, NULL, NULL, NULL, "partial" };
    KlSocketProvider p = { &partial, NULL, 0 };
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    ASSERT_EQ((ssize_t)2, kl_sock_send(&p, sv[0], "hi", 2));   /* falls back */
    char buf[4] = {0};
    ASSERT_EQ((ssize_t)2, kl_sock_recv(&p, sv[1], buf, sizeof(buf)));
    ASSERT_STREQ("hi", buf);
    ASSERT_EQ(0, kl_sock_set_nonblocking(&p, sv[0]));          /* fallback, no crash */
    close(sv[0]); close(sv[1]);
}

/* Decorator: mock wraps real I/O but caps each send — the tail still arrives
 * on a second send, proving faults compose with real bytes on the wire. */
UTEST(sockprov, decorator_short_write_real_io) {
    MockSock m; memset(&m, 0, sizeof(m)); m.wrap = 1; m.short_send = 4;
    KlSocketProvider p = mock_provider(&m);
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    const char *msg = "0123456789";
    ssize_t w1 = kl_sock_send(&p, sv[0], msg, 10);
    ASSERT_EQ((ssize_t)4, w1);                       /* capped to 4 */
    ssize_t w2 = kl_sock_send(&p, sv[0], msg + 4, 6);
    ASSERT_EQ((ssize_t)4, w2);                       /* capped again */

    char buf[16] = {0};
    ssize_t r = recv(sv[1], buf, sizeof(buf), 0);
    ASSERT_EQ((ssize_t)8, r);                        /* the 8 real bytes arrived */
    ASSERT_EQ(0, memcmp(buf, "01234567", 8));
    close(sv[0]); close(sv[1]);
}

/* End-to-end threading: a KlUdp created on a ctx carrying the mock provider
 * routes its setup calls through the mock (proves ctx->sockets reaches the
 * transport, not just the seam in isolation). */
UTEST(sockprov, udp_transport_threads_provider) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = -1;
    KlSocketProvider p = mock_provider(&m);

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    ctx.sockets = &p;                                /* inject before creating the transport */

    KlUdp udp;
    KlUdpConfig cfg = { .ctx = &ctx, .bind_addr = "127.0.0.1", .bind_port = 0 };
    ASSERT_EQ(0, kl_udp_init(&udp, &cfg));

    ASSERT_TRUE(m.nb_calls >= 1);                    /* set_nonblocking went through the mock */
    ASSERT_TRUE(m.cloexec_calls >= 1);               /* set_cloexec too */

    kl_udp_free(&udp);
    kl_event_ctx_free(&ctx);
}

UTEST_MAIN();
