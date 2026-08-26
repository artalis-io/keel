#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

static int set_nonblocking(int fd) {
    return kl_test_set_nonblock(fd);
}

typedef struct {
    int called;
    int got_fd;
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

UTEST_MAIN();
