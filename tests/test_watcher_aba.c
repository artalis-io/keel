/*
 * test_watcher_aba.c — deterministic regression for the watcher pointer-reuse ABA
 * (remedy: deferred node reclamation).
 *
 * The bug: kl_event_dispatch matches a watcher by POINTER IDENTITY against ctx->watchers. If a
 * watcher B is deleted DURING a drained batch and its freed node address is reused by a new watcher
 * C before B's already-captured stale event is dispatched, the pointer scan finds C at B's address
 * and MISDELIVERS B's event to C. Single-shot does not help (the stale event is B's one legitimate
 * completion).
 *
 * These tests force DETERMINISTIC same-address reuse via a fixture allocator (a LIFO free-list keyed
 * on sizeof(KlWatcher): a freed watcher node is handed back to the very next watcher allocation), and
 * drive all three internal dispatch loops:
 *   - readiness  (kl_event_ctx_run over a mock loop whose .wait returns a scripted [A, staleB] batch)
 *   - completion (kl_event_ctx_run → kl_comp_run over a mock .drain returning [A, staleB] as
 *                 KL_COMP_WATCHER)
 *   - server     (a real kl_http_server_run: a pre-armed watcher runs a probe INSIDE the server's batch)
 * plus unit checks for nested brackets, unmatched end, overflow refusal, and depth-0 reclamation.
 *
 * Pre-fix behaviour (kl_watcher_del freeing immediately even inside a batch) fails these: the deleted
 * node's address is reused and the stale event is misdelivered. Post-fix (deferred reclamation) the
 * address is NOT reused mid-batch, so the stale event finds no live node and is dropped.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/event.h>
#include "../src/completion.h"   /* KlCompletionOps, KlCompletionEvent, KL_COMP_WATCHER */
#include "net_compat.h"
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

/* ── fixture allocator: deterministic same-size reuse for KlWatcher nodes ───────────────────────── */
#define WSZ (sizeof(KlWatcher))
static struct FA {
    void *lifo[256]; int lifo_n;      /* freed WSZ blocks, LIFO (most-recent reused first) */
    void *handed[256]; int handed_n;  /* every WSZ block handed out, in order */
    void *last;                       /* most-recent WSZ block handed out */
    int   wsz_free_calls;             /* # of kl_free(size==WSZ) actually performed (not deferred) */
} g_fa;

static void *fa_malloc(void *ctx, size_t sz) {
    (void)ctx;
    if (sz == WSZ) {
        void *p = (g_fa.lifo_n > 0) ? g_fa.lifo[--g_fa.lifo_n] : malloc(sz);
        g_fa.last = p;
        if (g_fa.handed_n < 256) g_fa.handed[g_fa.handed_n++] = p;
        return p;
    }
    return malloc(sz);
}
static void *fa_realloc(void *ctx, void *p, size_t oldsz, size_t newsz) {
    (void)ctx; (void)oldsz; return realloc(p, newsz);
}
static void fa_free(void *ctx, void *p, size_t sz) {
    (void)ctx;
    if (!p) return;
    if (sz == WSZ) {                         /* keep for reuse (LIFO), do not return to libc yet */
        g_fa.wsz_free_calls++;
        if (g_fa.lifo_n < 256) { g_fa.lifo[g_fa.lifo_n++] = p; return; }
    }
    free(p);
}
static KlAllocator fa_alloc(void) {
    KlAllocator a; memset(&a, 0, sizeof(a));
    a.malloc = fa_malloc; a.realloc = fa_realloc; a.free = fa_free; a.ctx = NULL;
    return a;
}
static void fa_reset(void) {
    for (int i = 0; i < g_fa.lifo_n; i++) free(g_fa.lifo[i]);   /* drain reuse pool — no leak */
    memset(&g_fa, 0, sizeof(g_fa));
}
static void *tag(void *node) { return (void *)((uintptr_t)node | 1u); }

/* ── shared ABA actors: watcher A deletes B and adds C inside the batch; C must never be called ──── */
static KlEventCtx   *g_ctx;
static KlSocketHandle g_fdB, g_fdC;
static void         *g_Bnode, *g_Cnode;
static int           g_c_called;

static void c_cb(KlSocketHandle fd, KlEventMask r, void *ud) { (void)fd; (void)r; (void)ud; g_c_called++; }
static void a_cb(KlSocketHandle fd, KlEventMask r, void *ud) {
    (void)fd; (void)r; (void)ud;
    kl_watcher_del(g_ctx, g_fdB);                                  /* delete B mid-batch */
    kl_watcher_add(g_ctx, g_fdC, KL_EVENT_READ, c_cb, NULL);       /* add C (would reuse B pre-fix) */
    g_Cnode = g_fa.last;
}

/* ── mock readiness loop: .wait returns a scripted batch once ───────────────────────────────────── */
static int noop_init(KlEventLoop *l) { (void)l; return 0; }
static int noop_add(KlEventLoop *l, KlSocketHandle fd, KlEventMask m, void *u) { (void)l;(void)fd;(void)m;(void)u; return 0; }
static int noop_del(KlEventLoop *l, KlSocketHandle fd) { (void)l;(void)fd; return 0; }
static int noop_mod(KlEventLoop *l, KlSocketHandle fd, KlEventMask m, void *u) { (void)l;(void)fd;(void)m;(void)u; return 0; }
static void noop_close(KlEventLoop *l) { (void)l; }
static unsigned rdy_caps(const KlEventLoop *l) { (void)l; return KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD; }

static struct { KlEvent evs[8]; int n; int served; int calls; } g_wait;
static int rdy_wait(KlEventLoop *l, KlEvent *out, int max, int timeout) {
    (void)l; (void)timeout;
    g_wait.calls++;                       /* proves .wait was (not) reached — begin-before-acquire */
    if (g_wait.served) return 0;
    g_wait.served = 1;
    int n = g_wait.n < max ? g_wait.n : max;
    for (int i = 0; i < n; i++) out[i] = g_wait.evs[i];
    return n;
}
static const KlEventOps RDY_OPS = {
    .add = noop_add, .del = noop_del, .mod = noop_mod, .wait = rdy_wait, .close = noop_close, .caps = rdy_caps
};

/* ── mock completion loop: .completion->drain returns a scripted KL_COMP_WATCHER batch once ──────── */
static unsigned cmp_caps(const KlEventLoop *l) { (void)l; return KL_EVENT_CAP_COMPLETION; }
static struct { KlCompletionEvent evs[8]; int n; int served; int calls; } g_drain;
static int cmp_drain(struct KlEventCtx *ctx, KlCompletionEvent *out, int max, int timeout) {
    (void)ctx; (void)timeout;
    g_drain.calls++;                      /* proves .drain was (not) reached — begin-before-acquire */
    if (g_drain.served) return 0;
    g_drain.served = 1;
    int n = g_drain.n < max ? g_drain.n : max;
    for (int i = 0; i < n; i++) out[i] = g_drain.evs[i];
    return n;
}
static const KlCompletionOps CMP_COMP = { .drain = cmp_drain };
static const KlEventOps CMP_OPS = {
    .add = noop_add, .del = noop_del, .mod = noop_mod, .close = noop_close, .caps = cmp_caps, .completion = &CMP_COMP
};

/* ── readiness-loop ABA ─────────────────────────────────────────────────────────────────────────── */
UTEST(watcher_aba, readiness_loop_no_misdelivery) {
    fa_reset();
    KlAllocator a = fa_alloc();
    KlEventCtx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.loop.ops = &RDY_OPS; ctx.alloc = &a;
    g_ctx = &ctx; g_c_called = 0; g_Cnode = NULL;
    g_fdB = (KlSocketHandle)101; g_fdC = (KlSocketHandle)102;

    ASSERT_EQ(kl_watcher_add(&ctx, (KlSocketHandle)100, KL_EVENT_READ, a_cb, NULL), 0);
    void *Anode = g_fa.handed[0];
    ASSERT_EQ(kl_watcher_add(&ctx, g_fdB, KL_EVENT_READ, c_cb, NULL), 0);
    g_Bnode = g_fa.handed[1];

    g_wait.served = 0; g_wait.n = 2;
    g_wait.evs[0].udata = tag(Anode);   g_wait.evs[0].ready = KL_EVENT_READ;   /* A: dispatched first */
    g_wait.evs[1].udata = tag(g_Bnode); g_wait.evs[1].ready = KL_EVENT_READ;   /* stale B, later in batch */

    ASSERT_TRUE(kl_event_ctx_run(&ctx, 8, 0) >= 0);

    ASSERT_NE((uintptr_t)g_Cnode, (uintptr_t)g_Bnode);   /* C did NOT reuse B's address */
    ASSERT_EQ(g_c_called, 0);                            /* B's stale event was NOT misdelivered to C */

    kl_event_ctx_free(&ctx);
    fa_reset();
}

/* ── completion-loop ABA ────────────────────────────────────────────────────────────────────────── */
UTEST(watcher_aba, completion_loop_no_misdelivery) {
    fa_reset();
    KlAllocator a = fa_alloc();
    KlEventCtx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.loop.ops = &CMP_OPS; ctx.alloc = &a;
    g_ctx = &ctx; g_c_called = 0; g_Cnode = NULL;
    g_fdB = (KlSocketHandle)101; g_fdC = (KlSocketHandle)102;

    ASSERT_EQ(kl_watcher_add(&ctx, (KlSocketHandle)100, KL_EVENT_READ, a_cb, NULL), 0);
    void *Anode = g_fa.handed[0];
    ASSERT_EQ(kl_watcher_add(&ctx, g_fdB, KL_EVENT_READ, c_cb, NULL), 0);
    g_Bnode = g_fa.handed[1];

    g_drain.served = 0; g_drain.n = 2;
    memset(g_drain.evs, 0, sizeof(g_drain.evs));
    g_drain.evs[0].kind = KL_COMP_WATCHER; g_drain.evs[0].target = tag(Anode);   g_drain.evs[0].bytes = KL_EVENT_READ;
    g_drain.evs[1].kind = KL_COMP_WATCHER; g_drain.evs[1].target = tag(g_Bnode); g_drain.evs[1].bytes = KL_EVENT_READ;

    ASSERT_TRUE(kl_event_ctx_run(&ctx, 8, 0) >= 0);   /* delegates to kl_comp_run (caps=COMPLETION) */

    ASSERT_NE((uintptr_t)g_Cnode, (uintptr_t)g_Bnode);
    ASSERT_EQ(g_c_called, 0);

    kl_event_ctx_free(&ctx);
    fa_reset();
}

/* ── nested brackets: reclaim only at the OUTERMOST close ───────────────────────────────────────── */
UTEST(watcher_aba, nested_reclaim_at_outermost) {
    fa_reset();
    KlAllocator a = fa_alloc();
    KlEventCtx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.loop.ops = &RDY_OPS; ctx.alloc = &a;
    ASSERT_EQ(kl_watcher_add(&ctx, (KlSocketHandle)200, KL_EVENT_READ, c_cb, NULL), 0);

    ASSERT_EQ(kl_event_ctx_dispatch_begin(&ctx), 0);   /* depth 1 */
    ASSERT_EQ(kl_event_ctx_dispatch_begin(&ctx), 0);   /* depth 2 */
    int before = g_fa.wsz_free_calls;
    kl_watcher_del(&ctx, (KlSocketHandle)200);          /* deferred (depth > 0) */
    ASSERT_EQ(g_fa.wsz_free_calls, before);             /* not yet freed */
    kl_event_ctx_dispatch_end(&ctx);                    /* depth 1 — not outermost */
    ASSERT_EQ(g_fa.wsz_free_calls, before);             /* still deferred */
    kl_event_ctx_dispatch_end(&ctx);                    /* depth 0 — reclaim now */
    ASSERT_EQ(g_fa.wsz_free_calls, before + 1);

    kl_event_ctx_free(&ctx);
    fa_reset();
}

/* ── depth-0 (unbatched) del frees immediately — the fast path is unchanged ─────────────────────── */
UTEST(watcher_aba, depth_zero_immediate_free) {
    fa_reset();
    KlAllocator a = fa_alloc();
    KlEventCtx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.loop.ops = &RDY_OPS; ctx.alloc = &a;
    ASSERT_EQ(kl_watcher_add(&ctx, (KlSocketHandle)201, KL_EVENT_READ, c_cb, NULL), 0);
    int before = g_fa.wsz_free_calls;
    kl_watcher_del(&ctx, (KlSocketHandle)201);          /* depth 0 → immediate free */
    ASSERT_EQ(g_fa.wsz_free_calls, before + 1);
    kl_event_ctx_free(&ctx);
    fa_reset();
}

/* ── unmatched end is a no-op (underflow guard) ─────────────────────────────────────────────────── */
UTEST(watcher_aba, unmatched_end_is_noop) {
    KlEventCtx ctx; memset(&ctx, 0, sizeof(ctx));
    kl_event_ctx_dispatch_end(&ctx);       /* depth already 0 */
    ASSERT_EQ(ctx.dispatch_depth, 0);      /* no underflow */
}

/* ── begin refuses to overflow, consuming no depth ──────────────────────────────────────────────── */
UTEST(watcher_aba, overflow_refusal) {
    KlEventCtx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.dispatch_depth = INT_MAX;
    ASSERT_EQ(kl_event_ctx_dispatch_begin(&ctx), -1);    /* refuses rather than overflowing */
    ASSERT_EQ(ctx.dispatch_depth, INT_MAX);              /* unchanged */
    ctx.dispatch_depth = 0;
}

/* An overflow refusal at the READINESS loop entry must acquire NOTHING: begin precedes the wait, so
 * .wait is never called and no scripted event is consumed. */
UTEST(watcher_aba, overflow_refusal_readiness_acquires_nothing) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.loop.ops = &RDY_OPS; ctx.alloc = &a;
    ctx.dispatch_depth = INT_MAX;
    g_wait.served = 0; g_wait.calls = 0; g_wait.n = 1;
    g_wait.evs[0].udata = tag((void *)0x10); g_wait.evs[0].ready = KL_EVENT_READ;

    ASSERT_EQ(kl_event_ctx_run(&ctx, 8, 0), -1);   /* begin-before-acquire refuses */
    ASSERT_EQ(g_wait.calls, 0);                    /* .wait never reached */
    ASSERT_EQ(g_wait.served, 0);                   /* no event consumed */
    ctx.dispatch_depth = 0;
}

/* Same for the COMPLETION loop entry: begin precedes the drain, so .drain is never called. */
UTEST(watcher_aba, overflow_refusal_completion_acquires_nothing) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.loop.ops = &CMP_OPS; ctx.alloc = &a;
    ctx.dispatch_depth = INT_MAX;
    g_drain.served = 0; g_drain.calls = 0; g_drain.n = 1;
    memset(g_drain.evs, 0, sizeof(g_drain.evs));
    g_drain.evs[0].kind = KL_COMP_WATCHER; g_drain.evs[0].target = tag((void *)0x10); g_drain.evs[0].bytes = KL_EVENT_READ;

    ASSERT_EQ(kl_event_ctx_run(&ctx, 8, 0), -1);   /* delegates to kl_comp_run; begin precedes drain */
    ASSERT_EQ(g_drain.calls, 0);                   /* .drain never reached */
    ASSERT_EQ(g_drain.served, 0);                  /* no event consumed */
    ctx.dispatch_depth = 0;
}

/* ── server-loop ABA: the REAL kl_http_server_run loop receives a scripted [A, stale-B] readiness batch ─
 * A scripted readiness KlEventProvider is injected into KlHttpServer (cfg.event_provider). Its .wait
 * returns [tag(A), tag(stale-B)] once; the server dispatches them inside its own deferred-reclamation bracket, so A
 * deletes B + adds C, and B's already-captured stale event must NOT be misdelivered to C — the exact
 * ABA sequence, driven through the server loop (not a probe). */
static KlHttpServer      g_srv;
static volatile int  g_srv_batch_dispatched;

static int srv_wait(KlEventLoop *l, KlEvent *out, int max, int timeout) {
    (void)l; (void)timeout;
    if (!g_wait.served) {                       /* first tick: deliver the scripted batch */
        g_wait.served = 1;
        int n = g_wait.n < max ? g_wait.n : max;
        for (int i = 0; i < n; i++) out[i] = g_wait.evs[i];
        return n;
    }
    /* Second tick onward: the batch was fully dispatched last iteration. Signal + idle so
     * kl_http_server_stop (running=0) is observed at the loop top without a hot spin. */
    __atomic_store_n(&g_srv_batch_dispatched, 1, __ATOMIC_RELEASE);
    struct timespec ts = { 0, 2 * 1000 * 1000 }; nanosleep(&ts, NULL);
    return 0;
}
static const KlEventOps SRV_RDY_OPS = {
    .init = noop_init, .add = noop_add, .del = noop_del, .mod = noop_mod,
    .wait = srv_wait, .close = noop_close, .caps = rdy_caps
};
static const KlEventProvider SRV_RDY_PROV = { &SRV_RDY_OPS, "aba-srv" };
static void *srv_thread(void *arg) { (void)arg; kl_http_server_run(&g_srv); return NULL; }

UTEST(watcher_aba, server_loop_no_misdelivery) {
    fa_reset();
    KlAllocator a = fa_alloc();
    KlHttpServerConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0; cfg.bind_addr = "127.0.0.1"; cfg.alloc = &a;
    cfg.event_provider = &SRV_RDY_PROV;        /* scripted readiness backend → deterministic batch */
    ASSERT_EQ(kl_http_server_init(&g_srv, &cfg), 0);

    g_ctx = &g_srv.ev; g_c_called = 0; g_Cnode = NULL;
    g_fdB = (KlSocketHandle)101; g_fdC = (KlSocketHandle)102;

    /* Add A and B to the server's ctx BEFORE the loop starts (thread-safe). */
    ASSERT_EQ(kl_watcher_add(&g_srv.ev, (KlSocketHandle)100, KL_EVENT_READ, a_cb, NULL), 0);
    void *Anode = g_fa.last;
    ASSERT_EQ(kl_watcher_add(&g_srv.ev, g_fdB, KL_EVENT_READ, c_cb, NULL), 0);
    g_Bnode = g_fa.last;

    g_wait.served = 0; g_wait.calls = 0; g_wait.n = 2;
    g_wait.evs[0].udata = tag(Anode);   g_wait.evs[0].ready = KL_EVENT_READ;   /* A first */
    g_wait.evs[1].udata = tag(g_Bnode); g_wait.evs[1].ready = KL_EVENT_READ;   /* stale B, same batch */
    g_srv_batch_dispatched = 0;

    pthread_t th;
    ASSERT_EQ(pthread_create(&th, NULL, srv_thread, NULL), 0);
    for (int i = 0; i < 1000 && !__atomic_load_n(&g_srv_batch_dispatched, __ATOMIC_ACQUIRE); i++) {
        struct timespec ts = { 0, 2 * 1000 * 1000 }; nanosleep(&ts, NULL);
    }
    int dispatched = __atomic_load_n(&g_srv_batch_dispatched, __ATOMIC_ACQUIRE);
    kl_http_server_stop(&g_srv);
    pthread_join(th, NULL);

    ASSERT_TRUE(dispatched);
    ASSERT_NE((uintptr_t)g_Cnode, (uintptr_t)g_Bnode);   /* C did not reuse B's address */
    ASSERT_EQ(g_c_called, 0);                            /* B's stale event NOT misdelivered to C */

    kl_http_server_free(&g_srv);
    fa_reset();
}

UTEST_MAIN();
