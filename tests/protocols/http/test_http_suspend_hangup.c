/* test_http_suspend_hangup.c: a client hang-up while the server connection is suspended.
 *
 * A real KlHttpServer runs in a thread; the test is the client. GET /park suspends with an op that
 * a test-controlled wakeup completes (GET /fast answers at once). Covered, per policy:
 *   - the client closes, half-closes or resets while suspended;
 *   - CANCEL (default): on_cancel once, nothing written, the connection closed;
 *   - RESET_ONLY: a half-close keeps the op and its response is delivered; a reset cancels;
 *   - OFF: nothing is watched;
 *   - a request arriving while suspended is neither consumed nor reported as a hang-up, and is
 *     served after the resume;
 *   - a resume racing a hang-up ends the op exactly once;
 *   - kl_http_server_free with suspended ops after hang-ups.
 * Detection is prompt on a loop with KL_EVENT_CAP_HANGUP and within the 1 s sweep elsewhere; the
 * waits below allow for the sweep. TCP, so a reset (SO_LINGER 0) is real.
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/async.h>
#include <keel/clock.h>
#include <string.h>
#include "net_compat.h"
#include "platform_thread.h"
#include "kl_atomic.h"
#include "mock_tls.h"         /* identity TLS (no crypto), readiness and completion */
#include "http_conn_internal.h"   /* KlHttpConn.hup_state: proves detection happened (or not) */
#include <keel/thread_pool.h>
#include "thread_pool_internal.h"   /* keel__thread_pool_shutting_down: order the gate after shutdown */
#include "event_builtin.h"   /* the built-in backend, wrapped below without KL_EVENT_CAP_HANGUP */
#include "event_caps.h"
#include "socket.h"
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

/* ── shared state (the server thread writes, the test thread polls) ── */

static KlHttpServer srv;
static KlPlatThread srv_tid;

typedef struct {
    KlAsyncOp op;
    int wake[2];            /* test writes wake[1]; the watcher on wake[0] completes the op */
    int in_use;
} Park;

#define MAX_PARK 8
static Park parks[MAX_PARK];
static int n_parked;        /* suspended so far (atomic) */
static int n_cancel;        /* on_cancel calls (atomic) */
static int n_resume;        /* on_resume calls (atomic) */

static int ld(int *p) { return kl_atomic_load_int(p); }
static void inc(int *p) { kl_atomic_store_int(p, kl_atomic_load_int(p) + 1); }   /* server thread only */

static void park_resume(KlAsyncOp *op, void *ud) {
    (void)ud;
    inc(&n_resume);
    kl_http_response_json(kl_http_conn_response(op->conn), 200, "{\"r\":\"resumed\"}", 14);
}

static void park_cancel(KlAsyncOp *op, void *ud) {
    (void)op;
    Park *p = ud;
    inc(&n_cancel);
    kl_watcher_del(&srv.ev, p->wake[0]);
    kl_test_closesock(p->wake[0]);
    kl_test_closesock(p->wake[1]);
}

static void park_wake(KlSocketHandle fd, KlEventMask ready, void *ud) {
    (void)ready;
    Park *p = ud;
    char b[8];
    (void)kl_test_sockread(fd, b, sizeof b);
    kl_watcher_del(&srv.ev, fd);
    kl_async_complete(&srv, &p->op);   /* a no-op if the op was already cancelled */
    kl_test_closesock(p->wake[0]);
    kl_test_closesock(p->wake[1]);
}

static void handle_park(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)res; (void)ud;
    int i = ld(&n_parked);
    Park *p = &parks[i];
    memset(p, 0, sizeof *p);
    p->in_use = 1;
    p->op.on_resume = park_resume;
    p->op.on_cancel = park_cancel;
    p->op.user_data = p;
    kl_test_socketpair(p->wake);
    kl_test_set_nonblock(p->wake[0]);
    kl_watcher_add(&srv.ev, p->wake[0], KL_EVENT_READ, park_wake, p);
    kl_async_suspend(&srv, kl_http_request_conn(req), &p->op);
    inc(&n_parked);
}

static void handle_fast(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{\"r\":\"fast\"}", 11);
}

static void srv_thread(void *arg) { (void)arg; kl_http_server_run(&srv); }

/* ── The sweep, forced on any readiness backend: the built-in backend wrapped by a provider that
 * does not advertise KL_EVENT_CAP_HANGUP, and a socket provider that counts peeks per fd. ── */

static unsigned nohup_caps(const KlEventLoop *l) { return kl_event_caps_builtin(l) & ~KL_EVENT_CAP_HANGUP; }
static const KlEventOps nohup_ops = {
    kl_event_init_builtin, kl_event_add_builtin, kl_event_mod_builtin, kl_event_del_builtin,
    kl_event_wait_builtin, kl_event_close_builtin, nohup_caps, kl_event_native_provider_builtin, NULL,
};
static const KlEventProvider nohup_provider = { &nohup_ops, "builtin-without-hangup" };

static KlTlsConfig tls_cfg;

/* 1 when the built-in loop is a completion loop (the forced-sweep wrapper is readiness-only). */
static int builtin_is_completion(void) {
    KlEventLoop l;
    memset(&l, 0, sizeof l);
    KlAllocator a = kl_allocator_default();
    l.alloc = &a;
    if (kl_event_init(&l) != 0) return 1;
    unsigned caps = kl_event_caps(&l);
    kl_event_close(&l);
    return (caps & KL_EVENT_CAP_COMPLETION) != 0;
}

static void start_ex(KlHttpSuspendHangup policy, int tls, const KlEventProvider *ep) {
    memset(parks, 0, sizeof parks);
    kl_atomic_store_int(&n_parked, 0);
    kl_atomic_store_int(&n_cancel, 0);
    kl_atomic_store_int(&n_resume, 0);
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.port = 0;
    cfg.bind_addr = "127.0.0.1";
    cfg.suspended_hangup = policy;
    cfg.event_provider = ep;
    if (tls) {
        memset(&tls_cfg, 0, sizeof tls_cfg);
        tls_cfg.factory = mock_tls_create;
        cfg.tls = &tls_cfg;
    }
    if (kl_http_server_init(&srv, &cfg) != 0) abort();
    kl_http_server_route(&srv, "GET", "/park", handle_park, NULL, NULL);
    kl_http_server_route(&srv, "GET", "/fast", handle_fast, NULL, NULL);
    kl_plat_thread_create(&srv_tid, srv_thread, NULL);
    for (int i = 0; i < 300 && srv.bound_port == 0; i++) kl_test_sleep_ms(10);
}

static void start_tls(KlHttpSuspendHangup policy, int tls) { start_ex(policy, tls, NULL); }
static void start(KlHttpSuspendHangup policy) { start_ex(policy, 0, NULL); }

/* The suspended connection's hang-up state (read while it is still suspended). */
static int hup_state_of(int i) { return kl_atomic_load_int(&parks[i].op.conn->hup_state); }
static int wait_hup_state(int i, int want, int ms) {
    for (int k = 0; k < ms / 10 && hup_state_of(i) != want; k++) kl_test_sleep_ms(10);
    return hup_state_of(i) == want;
}

static void stop(void) {
    kl_http_server_stop(&srv);
    kl_plat_thread_join(&srv_tid);
    kl_http_server_free(&srv);
}

static int client(void) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)srv.bound_port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { kl_test_closesock(fd); return -1; }
    kl_test_set_rcvtimeo(fd, 4000);
    return fd;
}

static void reset_close(int fd) {
    struct linger lg;
    lg.l_onoff = 1;
    lg.l_linger = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof lg);
    kl_test_closesock(fd);
}

#define PARK_REQ "GET /park HTTP/1.1\r\nHost: x\r\n\r\n"
#define FAST_REQ "GET /fast HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"

/* Wait (up to ms) until *p >= want. */
static int wait_for(int *p, int want, int ms) {
    for (int i = 0; i < ms / 10 && ld(p) < want; i++) kl_test_sleep_ms(10);
    return ld(p) >= want;
}

static void wake(int i) { (void)kl_test_sockwrite(parks[i].wake[1], "!", 1); }

/* Read until EOF or timeout; returns the bytes read (0 = nothing before EOF). */
static long read_all(int fd, char *buf, size_t cap) {
    long t = 0, n;
    while ((size_t)t < cap - 1 && (n = kl_test_sockread(fd, buf + t, cap - 1 - (size_t)t)) > 0) t += n;
    buf[t] = 0;
    return t;
}

#define DETECT_MS 3500   /* prompt on CAP_HANGUP loops; within one 1 s sweep elsewhere */

UTEST(suspend_hangup, close_cancels_once_by_default) {
    start(KL_HTTP_SUSPEND_HANGUP_CANCEL);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    kl_test_closesock(fd);
    EXPECT_TRUE(wait_for(&n_cancel, 1, DETECT_MS));
    kl_test_sleep_ms(200);
    EXPECT_EQ(ld(&n_cancel), 1);
    EXPECT_EQ(ld(&n_resume), 0);
    stop();
    EXPECT_EQ(ld(&n_cancel), 1);   /* server free does not cancel it again */
}

UTEST(suspend_hangup, half_close_cancelled_by_default_nothing_written) {
    start(KL_HTTP_SUSPEND_HANGUP_CANCEL);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    shutdown(fd, 1 /* SHUT_WR / SD_SEND */);
    EXPECT_TRUE(wait_for(&n_cancel, 1, DETECT_MS));
    char buf[256];
    EXPECT_EQ(read_all(fd, buf, sizeof buf), 0);   /* closed with nothing written */
    kl_test_closesock(fd);
    stop();
}

UTEST(suspend_hangup, half_close_kept_under_reset_only) {
    start(KL_HTTP_SUSPEND_HANGUP_RESET_ONLY);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    shutdown(fd, 1);
    EXPECT_TRUE(wait_hup_state(0, KL_HUP_FIN_KEPT, DETECT_MS));   /* the FIN was seen, and kept */
    kl_test_sleep_ms(1500);                 /* past a sweep tick too: no spurious cancel */
    EXPECT_EQ(ld(&n_cancel), 0);
    wake(0);
    char buf[512];
    read_all(fd, buf, sizeof buf);
    EXPECT_TRUE(strstr(buf, "200 OK") != NULL);
    EXPECT_TRUE(strstr(buf, "resumed") != NULL);
    EXPECT_EQ(ld(&n_resume), 1);
    EXPECT_EQ(ld(&n_cancel), 0);
    kl_test_closesock(fd);
    stop();
}

UTEST(suspend_hangup, reset_cancels_under_reset_only) {
    start(KL_HTTP_SUSPEND_HANGUP_RESET_ONLY);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    reset_close(fd);
    EXPECT_TRUE(wait_for(&n_cancel, 1, DETECT_MS));
    EXPECT_EQ(ld(&n_resume), 0);
    stop();
    EXPECT_EQ(ld(&n_cancel), 1);
}

UTEST(suspend_hangup, off_watches_nothing) {
    start(KL_HTTP_SUSPEND_HANGUP_OFF);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    kl_test_closesock(fd);
    kl_test_sleep_ms(1500);
    EXPECT_EQ(hup_state_of(0), KL_HUP_NONE);   /* never watched, neither by watcher nor by sweep */
    EXPECT_EQ(ld(&n_cancel), 0);            /* the behavior before this feature */
    wake(0);
    EXPECT_TRUE(wait_for(&n_resume, 1, 2000));
    stop();
}

UTEST(suspend_hangup, request_arriving_while_suspended_kept_and_served_after_resume) {
    /* The second request is sent only once the first is suspended, so it waits UNREAD in the socket
     * while the hang-up watch is armed: it must neither be reported as a hang-up nor be consumed,
     * and it is served after the resume. (A request already read into Keel's buffer together with
     * the suspended one is a different path, not exercised here.) */
    start(KL_HTTP_SUSPEND_HANGUP_CANCEL);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    (void)kl_test_sockwrite(fd, FAST_REQ, sizeof FAST_REQ - 1);
    kl_test_sleep_ms(1500);
    EXPECT_EQ(ld(&n_cancel), 0);            /* readable bytes are not a hang-up */
    wake(0);
    char buf[1024];
    read_all(fd, buf, sizeof buf);
    EXPECT_TRUE(strstr(buf, "resumed") != NULL);
    EXPECT_TRUE(strstr(buf, "fast") != NULL);   /* the unread request was not consumed */
    kl_test_closesock(fd);
    stop();
}

UTEST(suspend_hangup, resume_racing_hangup_ends_once) {
    start(KL_HTTP_SUSPEND_HANGUP_CANCEL);
    for (int round = 0; round < 4; round++) {
        int fd = client();
        ASSERT_TRUE(fd >= 0);
        (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
        ASSERT_TRUE(wait_for(&n_parked, round + 1, 2000));
        wake(round);                         /* resume and hang-up land in the same moment */
        kl_test_closesock(fd);
        kl_test_sleep_ms(1500);
        EXPECT_EQ(ld(&n_resume) + ld(&n_cancel), round + 1);
    }
    stop();
    EXPECT_EQ(ld(&n_resume) + ld(&n_cancel), 4);
}

UTEST(suspend_hangup, server_free_with_suspended_after_hangups) {
    start(KL_HTTP_SUSPEND_HANGUP_CANCEL);
    int fds[3];
    for (int i = 0; i < 3; i++) {
        fds[i] = client();
        ASSERT_TRUE(fds[i] >= 0);
        (void)kl_test_sockwrite(fds[i], PARK_REQ, sizeof PARK_REQ - 1);
        ASSERT_TRUE(wait_for(&n_parked, i + 1, 2000));
    }
    kl_test_closesock(fds[0]);
    reset_close(fds[1]);
    EXPECT_TRUE(wait_for(&n_cancel, 2, DETECT_MS));
    stop();                                  /* the third is still suspended: free cancels it */
    EXPECT_EQ(ld(&n_cancel), 3);
    kl_test_closesock(fds[2]);
}

UTEST(suspend_hangup, reset_after_kept_half_close_is_not_a_cancel) {
    /* RESET_ONLY watches nothing once a FIN is kept (on Windows a later reset is not even
     * distinguishable from the half-close): a reset then must not cancel the op spuriously. The
     * op runs to its resume; the write to the reset peer just fails. */
    start(KL_HTTP_SUSPEND_HANGUP_RESET_ONLY);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    shutdown(fd, 1);
    EXPECT_TRUE(wait_hup_state(0, KL_HUP_FIN_KEPT, DETECT_MS));   /* the FIN was seen, and kept */
    reset_close(fd);
    kl_test_sleep_ms(1500);
    EXPECT_EQ(ld(&n_cancel), 0);
    wake(0);
    EXPECT_TRUE(wait_for(&n_resume, 1, 2000));
    stop();
    EXPECT_EQ(ld(&n_cancel), 0);
}

#define SWEEP_MAX 500
static KlSocketOps cnt_ops;
static KlSocketProvider cnt_prov;
static kl_ssize_t (*orig_peek)(void *, KlSocketHandle, void *, size_t);
static KlSocketHandle cnt_fd[SWEEP_MAX + 32];
static int cnt_n[SWEEP_MAX + 32];
static int cnt_slots;                                   /* server thread only */
static int cnt_total;
static uint64_t tick_first, tick_last, tick_span_max;   /* server thread only */

static kl_ssize_t cnt_peek(void *ctx, KlSocketHandle fd, void *buf, size_t len) {
    uint64_t now = kl_monotonic_ms();
    if (now - tick_last > 500) tick_first = now;       /* a new sweep tick began */
    tick_last = now;
    if (tick_last - tick_first > tick_span_max) tick_span_max = tick_last - tick_first;
    int i = 0;
    while (i < cnt_slots && cnt_fd[i] != fd) i++;
    if (i == cnt_slots && cnt_slots < SWEEP_MAX + 32) { cnt_fd[i] = fd; cnt_slots++; }
    if (i < cnt_slots) kl_atomic_store_int(&cnt_n[i], kl_atomic_load_int(&cnt_n[i]) + 1);
    kl_atomic_store_int(&cnt_total, kl_atomic_load_int(&cnt_total) + 1);
    return orig_peek(ctx, fd, buf, len);
}

static KlAsyncOp quiet_ops[SWEEP_MAX];
static void quiet_cancel(KlAsyncOp *op, void *ud) { (void)op; (void)ud; inc(&n_cancel); }
static void handle_quiet(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)res; (void)ud;
    KlAsyncOp *op = &quiet_ops[ld(&n_parked)];
    memset(op, 0, sizeof *op);
    op->on_cancel = quiet_cancel;
    kl_async_suspend(&srv, kl_http_request_conn(req), op);
    inc(&n_parked);
}

static int sweep_n(void) {
#if defined(_WIN32)
    return SWEEP_MAX;
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return 50;
    long n = ((long)rl.rlim_cur - 64) / 2;
    return n > SWEEP_MAX ? SWEEP_MAX : (n < 10 ? 10 : (int)n);
#endif
}

UTEST(suspend_hangup, sweep_bounded_on_many_suspended_connections) {
    KlEventLoop probe_loop;
    memset(&probe_loop, 0, sizeof probe_loop);
    KlAllocator a = kl_allocator_default();
    probe_loop.alloc = &a;
    if (kl_event_init(&probe_loop) != 0) UTEST_SKIP("no event loop");
    unsigned caps = kl_event_caps(&probe_loop);
    kl_event_close(&probe_loop);
    if (caps & KL_EVENT_CAP_COMPLETION)
        UTEST_SKIP("the forced-sweep wrapper is readiness-only; completion loops sweep via IOCP/pollcomp");

    const int N = sweep_n();
    const KlSocketProvider *base = (const KlSocketProvider *)kl_test_builtin_provider();
    cnt_ops = *base->ops;
    orig_peek = base->ops->recv_peek;
    cnt_ops.recv_peek = cnt_peek;
    cnt_prov = *base;
    cnt_prov.ops = &cnt_ops;
    cnt_slots = 0; kl_atomic_store_int(&cnt_total, 0);
    memset(cnt_n, 0, sizeof cnt_n);
    tick_first = tick_last = tick_span_max = 0;

    kl_atomic_store_int(&n_parked, 0);
    kl_atomic_store_int(&n_cancel, 0);
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.bind_addr = "127.0.0.1";
    cfg.max_connections = N + 16;
    cfg.event_provider = &nohup_provider;
    cfg.sockets = &cnt_prov;
    ASSERT_EQ(kl_http_server_init(&srv, &cfg), 0);
    kl_http_server_route(&srv, "GET", "/quiet", handle_quiet, NULL, NULL);
    kl_plat_thread_create(&srv_tid, srv_thread, NULL);
    for (int i = 0; i < 300 && srv.bound_port == 0; i++) kl_test_sleep_ms(10);

    static int fds[SWEEP_MAX];
    static const char q[] = "GET /quiet HTTP/1.1\r\nHost: x\r\n\r\n";
    for (int i = 0; i < N; i++) {
        fds[i] = client();
        ASSERT_TRUE(fds[i] >= 0);
        (void)kl_test_sockwrite(fds[i], q, sizeof q - 1);
    }
    ASSERT_TRUE(wait_for(&n_parked, N, 10000));
    kl_test_sleep_ms(1200);              /* every connection is in the sweep by now */

    int before[SWEEP_MAX + 32], slots = cnt_slots;
    for (int i = 0; i < slots; i++) before[i] = kl_atomic_load_int(&cnt_n[i]);
    kl_test_sleep_ms(2500);
    int lo = 1 << 30, hi = 0, probed = 0;
    for (int i = 0; i < slots; i++) {
        int d = kl_atomic_load_int(&cnt_n[i]) - before[i];
        if (d == 0) continue;            /* an fd probed only before suspension (header peek) */
        probed++;
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    printf("        sweep: %d suspended, %d probed in 2.5 s, %d..%d probes each, longest tick %llu ms\n",
           N, probed, lo, hi, (unsigned long long)tick_span_max);
    EXPECT_EQ(probed, N);                /* each suspended connection is probed */
    EXPECT_TRUE(hi - lo <= 1);           /* once per tick: never more for one than another */
    EXPECT_TRUE(lo >= 2 && hi <= 3);     /* 1 s ticks, not per loop iteration */
    EXPECT_EQ(ld(&n_cancel), 0);         /* nothing terminal observed: nothing cancelled */

    for (int i = 0; i < N; i++) kl_test_closesock(fds[i]);
    EXPECT_TRUE(wait_for(&n_cancel, N, 5000));   /* every FIN seen by the sweep */
    int total = kl_atomic_load_int(&cnt_total);
    kl_test_sleep_ms(2500);
    EXPECT_EQ(kl_atomic_load_int(&cnt_total), total);   /* no probe once no op needs one */
    EXPECT_EQ(kl_atomic_load_int(&srv.hup_sweep_armed), 0);
    stop();
}

UTEST(suspend_hangup, tls_connection_close_cancels) {
    /* Detection is at the transport, under the TLS session (identity mock here; real mbedTLS, with
     * and without close_notify, in integrations/tls/mbedtls/tests/tls_e2e.c). */
    start_tls(KL_HTTP_SUSPEND_HANGUP_CANCEL, 1);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    kl_test_closesock(fd);
    EXPECT_TRUE(wait_for(&n_cancel, 1, DETECT_MS));
    EXPECT_EQ(ld(&n_resume), 0);
    stop();
    EXPECT_EQ(ld(&n_cancel), 1);
}

UTEST(suspend_hangup, sweep_half_close_kept_under_reset_only) {
    /* RESET_ONLY through the sweep on any readiness backend: the FIN is seen by the peek and kept. */
    if (builtin_is_completion()) UTEST_SKIP("the forced-sweep wrapper is readiness-only");
    start_ex(KL_HTTP_SUSPEND_HANGUP_RESET_ONLY, 0, &nohup_provider);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    EXPECT_EQ(hup_state_of(0), KL_HUP_SWEPT);
    shutdown(fd, 1);
    EXPECT_TRUE(wait_hup_state(0, KL_HUP_FIN_KEPT, DETECT_MS));
    EXPECT_EQ(ld(&n_cancel), 0);
    wake(0);
    char buf[512];
    read_all(fd, buf, sizeof buf);
    EXPECT_TRUE(strstr(buf, "resumed") != NULL);
    kl_test_closesock(fd);
    stop();
    EXPECT_EQ(ld(&n_cancel), 0);
}

UTEST(suspend_hangup, sweep_reset_cancels_under_reset_only) {
    if (builtin_is_completion()) UTEST_SKIP("the forced-sweep wrapper is readiness-only");
    start_ex(KL_HTTP_SUSPEND_HANGUP_RESET_ONLY, 0, &nohup_provider);
    int fd = client();
    ASSERT_TRUE(fd >= 0);
    (void)kl_test_sockwrite(fd, PARK_REQ, sizeof PARK_REQ - 1);
    ASSERT_TRUE(wait_for(&n_parked, 1, 2000));
    EXPECT_EQ(hup_state_of(0), KL_HUP_SWEPT);
    reset_close(fd);
    EXPECT_TRUE(wait_for(&n_cancel, 1, DETECT_MS));
    EXPECT_EQ(ld(&n_resume), 0);
    stop();
    EXPECT_EQ(ld(&n_cancel), 1);
}

/* ── Teardown with queued pool work: context ownership across every terminal path ──
 * A work context has two owners that each release it once, in either order: the pool (done_fn if the
 * work ran, cancel_fn if it never did) and the op (resumed, or cancelled by a hang-up or by
 * kl_http_server_free). One worker, whose first job blocks on a gate, so later jobs stay queued. One
 * client hangs up while its job is queued (on_cancel first, cancel_fn later); at teardown the queued
 * job of a still-suspended op gets cancel_fn first and on_cancel later; the running job's done_fn
 * resumes its op. Every context must be freed exactly once (ASan catches a use-after-free). */
typedef struct { KlAsyncOp op; int op_ended, pool_released, idx; } TdCtx;
static int td_gate, td_started, td_frees, td_freed[4];
static KlThreadPool *td_pool;

static void td_free(TdCtx *w) { td_freed[w->idx]++; td_frees++; free(w); }
static void td_release_op(TdCtx *w)   { w->op_ended = 1;      if (w->pool_released) td_free(w); }
static void td_release_pool(TdCtx *w) { w->pool_released = 1; if (w->op_ended) td_free(w); }

/* Opens the gate only once kl_thread_pool_free has signalled shutdown, so job 0 cannot finish (and
 * free the only worker) while jobs 1 and 2 could still be dequeued: they are certain to reach
 * cancel_fn, whatever the scheduling. */
static void td_gate_opener(void *arg) {
    KlThreadPool *pool = arg;
    while (!keel__thread_pool_shutting_down(pool)) kl_test_sleep_ms(1);
    kl_atomic_store_int(&td_gate, 1);
}

static void td_work(void *u) {   /* worker thread */
    TdCtx *w = u;
    if (w->idx == 0) {
        kl_atomic_store_int(&td_started, 1);
        while (!kl_atomic_load_int(&td_gate)) kl_test_sleep_ms(5);
    }
}
static void td_done(void *u) {   /* loop thread, or main() once the loop has stopped */
    TdCtx *w = u;
    if (!w->op_ended) {
        kl_http_response_json(kl_http_conn_response(w->op.conn), 200, "{}", 2);
        kl_async_complete(&srv, &w->op);
        w->op_ended = 1;
    }
    td_release_pool(w);
}
static void td_cancel_fn(void *u) { td_release_pool(u); }
static void td_on_cancel(KlAsyncOp *op, void *u) { (void)u; inc(&n_cancel); td_release_op((TdCtx *)op); }

static void handle_td(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)res; (void)ud;
    int i = ld(&n_parked);
    TdCtx *w = calloc(1, sizeof *w);
    w->idx = i;
    w->op.on_cancel = td_on_cancel;
    kl_async_suspend(&srv, kl_http_request_conn(req), &w->op);
    KlWorkItem item = { .work_fn = td_work, .done_fn = td_done, .cancel_fn = td_cancel_fn, .user_data = w };
    if (kl_thread_pool_submit(td_pool, &item) != 0) abort();
    inc(&n_parked);
}

UTEST(suspend_hangup, teardown_with_queued_pool_work_frees_each_context_once) {
    kl_atomic_store_int(&td_gate, 0);
    kl_atomic_store_int(&td_started, 0);
    td_frees = 0;
    memset(td_freed, 0, sizeof td_freed);
    kl_atomic_store_int(&n_parked, 0);
    kl_atomic_store_int(&n_cancel, 0);
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.bind_addr = "127.0.0.1";
    ASSERT_EQ(kl_http_server_init(&srv, &cfg), 0);
    KlThreadPoolConfig pc = { .num_workers = 1, .queue_capacity = 8 };
    td_pool = kl_thread_pool_create(&srv.ev, &pc);
    ASSERT_TRUE(td_pool != NULL);
    kl_http_server_route(&srv, "GET", "/td", handle_td, NULL, NULL);
    kl_plat_thread_create(&srv_tid, srv_thread, NULL);
    for (int i = 0; i < 300 && srv.bound_port == 0; i++) kl_test_sleep_ms(10);

    static const char q[] = "GET /td HTTP/1.1\r\nHost: x\r\n\r\n";
    int fds[3];
    for (int i = 0; i < 3; i++) {
        fds[i] = client();
        ASSERT_TRUE(fds[i] >= 0);
        (void)kl_test_sockwrite(fds[i], q, sizeof q - 1);
        ASSERT_TRUE(wait_for(&n_parked, i + 1, 2000));
    }
    ASSERT_TRUE(wait_for(&td_started, 1, 2000));   /* job 0 holds the only worker; 1 and 2 are queued */
    kl_test_closesock(fds[2]);                        /* hang-up while its job is queued */
    EXPECT_TRUE(wait_for(&n_cancel, 1, DETECT_MS));
    EXPECT_EQ(td_frees, 0);                           /* the pool still owns it */

    kl_http_server_stop(&srv);
    kl_plat_thread_join(&srv_tid);
    KlPlatThread opener;                              /* job 0 finishes only after shutdown: 1, 2 never run */
    ASSERT_EQ(kl_plat_thread_create(&opener, td_gate_opener, td_pool), 0);
    kl_thread_pool_free(td_pool);                     /* done_fn(0), cancel_fn(1), cancel_fn(2) */
    kl_plat_thread_join(&opener);
    EXPECT_EQ(td_freed[0], 1);                        /* resumed and released by done_fn */
    EXPECT_EQ(td_freed[2], 1);                        /* cancelled earlier, released by cancel_fn */
    EXPECT_EQ(td_freed[1], 0);                        /* still suspended: its op owns it */
    kl_http_server_free(&srv);                        /* cancels op 1: on_cancel frees it */
    EXPECT_EQ(td_freed[1], 1);
    EXPECT_EQ(td_frees, 3);
    EXPECT_EQ(ld(&n_cancel), 2);
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
}

UTEST(suspend_hangup, invalid_policy_rejected) {
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.suspended_hangup = (KlHttpSuspendHangup)7;
    EXPECT_EQ(kl_http_server_init(&s, &cfg), -1);
}

UTEST_MAIN();
