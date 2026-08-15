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

#include "../vendor/utest.h"

#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/event_ctx.h>
#include <keel/allocator.h>
#include <keel/sockaddr.h>

#include "../src/completion.h"      /* KlCompletionEvent + KL_COMP_DGRAM_* */
#include "../src/datagram_life.h"   /* kl_dgram_life_dispatch/_target — drive completions like the driver */
#include "../src/socket.h"          /* KlSocketProvider / KlSocketOps — the close-ordering mock provider */
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

/* Terminal QUARANTINE classification (the fail-closed leak of the life-owned inbound storage) is proven
 * at the core/backend layer where the arena teardown keeps it LSan-clean — test_dgram_core (7A-3) and
 * the EFI host-mock (7B-2c retire_dgram override). The public test stays on the clean DETACHED paths so
 * it is leak-free under container ASan/UBSan/LSan. */

UTEST_MAIN();
