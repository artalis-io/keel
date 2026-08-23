/*
 * test_async.c — generic KlEventCtx watcher tests (substrate). The HTTP KlAsyncOp /
 * server-suspension coverage lives in tests/protocols/http/test_http_async.c; this file is
 * protocol-neutral — it drives kl_watcher_add/mod/del + kl_event_ctx_run over a bare
 * KlEventCtx, no HTTP server/connection/async types.
 */
#include "utest.h"
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include "net_compat.h"
#include <string.h>

/* ── Neutral event-loop fixture ──────────────────────────────────── */

typedef struct {
    KlAllocator alloc;
    KlEventCtx  ev;
} TestLoop;

static int test_loop_init(TestLoop *l) {
    l->alloc = kl_allocator_default();
    return kl_event_ctx_init(&l->ev, &l->alloc);
}

static void test_loop_free(TestLoop *l) {
    kl_event_ctx_free(&l->ev);   /* frees any remaining watchers + closes the loop */
}

static int set_nonblocking(int fd) {
    return kl_test_set_nonblock(fd);
}

/* ── Watcher callback context ─────────────────────────────────────── */

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

/* Pump the event loop until *flag reaches `want` (or attempts run out). Uses
 * kl_event_ctx_run — the portable tick (wait + dispatch watchers) that works on
 * BOTH the readiness and completion event models, so these watcher tests are
 * backend-agnostic instead of hard-coding a readiness kl_event_wait loop. */
static void pump_until(KlEventCtx *ev, const int *flag, int want) {
    for (int a = 0; a < 20 && *flag < want; a++)
        kl_event_ctx_run(ev, 16, 50);
}

/* ── Watcher Tests ────────────────────────────────────────────────── */

UTEST(async, watcher_add_fires_on_read) {
    TestLoop loop;
    ASSERT_EQ(test_loop_init(&loop), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    ASSERT_EQ(set_nonblocking(fds[0]), 0);
    ASSERT_EQ(set_nonblocking(fds[1]), 0);

    WatcherCtx ctx = {0};
    ASSERT_EQ(kl_watcher_add(&loop.ev, fds[0], KL_EVENT_READ, test_watcher_cb, &ctx), 0);

    /* Write to fds[1] → fds[0] becomes readable */
    (void)kl_test_sockwrite(fds[1], "x", 1);

    pump_until(&loop.ev, &ctx.called, 1);

    ASSERT_EQ(ctx.called, 1);
    ASSERT_EQ(ctx.got_fd, fds[0]);
    ASSERT_TRUE(ctx.got_mask & KL_EVENT_READ);

    kl_watcher_del(&loop.ev, fds[0]);
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    test_loop_free(&loop);
}

UTEST(async, watcher_mod_changes_interest) {
    TestLoop loop;
    ASSERT_EQ(test_loop_init(&loop), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    ASSERT_EQ(set_nonblocking(fds[0]), 0);
    ASSERT_EQ(set_nonblocking(fds[1]), 0);

    WatcherCtx ctx = {0};
    ASSERT_EQ(kl_watcher_add(&loop.ev, fds[0], KL_EVENT_READ, test_watcher_cb, &ctx), 0);

    /* Mod to WRITE interest — socket should be writable immediately */
    ASSERT_EQ(kl_watcher_mod(&loop.ev, fds[0], KL_EVENT_WRITE), 0);

    pump_until(&loop.ev, &ctx.called, 1);

    ASSERT_EQ(ctx.called, 1);
    ASSERT_EQ(ctx.got_fd, fds[0]);
    ASSERT_TRUE(ctx.got_mask & KL_EVENT_WRITE);

    kl_watcher_del(&loop.ev, fds[0]);
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    test_loop_free(&loop);
}

UTEST(async, watcher_del_stops_events) {
    TestLoop loop;
    ASSERT_EQ(test_loop_init(&loop), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    ASSERT_EQ(set_nonblocking(fds[0]), 0);
    ASSERT_EQ(set_nonblocking(fds[1]), 0);

    WatcherCtx ctx = {0};
    ASSERT_EQ(kl_watcher_add(&loop.ev, fds[0], KL_EVENT_READ, test_watcher_cb, &ctx), 0);

    /* Remove watcher before writing */
    kl_watcher_del(&loop.ev, fds[0]);

    /* Write to fds[1] — should NOT fire callback */
    (void)kl_test_sockwrite(fds[1], "x", 1);

    kl_event_ctx_run(&loop.ev, 8, 50);   /* one tick; the deleted watcher must not fire */

    ASSERT_EQ(ctx.called, 0);

    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    test_loop_free(&loop);
}

UTEST(async, watcher_multiple_fds) {
    TestLoop loop;
    ASSERT_EQ(test_loop_init(&loop), 0);

    int fds1[2], fds2[2], fds3[2];
    ASSERT_EQ(kl_test_socketpair(fds1), 0);
    ASSERT_EQ(kl_test_socketpair(fds2), 0);
    ASSERT_EQ(kl_test_socketpair(fds3), 0);
    for (int i = 0; i < 2; i++) {
        set_nonblocking(fds1[i]);
        set_nonblocking(fds2[i]);
        set_nonblocking(fds3[i]);
    }

    WatcherCtx ctx1 = {0}, ctx2 = {0}, ctx3 = {0};
    ASSERT_EQ(kl_watcher_add(&loop.ev, fds1[0], KL_EVENT_READ, test_watcher_cb, &ctx1), 0);
    ASSERT_EQ(kl_watcher_add(&loop.ev, fds2[0], KL_EVENT_READ, test_watcher_cb, &ctx2), 0);
    ASSERT_EQ(kl_watcher_add(&loop.ev, fds3[0], KL_EVENT_READ, test_watcher_cb, &ctx3), 0);

    /* Write to all three */
    (void)kl_test_sockwrite(fds1[1], "a", 1);
    (void)kl_test_sockwrite(fds2[1], "b", 1);
    (void)kl_test_sockwrite(fds3[1], "c", 1);

    /* Pump until each fd's watcher fires (portable across both event models). */
    pump_until(&loop.ev, &ctx1.called, 1);
    pump_until(&loop.ev, &ctx2.called, 1);
    pump_until(&loop.ev, &ctx3.called, 1);

    ASSERT_TRUE(ctx1.called >= 1);
    ASSERT_TRUE(ctx2.called >= 1);
    ASSERT_TRUE(ctx3.called >= 1);

    kl_watcher_del(&loop.ev, fds1[0]);
    kl_watcher_del(&loop.ev, fds2[0]);
    kl_watcher_del(&loop.ev, fds3[0]);
    for (int i = 0; i < 2; i++) {
        kl_test_closesock(fds1[i]);
        kl_test_closesock(fds2[i]);
        kl_test_closesock(fds3[i]);
    }
    test_loop_free(&loop);
}

UTEST(async, watcher_mod_not_found) {
    TestLoop loop;
    ASSERT_EQ(test_loop_init(&loop), 0);

    /* Mod on non-existent watcher should return -1 */
    ASSERT_EQ(kl_watcher_mod(&loop.ev, 999, KL_EVENT_READ), -1);

    test_loop_free(&loop);
}

UTEST(async, watcher_del_not_found) {
    TestLoop loop;
    ASSERT_EQ(test_loop_init(&loop), 0);

    /* Del on non-existent watcher should be a no-op (not crash) */
    kl_watcher_del(&loop.ev, 999);

    test_loop_free(&loop);
}

/* ── Watcher-mask re-arm (poll_update) churn soak ────────────────────────
 * Hammers kl_event_mod's re-arm-to-a-new-mask path under heavy churn. Over
 * BACKEND=iouring every mod on an armed watch drives io_uring_prep_poll_update
 * (the fix that made watcher_mod_changes_interest work); on readiness backends it
 * is plain kl_event_mod. Each round retargets the SAME armed watch READ↔WRITE
 * (the watch is armed at every mod, so the poll_update branch is taken, not a
 * fresh arm). The point is resource lifetime under churn — run under ASan/LSan
 * this proves the thousands of re-arms leak no CQE/SQE/watch and never UAF a
 * retired poll; exact fire timing is edge-triggered and intentionally NOT asserted
 * (functional correctness is covered by watcher_mod_changes_interest). No normal
 * HTTP path mods a watcher's mask over io_uring, so this is the dedicated stress
 * for that path (see docs/archive/phases/phase8f5_iouring_default_migration_design.md §4). */
#define SOAK_ROUNDS 3000
UTEST(async, watcher_mask_churn_soak) {
    TestLoop loop;
    ASSERT_EQ(test_loop_init(&loop), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    /* fds[0] stays writable (send buffer not full) and non-readable (nothing
     * queued), so WRITE interest can fire and READ interest does not. */
    WatcherCtx ctx = {0};
    ASSERT_EQ(kl_watcher_add(&loop.ev, fds[0], KL_EVENT_READ, test_watcher_cb, &ctx), 0);

    for (int i = 0; i < SOAK_ROUNDS; i++) {
        ASSERT_EQ(kl_watcher_mod(&loop.ev, fds[0], KL_EVENT_WRITE), 0);  /* poll_update → WRITE */
        (void)kl_event_ctx_run(&loop.ev, 16, 0);                        /* WRITE may fire */
        ASSERT_EQ(kl_watcher_mod(&loop.ev, fds[0], KL_EVENT_READ), 0);  /* poll_update → READ */
        (void)kl_event_ctx_run(&loop.ev, 16, 0);                        /* READ must not fire */
    }
    /* poll_update actually delivered interest changes — the WRITE interest fired. */
    ASSERT_TRUE(ctx.called > 0);

    kl_watcher_del(&loop.ev, fds[0]);
    for (int k = 0; k < 8; k++)                 /* drain the del's poll-cancel CQE → frees the watch */
        (void)kl_event_ctx_run(&loop.ev, 16, 0);
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    test_loop_free(&loop);
}

UTEST_MAIN();
