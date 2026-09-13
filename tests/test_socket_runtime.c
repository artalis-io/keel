/*
 * test_socket_runtime.c: the order-independent half of the PAL socket-runtime contract
 * (platform_socket.h). The one property that must be the process's first socket call lives in
 * tests/test_socket_runtime_first_use.c.
 *
 * Covers: idempotence, status reporting, concurrent initialisation, the mock-provider/synthetic-handle
 * error semantics, and the real-descriptor paths through each family that reaches the native stack
 * (default provider, adopted descriptors, wakeup, single-fd poll, blocking resolver). These last ones
 * are what would break loudly if a native boundary were left ungated.
 */
#include "utest.h"
#include "net_compat.h"
#include "platform_socket.h"
#include "platform.h"
#include "platform_thread.h"
#include "resolve_sync.h"
#include "socket.h"
#include <string.h>
#include <errno.h>

/* ── the facility itself ───────────────────────────────────────────────── */

UTEST(socket_runtime, init_succeeds) {
    ASSERT_EQ(0, kl_plat_socket_runtime_init());
}

UTEST(socket_runtime, init_is_idempotent_and_stays_successful) {
    /* Repeated calls are the normal case: every native boundary in the library opens with one, so this
     * runs on the order of once per socket operation. All of them must agree. */
    for (int i = 0; i < 10000; i++)
        ASSERT_EQ(0, kl_plat_socket_runtime_init());
}

UTEST(socket_runtime, status_reports_initialised_after_init) {
    ASSERT_EQ(0, kl_plat_socket_runtime_init());
    /* >0 means attempted and succeeded. Never 0 (not attempted) and never <0 (failed) on a host whose
     * socket runtime is working, which is every host that can run this suite at all. */
    ASSERT_GT(kl_plat_socket_runtime_status(), 0);
}

/* ── concurrent initialisation ─────────────────────────────────────────── */

#define RT_THREADS 8
static int rt_rc[RT_THREADS];

static void rt_worker(void *arg) {
    int slot = *(int *)arg;
    /* Hammer it rather than call once: the interesting window is inside the one-time initialisation,
     * and every caller must come out of it with the same answer. */
    int rc = 0;
    for (int i = 0; i < 256; i++)
        if (kl_plat_socket_runtime_init() != 0) rc = -1;
    rt_rc[slot] = rc;
}

UTEST(socket_runtime, concurrent_init_is_safe) {
    /* Deterministic by construction, not by timing: every thread is joined and every result checked,
     * so the test neither sleeps nor races for a verdict. It would fail reproducibly if the one-time
     * initialisation let a second caller through before the first had published its result. */
    KlPlatThread th[RT_THREADS];
    int slot[RT_THREADS];
    for (int i = 0; i < RT_THREADS; i++) {
        slot[i] = i;
        rt_rc[i] = 1;   /* poisoned: a thread that never ran would fail below */
        ASSERT_EQ(0, kl_plat_thread_create(&th[i], rt_worker, &slot[i]));
    }
    for (int i = 0; i < RT_THREADS; i++)
        kl_plat_thread_join(&th[i]);
    for (int i = 0; i < RT_THREADS; i++)
        ASSERT_EQ(0, rt_rc[i]);
    ASSERT_GT(kl_plat_socket_runtime_status(), 0);
}

/* ── mock/custom provider over a handle Keel never created ─────────────── */

UTEST(socket_runtime, mock_provider_synthetic_handle_reports_a_socket_error) {
    /* Order-independent restatement of the first-use property: whatever ran before, a synthetic handle
     * must produce an error ABOUT THE HANDLE. This is the class of call that made lazy
     * init-at-socket-creation insufficient. */
    KlSocketOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.name = "mock-without-io";
    KlSocketProvider prov = { &ops, NULL, KL_SOCK_CAP_NATIVE_FD, NULL };
    char buf[4] = { 0 };

    KlSockAddr scratch;
    memset(&scratch, 0, sizeof(scratch));
    struct { const char *what; kl_ssize_t r; } calls[3];
#if defined(_WIN32)
    WSASetLastError(0);
#endif
    calls[0].what = "recv";  calls[0].r = kl_sock_recv(&prov, (KlSocketHandle)0x1234, buf, sizeof(buf));
    calls[1].what = "send";  calls[1].r = kl_sock_send(&prov, (KlSocketHandle)0x1234, buf, sizeof(buf));
    calls[2].what = "local"; calls[2].r = kl_sock_get_local_addr(&prov, (KlSocketHandle)0x1234, &scratch);
    for (int i = 0; i < 3; i++)
        ASSERT_EQ_MSG(-1, (int)calls[i].r, calls[i].what);
#if defined(_WIN32)
    ASSERT_NE(WSANOTINITIALISED, WSAGetLastError());
#endif
}

/* ── real descriptors through each native family ───────────────────────── */

UTEST(socket_runtime, default_provider_creates_and_closes_a_socket) {
    KlSocketHandle fd = kl_sockdef_socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(kl_handle_valid(fd));
    ASSERT_EQ(0, kl_sockdef_set_nonblocking(fd));
    ASSERT_EQ(0, kl_sockdef_close(fd));
}

UTEST(socket_runtime, adopted_descriptors_remain_valid) {
    /* An externally supplied descriptor: the harness creates the pair, Keel only operates on it. The
     * gates must not disturb ordinary use of a descriptor Keel did not open. */
    int sv[2];
    ASSERT_EQ(0, kl_test_socketpair(sv));

    KlSockAddr local;
    memset(&local, 0, sizeof(local));
    ASSERT_EQ(0, kl_sockdef_get_local_addr((KlSocketHandle)sv[0], &local));
    ASSERT_EQ(0, kl_sockdef_set_nonblocking((KlSocketHandle)sv[0]));

    static const char msg[] = "adopted";
    ASSERT_EQ((int)sizeof(msg), (int)kl_sockdef_send((KlSocketHandle)sv[1], msg, sizeof(msg)));
    char got[sizeof(msg)] = { 0 };
    ASSERT_EQ((int)sizeof(msg), (int)kl_sockdef_recv((KlSocketHandle)sv[0], got, sizeof(got)));
    ASSERT_STREQ(msg, got);

    kl_test_closesock(sv[0]);
    kl_test_closesock(sv[1]);
}

UTEST(socket_runtime, wakeup_and_single_fd_poll_paths_work) {
    /* kl_plat_wakeup_open builds a loopback socket pair on Windows and kl_plat_poll1 is WSAPoll, so
     * both are native boundaries in their own right, reached with no socket provider in sight. */
    KlPlatWakeup w;
    ASSERT_EQ(0, kl_plat_wakeup_open(&w));
    kl_plat_wakeup_signal(&w);
    ASSERT_GT(kl_plat_poll1(w.rd, KL_POLL_IN, 1000), 0);
    kl_plat_wakeup_drain(w.rd);
    kl_plat_wakeup_close(&w);
}

UTEST(socket_runtime, blocking_resolver_path_works) {
    /* getaddrinfo is its own native boundary, in a SHARED TU: it is the one call site that made the
     * POSIX half of this PAL seam necessary rather than merely symmetric. A numeric literal so the
     * test needs no DNS. */
    KlSockAddr addrs[4];
    int n = 0;
    ASSERT_EQ(0, kl_resolve_sync("127.0.0.1", 80, SOCK_STREAM, addrs, 4, &n));
    ASSERT_GE(n, 1);
}

UTEST_MAIN();
