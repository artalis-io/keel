/*
 * test_socket_runtime_first_use.c: the PAL socket-runtime invariant, observed on the process's very
 * FIRST native socket call (platform_socket.h).
 *
 * A SEPARATE, SINGLE-TEST BINARY on purpose. The property under test is "the first native socket call
 * in a process brings the platform socket runtime up", and it is only observable while that runtime
 * has NOT yet been initialised. Any other test running first in the same executable would initialise
 * it and leave this one passing vacuously, so the guarantee comes from being alone in its process
 * rather than from utest's registration order. Everything order-independent lives in
 * tests/test_socket_runtime.c.
 *
 * The oracle is deliberately the native error code, not errno: kl_wsa_set_errno() maps both
 * WSAENOTSOCK and WSANOTINITIALISED to EIO, so errno cannot tell the two apart and would pass either
 * way. Without the gate in kl_sockdef_recv this test fails with 10093 (WSANOTINITIALISED) where it
 * expects 10038 (WSAENOTSOCK) -- the exact regression that made the whole test_datagram_public suite
 * fail the first time the load-time Winsock constructor was removed.
 */
#include "utest.h"
#include "net_compat.h"
#include "platform_socket.h"
#include "socket.h"
#include <string.h>
#include <errno.h>

UTEST(socket_runtime_first_use, first_native_socket_call_initialises_the_runtime) {
    /* Nothing in this process has touched a socket yet, and no compiler constructor remains to do it
     * behind our back: that is what makes the observation below meaningful. */
#if defined(_WIN32)
    ASSERT_EQ(0, kl_plat_socket_runtime_status());   /* not yet attempted */
#else
    ASSERT_EQ(1, kl_plat_socket_runtime_status());   /* no runtime to start; always up */
#endif

    /* A provider that supplies no recv op, over a handle it never created. kl_sock_recv falls through
     * to the built-in kl_sockdef_recv, which enters the native stack with a synthetic descriptor.
     * This is the shape a mock/custom provider produces, and the one lazy init-at-socket-creation
     * would have missed entirely. */
    KlSocketOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.name = "mock-without-recv";
    KlSocketProvider prov = { &ops, NULL, KL_SOCK_CAP_NATIVE_FD, NULL };

    char buf[4];
#if defined(_WIN32)
    WSASetLastError(0);
#endif
    errno = 0;
    kl_ssize_t r = kl_sock_recv(&prov, (KlSocketHandle)0x1234, buf, sizeof(buf));

    /* The call must fail as a socket error, never succeed. */
    ASSERT_EQ(-1, (int)r);

#if defined(_WIN32)
    /* The substance of the test: a real socket error about the handle, not a complaint that the
     * runtime was never started. */
    ASSERT_NE(WSANOTINITIALISED, WSAGetLastError());
    ASSERT_EQ(WSAENOTSOCK, WSAGetLastError());
    /* ...and the first use is what brought the runtime up. */
    ASSERT_GT(kl_plat_socket_runtime_status(), 0);
#else
    /* POSIX needs no runtime, so there is nothing to observe beyond the failure itself. The errno is
     * left unasserted on purpose: a never-opened descriptor number is EBADF, not ENOTSOCK, and which
     * one it is carries no information about the invariant under test. */
    ASSERT_NE(0, errno);
#endif
}

UTEST_MAIN();
