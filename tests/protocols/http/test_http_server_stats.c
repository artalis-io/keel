#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include <keel/keel.h>

#include <string.h>

UTEST(server_stats, stats_initial) {
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 4;

    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);

    KlHttpServerStats stats;
    kl_http_server_stats(&s, &stats);

    ASSERT_EQ(stats.active_connections, 0);
    ASSERT_EQ(stats.max_connections, 4);
    ASSERT_EQ(stats.async_suspended, 0);
    ASSERT_EQ(stats.listen_paused, 0);

    kl_http_server_free(&s);
}

UTEST(server_stats, stats_active_count) {
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 4;

    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);

    /* Simulate active connections by acquiring from pool */
    KlHttpConn *c1 = kl_http_conn_acquire(&s.pool, 100);
    ASSERT_TRUE(c1 != NULL);
    KlHttpConn *c2 = kl_http_conn_acquire(&s.pool, 101);
    ASSERT_TRUE(c2 != NULL);

    KlHttpServerStats stats;
    kl_http_server_stats(&s, &stats);
    ASSERT_EQ(stats.active_connections, 2);

    /* Release one */
    kl_http_conn_release(&s.pool, c1);
    kl_http_server_stats(&s, &stats);
    ASSERT_EQ(stats.active_connections, 1);

    kl_http_conn_release(&s.pool, c2);
    kl_http_server_stats(&s, &stats);
    ASSERT_EQ(stats.active_connections, 0);

    kl_http_server_free(&s);
}

UTEST(server_stats, stats_max_connections) {
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 16;

    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);

    KlHttpServerStats stats;
    kl_http_server_stats(&s, &stats);
    ASSERT_EQ(stats.max_connections, 16);

    kl_http_server_free(&s);
}

UTEST(server_stats, stats_null_safety) {
    KlHttpServerStats stats;
    memset(&stats, 0xFF, sizeof(stats));

    /* NULL server: should zero out */
    kl_http_server_stats(NULL, &stats);
    ASSERT_EQ(stats.active_connections, 0);
    ASSERT_EQ(stats.max_connections, 0);
    ASSERT_EQ(stats.async_suspended, 0);
    ASSERT_EQ(stats.listen_paused, 0);

    /* NULL out: should not crash */
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 4;
    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);

    kl_http_server_stats(&s, NULL);  /* no-op, no crash */

    kl_http_server_free(&s);
}

/* F2-B accessors: event-context and bound-port accessors, normal and NULL inputs. */
UTEST(server_stats, accessors) {
    KlHttpServer s;
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.max_connections = 4;
    ASSERT_EQ(kl_http_server_init(&s, &cfg), 0);

    ASSERT_EQ((void *)kl_http_server_event_ctx(&s), (void *)&s.ev);
    ASSERT_EQ((const void *)kl_http_server_event_ctx_const(&s), (const void *)&s.ev);
    ASSERT_EQ(kl_http_server_bound_port(&s), s.bound_port);

    ASSERT_TRUE(kl_http_server_event_ctx(NULL) == NULL);
    ASSERT_TRUE(kl_http_server_event_ctx_const(NULL) == NULL);
    ASSERT_EQ(kl_http_server_bound_port(NULL), -1);

    kl_http_server_free(&s);
}

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

static void stats_sleep_ms(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) { }
}
static double process_cpu_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) return 0.0;
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
static void *exhaust_server_thread(void *arg) {
    kl_http_server_run((KlHttpServer *)arg);
    return NULL;
}

/* The server lives in static storage, and every check is made after the server thread is joined:
 * an early ASSERT return must not leave the running server pointing at a dead stack frame. */
static KlHttpServer g_exhaust_srv;
static int g_exhaust_fill[4096];

/* When accept() fails because the process is out of descriptors, the connection stays queued in
 * the kernel and the listen socket stays readable. Retrying at once fails again, so the loop must
 * back off instead of spinning at full CPU, and it must accept the queued connection once
 * descriptors are free again. */
UTEST(server_stats, accept_descriptor_exhaustion_backs_off) {
    KlHttpServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.bind_addr = "127.0.0.1";
    cfg.max_connections = 4;
    ASSERT_EQ(0, kl_http_server_init(&g_exhaust_srv, &cfg));

    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, exhaust_server_thread, &g_exhaust_srv));
    for (int i = 0; i < 300 && kl_http_server_bound_port(&g_exhaust_srv) <= 0; i++)
        stats_sleep_ms(10);
    int port = kl_http_server_bound_port(&g_exhaust_srv);
    stats_sleep_ms(50);                                  /* let the loop reach its wait */

    /* The client socket exists before the table is filled: connect() needs no new descriptor. */
    int cfd = (int)socket(AF_INET, SOCK_STREAM, 0);

    struct rlimit old_lim, lim;
    int lim_ok = getrlimit(RLIMIT_NOFILE, &old_lim) == 0;
    lim = old_lim;
    if (lim_ok && (lim.rlim_cur == RLIM_INFINITY || lim.rlim_cur > 1024)) {
        lim.rlim_cur = 1024;
        lim_ok = setrlimit(RLIMIT_NOFILE, &lim) == 0;
    }
    int nfill = 0, exhausted = 0;
    int base = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (base >= 0) {
        while (nfill < (int)(sizeof(g_exhaust_fill) / sizeof(g_exhaust_fill[0]))) {
            int d = fcntl(base, F_DUPFD_CLOEXEC, 0);
            if (d < 0) { exhausted = (errno == EMFILE); break; }
            g_exhaust_fill[nfill++] = d;
        }
    }

    int connected = -1;
    double spent = -1.0;
    if (port > 0 && cfd >= 0 && exhausted) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connected = connect(cfd, (struct sockaddr *)&a, sizeof(a));
        double c0 = process_cpu_ms();
        stats_sleep_ms(600);                             /* the server's accept fails throughout */
        spent = process_cpu_ms() - c0;
    }

    for (int i = 0; i < nfill; i++) close(g_exhaust_fill[i]);
    if (base >= 0) close(base);
    if (lim_ok) (void)setrlimit(RLIMIT_NOFILE, &old_lim);

    /* Descriptors are free again: the queued connection must now be served. */
    char buf[256];
    long got = -1;
    int timed_out = 0;                                   /* the read gave up waiting: a hang */
    if (connected == 0) {
        struct timeval tv = { 5, 0 };
        (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const char *req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        if (send(cfd, req, strlen(req), 0) == (long)strlen(req)) {
            got = (long)recv(cfd, buf, sizeof(buf) - 1, 0);
            timed_out = got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
    }
    if (cfd >= 0) close(cfd);

    kl_http_server_stop(&g_exhaust_srv);
    pthread_join(th, NULL);
    kl_http_server_free(&g_exhaust_srv);

    ASSERT_GT(port, 0);
    ASSERT_TRUE(exhausted);
    ASSERT_EQ(0, connected);
    /* 600 ms of wall time with accept() failing: a spinning loop burns about all of it. */
    ASSERT_LT(spent, 200.0);
#ifdef __linux__
    /* Linux keeps the connection queued when accept() fails for a descriptor, so once one is free
     * the server accepts it and answers. */
    ASSERT_GT(got, 0L);
    buf[got > 0 ? got : 0] = '\0';
    ASSERT_EQ(0, strncmp(buf, "HTTP/1.1 ", 9));
#else
    /* macOS (kqueue) was seen to drop the connection whose accept() failed instead of leaving it
     * queued: the client sees it closed (0) or reset (-1, ECONNRESET), on the send or the read. What
     * must not happen is a hang: the read giving up after its 5 s timeout. */
    (void)got;
    ASSERT_FALSE(timed_out);
#endif
}
#endif

UTEST_MAIN();
