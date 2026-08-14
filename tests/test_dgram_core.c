/*
 * test_dgram_core.c — Phase B step 7A-3: the fixed-slot KlDgramCore assembly.
 *
 * Drives the integrated core over a SCRIPTED neutral adapter (no real socket/provider): init +
 * failure unwind; fixed-slot send FIFO / count-based backpressure / single-flight / writable+drain
 * edges; the backend-retirement + EXACTLY-ONCE fd close (incl. an idle posted-recv graceful close that
 * must NOT wedge); the copy-before-accept PROVIDER CONTRACT for payload AND address/TOS metadata; and
 * a QUARANTINED recv that pins its life-owned inbound storage (proved via counting-allocator
 * bookkeeping — on_final must NOT run while a posted-op life ref is outstanding).
 */
#include "utest.h"

#include "../src/datagram_core.h"

#include <keel/sockaddr.h>
#include <string.h>

/* ── scripted neutral adapter ─────────────────────────────────────────────────────────────── */

static KlDgramCore *g_core;   /* the core under test — the adapters reach it to pin/retire ops */
static KlDgramLife *g_send_life;  /* the outstanding posted-send op's token (captured at submit) */

/* submit: copy-before-accept of payload AND metadata into backend-owned storage, and — for an INFLIGHT
 * (async-posted) send — RETAIN a stable-token ref, uniform with the recv arm (B.6). */
static KlDgramSubmitResult g_submit_result;
static int        g_submit_calls;
static unsigned char g_submitted[64];         /* backend's COPY of the last payload */
static size_t     g_submitted_len;
static const void *g_retained_ptr;            /* the exact pool pointer submit was handed (never re-deref) */
static int        g_submit_has_peer, g_submit_has_local, g_submit_tos;
static KlSockAddr g_submit_peer, g_submit_local;   /* backend-owned COPIES of the addresses */

static KlDgramSubmitResult test_submit(void *ctx, const void *data, size_t len,
                                       const KlSockAddr *peer, const KlSockAddr *local, int tos) {
    (void)ctx;
    g_submit_calls++;
    g_retained_ptr  = data;                   /* recorded, but the contract forbids a later deref */
    g_submitted_len = len < sizeof(g_submitted) ? len : sizeof(g_submitted);
    memcpy(g_submitted, data, g_submitted_len);
    g_submit_has_peer  = peer  != NULL; if (peer)  g_submit_peer  = *peer;   /* copy addr metadata too */
    g_submit_has_local = local != NULL; if (local) g_submit_local = *local;
    g_submit_tos       = tos;
    if (g_submit_result == KL_DGRAM_SUBMIT_INFLIGHT) {   /* posted async op → holds one token ref */
        g_send_life = kl_dgram_core_life(g_core);
        kl_dgram_life_retain(g_send_life);
    }
    return g_submit_result;
}

/* Deliver a posted send's terminal completion the way a backend does: retire the machine op AND
 * release that op's token ref. (Capture the token BEFORE completion — send_on_complete may pump the
 * next queued send, which retains a fresh ref on the same token.) */
static void send_done(KlDgramCore *core, int ok) {
    KlDgramLife *l = g_send_life;
    kl_dgram_core_send_on_complete(core, ok);
    kl_dgram_life_release(l);
}

/* per-op retirement classifier (§4.3) */
static KlDgramRetireResult g_retire_send, g_retire_recv;
static KlDgramRetireResult test_retire(void *ctx, KlDgramOpKind kind, int *te) {
    (void)ctx; *te = 0;
    return kind == KL_DGRAM_OP_SEND ? g_retire_send : g_retire_recv;
}

/* recv arm. Completion: posting a recv PINS the op by retaining a core life ref (the B.6 contract) —
 * readiness interest is NOT a posted op, so it retains nothing. Readiness also tracks armed interest. */
static int         g_completion;        /* the core's mode (set by base_cfg) */
static int         g_arm_calls, g_disarm_calls, g_pull_calls, g_interest;
static KlDgramLife *g_recv_life;
static int         g_recv_quarantine;   /* 1 = cancel drops the op but does NOT release its life ref */
static int         g_avail;             /* readiness: datagrams available to pull before would-block */
static const char *g_rx_payload; static size_t g_rx_len; static int g_rx_no_peer;

/* Fill the inbound slot as a provider would, AFTER the machine reset it: peer MANDATORY. */
static void inbound_fill(KlDgramCore *core, const char *payload, size_t len) {
    KlDgramSlot *in = kl_dgram_core_inbound_slot(core);
    kl_sockaddr_from_ipv4(&in->peer, (const unsigned char *)"\x7f\0\0\1", 12345);
    if (g_rx_no_peer) memset(&in->peer, 0, sizeof(in->peer));   /* provider bug → fail-safe */
    if (len && payload) { size_t n = len < in->cap ? len : in->cap; memcpy(in->data, payload, n); }
    in->flags = 0;
}

static int test_arm(void *ctx) {
    (void)ctx; g_arm_calls++;
    if (g_completion) {
        g_recv_life = kl_dgram_core_life(g_core);
        kl_dgram_life_retain(g_recv_life);   /* posted op ref — released at terminal completion */
    } else {
        g_interest = 1;                       /* readiness: READ interest armed */
    }
    return 0;
}
static void test_disarm(void *ctx) { (void)ctx; g_disarm_calls++; g_interest = 0; }   /* readiness drops interest */
static int  test_pull(void *ctx, size_t *out) {
    (void)ctx; g_pull_calls++;
    if (g_avail <= 0) return 0;               /* would-block (drained) */
    g_avail--;
    inbound_fill(g_core, g_rx_payload, g_rx_len);
    *out = g_rx_len;
    return 1;
}
/* cancel_recv: the backend delivers the outstanding recv's terminal completion (cancel-drop). A clean
 * retirement releases the op's life ref; a QUARANTINED op abandons it (storage stays pinned). */
static int g_cancel_recv_calls;
static void test_cancel_recv(void *ctx) {
    (void)ctx; g_cancel_recv_calls++;
    kl_dgram_core_recv_on_complete(g_core, 0, 0);   /* drop machine inflight (stopped-drop) */
    if (!g_recv_quarantine) kl_dgram_life_release(g_recv_life);
}

/* delivery recorder (+ optional pause/stop from within the callback — invariant-9 confinement). */
static int          g_deliv;
static unsigned char g_last[128]; static size_t g_last_len;
static int          g_has_peer, g_has_local; static unsigned g_last_flags;
enum { ACT_NONE = 0, ACT_PAUSE, ACT_STOP };
static int          g_action, g_action_at;
static void test_deliver(void *ctx, const void *d, size_t n, const KlSockAddr *p,
                         const KlSockAddr *l, unsigned f) {
    (void)ctx;
    g_deliv++;
    if (n <= sizeof(g_last)) { memcpy(g_last, d, n); g_last_len = n; }
    g_has_peer = (p != NULL); g_has_local = (l != NULL); g_last_flags = f;
    if (g_deliv == g_action_at) {
        if (g_action == ACT_PAUSE)     kl_dgram_core_pause(g_core);
        else if (g_action == ACT_STOP) kl_dgram_core_recv_stop(g_core);
    }
}

/* A datagram arrives on a posted COMPLETION recv: fill the inbound slot, then signal completion and
 * release THIS op's token ref (capture it first — a delivery re-arms, retaining a fresh ref). */
static void recv_arrive(KlDgramCore *core, const char *payload, size_t len) {
    KlDgramLife *completing = g_recv_life;
    inbound_fill(core, payload, len);
    kl_dgram_core_recv_on_complete(core, len, 1);   /* deliver (or hold, if paused) + maybe re-arm */
    kl_dgram_life_release(completing);              /* the completed op's ref */
}

/* close_transport: the physical fd close — must run EXACTLY ONCE with the adopted fd. */
static int            g_fd_close_calls;
static KlSocketHandle g_fd_closed;
static void test_close_transport(void *ctx, KlSocketHandle fd) { (void)ctx; g_fd_close_calls++; g_fd_closed = fd; }

static int g_on_close_calls;
static KlDatagramCloseResult g_on_close_result;
static void test_on_close(void *ctx, KlDatagramCloseResult r) { (void)ctx; g_on_close_calls++; g_on_close_result = r; }

static void reset_globals(void) {
    g_core = NULL; g_send_life = NULL;
    g_submit_result = KL_DGRAM_SUBMIT_INFLIGHT; g_submit_calls = 0; g_submitted_len = 0;
    memset(g_submitted, 0, sizeof(g_submitted)); g_retained_ptr = NULL;
    g_submit_has_peer = g_submit_has_local = 0; g_submit_tos = -2;
    memset(&g_submit_peer, 0, sizeof(g_submit_peer)); memset(&g_submit_local, 0, sizeof(g_submit_local));
    g_retire_send = g_retire_recv = KL_DGRAM_RETIRE_RETIRED;
    g_completion = 1;
    g_arm_calls = g_disarm_calls = g_pull_calls = 0; g_interest = 0;
    g_recv_life = NULL; g_recv_quarantine = 0; g_cancel_recv_calls = 0;
    g_avail = 0; g_rx_payload = NULL; g_rx_len = 0; g_rx_no_peer = 0;
    g_deliv = 0; g_last_len = 0; g_has_peer = g_has_local = 0; g_last_flags = 0;
    g_action = ACT_NONE; g_action_at = 0;
    g_fd_close_calls = 0; g_fd_closed = KL_INVALID_SOCKET;
    g_on_close_calls = 0; g_on_close_result = KL_DGRAM_CLOSE_NONE;
}

/* Counting allocator — tracks live allocations so a quarantine test can prove that on_final (which
 * frees the life-owned inbound + rx holder) did NOT run while a posted-op token ref is outstanding. */
static int g_live;
static void *count_malloc(void *c, size_t n) { (void)c; void *p = malloc(n); if (p) g_live++; return p; }
static void *count_realloc(void *c, void *p, size_t o, size_t n) { (void)c;(void)o; void *q = realloc(p, n); if (!p && q) g_live++; return q; }
static void  count_free(void *c, void *p, size_t s) { (void)c;(void)s; if (p) { g_live--; free(p); } }

#define TEST_FD ((KlSocketHandle)3)

static KlDgramCoreConfig base_cfg(KlAllocator *a, int completion, size_t slots, size_t cap) {
    g_completion = completion ? 1 : 0;
    KlDgramCoreConfig c; memset(&c, 0, sizeof(c));
    c.alloc = a; c.fd = TEST_FD; c.completion = completion;
    c.send_slots = slots; c.send_slot_cap = cap; c.recv_cap = 64;
    c.caps = KL_DGRAM_CAP_CONNECTED | KL_DGRAM_CAP_SOURCE_PIN | KL_DGRAM_CAP_TOS;
    c.submit = test_submit; c.arm = test_arm; c.disarm = test_disarm; c.pull = test_pull;
    c.deliver = test_deliver; c.cancel_recv = test_cancel_recv;
    c.retire = test_retire; c.close_transport = test_close_transport; c.on_close = test_on_close;
    return c;
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
    ASSERT_TRUE(kl_dgram_core_life(&core) != NULL);

    ASSERT_EQ(kl_dgram_core_free(&core), -1);        /* refused before CLOSED */
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);  /* nothing in flight → detach immediately */
    ASSERT_EQ((int)kl_dgram_core_close_state(&core), (int)KL_DGRAM_CLOSE_CLOSED);
    ASSERT_EQ(g_on_close_calls, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(g_fd_close_calls, 1);                  /* fd closed exactly once, even with no ops */
    ASSERT_TRUE(g_fd_closed == TEST_FD);
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
    bad = cfg; bad.close_transport = NULL; ASSERT_EQ(kl_dgram_core_init(&core, &bad), -1);  /* fd owner needs a closer */
    ASSERT_EQ((int)core.inited, 0);      /* left zeroed/reusable on every rejection */
}

/* An allocator that fails after N successful allocations — drives the init unwind paths. */
static int g_alloc_budget;
static void *budget_malloc(void *c, size_t n) { (void)c; if (g_alloc_budget-- <= 0) return NULL; return malloc(n); }
static void *budget_realloc(void *c, void *p, size_t o, size_t n) { (void)c;(void)o; return realloc(p, n); }
static void  budget_free(void *c, void *p, size_t s) { (void)c;(void)s; free(p); }

UTEST(dgram_core, init_alloc_failure_unwinds) {
    reset_globals();
    KlAllocator a = { budget_malloc, budget_realloc, budget_free, NULL };
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, 1, 4, 32);
    /* Fail each internal allocation in turn (rx holder, inbound, life, outbound pool, ring); every
     * path must unwind to a zeroed, reusable object with nothing leaked (ASan/LSan). */
    for (int budget = 0; budget < 6; budget++) {
        g_alloc_budget = budget;
        int rc = kl_dgram_core_init(&core, &cfg);
        if (rc == 0) { ASSERT_EQ(kl_dgram_core_close_begin(&core), 0); ASSERT_EQ(kl_dgram_core_free(&core), 0); break; }
        ASSERT_EQ(rc, -1);
        ASSERT_EQ((int)core.inited, 0);
    }
    g_alloc_budget = 100;
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
    g_core = &core;
    g_writable_calls = 0;
    kl_dgram_core_on_writable(&core, on_writable, NULL);

    /* First accepted → submitted INFLIGHT (single-flight). Second queued. Third → count-based
     * WOULD_BLOCK at 2 slots (NOT a byte budget). */
    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "A", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "B", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "C", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_WOULD_BLOCK);
    ASSERT_EQ(g_submit_calls, 1);                    /* single-flight: only A submitted */
    ASSERT_EQ(g_submitted[0], 'A');

    send_done(&core, 1);                             /* A retires → B pumps (FIFO) */
    ASSERT_EQ(g_submit_calls, 2);
    ASSERT_EQ(g_submitted[0], 'B');
    ASSERT_EQ(g_writable_calls, 1);                  /* full(2)→non-full edge fired once */

    send_done(&core, 1);                             /* B retires, queue empty */
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

UTEST(dgram_core, send_drain_edge) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, 1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;
    g_drain_calls = 0;
    kl_dgram_core_on_drain(&core, on_drain, NULL);

    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "x", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_ACCEPTED);
    ASSERT_EQ(g_drain_calls, 0);
    send_done(&core, 1);                             /* non-empty → empty */
    ASSERT_EQ(g_drain_calls, 1);
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* ── copy-before-accept provider contract ─────────────────────────────────────────────────── */

/* Facade rule: kl_dgram_core_send copies caller → slot, so submit never sees a later mutation. */
UTEST(dgram_core, copy_before_accept_facade) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, 1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;

    char buf[4] = { 'W', 'X', 'Y', 'Z' };
    KlDatagramMessage m = { .data = buf, .len = 4, .tos = -1 };
    ASSERT_EQ((int)kl_dgram_core_send(&core, &m), (int)KL_DATAGRAM_ACCEPTED);
    memset(buf, 0, sizeof(buf));                     /* mutate caller buffer after ACCEPTED */
    ASSERT_EQ(g_submitted_len, (size_t)4);
    ASSERT_EQ(memcmp(g_submitted, "WXYZ", 4), 0);    /* submit saw the ORIGINAL */

    send_done(&core, 1);
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* Provider contract, storage ASYMMETRY. A QUARANTINED send exercises BOTH halves of the split:
 *  - copy-before-accept (payload AND address/TOS metadata) makes the OBJECT-owned outbound pool safe to
 *    release immediately at core_free — no late access through the pool pointer (ASan on the freed pool);
 *  - yet the send op still holds a B.6 token ref (uniform contract), so abandoning it on quarantine
 *    PINS the life-owned rx storage exactly as a quarantined recv would — on_final defers until the
 *    abandoned ref is reclaimed. */
UTEST(dgram_core, quarantined_send_releases_pool_but_pins_rx) {
    reset_globals();
    KlAllocator a = { count_malloc, count_realloc, count_free, NULL };
    g_live = 0;
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;
    ASSERT_TRUE(g_live > 0);

    KlSockAddr peer, local;
    kl_sockaddr_from_ipv4(&peer,  (const unsigned char *)"\xC0\xA8\x01\x2A", 4242);   /* 192.168.1.42:4242 */
    kl_sockaddr_from_ipv4(&local, (const unsigned char *)"\x0A\x00\x00\x07", 5353);   /* 10.0.0.7:5353 */
    char payload[5] = { 'H', 'E', 'L', 'L', 'O' };
    KlDatagramMessage m = { .data = payload, .len = 5, .peer = &peer, .local = &local, .tos = 0x28 };
    ASSERT_EQ((int)kl_dgram_core_send(&core, &m), (int)KL_DATAGRAM_ACCEPTED);   /* INFLIGHT → submit retains */
    const void *slot_ptr = g_retained_ptr;
    ASSERT_TRUE(slot_ptr != NULL);
    ASSERT_EQ(memcmp(g_submitted, "HELLO", 5), 0);   /* payload copied at submit */
    ASSERT_TRUE(g_submit_has_peer && g_submit_has_local);
    ASSERT_EQ(g_submit_tos, 0x28);
    KlSockAddr saved_peer = g_submit_peer, saved_local = g_submit_local;   /* snapshot the copies */

    /* Backend cannot confirm the cancel → QUARANTINED; the op reaches its MACHINE terminal so the close
     * can proceed, but its token ref is ABANDONED (raw on_complete, NOT send_done → no release). */
    g_retire_send = KL_DGRAM_RETIRE_QUARANTINED;
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ(kl_dgram_core_send_on_complete(&core, 1), 0);
    ASSERT_EQ(g_on_close_calls, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_QUARANTINED);
    ASSERT_EQ(g_fd_close_calls, 1);

    /* free RELEASES the object-owned outbound pool (slot_ptr's backing) immediately — ASan on the freed
     * pool is the check, and the backend's own payload+metadata copies stay intact... */
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
    ASSERT_EQ(memcmp(g_submitted, "HELLO", 5), 0);
    ASSERT_EQ(memcmp(&saved_peer,  &g_submit_peer,  sizeof(KlSockAddr)), 0);
    ASSERT_EQ(memcmp(&saved_local, &g_submit_local, sizeof(KlSockAddr)), 0);
    ASSERT_EQ(g_submit_tos, 0x28);
    /* ...but the life-owned rx storage stays PINNED by the abandoned send token ref. */
    ASSERT_TRUE(g_live > 0);
    kl_dgram_life_release(g_send_life);              /* reclaim the abandoned ref → on_final runs */
    ASSERT_EQ(g_live, 0);
    (void)slot_ptr;
}

/* ── backend retirement + fd close ────────────────────────────────────────────────────────── */

/* An idle posted completion recv (no datagram ever arrives) must NOT wedge a graceful close: the
 * backend-retirement step cancels + retires it and closes the fd exactly once → DETACHED. */
UTEST(dgram_core, graceful_close_idle_posted_recv) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;

    ASSERT_EQ(kl_dgram_core_recv_start(&core), 0);   /* posts a completion recv (inflight) */
    ASSERT_EQ(g_arm_calls, 1);

    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);  /* graceful, no datagram arriving */
    ASSERT_EQ(g_cancel_recv_calls, 1);               /* backend-retirement cancelled the idle recv */
    ASSERT_EQ((int)kl_dgram_core_close_state(&core), (int)KL_DGRAM_CLOSE_CLOSED);   /* did not wedge */
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(g_fd_close_calls, 1);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* A QUARANTINED recv keeps its life ref (the backend could not confirm retirement) → the inbound
 * storage stays PINNED after on_close(QUARANTINED) and core_free; on_final runs only once the abandoned
 * op ref is finally reclaimed. */
UTEST(dgram_core, quarantined_recv_pins_inbound_storage) {
    reset_globals();
    KlAllocator a = { count_malloc, count_realloc, count_free, NULL };
    g_live = 0;
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;
    int live_after_init = g_live;
    ASSERT_TRUE(live_after_init > 0);

    ASSERT_EQ(kl_dgram_core_recv_start(&core), 0);   /* posts recv; arm retains a life ref */
    g_recv_quarantine = 1;                            /* cancel drops the op but abandons its life ref */
    g_retire_recv = KL_DGRAM_RETIRE_QUARANTINED;

    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_QUARANTINED);
    ASSERT_EQ(g_fd_close_calls, 1);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);          /* object-owned parts freed... */
    ASSERT_TRUE(g_live > 0);                           /* ...but the life-owned inbound is STILL pinned */

    /* Reclaim the intentionally-abandoned op ref (test-only): on_final now runs, freeing inbound + rx +
     * the token. No production ref is touched and detachment already fired exactly once. */
    kl_dgram_life_release(g_recv_life);
    ASSERT_EQ(g_live, 0);                              /* fully reclaimed — LSan-clean */
}

UTEST(dgram_core, close_detached_when_all_retired) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, 1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;
    ASSERT_EQ((int)kl_dgram_core_send(&core, &(KlDatagramMessage){ .data = "x", .len = 1, .tos = -1 }), (int)KL_DATAGRAM_ACCEPTED);
    g_retire_send = KL_DGRAM_RETIRE_RETIRED;
    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    send_done(&core, 1);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(g_fd_close_calls, 1);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* ── strict pause / receive control (7A-4) ────────────────────────────────────────────────── */

/* A completion datagram delivers once (peer present) and the machine re-arms through the core. */
UTEST(dgram_core, recv_completion_deliver_and_rearm) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;

    ASSERT_EQ(kl_dgram_core_recv_start(&core), 0);
    ASSERT_EQ(g_arm_calls, 1);
    recv_arrive(&core, "hi", 2);
    ASSERT_EQ(g_deliv, 1);                 /* delivered exactly once */
    ASSERT_TRUE(g_has_peer);               /* peer mandatory + present */
    ASSERT_EQ(g_last_len, (size_t)2);
    ASSERT_EQ(g_arm_calls, 2);             /* re-armed for the next datagram */

    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* STRICT pause, completion: a recv posted before pause completes while paused → the datagram is HELD
 * (not delivered); resume delivers it exactly once, then re-arms. */
UTEST(dgram_core, recv_completion_pause_holds_one) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;

    ASSERT_EQ(kl_dgram_core_recv_start(&core), 0);
    kl_dgram_core_pause(&core);
    recv_arrive(&core, "held", 4);         /* completes while paused → held, not delivered */
    ASSERT_EQ(g_deliv, 0);
    ASSERT_EQ(kl_dgram_core_recv_held(&core), 1);
    ASSERT_EQ(g_arm_calls, 1);             /* no re-arm while paused */

    ASSERT_EQ(kl_dgram_core_resume(&core), 0);
    ASSERT_EQ(g_deliv, 1);                 /* the held datagram delivered exactly once */
    ASSERT_EQ(g_last_len, (size_t)4);
    ASSERT_EQ(kl_dgram_core_recv_held(&core), 0);
    ASSERT_EQ(g_arm_calls, 2);             /* re-armed after resume */

    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* STRICT pause, readiness: pause DROPS interest at the backend seam (disarm); resume re-arms it. */
UTEST(dgram_core, recv_readiness_pause_drops_interest) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/0, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;

    ASSERT_EQ(kl_dgram_core_recv_start(&core), 0);
    ASSERT_EQ(g_arm_calls, 1);
    ASSERT_EQ(g_interest, 1);              /* READ interest armed */

    g_rx_payload = "rd"; g_rx_len = 2; g_avail = 1;
    ASSERT_EQ(kl_dgram_core_recv_on_readable(&core), 0);
    ASSERT_EQ(g_deliv, 1);                 /* pulled + delivered one, then would-block */
    ASSERT_TRUE(g_pull_calls >= 1);
    ASSERT_EQ(g_interest, 1);              /* interest persists across a drain */

    kl_dgram_core_pause(&core);
    ASSERT_EQ(g_disarm_calls, 1);          /* pause dropped interest */
    ASSERT_EQ(g_interest, 0);
    ASSERT_EQ(kl_dgram_core_resume(&core), 0);
    ASSERT_EQ(g_interest, 1);              /* resume re-armed interest */

    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* Invariant 9 confinement: a delivery callback may pause — after which the machine does NOT re-arm. */
UTEST(dgram_core, recv_pause_from_callback) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;
    g_action = ACT_PAUSE; g_action_at = 1;

    ASSERT_EQ(kl_dgram_core_recv_start(&core), 0);
    recv_arrive(&core, "x", 1);
    ASSERT_EQ(g_deliv, 1);
    ASSERT_EQ(g_arm_calls, 1);             /* callback paused → NO re-arm */

    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

/* recv_stop discards a held (paused) datagram. */
UTEST(dgram_core, recv_stop_discards_held) {
    reset_globals();
    KlAllocator a = kl_allocator_default();
    KlDgramCore core;
    KlDgramCoreConfig cfg = base_cfg(&a, /*completion*/1, 4, 16);
    ASSERT_EQ(kl_dgram_core_init(&core, &cfg), 0);
    g_core = &core;

    ASSERT_EQ(kl_dgram_core_recv_start(&core), 0);
    kl_dgram_core_pause(&core);
    recv_arrive(&core, "held", 4);
    ASSERT_EQ(kl_dgram_core_recv_held(&core), 1);

    kl_dgram_core_recv_stop(&core);
    ASSERT_EQ(kl_dgram_core_recv_held(&core), 0);   /* held datagram discarded */
    ASSERT_EQ(g_deliv, 0);

    ASSERT_EQ(kl_dgram_core_close_begin(&core), 0);
    ASSERT_EQ((int)g_on_close_result, (int)KL_DGRAM_DETACHED);
    ASSERT_EQ(kl_dgram_core_free(&core), 0);
}

UTEST_MAIN();
