/*
 * test_socket_provider.c — the internal socket seam + provider vtable
 * (PAL Phase 2). Proves: POSIX default + fast path, provider dispatch, per-op
 * NULL fallback, deterministic fault injection (short write, EWOULDBLOCK,
 * ECONNRESET), a decorator over real socketpair I/O, and that  * (a KlDatagram) actually threads ctx->sockets to the seam.
 */
#include "utest.h"
#include "../src/socket.h"
#include "../src/event_caps.h"   /* kl_event_caps — guard readiness-only provider-seam tests */

#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>   /* D2: provider-threading tests use a stack KlDatagram now */
#include "datagram_test_util.h"   /* kl_dg_close_free — public lifecycle */
#include <keel/http_server.h>
#include <keel/http_client.h>

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "net_compat.h"

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
static kl_ssize_t mock_send(void *ctx, KlSocketHandle fd, const void *buf, size_t len) {
    MockSock *m = ctx;
    m->send_calls++;
    if (m->force_send_err) { int e = m->force_send_err; m->force_send_err = 0; errno = e; return -1; }
    size_t n = len;
    if (m->short_send >= 0 && (size_t)m->short_send < n) n = (size_t)m->short_send;
    if (m->wrap) return send((int)fd, buf, n, 0);
    return (ssize_t)n;
}
static kl_ssize_t mock_recv(void *ctx, KlSocketHandle fd, void *buf, size_t len) {
    MockSock *m = ctx;
    m->recv_calls++;
    if (m->force_recv_err) { int e = m->force_recv_err; m->force_recv_err = 0; errno = e; return -1; }
    if (m->wrap) return recv((int)fd, buf, len, 0);
    return 0;
}
static kl_ssize_t mock_writev(void *ctx, KlSocketHandle fd, const KlIoVec *iov, int iovcnt) {
    MockSock *m = ctx;
    m->writev_calls++;
    if (m->wrap) return kl_sockdef_writev(fd, iov, iovcnt);
    ssize_t t = 0;
    for (int i = 0; i < iovcnt; i++) t += (ssize_t)iov[i].len;
    return t;
}
static kl_ssize_t mock_sendfile(void *ctx, KlSocketHandle out_fd, int in_fd, uint64_t *offset, size_t count) {
    MockSock *m = ctx; (void)out_fd; (void)in_fd;
    m->sendfile_calls++;
    *offset += (uint64_t)count;              /* pretend fully sent */
    return (ssize_t)count;
}
static KlSocketHandle mock_socket(void *ctx, int domain, int type, int protocol) {
    MockSock *m = ctx; m->socket_calls++; return (KlSocketHandle)socket(domain, type, protocol);
}
static int mock_connect(void *ctx, KlSocketHandle fd, const KlSockAddr *a) {
    MockSock *m = ctx;
    m->connect_calls++;
    if (m->force_connect_err) { int e = m->force_connect_err; m->force_connect_err = 0; errno = e; return -1; }
    return kl_sockdef_connect(fd, a);   /* marshals KlSockAddr -> native */
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
    /* Decorate the stream ops (counted via MockSock); delegate the datagram
     * data-plane to the built-in default so a KlDatagram can run on this provider (its
     * socket lifecycle still threads the mock). */
    KlSocketProvider p = { &MOCK_OPS, m, KL_SOCK_CAP_DATAGRAM, kl_sockdef_dgram() };
    return p;
}

/* ── POSIX default / fast path ─────────────────────────────────────── */

#if !defined(_WIN32)   /* kl_socket_provider_posix() lives in the POSIX-only TU */
UTEST(sockprov, posix_identity_and_caps) {
    const KlSocketProvider *p = kl_socket_provider_posix();
    ASSERT_TRUE(p != NULL);
    ASSERT_STREQ("posix", p->ops->name);
    ASSERT_TRUE((p->capabilities & KL_SOCK_CAP_NATIVE_FD) != 0);
    ASSERT_TRUE(p->context == NULL);
}
#endif

/* NULL provider takes the inline POSIX path and moves real bytes. */
UTEST(sockprov, null_provider_real_io) {
    int sv[2];
    ASSERT_EQ(0, kl_test_socketpair(sv));
    ASSERT_EQ((ssize_t)5, kl_sock_send(NULL, sv[0], "hello", 5));
    char buf[8] = {0};
    ASSERT_EQ((ssize_t)5, kl_sock_recv(NULL, sv[1], buf, sizeof(buf)));
    ASSERT_STREQ("hello", buf);
    kl_test_closesock(sv[0]); kl_test_closesock(sv[1]);
}

/* Explicit POSIX provider dispatches through its ops to the same effect. */
#if !defined(_WIN32)   /* kl_socket_provider_posix() lives in the POSIX-only TU */
UTEST(sockprov, explicit_posix_provider_real_io) {
    const KlSocketProvider *p = kl_socket_provider_posix();
    int sv[2];
    ASSERT_EQ(0, kl_test_socketpair(sv));
    ASSERT_EQ((ssize_t)3, kl_sock_send(p, sv[0], "abc", 3));
    char buf[4] = {0};
    ASSERT_EQ((ssize_t)3, kl_sock_recv(p, sv[1], buf, sizeof(buf)));
    ASSERT_STREQ("abc", buf);
    kl_test_closesock(sv[0]); kl_test_closesock(sv[1]);
}
#endif

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
    KlSocketProvider p = { &partial, NULL, 0, NULL };
    int sv[2];
    ASSERT_EQ(0, kl_test_socketpair(sv));
    ASSERT_EQ((ssize_t)2, kl_sock_send(&p, sv[0], "hi", 2));   /* falls back */
    char buf[4] = {0};
    ASSERT_EQ((ssize_t)2, kl_sock_recv(&p, sv[1], buf, sizeof(buf)));
    ASSERT_STREQ("hi", buf);
    ASSERT_EQ(0, kl_sock_set_nonblocking(&p, sv[0]));          /* fallback, no crash */
    kl_test_closesock(sv[0]); kl_test_closesock(sv[1]);
}

/* Decorator: mock wraps real I/O but caps each send — the tail still arrives
 * on a second send, proving faults compose with real bytes on the wire. */
UTEST(sockprov, decorator_short_write_real_io) {
    MockSock m; memset(&m, 0, sizeof(m)); m.wrap = 1; m.short_send = 4;
    KlSocketProvider p = mock_provider(&m);
    int sv[2];
    ASSERT_EQ(0, kl_test_socketpair(sv));

    const char *msg = "0123456789";
    ssize_t w1 = kl_sock_send(&p, sv[0], msg, 10);
    ASSERT_EQ((ssize_t)4, w1);                       /* capped to 4 */
    ssize_t w2 = kl_sock_send(&p, sv[0], msg + 4, 6);
    ASSERT_EQ((ssize_t)4, w2);                       /* capped again */

    char buf[16] = {0};
    ssize_t r = recv(sv[1], buf, sizeof(buf), 0);
    ASSERT_EQ((ssize_t)8, r);                        /* the 8 real bytes arrived */
    ASSERT_EQ(0, memcmp(buf, "01234567", 8));
    kl_test_closesock(sv[0]); kl_test_closesock(sv[1]);
}

/* End-to-end threading: a KlDatagram created on a ctx carrying the mock provider routes its setup calls
 * through the mock (proves ctx->sockets reaches the datagram construction, not just the seam in
 * isolation). D2: migrated from the legacy UDP object. */
UTEST(sockprov, datagram_threads_provider) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = -1;
    KlSocketProvider p = mock_provider(&m);

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    ctx.sockets = &p;                                /* inject before creating the datagram */

    KlDatagram dg;
    KlDatagramSocketConfig cfg = { .ctx = &ctx, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_datagram_socket_init(&dg, &cfg));

    ASSERT_TRUE(m.nb_calls >= 1);                    /* set_nonblocking went through the mock */
    ASSERT_TRUE(m.cloexec_calls >= 1);               /* set_cloexec too */

    kl_dg_close_free(&ctx, &dg);
    kl_event_ctx_free(&ctx);
}

/* ── Mock datagram data-plane (KlDatagramOps) ──────────────────────── */

/* A programmable KlDatagramOps hung off a provider, proving that udp.c drives the
 * datagram machine (configure cap negotiation, send → enqueue-on-EAGAIN) purely
 * through the provider vtable — no real UDP syscalls, deterministic. The ops read
 * a file-static state (the provider ctx carries the *stream* MockSock). */
typedef struct {
    uint32_t configure_caps;   /* bitmask configure() reports as accepted */
    int      configure_calls;
    int      send_calls;
    int      force_send_err;   /* errno to fail the NEXT send with (0 = succeed) */
    size_t   last_send_len;
} MockDgram;

static MockDgram g_mdg;

static kl_ssize_t mdg_send(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                           const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    (void)ctx; (void)fd; (void)data; (void)dest; (void)src; (void)tos;
    g_mdg.send_calls++;
    g_mdg.last_send_len = len;
    if (g_mdg.force_send_err) { errno = g_mdg.force_send_err; return -1; }
    return (kl_ssize_t)len;              /* UDP send is all-or-nothing */
}
static kl_ssize_t mdg_recv(void *ctx, KlSocketHandle fd, void *buf, size_t buflen,
                           KlSockAddr *src, KlDgramRxMeta *meta) {
    (void)ctx; (void)fd; (void)buf; (void)buflen; (void)src;
    memset(meta, 0, sizeof(*meta)); meta->tos = -1;
    errno = EAGAIN; return -1;           /* always "drained" */
}
static uint32_t mdg_configure(void *ctx, KlSocketHandle fd, int family,
                              const struct KlDatagramSocketConfig *cfg) {
    (void)ctx; (void)fd; (void)family; (void)cfg;
    g_mdg.configure_calls++;
    return g_mdg.configure_caps;
}
static int mdg_set_tos(void *ctx, KlSocketHandle fd, int family, int tos) {
    (void)ctx; (void)fd; (void)family; (void)tos; return 0;
}
static int mdg_mcast(void *ctx, KlSocketHandle fd, int family,
                     const char *group, unsigned iface, int join) {
    (void)ctx; (void)fd; (void)family; (void)group; (void)iface; (void)join; return 0;
}
static const KlDatagramOps MOCK_DGRAM_OPS = {
    .send = mdg_send, .recv = mdg_recv, .configure = mdg_configure,
    .set_tos = mdg_set_tos, .mcast_membership = mdg_mcast,
    /* send_gso + batch ops NULL → udp.c uses the per-datagram path */
};

/* A provider whose stream ops are the built-in defaults (NULL ops → kl_sockdef_*
 * for the real socket()/bind() a KlDatagram needs) but whose datagram data-plane is the
 * mock above. */
static const KlSocketOps DGRAM_STREAM_OPS = { .name = "dgram-mock" }; /* all NULL → POSIX */

static KlSocketProvider mock_dgram_provider(void) {
    KlSocketProvider p = { &DGRAM_STREAM_OPS, NULL, KL_SOCK_CAP_DATAGRAM, &MOCK_DGRAM_OPS };
    return p;
}

/* configure() reports the accepted RX-capture bitmask; udp.c stores exactly those
 * flags (pktinfo/tos on, gro off here) — the capability handshake across the seam. */
UTEST(dgramprov, configure_caps_negotiated) {
    memset(&g_mdg, 0, sizeof(g_mdg));
    g_mdg.configure_caps = KL_DGRAM_RX_PKTINFO | KL_DGRAM_RX_TOS;   /* no GRO */
    KlSocketProvider p = mock_dgram_provider();

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    ctx.sockets = &p;

    KlDatagram dg;
    KlDatagramSocketConfig cfg = { .ctx = &ctx, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_datagram_socket_init(&dg, &cfg));

    ASSERT_EQ(1, g_mdg.configure_calls);
    unsigned rc = kl_datagram_accepted_rx_caps(&dg);
    ASSERT_TRUE((rc & KL_DGRAM_RX_PKTINFO) != 0);    /* accepted → stored */
    ASSERT_EQ((unsigned)0, (rc & KL_DGRAM_RX_GRO));  /* not reported → off */
    ASSERT_TRUE((rc & KL_DGRAM_RX_TOS) != 0);        /* accepted → stored */

    kl_dg_close_free(&ctx, &dg);
    kl_event_ctx_free(&ctx);
}

/* A successful provider send retires the datagram; an EAGAIN send makes udp.c
 * QUEUE it (backpressure) rather than drop it — both decisions live in udp.c and
 * are driven entirely off the provider send()'s return. */
UTEST(dgramprov, send_success_then_eagain_enqueues) {
    memset(&g_mdg, 0, sizeof(g_mdg));
    KlSocketProvider p = mock_dgram_provider();

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(0, kl_event_ctx_init(&ctx, &alloc));
    ctx.sockets = &p;

    /* This asserts the READINESS provider-seam send contract: a synchronous
     * provider->send that returns EAGAIN is queued, not dropped. On a completion
     * loop the datagram routes sends through the completion driver on the real
     * fd (the provider->send op is never consulted), so the mock-seam assertion
     * does not apply — skip there. */
    if (kl_event_caps(&ctx.loop) & KL_EVENT_CAP_COMPLETION) {
        kl_event_ctx_free(&ctx);
        UTEST_SKIP("readiness-only mock provider-seam send contract");
    }

    KlDatagram dg;
    KlDatagramSocketConfig cfg = { .ctx = &ctx, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_datagram_socket_init(&dg, &cfg));

    const uint8_t lo[4] = { 127, 0, 0, 1 };
    KlSockAddr dest;
    kl_sockaddr_from_ipv4(&dest, lo, 9999);

    /* Success: provider accepts the datagram; nothing left queued. */
    KlDatagramMessage m1 = { .data = "hello", .len = 5, .peer = &dest, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m1));
    ASSERT_EQ(1, g_mdg.send_calls);
    ASSERT_EQ((size_t)5, g_mdg.last_send_len);
    ASSERT_EQ((size_t)0, kl_datagram_send_queued_bytes(&dg));

    /* EAGAIN: the send machine QUEUES the datagram for a later writable flush, not a drop. */
    g_mdg.force_send_err = EWOULDBLOCK;
    KlDatagramMessage m2 = { .data = "world!!", .len = 7, .peer = &dest, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m2));   /* taken + queued */
    ASSERT_TRUE(g_mdg.send_calls >= 2);                          /* the provider was attempted (exact count
                                                                 * is machine-specific: it may retry the head) */
    ASSERT_EQ((size_t)7, kl_datagram_send_queued_bytes(&dg));   /* queued behind backpressure */
    ASSERT_EQ((uint64_t)0, kl_datagram_dropped(&dg));           /* not dropped */

    kl_dg_close_free(&ctx, &dg);
    kl_event_ctx_free(&ctx);
}

/* ── Lifecycle ops (Phase 2 ops-table extension) ───────────────────── */

/* socket/bind/listen/connect/accept/close all via the seam over loopback. */
UTEST(sockprov, lifecycle_posix_loopback) {
    int lfd = kl_sock_socket(NULL, AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(lfd >= 0);
    const uint8_t lo[4] = { 127, 0, 0, 1 };
    KlSockAddr a;
    kl_sockaddr_from_ipv4(&a, lo, 0);        /* ephemeral port */
    ASSERT_EQ(0, kl_sock_bind(NULL, lfd, &a));
    ASSERT_EQ(0, kl_sock_listen(NULL, lfd, 8));
    KlSockAddr bound;
    ASSERT_EQ(0, kl_sock_get_local_addr(NULL, lfd, &bound));
    ASSERT_GT((int)kl_sockaddr_port(&bound), 0);

    int cfd = kl_sock_socket(NULL, AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(cfd >= 0);
    KlSockAddr target;
    kl_sockaddr_from_ipv4(&target, lo, kl_sockaddr_port(&bound));
    ASSERT_EQ(0, kl_sock_connect(NULL, cfd, &target));

    KlSockAddr pa;
    int sfd = kl_sock_accept(NULL, lfd, &pa);
    ASSERT_TRUE(sfd >= 0);
    ASSERT_EQ((int)KL_AF_INET, (int)kl_sockaddr_family(&pa));   /* peer captured */

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
    const uint8_t lo[4] = { 127, 0, 0, 1 };
    KlSockAddr a;
    kl_sockaddr_from_ipv4(&a, lo, 9);
    errno = 0;
    ASSERT_EQ(-1, kl_sock_connect(&p, 9, &a));
    ASSERT_EQ(ECONNREFUSED, errno);
    ASSERT_EQ(1, m.connect_calls);
}

/* ── Phase 3 semantics: capabilities, native-fd, destroy, error taxonomy ── */

UTEST(sockprov, capability_query) {
    /* NULL provider == POSIX == native-fd. */
    ASSERT_TRUE(kl_socket_provider_has_cap(NULL, KL_SOCK_CAP_NATIVE_FD));
#if !defined(_WIN32)
    ASSERT_TRUE(kl_socket_provider_has_cap(kl_socket_provider_posix(), KL_SOCK_CAP_NATIVE_FD));
#endif
    /* A mock with no capabilities advertises none. */
    MockSock m; memset(&m, 0, sizeof(m));
    KlSocketProvider p = mock_provider(&m);   /* capabilities = 0 */
    ASSERT_FALSE(kl_socket_provider_has_cap(&p, KL_SOCK_CAP_NATIVE_FD));
}

UTEST(sockprov, native_fd_escape_hatch) {
    ASSERT_EQ(7, kl_sock_native_fd(NULL, 7));                      /* POSIX: fd is native */
#if !defined(_WIN32)
    ASSERT_EQ(7, kl_sock_native_fd(kl_socket_provider_posix(), 7));
#endif
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
#if !defined(_WIN32)
    kl_socket_provider_destroy(kl_socket_provider_posix());
#endif
    ASSERT_EQ(0, g_destroyed);
    /* A provider with a destroy op is torn down exactly once. */
    static const KlSocketOps ops = { .destroy = mock_destroy, .name = "destroyable" };
    KlSocketProvider p = { &ops, NULL, 0, NULL };
    kl_socket_provider_destroy(&p);
    ASSERT_EQ(1, g_destroyed);
}

#if !defined(_WIN32)   /* POSIX-errno taxonomy; the Winsock seam maps WSA codes */
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
#endif

/* A custom writev/sendfile op is dispatched (the Winsock WSASend/TransmitFile
 * path); a NULL provider takes the raw POSIX call. */
UTEST(sockprov, writev_sendfile_op_dispatch) {
    MockSock m; memset(&m, 0, sizeof(m)); m.short_send = -1; m.wrap = 1;
    KlSocketProvider p = mock_provider(&m);
    int sv[2];
    ASSERT_EQ(0, kl_test_socketpair(sv));

    char a[] = "AB", b[] = "CD";
    KlIoVec iov[2] = { { a, 2 }, { b, 2 } };
    ASSERT_EQ((ssize_t)4, kl_sock_writev(&p, sv[0], iov, 2));  /* dispatches to op */
    ASSERT_EQ(1, m.writev_calls);
    char buf[8] = {0};
    ASSERT_EQ((ssize_t)4, recv(sv[1], buf, sizeof(buf), 0));
    ASSERT_EQ(0, memcmp(buf, "ABCD", 4));

    ASSERT_EQ((ssize_t)4, kl_sock_writev(NULL, sv[0], iov, 2));/* NULL → real writev */
    ASSERT_EQ((ssize_t)4, recv(sv[1], buf, sizeof(buf), 0));
    kl_test_closesock(sv[0]); kl_test_closesock(sv[1]);

    uint64_t off = 0;
    ASSERT_EQ((ssize_t)100, kl_sock_sendfile(&p, 9, 9, &off, 100)); /* dispatches to op */
    ASSERT_EQ(1, m.sendfile_calls);
    ASSERT_EQ((uint64_t)100, off);
}

/* ── Provider SELECTION via public config (Phase 4 step 3) ─────────────── */

/* A native-fd decorator over the built-in defaults: counts socket()/accept()/
 * connect() and delegates everything else (NULL ops fall back to kl_sockdef_*).
 * Advertises NATIVE_FD so the readiness server/client accept it. */
typedef struct { int socket_calls, accept_calls, connect_calls; } Deco;

static KlSocketHandle deco_socket(void *ctx, int d, int t, int p) {
    ((Deco *)ctx)->socket_calls++;
    return kl_sockdef_socket(d, t, p);
}
static KlSocketHandle deco_accept(void *ctx, KlSocketHandle fd, KlSockAddr *peer) {
    ((Deco *)ctx)->accept_calls++;
    return kl_sockdef_accept(fd, peer);
}
static int deco_connect(void *ctx, KlSocketHandle fd, const KlSockAddr *a) {
    ((Deco *)ctx)->connect_calls++;
    return kl_sockdef_connect(fd, a);
}
static const KlSocketOps deco_ops = {
    .socket = deco_socket, .accept = deco_accept, .connect = deco_connect, .name = "deco",
};

/* A minimal ops table with NO native-fd capability — used only to prove the
 * guard rejects it (its ops are never invoked). */
static const KlSocketOps nonnative_ops = { .name = "nonnative" };

static void deco_ok_handler(KlHttpRequest *req, KlHttpResponse *res, void *u) {
    (void)req; (void)u;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}
static void *deco_server_thread(void *arg) { kl_http_server_run((KlHttpServer *)arg); return NULL; }

/* #1 — the native-fd guard: a non-native provider is rejected at init; a native
 * one (and NULL = built-in) is accepted. */
UTEST(sockprov, select_native_fd_guard) {
    KlSocketProvider nonnative = { &nonnative_ops, NULL, 0, NULL };  /* no NATIVE_FD */
    KlHttpServer s;
    KlHttpServerConfig bad = { .port = 0, .bind_addr = "127.0.0.1", .sockets = &nonnative };
    ASSERT_EQ(-1, kl_http_server_init(&s, &bad));
    ASSERT_EQ(KL_ERR_SOCKET, s.last_error);

    Deco d = {0,0,0};
    KlSocketProvider native = { &deco_ops, &d, KL_SOCK_CAP_NATIVE_FD, NULL };
    KlHttpServer s2;
    KlHttpServerConfig good = { .port = 0, .bind_addr = "127.0.0.1", .sockets = &native };
    ASSERT_EQ(0, kl_http_server_init(&s2, &good));
    kl_http_server_free(&s2);
}

/* #2 — end-to-end selection: a decorator provider on BOTH the server
 * (KlHttpServerConfig.sockets) and the client (KlHttpClientConfig.sockets), over a real
 * loopback request. The listen socket + accept flow through the server decorator;
 * the client socket + connect flow through the client decorator. (The listen
 * socket is created in kl_http_server_run, not _init, so this needs a running server.
 * Fixed port, like the smoke tests, to avoid a cross-thread bound_port read.) */
UTEST(sockprov, provider_selection_end_to_end) {
    Deco sd = {0,0,0}, cd = {0,0,0};
    KlSocketProvider sprov = { &deco_ops, &sd, KL_SOCK_CAP_NATIVE_FD, NULL };
    KlHttpServer s;
    KlHttpServerConfig scfg = { .port = 19099, .bind_addr = "127.0.0.1", .sockets = &sprov };
    ASSERT_EQ(0, kl_http_server_init(&s, &scfg));
    kl_http_server_route(&s, "GET", "/", deco_ok_handler, NULL, NULL);
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, deco_server_thread, &s));

    KlSocketProvider cprov = { &deco_ops, &cd, KL_SOCK_CAP_NATIVE_FD, NULL };
    KlAllocator a = kl_allocator_default();
    int ok = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        struct timespec ts = { 0, 30 * 1000000L };
        nanosleep(&ts, NULL);
        KlHttpClientConfig ccfg = { .timeout_ms = 2000, .sockets = &cprov };
        KlHttpClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        if (kl_http_client_request(&a, &ccfg, "GET", "http://127.0.0.1:19099/",
                              NULL, 0, NULL, 0, &resp) == 0) {
            ok = (resp.status == 200);
            kl_http_client_response_free(&resp);
        }
    }
    kl_http_server_stop(&s);
    pthread_join(th, NULL);
    kl_http_server_free(&s);

    ASSERT_TRUE(ok);
    ASSERT_GT(sd.socket_calls, 0);   /* server listen socket via its provider */
    ASSERT_GT(sd.accept_calls, 0);   /* accepted connection via its provider */
    ASSERT_GT(cd.socket_calls, 0);   /* client socket via its provider */
    ASSERT_GT(cd.connect_calls, 0);  /* ...and connected through it */
}

UTEST_MAIN();
