/*
 * test_loop_teardown.c: the event-loop teardown rule (kl_event_ctx_free, <keel/event_ctx.h>).
 *
 * Destroying a loop delivers NO further callback to anything still attached to it: a pending timer is
 * discarded without firing, a registered watcher is removed without its callback even if its handle
 * is already ready, and (completion engines) a posted operation is cancelled and reclaimed without a
 * terminal event. What the loop allocated is all given back. These cases pin that behaviour so the
 * documented rule cannot drift from the code; they run on every engine.
 *
 * The ORDER rule (release every attached object first) is the caller's side of the contract. Its
 * defined half is tested here: a datagram with a receive posted, closed and driven to on_close, then
 * the loop freed, leaves nothing behind on every engine. So are the two calls documented as inert
 * after the free. Breaking the order is undefined, so it is not asserted on.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <keel/wakeup.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>   /* caller-owned KlDatagram storage */
#include <string.h>
#include <stdlib.h>

/* Counting allocator: everything the loop takes, it must give back on free. */
typedef struct { long blocks; long long bytes; } Counts;
static Counts g_counts;
static void *ca_malloc(void *c, size_t n) {
    (void)c;
    void *p = malloc(n ? n : 1);
    if (p) { g_counts.blocks++; g_counts.bytes += (long long)n; }
    return p;
}
static void *ca_realloc(void *c, void *p, size_t o, size_t n) {
    (void)c;
    void *q = realloc(p, n ? n : 1);
    if (q) { if (!p) g_counts.blocks++; g_counts.bytes += (long long)n - (long long)(p ? o : 0); }
    return q;
}
static void ca_free(void *c, void *p, size_t n) {
    (void)c;
    if (!p) return;
    g_counts.blocks--; g_counts.bytes -= (long long)n;
    free(p);
}
static KlAllocator g_alloc = { ca_malloc, ca_realloc, ca_free, NULL };

static int g_timer_fired;
static void on_timer(void *ud) { (void)ud; g_timer_fired++; }

static int g_watcher_fired;
static void on_ready(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)fd; (void)ready; (void)ud;
    g_watcher_fired++;
}

UTEST(loop_teardown, pending_timers_are_discarded_not_fired) {
    Counts before = g_counts;
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    g_timer_fired = 0;
    ASSERT_GE(kl_timer_add(&ev, 0, on_timer, NULL), 0);          /* already due */
    ASSERT_GE(kl_timer_add(&ev, 60000, on_timer, NULL), 0);      /* far in the future */
    kl_event_ctx_free(&ev);
    ASSERT_EQ(g_timer_fired, 0);                                  /* not even the due one */
    ASSERT_EQ(g_counts.blocks, before.blocks);                    /* heap and loop given back */
    ASSERT_EQ(g_counts.bytes, before.bytes);
}

UTEST(loop_teardown, ready_watcher_is_removed_without_its_callback) {
    Counts before = g_counts;
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);
    g_watcher_fired = 0;
    ASSERT_EQ(kl_watcher_add(&ev, w.rd, KL_EVENT_READ, on_ready, NULL), 0);
    kl_wakeup_signal(&w);                                         /* the handle is READY now */
    kl_event_ctx_free(&ev);                                       /* ...but the loop never runs */
    ASSERT_EQ(g_watcher_fired, 0);
    ASSERT_EQ(g_counts.blocks, before.blocks);                    /* watcher node + loop state */
    ASSERT_EQ(g_counts.bytes, before.bytes);
    kl_wakeup_close(&w);                                          /* the fd was never the loop's */
}

UTEST(loop_teardown, a_freed_context_can_be_initialised_again) {
    Counts before = g_counts;
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    ASSERT_GE(kl_timer_add(&ev, 60000, on_timer, NULL), 0);
    kl_event_ctx_free(&ev);
    g_timer_fired = 0;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);               /* same storage, fresh loop */
    ASSERT_GE(kl_timer_add(&ev, 0, on_timer, NULL), 0);
    ASSERT_GE(kl_event_ctx_run(&ev, 8, 0), 0);
    ASSERT_EQ(g_timer_fired, 1);                                  /* the new loop works */
    kl_event_ctx_free(&ev);
    ASSERT_EQ(g_counts.blocks, before.blocks);
    ASSERT_EQ(g_counts.bytes, before.bytes);
}

/* The two documented exceptions: after the free, cancelling a timer and touching a watcher are inert
 * (the context's timer heap and watcher list are empty; nothing reaches the released loop). */
UTEST(loop_teardown, timer_cancel_and_watcher_calls_are_inert_after_free) {
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);
    int64_t id = kl_timer_add(&ev, 60000, on_timer, NULL);
    ASSERT_GE(id, 0);
    ASSERT_EQ(kl_watcher_add(&ev, w.rd, KL_EVENT_READ, on_ready, NULL), 0);
    kl_event_ctx_free(&ev);
    ASSERT_EQ(kl_timer_cancel(&ev, id), -1);                     /* nothing pending */
    ASSERT_EQ(kl_watcher_mod(&ev, w.rd, KL_EVENT_WRITE), -1);    /* no such watcher */
    kl_watcher_del(&ev, w.rd);                                    /* no-op */
    kl_wakeup_close(&w);
}

/* The caller's side of the contract, for a confirmed-detachment transport with an operation posted:
 * close it, drive the loop until on_close, free it, THEN free the loop. On every engine that leaves
 * nothing behind. (A completion engine's posted receive is retired by the close, not by the loop's
 * teardown.) */
static int g_dg_closes;
static void dg_on_close(void *ud, KlDatagramCloseResult r) { (void)ud; (void)r; g_dg_closes++; }
static void dg_on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                       const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)data; (void)len; (void)peer; (void)local; (void)flags;
}

UTEST(loop_teardown, release_first_then_free_leaves_nothing_behind) {
    Counts before = g_counts;
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &g_alloc), 0);
    KlDatagram dg;
    KlDatagramSocketConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.ctx = &ev; cfg.alloc = &g_alloc; cfg.bind_addr = "127.0.0.1";
    ASSERT_EQ(kl_datagram_socket_init(&dg, &cfg), 0);
    g_dg_closes = 0;
    kl_datagram_on_close(&dg, dg_on_close, NULL);
    ASSERT_EQ(kl_datagram_recv_start(&dg, dg_on_recv, NULL), 0);  /* a receive is now posted/armed */
    ASSERT_EQ(kl_datagram_close_begin(&dg), 0);
    for (int i = 0; i < 200 && kl_datagram_close_state(&dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        (void)kl_event_ctx_run(&ev, 16, 10);                      /* detachment needs the loop */
    ASSERT_EQ((int)kl_datagram_close_state(&dg), (int)KL_DGRAM_CLOSE_CLOSED);
    ASSERT_EQ(g_dg_closes, 1);
    ASSERT_EQ(kl_datagram_free(&dg), 0);
    kl_event_ctx_free(&ev);                                       /* only now */
    ASSERT_EQ(g_counts.blocks, before.blocks);
    ASSERT_EQ(g_counts.bytes, before.bytes);
}

UTEST_MAIN();
