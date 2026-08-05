/*
 * test_io_status.c — the portable I/O-result classification seam (freestanding
 * step B1). Proves:
 *   - kl_sock_io_status() dispatches to a provider's io_status op when set, and
 *     that a provider CAN classify every category WITHOUT ever touching errno
 *     (the freestanding contract: a stack with no hosted errno supplies io_status);
 *   - a NULL io_status op falls back to the hosted errno mapping
 *     (kl_sockdef_io_status), so every current provider is behaviour-neutral;
 *   - end to end, the async client's would-block / connect-pending / re-arm
 *     control flow now flows through the provider's io_status seam (a decorator
 *     over the real POSIX provider counts the classifications during a real
 *     loopback request), i.e. the client no longer reads errno directly.
 */
#include "utest.h"
#include "../src/socket.h"

#include <keel/client.h>
#include <keel/server.h>
#include <keel/resolver.h>
#include <keel/allocator.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "net_compat.h"

/* ── A provider whose io_status is caller-programmed and NEVER reads errno ──
 * Its send/recv/connect all return -1 without setting errno; classification is
 * driven purely by the programmed KlIoStatus. This is the freestanding case: no
 * hosted errno involved anywhere on the I/O-result path. */
typedef struct {
    KlIoStatus next;      /* what io_status() reports */
    int        status_calls;
} ErrnoFreeSock;

static kl_ssize_t efs_send(void *ctx, KlSocketHandle fd, const void *b, size_t n) {
    (void)ctx; (void)fd; (void)b; (void)n; return -1;   /* fail, DO NOT touch errno */
}
static kl_ssize_t efs_recv(void *ctx, KlSocketHandle fd, void *b, size_t n) {
    (void)ctx; (void)fd; (void)b; (void)n; return -1;   /* fail, DO NOT touch errno */
}
static int efs_connect(void *ctx, KlSocketHandle fd, const KlSockAddr *a) {
    (void)ctx; (void)fd; (void)a; return -1;            /* fail, DO NOT touch errno */
}
static KlIoStatus efs_io_status(void *ctx) {
    ErrnoFreeSock *m = ctx; m->status_calls++; return m->next;
}
static const KlSocketOps EFS_OPS = {
    .send = efs_send, .recv = efs_recv, .connect = efs_connect,
    .io_status = efs_io_status, .name = "errno-free",
};
static KlSocketProvider efs_provider(ErrnoFreeSock *m) {
    KlSocketProvider p = { &EFS_OPS, m, 0, NULL };
    return p;
}

/* The op, when present, is consulted — for every category — with errno untouched. */
UTEST(iostatus, op_classifies_without_errno) {
    ErrnoFreeSock m; memset(&m, 0, sizeof(m));
    KlSocketProvider p = efs_provider(&m);

    const KlIoStatus cats[] = {
        KL_IO_OK, KL_IO_WOULD_BLOCK, KL_IO_INTERRUPTED,
        KL_IO_PENDING, KL_IO_CLOSED, KL_IO_RESET, KL_IO_FATAL,
    };
    for (size_t i = 0; i < sizeof(cats)/sizeof(cats[0]); i++) {
        errno = 0xBADF00D;           /* poison: a correct provider path ignores it */
        m.next = cats[i];
        /* Drive a failing op first (mirrors real usage: classify after -1). */
        char buf[1];
        ASSERT_EQ((kl_ssize_t)-1, kl_sock_send(&p, 7, "x", 1));
        ASSERT_EQ((kl_ssize_t)-1, kl_sock_recv(&p, 7, buf, 1));
        ASSERT_EQ(cats[i], kl_sock_io_status(&p));
    }
    ASSERT_TRUE(m.status_calls >= 7);
    /* errno was never consulted by the seam — our poison survives. */
    ASSERT_EQ(0xBADF00D, errno);
}

/* connect returning PENDING via the op — the exact test the async client makes:
 * `kl_sock_io_status(...) != KL_IO_PENDING`. */
UTEST(iostatus, connect_pending_via_op) {
    ErrnoFreeSock m; memset(&m, 0, sizeof(m));
    KlSocketProvider p = efs_provider(&m);
    KlSockAddr a; const uint8_t lo[4] = {127,0,0,1};
    kl_sockaddr_from_ipv4(&a, lo, 80);

    errno = 0xBADF00D;
    m.next = KL_IO_PENDING;
    ASSERT_EQ(-1, kl_sock_connect(&p, 9, &a));
    ASSERT_EQ(KL_IO_PENDING, kl_sock_io_status(&p));     /* client would keep connecting */

    m.next = KL_IO_FATAL;
    ASSERT_EQ(-1, kl_sock_connect(&p, 9, &a));
    ASSERT_TRUE(kl_sock_io_status(&p) != KL_IO_PENDING); /* client would fail the attempt */
    ASSERT_EQ(0xBADF00D, errno);                         /* seam never read errno */
}

/* ── NULL io_status op → hosted errno fallback (behaviour-neutral) ──────── */

#if !defined(_WIN32)   /* POSIX errno taxonomy; the Winsock seam maps WSA codes */
UTEST(iostatus, null_op_errno_fallback) {
    /* NULL provider == built-in POSIX == NULL io_status op → kl_sockdef_io_status. */
    errno = EAGAIN;       ASSERT_EQ(KL_IO_WOULD_BLOCK,  kl_sock_io_status(NULL));
    errno = EWOULDBLOCK;  ASSERT_EQ(KL_IO_WOULD_BLOCK,  kl_sock_io_status(NULL));
    errno = EINTR;        ASSERT_EQ(KL_IO_INTERRUPTED,  kl_sock_io_status(NULL));
    errno = EINPROGRESS;  ASSERT_EQ(KL_IO_PENDING,      kl_sock_io_status(NULL));
    errno = EPIPE;        ASSERT_EQ(KL_IO_CLOSED,       kl_sock_io_status(NULL));
    errno = 0;            ASSERT_EQ(KL_IO_CLOSED,       kl_sock_io_status(NULL));
    errno = ECONNRESET;   ASSERT_EQ(KL_IO_RESET,        kl_sock_io_status(NULL));
    errno = ECONNREFUSED; ASSERT_EQ(KL_IO_FATAL,        kl_sock_io_status(NULL));

    /* A provider with all ops NULL takes the same fallback as the NULL provider. */
    static const KlSocketOps partial = { .name = "partial" };
    KlSocketProvider pp = { &partial, NULL, 0, NULL };
    errno = EAGAIN;       ASSERT_EQ(KL_IO_WOULD_BLOCK, kl_sock_io_status(&pp));
    errno = ECONNRESET;   ASSERT_EQ(KL_IO_RESET,       kl_sock_io_status(&pp));
}
#endif

/* ── Client-level: the ASYNC client consults io_status end to end ──────── */

/* A native-fd decorator over the built-in POSIX provider whose io_status op both
 * counts and delegates to the hosted mapping. Because it advertises NATIVE_FD the
 * readiness async client accepts it, and its fds are real — so the watcher polls
 * them and the full connect/send/recv state machine runs over real loopback.
 * Every -1 the async client classifies now routes through THIS op (proving the
 * client no longer reads errno directly). */
typedef struct { int io_status_calls; } IoDeco;
static IoDeco g_cdeco;

static KlSocketHandle iod_socket(void *c, int d, int t, int p){(void)c;return kl_sockdef_socket(d,t,p);}
static int iod_connect(void *c, KlSocketHandle fd, const KlSockAddr *a){(void)c;return kl_sockdef_connect(fd,a);}
static kl_ssize_t iod_send(void *c, KlSocketHandle fd, const void *b, size_t n){(void)c;return kl_sockdef_send(fd,b,n);}
static kl_ssize_t iod_recv(void *c, KlSocketHandle fd, void *b, size_t n){(void)c;return kl_sockdef_recv(fd,b,n);}
static KlIoStatus iod_io_status(void *c){ ((IoDeco*)c)->io_status_calls++; return kl_sockdef_io_status(); }
static const KlSocketOps IOD_OPS = {
    .socket = iod_socket, .connect = iod_connect,
    .send = iod_send, .recv = iod_recv, .io_status = iod_io_status,
    .name = "io-deco",   /* everything else NULL → POSIX default */
};

/* Mock resolver: hands the async client one live 127.0.0.1:<port> (sync-complete). */
static KlResolveReq g_iod_req;
static int          g_iod_port;
static KlResolveReq *iod_resolve(KlResolver *self, KlEventCtx *ctx, const char *host,
                                  int port, KlResolveDoneFn done_fn, void *ud) {
    (void)ctx; (void)host; (void)port;
    g_iod_req.resolver = self;
    KlResolveResult r; memset(&r, 0, sizeof(r));
    r.ai_socktype = SOCK_STREAM; r.naddrs = 1;
    const uint8_t lo[4] = {127,0,0,1};
    kl_sockaddr_from_ipv4(&r.addrs[0], lo, (uint16_t)g_iod_port);
    done_fn(&g_iod_req, &r, 0, ud);
    return &g_iod_req;
}
static void iod_res_cancel(KlResolveReq *r){ (void)r; }
static void iod_res_destroy(KlResolver *s){ (void)s; }
static KlResolver g_iod_resolver = {
    .resolve = iod_resolve, .cancel = iod_res_cancel, .destroy = iod_res_destroy,
};

static void iod_handler(KlRequest *req, KlResponse *res, void *u) {
    (void)req; (void)u;
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}
static void *iod_server_thread(void *arg) { kl_server_run((KlServer *)arg); return NULL; }

typedef struct { int done, status; } IodCtx;
static void iod_done(KlClient *cl, void *ud) {
    IodCtx *x = ud;
    const KlClientResponse *r = kl_client_response(cl);
    x->status = r ? r->status : -1;
    x->done = 1;
}

UTEST(iostatus, async_client_consults_io_status_end_to_end) {
    memset(&g_cdeco, 0, sizeof(g_cdeco));

    KlServer srv;
    KlConfig scfg = { .port = 0, .max_connections = 8, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_server_init(&srv, &scfg));
    kl_server_route(&srv, "GET", "/ok", iod_handler, NULL, NULL);
    pthread_t tid;
    ASSERT_EQ(0, pthread_create(&tid, NULL, iod_server_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) usleep(10000);
    ASSERT_GT(srv.bound_port, 0);
    g_iod_port = srv.bound_port;

    KlSocketProvider cprov = { &IOD_OPS, &g_cdeco, KL_SOCK_CAP_NATIVE_FD, NULL };
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));

    KlClientConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &g_iod_resolver;
    cfg.timeout_ms = 2000;
    cfg.sockets = &cprov;

    IodCtx x = { 0, 0 };
    KlClient *c = kl_client_start(&ev, &a, &cfg, "GET", "http://host.test/ok",
                                   NULL, 0, NULL, 0, iod_done, &x);
    ASSERT_TRUE(c != NULL);
    for (int i = 0; i < 300 && !x.done; i++) {
        kl_event_ctx_run(&ev, 16, 10);
        kl_timer_fire(&ev);
    }

    ASSERT_TRUE(x.done);
    ASSERT_EQ(200, x.status);
    /* The async client classified at least one -1 I/O result through the
     * provider's io_status op (nonblocking connect EINPROGRESS and/or an EAGAIN
     * on recv while draining) — it never read errno itself. */
    ASSERT_GT(g_cdeco.io_status_calls, 0);

    kl_client_free(c);
    kl_event_ctx_free(&ev);
    kl_server_stop(&srv);
    pthread_join(tid, NULL);
    kl_server_free(&srv);
}

UTEST_MAIN();
