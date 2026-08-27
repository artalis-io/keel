#include "utest.h"
#include <keel/event.h>
#include <keel/allocator.h>
#include "net_compat.h"
#include <string.h>
#ifndef _WIN32
#include <unistd.h>            /* dup/close/dup2 for the descriptor-zero test (POSIX) */
#endif
#include "../src/event_caps.h" /* kl_event_caps: gate the descriptor-zero round-trip to native-fd readiness */

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

/* F2-D: the backend descriptor moved off the public KlEventLoop into loop->alloc-owned private
 * state (_backend). A failing allocator at kl_event_init must unwind exactly once and leave
 * _backend == NULL, leaking neither memory nor a kernel descriptor (ASan/LSan enforce the leak
 * check). Backend-agnostic: every compiled-in backend now allocates its state through loop->alloc. */
static void *f2d_fail_malloc(void *ctx, size_t size) { (void)ctx; (void)size; return NULL; }
static void *f2d_fail_realloc(void *ctx, void *p, size_t os, size_t ns) { (void)ctx; (void)p; (void)os; (void)ns; return NULL; }
static void  f2d_fail_free(void *ctx, void *p, size_t size) { (void)ctx; (void)p; (void)size; }

UTEST(event, backend_state_alloc_failure) {
    KlAllocator failing = { f2d_fail_malloc, f2d_fail_realloc, f2d_fail_free, NULL };
    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    loop.alloc = &failing;

    ASSERT_EQ(kl_event_init(&loop), -1);   /* allocator failure -> init fails */
    ASSERT_TRUE(loop._backend == NULL);    /* unwound exactly once; nothing published */

    kl_event_close(&loop);                 /* close on a never-initialized loop is a no-op */
    ASSERT_TRUE(loop._backend == NULL);
}

#ifndef _WIN32
/* F2-D: a valid backend descriptor equal to 0 must not alias the closed (_backend == NULL) state.
 * Close stdin so the epoll/kqueue descriptor created by init lands on fd 0, drive a full readiness
 * round-trip (on a native-fd readiness backend), and confirm a clean close resets _backend. On a
 * backend without a single kernel fd this still exercises init/close with fd 0 free. */
UTEST(event, backend_descriptor_zero) {
    int saved = dup(0);
    ASSERT_GE(saved, 0);
    ASSERT_EQ(close(0), 0);                /* fd 0 is now the lowest free descriptor */

    KlEventLoop loop;
    memset(&loop, 0, sizeof(loop));
    KlAllocator alloc = kl_allocator_default();
    loop.alloc = &alloc;
    ASSERT_EQ(kl_event_init(&loop), 0);    /* an epoll/kqueue descriptor lands on fd 0 */
    ASSERT_TRUE(loop._backend != NULL);

    unsigned caps = kl_event_caps(&loop);
    if ((caps & KL_EVENT_CAP_READINESS) && (caps & KL_EVENT_CAP_NATIVE_FD) &&
        !(caps & KL_EVENT_CAP_COMPLETION)) {
        int fds[2];
        ASSERT_EQ(kl_test_socketpair(fds), 0);
        int marker = 7;
        ASSERT_EQ(kl_event_add(&loop, fds[0], KL_EVENT_READ, &marker), 0);
        char c = 'x';
        ASSERT_EQ((int)kl_test_sockwrite(fds[1], &c, 1), 1);
        KlEvent evs[4];
        int n = kl_event_wait(&loop, evs, 4, 1000);
        ASSERT_GE(n, 1);
        int found = 0;
        for (int i = 0; i < n; i++)
            if (evs[i].udata == &marker) found = 1;
        ASSERT_TRUE(found);                /* the loop functions with its descriptor at fd 0 */
        kl_event_del(&loop, fds[0]);
        kl_test_closesock(fds[0]);
        kl_test_closesock(fds[1]);
    }

    kl_event_close(&loop);
    ASSERT_TRUE(loop._backend == NULL);    /* fd 0 closed and state freed, not aliased with closed */

    ASSERT_EQ(dup2(saved, 0), 0);          /* restore stdin */
    ASSERT_EQ(close(saved), 0);
}
#endif

UTEST_MAIN();
