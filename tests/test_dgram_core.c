/*
 * test_dgram_core.c — Phase B step 7A-3: the fixed-slot KlDgramCore assembly.
 *
 * Drives the integrated core over a SCRIPTED neutral adapter (no real socket/provider): init +
 * failure unwind; fixed-slot send FIFO / count-based backpressure / single-flight / writable+drain
 * edges; and the copy-before-accept PROVIDER CONTRACT — including a quarantined send whose object-owned
 * outbound pool is then released (safe only because the backend copied at submit).
 */
#include "utest.h"

#include "../src/datagram_core.h"

#include <keel/sockaddr.h>
#include <string.h>

/* ── scripted neutral adapter ─────────────────────────────────────────────────────────────── */

static KlDgramSubmitResult g_submit_result;   /* what submit returns (DONE/INFLIGHT/WOULDBLOCK/ERROR) */
static int      g_submit_calls;
static unsigned char g_submitted[64];         /* the backend's COPY of the last submitted payload */
static size_t   g_submitted_len;
static const void *g_retained_ptr;            /* the exact pointer submit was handed (into the pool) */

/* Copy-before-accept: the backend copies the payload into its OWN storage before returning, and keeps
 * only that copy — never dereferencing g_retained_ptr afterwards. */
static KlDgramSubmitResult test_submit(void *ctx, const void *data, size_t len,
                                       const KlSockAddr *peer, const KlSockAddr *local, int tos) {
    (void)ctx; (void)peer; (void)local; (void)tos;
    g_submit_calls++;
    g_retained_ptr  = data;                   /* record but DO NOT keep-deref (contract) */
    g_submitted_len = len < sizeof(g_submitted) ? len : sizeof(g_submitted);
    memcpy(g_submitted, data, g_submitted_len);   /* the copy-before-accept */
    return g_submit_result;
}

static KlDgramRetireResult g_retire_send;
static KlDgramRetireResult test_retire(void *ctx, KlDgramOpKind kind, int *te) {
    (void)ctx; *te = 0;
    if (kind == KL_DGRAM_OP_SEND) return g_retire_send;
    return KL_DGRAM_RETIRE_RETIRED;   /* recv: not exercised here */
}

static int  g_arm_calls;
static int  test_arm(void *ctx)    { (void)ctx; g_arm_calls++; return 0; }   /* recv arm — no completion */
static void test_deliver(void *ctx, const void *d, size_t n, const KlSockAddr *p,
                         const KlSockAddr *l, unsigned f) { (void)ctx;(void)d;(void)n;(void)p;(void)l;(void)f; }

static int g_on_close_calls;
static KlDatagramCloseResult g_on_close_result;
static void test_on_close(void *ctx, KlDatagramCloseResult r) { (void)ctx; g_on_close_calls++; g_on_close_result = r; }

static void reset_globals(void) {
    g_submit_result = KL_DGRAM_SUBMIT_INFLIGHT; g_submit_calls = 0; g_submitted_len = 0;
    memset(g_submitted, 0, sizeof(g_submitted)); g_retained_ptr = NULL;
    g_retire_send = KL_DGRAM_RETIRE_RETIRED; g_arm_calls = 0;
    g_on_close_calls = 0; g_on_close_result = KL_DGRAM_CLOSE_NONE;
}

static KlDgramCoreConfig base_cfg(KlAllocator *a, int completion, size_t slots, size_t cap) {
    KlDgramCoreConfig c; memset(&c, 0, sizeof(c));
    c.alloc = a; c.fd = (KlSocketHandle)3; c.completion = completion;
    c.send_slots = slots; c.send_slot_cap = cap; c.recv_cap = 64; c.caps = KL_DGRAM_CAP_CONNECTED;
    c.submit = test_submit; c.arm = test_arm; c.deliver = test_deliver;
    c.retire = test_retire; c.on_close = test_on_close;
    return c;
}

static KlDatagramMessage msg_of(const void *data, size_t len) {
    KlDatagramMessage m; memset(&m, 0, sizeof(m));
    m.data = data; m.len = len; m.peer = NULL; m.tos = -1;   /* connected-mode send (cap present) */
    return m;
}

/* ── init / failure unwind ────────────────────────────────────────────────────────────────── */

UTEST(dgram_core, init_and_free) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, 4, 32);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    ASSERT_EQ((int)kl_dgram_core_close_state(&core), (int)KL_DGRAM_CLOSE_OPEN);
    ASSERT_EQ((int)kl_dgram_core_close_result(&core), (int)KL_DGRAM_CLOSE_NONE);
    ASSERT_TRUE(kl_dgram_core_inbound_slot(&core) != NULL);

    ASSERT_EQ(kl_dgram_core_free(&core), -1);        /* refused before CLOSED */
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);  /* nothing in flight → detach immediately */
    ASSERT_EQ((int)kl_dgram_core_close_state(&core), (int)KL_DGRAM_CLOSE_CLOSED);
    ASSERT_EQ(g_on_close_calls, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

UTEST(dgram_core, init_rejects_bad_args) {
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, 1, 4, 32);
    KlDgramCoreConfig bad;

    bad = cfg; bad.alloc = NULL;         ASSERT_EQ(kl_dgram_core_init(&core, &bad), -1);
    bad = cfg; bad.send_slots = 0;       ASSERT_EQ(kl_dgram_core_init(&core, &bad), -1);
    bad = cfg; bad.recv_cap = 0;         ASSERT_EQ(kl_dgram_core_init(&core, &bad), -1);
    bad = cfg; bad.submit = NULL;        ASSERT_EQ(kl_dgram_core_init(&core, &bad), -1);
    bad = cfg; bad.arm = NULL;           ASSERT_EQ(kl_dgram_core_init(&core, &bad), -1);
    bad = cfg; bad.deliver = NULL;       ASSERT_EQ(kl_dgram_core_init(&core, &bad), -1);
    ASSERT_EQ((int)core.inited, 0);      /* left zeroed/reusable on every rejection */
}

/* An allocator that fails after N successful allocations — drives the init unwind paths. */
static int g_alloc_budget;
static void *budget_malloc(void *c, size_t n) { (void)c; if (g_alloc_budget-- <= 0) return NULL; return malloc(n); }
static void *budget_realloc(void *c, void *p, size_t o, size_t n) { (void)c;(void)o; return realloc(p, n); }
static void  budget_free(void *c, void *p, size_t s) { (void)c;(void)s; free(p); }

UTEST(dgram_core, init_alloc_failure_unwinds) {
    KlAllocator a = { budget_malloc, budget_realloc, budget_free, NULL };
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, 1, 4, 32);
    /* Fail each of the internal allocations in turn (rx holder, inbound, life, outbound pool, ring);
     * every path must unwind to a zeroed, reusable object with nothing leaked (ASan/LSan). */
    for (int budget = 0; budget < 6; budget++) {
        g_alloc_budget = budget;
        int rc = kl_dgram_core_init(&core, &cfg);
        if (rc == 0) { ASSERT_EQ(kl_dgram_core_close_begin(&core), 0); ASSERT_EQ(kl_dgram_core_free(&core), 0); break; }
        ASSERT_EQ(rc, -1);
        ASSERT_EQ((int)core.inited, 0);
    }
    /* a generous budget finally succeeds */
    g_alloc_budget = 100; reset_globals();
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* ── fixed-slot send: FIFO, count backpressure, single-flight, edges ──────────────────────── */

static int g_writable_calls, g_drain_calls;
static void on_writable(void *ctx) { (void)ctx; g_writable_calls++; }
static void on_drain(void *ctx)    { (void)ctx; g_drain_calls++; }

UTEST(dgram_core, send_count_backpressure_and_fifo) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, /*slots*/2, /*cap*/16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    kl_dgram_core_on_writable(&core, on_writable, NULL);

    /* First accepted → submitted INFLIGHT (single-flight). Second queued behind it. Third → count-based
     * WOULD_BLOCK at 2 slots (NOT a byte budget). */
    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "A", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "B", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "C", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_WOULD_BLOCK);
    ASSERT_EQ(g_submit_calls, 1);                    /* single-flight: only A submitted */
    ASSERT_EQ(g_submitted[0], 'A');

    /* Retire A → B pumps (FIFO order). */
    ASSERT_EQ(kl_dgram_core_send_on_complete(&core, 1), 0);
    ASSERT_EQ(g_submit_calls, 2);
    ASSERT_EQ(g_submitted[0], 'B');
    ASSERT_EQ(g_writable_calls, 1);                  /* full(2)→non-full edge fired once */

    ASSERT_EQ(kl_dgram_core_send_on_complete(&core, 1), 0);   /* B retires, queue empty */
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

UTEST(dgram_core, send_drain_edge) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, 1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    kl_dgram_core_on_drain(&core, on_drain, NULL);

    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "x", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(g_drain_calls, 0);
    ASSERT_EQ(kl_dgram_core_send_on_complete(&core, 1), 0);   /* non-empty → empty */
    ASSERT_EQ(g_drain_calls, 1);
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* ── copy-before-accept provider contract ─────────────────────────────────────────────────── */

/* Facade rule: kl_dgram_core_send copies caller → slot, so the submit adapter never sees a later
 * mutation of the caller buffer. */
UTEST(dgram_core, copy_before_accept_facade) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, 1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);

    char buf[4] = { 'W', 'X', 'Y', 'Z' };
    KlDatagramMessage m = msg_of(buf, 4);
    ASSERT_EQ((int)kl_dgram_core_send(&core, &m), (int)KL_DATAGRAM_ACCEPTED);
    memset(buf, 0, sizeof(buf));                     /* mutate caller buffer after ACCEPTED */
    ASSERT_EQ(g_submitted_len, (size_t)4);
    ASSERT_EQ(memcmp(g_submitted, "WXYZ", 4), 0);    /* submit saw the ORIGINAL (send copied caller→slot) */

    ASSERT_EQ(kl_dgram_core_send_on_complete(&core, 1), 0);
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* Provider contract: a QUARANTINED send still releases the OBJECT-owned outbound pool at free — safe
 * ONLY because the backend copied the payload at submit (it holds no pointer into the pool). If it had
 * kept g_retained_ptr and dereffed it after free, ASan on the freed pool would fire. */
UTEST(dgram_core, quarantined_send_releases_outbound_pool) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);

    char payload[5] = { 'H', 'E', 'L', 'L', 'O' };
    KlDatagramMessage m = msg_of(payload, 5);
    ASSERT_EQ((int)kl_dgram_core_send(&core, &m), (int)KL_DATAGRAM_ACCEPTED);   /* INFLIGHT */
    const void *slot_ptr = g_retained_ptr;
    ASSERT_TRUE(slot_ptr != NULL);
    ASSERT_EQ(memcmp(g_submitted, "HELLO", 5), 0);   /* copied at submit, before accept */

    /* The backend cannot confirm the cancel → QUARANTINED. The op still reaches its MACHINE terminal
     * (it drained — inflight → 0, so the close can proceed); the QUARANTINE verdict is the classifier's,
     * orthogonal to the send's ok. */
    g_retire_send = KL_DGRAM_RETIRE_QUARANTINED;
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_send_on_complete(&core, 1), 0);   /* op terminal (machine drained) */
    ASSERT_EQ(g_on_close_calls, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_QUARANTINED);

    /* free RELEASES the object-owned outbound pool (where slot_ptr pointed). The backend performs no
     * later access through slot_ptr — proven safe by ASan on the now-freed pool. */
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
    ASSERT_EQ(memcmp(g_submitted, "HELLO", 5), 0);   /* the backend's own copy is intact */
    (void)slot_ptr;
}

UTEST(dgram_core, close_detached_when_all_retired) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, 1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "x", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_ACCEPTED);
    g_retire_send = KL_DGRAM_RETIRE_RETIRED;
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_send_on_complete(&core, 1), 0);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

UTEST_MAIN();
