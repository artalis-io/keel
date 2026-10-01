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

UTEST_MAIN();
