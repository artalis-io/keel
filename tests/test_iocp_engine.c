/*
 * test_iocp_engine.c: IOCP backend lifecycle (Windows/BACKEND=iocp).
 *
 * Increment 2 scope: prove the IOCP event backend boots and advertises the
 * completion capability, the overlapped provider carries KL_SOCK_CAP_OVERLAPPED,
 * and the capability negotiation accepts that pairing on a REAL completion loop.
 * The completion tick + connection driver (accept/read/write over WSARecv/WSASend)
 * land in the next increment; this is the runtime lifecycle gate.
 *
 * Built and run ONLY under BACKEND=iocp (it asserts COMPLETION caps, which the
 * WSAPoll backend does not advertise). The Windows-IOCP CI job is its oracle.
 */
#include "utest.h"
#include "../src/event_caps.h"
#include "../src/socket.h"     /* KL_SOCK_CAP_OVERLAPPED + kl_socket_provider_iocp */

#include <keel/event.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/socket.h>
#include <keel/sockaddr.h>
#include "../src/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

UTEST(iocp, loop_boots_and_advertises_completion) {
    KlAllocator alloc = kl_allocator_default();
    KlEventLoop loop;
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);

    unsigned caps = kl_event_caps(&loop);
    ASSERT_TRUE(caps & KL_EVENT_CAP_COMPLETION);
    ASSERT_TRUE(caps & KL_EVENT_CAP_NATIVE_FD);
    ASSERT_FALSE(caps & KL_EVENT_CAP_READINESS);

    kl_event_close(&loop);
}

UTEST(iocp, overlapped_provider_negotiates) {
    const KlSocketProvider *p = kl_socket_provider_iocp();
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(kl_socket_provider_has_cap(p, KL_SOCK_CAP_OVERLAPPED));

    /* A real IOCP loop ⋄ the overlapped provider must negotiate as compatible;
     * the built-in (non-overlapped) default must not. */
    KlAllocator alloc = kl_allocator_default();
    KlEventLoop loop;
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);
    unsigned caps = kl_event_caps(&loop);

    ASSERT_TRUE(kl_caps_compatible(caps, p));       /* overlapped provider: OK */
    ASSERT_FALSE(kl_caps_compatible(caps, NULL));   /* POSIX/default: rejected */

    kl_event_close(&loop);
}

/* ── Watcher lifecycle ─────────────────────────────────────────────────────────────────────── */

static long g_blocks;
static void *ca_malloc(void *c, size_t n) { (void)c; void *p = malloc(n ? n : 1); if (p) g_blocks++; return p; }
static void *ca_realloc(void *c, void *p, size_t o, size_t n) { (void)c; (void)o; void *q = realloc(p, n ? n : 1); if (q && !p) g_blocks++; return q; }
static void ca_free(void *c, void *p, size_t n) { (void)c; (void)n; if (p) { g_blocks--; free(p); } }

/* A connected loopback TCP pair (default provider sockets are overlapped-capable on Windows). */
static int tcp_pair(KlSocketHandle *a, KlSocketHandle *b) {
    KlSocketHandle lis = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(lis)) return -1;
    uint8_t lo[4] = { 127, 0, 0, 1 };
    KlSockAddr addr, bound;
    kl_sockaddr_from_ipv4(&addr, lo, 0);
    KlSocketHandle cli = KL_INVALID_SOCKET, srv = KL_INVALID_SOCKET;
    if (kl_sockdef_bind(lis, &addr) < 0 || kl_sockdef_listen(lis, 1) < 0 ||
        kl_sockdef_get_local_addr(lis, &bound) < 0) goto fail;
    cli = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(cli) || kl_sockdef_connect(cli, &bound) < 0) goto fail;
    srv = kl_sockdef_accept(lis, NULL);
    if (!kl_handle_valid(srv)) goto fail;
    kl_sockdef_close(lis);
    *a = cli; *b = srv;
    return 0;
fail:
    if (kl_handle_valid(lis)) kl_sockdef_close(lis);
    if (kl_handle_valid(cli)) kl_sockdef_close(cli);
    return -1;
}

typedef struct { KlEventCtx *ev; KlSocketHandle fd; int calls; int close_in_cb; } WCtx;

/* Toggle the interest mask from inside the watcher's own callback: every call changes it. */
static void on_ready_toggle(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    WCtx *w = ud;
    w->calls++;
    kl_watcher_mod(w->ev, fd, (w->calls & 1) ? (KL_EVENT_READ | KL_EVENT_WRITE) : KL_EVENT_WRITE);
}

/* An interest change made from a watcher callback, while its probe waits to be re-armed, used to
 * allocate a fresh probe and "cancel" the idle old one, which owned no I/O and so never completed:
 * one leaked op per change, until the loop closed. The async HTTP and WebSocket clients do this on
 * every request. The ctx allocator counts live blocks: they must stay flat across the toggles. */
UTEST(iocp, watcher_interest_change_from_callback_does_not_leak) {
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, NULL };
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    WCtx w = { &ev, x, 0, 0 };
    ASSERT_EQ(kl_watcher_add(&ev, x, KL_EVENT_WRITE, on_ready_toggle, &w), 0);   /* always writable */
    for (int i = 0; i < 20 && w.calls < 10; i++) kl_event_ctx_run(&ev, 16, 20);
    ASSERT_GE(w.calls, 10);
    long settled = g_blocks;                      /* after warm-up */
    for (int i = 0; i < 400 && w.calls < 210; i++) kl_event_ctx_run(&ev, 16, 20);
    ASSERT_GE(w.calls, 210);                      /* 200 more interest changes, all from the callback */
    ASSERT_LE(g_blocks, settled + 2);             /* no op per change (was: one leaked op each) */
    kl_watcher_del(&ev, x);
    kl_sockdef_close(x);
    kl_sockdef_close(y);
    kl_event_ctx_free(&ev);
    ASSERT_EQ(g_blocks, 0);
}

/* A probe re-arm that fails (here: the socket was closed from the callback, so the zero-byte probe
 * fails synchronously) used to leave an op that neither kl_watcher_del nor the loop's close would
 * free, and kl_event_ctx_free then waited forever for its completion. A watchdog turns a regression
 * into a failure instead of a hung CI job. */
static volatile LONG g_teardown_done;
static void watchdog(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000 && !g_teardown_done; i++) Sleep(10);   /* 10 s */
    if (!g_teardown_done) {
        fprintf(stderr, "iocp: kl_event_ctx_free hung after a failed watcher re-arm\n");
        fflush(stderr);
        _exit(3);
    }
}
static void on_ready_close(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    WCtx *w = ud;
    w->calls++;
    if (w->close_in_cb && kl_handle_valid(w->fd)) {
        kl_sockdef_close(fd);                     /* the next re-arm cannot post on a closed socket */
        w->fd = KL_INVALID_SOCKET;
    }
}

UTEST(iocp, failed_watcher_rearm_does_not_hang_close) {
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, NULL };
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    WCtx w = { &ev, x, 0, 1 };
    ASSERT_EQ(kl_watcher_add(&ev, x, KL_EVENT_WRITE, on_ready_close, &w), 0);
    for (int i = 0; i < 20 && w.calls < 1; i++) kl_event_ctx_run(&ev, 16, 20);
    ASSERT_GE(w.calls, 1);
    for (int i = 0; i < 5; i++) kl_event_ctx_run(&ev, 16, 10);   /* the re-arm is attempted, and fails */
    kl_watcher_del(&ev, x);
    g_teardown_done = 0;
    KlPlatThread t;
    ASSERT_EQ(kl_plat_thread_create(&t, watchdog, NULL), 0);
    kl_event_ctx_free(&ev);                       /* hung here before the fix */
    g_teardown_done = 1;
    kl_plat_thread_join(&t);
    kl_sockdef_close(y);
    ASSERT_EQ(g_blocks, 0);                       /* the failed op was freed, not leaked */
}

/* A probe re-arm that fails because the peer reset the connection used to retire the watcher
 * silently: no further callback, ever, so its owner never learned the socket was dead. A readiness
 * backend keeps reporting such a socket (level-triggered) until its owner deletes the watcher, and
 * IOCP must do the same. */
typedef struct { int calls; } RCtx;
static void on_ready_count(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)fd; (void)ready;
    ((RCtx *)ud)->calls++;
}

UTEST(iocp, failed_watcher_rearm_keeps_reporting) {
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, NULL };
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    RCtx r = { 0 };
    ASSERT_EQ(kl_watcher_add(&ev, x, KL_EVENT_READ, on_ready_count, &r), 0);
    ASSERT_EQ(send((SOCKET)y, "z", 1, 0), 1);
    for (int i = 0; i < 20 && r.calls < 1; i++) kl_event_ctx_run(&ev, 16, 20);
    ASSERT_GE(r.calls, 1);
    struct linger lg = { 1, 0 };                  /* abortive close: the peer sends RST */
    setsockopt((SOCKET)y, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof lg);
    kl_sockdef_close(y);
    for (int i = 0; i < 10; i++) kl_event_ctx_run(&ev, 16, 20);   /* the reset lands */
    int before = r.calls;
    for (int i = 0; i < 10; i++) kl_event_ctx_run(&ev, 16, 20);
    int after = r.calls;
    kl_watcher_del(&ev, x);
    kl_sockdef_close(x);
    kl_event_ctx_free(&ev);
    ASSERT_EQ(g_blocks, 0);
    ASSERT_GE(after - before, 5);                 /* was: silence after the reset */
}

/* ── Handle-value reuse after a close without kl_watcher_del ──────────────────────────────────
 *
 * Breaking the del-before-close contract leaves a watch behind on a handle value Winsock soon gives to
 * another socket. The backend must never post a probe there on its own: a probe on a socket that is
 * associated with no port completes nowhere, and kl_event_ctx_free waited for it forever; one on a
 * socket this loop associated for something else completes here and ran the stale watcher on it. */

/* Create sockets until one takes `old`'s handle value (Windows hands values back in no fixed order;
 * each miss is closed, as holding them keeps the value from coming back). */
static SOCKET reuse_value(KlSocketHandle old) {
    for (int i = 0; i < 50000; i++) {
        SOCKET t = socket(AF_INET, SOCK_STREAM, 0);
        if (t == INVALID_SOCKET) return INVALID_SOCKET;
        if ((KlSocketHandle)t == old) return t;
        closesocket(t);
    }
    return INVALID_SOCKET;
}

/* Connect `s` to a fresh loopback listener; returns the accepted peer. */
static SOCKET connect_fresh(SOCKET s) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int al = (int)sizeof a;
    if (bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 1) != 0 ||
        getsockname(l, (struct sockaddr *)&a, &al) != 0 ||
        connect(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(l); return INVALID_SOCKET; }
    SOCKET p = accept(l, NULL, NULL);
    closesocket(l);
    return p;
}

static void free_with_watchdog(KlEventCtx *ev) {
    g_teardown_done = 0;
    KlPlatThread t;
    int have = kl_plat_thread_create(&t, watchdog, NULL) == 0;
    kl_event_ctx_free(ev);
    g_teardown_done = 1;
    if (have) kl_plat_thread_join(&t);
}

/* The audit's reproduction: closed without del, value reused by a socket no port knows, made readable.
 * The old drain re-posted the probe on it (a mod on every dispatch revived the dead watch), the probe
 * completed on no port, and kl_event_ctx_free hung. Now the close orphans the watch, nothing is posted
 * on the reused value, and the free returns at once with nothing leaked. */
UTEST(iocp, closed_watch_is_not_reposted_on_a_reused_unassociated_handle) {
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, NULL };
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    RCtx r = { 0 };
    ASSERT_EQ(kl_watcher_add(&ev, x, KL_EVENT_READ, on_ready_count, &r), 0);
    kl_sockdef_close(x);                          /* closed without kl_watcher_del */
    for (int i = 0; i < 5; i++) kl_event_ctx_run(&ev, 16, 20);
    EXPECT_EQ(r.calls, 0);                        /* the cancelled probe is not readiness */
    SOCKET reused = reuse_value(x);
    if (reused == INVALID_SOCKET) {
        kl_sockdef_close(y);
        free_with_watchdog(&ev);
        UTEST_SKIP("the handle value was not reused");
    }
    SOCKET peer = connect_fresh(reused);
    ASSERT_TRUE(peer != INVALID_SOCKET);
    ASSERT_EQ(send(peer, "x", 1, 0), 1);          /* a probe on it would complete at once */
    for (int i = 0; i < 10; i++) kl_event_ctx_run(&ev, 16, 20);
    EXPECT_EQ(r.calls, 0);                        /* the stale watcher never runs on the new socket */
    free_with_watchdog(&ev);                      /* hung here before the fix */
    EXPECT_EQ(g_blocks, 0);                       /* nothing posted, so nothing to leak */
    closesocket(reused);
    closesocket(peer);
    kl_sockdef_close(y);
}

/* The reused value belongs to a socket THIS loop associated (as a connection is, at accept). A probe
 * re-posted there would complete on this port and run the stale watcher on someone else's socket. */
UTEST(iocp, closed_watch_does_not_fire_on_a_reused_handle_this_loop_associated) {
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, NULL };
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    RCtx r = { 0 };
    ASSERT_EQ(kl_watcher_add(&ev, x, KL_EVENT_READ, on_ready_count, &r), 0);
    kl_sockdef_close(x);
    for (int i = 0; i < 5; i++) kl_event_ctx_run(&ev, 16, 20);
    SOCKET reused = reuse_value(x);
    if (reused == INVALID_SOCKET) {
        kl_sockdef_close(y);
        free_with_watchdog(&ev);
        UTEST_SKIP("the handle value was not reused");
    }
    static int conn_marker;
    ASSERT_EQ(kl_event_add(&ev.loop, (KlSocketHandle)reused, KL_EVENT_READ, &conn_marker), 0);
    SOCKET peer = connect_fresh(reused);
    ASSERT_TRUE(peer != INVALID_SOCKET);
    ASSERT_EQ(send(peer, "x", 1, 0), 1);
    int before = r.calls;
    for (int i = 0; i < 10; i++) kl_event_ctx_run(&ev, 16, 20);
    EXPECT_EQ(r.calls - before, 0);
    EXPECT_EQ(r.calls, 0);
    free_with_watchdog(&ev);
    EXPECT_EQ(g_blocks, 0);
    closesocket(reused);
    closesocket(peer);
    kl_sockdef_close(y);
}

/* The one window left: the watcher's OWN callback closes its socket without del and the value is
 * reused (and connected) before the callback returns, so the dispatch re-arm posts the probe on the
 * new socket, which no port knows. kl_event_ctx_free must still return: it gives up on that probe
 * after its stall budget and leaks the op record (one block) rather than free an OVERLAPPED the kernel
 * may still write. */
typedef struct { KlSocketHandle fd; SOCKET reused, peer; int calls; } BCtx;
static void on_ready_close_and_reuse(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    BCtx *b = ud;
    b->calls++;
    if (b->reused != INVALID_SOCKET || b->calls > 1) return;
    char buf[8];
    (void)recv((SOCKET)fd, buf, sizeof buf, 0);
    kl_sockdef_close(fd);                         /* closed without kl_watcher_del, in the callback */
    b->reused = reuse_value(fd);
    if (b->reused != INVALID_SOCKET) b->peer = connect_fresh(b->reused);   /* connected, no data */
}

UTEST(iocp, close_waits_a_bounded_time_for_a_probe_on_a_reused_handle) {
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, NULL };
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    BCtx b = { x, INVALID_SOCKET, INVALID_SOCKET, 0 };
    ASSERT_EQ(kl_watcher_add(&ev, x, KL_EVENT_READ, on_ready_close_and_reuse, &b), 0);
    ASSERT_EQ(send((SOCKET)y, "z", 1, 0), 1);
    for (int i = 0; i < 20 && b.calls < 1; i++) kl_event_ctx_run(&ev, 16, 20);
    ASSERT_GE(b.calls, 1);
    if (b.reused == INVALID_SOCKET || b.peer == INVALID_SOCKET) {
        kl_sockdef_close(y);
        free_with_watchdog(&ev);
        if (b.reused != INVALID_SOCKET) closesocket(b.reused);
        UTEST_SKIP("the handle value was not reused inside the callback");
    }
    for (int i = 0; i < 5; i++) kl_event_ctx_run(&ev, 16, 20);
    free_with_watchdog(&ev);                      /* hung forever before the bounded wait */
    EXPECT_EQ(g_blocks, 1);                       /* exactly the abandoned probe's op, leaked */
    g_blocks = 0;                                 /* the leak is deliberate: do not fail later cases */
    closesocket(b.reused);
    closesocket(b.peer);
    kl_sockdef_close(y);
}

/* ── Review regressions: re-registration before the drain, and other loops' ports ─────────── */

static void on_ready_count_read(KlSocketHandle fd, KlEventMask ready, void *ud) {
    char b[16];
    u_long nb = 1;
    (void)ioctlsocket((SOCKET)fd, FIONBIO, &nb);   /* a WRITE report must not block on the read */
    if (ready & KL_EVENT_READ) (void)recv((SOCKET)fd, b, sizeof b, 0);
    ((RCtx *)ud)->calls++;
}

/* Closed without del, value reused by a socket no port knows, and the new owner registers it (an
 * upsert on the stale ctx node) BEFORE any drain has dequeued the close's completion. The probe was
 * still PENDING: the registration used to be lost (the close's completion then orphaned the watch),
 * or, with a changed mask, a probe was posted on the never-associated socket and close stalled. The
 * fresh association now proves the socket is new: it gets its own probe. */
static int reregister_before_drain(int change_mask, long *blocks_after) {
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, NULL };
    KlEventCtx ev;
    g_blocks = 0;
    if (kl_event_ctx_init(&ev, &a) != 0) return -1;
    KlSocketHandle x, y;
    if (tcp_pair(&x, &y) != 0) return -1;
    RCtx r = { 0 }, r2 = { 0 };
    if (kl_watcher_add(&ev, x, KL_EVENT_READ, on_ready_count, &r) != 0) return -1;
    kl_event_ctx_run(&ev, 16, 10);
    kl_sockdef_close(x);                          /* closed without del ... */
    SOCKET reused = reuse_value(x);               /* ... and reused, with no drain in between */
    if (reused == INVALID_SOCKET) { kl_sockdef_close(y); free_with_watchdog(&ev); g_blocks = 0; return -2; }
    SOCKET peer = connect_fresh(reused);
    if (peer == INVALID_SOCKET) return -1;
    int rc = kl_watcher_add(&ev, (KlSocketHandle)reused,
                            change_mask ? (KL_EVENT_READ | KL_EVENT_WRITE) : KL_EVENT_READ,
                            on_ready_count_read, &r2);
    send(peer, "x", 1, 0);
    for (int i = 0; i < 20; i++) kl_event_ctx_run(&ev, 16, 20);
    kl_watcher_del(&ev, (KlSocketHandle)reused);
    free_with_watchdog(&ev);
    *blocks_after = g_blocks;
    g_blocks = 0;
    closesocket(reused);
    closesocket(peer);
    kl_sockdef_close(y);
    return rc == 0 ? r2.calls : -3;
}

UTEST(iocp, reregister_before_drain_same_mask) {
    long blocks = 0;
    int calls = reregister_before_drain(0, &blocks);
    if (calls == -2) UTEST_SKIP("the handle value was not reused");
    EXPECT_GE(calls, 1);
    EXPECT_EQ(blocks, 0);
}

UTEST(iocp, reregister_before_drain_changed_mask) {
    long blocks = 0;
    int calls = reregister_before_drain(1, &blocks);
    if (calls == -2) UTEST_SKIP("the handle value was not reused");
    EXPECT_GE(calls, 1);
    EXPECT_EQ(blocks, 0);
}

/* A poisoning allocator for loop A: freed blocks are kept and filled with 0xDD, so a later write into
 * one (another loop touching A's destroyed state through a foreign op) shows. */
#define PMAX 4096
static unsigned char *p_blk[PMAX];
static size_t p_len[PMAX];
static int p_freed[PMAX], p_n;
static volatile int p_closed;
static int p_free_after_close;
static void *pa_malloc(void *c, size_t n) {
    (void)c;
    unsigned char *p = malloc(n ? n : 1);
    if (p && p_n < PMAX) { p_blk[p_n] = p; p_len[p_n] = n; p_freed[p_n] = 0; p_n++; }
    return p;
}
static void *pa_realloc(void *c, void *p, size_t o, size_t n) {
    void *q = pa_malloc(c, n);
    if (q && p) memcpy(q, p, o < n ? o : n);
    return q;
}
static void pa_free(void *c, void *p, size_t n) {
    (void)c; (void)n;
    if (p_closed) p_free_after_close++;
    for (int i = 0; i < p_n; i++)
        if (p_blk[i] == p) { p_freed[i] = 1; memset(p, 0xDD, p_len[i]); }
}
static int poison_damaged(void) {
    int d = 0;
    for (int i = 0; i < p_n; i++) {
        if (!p_freed[i]) continue;
        for (size_t k = 0; k < p_len[i]; k++)
            if (p_blk[i][k] != 0xDD) { d++; break; }
    }
    return d;
}
static void poison_reset(void) { p_n = 0; p_closed = 0; p_free_after_close = 0; }

/* Loop A's watch is orphaned (closed without del); the value is reused by a socket loop B watches;
 * A's owner then names the value again. ERROR_INVALID_PARAMETER cannot tell A's port from B's, and
 * accepting it posted A's probe on B's socket: after A's teardown, B dequeued that op and freed it
 * through A's destroyed state and allocator. A must refuse the registration (the socket is in no
 * record of A's) and post nothing. */
UTEST(iocp, closed_watch_is_not_revived_on_another_loops_socket) {
    poison_reset();
    KlAllocator aa = { pa_malloc, pa_realloc, pa_free, NULL };
    KlAllocator ab = kl_allocator_default();
    KlEventCtx A, B;
    ASSERT_EQ(kl_event_ctx_init(&A, &aa), 0);
    ASSERT_EQ(kl_event_ctx_init(&B, &ab), 0);
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    RCtx ra = { 0 }, rb = { 0 };
    ASSERT_EQ(kl_watcher_add(&A, x, KL_EVENT_READ, on_ready_count, &ra), 0);
    kl_sockdef_close(x);                          /* breach: closed without del */
    for (int i = 0; i < 3; i++) kl_event_ctx_run(&A, 16, 10);   /* orphaned */
    SOCKET reused = reuse_value(x);
    if (reused == INVALID_SOCKET) {
        free_with_watchdog(&A); kl_event_ctx_free(&B); kl_sockdef_close(y);
        UTEST_SKIP("the handle value was not reused");
    }
    SOCKET peer = connect_fresh(reused);
    ASSERT_TRUE(peer != INVALID_SOCKET);
    ASSERT_EQ(kl_watcher_add(&B, (KlSocketHandle)reused, KL_EVENT_READ, on_ready_count_read, &rb), 0);
    EXPECT_EQ(kl_watcher_mod(&A, x, KL_EVENT_READ), -1);   /* refused: not verifiably A's */
    kl_watcher_del(&A, x);
    free_with_watchdog(&A);
    p_closed = 1;
    send(peer, "x", 1, 0);
    for (int i = 0; i < 10; i++) kl_event_ctx_run(&B, 16, 20);
    EXPECT_GE(rb.calls, 1);                       /* B's own watcher works */
    EXPECT_EQ(ra.calls, 0);
    EXPECT_EQ(p_free_after_close, 0);             /* nothing freed through A's allocator after A */
    EXPECT_EQ(poison_damaged(), 0);               /* nor written into A's freed memory */
    kl_watcher_del(&B, (KlSocketHandle)reused);
    kl_event_ctx_free(&B);
    closesocket(reused);
    closesocket(peer);
    kl_sockdef_close(y);
}

/* The window identity cannot see: A's watcher callback closes its socket without del and the value
 * is reused, inside that callback, by a socket loop B watches. A's dispatch re-arm then posts on B's
 * socket; A's del cancels it, and the cancelled packet goes to B's port. A's teardown gives up on the
 * probe and retains its op; B's drain must drop the foreign op untouched (its owner tag is not B's),
 * not free it through A's destroyed state. */
typedef struct { KlEventCtx *b; RCtx *rb; SOCKET reused, peer; int calls; } FCtx;
static void on_ready_close_reuse_into_b(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    FCtx *f = ud;
    f->calls++;
    if (f->reused != INVALID_SOCKET || f->calls > 1) return;
    char buf[8];
    (void)recv((SOCKET)fd, buf, sizeof buf, 0);
    kl_sockdef_close(fd);                         /* closed without kl_watcher_del, in the callback */
    f->reused = reuse_value(fd);
    if (f->reused == INVALID_SOCKET) return;
    f->peer = connect_fresh(f->reused);           /* connected, no data yet */
    (void)kl_watcher_add(f->b, (KlSocketHandle)f->reused, KL_EVENT_READ, on_ready_count_read, f->rb);
}

UTEST(iocp, foreign_probe_packet_is_dropped_untouched) {
    poison_reset();
    KlAllocator aa = { pa_malloc, pa_realloc, pa_free, NULL };
    KlAllocator ab = kl_allocator_default();
    KlEventCtx A, B;
    ASSERT_EQ(kl_event_ctx_init(&A, &aa), 0);
    ASSERT_EQ(kl_event_ctx_init(&B, &ab), 0);
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    RCtx rb = { 0 };
    FCtx f = { &B, &rb, INVALID_SOCKET, INVALID_SOCKET, 0 };
    ASSERT_EQ(kl_watcher_add(&A, x, KL_EVENT_READ, on_ready_close_reuse_into_b, &f), 0);
    ASSERT_EQ(send((SOCKET)y, "z", 1, 0), 1);
    for (int i = 0; i < 20 && f.calls < 1; i++) kl_event_ctx_run(&A, 16, 20);
    ASSERT_GE(f.calls, 1);
    if (f.reused == INVALID_SOCKET || f.peer == INVALID_SOCKET) {
        free_with_watchdog(&A); kl_event_ctx_free(&B); kl_sockdef_close(y);
        if (f.reused != INVALID_SOCKET) closesocket(f.reused);
        UTEST_SKIP("the handle value was not reused inside the callback");
    }
    kl_watcher_del(&A, x);                        /* cancels A's probe on B's socket: packet to B */
    free_with_watchdog(&A);                       /* gives up on that probe; retains its op */
    p_closed = 1;
    send(f.peer, "x", 1, 0);
    for (int i = 0; i < 10; i++) kl_event_ctx_run(&B, 16, 20);
    EXPECT_GE(rb.calls, 1);                       /* B's own watcher is unaffected */
    EXPECT_EQ(p_free_after_close, 0);             /* B freed nothing through A's allocator */
    EXPECT_EQ(poison_damaged(), 0);               /* nor wrote into A's freed state */
    kl_watcher_del(&B, (KlSocketHandle)f.reused);
    kl_event_ctx_free(&B);
    closesocket(f.reused);
    closesocket(f.peer);
    kl_sockdef_close(y);
}

/* Close without del, reuse and re-registration all INSIDE the watcher's callback, so the watch is IDLE
 * (its completion dequeued, its re-arm not yet posted) when the new owner registers. With a changed
 * interest the registration used to post the probe on the never-associated socket: it returned 0, the
 * watcher never fired, and close stalled and retained the op. It is now held to the identity rule (a
 * fresh association here), like any explicit registration. */
typedef struct { KlEventCtx *ev; RCtx *r2; SOCKET reused, peer; int calls; int rc; KlEventMask mask; } ICtx;
static void on_ready_close_reuse_rereg(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    ICtx *c = ud;
    c->calls++;
    if (c->reused != INVALID_SOCKET || c->calls > 1) return;
    char buf[8];
    (void)recv((SOCKET)fd, buf, sizeof buf, 0);
    kl_sockdef_close(fd);
    c->reused = reuse_value(fd);
    if (c->reused == INVALID_SOCKET) return;
    c->peer = connect_fresh(c->reused);
    c->rc = kl_watcher_add(c->ev, (KlSocketHandle)c->reused, c->mask, on_ready_count_read, c->r2);
}

static int rereg_in_callback(KlEventMask mask, long *blocks_after) {
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, NULL };
    g_blocks = 0;
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &a) != 0) return -1;
    KlSocketHandle x, y;
    if (tcp_pair(&x, &y) != 0) return -1;
    RCtx r2 = { 0 };
    ICtx c = { &ev, &r2, INVALID_SOCKET, INVALID_SOCKET, 0, -9, mask };
    if (kl_watcher_add(&ev, x, KL_EVENT_READ, on_ready_close_reuse_rereg, &c) != 0) return -1;
    if (send((SOCKET)y, "z", 1, 0) != 1) return -1;
    for (int i = 0; i < 20 && c.calls < 1; i++) kl_event_ctx_run(&ev, 16, 20);
    if (c.reused == INVALID_SOCKET || c.peer == INVALID_SOCKET) {
        free_with_watchdog(&ev); g_blocks = 0; kl_sockdef_close(y);
        if (c.reused != INVALID_SOCKET) closesocket(c.reused);
        return -2;
    }
    send(c.peer, "x", 1, 0);
    for (int i = 0; i < 20; i++) kl_event_ctx_run(&ev, 16, 20);
    kl_watcher_del(&ev, (KlSocketHandle)c.reused);
    free_with_watchdog(&ev);
    *blocks_after = g_blocks;
    g_blocks = 0;
    closesocket(c.reused); closesocket(c.peer); kl_sockdef_close(y);
    return c.rc == 0 ? r2.calls : -3;
}

UTEST(iocp, reregister_in_callback_same_mask) {
    long blocks = 0;
    int calls = rereg_in_callback(KL_EVENT_READ, &blocks);
    if (calls == -2) UTEST_SKIP("the handle value was not reused inside the callback");
    EXPECT_GE(calls, 1);
    EXPECT_EQ(blocks, 0);
}

UTEST(iocp, reregister_in_callback_changed_mask) {
    long blocks = 0;
    int calls = rereg_in_callback(KL_EVENT_READ | KL_EVENT_WRITE, &blocks);
    if (calls == -2) UTEST_SKIP("the handle value was not reused inside the callback");
    EXPECT_GE(calls, 1);
    EXPECT_EQ(blocks, 0);
}

/* The identity record must not outlive what the owner tells the loop: a connection registered on loop
 * A (recorded), then removed from A with kl_event_del and closed; its value reused by a socket loop B
 * owns. A watch registration on A for that value must be refused (the record entry went with the del),
 * not posted on B's socket. And a connection add of a handle already on another port is refused, as it
 * always was. */
UTEST(iocp, identity_record_forgets_a_deleted_connection) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx A, B;
    ASSERT_EQ(kl_event_ctx_init(&A, &a), 0);
    ASSERT_EQ(kl_event_ctx_init(&B, &a), 0);
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    static int conn_marker;
    ASSERT_EQ(kl_event_add(&A.loop, x, KL_EVENT_READ, &conn_marker), 0);   /* recorded on A */
    kl_event_del(&A.loop, x);                                               /* the connection leaves */
    kl_sockdef_close(x);
    SOCKET reused = reuse_value(x);
    if (reused == INVALID_SOCKET) {
        kl_event_ctx_free(&A); kl_event_ctx_free(&B); kl_sockdef_close(y);
        UTEST_SKIP("the handle value was not reused");
    }
    SOCKET peer = connect_fresh(reused);         /* a probe needs a connected socket */
    ASSERT_TRUE(peer != INVALID_SOCKET);
    RCtx ra = { 0 }, rb = { 0 };
    ASSERT_EQ(kl_watcher_add(&B, (KlSocketHandle)reused, KL_EVENT_READ, on_ready_count_read, &rb), 0);
    EXPECT_EQ(kl_watcher_add(&A, (KlSocketHandle)reused, KL_EVENT_READ, on_ready_count, &ra), -1);
    EXPECT_EQ(kl_event_add(&A.loop, (KlSocketHandle)reused, KL_EVENT_READ, &conn_marker), -1);
    kl_watcher_del(&B, (KlSocketHandle)reused);
    kl_event_ctx_free(&A);
    kl_event_ctx_free(&B);
    closesocket(reused);
    closesocket(peer);
    kl_sockdef_close(y);
}

/* The same-interest form of the in-callback re-registration, where the new socket's probe completes
 * at once (WRITE interest; READ with data already queued). The upsert passes the same watcher, so it
 * used to be taken for the dispatch re-arm and left unchecked: the probe completed with no port, the
 * re-arm then superseded it with one whose completion never came, and close stalled and retained the
 * op. Only kl_watcher_rearm is now taken as a re-arm; the upsert is checked like any registration. */
typedef struct { KlEventCtx *ev; RCtx *r2; SOCKET reused, peer; int calls; int rc; KlEventMask mask; int predata; } JCtx;
static void on_ready_close_reuse_same(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    JCtx *c = ud;
    c->calls++;
    if (c->reused != INVALID_SOCKET || c->calls > 1) return;
    kl_sockdef_close(fd);
    c->reused = reuse_value(fd);
    if (c->reused == INVALID_SOCKET) return;
    c->peer = connect_fresh(c->reused);
    if (c->predata) { send(c->peer, "q", 1, 0); Sleep(30); }
    c->rc = kl_watcher_add(c->ev, (KlSocketHandle)c->reused, c->mask, on_ready_count, c->r2);
}
static int rereg_same_immediate(KlEventMask mask, int predata, long *blocks_after) {
    KlAllocator a = { ca_malloc, ca_realloc, ca_free, NULL };
    g_blocks = 0;
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &a) != 0) return -1;
    KlSocketHandle x, y;
    if (tcp_pair(&x, &y) != 0) return -1;
    RCtx r2 = { 0 };
    JCtx c = { &ev, &r2, INVALID_SOCKET, INVALID_SOCKET, 0, -9, mask, predata };
    if (kl_watcher_add(&ev, x, mask, on_ready_close_reuse_same, &c) != 0) return -1;
    if (send((SOCKET)y, "z", 1, 0) != 1) return -1;
    for (int i = 0; i < 20 && c.calls < 1; i++) kl_event_ctx_run(&ev, 16, 20);
    if (c.reused == INVALID_SOCKET || c.peer == INVALID_SOCKET) {
        free_with_watchdog(&ev); g_blocks = 0; kl_sockdef_close(y);
        if (c.reused != INVALID_SOCKET) closesocket(c.reused);
        return -2;
    }
    for (int i = 0; i < 20; i++) kl_event_ctx_run(&ev, 16, 20);
    kl_watcher_del(&ev, (KlSocketHandle)c.reused);
    free_with_watchdog(&ev);
    *blocks_after = g_blocks;
    g_blocks = 0;
    closesocket(c.reused); closesocket(c.peer); kl_sockdef_close(y);
    return c.rc == 0 ? r2.calls : -3;
}

UTEST(iocp, reregister_in_callback_same_mask_write) {
    long blocks = 0;
    int calls = rereg_same_immediate(KL_EVENT_WRITE, 0, &blocks);
    if (calls == -2) UTEST_SKIP("the handle value was not reused inside the callback");
    EXPECT_GE(calls, 1);
    EXPECT_EQ(blocks, 0);
}

UTEST(iocp, reregister_in_callback_same_mask_read_predata) {
    long blocks = 0;
    int calls = rereg_same_immediate(KL_EVENT_READ, 1, &blocks);
    if (calls == -2) UTEST_SKIP("the handle value was not reused inside the callback");
    EXPECT_GE(calls, 1);
    EXPECT_EQ(blocks, 0);
}

/* Out of memory for the identity record must not let a stale entry stand in for the check. With the
 * record full enough that one more entry needs it to grow, and every allocation failing, the
 * callback's same-interest re-registration of a reused value still has its association tried: the
 * value is recorded, so its slot is updated in place. It used to skip the attempt and accept the
 * stale entry, posting the probe on a socket no port knew (watcher dead, close stalled). */
static volatile int g_fail_alloc;
static void *fa_malloc(void *c, size_t n) { return g_fail_alloc ? NULL : ca_malloc(c, n); }
static void *fa_realloc(void *c, void *p, size_t o, size_t n) { return g_fail_alloc ? NULL : ca_realloc(c, p, o, n); }
static void on_ready_close_reuse_oom(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    JCtx *c = ud;
    c->calls++;
    if (c->reused != INVALID_SOCKET || c->calls > 1) return;
    kl_sockdef_close(fd);
    c->reused = reuse_value(fd);
    if (c->reused == INVALID_SOCKET) return;
    c->peer = connect_fresh(c->reused);
    g_fail_alloc = 1;   /* the record cannot grow now */
    c->rc = kl_watcher_add(c->ev, (KlSocketHandle)c->reused, c->mask, on_ready_count, c->r2);
    g_fail_alloc = 0;
}

UTEST(iocp, reregister_in_callback_under_oom_still_checks_the_handle) {
    KlAllocator a = { fa_malloc, fa_realloc, ca_free, NULL };
    g_blocks = 0; g_fail_alloc = 0;
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    /* 31 recorded connections + the watch = 32 entries: the next new entry needs the 64-slot table
     * to grow. */
    enum { NREC = 31 };
    KlSocketHandle ca[NREC], cb[NREC];
    static int marks[NREC];
    for (int i = 0; i < NREC; i++) {
        ASSERT_EQ(tcp_pair(&ca[i], &cb[i]), 0);
        ASSERT_EQ(kl_event_add(&ev.loop, ca[i], KL_EVENT_READ, &marks[i]), 0);
    }
    KlSocketHandle x, y;
    ASSERT_EQ(tcp_pair(&x, &y), 0);
    RCtx r2 = { 0 };
    JCtx c = { &ev, &r2, INVALID_SOCKET, INVALID_SOCKET, 0, -9, KL_EVENT_WRITE, 0 };
    ASSERT_EQ(kl_watcher_add(&ev, x, KL_EVENT_WRITE, on_ready_close_reuse_oom, &c), 0);
    for (int i = 0; i < 20 && c.calls < 1; i++) kl_event_ctx_run(&ev, 16, 20);
    int skip = (c.reused == INVALID_SOCKET || c.peer == INVALID_SOCKET);
    if (!skip) for (int i = 0; i < 20; i++) kl_event_ctx_run(&ev, 16, 20);
    if (!skip) kl_watcher_del(&ev, (KlSocketHandle)c.reused);
    free_with_watchdog(&ev);
    long blocks = g_blocks;
    g_blocks = 0;
    for (int i = 0; i < NREC; i++) { kl_sockdef_close(ca[i]); kl_sockdef_close(cb[i]); }
    if (c.reused != INVALID_SOCKET) closesocket(c.reused);
    if (c.peer != INVALID_SOCKET) closesocket(c.peer);
    kl_sockdef_close(y);
    if (skip) UTEST_SKIP("the handle value was not reused inside the callback");
    EXPECT_EQ(c.rc, 0);
    EXPECT_GE(r2.calls, 1);
    EXPECT_EQ(blocks, 0);
}

UTEST_MAIN();
