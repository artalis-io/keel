/*
 * test_iocp_engine.c — IOCP backend lifecycle (Windows/BACKEND=iocp).
 *
 * Increment 2 scope: prove the IOCP event backend boots and advertises the
 * completion capability, the overlapped provider carries KL_SOCK_CAP_OVERLAPPED,
 * and the capability negotiation accepts that pairing on a REAL completion loop.
 * The completion tick + connection driver (accept/read/write over WSARecv/WSASend)
 * land in the next increment; this is the runtime lifecycle gate.
 *
 * Built and run ONLY under BACKEND=iocp (it asserts COMPLETION caps, which the
 * WSAPoll backend does not advertise). The Windows-IOCP CI job is its oracle.
 */
#include "utest.h"
#include "../src/event_caps.h"
#include "../src/socket.h"     /* KL_SOCK_CAP_OVERLAPPED + kl_socket_provider_iocp */

#include <keel/event.h>
#include <keel/allocator.h>
#include <keel/socket.h>

UTEST(iocp, loop_boots_and_advertises_completion) {
    KlAllocator alloc = kl_allocator_default();
    KlEventLoop loop;
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);

    unsigned caps = kl_event_caps(&loop);
    ASSERT_TRUE(caps & KL_EVENT_CAP_COMPLETION);
    ASSERT_TRUE(caps & KL_EVENT_CAP_NATIVE_FD);
    ASSERT_FALSE(caps & KL_EVENT_CAP_READINESS);

    kl_event_close(&loop);
}

UTEST(iocp, overlapped_provider_negotiates) {
    const KlSocketProvider *p = kl_socket_provider_iocp();
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(kl_socket_provider_has_cap(p, KL_SOCK_CAP_OVERLAPPED));

    /* A real IOCP loop ⋄ the overlapped provider must negotiate as compatible;
     * the built-in (non-overlapped) default must not. */
    KlAllocator alloc = kl_allocator_default();
    KlEventLoop loop;
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);
    unsigned caps = kl_event_caps(&loop);

    ASSERT_TRUE(kl_caps_compatible(caps, p));       /* overlapped provider: OK */
    ASSERT_FALSE(kl_caps_compatible(caps, NULL));   /* POSIX/default: rejected */

    kl_event_close(&loop);
}

UTEST_MAIN();
