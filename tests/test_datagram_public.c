/*
 * test_datagram_public.c — the PUBLIC KlDatagram surface over a scripted completion mock (7B-3).
 *
 * No live backend: a hand-built KlEventCtx whose loop advertises KL_EVENT_CAP_COMPLETION and whose
 * completion vtable is a scripted double records the posted send/recv ops (by their KlDgramLife token)
 * and lets the test drive completions exactly as the real driver would — life->dispatch(target, ev).
 * This proves the public surface + ABI + ownership (init/reuse/free-refusal, fixed-slot send geometry,
 * copy-before-accept, recv delivery, strict pause/resume, confirmed-detachment close + terminal result)
 * with ZERO live-backend risk. §10 live rows are wired in 7B-4+.
 */

/* Match the POSIX provider's feature-test preamble (socket_dgram_posix.c) BEFORE any system header, so
 * the m2_posix_provider_caps_per_family test sees the SAME IP_PKTINFO/IPV6_* macro visibility the
 * provider compiled against, and can build the expected capability mask under the identical guards. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

#include "../vendor/utest.h"

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>
#include <keel/error.h>

#include "../src/completion.h"      /* KlCompletionEvent + KL_COMP_DGRAM_* */
#include "../src/datagram_life.h"   /* kl_dgram_life_dispatch/_target — drive completions like the driver */
#include "../src/socket.h"          /* KlSocketProvider / KlSocketOps — the close-ordering mock provider */
#include "../src/datagram_open.h"   /* kl_datagram_teardown — synchronous owner-destruction (Option A) */
#include <unistd.h>                 /* close() */

#include <string.h>
#include <sys/socket.h>

/* ── scripted completion double ───────────────────────────────────────────────────────────────── */
typedef struct {
    /* last posted send op */
    struct KlDgramLife *send_life;
    unsigned char       send_copy[2048];   /* COPY the payload at submit (copy-before-accept contract) */
    size_t              send_len;
    KlSockAddr          send_dest;
    int                 send_posted;        /* count of send posts */
    int                 send_fail;          /* 1 → next post_dgram_send returns -1 (took nothing) */
    /* last posted recv op */
    struct KlDgramLife *recv_life;
    void               *recv_buf;
    size_t              recv_cap;
    int                 recv_posted;
    /* retire scripting */
    KlDgramRetireResult retire_result;
    int                 cancels;
    /* 7B-7 fd↔loop registration (kl_event_add/del) + lifecycle ordering (monotonic seq) */
    int                 add_calls, del_calls, add_fail;
    int                 seq, add_seq, first_post_seq, del_seq, close_seq;
} MockComp;
static MockComp g_mc;

static int mc_post_send(struct KlEventCtx *ctx, const KlDgramSendOp *op) {
    (void)ctx;
    if (g_mc.send_fail) return -1;   /* transfer-only-on-success: caller releases its ref */
    if (!g_mc.first_post_seq) g_mc.first_post_seq = ++g_mc.seq;
    g_mc.send_life = op->life;       /* the transferred ref rides the op */
    g_mc.send_len  = op->len < sizeof(g_mc.send_copy) ? op->len : sizeof(g_mc.send_copy);
    memcpy(g_mc.send_copy, op->data, g_mc.send_len);   /* copy-before-accept (provider contract) */
    if (op->dest) g_mc.send_dest = *op->dest;
    g_mc.send_posted++;
    return 0;
}
static int mc_post_recv(struct KlEventCtx *ctx, const KlDgramRecvOp *op) {
    (void)ctx;
    if (!g_mc.first_post_seq) g_mc.first_post_seq = ++g_mc.seq;
    g_mc.recv_life = op->life; g_mc.recv_buf = op->buf; g_mc.recv_cap = op->cap;
    g_mc.recv_posted++;
    return 0;
}
static int mc_cancel(struct KlEventCtx *ctx, struct KlDgramLife *life, KlDgramOpKind kind) {
    (void)ctx; (void)life; (void)kind; g_mc.cancels++; return 0;
}
static KlDgramRetireResult mc_retire(struct KlEventCtx *ctx, struct KlDgramLife *life,
                                     KlDgramOpKind kind, int *terr) {
    (void)ctx; (void)life; (void)kind; if (terr) *terr = 0; return g_mc.retire_result;
}
static const KlCompletionOps MC_COMP = {
    .post_dgram_send = mc_post_send, .post_dgram_recv = mc_post_recv,
    .cancel_dgram = mc_cancel, .retire_dgram = mc_retire,
};
static unsigned mc_caps(const KlEventLoop *loop) { (void)loop; return KL_EVENT_CAP_COMPLETION; }
/* 7B-7: the facade registers/deregisters the fd with the loop via the generic kl_event_add/del — the
 * mock records them + can force a registration failure. `add` records BEFORE any post; `del` before close. */
static int mc_add(KlEventLoop *loop, KlSocketHandle fd, KlEventMask mask, void *udata) {
    (void)loop; (void)fd; (void)mask; (void)udata;
    if (g_mc.add_fail) return -1;
    g_mc.add_calls++; g_mc.add_seq = ++g_mc.seq; return 0;
}
static int mc_del(KlEventLoop *loop, KlSocketHandle fd) {
    (void)loop; (void)fd; g_mc.del_calls++; g_mc.del_seq = ++g_mc.seq; return 0;
}
static const KlEventOps MC_EVOPS = { .caps = mc_caps, .completion = &MC_COMP, .add = mc_add, .del = mc_del };

/* A mock socket provider whose only job is to record the close ordering (del must precede close). It
 * actually closes the fd so no descriptor leaks. Completion mode never calls its dgram send/recv. */
static int mc_sock_close(void *pctx, KlSocketHandle fd) {
    (void)pctx; g_mc.close_seq = ++g_mc.seq; return close((int)fd);
}
static const KlSocketOps MC_SOCK_OPS = { .close = mc_sock_close };
static const KlSocketProvider MC_SP = { .ops = &MC_SOCK_OPS };

/* Drive the pending SEND / RECV completion exactly as the real driver: life->dispatch(target, ev). */
static void drive_send(int ok) {
    struct KlDgramLife *life = g_mc.send_life;
    KlCompletionEvent ev; memset(&ev, 0, sizeof(ev));
    ev.kind = KL_COMP_DGRAM_SEND; ev.ok = ok; ev.life = life;
    kl_dgram_life_dispatch(life)(kl_dgram_life_target(life), &ev);
}
static void drive_recv(const void *data, size_t len, const KlSockAddr *peer, int truncated) {
    struct KlDgramLife *life = g_mc.recv_life;
    if (g_mc.recv_buf && len) memcpy(g_mc.recv_buf, data, len <= g_mc.recv_cap ? len : g_mc.recv_cap);
    KlCompletionEvent ev; memset(&ev, 0, sizeof(ev));
    ev.kind = KL_COMP_DGRAM_RECV; ev.ok = 1; ev.bytes = len; ev.buf = g_mc.recv_buf;
    ev.truncated = truncated; ev.life = life;
    if (peer) ev.peer = *peer;
    kl_dgram_life_dispatch(life)(kl_dgram_life_target(life), &ev);
}
/* The cancelled (terminal) completion of an outstanding recv op — as the driver would drain it after a
 * cancel at close. Retires the recv machine + releases the op's life ref so the close coordinator joins. */
static void drive_recv_cancelled(void) {
    struct KlDgramLife *life = g_mc.recv_life;
    KlCompletionEvent ev; memset(&ev, 0, sizeof(ev));
    ev.kind = KL_COMP_DGRAM_RECV; ev.ok = 0; ev.life = life;
    kl_dgram_life_dispatch(life)(kl_dgram_life_target(life), &ev);
}

/* ── fixture ──────────────────────────────────────────────────────────────────────────────────── */
static KlEventCtx g_ctx;
static KlAllocator g_alloc;
static int mk_ctx(void) {   /* a completion-capable mock loop; sockets=NULL → kl_sockdef_close on teardown */
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_alloc = kl_allocator_default();
    g_ctx.loop.ops = &MC_EVOPS;
    g_ctx.alloc = &g_alloc;
    return 0;
}
static KlSocketHandle mk_fd(void) { return (KlSocketHandle)socket(AF_INET, SOCK_DGRAM, 0); }
static KlSockAddr addr4(int a, int b, int c, int d, int port) {
    uint8_t ip[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    KlSockAddr s; kl_sockaddr_from_ipv4(&s, ip, (uint16_t)port); return s;
}
static void mc_reset(void) { memset(&g_mc, 0, sizeof(g_mc)); g_mc.retire_result = KL_DGRAM_RETIRE_RETIRED; }

static KlDatagramConfig cfg_for(KlSocketHandle fd, size_t slots, size_t slot_cap) {
    KlDatagramConfig c; memset(&c, 0, sizeof(c));
    c.ctx = &g_ctx; c.alloc = &g_alloc; c.sockets = NULL; c.fd = fd;
    c.send_slots = slots; c.send_slot_cap = slot_cap; c.recv_cap = 2048; c.want_caps = 0;
    return c;
}

/* recv recorder */
static int   g_recv_calls; static unsigned char g_recv_buf[2048]; static size_t g_recv_len;
static unsigned g_recv_flags; static int g_recv_has_peer;
static void on_recv(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                    const KlSockAddr *local, unsigned flags) {
    (void)ud; (void)local;
    g_recv_calls++; g_recv_len = len; g_recv_flags = flags; g_recv_has_peer = (peer != NULL);
    if (len) memcpy(g_recv_buf, data, len <= sizeof(g_recv_buf) ? len : sizeof(g_recv_buf));
}
/* close recorder */
static int g_close_calls; static KlDatagramCloseResult g_close_result;
static void on_close(void *ud, KlDatagramCloseResult r) { (void)ud; g_close_calls++; g_close_result = r; }

UTEST(datagram_public, init_success_and_free_refusal) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlSocketHandle fd = mk_fd();
    KlDatagramConfig c = cfg_for(fd, 4, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ((int)KL_DGRAM_CLOSE_OPEN, (int)kl_datagram_close_state(&dg));
    ASSERT_EQ((int)KL_DGRAM_CLOSE_NONE, (int)kl_datagram_close_result(&dg));   /* NONE until CLOSED */
    ASSERT_EQ(-1, kl_datagram_free(&dg));   /* refused before CLOSED */
    /* graceful close with no outstanding ops → DETACHED, then free succeeds */
    kl_datagram_on_close(&dg, on_close, NULL); g_close_calls = 0;
    ASSERT_EQ(0, kl_datagram_close_begin(&dg));
    ASSERT_EQ((int)KL_DGRAM_CLOSE_CLOSED, (int)kl_datagram_close_state(&dg));
    ASSERT_EQ(1, g_close_calls);
    ASSERT_EQ((int)KL_DGRAM_DETACHED, (int)kl_datagram_close_result(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
    /* reuse: the fully-detached (memset-zero) object re-inits */
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
}

UTEST(datagram_public, init_failure_keeps_fd) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlSocketHandle fd = mk_fd();
    KlDatagramConfig c = cfg_for(fd, 0 /*bad*/, 1500);   /* send_slots 0 → core init fails */
    ASSERT_EQ(-1, kl_datagram_init(&dg, &c));
    ASSERT_NE((int)KL_ERR_NONE, (int)kl_datagram_last_error(&dg));   /* error set, fd NOT adopted */
    (void)close((int)fd);   /* caller still owns the fd */
}

UTEST(datagram_public, send_fixed_slot_geometry_and_fifo) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2 /*slots*/, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));

    KlSockAddr dA = addr4(10,0,0,1, 53), dB = addr4(10,0,0,2, 5353), dC = addr4(10,0,0,3, 1);
    KlDatagramMessage m1 = { .data = "AAAA", .len = 4, .peer = &dA, .tos = -1 };
    KlDatagramMessage m2 = { .data = "BB",   .len = 2, .peer = &dB, .tos = -1 };
    KlDatagramMessage m3 = { .data = "C",    .len = 1, .peer = &dC, .tos = -1 };

    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m1));  /* submits (single-flight) */
    ASSERT_EQ(1, g_mc.send_posted);
    ASSERT_EQ((size_t)1, kl_datagram_send_inflight(&dg));
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m2));  /* queued behind (2 slots) */
    ASSERT_EQ(1, g_mc.send_posted);                                        /* still single-flight */
    ASSERT_EQ((size_t)2, kl_datagram_send_queued(&dg));
    /* count-based backpressure: both slots occupied → WOULD_BLOCK, nothing taken */
    ASSERT_EQ((int)KL_DATAGRAM_WOULD_BLOCK, (int)kl_datagram_send(&dg, &m3));
    ASSERT_EQ((size_t)2, kl_datagram_send_queued(&dg));

    drive_send(1);                              /* #1 retires → FIFO pumps #2 (dest B) */
    ASSERT_EQ(2, g_mc.send_posted);
    ASSERT_EQ(5353, (int)kl_sockaddr_port(&g_mc.send_dest));   /* B, not C — FIFO */
    drive_send(1);
    ASSERT_EQ((size_t)0, kl_datagram_send_queued(&dg));

    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
}

UTEST(datagram_public, copy_before_accept_facade) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    char buf[4] = { 'A','A','A','A' };
    KlSockAddr d = addr4(10,0,0,1, 53);
    KlDatagramMessage m = { .data = buf, .len = 4, .peer = &d, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m));
    memcpy(buf, "BBBB", 4);          /* mutate the CALLER buffer right after ACCEPTED */
    drive_send(1);
    ASSERT_EQ((size_t)4, g_mc.send_len);
    ASSERT_EQ(0, memcmp(g_mc.send_copy, "AAAA", 4));   /* transmitted bytes are the ORIGINAL (facade copied) */
    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
}

UTEST(datagram_public, recv_deliver_and_truncated) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));
    ASSERT_EQ(1, g_mc.recv_posted);   /* the machine armed a completion recv */
    KlSockAddr peer = addr4(192,168,0,9, 40000);
    drive_recv("hello", 5, &peer, 0);
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_EQ((size_t)5, g_recv_len);
    ASSERT_TRUE(g_recv_has_peer);
    ASSERT_EQ(0, memcmp(g_recv_buf, "hello", 5));
    ASSERT_EQ(2, g_mc.recv_posted);   /* re-armed after delivery */
    /* a truncated datagram delivers once with the flag + bumps the counter */
    drive_recv("toolong", 7, &peer, 1);
    ASSERT_EQ(2, g_recv_calls);
    ASSERT_TRUE((g_recv_flags & KL_DGRAM_TRUNCATED) != 0);
    ASSERT_EQ((uint64_t)1, kl_datagram_truncated(&dg));
    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    drive_recv_cancelled();   /* the cancelled recv op's terminal completion → close joins CLOSED */
    ASSERT_EQ((int)KL_DGRAM_CLOSE_CLOSED, (int)kl_datagram_close_state(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
}

UTEST(datagram_public, pause_holds_one_then_resume_delivers) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    g_recv_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));
    kl_datagram_pause(&dg);
    KlSockAddr peer = addr4(192,168,0,9, 40000);
    drive_recv("held", 4, &peer, 0);       /* a posted recv completes while paused → HELD, not delivered */
    ASSERT_EQ(0, g_recv_calls);
    ASSERT_EQ(0, kl_datagram_resume(&dg));  /* resume delivers the held datagram exactly once */
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_EQ(0, memcmp(g_recv_buf, "held", 4));
    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    drive_recv_cancelled();
    ASSERT_EQ(0, kl_datagram_free(&dg));
}

/* 7B-7: a completion transport registers its fd with the loop (kl_event_add) before posting. A
 * registration FAILURE must fail init cleanly — nothing adopted, posted, or closed; the caller keeps
 * the fd. (Mirrors KlUdp's fd↔loop lifecycle; inert on io_uring/pollcomp, CreateIoCompletionPort on IOCP.) */
UTEST(datagram_public, registration_failure_keeps_fd) {
    mk_ctx(); mc_reset();
    g_mc.add_fail = 1;   /* the loop refuses to register the fd */
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlSocketHandle fd = mk_fd();
    KlDatagramConfig c = cfg_for(fd, 4, 1500);
    ASSERT_EQ(-1, kl_datagram_init(&dg, &c));
    ASSERT_EQ((int)KL_ERR_EVENT_ADD, (int)kl_datagram_last_error(&dg));
    ASSERT_EQ(0, g_mc.add_calls);     /* add returned -1, recorded nothing */
    ASSERT_EQ(0, g_mc.recv_posted);   /* nothing posted (init never reached the core) */
    ASSERT_EQ(0, g_mc.send_posted);
    ASSERT_EQ(0, g_mc.close_seq);      /* fd NOT closed — the caller retains it */
    (void)close((int)fd);
}

/* 7B-7 lifecycle ordering: register BEFORE the first post; on close, deregister BEFORE the socket close
 * (retire → kl_event_del → close, exactly once). Uses the mock socket provider to observe the close. */
UTEST(datagram_public, registration_ordering) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 4, 1500);
    c.sockets = &MC_SP;   /* observe the close ordering */
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ(1, g_mc.add_calls);     /* registered exactly once at init */
    ASSERT_EQ(0, g_mc.recv_posted);   /* ...before any post */
    kl_datagram_on_close(&dg, on_close, NULL); g_close_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));
    ASSERT_TRUE(g_mc.add_seq > 0 && g_mc.first_post_seq > 0);
    ASSERT_TRUE(g_mc.add_seq < g_mc.first_post_seq);   /* register BEFORE posting */
    ASSERT_EQ(0, kl_datagram_close_begin(&dg));
    drive_recv_cancelled();
    ASSERT_EQ((int)KL_DGRAM_CLOSE_CLOSED, (int)kl_datagram_close_state(&dg));
    ASSERT_EQ((int)KL_DGRAM_DETACHED, (int)kl_datagram_close_result(&dg));
    ASSERT_EQ(1, g_mc.del_calls);     /* deregistered exactly once */
    ASSERT_TRUE(g_mc.del_seq > 0 && g_mc.close_seq > 0);
    ASSERT_TRUE(g_mc.del_seq < g_mc.close_seq);   /* deregister BEFORE the socket close */
    ASSERT_EQ(0, kl_datagram_free(&dg));
}

/* A failing allocator: forwards to the default, but the Nth malloc returns NULL — to force an
 * allocation failure DURING core preparation, BEFORE the pre-adoption registration hook. */
static KlAllocator g_dfl; static int g_fail_at, g_alloc_n;
static void *fa_malloc(void *ctx, size_t size) {
    (void)ctx; if (++g_alloc_n == g_fail_at) return NULL; return g_dfl.malloc(g_dfl.ctx, size);
}
static void *fa_realloc(void *ctx, void *p, size_t o, size_t n) { (void)ctx; return g_dfl.realloc(g_dfl.ctx, p, o, n); }
static void  fa_free(void *ctx, void *p, size_t s) { (void)ctx; g_dfl.free(g_dfl.ctx, p, s); }
static KlAllocator failing_alloc(int fail_at) {
    g_dfl = kl_allocator_default(); g_fail_at = fail_at; g_alloc_n = 0;
    KlAllocator a = { fa_malloc, fa_realloc, fa_free, NULL }; return a;
}

/* 7B-7 (review): an allocation failure DURING core preparation must leave the fd UN-registered — proving
 * registration is gated behind successful prep (the pre-adoption hook), not rolled back by kl_event_del
 * (which on IOCP is a no-op and cannot detach an ordinary socket from its port). The failing allocator
 * fails the first core-internal allocation (#2: the facade's KlDgramCore struct is #1), which is before
 * the registration hook — so kl_event_add must NEVER be called and the caller keeps a clean fd. */
UTEST(datagram_public, alloc_failure_during_prep_leaves_fd_unregistered) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlSocketHandle fd = mk_fd();
    KlAllocator fa = failing_alloc(2);
    KlDatagramConfig c = cfg_for(fd, 4, 1500);
    c.alloc = &fa;   /* the core allocates through this; the ctx keeps its own allocator */
    ASSERT_EQ(-1, kl_datagram_init(&dg, &c));
    ASSERT_EQ(0, g_mc.add_calls);   /* registration NEVER ran — prep failed first → fd NOT associated */
    ASSERT_EQ(0, g_mc.del_calls);   /* and no del-as-rollback was relied upon */
    ASSERT_NE((int)KL_ERR_NONE, (int)kl_datagram_last_error(&dg));
    (void)close((int)fd);           /* the caller keeps a clean, un-associated fd */
}

/* Terminal QUARANTINE classification (the fail-closed leak of the life-owned inbound storage) is proven
 * at the core/backend layer where the arena teardown keeps it LSan-clean — test_dgram_core (7A-3) and
 * the EFI host-mock (7B-2c retire_dgram override). The public test stays on the clean DETACHED paths so
 * it is leak-free under container ASan/UBSan/LSan. */

/* ── Option A: synchronous owner-destruction teardown (kl_datagram_teardown) ──────────────────────
 *
 * kl_datagram_teardown abandons any in-flight op and reclaims the object IMMEDIATELY (no loop pump),
 * for a consumer bound to a synchronous free contract (the DNS resolver). These drive the scripted
 * completion mock exactly like the real driver: the op's late terminal completion is delivered AFTER the
 * teardown; it must dispatch against the now-dead token (NULL owner), drop, and release the op's ref so
 * the life-owned rx storage is reclaimed — all UAF/leak-clean (ASan/UBSan; LSan on Linux). Silent: no
 * on_close callback fires and no public terminal is reported (§4a). See
 * docs/datagram_sync_teardown_design.md. */

UTEST(datagram_public, teardown_recv_only_then_late_terminal) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    kl_datagram_on_close(&dg, on_close, NULL); g_close_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));   /* posts a recv (holds a token ref) */

    ASSERT_EQ(0, kl_datagram_teardown(&dg, NULL, NULL));                    /* synchronous abandon + free */
    ASSERT_EQ(0, g_close_calls);                               /* SILENT — no on_close fired (§4a) */
    ASSERT_EQ((int)KL_DGRAM_CLOSE_CLOSED, (int)kl_datagram_close_state(&dg));  /* handle memset → CLOSED */

    /* the posted recv's cancel terminal drains LATER → dead token → drop + release ref → rx freed */
    drive_recv_cancelled();
}

UTEST(datagram_public, teardown_send_inflight_then_late_terminal) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    KlSockAddr peer = addr4(10, 0, 0, 1, 53);
    KlDatagramMessage m = { .data = "query", .len = 5, .peer = &peer, .local = NULL, .tos = -1, .flags = 0 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m));   /* send in-flight (holds a ref) */
    ASSERT_EQ(1, g_mc.send_posted);

    ASSERT_EQ(0, kl_datagram_teardown(&dg, NULL, NULL));   /* abandons the in-flight send + frees the ring (the 64B case) */

    drive_send(1);   /* the send's late completion → dead token → drop (no send_on_complete) + release ref */
}

UTEST(datagram_public, teardown_recv_and_send_simultaneous) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));
    KlSockAddr peer = addr4(10, 0, 0, 1, 53);
    KlDatagramMessage m = { .data = "q", .len = 1, .peer = &peer, .local = NULL, .tos = -1, .flags = 0 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m));

    ASSERT_EQ(0, kl_datagram_teardown(&dg, NULL, NULL));   /* both a recv AND a send in-flight at destroy */

    drive_send(1);            /* late send terminal drops */
    drive_recv_cancelled();   /* late recv terminal drops → last ref → rx storage reclaimed */
}

UTEST(datagram_public, teardown_is_silent_no_callback) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    kl_datagram_on_close(&dg, on_close, NULL); g_close_calls = 0;
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));

    ASSERT_EQ(0, kl_datagram_teardown(&dg, NULL, NULL));
    ASSERT_EQ(0, g_close_calls);   /* the user close callback must NEVER fire during owner-destruction */
    drive_recv_cancelled();
    ASSERT_EQ(0, g_close_calls);   /* nor when the late terminal drains */
}

UTEST(datagram_public, teardown_then_reuse_late_terminal_drops) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));
    struct KlDgramLife *old_recv_life = g_mc.recv_life;   /* the FIRST datagram's recv op */

    ASSERT_EQ(0, kl_datagram_teardown(&dg, NULL, NULL));

    /* Re-init a NEW datagram on the same ctx (the handle was memset → reusable). */
    KlDatagramConfig c2 = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c2));
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));

    /* The OLD op's late terminal drains now — it must drop against its own dead token and NOT touch the
     * new object. Drive it explicitly by the OLD life (not g_mc.recv_life, which is now the new op's). */
    {
        KlCompletionEvent ev; memset(&ev, 0, sizeof(ev));
        ev.kind = KL_COMP_DGRAM_RECV; ev.ok = 0; ev.life = old_recv_life;
        kl_dgram_life_dispatch(old_recv_life)(kl_dgram_life_target(old_recv_life), &ev);
    }

    /* Tear the new one down cleanly too. */
    ASSERT_EQ(0, kl_datagram_teardown(&dg, NULL, NULL));
    drive_recv_cancelled();   /* the new op's late terminal (g_mc.recv_life is the new one) */
}

/* ── Option A §4a: reentrancy (teardown from within a delivery frame) + idempotence ───────────────
 *
 * When kl_datagram_teardown is invoked from on_recv / on_writable / on_drain, the datagram's own machine
 * frame is still unwinding (close.busy > 0). The teardown must DEFER the silent terminal + reclamation to
 * the outermost frame leave (still within the same top-level dispatch) so recv_leave/send_leave never run
 * against freed state, the owner reclaim fires exactly once, and there is no UAF (ASan/UBSan; LSan). */
static int g_owner_reclaimed;
static int g_reclaimed_in_frame;   /* snapshot of g_owner_reclaimed taken just after the in-frame teardown */
static void owner_reclaim_cb(void *ctx) { (void)ctx; g_owner_reclaimed++; }

static void on_recv_teardown(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                             const KlSockAddr *local, unsigned flags) {
    (void)data; (void)len; (void)peer; (void)local; (void)flags;
    g_recv_calls++;
    /* Tear down from WITHIN delivery (busy > 0). Must defer — the reclaim runs at the outermost leave. */
    kl_datagram_teardown((KlDatagram *)ud, owner_reclaim_cb, NULL);
    g_reclaimed_in_frame = g_owner_reclaimed;   /* must still be 0 here (deferred), asserted in the body */
}

UTEST(datagram_public, teardown_from_within_on_recv_defers) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    g_recv_calls = 0; g_owner_reclaimed = 0; g_reclaimed_in_frame = -1;
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv_teardown, &dg));
    KlSockAddr peer = addr4(1, 2, 3, 4, 53);
    drive_recv("x", 1, &peer, 0);   /* deliver → on_recv → teardown (deferred to recv_leave) */
    ASSERT_EQ(1, g_recv_calls);
    ASSERT_EQ(0, g_reclaimed_in_frame);   /* deferred: NOT reclaimed while the delivery frame was active */
    ASSERT_EQ(1, g_owner_reclaimed);      /* reclaim ran exactly once, at the outermost leave */
}

/* Same, from the SEND side: teardown from within on_drain (fired on the non-empty→empty edge, inside the
 * send op's busy frame). The reclaim — which frees the SEND machine — must defer to send_leave. */
static void on_drain_teardown(void *ud) {
    kl_datagram_teardown((KlDatagram *)ud, owner_reclaim_cb, NULL);
    g_reclaimed_in_frame = g_owner_reclaimed;   /* deferred → still 0 here */
}
UTEST(datagram_public, teardown_from_within_on_drain_defers) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    g_owner_reclaimed = 0; g_reclaimed_in_frame = -1;
    kl_datagram_on_drain(&dg, on_drain_teardown, &dg);
    KlSockAddr peer = addr4(1, 2, 3, 4, 53);
    KlDatagramMessage m = { .data = "q", .len = 1, .peer = &peer, .local = NULL, .tos = -1, .flags = 0 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m));
    drive_send(1);   /* retire the send → queue non-empty→empty → on_drain → teardown (deferred) */
    ASSERT_EQ(0, g_reclaimed_in_frame);   /* deferred while the send frame was active */
    ASSERT_EQ(1, g_owner_reclaimed);      /* reclaim ran once, at send_leave */
}

/* §4b deferred-window idempotence: a SECOND teardown while the first is still deferred (inside the same
 * active frame) — the shape a nested cancellation callback would take — must NOT replace the first
 * request's owner reclaim (nor NULL it out and leak the owner). The ORIGINAL owner runs exactly once. */
static int g_owner_a, g_owner_b;
static void owner_a(void *c) { (void)c; g_owner_a++; }
static void owner_b(void *c) { (void)c; g_owner_b++; }
static void on_recv_teardown_twice(void *ud, const void *data, size_t len, const KlSockAddr *peer,
                                   const KlSockAddr *local, unsigned flags) {
    (void)data; (void)len; (void)peer; (void)local; (void)flags;
    KlDatagram *dg = ud;
    kl_datagram_teardown(dg, owner_a, NULL);   /* first request → owner_a (deferred) */
    kl_datagram_teardown(dg, owner_b, NULL);   /* second, deferred window: must be a no-op, keep owner_a */
    kl_datagram_teardown(dg, NULL, NULL);      /* third with NULL owner: must NOT null the armed reclaim */
    g_reclaimed_in_frame = g_owner_a + g_owner_b;   /* both still 0 — deferred */
}
UTEST(datagram_public, teardown_twice_in_frame_keeps_first_owner) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    g_owner_a = 0; g_owner_b = 0; g_reclaimed_in_frame = -1;
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv_teardown_twice, &dg));
    KlSockAddr peer = addr4(1, 2, 3, 4, 53);
    drive_recv("x", 1, &peer, 0);
    ASSERT_EQ(0, g_reclaimed_in_frame);   /* deferred: nothing ran while the frame was active */
    ASSERT_EQ(1, g_owner_a);              /* the ORIGINAL owner ran exactly once */
    ASSERT_EQ(0, g_owner_b);              /* the deferred-window replacement was ignored (no owner leak) */
}

UTEST(datagram_public, teardown_idempotent_twice) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));
    g_owner_reclaimed = 0;
    ASSERT_EQ(0, kl_datagram_teardown(&dg, owner_reclaim_cb, NULL));   /* synchronous (not in a frame) */
    ASSERT_EQ(1, g_owner_reclaimed);
    /* Second teardown on the now-cleared handle → no-op success, reclaim NOT re-invoked. */
    ASSERT_EQ(0, kl_datagram_teardown(&dg, owner_reclaim_cb, NULL));
    ASSERT_EQ(1, g_owner_reclaimed);
    drive_recv_cancelled();   /* the posted recv's late terminal drops → rx storage reclaimed */
}

UTEST(datagram_public, teardown_then_free_idempotent) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ(0, kl_datagram_recv_start(&dg, on_recv, NULL));
    ASSERT_EQ(0, kl_datagram_teardown(&dg, NULL, NULL));
    ASSERT_EQ(0, kl_datagram_free(&dg));   /* free after teardown → no-op success (was -1) */
    drive_recv_cancelled();
}

/* ══ M1 — BOTH byte-gate policy through the PUBLIC facade (§10.10) ═════════════════════════════════
 * kl_datagram_init_ex(send_byte_budget > 0) selects BOTH end-to-end over the scripted completion mock:
 * the byte gate binds before the slot count, an oversize datagram is a permanent TOO_LARGE (completion
 * mode refuses upfront), and a retirement reopens admission. */
UTEST(datagram_public, m1_both_budget_via_init_ex) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 4 /*slots*/, 1500 /*slot_cap*/);
    ASSERT_EQ(0, kl_datagram_init_ex(&dg, &c, 20 /*byte_budget*/));   /* BOTH, budget 20 */

    KlSockAddr d = addr4(10,0,0,1, 53);
    KlDatagramMessage m10 = { .data = "0123456789", .len = 10, .peer = &d, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m10));  /* in flight (bytes 10) */
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m10));  /* queued (bytes 20) */
    ASSERT_EQ(1, g_mc.send_posted);                                         /* single-flight */
    /* case (b): budget full though slots remain free */
    ASSERT_EQ((int)KL_DATAGRAM_WOULD_BLOCK, (int)kl_datagram_send(&dg, &m10));
    ASSERT_EQ((size_t)2, kl_datagram_send_queued(&dg));
    /* case (a): a datagram larger than the WHOLE budget (30 > 20, ≤ slot_cap) → permanent TOO_LARGE,
     * refused upfront in completion mode (never posted). */
    char big[30]; memset(big, 'X', sizeof(big));
    KlDatagramMessage m30 = { .data = big, .len = 30, .peer = &d, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_TOO_LARGE, (int)kl_datagram_send(&dg, &m30));
    ASSERT_EQ(1, g_mc.send_posted);                                         /* not posted */
    ASSERT_EQ((size_t)2, kl_datagram_send_queued(&dg));                    /* queue unchanged */
    /* retire the in-flight head → budget frees → admission reopens */
    drive_send(1);
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m10));

    drive_send(1); drive_send(1);                /* drain the remaining in-flight+queued sends */
    ASSERT_EQ((size_t)0, kl_datagram_send_queued(&dg));
    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
}

/* kl_datagram_init is the SLOT policy: the byte budget is inert, so admission is count-bounded only —
 * two 1000-byte datagrams (2000 bytes, far above any small budget) both admit; the slot count refuses. */
UTEST(datagram_public, m1_init_is_slot_policy) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2 /*slots*/, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));                                /* SLOT (budget 0) */

    KlSockAddr d = addr4(10,0,0,1, 53);
    char big[1000]; memset(big, 'Z', sizeof(big));
    KlDatagramMessage m = { .data = big, .len = 1000, .peer = &d, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m));   /* in flight */
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m));   /* queued — no byte gate */
    ASSERT_EQ((int)KL_DATAGRAM_WOULD_BLOCK, (int)kl_datagram_send(&dg, &m)); /* refused by SLOTS only */
    ASSERT_EQ((size_t)2, kl_datagram_send_queued(&dg));

    drive_send(1); drive_send(1);                /* drain in-flight+queued before close */
    ASSERT_EQ((size_t)0, kl_datagram_send_queued(&dg));
    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
}

/* ══ M2 — capability derivation + multicast (docs/datagram_m2_capability_design.md §9) ════════════
 * A scriptable mock socket provider whose dgram vtable reports caps + records mcast_membership, so the
 * FACADE gate/routing can be tested independently of any real provider. */
static unsigned g_mock_caps;                     /* what mock caps() returns */
static int      g_mock_caps_null;                /* 1 → present a NULL caps op */
static int      g_mcast_calls, g_mcast_family, g_mcast_join; static unsigned g_mcast_iface;
static char     g_mcast_group[64]; static int g_mcast_ret;
static unsigned mock_dg_caps(void *ctx, KlSocketHandle fd) { (void)ctx; (void)fd; return g_mock_caps; }
static int mock_dg_mcast(void *ctx, KlSocketHandle fd, int family, const char *group,
                         unsigned iface, int join) {
    (void)ctx; (void)fd;
    g_mcast_calls++; g_mcast_family = family; g_mcast_join = join; g_mcast_iface = iface;
    snprintf(g_mcast_group, sizeof(g_mcast_group), "%s", group ? group : "");
    return g_mcast_ret;
}
static const KlDatagramOps MOCK_DG_WITH_CAPS = { .caps = mock_dg_caps, .mcast_membership = mock_dg_mcast };
static const KlDatagramOps MOCK_DG_NO_CAPS   = { .caps = NULL,         .mcast_membership = mock_dg_mcast };
static const KlSocketProvider MOCK_SP_CAPS    = { .ops = &MC_SOCK_OPS, .dgram = &MOCK_DG_WITH_CAPS };
static const KlSocketProvider MOCK_SP_NOCAPS  = { .ops = &MC_SOCK_OPS, .dgram = &MOCK_DG_NO_CAPS };

static KlDatagramConfig cfg_caps(KlSocketHandle fd, unsigned want_caps) {
    KlDatagramConfig c; memset(&c, 0, sizeof(c));
    c.ctx = &g_ctx; c.alloc = &g_alloc;
    c.sockets = g_mock_caps_null ? &MOCK_SP_NOCAPS : &MOCK_SP_CAPS;
    c.fd = fd; c.send_slots = 4; c.send_slot_cap = 1500; c.recv_cap = 2048; c.want_caps = want_caps;
    return c;
}
/* no ops outstanding → DETACHED → free (macro: utest ASSERTs must expand inside a UTEST body) */
#define m2_close(dg) do { ASSERT_EQ(0, kl_datagram_close_begin(dg)); ASSERT_EQ(0, kl_datagram_free(dg)); } while (0)

/* §9.1 — kl_datagram_provider_caps() = provider set; kl_datagram_caps() = granted (want_caps). */
UTEST(datagram_public, m2_caps_derivation) {
    mk_ctx(); mc_reset(); g_mock_caps_null = 0;
    g_mock_caps = KL_DGRAM_CAP_SOURCE_PIN | KL_DGRAM_CAP_MULTICAST;
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_caps(mk_fd(), KL_DGRAM_CAP_SOURCE_PIN);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ((unsigned)(KL_DGRAM_CAP_SOURCE_PIN | KL_DGRAM_CAP_MULTICAST), kl_datagram_provider_caps(&dg));
    ASSERT_EQ((unsigned)KL_DGRAM_CAP_SOURCE_PIN, kl_datagram_caps(&dg));   /* granted, not the full set */
    m2_close(&dg);
}

/* §9.2 — want_caps init gate is fail-loud, fd not adopted. */
UTEST(datagram_public, m2_want_caps_gate_failloud) {
    mk_ctx(); mc_reset(); g_mock_caps_null = 0;
    g_mock_caps = KL_DGRAM_CAP_SOURCE_PIN;   /* provider lacks TOS */
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlSocketHandle fd = mk_fd();
    KlDatagramConfig bad = cfg_caps(fd, KL_DGRAM_CAP_SOURCE_PIN | KL_DGRAM_CAP_TOS);
    ASSERT_EQ(-1, kl_datagram_init(&dg, &bad));
    ASSERT_EQ((int)KL_ERR_UNSUPPORTED, (int)kl_datagram_last_error(&dg));
    (void)close((int)fd);   /* caller still owns the fd (not adopted) */
    /* a subset request succeeds */
    KlDatagram dg2; memset(&dg2, 0, sizeof(dg2));
    KlDatagramConfig ok = cfg_caps(mk_fd(), KL_DGRAM_CAP_SOURCE_PIN);
    ASSERT_EQ(0, kl_datagram_init(&dg2, &ok));
    m2_close(&dg2);
}

/* §9.3 — NULL caps op ⇒ no optional caps; any non-zero want_caps fails init. */
UTEST(datagram_public, m2_null_caps_no_optional) {
    mk_ctx(); mc_reset(); g_mock_caps_null = 1;   /* MOCK_SP_NOCAPS: caps == NULL */
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c0 = cfg_caps(mk_fd(), 0);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c0));               /* want_caps 0 → ok */
    ASSERT_EQ((unsigned)0, kl_datagram_provider_caps(&dg)); /* no optional caps */
    m2_close(&dg);
    KlDatagram dg2; memset(&dg2, 0, sizeof(dg2));
    KlSocketHandle fd = mk_fd();
    KlDatagramConfig cc = cfg_caps(fd, KL_DGRAM_CAP_CONNECTED);
    ASSERT_EQ(-1, kl_datagram_init(&dg2, &cc));             /* any cap → fail */
    ASSERT_EQ((int)KL_ERR_UNSUPPORTED, (int)kl_datagram_last_error(&dg2));
    (void)close((int)fd);
    g_mock_caps_null = 0;
}

/* §9.4 (blocker P1) — a family-limited report makes an unavailable requested cap fail INIT. */
UTEST(datagram_public, m2_family_limited_rejects_at_init) {
    mk_ctx(); mc_reset(); g_mock_caps_null = 0;
    /* model an IPv6 fd: everything BUT the IPv4-only BROADCAST */
    g_mock_caps = KL_DGRAM_CAP_SOURCE_PIN | KL_DGRAM_CAP_TOS |
                  KL_DGRAM_CAP_CONNECTED | KL_DGRAM_CAP_MULTICAST;
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlSocketHandle fd = mk_fd();
    KlDatagramConfig c = cfg_caps(fd, KL_DGRAM_CAP_BROADCAST);
    ASSERT_EQ(-1, kl_datagram_init(&dg, &c));               /* BROADCAST not usable on this fd */
    ASSERT_EQ((int)KL_ERR_UNSUPPORTED, (int)kl_datagram_last_error(&dg));
    (void)close((int)fd);
    /* model an IPv4 fd: BROADCAST now present → granted */
    g_mock_caps |= KL_DGRAM_CAP_BROADCAST;
    KlDatagram dg2; memset(&dg2, 0, sizeof(dg2));
    KlDatagramConfig c2 = cfg_caps(mk_fd(), KL_DGRAM_CAP_BROADCAST);
    ASSERT_EQ(0, kl_datagram_init(&dg2, &c2));
    m2_close(&dg2);
}

/* §9.5 — multicast gated on the capability; no provider call when ungranted. */
UTEST(datagram_public, m2_multicast_gated) {
    mk_ctx(); mc_reset(); g_mock_caps_null = 0;
    g_mock_caps = KL_DGRAM_CAP_CONNECTED;   /* no MULTICAST */
    g_mcast_calls = 0; g_mcast_ret = 0;
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_caps(mk_fd(), 0);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ(-1, kl_datagram_multicast_join(&dg, "239.1.2.3", 0));
    ASSERT_EQ((int)KL_ERR_UNSUPPORTED, (int)kl_datagram_last_error(&dg));
    ASSERT_EQ(0, g_mcast_calls);            /* provider NOT called */
    m2_close(&dg);
}

/* §9.6 — deterministic multicast error outcomes. */
UTEST(datagram_public, m2_multicast_error_outcomes) {
    mk_ctx(); mc_reset(); g_mock_caps_null = 0;
    g_mock_caps = KL_DGRAM_CAP_MULTICAST;
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_caps(mk_fd(), 0);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    /* malformed group → INVALID_ARG, no provider call */
    g_mcast_calls = 0;
    ASSERT_EQ(-1, kl_datagram_multicast_join(&dg, "not-an-ip", 0));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)kl_datagram_last_error(&dg));
    ASSERT_EQ(0, g_mcast_calls);
    /* valid but NON-multicast IP → INVALID_ARG, still no provider call */
    ASSERT_EQ(-1, kl_datagram_multicast_join(&dg, "8.8.8.8", 0));
    ASSERT_EQ((int)KL_ERR_INVALID_ARG, (int)kl_datagram_last_error(&dg));
    ASSERT_EQ(0, g_mcast_calls);
    /* provider/syscall failure → IO */
    g_mcast_ret = -1;
    ASSERT_EQ(-1, kl_datagram_multicast_join(&dg, "239.1.2.3", 0));
    ASSERT_EQ((int)KL_ERR_IO, (int)kl_datagram_last_error(&dg));
    ASSERT_EQ(1, g_mcast_calls);
    m2_close(&dg);
}

/* §9.7 — join/leave route to mcast_membership with the family derived from the group literal. */
UTEST(datagram_public, m2_multicast_routes) {
    mk_ctx(); mc_reset(); g_mock_caps_null = 0;
    g_mock_caps = KL_DGRAM_CAP_MULTICAST; g_mcast_ret = 0;
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_caps(mk_fd(), 0);
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    g_mcast_calls = 0;
    ASSERT_EQ(0, kl_datagram_multicast_join(&dg, "239.1.2.3", 7));
    ASSERT_EQ(1, g_mcast_calls);
    ASSERT_EQ(AF_INET, g_mcast_family);       /* IPv4 group → AF_INET */
    ASSERT_EQ(1, g_mcast_join);
    ASSERT_EQ((unsigned)7, g_mcast_iface);
    ASSERT_EQ(0, strcmp(g_mcast_group, "239.1.2.3"));
    ASSERT_EQ(0, kl_datagram_multicast_leave(&dg, "ff02::fb", 0));
    ASSERT_EQ(AF_INET6, g_mcast_family);      /* IPv6 group → AF_INET6 */
    ASSERT_EQ(0, g_mcast_join);
    m2_close(&dg);
}

/* Per-provider verification — the REAL POSIX provider (sockets = NULL) reports its true per-fd-family
 * set. The expected mask is built under the SAME family-specific compile guards the provider uses (so
 * this passes on reduced-capability POSIX builds where a macro is absent), with explicit assertions for
 * the always-available CONNECTED and the IPv4-only BROADCAST. */
UTEST(datagram_public, m2_posix_provider_caps_per_family) {
    mk_ctx(); mc_reset();
    unsigned exp4 = KL_DGRAM_CAP_CONNECTED;      /* connect()+send: always */
#if defined(IP_PKTINFO)
    exp4 |= KL_DGRAM_CAP_SOURCE_PIN;
#endif
#if defined(IP_TOS)
    exp4 |= KL_DGRAM_CAP_TOS;
#endif
#if defined(IP_ADD_MEMBERSHIP)
    exp4 |= KL_DGRAM_CAP_MULTICAST;
#endif
#if defined(SO_BROADCAST)
    exp4 |= KL_DGRAM_CAP_BROADCAST;              /* IPv4-only */
#endif
    unsigned exp6 = KL_DGRAM_CAP_CONNECTED;
#if defined(IPV6_PKTINFO)
    exp6 |= KL_DGRAM_CAP_SOURCE_PIN;
#endif
#if defined(IPV6_TCLASS)
    exp6 |= KL_DGRAM_CAP_TOS;
#endif
#if defined(IPV6_JOIN_GROUP) || defined(IPV6_ADD_MEMBERSHIP)
    exp6 |= KL_DGRAM_CAP_MULTICAST;
#endif
    /* M5 high-throughput SUPPORT is family-independent and, in the POSIX provider, gated on __linux__
     * (recvmmsg/sendmmsg + the always-defined-on-Linux UDP_SEGMENT/UDP_GRO fallbacks) — so all four are
     * present on Linux and absent elsewhere. Mirror the provider (socket_dgram_posix.c pdg_caps). */
#if defined(__linux__)
    unsigned m5 = KL_DGRAM_CAP_RX_BATCH | KL_DGRAM_CAP_TX_BATCH | KL_DGRAM_CAP_GSO | KL_DGRAM_CAP_GRO;
    exp4 |= m5; exp6 |= m5;
#endif
    /* AF_INET */
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 4, 1500);   /* sockets = NULL → posix ops, AF_INET fd */
    ASSERT_EQ(0, kl_datagram_init(&dg, &c));
    ASSERT_EQ(exp4, kl_datagram_provider_caps(&dg));
    ASSERT_TRUE((kl_datagram_provider_caps(&dg) & KL_DGRAM_CAP_CONNECTED));   /* explicit: always */
#if defined(SO_BROADCAST)
    ASSERT_TRUE((kl_datagram_provider_caps(&dg) & KL_DGRAM_CAP_BROADCAST));   /* explicit: IPv4 has it where compiled */
#else
    ASSERT_EQ((unsigned)0, (kl_datagram_provider_caps(&dg) & KL_DGRAM_CAP_BROADCAST));
#endif
    m2_close(&dg);
    /* AF_INET6 (skip if unavailable) */
    KlSocketHandle fd6 = (KlSocketHandle)socket(AF_INET6, SOCK_DGRAM, 0);
    if (kl_handle_valid(fd6)) {
        KlDatagram dg6; memset(&dg6, 0, sizeof(dg6));
        KlDatagramConfig c6 = cfg_for(fd6, 4, 1500);
        ASSERT_EQ(0, kl_datagram_init(&dg6, &c6));
        ASSERT_EQ(exp6, kl_datagram_provider_caps(&dg6));
        ASSERT_TRUE((kl_datagram_provider_caps(&dg6) & KL_DGRAM_CAP_CONNECTED));          /* explicit: always */
        ASSERT_EQ((unsigned)0, (kl_datagram_provider_caps(&dg6) & KL_DGRAM_CAP_BROADCAST)); /* explicit: no IPv6 broadcast */
        m2_close(&dg6);
    }
}

/* M4 — kl_datagram_fd/local_port require a LIVE core: a zeroed handle, a failed init, and a freed
 * datagram all report the invalid fd / port 0 (never a zeroed or stale-closed descriptor). */
UTEST(datagram_public, m4_fd_accessors_require_live_core) {
    mk_ctx(); mc_reset();
    /* before init: a memset-zero handle */
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    ASSERT_EQ(KL_INVALID_SOCKET, kl_datagram_fd(&dg));
    ASSERT_EQ((uint16_t)0, kl_datagram_local_port(&dg));
    /* failed init (send_slots 0 → core not created): fd not adopted → still invalid */
    KlSocketHandle bfd = mk_fd();
    KlDatagramConfig bad = cfg_for(bfd, 0 /*bad*/, 1500);
    ASSERT_EQ(-1, kl_datagram_init(&dg, &bad));
    ASSERT_EQ(KL_INVALID_SOCKET, kl_datagram_fd(&dg));
    ASSERT_EQ((uint16_t)0, kl_datagram_local_port(&dg));
    (void)close((int)bfd);   /* caller still owns the unadopted fd */
    /* success → live fd; then free → core NULL → invalid again */
    KlDatagram dg2; memset(&dg2, 0, sizeof(dg2));
    KlDatagramConfig c = cfg_for(mk_fd(), 4, 1500);
    ASSERT_EQ(0, kl_datagram_init(&dg2, &c));
    ASSERT_TRUE(kl_handle_valid(kl_datagram_fd(&dg2)));
    m2_close(&dg2);
    ASSERT_EQ(KL_INVALID_SOCKET, kl_datagram_fd(&dg2));   /* after free: core NULL */
    ASSERT_EQ((uint16_t)0, kl_datagram_local_port(&dg2));
}

/* A control (source-pinned) send whose completion is a TERMINAL ERROR retires the single in-flight op
 * exactly once (sticky error, no re-post, no queued-without-op state) — the completion guarantee. */
UTEST(datagram_public, m_control_send_terminal_error_retires_once) {
    mk_ctx(); mc_reset();
    KlDatagram dg; memset(&dg, 0, sizeof(dg));
    KlDatagramConfig c = cfg_for(mk_fd(), 2, 1500);
    c.want_caps = KL_DGRAM_CAP_SOURCE_PIN;                 /* the posix provider grants it on a real fd */
    ASSERT_EQ(0, kl_datagram_init_ex(&dg, &c, 0));

    KlSockAddr d = addr4(10,0,0,1, 53), loc = addr4(127,0,0,1, 0);
    KlDatagramMessage m = { .data = "AAAA", .len = 4, .peer = &d, .local = &loc, .tos = -1 };
    ASSERT_EQ((int)KL_DATAGRAM_ACCEPTED, (int)kl_datagram_send(&dg, &m));   /* one posted op */
    ASSERT_EQ(1, g_mc.send_posted);
    ASSERT_EQ((size_t)1, kl_datagram_send_inflight(&dg));

    drive_send(0);                                          /* TERMINAL ERROR completion */
    ASSERT_EQ((size_t)0, kl_datagram_send_inflight(&dg));  /* retired exactly once */
    ASSERT_EQ((size_t)0, kl_datagram_send_queued(&dg));    /* no queued-without-op state */
    ASSERT_EQ(1, g_mc.send_posted);                        /* not re-posted after the error */
    /* the send error is STICKY — a subsequent send is refused with ERROR (no new op posted). */
    ASSERT_EQ((int)KL_DATAGRAM_ERROR, (int)kl_datagram_send(&dg, &m));
    ASSERT_EQ(1, g_mc.send_posted);

    ASSERT_EQ(0, kl_datagram_close_cancel(&dg));
    ASSERT_EQ(0, kl_datagram_free(&dg));
}

UTEST_MAIN();
