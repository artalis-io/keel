/*
 * test_cloexec.c: every descriptor or handle Keel creates is close-on-exec (non-inheritable).
 *
 * An embedder that spawns children (Hull does) must not hand them Keel's own descriptors: the event
 * loop's (epoll), the run-loop wakeup pipe, a thread pool's wakeup, a datagram socket. A leaked
 * descriptor keeps a pipe's write end open in a child, so the parent's reader never sees EOF, and it
 * holds kernel objects the child has no business with.
 *
 * POSIX. The deterministic check snapshots the open descriptors, creates a loop, a KlWakeup, a
 * KlThreadPool and a KlDatagram, and requires every NEW descriptor to carry FD_CLOEXEC. It is
 * generic, so a descriptor added to Keel later is covered without editing this test. Where fork/exec
 * is practical, a real exec'd shell also confirms it cannot open any of them.
 *
 * Windows. The wakeup channel's socket handles must not be inheritable (HANDLE_FLAG_INHERIT clear).
 */
#include "utest.h"
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/wakeup.h>
#include <keel/thread_pool.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <string.h>
#include <stdio.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_FD 1024
static void snapshot(unsigned char open_fd[MAX_FD]) {
    for (int fd = 0; fd < MAX_FD; fd++) open_fd[fd] = (fcntl(fd, F_GETFD) != -1);
}

UTEST(cloexec, every_descriptor_keel_creates_is_close_on_exec) {
    static unsigned char before[MAX_FD], after[MAX_FD];
    snapshot(before);

    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);              /* epoll / kqueue / io_uring ring */
    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);                       /* the run-loop wakeup pipe */
    KlThreadPoolConfig pc; memset(&pc, 0, sizeof pc); pc.num_workers = 1;
    KlThreadPool *pool = kl_thread_pool_create(&ev, &pc);   /* its own wakeup */
    ASSERT_TRUE(pool != NULL);
    KlDatagram dg;
    KlDatagramSocketConfig dc; memset(&dc, 0, sizeof dc);
    dc.ctx = &ev; dc.alloc = &a; dc.bind_addr = "127.0.0.1";
    ASSERT_EQ(kl_datagram_socket_init(&dg, &dc), 0);       /* a socket, through the provider */

    snapshot(after);
    int fresh = 0, leaking = 0;
    int list[64]; int nlist = 0;
    for (int fd = 0; fd < MAX_FD; fd++) {
        if (!after[fd] || before[fd]) continue;
        fresh++;
        if (nlist < 64) list[nlist++] = fd;
        if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC)) {
            leaking++;
            fprintf(stderr, "  fd %d created by Keel is NOT close-on-exec\n", fd);
        }
    }
    ASSERT_GE(fresh, 3);                                    /* the check really saw Keel's fds */
    ASSERT_EQ(leaking, 0);

#if !defined(__COSMOPOLITAN__)
    /* The real consequence: an exec'd child must not be able to use any of them. */
    char script[1024]; size_t off = 0;
    off += (size_t)snprintf(script + off, sizeof script - off, "for fd in");
    for (int i = 0; i < nlist; i++) off += (size_t)snprintf(script + off, sizeof script - off, " %d", list[i]);
    snprintf(script + off, sizeof script - off,
             "; do if (: <&$fd) 2>/dev/null || (: >&$fd) 2>/dev/null; then exit 1; fi; done; exit 0");
    pid_t pid = fork();
    ASSERT_GE((int)pid, 0);
    if (pid == 0) { execl("/bin/sh", "sh", "-c", script, (char *)NULL); _exit(2); }
    int status = 0;
    ASSERT_EQ((int)waitpid(pid, &status, 0), (int)pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);                      /* 1 = the child could see one */
#endif

    kl_datagram_close_begin(&dg);
    for (int i = 0; i < 200 && kl_datagram_close_state(&dg) != KL_DGRAM_CLOSE_CLOSED; i++)
        (void)kl_event_ctx_run(&ev, 16, 10);
    ASSERT_EQ(kl_datagram_free(&dg), 0);
    kl_thread_pool_free(pool);
    kl_wakeup_close(&w);
    kl_event_ctx_free(&ev);
}

#else  /* _WIN32 */

UTEST(cloexec, wakeup_handles_are_not_inheritable) {
    KlWakeup w;
    ASSERT_EQ(kl_wakeup_open(&w), 0);
    DWORD f = 0;
    ASSERT_TRUE(GetHandleInformation((HANDLE)(uintptr_t)w.rd, &f));
    ASSERT_EQ((int)(f & HANDLE_FLAG_INHERIT), 0);
    ASSERT_TRUE(GetHandleInformation((HANDLE)(uintptr_t)w.wr, &f));
    ASSERT_EQ((int)(f & HANDLE_FLAG_INHERIT), 0);
    kl_wakeup_close(&w);
}

#endif

UTEST_MAIN();
