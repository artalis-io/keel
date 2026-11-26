/*
 * test_event_fairness.c: dispatch fairness of every event backend under sustained readiness.
 *
 * A wait (or a completion drain) returns at most `max` events. A backend that scanned its
 * registrations from the first one and stopped at `max` served the same `max` continuously ready
 * registrations on every wait and starved the rest for as long as the load lasted: later
 * connections, and a thread-pool wakeup registered after them. The HTTP server waits for 64 events a
 * tick, so on such a backend the 65th busy connection was never served and its idle timer then
 * closed it.
 *
 * Each case registers well over 64 continuously ready handles (a byte the test never consumes, or a
 * receive that is re-posted at once) and asserts a BOUNDED number of rounds before every one of them
 * is served: ceil(N / max) + 1 for a set registered together, 2 * ceil(N / max) for one that joins
 * or is served mid-lap, including under connection churn (src/event_turn.h). Eventual progress is not
 * enough. The readiness cases with removals also assert strict round-robin: no registration is served
 * a second time while another continuously ready one has not been served.
 *
 *   readiness (kl_event_wait):   poll, Windows select(), epoll, kqueue. Skips on a completion loop.
 *   watchers (kl_event_ctx_run): every backend, including the completion engines' watcher relay.
 *   posted ops (completion):     pollcomp, io_uring, IOCP. Skips on a readiness loop.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/event.h>
#include <keel/event_ctx.h>
#include <keel/stream.h>
#include <keel/stream_detail.h>     /* struct KlStream layout: set fd/ctx/alloc for a bare stream */
#include <keel/socket.h>            /* KlIoVec */
#include <keel/sockaddr.h>
#include <keel/handle.h>
#include "../src/socket.h"          /* kl_sockdef_*: full-width KlSocketHandle pairs on every platform */
#include "../src/completion.h"      /* kl_comp_post_recv_raw, KlCompletionEvent */
#include "../src/completion_io.h"   /* kl_comp_cancel */
#include "../src/event_caps.h"
#include "net_compat.h"
#include <stddef.h>
#include <string.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif

#define N_REG   200   /* registrations: a little over three waits' worth at 64 */
#define MAXEV   64    /* what the HTTP server asks for per tick */

static int rounds_bound(int n, int max) { return (n + max - 1) / max + 1; }
/* For a registration that joins (or is served) while others are mid-turn: the rest of the current
 * lap plus the next one (event_turn.h). */
static int joined_bound(int n, int max) { return 2 * ((n + max - 1) / max); }

static KlSocketHandle g_end[N_REG], g_peer[N_REG];
static KlSocketHandle g_lis = KL_INVALID_SOCKET;
static KlSockAddr     g_lis_addr;

/* 2 * N_REG + 1 descriptors: lift a low soft limit (macOS defaults to 256) to what the hard limit
 * allows. */
static void raise_fd_limit(void) {
#ifndef _WIN32
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 4096) {
        rlim_t want = 4096;
        if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want) want = rl.rlim_max;
        rl.rlim_cur = want;
        (void)setrlimit(RLIMIT_NOFILE, &rl);
    }
#endif
}

static int listener_open(void) {
    raise_fd_limit();
    g_lis = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(g_lis)) return -1;
    uint8_t lo[4] = { 127, 0, 0, 1 };
    KlSockAddr addr;
    kl_sockaddr_from_ipv4(&addr, lo, 0);
    if (kl_sockdef_bind(g_lis, &addr) < 0 || kl_sockdef_listen(g_lis, 16) < 0 ||
        kl_sockdef_get_local_addr(g_lis, &g_lis_addr) < 0) {
        kl_sockdef_close(g_lis);
        g_lis = KL_INVALID_SOCKET;
        return -1;
    }
    return 0;
}

static void listener_close(void) {
    if (kl_handle_valid(g_lis)) kl_sockdef_close(g_lis);
    g_lis = KL_INVALID_SOCKET;
}

/* A connected, non-blocking loopback TCP pair (overlapped-capable on IOCP). */
static int make_pair(int i) {
    KlSocketHandle cli = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(cli)) return -1;
    if (kl_sockdef_connect(cli, &g_lis_addr) < 0) { kl_sockdef_close(cli); return -1; }
    KlSocketHandle srv = kl_sockdef_accept(g_lis, NULL);
    if (!kl_handle_valid(srv)) { kl_sockdef_close(cli); return -1; }
    if (kl_sockdef_set_nonblocking(cli) < 0 || kl_sockdef_set_nonblocking(srv) < 0) {
        kl_sockdef_close(cli); kl_sockdef_close(srv); return -1;
    }
    g_end[i] = cli;
    g_peer[i] = srv;
    return 0;
}

static void close_pair(int i) {
    if (kl_handle_valid(g_end[i])) kl_sockdef_close(g_end[i]);
    if (kl_handle_valid(g_peer[i])) kl_sockdef_close(g_peer[i]);
    g_end[i] = g_peer[i] = KL_INVALID_SOCKET;
}

/* Queue `n` bytes at g_end[i]: it stays readable while the test does not consume them. */
static int make_ready(int i, int n) {
    char buf[256];
    memset(buf, 'x', sizeof buf);
    if (n > (int)sizeof buf) n = (int)sizeof buf;
    return (int)kl_sockdef_send(g_peer[i], buf, (size_t)n) == n ? 0 : -1;
}

/* Wait until g_end[i] has a byte to read (loopback delivery is asynchronous on some stacks), so a
 * measurement starts with every registration genuinely ready. */
static int await_readable(int i) {
    for (int t = 0; t < 2000; t++) {
        char b;
        if (kl_sock_recv_peek(NULL, g_end[i], &b, 1) == 1) return 0;
        kl_test_sleep_ms(1);
    }
    return -1;
}

/* Service accounting, by registration index (udata points into g_mark). */
static int g_mark[N_REG];
static int g_served[N_REG];        /* times served */
static int g_first_round[N_REG];   /* round of the first service, 0 = never */
static int g_live[N_REG];          /* registered and continuously ready */
static int g_rr_violation;         /* strict round-robin broken (see note_service) */

static void reset_accounting(void) {
    for (int i = 0; i < N_REG; i++) {
        g_mark[i] = i;
        g_served[i] = 0;
        g_first_round[i] = 0;
        g_live[i] = 0;
    }
    g_rr_violation = 0;
}

/* Record one service. Strict round-robin: serving a live registration for the (k+1)-th time while
 * another live one has been served fewer than k times means the second one lost its turn. */
static void note_service(int i, int round) {
    for (int j = 0; j < N_REG; j++)
        if (g_live[j] && j != i && g_served[j] < g_served[i])
            g_rr_violation = 1;
    g_served[i]++;
    if (!g_first_round[i]) g_first_round[i] = round;
}

static int all_live_served(void) {
    for (int i = 0; i < N_REG; i++)
        if (g_live[i] && !g_served[i]) return 0;
    return 1;
}

static int count_unserved(int from, int to) {
    int n = 0;
    for (int i = from; i < to; i++) n += g_served[i] == 0;
    return n;
}

static int count_live(void) {
    int n = 0;
    for (int i = 0; i < N_REG; i++) n += g_live[i];
    return n;
}

static int is_readiness(const KlEventLoop *loop) {
    return (kl_event_caps(loop) & KL_EVENT_CAP_READINESS) != 0;
}

/* ── Readiness: kl_event_wait directly ─────────────────────────────────────────────────────── */

/* Serve one reported registration as a consumer does: note it, consume nothing, and re-arm it the way
 * kl_event_dispatch re-arms a watcher (kl_event_mod). On a level-triggered backend (poll, select())
 * that is a no-op; on an edge-triggered one (epoll, kqueue) it is what reports a still-ready
 * descriptor again. Either way the registration stays continuously ready. */
static void serve(KlEventLoop *loop, int i, int round) {
    note_service(i, round);
    (void)kl_event_mod(loop, g_end[i], KL_EVENT_READ, &g_mark[i]);
}

/* One wait; every reported registration is served. Returns the number of events or -1. */
static int wait_round(KlEventLoop *loop, int round) {
    KlEvent ev[MAXEV];
    int n = kl_event_wait(loop, ev, MAXEV, 1000);
    for (int k = 0; k < n; k++) {
        const int i = *(const int *)ev[k].udata;
        if (i >= 0 && i < N_REG && g_live[i]) serve(loop, i, round);
    }
    return n;
}

typedef struct { KlEventLoop loop; KlAllocator alloc; int pairs; } RdFix;

static int rd_setup(RdFix *f, int pairs) {
    memset(f, 0, sizeof(*f));
    f->alloc = kl_allocator_default();
    f->loop.alloc = &f->alloc;
    if (kl_event_init(&f->loop) != 0) return -1;
    if (!is_readiness(&f->loop)) { kl_event_close(&f->loop); return 1; }
    if (listener_open() < 0) { kl_event_close(&f->loop); return -1; }
    reset_accounting();
    for (int i = 0; i < pairs; i++)
        if (make_pair(i) < 0) return -1;
    f->pairs = pairs;
    return 0;
}

static void rd_teardown(RdFix *f) {
    for (int i = 0; i < f->pairs; i++) {
        if (g_live[i]) kl_event_del(&f->loop, g_end[i]);
        close_pair(i);
    }
    listener_close();
    kl_event_close(&f->loop);
}

static int rd_register(RdFix *f, int i) {
    if (kl_event_add(&f->loop, g_end[i], KL_EVENT_READ, &g_mark[i]) != 0) return -1;
    if (make_ready(i, 1) != 0 || await_readable(i) != 0) return -1;
    g_live[i] = 1;
    return 0;
}

/* Every continuously ready registration is served within ceil(N / max) + 1 waits; the registrations
 * past the first `max` included. Scanning from slot 0 served registrations 0..63 on every wait and
 * never reached the other 136. */
UTEST(event_fairness, readiness_every_ready_registration_is_served_within_bound) {
    RdFix f;
    int s = rd_setup(&f, N_REG);
    if (s == 1) { UTEST_SKIP("not a readiness loop"); }
    ASSERT_EQ(s, 0);
    for (int i = 0; i < N_REG; i++) ASSERT_EQ(rd_register(&f, i), 0);

    const int bound = rounds_bound(N_REG, MAXEV);
    int round = 0;
    while (round < bound && !all_live_served()) {
        ++round;
        ASSERT_EQ(wait_round(&f.loop, round), MAXEV);   /* all ready: every wait is cut off at max */
    }
    EXPECT_EQ(count_unserved(0, N_REG), 0);
    EXPECT_LE(round, bound);
    /* Keep going: the service stays even, not merely complete once. */
    for (int r = 0; r < 3 * bound; r++) ASSERT_EQ(wait_round(&f.loop, ++round), MAXEV);
    int lo = g_served[0], hi = g_served[0];
    for (int i = 1; i < N_REG; i++) {
        if (g_served[i] < lo) lo = g_served[i];
        if (g_served[i] > hi) hi = g_served[i];
    }
    EXPECT_LE(hi - lo, 1);
    rd_teardown(&f);
}

/* Registrations removed between waits and in the middle of a batch (both ones reported in the batch
 * being dispatched and ones not reached yet), with the compaction that follows: no remaining
 * registration loses its turn (strict round-robin) and a removed one is never reported again. */
UTEST(event_fairness, readiness_removals_during_a_batch_keep_round_robin) {
    RdFix f;
    int s = rd_setup(&f, N_REG);
    if (s == 1) { UTEST_SKIP("not a readiness loop"); }
    ASSERT_EQ(s, 0);
    for (int i = 0; i < N_REG; i++) ASSERT_EQ(rd_register(&f, i), 0);

    int round = 0;
    for (int pass = 0; pass < 6; pass++) {
        KlEvent ev[MAXEV];
        int n = kl_event_wait(&f.loop, ev, MAXEV, 1000);
        ASSERT_GT(n, 0);
        ++round;
        for (int k = 0; k < n; k++) {
            const int i = *(const int *)ev[k].udata;
            ASSERT_TRUE(i >= 0 && i < N_REG);
            if (!g_live[i]) continue;   /* removed earlier in this batch: the consumer skips it */
            serve(&f.loop, i, round);
            if (k == 3 || k == n / 2) {
                /* Mid-batch: remove a registration later in this batch, one reported already, and
                 * one not reached by the scan yet (the highest live index, near the array's end). */
                const int later = *(const int *)ev[n - 1].udata;
                const int earlier = *(const int *)ev[0].udata;
                int unreached = -1;
                for (int j = N_REG - 1; j >= 0; j--)
                    if (g_live[j] && j != later && j != earlier && j != i) { unreached = j; break; }
                const int victims[3] = { later, earlier, unreached };
                for (int v = 0; v < 3; v++) {
                    const int x = victims[v];
                    if (x < 0 || x == i || !g_live[x]) continue;
                    ASSERT_EQ(kl_event_del(&f.loop, g_end[x]), 0);
                    g_live[x] = 0;
                }
            }
        }
    }
    EXPECT_EQ(g_rr_violation, 0);
    /* Every survivor is served again within the bound for what is left (counted from now, mid-lap:
     * the joined bound), and the counts stay round-robin. */
    static int base[N_REG];
    for (int i = 0; i < N_REG; i++) base[i] = g_served[i];
    const int left = count_live();
    const int bound = joined_bound(left, MAXEV);
    int r = 0, again = 0;
    while (r < bound && !again) {
        ++r;
        KlEvent ev[MAXEV];
        int n = kl_event_wait(&f.loop, ev, MAXEV, 1000);
        ASSERT_GT(n, 0);
        for (int k = 0; k < n; k++) {
            const int i = *(const int *)ev[k].udata;
            EXPECT_TRUE(g_live[i]);          /* a removed registration is never reported again */
            if (g_live[i]) serve(&f.loop, i, r);
        }
        again = 1;
        for (int i = 0; i < N_REG; i++)
            if (g_live[i] && g_served[i] == base[i]) again = 0;
    }
    EXPECT_TRUE(again);
    EXPECT_LE(r, bound);
    EXPECT_EQ(g_rr_violation, 0);
    rd_teardown(&f);
}

/* Registrations added mid-lap are served within the joined bound for the new total, and a mass
 * removal (the swap compaction moving most of the survivors) still reports every survivor. */
UTEST(event_fairness, readiness_additions_and_mass_removal) {
    RdFix f;
    int s = rd_setup(&f, N_REG);
    if (s == 1) { UTEST_SKIP("not a readiness loop"); }
    ASSERT_EQ(s, 0);
    const int first = 150;
    for (int i = 0; i < first; i++) ASSERT_EQ(rd_register(&f, i), 0);
    int round = 0;
    ASSERT_EQ(wait_round(&f.loop, ++round), MAXEV);   /* mid-lap */
    for (int i = first; i < N_REG; i++) ASSERT_EQ(rd_register(&f, i), 0);
    const int added_at = round;
    const int bound = joined_bound(N_REG, MAXEV);
    while (round - added_at < bound && !all_live_served())
        ASSERT_GT(wait_round(&f.loop, ++round), 0);
    EXPECT_EQ(count_unserved(0, N_REG), 0);   /* the late ones too */
    EXPECT_LE(round - added_at, bound);

    /* Two more waits, then keep only 40. */
    ASSERT_GT(wait_round(&f.loop, ++round), 0);
    ASSERT_GT(wait_round(&f.loop, ++round), 0);
    for (int i = 0; i < N_REG; i++) {
        if (i % 5 == 0) continue;   /* 40 survivors, spread over the old slot range */
        ASSERT_EQ(kl_event_del(&f.loop, g_end[i]), 0);
        g_live[i] = 0;
    }
    for (int i = 0; i < N_REG; i++) g_served[i] = 0;
    /* 40 <= max: one wait reports every survivor. */
    ASSERT_EQ(wait_round(&f.loop, ++round), count_live());
    EXPECT_TRUE(all_live_served());
    /* Then down to one: still reported. */
    for (int i = 0; i < N_REG; i++)
        if (g_live[i] && i != 0) { ASSERT_EQ(kl_event_del(&f.loop, g_end[i]), 0); g_live[i] = 0; }
    g_served[0] = 0;
    ASSERT_EQ(wait_round(&f.loop, ++round), 1);
    EXPECT_EQ(g_served[0], 1);
    rd_teardown(&f);
}

/* ── Connection churn ────────────────────────────────────────────────────────────────────────
 *
 * The load a Connection: close server sees: every wait, each reported registration is removed (and its
 * socket closed) and a new, ready one is added; every fifth original registration is long-lived and
 * stays. Every registration present must be served within joined_bound waits of joining or of its
 * last service. A rule that let a new registration jump ahead of the ones already waiting (an earlier
 * fix moved the last slot to the front of the scan on removal) starved the long-lived ones again,
 * exactly as the original first-64 scan did. */

#define CH_SLOTS 1400   /* N_REG + 64 per round, with room */
#define CH_ROUNDS 16
static KlSocketHandle c_end[CH_SLOTS], c_peer[CH_SLOTS];
static int c_mark[CH_SLOTS], c_since[CH_SLOTS], c_alive[CH_SLOTS], c_long[CH_SLOTS], c_hit[CH_SLOTS];
static int c_next, c_worst;

static int churn_new(void) {
    if (c_next >= CH_SLOTS) return -1;
    const int i = c_next++;
    KlSocketHandle cli = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    if (!kl_handle_valid(cli)) return -1;
    if (kl_sockdef_connect(cli, &g_lis_addr) < 0) { kl_sockdef_close(cli); return -1; }
    KlSocketHandle srv = kl_sockdef_accept(g_lis, NULL);
    if (!kl_handle_valid(srv)) { kl_sockdef_close(cli); return -1; }
    if (kl_sockdef_set_nonblocking(cli) < 0) { kl_sockdef_close(cli); kl_sockdef_close(srv); return -1; }
    if ((int)kl_sockdef_send(srv, "x", 1) != 1) { kl_sockdef_close(cli); kl_sockdef_close(srv); return -1; }
    for (int t = 0; t < 2000; t++) {   /* loopback delivery may lag the send */
        char b;
        if (kl_sock_recv_peek(NULL, cli, &b, 1) == 1) break;
        kl_test_sleep_ms(1);
    }
    c_end[i] = cli; c_peer[i] = srv;
    c_mark[i] = i; c_alive[i] = 1; c_long[i] = 0; c_hit[i] = 0;
    return i;
}

static void churn_close(int i) {
    kl_sockdef_close(c_end[i]);
    kl_sockdef_close(c_peer[i]);
    c_alive[i] = 0;
}

/* After a round: everyone present was served within the bound of joining or of its last service. */
static void churn_check(int round) {
    for (int i = 0; i < c_next; i++)
        if (c_alive[i] && round - c_since[i] > c_worst) c_worst = round - c_since[i];
}

UTEST(event_fairness, readiness_churn_serves_every_registration_within_bound) {
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator a = kl_allocator_default();
    loop.alloc = &a;
    ASSERT_EQ(kl_event_init(&loop), 0);
    if (!is_readiness(&loop)) { kl_event_close(&loop); UTEST_SKIP("not a readiness loop"); }
    ASSERT_EQ(listener_open(), 0);
    c_next = 0; c_worst = 0;
    for (int k = 0; k < N_REG; k++) {
        const int i = churn_new();
        ASSERT_EQ(i, k);
        c_long[i] = (i % 5 == 0);
        c_since[i] = 0;
        ASSERT_EQ(kl_event_add(&loop, c_end[i], KL_EVENT_READ, &c_mark[i]), 0);
    }
    for (int round = 1; round <= CH_ROUNDS; round++) {
        KlEvent ev[MAXEV];
        const int n = kl_event_wait(&loop, ev, MAXEV, 1000);
        ASSERT_GT(n, 0);
        for (int k = 0; k < n; k++) {
            const int i = *(const int *)ev[k].udata;
            ASSERT_TRUE(i >= 0 && i < c_next && c_alive[i]);
            c_since[i] = round;
            c_hit[i]++;
            if (c_long[i]) {
                (void)kl_event_mod(&loop, c_end[i], KL_EVENT_READ, &c_mark[i]);   /* re-arm, as dispatch does */
                continue;
            }
            ASSERT_EQ(kl_event_del(&loop, c_end[i]), 0);   /* the connection closes after one request */
            churn_close(i);
            const int j = churn_new();                     /* and a new one arrives */
            ASSERT_GE(j, 0);
            c_since[j] = round;
            ASSERT_EQ(kl_event_add(&loop, c_end[j], KL_EVENT_READ, &c_mark[j]), 0);
        }
        churn_check(round);
    }
    int long_unserved = 0;
    for (int i = 0; i < N_REG; i++) long_unserved += c_long[i] && c_hit[i] < 2;
    EXPECT_EQ(long_unserved, 0);   /* each long-lived one came round again, not just once */
    EXPECT_LE(c_worst, joined_bound(N_REG, MAXEV));
    for (int i = 0; i < c_next; i++)
        if (c_alive[i]) { kl_event_del(&loop, c_end[i]); churn_close(i); }
    listener_close();
    kl_event_close(&loop);
}

/* The same churn through watchers (kl_watcher_add / kl_watcher_del around kl_event_ctx_run), so it
 * runs on every backend, the completion engines' watcher relay and pollcomp's watch scan included. */
static int c_round;
static int c_reported[MAXEV * 2], c_nreported;
static void on_churn(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)fd; (void)ready;
    const int i = *(const int *)ud;
    if (i < 0 || i >= c_next || !c_alive[i]) return;
    c_since[i] = c_round;
    c_hit[i]++;
    if (!c_long[i] && c_nreported < (int)(sizeof c_reported / sizeof c_reported[0]))
        c_reported[c_nreported++] = i;   /* closed after the batch, as a server closes after replying */
}

UTEST(event_fairness, watchers_churn_serves_every_watcher_within_bound) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &a), 0);
    ASSERT_EQ(listener_open(), 0);
    c_next = 0; c_worst = 0;
    for (int k = 0; k < N_REG; k++) {
        const int i = churn_new();
        ASSERT_EQ(i, k);
        c_long[i] = (i % 5 == 0);
        c_since[i] = 0;
        ASSERT_EQ(kl_watcher_add(&ctx, c_end[i], KL_EVENT_READ, on_churn, &c_mark[i]), 0);
    }
    for (c_round = 1; c_round <= CH_ROUNDS; c_round++) {
        c_nreported = 0;
        ASSERT_GE(kl_event_ctx_run(&ctx, MAXEV, 1000), 0);
        for (int k = 0; k < c_nreported; k++) {
            const int i = c_reported[k];
            kl_watcher_del(&ctx, c_end[i]);
            churn_close(i);
            const int j = churn_new();
            ASSERT_GE(j, 0);
            c_since[j] = c_round;
            ASSERT_EQ(kl_watcher_add(&ctx, c_end[j], KL_EVENT_READ, on_churn, &c_mark[j]), 0);
        }
        churn_check(c_round);
    }
    int long_unserved = 0;
    for (int i = 0; i < N_REG; i++) long_unserved += c_long[i] && c_hit[i] < 2;
    EXPECT_EQ(long_unserved, 0);
    EXPECT_LE(c_worst, joined_bound(N_REG, MAXEV));
    for (int i = 0; i < c_next; i++)
        if (c_alive[i]) { kl_watcher_del(&ctx, c_end[i]); churn_close(i); }
    kl_event_ctx_free(&ctx);
    listener_close();
}

/* ── Watchers: kl_event_ctx_run, on every backend ──────────────────────────────────────────── */

static int g_round;
static void on_watch(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)fd; (void)ready;
    const int i = *(const int *)ud;
    if (i >= 0 && i < N_REG && g_live[i]) note_service(i, g_round);   /* nothing consumed: stays ready */
}

/* Continuously ready watchers, more than one tick's worth: every one is dispatched within the bound.
 * On the pollcomp double the watch scan restarted at the list head each drain, the same starvation. */
UTEST(event_fairness, watchers_every_ready_watcher_is_dispatched_within_bound) {
    KlAllocator a = kl_allocator_default();
    KlEventCtx ctx;
    ASSERT_EQ(kl_event_ctx_init(&ctx, &a), 0);
    ASSERT_EQ(listener_open(), 0);
    reset_accounting();
    for (int i = 0; i < N_REG; i++) {
        ASSERT_EQ(make_pair(i), 0);
        ASSERT_EQ(kl_watcher_add(&ctx, g_end[i], KL_EVENT_READ, on_watch, &g_mark[i]), 0);
        ASSERT_EQ(make_ready(i, 1), 0);
        ASSERT_EQ(await_readable(i), 0);
        g_live[i] = 1;
    }
    const int bound = rounds_bound(N_REG, MAXEV);
    g_round = 0;
    while (g_round < bound && !all_live_served()) {
        ++g_round;
        ASSERT_GE(kl_event_ctx_run(&ctx, MAXEV, 1000), 0);
    }
    EXPECT_EQ(count_unserved(0, N_REG), 0);
    EXPECT_LE(g_round, bound);
    for (int i = 0; i < N_REG; i++) { kl_watcher_del(&ctx, g_end[i]); g_live[i] = 0; }
    kl_event_ctx_free(&ctx);
    for (int i = 0; i < N_REG; i++) close_pair(i);
    listener_close();
}

/* ── Posted operations: the completion engines ─────────────────────────────────────────────── */

static KlStream g_st[N_REG];
static char     g_rbuf[N_REG];
static int      g_repost_fail;

static int stream_index(const KlStream *s) {
    const ptrdiff_t d = s - g_st;
    return (d >= 0 && d < N_REG) ? (int)d : -1;
}

/* A read completed: note it and post the next one-byte receive at once, so the stream (which has
 * plenty of unread bytes) stays continuously ready. */
static void on_comp(struct KlEventCtx *ctx, const void *evp) {
    (void)ctx;
    const KlCompletionEvent *ev = evp;
    if (ev->kind != KL_COMP_READ) return;
    KlStream *s = (KlStream *)ev->target;
    const int i = stream_index(s);
    if (i < 0 || !g_live[i]) return;
    note_service(i, g_round);
    if (kl_comp_post_recv_raw(s, &g_rbuf[i], 1) != 0) g_repost_fail = 1;
}

typedef struct { KlEventCtx ctx; KlAllocator a; } CompFix;

static int comp_setup(CompFix *f) {
    memset(f, 0, sizeof(*f));
    f->a = kl_allocator_default();
    if (kl_event_ctx_init(&f->ctx, &f->a) != 0) return -1;
    if (!(kl_event_caps(&f->ctx.loop) & KL_EVENT_CAP_COMPLETION)) { kl_event_ctx_free(&f->ctx); return 1; }
    f->ctx.comp_conn_dispatch = on_comp;
    if (listener_open() < 0) return -1;
    reset_accounting();
    g_repost_fail = 0;
    for (int i = 0; i < N_REG; i++) {
        if (make_pair(i) < 0) return -1;
        memset(&g_st[i], 0, sizeof(g_st[i]));
        g_st[i].fd = g_end[i];
        g_st[i].ctx = &f->ctx;
        g_st[i].alloc = &f->a;
        /* Associate the fd as the completion server does at accept (CreateIoCompletionPort on IOCP). */
        if (kl_event_add(&f->ctx.loop, g_end[i], KL_EVENT_READ, &g_st[i]) != 0) return -1;
        if (make_ready(i, 200) != 0) return -1;   /* enough bytes for every round below */
        if (await_readable(i) != 0) return -1;
    }
    for (int i = 0; i < N_REG; i++) {
        if (kl_comp_post_recv_raw(&g_st[i], &g_rbuf[i], 1) != 0) return -1;
        g_live[i] = 1;
    }
    return 0;
}

/* Close every pair (aborting its outstanding receive) and drain until the aborted completions are
 * delivered, then free the loop. */
static void comp_teardown(CompFix *f) {
    for (int i = 0; i < N_REG; i++) g_live[i] = 0;
    for (int i = 0; i < N_REG; i++) {
        kl_comp_cancel(&f->ctx, g_end[i]);
        kl_event_del(&f->ctx.loop, g_end[i]);
    }
    for (int i = 0; i < 20; i++) (void)kl_event_ctx_run(&f->ctx, MAXEV, 5);
    kl_event_ctx_free(&f->ctx);
    for (int i = 0; i < N_REG; i++) close_pair(i);
    listener_close();
}

/* Continuously ready posted receives, more than one drain's worth: every stream's read completes
 * within the bound. On the pollcomp double ops were taken newest first, so the re-posted receives of
 * the first 64 streams won every drain and the other 136 never completed. */
UTEST(event_fairness, ops_every_ready_receive_completes_within_bound) {
    CompFix f;
    int s = comp_setup(&f);
    if (s == 1) { UTEST_SKIP("not a completion loop"); }
    ASSERT_EQ(s, 0);
    const int bound = rounds_bound(N_REG, MAXEV);
    g_round = 0;
    while (g_round < bound && !all_live_served()) {
        ++g_round;
        ASSERT_GE(kl_event_ctx_run(&f.ctx, MAXEV, 1000), 0);
    }
    EXPECT_EQ(count_unserved(0, N_REG), 0);
    EXPECT_LE(g_round, bound);
    EXPECT_EQ(g_repost_fail, 0);
    comp_teardown(&f);
}

/* A watcher that becomes ready behind a full drain's worth of ready operations is dispatched within
 * the bound. On the pollcomp double the ops were always taken first, so a busy server starved its
 * own thread-pool wakeup. */
static int g_watch_round;
static void on_wake(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)fd; (void)ready; (void)ud;
    if (!g_watch_round) g_watch_round = g_round;
}

UTEST(event_fairness, ops_do_not_starve_a_watcher) {
    CompFix f;
    int s = comp_setup(&f);
    if (s == 1) { UTEST_SKIP("not a completion loop"); }
    ASSERT_EQ(s, 0);
    KlSocketHandle wa, wb;
    {
        KlSocketHandle cli = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_TRUE(kl_handle_valid(cli));
        ASSERT_EQ(kl_sockdef_connect(cli, &g_lis_addr), 0);
        KlSocketHandle srv = kl_sockdef_accept(g_lis, NULL);
        ASSERT_TRUE(kl_handle_valid(srv));
        ASSERT_EQ(kl_sockdef_set_nonblocking(cli), 0);
        wa = cli; wb = srv;
    }
    g_watch_round = 0;
    g_round = 0;
    ASSERT_GT(kl_event_ctx_run(&f.ctx, MAXEV, 1000), 0);   /* the ops are busy before the watcher exists */
    ASSERT_EQ(kl_watcher_add(&f.ctx, wa, KL_EVENT_READ, on_wake, NULL), 0);
    ASSERT_EQ((int)kl_sockdef_send(wb, "w", 1), 1);
    const int bound = rounds_bound(N_REG + 1, MAXEV);
    while (g_round < bound && !g_watch_round) {
        ++g_round;
        ASSERT_GE(kl_event_ctx_run(&f.ctx, MAXEV, 1000), 0);
    }
    EXPECT_TRUE(g_watch_round > 0);
    EXPECT_LE(g_round, bound);
    kl_watcher_del(&f.ctx, wa);
    kl_sockdef_close(wa);
    kl_sockdef_close(wb);
    comp_teardown(&f);
}

UTEST_MAIN();
