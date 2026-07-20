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
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Programmable mock provider ────────────────────────────────────── */

typedef struct {
    int     wrap;              /* 1 = perform real send/recv on the given fd */
    ssize_t short_send;        /* cap a send to this many bytes (-1 = no cap) */
    int     force_send_err;    /* errno to fail the NEXT send with (0 = none) */
    int     force_recv_err;    /* errno to fail the NEXT recv with (0 = none) */
    int     force_connect_err; /* errno to fail the NEXT connect with (0 = none) */
    int     nb_calls, cloexec_calls, nosig_calls, send_calls, recv_calls;
    int     socket_calls, connect_calls, writev_calls, sendfile_calls;
} MockSock;

static int mock_set_nonblocking(void *ctx, KlSocketHandle fd) {
    MockSock *m = ctx; (void)fd; m->nb_calls++; return 0;
}
static void mock_set_cloexec(void *ctx, KlSocketHandle fd) {
    MockSock *m = ctx; (void)fd; m->cloexec_calls++;
}
static void mock_set_nosigpipe(void *ctx, KlSocketHandle fd) {
    MockSock *m = ctx; (void)fd; m->nosig_calls++;
}
static ssize_t mock_send(void *ctx, KlSocketHandle fd, const void *buf, size_t len) {
    MockSock *m = ctx;
    m->send_calls++;
    if (m->force_send_err) { int e = m->force_send_err; m->force_send_err = 0; errno = e; return -1; }
    size_t n = len;
    if (m->short_send >= 0 && (size_t)m->short_send < n) n = (size_t)m->short_send;
    if (m->wrap) return send((int)fd, buf, n, 0);
    return (ssize_t)n;
}
static ssize_t mock_recv(void *ctx, KlSocketHandle fd, void *buf, size_t len) {
    MockSock *m = ctx;
    m->recv_calls++;
    if (m->force_recv_err) { int e = m->force_recv_err; m->force_recv_err = 0; errno = e; return -1; }
    if (m->wrap) return recv((int)fd, buf, len, 0);
    return 0;
}
static ssize_t mock_writev(void *ctx, KlSocketHandle fd, const struct iovec *iov, int iovcnt) {
    MockSock *m = ctx;
    m->writev_calls++;
    if (m->wrap) return writev((int)fd, iov, iovcnt);
    ssize_t t = 0;
    for (int i = 0; i < iovcnt; i++) t += (ssize_t)iov[i].iov_len;
    return t;
}
static ssize_t mock_sendfile(void *ctx, KlSocketHandle out_fd, int in_fd, off_t *offset, size_t count) {
    MockSock *m = ctx; (void)out_fd; (void)in_fd;
    m->sendfile_calls++;
    *offset += (off_t)count;                 /* pretend fully sent */
    return (ssize_t)count;
}
static KlSocketHandle mock_socket(void *ctx, int domain, int type, int protocol) {
    MockSock *m = ctx; m->socket_calls++; return (KlSocketHandle)socket(domain, type, protocol);
}
static int mock_connect(void *ctx, KlSocketHandle fd, const struct sockaddr *a, socklen_t l) {
    MockSock *m = ctx;
    m->connect_calls++;
    if (m->force_connect_err) { int e = m->force_connect_err; m->force_connect_err = 0; errno = e; return -1; }
    return connect((int)fd, a, l);
}

static const KlSocketOps MOCK_OPS = {
    .set_nonblocking = mock_set_nonblocking,
    .set_cloexec     = mock_set_cloexec,
    .set_nosigpipe   = mock_set_nosigpipe,
    .socket          = mock_socket,
    .connect         = mock_connect,
    .send            = mock_send,
    .recv            = mock_recv,
    .writev          = mock_writev,
    .sendfile        = mock_sendfile,
    .name            = "mock",
    /* bind/listen/accept/close left NULL → POSIX fallback */
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
    static const KlSocketOps partial = { .name = "partial" };  /* all ops NULL */
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

/* ── Lifecycle ops (Phase 2 ops-table extension) ───────────────────── */

/* socket/bind/listen/connect/accept/close all via the seam over loopback. */
UTEST(sockprov, lifecycle_posix_loopback) {
    int lfd = kl_sock_socket(NULL, AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(lfd >= 0);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ASSERT_EQ(0, kl_sock_bind(NULL, lfd, (struct sockaddr *)&a, sizeof(a)));
    ASSERT_EQ(0, kl_sock_listen(NULL, lfd, 8));
    socklen_t al = sizeof(a);
    ASSERT_EQ(0, getsockname(lfd, (struct sockaddr *)&a, &al));

    int cfd = kl_sock_socket(NULL, AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(cfd >= 0);
    ASSERT_EQ(0, kl_sock_connect(NULL, cfd, (struct sockaddr *)&a, sizeof(a)));

    struct sockaddr_storage pa; socklen_t pl = sizeof(pa);
    int sfd = kl_sock_accept(NULL, lfd, (struct sockaddr *)&pa, &pl);
    ASSERT_TRUE(sfd >= 0);

    ASSERT_EQ(0, kl_sock_close(NULL, sfd));
    ASSERT_EQ(0, kl_sock_close(NULL, cfd));
    ASSERT_EQ(0, kl_sock_close(NULL, lfd));
}

/* A mock socket op is dispatched + counted; a NULL op (close) falls back. */
UTEST(sockprov, mock_socket_dispatch) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = -1;
    KlSocketProvider p = mock_provider(&m);
    int fd = kl_sock_socket(&p, AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(1, m.socket_calls);
    ASSERT_EQ(0, kl_sock_close(&p, fd));       /* close op is NULL → POSIX */
}

/* Deterministic connect-failure injection through the seam. */
UTEST(sockprov, mock_connect_failure) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = -1; m.force_connect_err = ECONNREFUSED;
    KlSocketProvider p = mock_provider(&m);
    struct sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET;
    errno = 0;
    ASSERT_EQ(-1, kl_sock_connect(&p, 9, (struct sockaddr *)&a, sizeof(a)));
    ASSERT_EQ(ECONNREFUSED, errno);
    ASSERT_EQ(1, m.connect_calls);
}

/* ── Phase 3 semantics: capabilities, native-fd, destroy, error taxonomy ── */

UTEST(sockprov, capability_query) {
    /* NULL provider == POSIX == native-fd. */
    ASSERT_TRUE(kl_socket_provider_has_cap(NULL, KL_SOCK_CAP_NATIVE_FD));
    ASSERT_TRUE(kl_socket_provider_has_cap(kl_socket_provider_posix(), KL_SOCK_CAP_NATIVE_FD));
    /* A mock with no capabilities advertises none. */
    MockSock m; memset(&m, 0, sizeof(m));
    KlSocketProvider p = mock_provider(&m);   /* capabilities = 0 */
    ASSERT_FALSE(kl_socket_provider_has_cap(&p, KL_SOCK_CAP_NATIVE_FD));
}

UTEST(sockprov, native_fd_escape_hatch) {
    ASSERT_EQ(7, kl_sock_native_fd(NULL, 7));                      /* POSIX: fd is native */
    ASSERT_EQ(7, kl_sock_native_fd(kl_socket_provider_posix(), 7));
    MockSock m; memset(&m, 0, sizeof(m));
    KlSocketProvider p = mock_provider(&m);                        /* no NATIVE_FD cap */
    ASSERT_EQ(-1, kl_sock_native_fd(&p, 7));                       /* not a native fd */
}

static int g_destroyed;
static void mock_destroy(void *ctx) { (void)ctx; g_destroyed++; }

UTEST(sockprov, provider_destroy_lifecycle) {
    g_destroyed = 0;
    /* NULL provider and a NULL-destroy provider are both no-ops. */
    kl_socket_provider_destroy(NULL);
    kl_socket_provider_destroy(kl_socket_provider_posix());
    ASSERT_EQ(0, g_destroyed);
    /* A provider with a destroy op is torn down exactly once. */
    static const KlSocketOps ops = { .destroy = mock_destroy, .name = "destroyable" };
    KlSocketProvider p = { &ops, NULL, 0 };
    kl_socket_provider_destroy(&p);
    ASSERT_EQ(1, g_destroyed);
}

UTEST(sockprov, errno_to_error_taxonomy) {
    ASSERT_EQ(KL_ERR_TIMEOUT, kl_sock_errno_to_error(ETIMEDOUT));
    ASSERT_EQ(KL_ERR_CONNECT, kl_sock_errno_to_error(ECONNREFUSED));
    ASSERT_EQ(KL_ERR_CONNECT, kl_sock_errno_to_error(EHOSTUNREACH));
    ASSERT_EQ(KL_ERR_BIND,    kl_sock_errno_to_error(EADDRINUSE));
    ASSERT_EQ(KL_ERR_ALLOC,   kl_sock_errno_to_error(EMFILE));
    ASSERT_EQ(KL_ERR_INVALID_ARG, kl_sock_errno_to_error(EAFNOSUPPORT));
    ASSERT_EQ(KL_ERR_IO,      kl_sock_errno_to_error(ECONNRESET));
    ASSERT_EQ(KL_ERR_IO,      kl_sock_errno_to_error(EAGAIN));
    /* errno is not clobbered by the translation. */
    errno = ECONNREFUSED;
    (void)kl_sock_errno_to_error(ETIMEDOUT);
    ASSERT_EQ(ECONNREFUSED, errno);
}

/* A custom writev/sendfile op is dispatched (the Winsock WSASend/TransmitFile
 * path); a NULL provider takes the raw POSIX call. */
UTEST(sockprov, writev_sendfile_op_dispatch) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = -1; m.wrap = 1;
    KlSocketProvider p = mock_provider(&m);
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    char a[] = "AB", b[] = "CD";
    struct iovec iov[2] = { { a, 2 }, { b, 2 } };
    ASSERT_EQ((ssize_t)4, kl_sock_writev(&p, sv[0], iov, 2));  /* dispatches to op */
    ASSERT_EQ(1, m.writev_calls);
    char buf[8] = {0};
    ASSERT_EQ((ssize_t)4, recv(sv[1], buf, sizeof(buf), 0));
    ASSERT_EQ(0, memcmp(buf, "ABCD", 4));

    ASSERT_EQ((ssize_t)4, kl_sock_writev(NULL, sv[0], iov, 2));/* NULL → real writev */
    ASSERT_EQ((ssize_t)4, recv(sv[1], buf, sizeof(buf), 0));
    close(sv[0]); close(sv[1]);

    off_t off = 0;
    ASSERT_EQ((ssize_t)100, kl_sock_sendfile(&p, 9, 9, &off, 100)); /* dispatches to op */
    ASSERT_EQ(1, m.sendfile_calls);
    ASSERT_EQ((off_t)100, off);
}

UTEST_MAIN();
