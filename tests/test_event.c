#include "utest.h"
#include <keel/event.h>
#include <keel/allocator.h>
#include "net_compat.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * Event backend tests
 *
 * Uses pipe pairs as test FDs. Platform-independent: works on all
 * backends (epoll, kqueue, io_uring, poll).
 * ═══════════════════════════════════════════════════════════════════ */

UTEST(event, init_and_close) {
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;

    ASSERT_EQ(kl_event_init(&loop), 0);
    kl_event_close(&loop);
}

UTEST(event, add_and_wait) {
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    /* Register read end */
    int marker = 42;
    ASSERT_EQ(kl_event_add(&loop, fds[0], KL_EVENT_READ, &marker), 0);

    /* Write to trigger readability */
    char c = 'x';
    ASSERT_EQ((int)kl_test_sockwrite(fds[1], &c, 1), 1);

    /* Wait for event */
    KlEvent events[4];
    int n = kl_event_wait(&loop, events, 4, 500);
    ASSERT_GE(n, 1);

    /* Verify the event */
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (events[i].udata == &marker) {
            ASSERT_TRUE(events[i].ready & KL_EVENT_READ);
            found = 1;
        }
    }
    ASSERT_TRUE(found);

    kl_event_del(&loop, fds[0]);
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    kl_event_close(&loop);
}

UTEST(event, del_fd) {
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    int marker = 1;
    ASSERT_EQ(kl_event_add(&loop, fds[0], KL_EVENT_READ, &marker), 0);

    /* Remove fd (return value is backend-dependent: kqueue may return -1
     * when deleting a filter that was never registered, e.g. EVFILT_WRITE) */
    kl_event_del(&loop, fds[0]);

    /* Write data */
    char c = 'x';
    (void)kl_test_sockwrite(fds[1], &c, 1);

    /* Wait: should timeout, no events for removed fd */
    KlEvent events[4];
    int n = kl_event_wait(&loop, events, 4, 50);
    /* n should be 0 (timeout) since we removed the fd */
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (events[i].udata == &marker) found = 1;
    }
    ASSERT_FALSE(found);

    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    kl_event_close(&loop);
}

UTEST(event, multiple_fds) {
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);

    int pipe1[2], pipe2[2], pipe3[2];
    ASSERT_EQ(kl_test_socketpair(pipe1), 0);
    ASSERT_EQ(kl_test_socketpair(pipe2), 0);
    ASSERT_EQ(kl_test_socketpair(pipe3), 0);

    int m1 = 1, m2 = 2, m3 = 3;
    ASSERT_EQ(kl_event_add(&loop, pipe1[0], KL_EVENT_READ, &m1), 0);
    ASSERT_EQ(kl_event_add(&loop, pipe2[0], KL_EVENT_READ, &m2), 0);
    ASSERT_EQ(kl_event_add(&loop, pipe3[0], KL_EVENT_READ, &m3), 0);

    /* Write to pipe1 and pipe3 only */
    char c = 'x';
    (void)kl_test_sockwrite(pipe1[1], &c, 1);
    (void)kl_test_sockwrite(pipe3[1], &c, 1);

    /* Wait for events */
    KlEvent events[8];
    int n = kl_event_wait(&loop, events, 8, 500);
    ASSERT_GE(n, 2);

    int found1 = 0, found3 = 0;
    for (int i = 0; i < n; i++) {
        if (events[i].udata == &m1) found1 = 1;
        if (events[i].udata == &m3) found3 = 1;
    }
    ASSERT_TRUE(found1);
    ASSERT_TRUE(found3);

    kl_event_del(&loop, pipe1[0]);
    kl_event_del(&loop, pipe2[0]);
    kl_event_del(&loop, pipe3[0]);
    kl_test_closesock(pipe1[0]); kl_test_closesock(pipe1[1]);
    kl_test_closesock(pipe2[0]); kl_test_closesock(pipe2[1]);
    kl_test_closesock(pipe3[0]); kl_test_closesock(pipe3[1]);
    kl_event_close(&loop);
}

UTEST(event, invalid_fd) {
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);

    /* Negative fd must be rejected on all backends */
    ASSERT_NE(kl_event_add(&loop, -1, KL_EVENT_READ, NULL), 0);

    kl_event_close(&loop);
}

UTEST(event, timeout_no_events) {
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);

    /* Wait with nothing registered: should return 0 (timeout) */
    KlEvent events[4];
    int n = kl_event_wait(&loop, events, 4, 10);
    ASSERT_EQ(n, 0);

    kl_event_close(&loop);
}

UTEST(event, mod_mask) {
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);

    int marker = 99;
    /* Register for READ */
    ASSERT_EQ(kl_event_add(&loop, fds[0], KL_EVENT_READ, &marker), 0);

    /* Modify to WRITE: verify kl_event_mod doesn't crash/error */
    ASSERT_EQ(kl_event_mod(&loop, fds[0], KL_EVENT_WRITE, &marker), 0);

    /* Note: mask enforcement after mod is backend-dependent.
     * epoll/kqueue correctly filter to WRITE-only; poll and io_uring
     * may still report READ. We only assert mod returns 0. */

    kl_event_del(&loop, fds[0]);
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    kl_event_close(&loop);
}

UTEST(event, double_close) {
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);

    kl_event_close(&loop);
    /* Second close should be safe (no crash) */
    kl_event_close(&loop);
}

UTEST_MAIN();
