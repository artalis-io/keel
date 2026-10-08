/*
 * test_io_status.c: the portable I/O-result classification seam (freestanding
 * I/O-status contract). Proves:
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
#include "../src/event_caps.h"   /* kl_event_caps: the completion axis substitutes the provider */

#include <keel/http_client.h>
#include <keel/http_server.h>
#include <keel/resolver.h>
#include <keel/allocator.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/websocket.h>          /* KL_WS_MAGIC_GUID */
#include <keel/websocket_client.h>
#include <keel/http2_client.h>
#include "../src/platform_socket.h"  /* kl_plat_socket_runtime_init */
#include "../src/sha1.h"
#include "../src/base64.h"
#include <stdio.h>

#include <errno.h>
#include <string.h>
#if !defined(_MSC_VER)
#if !defined(_MSC_VER)
#include <unistd.h>
#endif   /* MSVC has no <unistd.h>; the harness helpers cover it */
#endif   /* MSVC has no <unistd.h>; usleep replaced by kl_test_sleep_ms */
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
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

/* The op, when present, is consulted, for every category, with errno untouched. */
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
    /* errno was never consulted by the seam; our poison survives. */
    ASSERT_EQ(0xBADF00D, errno);
}

/* connect returning PENDING via the op: the exact test the async client makes:
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
 * readiness async client accepts it, and its fds are real; so the watcher polls
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

static void iod_handler(KlHttpRequest *req, KlHttpResponse *res, void *u) {
    (void)req; (void)u;
    kl_http_response_json(res, 200, "{\"ok\":true}", 11);
}
static void iod_server_thread(void *arg) { kl_http_server_run((KlHttpServer *)arg); return; }

typedef struct { int done, status; } IodCtx;
static void iod_done(KlHttpClient *cl, void *ud) {
    IodCtx *x = ud;
    const KlHttpClientResponse *r = kl_http_client_response(cl);
    x->status = r ? r->status : -1;
    x->done = 1;
}

UTEST(iostatus, async_client_consults_io_status_end_to_end) {
    memset(&g_cdeco, 0, sizeof(g_cdeco));

    KlHttpServer srv;
    KlHttpServerConfig scfg = { .port = 0, .max_connections = 8, .bind_addr = "127.0.0.1" };
    ASSERT_EQ(0, kl_http_server_init(&srv, &scfg));
    kl_http_server_route(&srv, "GET", "/ok", iod_handler, NULL, NULL);
    KlPlatThread tid;
    ASSERT_EQ(0, kl_plat_thread_create(&tid, iod_server_thread, &srv));
    for (int i = 0; i < 200 && srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    ASSERT_GT(srv.bound_port, 0);
    g_iod_port = srv.bound_port;

    KlSocketProvider cprov = { &IOD_OPS, &g_cdeco, KL_SOCK_CAP_NATIVE_FD, NULL };
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev; ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));

    KlHttpClientConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &g_iod_resolver;
    cfg.timeout_ms = 2000;
    cfg.sockets = &cprov;

    IodCtx x = { 0, 0 };
    KlHttpClient *c = kl_http_client_start(&ev, &a, &cfg, "GET", "http://host.test/ok",
                                   NULL, 0, NULL, 0, iod_done, &x);
    ASSERT_TRUE(c != NULL);
    for (int i = 0; i < 300 && !x.done; i++) {
        kl_event_ctx_run(&ev, 16, 10);
        kl_timer_fire(&ev);
    }

    /* Capture BEFORE asserting, and tear down BEFORE asserting. utest's ASSERT_* macros RETURN from
     * the test body on failure, so asserting here would skip the stop/join below and leave the server
     * thread running on `srv`, which lives on this frame. The frame then dies under it and the thread
     * faults in iocp_accept_untrack with a dangling allocator. That is what #295 actually was: a
     * fixture that turns any assertion failure into a SIGSEGV, hiding the assertion it was trying to
     * report. Same class as #267. */
    int done = x.done, status = x.status, io_calls = g_cdeco.io_status_calls;
    int completion = (kl_event_caps(&ev.loop) & KL_EVENT_CAP_COMPLETION) ? 1 : 0;

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    kl_http_server_stop(&srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&srv);

    ASSERT_TRUE(done);
    ASSERT_EQ(200, status);
    /* The async client classified at least one -1 I/O result through the provider's io_status op
     * (nonblocking connect EINPROGRESS and/or an EAGAIN on recv while draining); it never read errno
     * itself.
     *
     * Readiness only. On a COMPLETION loop kl_http_client_start deliberately replaces a configured
     * provider that lacks KL_SOCK_CAP_OVERLAPPED with the backend's own overlapped provider, so that a
     * completion backend is a drop-in for the client. This decorator is readiness-shaped
     * (KL_SOCK_CAP_NATIVE_FD), so it is substituted away and its io_status is correctly never called.
     * The request still has to succeed, which the assertions above check on both axes. */
    if (!completion)
        ASSERT_GT(io_calls, 0);
}

/* ── Protocol code classifies through the provider, never a hosted errno ─────────────────────────
 *
 * A native-fd decorator over the built-in provider whose failing calls leave errno POISONED: the
 * real outcome is reported only through io_status. A consumer that still reads errno after a -1
 * sees EBADF and misclassifies a pending connect, a would-block read or an exhausted accept as a
 * hard error. Readiness only: on a completion loop the clients and the server replace a native-fd
 * provider with the backend's own overlapped one, so the decorator is never called there. */
static struct {
    KlIoStatus status;          /* what io_status reports for the last forced failure */
    int        forced;          /* the last call was a forced failure: report `status` */
    int        poison_connect;  /* connect reports pending through io_status only */
    int        spurious_reads;  /* recv calls still to fail as would-block before delegating */
    volatile int accept_fail;   /* accept fails as resource-exhausted while set */
    volatile int accept_calls;  /* accept calls while accept_fail was set */
} g_pz;

static void pz_fail(KlIoStatus st) {
    g_pz.status = st;
    g_pz.forced = 1;
    errno = EBADF;              /* the poison: nothing about the real outcome */
}
static int pz_connect(void *c, KlSocketHandle fd, const KlSockAddr *a) {
    (void)c;
    int rc = kl_sockdef_connect(fd, a);
    g_pz.forced = 0;
    if (!g_pz.poison_connect) return rc;
    if (rc == 0 || kl_sockdef_io_status() == KL_IO_PENDING) {
        pz_fail(KL_IO_PENDING);   /* pending even when it completed: the client waits for WRITE */
        return -1;
    }
    return rc;
}
static kl_ssize_t pz_recv(void *c, KlSocketHandle fd, void *b, size_t n) {
    (void)c;
    if (g_pz.spurious_reads > 0) {   /* a readiness wake with nothing to read */
        g_pz.spurious_reads--;
        pz_fail(KL_IO_WOULD_BLOCK);
        return -1;
    }
    g_pz.forced = 0;
    return kl_sockdef_recv(fd, b, n);
}
static kl_ssize_t pz_send(void *c, KlSocketHandle fd, const void *b, size_t n) {
    (void)c; g_pz.forced = 0; return kl_sockdef_send(fd, b, n);
}
static KlSocketHandle pz_accept(void *c, KlSocketHandle fd, KlSockAddr *peer) {
    (void)c;
    if (g_pz.accept_fail) {
        g_pz.accept_calls++;
        pz_fail(KL_IO_RESOURCE_EXHAUSTED);
        return KL_INVALID_SOCKET;
    }
    g_pz.forced = 0;
    return kl_sockdef_accept(fd, peer);
}
static KlIoStatus pz_io_status(void *c) {
    (void)c;
    if (g_pz.forced) return g_pz.status;
    return kl_sockdef_io_status();
}
static const KlSocketOps PZ_OPS = {
    .connect = pz_connect, .recv = pz_recv, .send = pz_send, .accept = pz_accept,
    .io_status = pz_io_status, .name = "errno-poison",   /* everything else: the native default */
};
static const KlSocketProvider PZ_PROV = { &PZ_OPS, NULL, KL_SOCK_CAP_NATIVE_FD, NULL };

/* A loopback listener the test drives by hand (no thread): returns the port, or -1. */
static KlSocketHandle g_pz_lfd = KL_INVALID_SOCKET;
static int pz_listen(void) {
    if (kl_plat_socket_runtime_init() != 0) return -1;
    KlSocketHandle fd = (KlSocketHandle)socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd)) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    if (bind((int)fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen((int)fd, 4) != 0 ||
        getsockname((int)fd, (struct sockaddr *)&a, &al) != 0) {
        kl_test_closesock(fd);
        return -1;
    }
    g_pz_lfd = fd;
    return ntohs(a.sin_port);
}
static void pz_unlisten(void) {
    if (kl_handle_valid(g_pz_lfd)) kl_test_closesock(g_pz_lfd);
    g_pz_lfd = KL_INVALID_SOCKET;
}

static int pz_completion_ctx(KlEventCtx *ev) {
    return (kl_event_caps(&ev->loop) & KL_EVENT_CAP_COMPLETION) != 0;
}

static KlHttp2ClientSession *pz_no_session(KlAllocator *a) { (void)a; return NULL; }
static void pz_h2_error(KlHttp2ClientConn *c, const char *m, void *u) { (void)c; (void)m; (void)u; }

/* A nonblocking connect reports "in progress" through the provider. The WebSocket and HTTP/2
 * clients read errno for it and gave up on every connect when the provider leaves errno alone. */
UTEST(iostatus, ws_and_h2_clients_take_connect_pending_from_the_provider) {
    memset(&g_pz, 0, sizeof(g_pz));
    g_pz.poison_connect = 1;
    int port = pz_listen();
    ASSERT_GT(port, 0);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    if (pz_completion_ctx(&ev)) {
        kl_event_ctx_free(&ev); pz_unlisten();
        UTEST_SKIP("readiness only: a completion loop replaces a native-fd provider");
    }
    ev.sockets = &PZ_PROV;

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/ws", port);
    KlWsClientCallbacks cbs; memset(&cbs, 0, sizeof(cbs));
    KlWsClientConn *ws = kl_ws_client_connect(&ev, &a, NULL, url, &cbs, NULL);
    int ws_ok = ws != NULL;
    if (ws) kl_ws_client_free(ws);

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    KlHttp2ClientConfig h2cfg; memset(&h2cfg, 0, sizeof(h2cfg));
    h2cfg.session = pz_no_session;   /* never reached: the connect stays pending */
    KlHttp2ClientConn *h2 = kl_http2_client_connect(&ev, &a, &h2cfg, url, pz_h2_error, NULL);
    int h2_ok = h2 != NULL;
    if (h2) kl_http2_client_free(h2);

    kl_event_ctx_free(&ev);
    pz_unlisten();
    printf("  connect pending: ws %s, h2 %s\n", ws_ok ? "kept" : "refused", h2_ok ? "kept" : "refused");
    ASSERT_TRUE(ws_ok);   /* was NULL: errno (EBADF) is not EINPROGRESS */
    ASSERT_TRUE(h2_ok);
}

static struct { int opened, errored; } g_pz_ws;
static void pz_ws_open(KlWsClientConn *ws, void *u) { (void)ws; (void)u; g_pz_ws.opened = 1; }
static void pz_ws_error(KlWsClientConn *ws, const char *m, void *u) {
    (void)ws; (void)m; (void)u; g_pz_ws.errored = 1;
}

/* Build the 101 answer to the upgrade request in `req`. 0, or -1 if it has no key. */
static int pz_upgrade_reply(const char *req, char *out, size_t cap) {
    const char *k = strstr(req, "Sec-WebSocket-Key: ");
    if (!k) return -1;
    k += strlen("Sec-WebSocket-Key: ");
    const char *e = strstr(k, "\r\n");
    if (!e || e - k > 64) return -1;
    char cat[128];
    size_t kl = (size_t)(e - k);
    memcpy(cat, k, kl);
    memcpy(cat + kl, KL_WS_MAGIC_GUID, strlen(KL_WS_MAGIC_GUID));
    uint8_t dig[20];
    kl_sha1(cat, kl + strlen(KL_WS_MAGIC_GUID), dig);
    char acc[64];
    size_t ol = 0;
    kl_base64_encode(dig, sizeof dig, acc, &ol);
    acc[ol] = '\0';
    snprintf(out, cap, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                       "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    return 0;
}

/* A read that would block, reported through the provider: the WebSocket client must wait for the
 * next readiness, not fail the handshake. The connect is left unpoisoned so only the read is tested. */
UTEST(iostatus, ws_client_takes_read_would_block_from_the_provider) {
    memset(&g_pz, 0, sizeof(g_pz));
    memset(&g_pz_ws, 0, sizeof(g_pz_ws));
    int port = pz_listen();
    ASSERT_GT(port, 0);
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(0, kl_event_ctx_init(&ev, &a));
    if (pz_completion_ctx(&ev)) {
        kl_event_ctx_free(&ev); pz_unlisten();
        UTEST_SKIP("readiness only: a completion loop replaces a native-fd provider");
    }
    ev.sockets = &PZ_PROV;

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/ws", port);
    KlWsClientCallbacks cbs; memset(&cbs, 0, sizeof(cbs));
    cbs.on_open = pz_ws_open;
    cbs.on_error = pz_ws_error;
    KlWsClientConn *ws = kl_ws_client_connect(&ev, &a, NULL, url, &cbs, NULL);

    KlSocketHandle peer = KL_INVALID_SOCKET;
    int replied = 0;
    if (ws && kl_test_poll1(g_pz_lfd, 0, 2000) > 0)
        peer = (KlSocketHandle)accept((int)g_pz_lfd, NULL, NULL);
    if (kl_handle_valid(peer)) {
        /* Drive the client until its upgrade request has arrived. */
        char req[2048];
        size_t got = 0;
        req[0] = '\0';
        for (int i = 0; i < 200 && !strstr(req, "\r\n\r\n") && !g_pz_ws.errored; i++) {
            kl_event_ctx_run(&ev, 16, 10);
            while (got < sizeof(req) - 1 && kl_test_poll1(peer, 0, 0) > 0) {
                long n = kl_test_sockread(peer, req + got, sizeof(req) - 1 - got);
                if (n <= 0) break;
                got += (size_t)n;
                req[got] = '\0';
            }
        }
        char reply[512];
        if (strstr(req, "\r\n\r\n") && pz_upgrade_reply(req, reply, sizeof(reply)) == 0) {
            g_pz.spurious_reads = 1;   /* the first read after the reply would block */
            replied = kl_test_sockwrite(peer, reply, strlen(reply)) == (long)strlen(reply);
        }
        for (int i = 0; i < 200 && replied && !g_pz_ws.opened && !g_pz_ws.errored; i++)
            kl_event_ctx_run(&ev, 16, 10);
    }
    int opened = g_pz_ws.opened, errored = g_pz_ws.errored, left = g_pz.spurious_reads;

    if (ws) kl_ws_client_free(ws);
    if (kl_handle_valid(peer)) kl_test_closesock(peer);
    kl_event_ctx_free(&ev);
    pz_unlisten();

    ASSERT_TRUE(ws != NULL);
    ASSERT_TRUE(replied);
    ASSERT_EQ(0, left);           /* the would-block read happened */
    ASSERT_FALSE(errored);        /* was: "handshake read failed" (errno EBADF) */
    ASSERT_TRUE(opened);
}

/* The readiness accept loop backs off when the provider reports the accept as resource-exhausted
 * (out of descriptors or memory), instead of retrying on every wake of the still-readable listen
 * socket. It read errno for that, so a provider that reports by status alone made it spin. */
static KlHttpServer g_pz_srv;
static void pz_srv_thread(void *arg) { kl_http_server_run((KlHttpServer *)arg); }

UTEST(iostatus, accept_loop_backs_off_on_provider_resource_exhausted) {
    memset(&g_pz, 0, sizeof(g_pz));
    KlHttpServerConfig scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.port = 0;
    scfg.max_connections = 4;
    scfg.bind_addr = "127.0.0.1";
    scfg.sockets = &PZ_PROV;
    ASSERT_EQ(0, kl_http_server_init(&g_pz_srv, &scfg));
    if (kl_event_caps(&g_pz_srv.ev.loop) & KL_EVENT_CAP_COMPLETION) {
        kl_http_server_free(&g_pz_srv);
        UTEST_SKIP("readiness only: the completion accept path does not call the provider's accept");
    }
    kl_http_server_route(&g_pz_srv, "GET", "/ok", iod_handler, NULL, NULL);
    KlPlatThread tid;
    ASSERT_EQ(0, kl_plat_thread_create(&tid, pz_srv_thread, &g_pz_srv));
    for (int i = 0; i < 200 && g_pz_srv.bound_port == 0; i++) kl_test_sleep_ms(10);
    int port = g_pz_srv.bound_port;

    g_pz.accept_fail = 1;
    KlSocketHandle cfd = (KlSocketHandle)socket(AF_INET, SOCK_STREAM, 0);
    int connected = -1;
    if (port > 0 && kl_handle_valid(cfd)) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connected = connect((int)cfd, (struct sockaddr *)&sa, sizeof(sa));
    }
    kl_test_sleep_ms(400);              /* every accept fails meanwhile */
    int calls = g_pz.accept_calls;
    g_pz.accept_fail = 0;               /* resources are back: the queued connection is served */

    char buf[256];
    long got = -1;
    if (connected == 0) {
        const char *req = "GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        if (kl_test_sockwrite(cfd, req, strlen(req)) == (long)strlen(req) &&
            kl_test_poll1(cfd, 0, 3000) > 0)
            got = kl_test_sockread(cfd, buf, sizeof(buf) - 1);
    }
    if (kl_handle_valid(cfd)) kl_test_closesock(cfd);
    kl_http_server_stop(&g_pz_srv);
    kl_plat_thread_join(&tid);
    kl_http_server_free(&g_pz_srv);

    ASSERT_GT(port, 0);
    ASSERT_EQ(0, connected);
    printf("  accept attempts while exhausted: %d\n", calls);
    ASSERT_GT(calls, 0);
    /* 400 ms with a 100 ms back-off: a handful of attempts. A loop that ignores the status retries
     * on every wake of the readable listen socket, thousands of times. */
    ASSERT_LT(calls, 20);
    ASSERT_GT(got, 0L);
    buf[got > 0 ? got : 0] = '\0';
    ASSERT_EQ(0, strncmp(buf, "HTTP/1.1 200", 12));
}

UTEST_MAIN();
