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
 *
 * Both. The client transports (sync, async, WebSocket) create their socket through the provider seam
 * and must mark it through the seam too (kl_sock_set_cloexec), because a custom provider's socket op
 * need not be close-on-exec at creation. A probe provider hands out a deliberately INHERITABLE socket
 * and, at connect time, records whether the client has marked it by then.
 */
#include "utest.h"
#include "net_compat.h"   /* platform socket headers (winsock2 before windows.h) */
#include <keel/keel.h>
#include <keel/event_ctx.h>
#include <keel/wakeup.h>
#include <keel/thread_pool.h>
#include <keel/datagram.h>
#include <keel/datagram_detail.h>
#include <keel/socket.h>
#include <keel/http_client.h>
#include <keel/websocket_client.h>
#include "event_caps.h"   /* kl_event_caps: the async probes need a readiness loop */
#include <errno.h>
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

/* ── The client transports mark their socket through the provider seam ── */

static int g_created, g_connects, g_inheritable_at_connect;

static int handle_inheritable(KlSocketHandle fd) {
#if defined(_WIN32)
    DWORD f = 0;
    if (!GetHandleInformation((HANDLE)(uintptr_t)fd, &f)) return -1;
    return (f & HANDLE_FLAG_INHERIT) ? 1 : 0;
#else
    int f = fcntl((int)fd, F_GETFD);
    if (f < 0) return -1;
    return (f & FD_CLOEXEC) ? 0 : 1;
#endif
}

/* A plain socket, forced INHERITABLE: what a custom provider may legitimately hand back. */
static KlSocketHandle probe_socket(void *ctx, int domain, int type, int protocol) {
    (void)ctx;
#if defined(_WIN32)
    SOCKET s = socket(domain, type, protocol);
    if (s == INVALID_SOCKET) return KL_INVALID_SOCKET;
    (void)SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
#else
    int s = socket(domain, type, protocol);
    if (s < 0) return KL_INVALID_SOCKET;
    int f = fcntl(s, F_GETFD);
    if (f >= 0) (void)fcntl(s, F_SETFD, f & ~FD_CLOEXEC);
#endif
    KlSocketHandle fd = (KlSocketHandle)s;
    if (handle_inheritable(fd) == 1) g_created++;   /* the probe really starts out inheritable */
    return fd;
}

/* Record the socket's state when the client connects, then refuse (nothing listens). */
static int probe_connect(void *ctx, KlSocketHandle fd, const KlSockAddr *addr) {
    (void)ctx; (void)addr;
    g_connects++;
    if (handle_inheritable(fd) != 0) g_inheritable_at_connect++;
#if defined(_WIN32)
    WSASetLastError(WSAECONNREFUSED);
#endif
    errno = ECONNREFUSED;
    return -1;
}

static const KlSocketOps g_probe_ops = { .name = "cloexec-probe", .socket = probe_socket,
                                         .connect = probe_connect };
static const KlSocketProvider g_probe = { &g_probe_ops, NULL, KL_SOCK_CAP_NATIVE_FD, NULL };

static void probe_reset(void) { g_created = 0; g_connects = 0; g_inheritable_at_connect = 0; }

UTEST(cloexec, sync_client_marks_its_socket) {
    probe_reset();
    KlAllocator a = kl_allocator_default();
    KlHttpClientConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.sockets = &g_probe; cfg.timeout_ms = 2000;
    KlHttpClientResponse resp;
    int rc = kl_http_client_request(&a, &cfg, "GET", "http://127.0.0.1:9/", NULL, 0, NULL, 0, &resp);
    if (rc == 0) kl_http_client_response_free(&resp);
    ASSERT_GE(g_created, 1);
    ASSERT_GE(g_connects, 1);
    ASSERT_EQ(g_inheritable_at_connect, 0);   /* marked before connect: never handed to a child */
}

static int g_async_done;
static void async_done(KlHttpClient *c, void *ud) { (void)c; (void)ud; g_async_done = 1; }

UTEST(cloexec, async_client_marks_its_socket) {
    probe_reset();
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    if (!(kl_event_caps(kl_event_ctx_loop(&ev)) & KL_EVENT_CAP_READINESS)) {
        kl_event_ctx_free(&ev);   /* a native-fd probe cannot drive a completion loop */
        return;
    }
    ev.sockets = &g_probe;
    g_async_done = 0;
    KlHttpClient *c = kl_http_client_start(&ev, &a, NULL, "GET", "http://127.0.0.1:9/",
                                           NULL, 0, NULL, 0, async_done, NULL);
    for (int i = 0; i < 200 && c && !g_async_done; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    if (c) kl_http_client_free(c);
    kl_event_ctx_free(&ev);
    ASSERT_GE(g_created, 1);
    ASSERT_GE(g_connects, 1);
    ASSERT_EQ(g_inheritable_at_connect, 0);
}

static int g_ws_done;
static void ws_on_error(KlWsClientConn *ws, const char *msg, void *ud) {
    (void)ws; (void)msg; (void)ud; g_ws_done = 1;
}

UTEST(cloexec, websocket_client_marks_its_socket) {
    probe_reset();
    KlAllocator a = kl_allocator_default();
    KlEventCtx ev;
    ASSERT_EQ(kl_event_ctx_init(&ev, &a), 0);
    if (!(kl_event_caps(kl_event_ctx_loop(&ev)) & KL_EVENT_CAP_READINESS)) {
        kl_event_ctx_free(&ev);
        return;
    }
    ev.sockets = &g_probe;
    g_ws_done = 0;
    KlWsClientCallbacks cbs; memset(&cbs, 0, sizeof cbs);
    cbs.on_error = ws_on_error;
    KlWsClientConn *ws = kl_ws_client_connect(&ev, &a, NULL, "ws://127.0.0.1:9/", &cbs, NULL);
    for (int i = 0; i < 200 && ws && !g_ws_done; i++) (void)kl_event_ctx_run(&ev, 16, 10);
    if (ws) kl_ws_client_free(ws);
    kl_event_ctx_free(&ev);
    ASSERT_GE(g_created, 1);
    ASSERT_GE(g_connects, 1);
    ASSERT_EQ(g_inheritable_at_connect, 0);
}

UTEST_MAIN();
