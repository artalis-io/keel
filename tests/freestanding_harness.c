/*
 * freestanding_harness.c — F-7 host mock harness: proves libkeel_freestanding.a
 * actually RUNS the async HTTP/1.1 client end-to-end, on the host, over fully
 * MOCKED (non-hosted) platform hooks + socket provider + completion event
 * provider — no real syscalls, no errno, no loopback socket. This is the
 * freestanding-phase acceptance milestone
 * (docs/phase10_uefi_feasibility_design.md: "performs an HTTP/1.1 GET over a mock
 * completion provider") BEFORE any UEFI/firmware toolchain work.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * HOW THE CLIENT DRIVES A COMPLETION LOOP (the model the mocks reproduce)
 * ─────────────────────────────────────────────────────────────────────────
 * The async KlHttpClient on a completion loop (KL_EVENT_CAP_COMPLETION) uses an
 * EMULATED-READINESS shape identical to the shipped pollcomp/lwip-raw backends:
 *
 *   - CONNECT rides the completion axis: the client posts kl_comp_post_connect
 *     against a DETACHED tagged watcher; the backend's drain surfaces a
 *     KL_COMP_CONNECT (mask KL_EVENT_WRITE = connected) which completion_core.c
 *     routes to the client's connect watcher (he_on_connect_result).
 *   - SEND/RECV ride the socket provider's SYNCHRONOUS send/recv ops: the client
 *     arms KL_EVENT_WRITE/READ via kl_watcher_mod (registering a tagged watch on
 *     the backend), then calls kl_sock_send/kl_sock_recv; on a -1 it classifies
 *     via kl_sock_io_status (WOULD_BLOCK → kl_watcher_rearm and wait). The
 *     backend's drain relays each armed watch as a KL_COMP_WATCHER carrying the
 *     ready mask, which re-drives the client's state handler → it retries the
 *     send/recv and consumes the next scripted step.
 *
 * So a pure mock needs exactly two cooperating objects (the U-2 / U-3 seams):
 *   1. MOCK SOCKET PROVIDER (KlSocketProvider, KL_SOCK_CAP_OVERLAPPED) — a
 *      per-fd script of connect / send / recv results; no syscall, no errno,
 *      io_status reports the category directly.
 *   2. MOCK COMPLETION EVENT PROVIDER (KlEventProvider + KlCompletionOps,
 *      KL_EVENT_CAP_COMPLETION) — an in-memory loop: post_connect queues a
 *      connect completion, drain emits KL_COMP_CONNECT + KL_COMP_WATCHER for
 *      armed watches, cancel drops queued connect ops. native_provider returns
 *      the mock socket provider so kl_event_ctx_sockets_compatible passes.
 *
 * Every scenario is DETERMINISTIC (the mock clock is advanced, never slept on)
 * and asserts BOTH the observable result AND that every allocation is freed
 * (built + run under ASan+UBSan+LSan by `make freestanding-harness`).
 */

#include <keel/http_client.h>
#include <keel/resolver.h>
#include <keel/allocator.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>

#include "../src/socket.h"       /* KlSocketProvider / KlSocketOps / KlIoStatus */
#include "../src/completion.h"   /* KlCompletionOps / KlCompletionEvent / KL_COMP_* */
#include "freestanding_platform_test.h"  /* fs_clock_* */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ══════════════════════════════════════════════════════════════════════
 * Test harness plumbing
 * ══════════════════════════════════════════════════════════════════════ */

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ══════════════════════════════════════════════════════════════════════
 * A counting allocator (LSan double-checks; this makes leaks LOUD + lets us
 * inject a failing allocation at a chosen call index — scenario 9).
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    KlAllocator base;
    long live;          /* outstanding allocations */
    long total;         /* cumulative mallocs */
    long fail_at;       /* -1 = never; else the (1-based) malloc# that returns NULL */
} CountAlloc;

static void *ca_malloc(void *ctx, size_t n) {
    CountAlloc *a = ctx;
    a->total++;
    if (a->fail_at > 0 && a->total == a->fail_at) return NULL;
    void *p = malloc(n);
    if (p) a->live++;
    return p;
}
static void *ca_realloc(void *ctx, void *p, size_t oldn, size_t newn) {
    CountAlloc *a = ctx;
    (void)oldn;
    void *np = realloc(p, newn);
    if (!p && np) a->live++;   /* realloc(NULL,..) == malloc */
    return np;
}
static void ca_free(void *ctx, void *p, size_t n) {
    CountAlloc *a = ctx;
    (void)n;
    if (p) a->live--;
    free(p);
}
static void ca_init(CountAlloc *a) {
    memset(a, 0, sizeof(*a));
    a->base.malloc = ca_malloc;
    a->base.realloc = ca_realloc;
    a->base.free = ca_free;
    a->base.ctx = a;
    a->fail_at = -1;
}

/* ══════════════════════════════════════════════════════════════════════
 * MOCK SOCKET PROVIDER — per-fd scripted results, no syscalls, no errno.
 * ══════════════════════════════════════════════════════════════════════ */

/* One scripted recv step: hand back `data[0..len)` (len==0 => peer close), or a
 * transient status (WOULD_BLOCK forces a re-arm; the client retries next tick). */
typedef struct {
    KlIoStatus status;      /* KL_IO_OK => deliver bytes; else -1 with this class */
    const char *data;       /* bytes for KL_IO_OK (may be a slice of a response) */
    size_t      len;        /* 0 with KL_IO_OK => peer close (recv returns 0) */
} RecvStep;

/* One scripted send step: accept `accept` bytes (partial allowed), or -1 with a
 * status (WOULD_BLOCK => client re-arms and retries the remainder). */
typedef struct {
    KlIoStatus status;      /* KL_IO_OK => accept `accept` bytes; else -1 */
    size_t     accept;      /* bytes consumed on KL_IO_OK (0..request len) */
} SendStep;

#define MAX_STEPS 16
#define MAX_FDS   8

typedef struct {
    int            in_use;
    KlSocketHandle fd;

    /* connect: result surfaced by the completion provider (see mock loop). */
    int            connect_ok;      /* 1 = connect succeeds, 0 = refused */

    /* send script */
    SendStep       send_steps[MAX_STEPS];
    int            send_n, send_i;

    /* recv script */
    RecvStep       recv_steps[MAX_STEPS];
    int            recv_n, recv_i;

    int            closed;
} MockFd;

typedef struct {
    MockFd      fds[MAX_FDS];
    KlIoStatus  last_status;   /* what io_status() reports for the last -1 op */
} MockSock;

static MockSock g_sock;

static MockFd *mock_fd_lookup(KlSocketHandle fd) {
    for (int i = 0; i < MAX_FDS; i++)
        if (g_sock.fds[i].in_use && g_sock.fds[i].fd == fd)
            return &g_sock.fds[i];
    return NULL;
}

static KlSocketHandle msock_socket(void *c, int d, int t, int p) {
    (void)c; (void)d; (void)t; (void)p;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!g_sock.fds[i].in_use) {
            memset(&g_sock.fds[i], 0, sizeof(g_sock.fds[i]));
            g_sock.fds[i].in_use = 1;
            g_sock.fds[i].fd = (KlSocketHandle)(1000 + i);
            g_sock.fds[i].connect_ok = 1;   /* default: connects */
            return g_sock.fds[i].fd;
        }
    }
    return KL_INVALID_SOCKET;
}
static int msock_connect(void *c, KlSocketHandle fd, const KlSockAddr *a) {
    (void)c; (void)a;
    /* On a completion loop the client never calls connect() directly — the
     * completion provider's post_connect drives it. But be safe: report PENDING. */
    (void)fd;
    g_sock.last_status = KL_IO_PENDING;
    return -1;
}
static int msock_close(void *c, KlSocketHandle fd) {
    (void)c;
    MockFd *m = mock_fd_lookup(fd);
    if (m) { m->closed = 1; m->in_use = 0; }
    return 0;
}
static int msock_set_nonblocking(void *c, KlSocketHandle fd) { (void)c;(void)fd; return 0; }
static void msock_set_nosigpipe(void *c, KlSocketHandle fd) { (void)c;(void)fd; }
static int msock_get_so_error(void *c, KlSocketHandle fd, int *e) {
    (void)c;
    const MockFd *m = mock_fd_lookup(fd);
    if (e) *e = (m && m->connect_ok) ? 0 : 111 /* ECONNREFUSED-ish */;
    return 0;
}
static KlIoStatus msock_io_status(void *c) { (void)c; return g_sock.last_status; }

static kl_ssize_t msock_send(void *c, KlSocketHandle fd, const void *b, size_t n) {
    (void)c; (void)b;
    MockFd *m = mock_fd_lookup(fd);
    if (!m) { g_sock.last_status = KL_IO_FATAL; return -1; }
    if (m->send_i >= m->send_n) {
        /* No more scripted send steps: accept the rest (drain the request). */
        g_sock.last_status = KL_IO_OK;
        return (kl_ssize_t)n;
    }
    SendStep *s = &m->send_steps[m->send_i++];
    if (s->status != KL_IO_OK) { g_sock.last_status = s->status; return -1; }
    size_t take = s->accept < n ? s->accept : n;
    g_sock.last_status = KL_IO_OK;
    return (kl_ssize_t)take;
}

static kl_ssize_t msock_recv(void *c, KlSocketHandle fd, void *b, size_t n) {
    (void)c;
    MockFd *m = mock_fd_lookup(fd);
    if (!m) { g_sock.last_status = KL_IO_FATAL; return -1; }
    if (m->recv_i >= m->recv_n) {
        /* Script exhausted: behave as WOULD_BLOCK so the client parks (a scenario
         * that wants completion must script a final OK/close step). */
        g_sock.last_status = KL_IO_WOULD_BLOCK;
        return -1;
    }
    RecvStep *s = &m->recv_steps[m->recv_i++];
    if (s->status != KL_IO_OK) { g_sock.last_status = s->status; return -1; }
    if (s->len == 0) { g_sock.last_status = KL_IO_OK; return 0; }  /* peer close */
    size_t take = s->len < n ? s->len : n;
    memcpy(b, s->data, take);
    g_sock.last_status = KL_IO_OK;
    return (kl_ssize_t)take;
}

static const KlSocketOps MOCK_SOCK_OPS = {
    .socket = msock_socket,
    .connect = msock_connect,
    .close = msock_close,
    .set_nonblocking = msock_set_nonblocking,
    .set_nosigpipe = msock_set_nosigpipe,
    .get_so_error = msock_get_so_error,
    .send = msock_send,
    .recv = msock_recv,
    .io_status = msock_io_status,
    .name = "mock-freestanding-sock",
};

/* KL_SOCK_CAP_OVERLAPPED: a completion loop requires it (kl_caps_compatible). */
static KlSocketProvider g_sock_provider = {
    &MOCK_SOCK_OPS, &g_sock, KL_SOCK_CAP_OVERLAPPED, NULL,
};

/* ══════════════════════════════════════════════════════════════════════
 * MOCK COMPLETION EVENT PROVIDER — in-memory loop, no OS poll.
 * ══════════════════════════════════════════════════════════════════════ */

/* A queued outbound connect op (post_connect). Its completion is surfaced as a
 * KL_COMP_CONNECT on the next drain, then retired (exactly-once terminal). */
typedef struct {
    int            in_use;
    KlSocketHandle fd;
    void          *watcher_udata;   /* the tagged KlWatcher the client registered */
} MockConnectOp;

/* A registered tagged watch (add/mod) — the emulated-readiness relay. drain
 * surfaces its armed mask as a KL_COMP_WATCHER so the client retries send/recv. */
typedef struct {
    int            in_use;
    KlSocketHandle fd;
    KlEventMask    mask;
    void          *udata;           /* tagged KlWatcher pointer */
} MockWatch;

#define MAX_OPS 8
typedef struct {
    MockConnectOp conns[MAX_OPS];
    MockWatch     watches[MAX_OPS];
} MockLoop;

static MockLoop g_loop;

/* ── KlEventOps (the readiness face — connection fds are op-driven, only tagged
 *    watchers register a relay, exactly like pollcomp) ──────────────────── */

static int mloop_init(KlEventLoop *loop) { (void)loop; memset(&g_loop, 0, sizeof(g_loop)); return 0; }

static int mloop_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop;
    if (!((uintptr_t)udata & 1)) return 0;   /* untagged (connection) — op-driven */
    for (int i = 0; i < MAX_OPS; i++) {
        if (g_loop.watches[i].in_use && g_loop.watches[i].udata == udata) {
            g_loop.watches[i].mask = mask; g_loop.watches[i].fd = fd; return 0;
        }
    }
    for (int i = 0; i < MAX_OPS; i++) {
        if (!g_loop.watches[i].in_use) {
            g_loop.watches[i].in_use = 1;
            g_loop.watches[i].fd = fd;
            g_loop.watches[i].mask = mask;
            g_loop.watches[i].udata = udata;
            return 0;
        }
    }
    return -1;
}
static int mloop_mod(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    if (!((uintptr_t)udata & 1)) return 0;
    for (int i = 0; i < MAX_OPS; i++)
        if (g_loop.watches[i].in_use && g_loop.watches[i].udata == udata) {
            g_loop.watches[i].mask = mask; g_loop.watches[i].fd = fd; return 0;
        }
    return mloop_add(loop, fd, mask, udata);
}
static int mloop_del(KlEventLoop *loop, KlSocketHandle fd) {
    (void)loop;
    for (int i = 0; i < MAX_OPS; i++)
        if (g_loop.watches[i].in_use && g_loop.watches[i].fd == fd)
            g_loop.watches[i].in_use = 0;
    return 0;
}
static int mloop_wait(KlEventLoop *loop, KlEvent *out, int max, int timeout_ms) {
    /* Completion loops drive via kl_comp_run/drain, not this readiness wait. */
    (void)loop; (void)out; (void)max; (void)timeout_ms; return 0;
}
static void mloop_close(KlEventLoop *loop) { (void)loop; }
static unsigned mloop_caps(const KlEventLoop *loop) { (void)loop; return KL_EVENT_CAP_COMPLETION; }
static const struct KlSocketProvider *mloop_native_provider(const KlEventLoop *loop) {
    (void)loop; return &g_sock_provider;
}

/* ── KlCompletionOps ──────────────────────────────────────────────────── */

static int mloop_post_connect(struct KlEventCtx *ctx, KlSocketHandle fd,
                              const KlSockAddr *addr, void *watcher_udata) {
    (void)ctx; (void)addr;
    for (int i = 0; i < MAX_OPS; i++) {
        if (!g_loop.conns[i].in_use) {
            g_loop.conns[i].in_use = 1;
            g_loop.conns[i].fd = fd;
            g_loop.conns[i].watcher_udata = watcher_udata;
            return 0;
        }
    }
    return -1;
}

static void mloop_cancel(struct KlEventCtx *ctx, KlSocketHandle fd) {
    (void)ctx;
    /* Drop any queued connect op on this fd (stale completion must not fire). */
    for (int i = 0; i < MAX_OPS; i++)
        if (g_loop.conns[i].in_use && g_loop.conns[i].fd == fd)
            g_loop.conns[i].in_use = 0;
}

/* drain: emit each queued connect completion (exactly-once), then relay each
 * armed tagged watch as a KL_COMP_WATCHER (level-triggered, persistent — the
 * client re-arms it while it still needs to send/recv). */
static int mloop_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout_ms) {
    (void)ctx; (void)timeout_ms;
    int count = 0;

    /* Connect completions — one terminal edge each, then retire the op. */
    for (int i = 0; i < MAX_OPS && count < max; i++) {
        if (!g_loop.conns[i].in_use) continue;
        MockConnectOp *op = &g_loop.conns[i];
        const MockFd *m = mock_fd_lookup(op->fd);
        int connected = (m && m->connect_ok);
        memset(&out[count], 0, sizeof(out[count]));
        out[count].kind = KL_COMP_CONNECT;
        out[count].target = op->watcher_udata;
        out[count].bytes = connected ? (size_t)KL_EVENT_WRITE : 0;
        out[count].ok = connected;
        count++;
        op->in_use = 0;   /* retire: exactly-once terminal result */
    }

    /* Emulated-readiness relay: each armed watch → a KL_COMP_WATCHER carrying its
     * mask. The client's send/recv handlers consume the next scripted step; when
     * a step is WOULD_BLOCK the client re-arms (kl_watcher_rearm → mod) and the
     * next drain relays it again, so progress is driven purely by the script. */
    for (int i = 0; i < MAX_OPS && count < max; i++) {
        if (!g_loop.watches[i].in_use) continue;
        memset(&out[count], 0, sizeof(out[count]));
        out[count].kind = KL_COMP_WATCHER;
        out[count].target = g_loop.watches[i].udata;
        out[count].bytes = (size_t)g_loop.watches[i].mask;
        out[count].ok = 1;
        count++;
    }
    return count;
}

/* Ops the client never exercises on the client-only completion path get NULL —
 * server/UDP primitives are out of the freestanding archive. */
static const KlCompletionOps MOCK_COMP_OPS = {
    .drain = mloop_drain,
    .post_connect = mloop_post_connect,
    .cancel = mloop_cancel,
    /* prime_accepts/post_recv/post_send/post_accept/post_sendfile/post_udp_* =
     * NULL: unreached by the async client (it uses connect completion + sync
     * send/recv + watcher relay). */
};

static const KlEventOps MOCK_EVENT_OPS = {
    .init = mloop_init, .add = mloop_add, .mod = mloop_mod, .del = mloop_del,
    .wait = mloop_wait, .close = mloop_close, .caps = mloop_caps,
    .native_provider = mloop_native_provider,
    .completion = &MOCK_COMP_OPS,
};
static const KlEventProvider MOCK_EVENT_PROVIDER = { &MOCK_EVENT_OPS, "mock-freestanding-comp" };

/* ══════════════════════════════════════════════════════════════════════
 * MOCK RESOLVER — returns a fixed numeric address synchronously.
 * ══════════════════════════════════════════════════════════════════════ */

static KlResolveReq g_resolve_req;
static KlResolveReq *mock_resolve(KlResolver *self, KlEventCtx *ctx, const char *host,
                                  int port, KlResolveDoneFn done_fn, void *ud) {
    (void)ctx; (void)host;
    g_resolve_req.resolver = self;
    KlResolveResult r; memset(&r, 0, sizeof(r));
    r.ai_socktype = SOCK_STREAM; r.naddrs = 1;
    const uint8_t ip[4] = { 203, 0, 113, 7 };   /* TEST-NET-3 (never a real host) */
    kl_sockaddr_from_ipv4(&r.addrs[0], ip, (uint16_t)port);
    done_fn(&g_resolve_req, &r, 0, ud);          /* sync completion (contract) */
    return &g_resolve_req;
}
static void mock_res_cancel(KlResolveReq *r) { (void)r; }
static void mock_res_destroy(KlResolver *s) { (void)s; }
static KlResolver g_resolver = {
    .resolve = mock_resolve, .cancel = mock_res_cancel, .destroy = mock_res_destroy,
};

/* ══════════════════════════════════════════════════════════════════════
 * Scenario driver
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct { int done; int status; KlError err; size_t body_len; char body[512]; } DoneCtx;

static void on_done(KlHttpClient *cl, void *ud) {
    DoneCtx *d = ud;
    const KlHttpClientResponse *r = kl_http_client_response(cl);
    if (r) {
        d->status = r->status;
        d->body_len = r->body_len;
        if (r->body && r->body_len < sizeof(d->body))
            memcpy(d->body, r->body, r->body_len);
    } else {
        d->status = -1;
    }
    d->err = kl_http_client_last_error(cl);
    d->done = 1;
}

/* Reset the global mock state between scenarios. */
static void mocks_reset(void) {
    memset(&g_sock, 0, sizeof(g_sock));
    memset(&g_loop, 0, sizeof(g_loop));
    g_sock.last_status = KL_IO_OK;
    fs_clock_set(1000);
}

/* Pump the loop a bounded number of ticks (no real waiting — the mock drain
 * returns immediately; timers fire off the advanceable clock). */
static void pump(KlEventCtx *ev, const DoneCtx *d, int max_ticks) {
    for (int i = 0; i < max_ticks && !d->done; i++) {
        kl_event_ctx_run(ev, 16, 0);
        kl_timer_fire(ev);
    }
}

static const char HTTP_200[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello";

/* Split HTTP_200 into two slices for fragmentation scenarios. */
#define HTTP_200_LEN (sizeof(HTTP_200) - 1)

/* Program a single fd's recv script to deliver the full 200 in one OK step. */
static void script_full_200(MockFd *m) {
    m->recv_steps[0] = (RecvStep){ KL_IO_OK, HTTP_200, HTTP_200_LEN };
    m->recv_n = 1;
}

/* ── The scenarios ─────────────────────────────────────────────────────── */

/* Common setup: a client over the mock completion loop + mock socket provider +
 * mock resolver. Returns the started client (or NULL). */
static KlHttpClient *start_client(KlEventCtx *ev, KlAllocator *a, DoneCtx *d,
                              int timeout_ms) {
    KlHttpClientConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &g_resolver;
    cfg.timeout_ms = timeout_ms;
    memset(d, 0, sizeof(*d));
    return kl_http_client_start(ev, a, &cfg, "GET", "http://host.test/x",
                           NULL, 0, NULL, 0, on_done, d);
}

static void scenario_1_happy(void) {
    printf("[1] happy path: connect -> send -> recv 200\n");
    mocks_reset();
    CountAlloc a; ca_init(&a);
    KlEventCtx ev;
    CHECK(kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER) == 0, "ctx init");

    DoneCtx d;
    KlHttpClient *c = start_client(&ev, &a.base, &d, 5000);
    CHECK(c != NULL, "client start");
    /* The socket fd is created during connect (first tick). Script recv after the
     * first pump so the fd exists — but simpler: script via a post-connect hook.
     * Here we pump one tick to let the socket be created + connect complete, then
     * script the recv on the live fd. */
    pump(&ev, &d, 3);
    MockFd *m = &g_sock.fds[0];
    CHECK(m->in_use, "fd created");
    script_full_200(m);
    pump(&ev, &d, 20);

    CHECK(d.done, "completed");
    CHECK(d.status == 200, "status 200");
    CHECK(d.body_len == 5 && memcmp(d.body, "hello", 5) == 0, "body == hello");

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leak (all freed)");
}

static void scenario_2_refused(void) {
    printf("[2] connect refused -> error, clean up\n");
    mocks_reset();
    CountAlloc a; ca_init(&a);
    KlEventCtx ev;
    kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER);

    DoneCtx d;
    KlHttpClient *c = start_client(&ev, &a.base, &d, 5000);
    CHECK(c != NULL, "client start");
    /* The mock resolver completes synchronously inside kl_http_client_start, so the
     * socket is already created + the connect op queued when start returns. Mark
     * the fd refused; drain reads connect_ok LIVE and surfaces KL_COMP_CONNECT(ok=0). */
    CHECK(g_sock.fds[0].in_use, "fd created at start");
    g_sock.fds[0].connect_ok = 0;
    pump(&ev, &d, 20);

    CHECK(d.done, "completed");
    CHECK(d.status == -1, "no response");
    CHECK(d.err == KL_ERR_CONNECT || d.err == KL_ERR_IO, "connect error");

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leak");
}

static void scenario_3_partial_send(void) {
    printf("[3] partial send + WOULD_BLOCK re-arm -> 200\n");
    mocks_reset();
    CountAlloc a; ca_init(&a);
    KlEventCtx ev;
    kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER);

    DoneCtx d;
    KlHttpClient *c = start_client(&ev, &a.base, &d, 5000);
    CHECK(c != NULL, "client start");
    /* Script BEFORE pumping: the sync mock resolver already created the fd, and the
     * SENDING state runs on the first pump (right after the connect completion), so
     * the send script must be in place first. */
    MockFd *m = &g_sock.fds[0];
    CHECK(m->in_use, "fd created at start");
    /* send: 10 bytes, then WOULD_BLOCK (forces re-arm), then accept the rest. */
    m->send_steps[0] = (SendStep){ KL_IO_OK, 10 };
    m->send_steps[1] = (SendStep){ KL_IO_WOULD_BLOCK, 0 };
    m->send_n = 2;
    script_full_200(m);
    pump(&ev, &d, 30);

    CHECK(d.done, "completed");
    CHECK(d.status == 200, "status 200");
    CHECK(m->send_i >= 2, "partial send path exercised");

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leak");
}

static void scenario_4_fragmented_recv(void) {
    printf("[4] fragmented recv (3 chunks + WOULD_BLOCK) -> 200\n");
    mocks_reset();
    CountAlloc a; ca_init(&a);
    KlEventCtx ev;
    kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER);

    DoneCtx d;
    KlHttpClient *c = start_client(&ev, &a.base, &d, 5000);
    CHECK(c != NULL, "client start");
    pump(&ev, &d, 3);
    MockFd *m = &g_sock.fds[0];
    CHECK(m->in_use, "fd created");
    /* Deliver the response in 3 slices with a WOULD_BLOCK between the first two. */
    size_t s1 = 12, s2 = 20;   /* arbitrary split points within HTTP_200 */
    m->recv_steps[0] = (RecvStep){ KL_IO_OK, HTTP_200, s1 };
    m->recv_steps[1] = (RecvStep){ KL_IO_WOULD_BLOCK, NULL, 0 };
    m->recv_steps[2] = (RecvStep){ KL_IO_OK, HTTP_200 + s1, s2 - s1 };
    m->recv_steps[3] = (RecvStep){ KL_IO_OK, HTTP_200 + s2, HTTP_200_LEN - s2 };
    m->recv_n = 4;
    pump(&ev, &d, 40);

    CHECK(d.done, "completed");
    CHECK(d.status == 200, "status 200");
    CHECK(d.body_len == 5 && memcmp(d.body, "hello", 5) == 0, "reassembled body");

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leak");
}

static void scenario_5_peer_close(void) {
    printf("[5] peer close mid-response -> error, clean up\n");
    mocks_reset();
    CountAlloc a; ca_init(&a);
    KlEventCtx ev;
    kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER);

    DoneCtx d;
    KlHttpClient *c = start_client(&ev, &a.base, &d, 5000);
    CHECK(c != NULL, "client start");
    pump(&ev, &d, 3);
    MockFd *m = &g_sock.fds[0];
    CHECK(m->in_use, "fd created");
    /* A partial header, then peer close (recv returns 0) before status is parsed. */
    m->recv_steps[0] = (RecvStep){ KL_IO_OK, "HTTP/1.1 2", 10 };
    m->recv_steps[1] = (RecvStep){ KL_IO_OK, NULL, 0 };   /* len 0 => close */
    m->recv_n = 2;
    pump(&ev, &d, 30);

    CHECK(d.done, "completed");
    CHECK(d.status == -1, "no valid response");
    CHECK(d.err != KL_ERR_NONE, "error reported");

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leak");
}

static void scenario_6_timeout(void) {
    printf("[6] deadline (advance mock clock past timeout) -> error\n");
    mocks_reset();
    CountAlloc a; ca_init(&a);
    KlEventCtx ev;
    kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER);

    DoneCtx d;
    KlHttpClient *c = start_client(&ev, &a.base, &d, 1000);   /* 1s deadline */
    CHECK(c != NULL, "client start");
    pump(&ev, &d, 3);
    const MockFd *m = &g_sock.fds[0];
    CHECK(m->in_use, "fd created");
    /* Never script a response: the client parks in RECEIVING. Advance the clock
     * past the deadline, then pump so the deadline timer fires. */
    CHECK(!d.done, "not done before deadline");
    fs_clock_advance(2000);   /* now past the 1000ms deadline */
    pump(&ev, &d, 10);

    CHECK(d.done, "completed via deadline");
    CHECK(d.status == -1, "no response");
    CHECK(d.err == KL_ERR_TIMEOUT, "timeout error");

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leak");
}

static void scenario_7_cancel(void) {
    printf("[7] cancel mid-flight (after connect, before response) -> clean\n");
    mocks_reset();
    CountAlloc a; ca_init(&a);
    KlEventCtx ev;
    kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER);

    DoneCtx d;
    KlHttpClient *c = start_client(&ev, &a.base, &d, 5000);
    CHECK(c != NULL, "client start");
    pump(&ev, &d, 3);   /* connect completes; client now in SENDING/RECEIVING */
    CHECK(!d.done, "still in flight");
    kl_http_client_cancel(c);
    /* A further pump must NOT resurrect the client or touch freed memory. */
    pump(&ev, &d, 10);

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leak after cancel");
}

static void scenario_8_size_cap(void) {
    printf("[8] response-size cap exceeded -> error, clean up\n");
    mocks_reset();
    CountAlloc a; ca_init(&a);
    KlEventCtx ev;
    kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER);

    KlHttpClientConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.resolver = &g_resolver;
    cfg.timeout_ms = 5000;
    cfg.max_response_size = 16;   /* tiny cap: a real 200 body overflows it */
    DoneCtx d; memset(&d, 0, sizeof(d));
    KlHttpClient *c = kl_http_client_start(&ev, &a.base, &cfg, "GET", "http://host.test/x",
                                  NULL, 0, NULL, 0, on_done, &d);
    CHECK(c != NULL, "client start");
    pump(&ev, &d, 3);
    MockFd *m = &g_sock.fds[0];
    CHECK(m->in_use, "fd created");
    /* A large body that blows the 16-byte cap. */
    static const char BIG[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    m->recv_steps[0] = (RecvStep){ KL_IO_OK, BIG, sizeof(BIG) - 1 };
    m->recv_n = 1;
    pump(&ev, &d, 30);

    CHECK(d.done, "completed");
    CHECK(d.status != 200, "capped, not a clean 200");
    CHECK(d.err != KL_ERR_NONE, "error reported");

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leak");
}

static void scenario_9_alloc_fail(void) {
    printf("[9] allocation failure -> graceful, no leak\n");
    /* Sweep a few early malloc indices; each must fail cleanly with no leak. */
    for (long fail_at = 1; fail_at <= 6; fail_at++) {
        mocks_reset();
        CountAlloc a; ca_init(&a);
        a.fail_at = fail_at;
        KlEventCtx ev;
        int ctx_ok = (kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER) == 0);

        DoneCtx d;
        KlHttpClient *c = start_client(&ev, &a.base, &d, 5000);
        /* Either the start failed cleanly (NULL) or it proceeded; either way pump
         * and require no leak. If it started, a later scripted response finishes it. */
        if (c) {
            pump(&ev, &d, 3);
            if (g_sock.fds[0].in_use) script_full_200(&g_sock.fds[0]);
            pump(&ev, &d, 20);
            kl_http_client_free(c);
        }
        if (ctx_ok) kl_event_ctx_free(&ev);
        CHECK(a.live == 0, "no leak on alloc failure");
    }
}

static void scenario_10_stale_completion(void) {
    printf("[10] completion-after-cancel (stale) -> ignored, no UAF\n");
    mocks_reset();
    CountAlloc a; ca_init(&a);
    KlEventCtx ev;
    kl_event_ctx_init_ex(&ev, &a.base, &MOCK_EVENT_PROVIDER);

    DoneCtx d;
    KlHttpClient *c = start_client(&ev, &a.base, &d, 5000);
    CHECK(c != NULL, "client start");
    /* Do NOT pump first: leave the connect op queued in the mock loop. Cancel now
     * (client_cancel calls kl_comp_cancel → mloop_cancel drops the queued op), so
     * a subsequent drain must surface NO stale KL_COMP_CONNECT against the freed
     * detached watcher. Then also directly re-inject a stale completion targeting
     * a now-freed watcher to exercise the driver's liveness guard. */
    CHECK(g_loop.conns[0].in_use, "connect op queued");
    void *stale_udata = g_loop.conns[0].watcher_udata;
    kl_http_client_cancel(c);
    CHECK(!g_loop.conns[0].in_use, "cancel dropped the queued connect op");

    /* Inject a stale completion by hand: a KL_COMP_CONNECT whose target watcher
     * was freed by cancel. kl_event_dispatch's liveness walk must discard it. */
    for (int i = 0; i < MAX_OPS; i++) {
        if (!g_loop.conns[i].in_use) {
            g_loop.conns[i].in_use = 1;
            g_loop.conns[i].fd = (KlSocketHandle)9999;
            g_loop.conns[i].watcher_udata = stale_udata;   /* dangling tag */
            break;
        }
    }
    pump(&ev, &d, 10);   /* must not crash / UAF (ASan-checked) */

    kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    CHECK(a.live == 0, "no leak after stale completion");
}

int main(void) {
    printf("== FREESTANDING-HARNESS: libkeel_freestanding.a async client over mocks ==\n");
    scenario_1_happy();
    scenario_2_refused();
    scenario_3_partial_send();
    scenario_4_fragmented_recv();
    scenario_5_peer_close();
    scenario_6_timeout();
    scenario_7_cancel();
    scenario_8_size_cap();
    scenario_9_alloc_fail();
    scenario_10_stale_completion();

    int total = g_pass + g_fail;
    printf("FREESTANDING-HARNESS: %d/%d PASS\n", g_pass, total);
    if (g_fail) { printf("FREESTANDING-HARNESS: %d FAILED\n", g_fail); return 1; }
    return 0;
}
