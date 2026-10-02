/* test_wakeup.c: the public KlWakeup cross-thread signal channel.
 *
 * Covers what a consumer actually does with it: open the channel, register its
 * read end as a KlWatcher, signal it from another thread, and have the callback
 * run on the loop thread. The Windows subset runs this too, which is the point:
 * a raw pipe(2) would leave the watcher dead there.
 */
#include "utest.h"
#include <keel/keel.h>
#include "net_compat.h"

#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

typedef struct {
    KlWakeup *wakeup;
    int called;
} SignalCtx;

static void wakeup_cb(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)fd; (void)ready;
    SignalCtx *ctx = user_data;
    kl_wakeup_drain(ctx->wakeup);
    ctx->called++;
}

static void signal_thread(void *arg) {
    kl_wakeup_signal((const KlWakeup *)arg);
}

/* Run ticks until the callback has fired (or the attempts run out). */
static void pump_until_called(KlEventCtx *ev, const SignalCtx *ctx) {
    for (int i = 0; i < 40 && ctx->called == 0; i++)
        kl_event_ctx_run(ev, 8, 50);
}

/* ── Tests ───────────────────────────────────────────────────────── */

UTEST(wakeup, open_yields_two_valid_handles) {
    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);
    ASSERT_TRUE(kl_handle_valid(w.rd));
    ASSERT_TRUE(kl_handle_valid(w.wr));
    ASSERT_TRUE(w.rd != w.wr);
    kl_wakeup_close(&w);
}

UTEST(wakeup, close_invalidates_both_ends) {
    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);
    kl_wakeup_close(&w);
    ASSERT_FALSE(kl_handle_valid(w.rd));
    ASSERT_FALSE(kl_handle_valid(w.wr));
    kl_wakeup_close(&w);   /* idempotent: closing a closed channel is a no-op */
    ASSERT_FALSE(kl_handle_valid(w.rd));
}

UTEST(wakeup, signal_from_another_thread_runs_the_watcher) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);

    SignalCtx ctx = {.wakeup = &w};
    ASSERT_EQ(kl_watcher_add(&ev, w.rd, KL_EVENT_READ, wakeup_cb, &ctx), 0);

    KlPlatThread th;
    ASSERT_EQ(kl_plat_thread_create(&th, signal_thread, &w), 0);
    pump_until_called(&ev, &ctx);
    kl_plat_thread_join(&th);

    ASSERT_EQ(ctx.called, 1);

    kl_watcher_del(&ev, w.rd);
    kl_wakeup_close(&w);
    kl_event_ctx_free(&ev);
}

/* A drained channel goes quiet: without the drain a readiness backend would
 * report the read end ready on every tick and spin the loop. */
UTEST(wakeup, drained_channel_stops_firing) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);

    SignalCtx ctx = {.wakeup = &w};
    ASSERT_EQ(kl_watcher_add(&ev, w.rd, KL_EVENT_READ, wakeup_cb, &ctx), 0);

    kl_wakeup_signal(&w);
    pump_until_called(&ev, &ctx);
    ASSERT_EQ(ctx.called, 1);

    for (int i = 0; i < 3; i++)
        kl_event_ctx_run(&ev, 8, 10);
    ASSERT_EQ(ctx.called, 1);   /* still 1: the byte was consumed */

    kl_watcher_del(&ev, w.rd);
    kl_wakeup_close(&w);
    kl_event_ctx_free(&ev);
}

/* Repeated signals coalesce into at least one wakeup and never lose the loop. */
UTEST(wakeup, repeated_signals_keep_waking_the_loop) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);

    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);

    SignalCtx ctx = {.wakeup = &w};
    ASSERT_EQ(kl_watcher_add(&ev, w.rd, KL_EVENT_READ, wakeup_cb, &ctx), 0);

    for (int round = 1; round <= 3; round++) {
        kl_wakeup_signal(&w);
        for (int i = 0; i < 40 && ctx.called < round; i++)
            kl_event_ctx_run(&ev, 8, 50);
        ASSERT_EQ(ctx.called, round);
    }

    kl_watcher_del(&ev, w.rd);
    kl_wakeup_close(&w);
    kl_event_ctx_free(&ev);
}

UTEST(wakeup, null_channel_is_rejected_not_crashed) {
    ASSERT_EQ(kl_wakeup_open(NULL), -1);
    kl_wakeup_signal(NULL);
    kl_wakeup_drain(NULL);
    kl_wakeup_close(NULL);
}

/* A flood of signals with nobody draining must never block the signaller: once the channel is full
 * a wakeup is already pending, so the extra bytes are not needed. Before the fix the write end was
 * blocking and the flooding thread hung on the full pipe / socket pair. The main thread waits with a
 * deadline, then drains until the thread returns, so a regression fails the test instead of hanging. */
typedef struct { KlWakeup *w; volatile int finished; } FloodCtx;

#define FLOOD_SIGNALS (1 << 20)   /* well past a pipe (64 KiB) or a loopback pair's buffers */

static void flood_thread(void *arg) {
    FloodCtx *f = arg;
    for (int i = 0; i < FLOOD_SIGNALS; i++) kl_wakeup_signal(f->w);
    f->finished = 1;
}

UTEST(wakeup, signal_never_blocks_on_a_full_channel) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);

    FloodCtx f = {.w = &w, .finished = 0};
    KlPlatThread t;
    ASSERT_EQ(kl_plat_thread_create(&t, flood_thread, &f), 0);
    for (int i = 0; i < 600 && !f.finished; i++)   /* up to ~30 s, driving the loop meanwhile */
        (void)kl_event_ctx_run(&ev, 8, 50);
    int finished_undrained = f.finished;
    while (!f.finished) kl_wakeup_drain(&w);          /* unstick a blocked signaller before joining */
    kl_plat_thread_join(&t);
    ASSERT_EQ(finished_undrained, 1);

    /* One drain empties the flooded channel: the burst costs a single wakeup. */
    SignalCtx ctx = {.wakeup = &w};
    ASSERT_EQ(kl_watcher_add(&ev, w.rd, KL_EVENT_READ, wakeup_cb, &ctx), 0);
    pump_until_called(&ev, &ctx);
    for (int i = 0; i < 3; i++) (void)kl_event_ctx_run(&ev, 8, 10);
    ASSERT_EQ(ctx.called, 1);

    /* And the channel still works afterwards. */
    kl_wakeup_signal(&w);
    for (int i = 0; i < 40 && ctx.called < 2; i++) (void)kl_event_ctx_run(&ev, 8, 50);
    ASSERT_EQ(ctx.called, 2);

    kl_watcher_del(&ev, w.rd);
    kl_wakeup_close(&w);
    kl_event_ctx_free(&ev);
}

/* The Windows pair is a loopback TCP connection carrying 1-byte signals: with Nagle on, a signal
 * sent while an earlier one is unacknowledged waits for the delayed ACK (up to ~200 ms). */
#if defined(_WIN32) && !defined(__COSMOPOLITAN__)
UTEST(wakeup, windows_pair_sends_each_signal_at_once) {
    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);
    int v = 0, len = (int)sizeof v;
    int r = getsockopt((SOCKET)w.wr, IPPROTO_TCP, TCP_NODELAY, (char *)&v, &len);
    kl_wakeup_close(&w);
    ASSERT_EQ(r, 0);
    ASSERT_NE(v, 0);                             /* was 0: Nagle on */
}
#endif

UTEST_MAIN();
