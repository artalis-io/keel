/* test_ctrl_handler_win.c: the console control handler can no longer stop a server the caller has
 * freed. Built and run only on Windows (excluded from the POSIX wildcard; enrolled in
 * WIN_TEST_SUITES). Links a -DKEEL_HTTP_SERVER_PLAT_TEST_HOOKS copy of http_server_plat_win.c ahead
 * of libkeel.a (see the Makefile rule), which exposes the handler and a hook it calls once it has
 * loaded the server. */
#include "utest.h"
#include <keel/keel.h>
#include "http_server_plat.h"

#include <windows.h>

extern void (*kl_http_server_test_ctrl_loaded)(void);
extern BOOL kl_http_server_test_ctrl_handler(DWORD type);

static HANDLE g_loaded, g_release;
static volatile LONG g_restored;

static void hold_after_load(void) {
    SetEvent(g_loaded);
    WaitForSingleObject(g_release, INFINITE);
}

static DWORD WINAPI handler_thread(LPVOID arg) {
    return kl_http_server_test_ctrl_handler(*(DWORD *)arg) ? 1u : 0u;
}

static DWORD WINAPI restore_thread(LPVOID arg) {
    kl_http_server_plat_signals_restore((KlHttpServer *)arg);
    InterlockedExchange(&g_restored, 1);
    return 0;
}

/* Absent the console's own serialisation, which Windows does not document, a console event that
 * arrived just before the restore could load the server, and the caller could then free it while the
 * handler's kl_http_server_stop was still to run. The restore now returns only after a handler that
 * saw the server is done with it. The handler is called directly here, not by the dispatcher. */
UTEST(ctrl_handler, the_restore_waits_for_a_handler_that_saw_the_server) {
    KlHttpServer srv;
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4,
                               .install_signal_handlers = 1 };
    ASSERT_EQ(0, kl_http_server_init(&srv, &cfg));
    g_loaded = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_release = CreateEventA(NULL, TRUE, FALSE, NULL);
    ASSERT_TRUE(g_loaded && g_release);
    g_restored = 0;
    kl_http_server_test_ctrl_loaded = hold_after_load;
    kl_http_server_plat_signals_install(&srv);

    DWORD type = CTRL_C_EVENT;
    HANDLE h = CreateThread(NULL, 0, handler_thread, &type, 0, NULL);
    ASSERT_TRUE(h != NULL);
    ASSERT_EQ(WAIT_OBJECT_0, WaitForSingleObject(g_loaded, 10000));   /* it holds the server */

    HANDLE r = CreateThread(NULL, 0, restore_thread, &srv, 0, NULL);
    ASSERT_TRUE(r != NULL);
    DWORD early = WaitForSingleObject(r, 300);       /* must still be waiting for the handler */
    LONG restored_early = g_restored;
    SetEvent(g_release);
    ASSERT_EQ(WAIT_OBJECT_0, WaitForSingleObject(r, 10000));
    ASSERT_EQ(WAIT_OBJECT_0, WaitForSingleObject(h, 10000));
    DWORD handled = 0;
    GetExitCodeThread(h, &handled);
    CloseHandle(h); CloseHandle(r);
    kl_http_server_test_ctrl_loaded = NULL;

    /* After the restore the handler no longer sees a server, so it leaves the event to the next one. */
    BOOL after = kl_http_server_test_ctrl_handler(CTRL_C_EVENT);
    kl_http_server_free(&srv);
    CloseHandle(g_loaded); CloseHandle(g_release);

    ASSERT_EQ((DWORD)WAIT_TIMEOUT, early);
    ASSERT_EQ(0, (int)restored_early);
    ASSERT_EQ(1u, (unsigned)handled);               /* the handler stopped the server it saw */
    ASSERT_TRUE(after == FALSE);
}

/* Two servers: the one whose run returns first cleared the handler's only server pointer, so the
 * other, still running and still registered, no longer received console events (each fell through
 * to the default handler, which ends the process). A restore now clears the pointer only if it is
 * still its own. */
UTEST(ctrl_handler, a_restore_leaves_a_later_server_its_events) {
    KlHttpServer a, b;
    KlHttpServerConfig cfg = { .port = 0, .bind_addr = "127.0.0.1", .max_connections = 4,
                               .install_signal_handlers = 1 };
    ASSERT_EQ(0, kl_http_server_init(&a, &cfg));
    ASSERT_EQ(0, kl_http_server_init(&b, &cfg));
    kl_http_server_plat_signals_install(&a);
    kl_http_server_plat_signals_install(&b);       /* the last to install receives the events */
    kl_http_server_plat_signals_restore(&a);       /* a's run returns first */
    BOOL to_b = kl_http_server_test_ctrl_handler(CTRL_C_EVENT);
    int b_stopped = b.stop_requested != 0, a_stopped = a.stop_requested != 0;
    kl_http_server_plat_signals_restore(&b);
    BOOL after = kl_http_server_test_ctrl_handler(CTRL_C_EVENT);
    kl_http_server_free(&a);
    kl_http_server_free(&b);

    ASSERT_TRUE(to_b == TRUE);                      /* was FALSE: b's events went to the default */
    ASSERT_EQ(1, b_stopped);
    ASSERT_EQ(0, a_stopped);
    ASSERT_TRUE(after == FALSE);
}

UTEST_MAIN();
