/*
 * test_async.c: generic KlEventCtx watcher tests (substrate). The HTTP KlAsyncOp /
 * server-suspension coverage lives in tests/protocols/http/test_http_async.c; this file is
 * protocol-neutral: it drives kl_watcher_add/mod/del + kl_event_ctx_run over a bare
 * KlEventCtx, no HTTP server/connection/async types.
 */
#include "utest.h"
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include "net_compat.h"
#include "../src/watcher_internal.h"   /* kl_watcher_add_detached (the ABA regression below) */
#include <keel/thread_pool.h>
#include <stdlib.h>
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
    KlSocketHandle got_fd;
    KlEventMask got_mask;
} WatcherCtx;

static void test_watcher_cb(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    WatcherCtx *ctx = user_data;
    ctx->called++;
    ctx->got_fd = fd;
    ctx->got_mask = ready;
}

/* Pump the event loop until *flag reaches `want` (or attempts run out). Uses
 * kl_event_ctx_run: the portable tick (wait + dispatch watchers) that works on
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

    /* Mod to WRITE interest: socket should be writable immediately */
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

    /* Write to fds[1]: should NOT fire callback */
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
 * fresh arm). The point is resource lifetime under churn: run under ASan/LSan
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
    /* poll_update actually delivered interest changes: the WRITE interest fired. */
    ASSERT_TRUE(ctx.called > 0);

    kl_watcher_del(&loop.ev, fds[0]);
    for (int k = 0; k < 8; k++)                 /* drain the del's poll-cancel CQE → frees the watch */
        (void)kl_event_ctx_run(&loop.ev, 16, 0);
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    test_loop_free(&loop);
}

/* A watcher whose callback removes a DIFFERENT watcher is still re-armed. On a one-shot backend
 * (io_uring's POLL_ADD) a skipped re-arm means this fd is never watched again: a thread pool's wakeup
 * whose done_fn completes a suspended HTTP op (which removes that connection's hang-up watcher) stops
 * after its first completion. Drives two reads through the same watcher; the first removes the other. */
typedef struct {
    KlEventCtx *ev;
    int other_fd;
    int calls;
} DelOtherCtx;

static void del_other_cb(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready;
    DelOtherCtx *c = user_data;
    char b[8];
    (void)kl_test_sockread(fd, b, sizeof b);
    if (c->calls++ == 0) kl_watcher_del(c->ev, c->other_fd);
}

UTEST(async, watcher_rearmed_after_its_callback_removes_another) {
    TestLoop l;
    ASSERT_EQ(test_loop_init(&l), 0);
    int a[2], o[2];
    ASSERT_EQ(kl_test_socketpair(a), 0);
    ASSERT_EQ(kl_test_socketpair(o), 0);
    set_nonblocking(a[0]); set_nonblocking(a[1]); set_nonblocking(o[0]); set_nonblocking(o[1]);
    DelOtherCtx c = { &l.ev, o[0], 0 };
    WatcherCtx other = {0};
    ASSERT_EQ(kl_watcher_add(&l.ev, a[0], KL_EVENT_READ, del_other_cb, &c), 0);
    ASSERT_EQ(kl_watcher_add(&l.ev, o[0], KL_EVENT_READ, test_watcher_cb, &other), 0);
    (void)kl_test_sockwrite(a[1], "1", 1);
    pump_until(&l.ev, &c.calls, 1);
    ASSERT_EQ(c.calls, 1);
    (void)kl_test_sockwrite(a[1], "2", 1);
    pump_until(&l.ev, &c.calls, 2);
    EXPECT_EQ(c.calls, 2);              /* the watcher was re-armed despite the other's removal */
    EXPECT_EQ(other.called, 0);
    kl_watcher_del(&l.ev, a[0]);
    kl_test_closesock(a[0]); kl_test_closesock(a[1]); kl_test_closesock(o[0]); kl_test_closesock(o[1]);
    test_loop_free(&l);
}

/* ── Re-arm identity: the watcher that was dispatched, and only that registration ──
 * After a callback the loop re-arms the dispatched registration unless the callback removed it or
 * changed its interest. These cases discriminate on a one-shot backend (io_uring's POLL_ADD), where a
 * missed re-arm means the fd is never watched again; elsewhere they check that nothing regresses. */

typedef struct {
    KlEventCtx *ev;
    int self_fd, other_fd;
    int mode;
    int calls;
} RearmCtx;

static int g_other_calls, g_new_calls;

static void drain_fd(KlSocketHandle fd) { char b[64]; while (kl_test_sockread(fd, b, sizeof b) > 0) {} }
static void other_cb(KlSocketHandle fd, KlEventMask r, void *u) { (void)r; (void)u; drain_fd(fd); g_other_calls++; }
static void new_cb(KlSocketHandle fd, KlEventMask r, void *u) { (void)r; (void)u; drain_fd(fd); g_new_calls++; }

enum { RM_DEL_OTHER = 1, RM_DEL_OTHER_MOD_SELF, RM_REPLACE_SELF, RM_REPLACE_OTHER_IN_BATCH, RM_UPSERT_SELF };

static void rearm_cb(KlSocketHandle fd, KlEventMask r, void *u) {
    (void)r;
    RearmCtx *c = u;
    drain_fd(fd);
    if (c->calls++ != 0) return;
    switch (c->mode) {
    case RM_DEL_OTHER:                /* case 1 */
        kl_watcher_del(c->ev, c->other_fd);
        break;
    case RM_DEL_OTHER_MOD_SELF:       /* case 5 */
        kl_watcher_del(c->ev, c->other_fd);
        kl_watcher_mod(c->ev, fd, KL_EVENT_READ);
        break;
    case RM_REPLACE_SELF:             /* case 4: a new registration on the same fd */
        kl_watcher_del(c->ev, fd);
        kl_watcher_add(c->ev, fd, KL_EVENT_READ, new_cb, NULL);
        break;
    case RM_REPLACE_OTHER_IN_BATCH:   /* case 9: the other's event is in the same batch */
        kl_watcher_del(c->ev, c->other_fd);
        kl_watcher_add(c->ev, c->other_fd, KL_EVENT_READ, new_cb, NULL);
        break;
    case RM_UPSERT_SELF:              /* an in-place upsert with the same interest */
        kl_watcher_add(c->ev, fd, KL_EVENT_READ, rearm_cb, c);
        break;
    }
}

/* Run one mode: the watcher's first callback acts, then three more reads follow. Returns its calls;
 * *other / *fresh: calls on the original other watcher / on a replacement registration. */
static int run_rearm(int mode, int other_first, int *other, int *fresh) {
    TestLoop l;
    if (test_loop_init(&l) != 0) return -1;
    int a[2], o[2];
    kl_test_socketpair(a);
    kl_test_socketpair(o);
    set_nonblocking(a[0]); set_nonblocking(o[0]);
    RearmCtx c = { &l.ev, a[0], o[0], mode, 0 };
    g_other_calls = g_new_calls = 0;
    kl_watcher_add(&l.ev, a[0], KL_EVENT_READ, rearm_cb, &c);
    kl_watcher_add(&l.ev, o[0], KL_EVENT_READ, other_cb, NULL);
    if (other_first) (void)kl_test_sockwrite(o[1], "o", 1);   /* its event lands in the same batch */
    (void)kl_test_sockwrite(a[1], "1", 1);
    pump_until(&l.ev, &c.calls, 1);
    for (int i = 0; i < 3; i++) {
        (void)kl_test_sockwrite(a[1], "2", 1);
        int want = (mode == RM_REPLACE_SELF) ? i + 1 : c.calls + 1;
        if (mode == RM_REPLACE_SELF) pump_until(&l.ev, &g_new_calls, want); else pump_until(&l.ev, &c.calls, want);
    }
    if (mode == RM_REPLACE_OTHER_IN_BATCH) {
        (void)kl_test_sockwrite(o[1], "p", 1);
        pump_until(&l.ev, &g_new_calls, g_new_calls + 1);
    }
    *other = g_other_calls;
    *fresh = g_new_calls;
    int calls = c.calls;
    kl_watcher_del(&l.ev, a[0]);
    kl_watcher_del(&l.ev, o[0]);
    test_loop_free(&l);
    kl_test_closesock(a[0]); kl_test_closesock(a[1]); kl_test_closesock(o[0]); kl_test_closesock(o[1]);
    return calls;
}

UTEST(async, rearm_after_removing_an_unrelated_watcher) {
    int other, fresh;
    EXPECT_EQ(run_rearm(RM_DEL_OTHER, 0, &other, &fresh), 4);
    EXPECT_EQ(other, 0);
}

UTEST(async, rearm_after_removing_another_and_modifying_itself) {
    int other, fresh;
    EXPECT_EQ(run_rearm(RM_DEL_OTHER_MOD_SELF, 0, &other, &fresh), 4);
}

UTEST(async, replaced_registration_is_its_own_not_the_old_one) {
    int other, fresh;
    EXPECT_EQ(run_rearm(RM_REPLACE_SELF, 0, &other, &fresh), 1);   /* the old one never runs again */
    EXPECT_EQ(fresh, 3);                                            /* the new one gets every read */
}

UTEST(async, rearm_when_another_event_in_the_batch_is_invalidated) {
    int other, fresh;
    EXPECT_EQ(run_rearm(RM_REPLACE_OTHER_IN_BATCH, 1, &other, &fresh), 4);
    /* Batch order is the backend's. If the other's event came first, it ran once before being replaced.
     * If it came second, it was stale and was dropped, never delivered to the freed node (ASan checks
     * that), and its unread byte then goes to the replacement on a level-triggered backend. Either way
     * the old watcher runs at most once and the replacement gets the later read. */
    EXPECT_TRUE(other <= 1);
    EXPECT_TRUE(fresh >= 1);
}

UTEST(async, rearm_after_an_in_place_upsert) {
    int other, fresh;
    EXPECT_EQ(run_rearm(RM_UPSERT_SELF, 0, &other, &fresh), 4);
}

/* Address reuse (ABA) at dispatch depth 0: the callback removes its own watcher and creates a DETACHED
 * one on the same fd, which a LIFO allocator places at the freed node's address with the same mask. It
 * is a different registration, and a detached watcher must get no readiness relay, so the loop must not
 * re-arm it. The allocator makes the reuse deterministic, and the test asserts it really happened. */
typedef struct { void *cached; size_t cached_size; } LifoState;
static void *lifo_malloc(void *ctx, size_t n) {
    LifoState *s = ctx;
    if (s->cached && s->cached_size == n) { void *p = s->cached; s->cached = NULL; return p; }
    return malloc(n);
}
static void *lifo_realloc(void *ctx, void *p, size_t o, size_t n) { (void)ctx; (void)o; return realloc(p, n); }
static void lifo_free(void *ctx, void *p, size_t n) {   /* keeps the most recently freed block */
    LifoState *s = ctx;
    free(s->cached);
    s->cached = p;
    s->cached_size = n;
}

static KlEventCtx *g_aba_ev;
static void *g_aba_orig, *g_aba_new;
static int g_aba_detached_calls;
static void aba_detached_cb(KlSocketHandle fd, KlEventMask r, void *u) { (void)r; (void)u; drain_fd(fd); g_aba_detached_calls++; }
static void aba_cb(KlSocketHandle fd, KlEventMask r, void *u) {
    (void)r; (void)u;
    kl_watcher_del(g_aba_ev, fd);
    g_aba_new = kl_watcher_add_detached(g_aba_ev, fd, aba_detached_cb, NULL);
}

UTEST(async, reused_watcher_address_is_not_rearmed_as_the_old_registration) {
    LifoState ls = { NULL, 0 };
    KlAllocator alloc = { lifo_malloc, lifo_realloc, lifo_free, &ls };
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    g_aba_ev = &ev;
    g_aba_detached_calls = 0;
    int a[2];
    ASSERT_EQ(kl_test_socketpair(a), 0);
    set_nonblocking(a[0]); set_nonblocking(a[1]);
    ASSERT_EQ(kl_watcher_add(&ev, a[0], KL_EVENT_WRITE, aba_cb, NULL), 0);
    g_aba_orig = kl_watcher_add_detached(&ev, a[0], aba_cb, NULL);   /* the existing node's tag */
    KlEvent e = { g_aba_orig, KL_EVENT_WRITE };
    (void)kl_event_dispatch(&ev, &e);                                 /* depth 0, unbatched */
    ASSERT_TRUE(g_aba_new == g_aba_orig);                             /* the address really was reused */
    for (int i = 0; i < 5; i++) kl_event_ctx_run(&ev, 16, 20);
    EXPECT_EQ(g_aba_detached_calls, 0);                               /* no readiness relay for it */
    kl_watcher_del(&ev, a[0]);
    kl_event_ctx_free(&ev);
    free(ls.cached);
    kl_test_closesock(a[0]); kl_test_closesock(a[1]);
}

/* The thread-pool completion path: done_fn runs inside the pool's wakeup-watcher callback and removes a
 * DIFFERENT watcher (as completing a suspended HTTP op removes its hang-up watcher). The pool's wakeup
 * must stay armed, so every later item still completes. */
typedef struct { KlEventCtx *ev; int victim_fd; int done; } PoolCtx;
static void pool_work(void *u) { (void)u; }
static void pool_done(void *u) {
    PoolCtx *c = u;
    if (c->done++ == 0) kl_watcher_del(c->ev, c->victim_fd);
}

UTEST(async, thread_pool_wakeup_rearmed_after_done_fn_removes_another_watcher) {
    TestLoop l;
    ASSERT_EQ(test_loop_init(&l), 0);
    int v[2];
    ASSERT_EQ(kl_test_socketpair(v), 0);
    set_nonblocking(v[0]);
    WatcherCtx victim = {0};
    ASSERT_EQ(kl_watcher_add(&l.ev, v[0], KL_EVENT_READ, test_watcher_cb, &victim), 0);
    KlThreadPoolConfig cfg = { .num_workers = 1, .queue_capacity = 4 };
    KlThreadPool *pool = kl_thread_pool_create(&l.ev, &cfg);
    ASSERT_TRUE(pool != NULL);
    PoolCtx c = { &l.ev, v[0], 0 };
    for (int i = 0; i < 3; i++) {
        KlWorkItem item = { .work_fn = pool_work, .done_fn = pool_done, .cancel_fn = NULL, .user_data = &c };
        ASSERT_EQ(kl_thread_pool_submit(pool, &item), 0);
        pump_until(&l.ev, &c.done, i + 1);   /* one at a time: each needs a fresh wakeup */
        EXPECT_EQ(c.done, i + 1);
    }
    kl_thread_pool_free(pool);
    test_loop_free(&l);
    kl_test_closesock(v[0]); kl_test_closesock(v[1]);
}

UTEST_MAIN();
