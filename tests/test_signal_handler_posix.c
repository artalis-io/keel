/* test_signal_handler_posix.c: the SIGTERM/SIGINT handler can no longer stop a server its caller has
 * freed, and overlapping servers put the original signal dispositions back. POSIX only (excluded
 * from the Windows suite lists). Links a -DKEEL_HTTP_SERVER_PLAT_TEST_HOOKS copy of
 * http_server_plat_posix.c ahead of libkeel.a (see the Makefile rule), which exposes the handler and
 * a hook it calls once it has loaded the server. */
#include "utest.h"
#include <keel/keel.h>
#include "http_server_plat.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#if !defined(KL_NO_SIGNAL)
extern void (*kl_http_server_test_signal_loaded)(void);
extern void kl_http_server_test_signal_handler(int sig);

static atomic_int g_loaded, g_release, g_restored, g_target_done;

/* Runs inside the signal handler: only lock-free atomics. */
static void hold_after_load(void) {
    atomic_store(&g_loaded, 1);
    while (!atomic_load(&g_release)) { }
}

/* The thread the signal is sent to: it waits, with SIGTERM unblocked, until told to finish. */
static void *target_thread(void *arg) {
    (void)arg;
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGTERM);
    pthread_sigmask(SIG_UNBLOCK, &m, NULL);
    while (!atomic_load(&g_target_done)) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void *restore_thread(void *arg) {
    kl_http_server_plat_signals_restore((KlHttpServer *)arg);
    atomic_store(&g_restored, 1);
    return NULL;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* A SIGTERM delivered to another thread just before the restore could load the server, and the
 * caller could then free it while the handler's kl_http_server_stop was still to run (a write into
 * freed memory and a write() to a descriptor number that may have been reused). The restore now
 * returns only after a handler that saw the server is done with it. A real signal, sent to a thread
 * of the test's choosing. */
UTEST(signal_handler, the_restore_waits_for_a_handler_that_saw_the_server) {
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);      /* this thread and the restore's never take it */

    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4,
                               .install_signal_handlers = 1 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    atomic_store(&g_loaded, 0);
    atomic_store(&g_release, 0);
    atomic_store(&g_restored, 0);
    atomic_store(&g_target_done, 0);
    kl_http_server_test_signal_loaded = hold_after_load;
    kl_http_server_plat_signals_install(&srv);

    pthread_t t, r;
    ASSERT_EQ(0, pthread_create(&t, NULL, target_thread, NULL));
    ASSERT_EQ(0, pthread_kill(t, SIGTERM));
    for (int i = 0; i < 10000 && !atomic_load(&g_loaded); i++) sleep_ms(1);
    int loaded = atomic_load(&g_loaded);             /* the handler holds the server */

    ASSERT_EQ(0, pthread_create(&r, NULL, restore_thread, &srv));
    sleep_ms(300);
    int restored_early = atomic_load(&g_restored);   /* must still be waiting for the handler */
    atomic_store(&g_release, 1);
    pthread_join(r, NULL);
    int restored = atomic_load(&g_restored);
    int stopped = srv.stop_requested != 0;           /* the handler stopped the server it saw */
    atomic_store(&g_target_done, 1);
    pthread_join(t, NULL);
    kl_http_server_test_signal_loaded = NULL;

    /* After the restore the handler no longer sees a server. */
    kl_http_server_test_signal_handler(SIGTERM);
    kl_http_server_free(&srv);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    ASSERT_EQ(1, loaded);
    ASSERT_EQ(0, restored_early);                   /* was 1: the restore returned at once */
    ASSERT_EQ(1, restored);
    ASSERT_EQ(1, stopped);
}

static void marker_handler(int sig) { (void)sig; }

static int term_is_marker(void) {
    struct sigaction cur;
    sigaction(SIGTERM, NULL, &cur);
    return cur.sa_handler == marker_handler;
}

/* Two servers whose runs overlap. The second install saved the first's handler as "the old
 * disposition", so once both had returned SIGTERM and SIGINT stayed routed to a handler with no
 * server: the process could no longer be stopped by them at all. And the first to return cleared
 * the only server pointer, so the other no longer received signals. The dispositions in place
 * before the first install are now saved once and put back by the server receiving signals (the
 * last to install), and a superseded server's restore leaves the other its signals. */
UTEST(signal_handler, overlapping_servers_put_the_original_dispositions_back) {
    struct sigaction marker, saved_term;
    memset(&marker, 0, sizeof(marker));
    marker.sa_handler = marker_handler;
    sigemptyset(&marker.sa_mask);
    sigaction(SIGTERM, &marker, &saved_term);

    KlHttpServer a, b;
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4,
                               .install_signal_handlers = 1 };
    ASSERT_EQ(0, kl_http_server_init(&a, &cfg));
    ASSERT_EQ(0, kl_http_server_init(&b, &cfg));

    /* a returns first */
    kl_http_server_plat_signals_install(&a);
    kl_http_server_plat_signals_install(&b);
    kl_http_server_plat_signals_restore(&a);
    int still_ours = !term_is_marker();
    kl_http_server_test_signal_handler(SIGTERM);
    int b_stopped = b.stop_requested != 0, a_stopped = a.stop_requested != 0;
    kl_http_server_plat_signals_restore(&b);
    int back_1 = term_is_marker();

    /* b returns first */
    KlHttpServer c, d;
    ASSERT_EQ(0, kl_http_server_init(&c, &cfg));
    ASSERT_EQ(0, kl_http_server_init(&d, &cfg));
    kl_http_server_plat_signals_install(&c);
    kl_http_server_plat_signals_install(&d);
    kl_http_server_plat_signals_restore(&d);
    int back_2a = term_is_marker();
    kl_http_server_plat_signals_restore(&c);
    int back_2b = term_is_marker();

    kl_http_server_free(&a);
    kl_http_server_free(&b);
    kl_http_server_free(&c);
    kl_http_server_free(&d);
    sigaction(SIGTERM, &saved_term, NULL);

    ASSERT_EQ(1, still_ours);
    ASSERT_EQ(1, b_stopped);                         /* was 0: a's restore cut b off */
    ASSERT_EQ(0, a_stopped);
    ASSERT_EQ(1, back_1);                            /* was 0: b put a's Keel handler back */
    ASSERT_EQ(1, back_2a);
    ASSERT_EQ(1, back_2b);                           /* was 0: a Keel handler with no server */
}

#endif /* !KL_NO_SIGNAL */

UTEST_MAIN();
