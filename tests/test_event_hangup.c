/*
 * test_event_hangup.c: the KL_EVENT_CAP_HANGUP guarantee, proved per backend rather than assumed
 * from the OS.
 *
 * On a loop that advertises KL_EVENT_CAP_HANGUP, a watcher with KL_EVENT_HANGUP interest (and no READ
 * interest) on the server end of a loopback TCP connection must:
 *   - report a FIN, including one behind unread bytes, as HANGUP without ERROR;
 *   - report a reset (SO_LINGER 0) as HANGUP | ERROR;
 *   - never report readable data, and never consume it (the bytes are still there afterwards);
 *   - never report ERROR without HANGUP.
 * And a READ registration must be reported exactly as before (READ, no HANGUP/ERROR bits).
 * The callback removes the watcher on its first report, as the HTTP server does, so a
 * level-triggered backend does not re-report. On a loop without the capability every case skips.
 * TCP, not a socketpair: AF_UNIX has no reset.
 */
#include "utest.h"
#include <keel/keel.h>
#include <string.h>
#include <stdint.h>
#include <keel/clock.h>
#include "net_compat.h"
#include "../src/event_caps.h"   /* kl_event_caps */

#define REQ "GET / HTTP/1.1\r\nHost: x\r\n\r\n"

/* A connected loopback TCP pair; *srv is non-blocking. 0 on success. */
static int tcp_pair(int *srv, int *cli) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int l = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (l < 0) return -1;
    socklen_t al = (socklen_t)sizeof a;
    if (bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 1) != 0 ||
        getsockname(l, (struct sockaddr *)&a, &al) != 0) { kl_test_closesock(l); return -1; }
    int c = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0 || connect(c, (struct sockaddr *)&a, sizeof a) != 0) { kl_test_closesock(l); return -1; }
    int s = (int)accept(l, NULL, NULL);
    kl_test_closesock(l);
    if (s < 0) { kl_test_closesock(c); return -1; }
    kl_test_set_nonblock(s);
    *srv = s; *cli = c;
    return 0;
}

static void reset_close(int c) {
    struct linger lg;
    lg.l_onoff = 1;
    lg.l_linger = 0;
    (void)setsockopt(c, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof lg);
    kl_test_closesock(c);
}

typedef struct {
    KlEventCtx *ctx;
    int calls;
    KlEventMask ready;
    int del_on_report;
} Watch;

static void on_ready(KlSocketHandle fd, KlEventMask ready, void *ud) {
    Watch *w = ud;
    w->calls++;
    w->ready = ready;
    if (w->del_on_report) kl_watcher_del(w->ctx, fd);
}

/* Run the loop until the watcher reported or `ticks` 20 ms ticks passed. */
static void run_until(KlEventCtx *ev, const Watch *w, int ticks) {
    for (int i = 0; i < ticks && w->calls == 0; i++) (void)kl_event_ctx_run(ev, 16, 20);
}

enum { SC_FIN, SC_DATA_FIN, SC_RST, SC_DATA_RST };

/* One scenario on a fresh loop and pair: returns the first report (0 = none) and whether the
 * request bytes were still readable afterwards (FIN cases). */
static int scenario(int sc, KlEventMask *ready, int *calls, int *bytes_left) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    if (kl_event_ctx_init(&ev, &alloc) != 0) return -1;
    if (!(kl_event_caps(&ev.loop) & KL_EVENT_CAP_HANGUP)) { kl_event_ctx_free(&ev); return 1; }
    int s, c;
    if (tcp_pair(&s, &c) != 0) { kl_event_ctx_free(&ev); return -1; }
    Watch w = { &ev, 0, 0, 1 };
    if (kl_watcher_add(&ev, s, KL_EVENT_HANGUP, on_ready, &w) != 0) {
        kl_test_closesock(s); kl_test_closesock(c); kl_event_ctx_free(&ev); return -1;
    }
    if (sc == SC_DATA_FIN || sc == SC_DATA_RST) (void)kl_test_sockwrite(c, REQ, sizeof REQ - 1);
    if (sc == SC_RST || sc == SC_DATA_RST) reset_close(c); else kl_test_closesock(c);
    run_until(&ev, &w, 50);
    *ready = w.ready;
    *calls = w.calls;
    char buf[64];
    *bytes_left = (int)kl_test_sockread(s, buf, sizeof buf);
    if (w.calls == 0) kl_watcher_del(&ev, s);
    kl_test_closesock(s);
    kl_event_ctx_free(&ev);
    return 0;
}

UTEST(event_hangup, fin_reported_without_error) {
    KlEventMask r; int calls, left;
    int rc = scenario(SC_FIN, &r, &calls, &left);
    if (rc == 1) UTEST_SKIP("loop has no KL_EVENT_CAP_HANGUP");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(calls, 1);
    ASSERT_EQ((int)r, (int)KL_EVENT_HANGUP);
}

UTEST(event_hangup, fin_behind_unread_bytes_reported_and_bytes_kept) {
    KlEventMask r; int calls, left;
    int rc = scenario(SC_DATA_FIN, &r, &calls, &left);
    if (rc == 1) UTEST_SKIP("loop has no KL_EVENT_CAP_HANGUP");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(calls, 1);
    ASSERT_EQ((int)r, (int)KL_EVENT_HANGUP);
    ASSERT_EQ(left, (int)(sizeof REQ - 1));   /* nothing was consumed */
}

UTEST(event_hangup, reset_reported_with_error) {
    KlEventMask r; int calls, left;
    int rc = scenario(SC_RST, &r, &calls, &left);
    if (rc == 1) UTEST_SKIP("loop has no KL_EVENT_CAP_HANGUP");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(calls, 1);
    ASSERT_EQ((int)r, (int)(KL_EVENT_HANGUP | KL_EVENT_ERROR));
}

UTEST(event_hangup, reset_behind_unread_bytes_reported_with_error) {
    KlEventMask r; int calls, left;
    int rc = scenario(SC_DATA_RST, &r, &calls, &left);
    if (rc == 1) UTEST_SKIP("loop has no KL_EVENT_CAP_HANGUP");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(calls, 1);
    ASSERT_EQ((int)r, (int)(KL_EVENT_HANGUP | KL_EVENT_ERROR));
}

UTEST(event_hangup, pipelined_bytes_never_reported_nor_consumed) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    if (!(kl_event_caps(&ev.loop) & KL_EVENT_CAP_HANGUP)) {
        kl_event_ctx_free(&ev);
        UTEST_SKIP("loop has no KL_EVENT_CAP_HANGUP");
    }
    int s, c;
    ASSERT_EQ(tcp_pair(&s, &c), 0);
    Watch w = { &ev, 0, 0, 0 };   /* not deleted on report: any report at all is a failure */
    ASSERT_EQ(kl_watcher_add(&ev, s, KL_EVENT_HANGUP, on_ready, &w), 0);
    (void)kl_test_sockwrite(c, REQ, sizeof REQ - 1);
    for (int i = 0; i < 10; i++) (void)kl_event_ctx_run(&ev, 16, 20);
    EXPECT_EQ(w.calls, 0);
    char buf[64];
    EXPECT_EQ((int)kl_test_sockread(s, buf, sizeof buf), (int)(sizeof REQ - 1));
    kl_watcher_del(&ev, s);
    kl_test_closesock(s);
    kl_test_closesock(c);
    kl_event_ctx_free(&ev);
}

UTEST(event_hangup, read_registration_unchanged_on_fin) {
    /* Runs on every backend: a READ watcher still sees exactly READ for a FIN. */
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    int s, c;
    ASSERT_EQ(tcp_pair(&s, &c), 0);
    Watch w = { &ev, 0, 0, 1 };
    ASSERT_EQ(kl_watcher_add(&ev, s, KL_EVENT_READ, on_ready, &w), 0);
    kl_test_closesock(c);
    run_until(&ev, &w, 50);
    EXPECT_EQ(w.calls, 1);
    EXPECT_EQ((int)w.ready, (int)KL_EVENT_READ);
    if (w.calls == 0) kl_watcher_del(&ev, s);
    kl_test_closesock(s);
    kl_event_ctx_free(&ev);
}

UTEST(event_hangup, any_udata_cookie_keeps_its_registration_kind) {
    /* The backend decides a registration's kind from the interest it registered, never from the
     * caller's udata: kl_event_add is public and takes any cookie, including ones with low bits set.
     * A READ registration with cookie 6 still gets READ (and its cookie back); a hang-up registration
     * with cookie 10 gets HANGUP (and its cookie back). Raw kl_event_add/kl_event_wait, as a library
     * user would; readiness loops only (a completion loop has no kl_event_wait). */
    KlEventLoop loop;
    memset(&loop, 0, sizeof loop);
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);
    unsigned caps = kl_event_caps(&loop);
    if (caps & KL_EVENT_CAP_COMPLETION) { kl_event_close(&loop); UTEST_SKIP("completion loop"); }
    int rs, rc, hs, hc;
    ASSERT_EQ(tcp_pair(&rs, &rc), 0);
    ASSERT_EQ(tcp_pair(&hs, &hc), 0);
    void *read_cookie = (void *)(uintptr_t)6;   /* bits 1 and 2 set */
    void *hup_cookie = (void *)(uintptr_t)10;   /* bits 1 and 3 set */
    ASSERT_EQ(kl_event_add(&loop, rs, KL_EVENT_READ, read_cookie), 0);
    int hup = (caps & KL_EVENT_CAP_HANGUP) != 0;
    if (hup) ASSERT_EQ(kl_event_add(&loop, hs, KL_EVENT_HANGUP, hup_cookie), 0);
    (void)kl_test_sockwrite(rc, "x", 1);
    kl_test_closesock(hc);
    int got_read = 0, got_hup = 0, other = 0;
    for (int i = 0; i < 20 && (!got_read || (hup && !got_hup)); i++) {
        KlEvent ev[8];
        int n = kl_event_wait(&loop, ev, 8, 20);
        for (int k = 0; k < n; k++) {
            if (ev[k].udata == read_cookie && ev[k].ready == KL_EVENT_READ) got_read = 1;
            else if (ev[k].udata == hup_cookie && ev[k].ready == KL_EVENT_HANGUP) got_hup = 1;
            else other++;
        }
    }
    EXPECT_EQ(got_read, 1);
    EXPECT_EQ(got_hup, hup);
    EXPECT_EQ(other, 0);
    kl_event_del(&loop, rs);
    if (hup) kl_event_del(&loop, hs);
    kl_test_closesock(rs); kl_test_closesock(rc); kl_test_closesock(hs);
    kl_event_close(&loop);
}

/* ── Write readiness is independent of the peer's half-close ──────────────────────────────────────
 * A peer that half-closes (shutdown of its sending direction) still reads, so a writer waiting for
 * room must get it: TCP is full duplex. With the server's send path full and the peer not reading, a
 * WRITE registration must report nothing (and the loop must not spin); once the peer reads, it must
 * report WRITE. Every readiness backend. WSAPoll could not do this (after a FIN it reports POLLHUP on
 * every call, so the wait returned at once with a bare READ); the Windows readiness backend is
 * select() for this reason. */
static void half_close(int c) {
#if defined(_WIN32)
    (void)shutdown((SOCKET)c, SD_SEND);
#else
    (void)shutdown(c, SHUT_WR);
#endif
}

typedef struct { int fd; int calls; int spurious; int others; long sent; } HcWriter;
static char g_hc_buf[16384];
static void hc_fill(HcWriter *w) {                          /* write until the send path is full */
    for (int i = 0; i < 100000; i++) {
        long n = kl_test_sockwrite(w->fd, g_hc_buf, sizeof g_hc_buf);
        if (n <= 0) break;
        w->sent += n;
    }
}
static void hc_on_ready(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)fd;
    HcWriter *w = ud;
    w->calls++;
    if (!(ready & KL_EVENT_WRITE)) { w->others++; return; }
    long before = w->sent;
    hc_fill(w);
    if (w->sent == before) w->spurious++;                   /* reported writable, but nothing went */
}

UTEST(event_hangup, write_readiness_survives_a_peer_half_close) {
    KlAllocator alloc = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &alloc), 0);
    if (kl_event_caps(&ev.loop) & KL_EVENT_CAP_COMPLETION) { kl_event_ctx_free(&ev); UTEST_SKIP("completion loop"); }
    int s, c;
    ASSERT_EQ(tcp_pair(&s, &c), 0);
    int rb = 4096, sb = 8192;
    (void)setsockopt(c, SOL_SOCKET, SO_RCVBUF, (const char *)&rb, sizeof rb);
    (void)setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&sb, sizeof sb);
    memset(g_hc_buf, 'w', sizeof g_hc_buf);
    HcWriter w = { s, 0, 0, 0, 0 };
    hc_fill(&w);
    ASSERT_TRUE(w.sent > 0);
    half_close(c);
    kl_test_sleep_ms(100);
    ASSERT_EQ(kl_watcher_add(&ev, s, KL_EVENT_WRITE, hc_on_ready, &w), 0);

    for (int i = 0; i < 6; i++) kl_event_ctx_run(&ev, 8, 50);   /* about 300 ms, peer not reading */
    EXPECT_EQ(w.others, 0);                                 /* no bare READ for the FIN */
    EXPECT_EQ(w.spurious, 0);                               /* every WRITE report had room behind it */
    EXPECT_TRUE_MSG(w.calls <= 6, "the loop kept reporting the half-closed socket: a spin");
    const long sent_before_read = w.sent;

    kl_test_set_nonblock(c);                                /* the peer reads everything */
    char rd[65536];
    long got = 0;
    for (int i = 0; i < 400 && (got < w.sent || w.sent == sent_before_read); i++) {
        long n = kl_test_sockread(c, rd, sizeof rd);
        if (n > 0) got += n;
        else kl_event_ctx_run(&ev, 8, 10);                  /* the writer refills as room appears */
    }
    EXPECT_TRUE(w.sent > sent_before_read);                 /* writable again: the writer went on */
    EXPECT_EQ(w.spurious, 0);
    kl_watcher_del(&ev, s);
    kl_event_ctx_free(&ev);
    kl_test_closesock(s);
    kl_test_closesock(c);
}

UTEST_MAIN();
