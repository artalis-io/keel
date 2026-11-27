/*
 * test_server_start_fail.c: a kl_http_server_run that fails to start after installing its
 * SIGTERM/SIGINT handlers restores the ones it replaced. Links a -DKEEL_HTTP_SERVER_TEST_HOOKS
 * copy of the server (see the Makefile rule), whose hook makes the readiness accept path fail.
 */
#include "utest.h"
#include <keel/keel.h>
#include "../src/event_caps.h"

#include <signal.h>
#include <string.h>

extern int kl_http_server_test_fail_listener_start;

static void sentinel(int sig) { (void)sig; }

/* With install_signal_handlers, a run whose accept path could not start returned -1 with SIGTERM
 * and SIGINT still routed to that server, so a later signal stopped a server the caller had freed. */
UTEST(server_start, a_failed_start_restores_the_signal_handlers) {
    struct sigaction mine, term_before, int_before, term_after, int_after;
    memset(&mine, 0, sizeof(mine));
    mine.sa_handler = sentinel;
    sigemptyset(&mine.sa_mask);
    ASSERT_EQ(0, sigaction(SIGTERM, &mine, &term_before));
    ASSERT_EQ(0, sigaction(SIGINT, &mine, &int_before));

    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4,
                               .install_signal_handlers = 1 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    if (kl_event_caps(&srv.ev.loop) & KL_EVENT_CAP_COMPLETION) {
        kl_http_server_free(&srv);
        sigaction(SIGTERM, &term_before, NULL);
        sigaction(SIGINT, &int_before, NULL);
        UTEST_SKIP("a completion loop starts its accepts in the run loop, which restores on exit");
    }
    kl_http_server_test_fail_listener_start = 1;
    int rc = kl_http_server_run(&srv);
    kl_http_server_test_fail_listener_start = 0;
    KlError err = srv.last_error;
    sigaction(SIGTERM, NULL, &term_after);
    sigaction(SIGINT, NULL, &int_after);
    kl_http_server_free(&srv);
    sigaction(SIGTERM, &term_before, NULL);
    sigaction(SIGINT, &int_before, NULL);

    ASSERT_EQ(-1, rc);
    ASSERT_EQ(KL_ERR_EVENT_ADD, err);                  /* it failed where the hook made it fail */
    ASSERT_TRUE(term_after.sa_handler == sentinel);    /* was the server's handler */
    ASSERT_TRUE(int_after.sa_handler == sentinel);
}

UTEST_MAIN();
