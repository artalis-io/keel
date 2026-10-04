#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

static int set_nonblocking(int fd) {
    return kl_test_set_nonblock(fd);
}

typedef struct {
    int called;
    KlSocketHandle got_fd;
    KlEventMask got_mask;
} WatcherCtx;

static void test_watcher_cb(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    WatcherCtx *ctx = user_data;
    ctx->called++;
    ctx->got_fd = fd;
    ctx->got_mask = ready;
}

/* ── kl_event_dispatch tests ─────────────────────────────────────── */

UTEST(event_ctx, dispatch_watcher_returns_1) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    WatcherCtx ctx = {0};
    ASSERT_EQ(kl_watcher_add(&ev, fds[0], KL_EVENT_READ, test_watcher_cb, &ctx), 0);

    (void)kl_test_sockwrite(fds[1], "x", 1);

    KlEvent events[4];
    int n = kl_event_wait(&ev.loop, events, 4, 100);
    ASSERT_TRUE(n > 0);

    int dispatched = kl_event_dispatch(&ev, &events[0]);
    ASSERT_EQ(dispatched, 1);
    ASSERT_EQ(ctx.called, 1);
    ASSERT_EQ(ctx.got_fd, fds[0]);

    kl_watcher_del(&ev, fds[0]);
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    kl_event_ctx_free(&ev);
}

UTEST(event_ctx, dispatch_non_watcher_returns_0) {
    /* An event with udata=NULL (listen socket, connection) should return 0 */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    KlEvent fake_event = {.udata = NULL, .ready = KL_EVENT_READ};
    ASSERT_EQ(kl_event_dispatch(&ev, &fake_event), 0);

    kl_event_ctx_free(&ev);
}

UTEST(event_ctx, dispatch_connection_ptr_returns_0) {
    /* A non-tagged pointer (even address) should return 0 */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    /* Simulate a connection pointer (any even address) */
    int dummy;
    KlEvent fake_event = {.udata = &dummy, .ready = KL_EVENT_READ};
    ASSERT_EQ(kl_event_dispatch(&ev, &fake_event), 0);

    kl_event_ctx_free(&ev);
}

/* ── kl_event_ctx_run tests ──────────────────────────────────────── */

UTEST(event_ctx, run_dispatches_watcher) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    WatcherCtx ctx = {0};
    ASSERT_EQ(kl_watcher_add(&ev, fds[0], KL_EVENT_READ, test_watcher_cb, &ctx), 0);

    (void)kl_test_sockwrite(fds[1], "hello", 5);

    int n = kl_event_ctx_run(&ev, 8, 100);
    ASSERT_TRUE(n > 0);
    ASSERT_EQ(ctx.called, 1);
    ASSERT_EQ(ctx.got_fd, fds[0]);
    ASSERT_TRUE(ctx.got_mask & KL_EVENT_READ);

    kl_watcher_del(&ev, fds[0]);
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    kl_event_ctx_free(&ev);
}

UTEST(event_ctx, run_returns_0_on_timeout) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    /* No FDs registered: should timeout and return 0 */
    int n = kl_event_ctx_run(&ev, 8, 1);
    ASSERT_EQ(n, 0);

    kl_event_ctx_free(&ev);
}

UTEST(event_ctx, run_null_ctx_returns_minus_1) {
    ASSERT_EQ(kl_event_ctx_run(NULL, 8, 100), -1);
}

UTEST(event_ctx, run_zero_max_events_returns_minus_1) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    ASSERT_EQ(kl_event_ctx_run(&ev, 0, 100), -1);

    kl_event_ctx_free(&ev);
}

/* F2-B accessor: kl_event_ctx_loop[_const] return the embedded loop, NULL on NULL input. */
UTEST(event_ctx, loop_accessor) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    ASSERT_EQ((void *)kl_event_ctx_loop(&ev), (void *)&ev.loop);
    ASSERT_EQ((const void *)kl_event_ctx_loop_const(&ev), (const void *)&ev.loop);
    ASSERT_TRUE(kl_event_ctx_loop(NULL) == NULL);
    ASSERT_TRUE(kl_event_ctx_loop_const(NULL) == NULL);

    kl_event_ctx_free(&ev);
}

#ifndef _WIN32
#include <signal.h>
#include <string.h>
#include <sys/time.h>

static volatile sig_atomic_t eintr_hits;
static void eintr_handler(int sig) { (void)sig; eintr_hits++; }

/* A signal that lands while the loop waits (SIGCHLD, a profiler's SIGPROF) interrupts the wait
 * syscall. That is a tick with no events, not a loop failure: a caller's
 * `while (kl_event_ctx_run(...) >= 0)` must keep running. The handler is installed WITHOUT
 * SA_RESTART so the wait really returns EINTR. */
UTEST(event_ctx, run_interrupted_by_signal_is_an_empty_tick) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = eintr_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ASSERT_EQ(0, sigaction(SIGALRM, &sa, &old));

    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    eintr_hits = 0;
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_usec = 50000;                  /* fires 50 ms into a 2 s wait */
    ASSERT_EQ(0, setitimer(ITIMER_REAL, &it, NULL));
    int rc = kl_event_ctx_run(&ev, 8, 2000);

    struct itimerval off;
    memset(&off, 0, sizeof(off));
    (void)setitimer(ITIMER_REAL, &off, NULL);
    (void)sigaction(SIGALRM, &old, NULL);
    kl_event_ctx_free(&ev);

    ASSERT_EQ(1, (int)eintr_hits);                /* the signal arrived during the wait */
    ASSERT_GE(rc, 0);                             /* an interrupted wait is not an error */
}
#endif

UTEST_MAIN();
