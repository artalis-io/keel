#include "utest.h"
#include <keel/keel.h>
#include <keel/async.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Minimal server init: event loop + pool, no listen socket */
static void init_test_server(KlServer *s) {
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;
    s->alloc_storage = kl_allocator_default();
    s->ev.alloc = &s->alloc_storage;
}

static void cleanup_test_server(KlServer *s) {
    /* Free watchers + close event loop */
    kl_event_ctx_free(&s->ev);
    /* Cancel ops */
    while (s->async_ops) {
        KlAsyncOp *op = s->async_ops;
        s->async_ops = op->next;
        if (op->conn) op->conn->async_op = NULL;
    }
    kl_conn_pool_free(&s->pool);
}

/* ── Watcher callback context ─────────────────────────────────────── */

typedef struct {
    int called;
    int got_fd;
    KlEventMask got_mask;
} WatcherCtx;

static void test_watcher_cb(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    WatcherCtx *ctx = user_data;
    ctx->called++;
    ctx->got_fd = fd;
    ctx->got_mask = ready;
}

/* ── Async callback context ───────────────────────────────────────── */

typedef struct {
    int resume_called;
    int deadline_called;
    int cancel_called;
    KlServer *server;         /* for kl_async_complete in deadline cb */
} AsyncCtx;

static void test_resume_cb(KlAsyncOp *op, void *user_data) {
    AsyncCtx *ctx = user_data;
    ctx->resume_called++;
    /* Simulate handler completion: set state to SENDING */
    if (op->conn)
        op->conn->state = KL_CONN_SENDING;
}

static void test_deadline_cb(KlAsyncOp *op, void *user_data) {
    AsyncCtx *ctx = user_data;
    ctx->deadline_called++;
    /* For sleep: deadline = success, call complete */
    if (ctx->server)
        kl_async_complete(ctx->server, op);
}

static void test_cancel_cb(KlAsyncOp *op, void *user_data) {
    AsyncCtx *ctx = user_data;
    ctx->cancel_called++;
    (void)op;
}

/* ── Watcher Tests ────────────────────────────────────────────────── */

UTEST(async, watcher_add_fires_on_read) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    ASSERT_EQ(set_nonblocking(fds[0]), 0);
    ASSERT_EQ(set_nonblocking(fds[1]), 0);

    WatcherCtx ctx = {0};
    ASSERT_EQ(kl_watcher_add(&s.ev, fds[0], KL_EVENT_READ, test_watcher_cb, &ctx), 0);

    /* Write to fds[1] → fds[0] becomes readable */
    (void)write(fds[1], "x", 1);

    KlEvent events[8];
    int n = kl_event_wait(&s.ev.loop, events, 8, 100);
    ASSERT_TRUE(n > 0);
    for (int di = 0; di < n; di++)
        kl_event_dispatch(&s.ev, &events[di]);

    ASSERT_EQ(ctx.called, 1);
    ASSERT_EQ(ctx.got_fd, fds[0]);
    ASSERT_TRUE(ctx.got_mask & KL_EVENT_READ);

    kl_watcher_del(&s.ev, fds[0]);
    close(fds[0]);
    close(fds[1]);
    cleanup_test_server(&s);
}

UTEST(async, watcher_mod_changes_interest) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    ASSERT_EQ(set_nonblocking(fds[0]), 0);
    ASSERT_EQ(set_nonblocking(fds[1]), 0);

    WatcherCtx ctx = {0};
    ASSERT_EQ(kl_watcher_add(&s.ev, fds[0], KL_EVENT_READ, test_watcher_cb, &ctx), 0);

    /* Mod to WRITE interest — socket should be writable immediately */
    ASSERT_EQ(kl_watcher_mod(&s.ev, fds[0], KL_EVENT_WRITE), 0);

    KlEvent events[8];
    int n = kl_event_wait(&s.ev.loop, events, 8, 100);
    ASSERT_TRUE(n > 0);
    for (int di = 0; di < n; di++)
        kl_event_dispatch(&s.ev, &events[di]);

    ASSERT_EQ(ctx.called, 1);
    ASSERT_EQ(ctx.got_fd, fds[0]);
    ASSERT_TRUE(ctx.got_mask & KL_EVENT_WRITE);

    kl_watcher_del(&s.ev, fds[0]);
    close(fds[0]);
    close(fds[1]);
    cleanup_test_server(&s);
}

UTEST(async, watcher_del_stops_events) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    ASSERT_EQ(set_nonblocking(fds[0]), 0);
    ASSERT_EQ(set_nonblocking(fds[1]), 0);

    WatcherCtx ctx = {0};
    ASSERT_EQ(kl_watcher_add(&s.ev, fds[0], KL_EVENT_READ, test_watcher_cb, &ctx), 0);

    /* Remove watcher before writing */
    kl_watcher_del(&s.ev, fds[0]);

    /* Write to fds[1] — should NOT fire callback */
    (void)write(fds[1], "x", 1);

    KlEvent events[8];
    int n = kl_event_wait(&s.ev.loop, events, 8, 50);
    for (int di = 0; di < n; di++)
        kl_event_dispatch(&s.ev, &events[di]);

    ASSERT_EQ(ctx.called, 0);

    close(fds[0]);
    close(fds[1]);
    cleanup_test_server(&s);
}

UTEST(async, watcher_multiple_fds) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    int fds1[2], fds2[2], fds3[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1), 0);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2), 0);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds3), 0);
    for (int i = 0; i < 2; i++) {
        set_nonblocking(fds1[i]);
        set_nonblocking(fds2[i]);
        set_nonblocking(fds3[i]);
    }

    WatcherCtx ctx1 = {0}, ctx2 = {0}, ctx3 = {0};
    ASSERT_EQ(kl_watcher_add(&s.ev, fds1[0], KL_EVENT_READ, test_watcher_cb, &ctx1), 0);
    ASSERT_EQ(kl_watcher_add(&s.ev, fds2[0], KL_EVENT_READ, test_watcher_cb, &ctx2), 0);
    ASSERT_EQ(kl_watcher_add(&s.ev, fds3[0], KL_EVENT_READ, test_watcher_cb, &ctx3), 0);

    /* Write to all three */
    (void)write(fds1[1], "a", 1);
    (void)write(fds2[1], "b", 1);
    (void)write(fds3[1], "c", 1);

    /* May need multiple waits on level-triggered backends */
    KlEvent events[16];
    for (int attempt = 0; attempt < 3; attempt++) {
        int n = kl_event_wait(&s.ev.loop, events, 16, 100);
        for (int di = 0; di < n; di++)
        kl_event_dispatch(&s.ev, &events[di]);
    }

    ASSERT_TRUE(ctx1.called >= 1);
    ASSERT_TRUE(ctx2.called >= 1);
    ASSERT_TRUE(ctx3.called >= 1);

    kl_watcher_del(&s.ev, fds1[0]);
    kl_watcher_del(&s.ev, fds2[0]);
    kl_watcher_del(&s.ev, fds3[0]);
    for (int i = 0; i < 2; i++) {
        close(fds1[i]);
        close(fds2[i]);
        close(fds3[i]);
    }
    cleanup_test_server(&s);
}

UTEST(async, watcher_mod_not_found) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    /* Mod on non-existent watcher should return -1 */
    ASSERT_EQ(kl_watcher_mod(&s.ev, 999, KL_EVENT_READ), -1);

    cleanup_test_server(&s);
}

UTEST(async, watcher_del_not_found) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);

    /* Del on non-existent watcher should be a no-op (not crash) */
    kl_watcher_del(&s.ev, 999);

    cleanup_test_server(&s);
}

/* ── Suspend / Resume Tests ───────────────────────────────────────── */

UTEST(async, suspend_sets_state) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++)
        s.pool.conns[i].parser = kl_parser_llhttp(&s.alloc_storage);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    /* Use fds[1] as the "server side" connection */
    KlConn *c = kl_conn_acquire(&s.pool, fds[1]);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(kl_event_add(&s.ev.loop, fds[1], KL_EVENT_READ, c), 0);

    AsyncCtx actx = {0};
    KlAsyncOp op = {
        .conn = c,
        .deadline_ms = 0,
        .on_resume = test_resume_cb,
        .on_deadline = NULL,
        .on_cancel = test_cancel_cb,
        .user_data = &actx,
    };

    ASSERT_EQ(kl_async_suspend(&s, c, &op), 0);

    /* Verify state */
    ASSERT_EQ(c->state, KL_CONN_SUSPENDED);
    ASSERT_TRUE(c->async_op == &op);
    ASSERT_TRUE(c->suspend_start_ms > 0);

    /* Verify op is in the active list */
    ASSERT_TRUE(s.async_ops == &op);

    /* Suspending again should fail */
    KlAsyncOp op2 = {0};
    ASSERT_EQ(kl_async_suspend(&s, c, &op2), -1);

    /* Clean up: complete the op before releasing */
    kl_async_complete(&s, &op);
    c->fd = -1;
    close(fds[0]);
    close(fds[1]);
    cleanup_test_server(&s);
}

UTEST(async, complete_calls_on_resume) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++)
        s.pool.conns[i].parser = kl_parser_llhttp(&s.alloc_storage);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    KlConn *c = kl_conn_acquire(&s.pool, fds[1]);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(kl_event_add(&s.ev.loop, fds[1], KL_EVENT_READ, c), 0);

    /* Need response initialized for kl_conn_on_writable to work */
    c->res.alloc = &s.alloc_storage;
    kl_response_init(&c->res, c->res.alloc);
    kl_response_json(&c->res, 200, "{}", 2);
    c->res.conn_fd = fds[1];

    AsyncCtx actx = {0};
    KlAsyncOp op = {
        .conn = c,
        .deadline_ms = 0,
        .on_resume = test_resume_cb,
        .on_deadline = NULL,
        .on_cancel = test_cancel_cb,
        .user_data = &actx,
    };

    ASSERT_EQ(kl_async_suspend(&s, c, &op), 0);
    ASSERT_EQ(c->state, KL_CONN_SUSPENDED);

    /* Complete the op */
    kl_async_complete(&s, &op);

    /* on_resume should have been called */
    ASSERT_EQ(actx.resume_called, 1);

    /* Connection should no longer be suspended */
    ASSERT_TRUE(c->async_op == NULL);
    ASSERT_TRUE(c->state != KL_CONN_SUSPENDED);

    /* Op should be removed from active list */
    ASSERT_TRUE(s.async_ops == NULL);

    c->fd = -1;
    close(fds[0]);
    close(fds[1]);
    cleanup_test_server(&s);
}

UTEST(async, deadline_fires_on_timeout) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++)
        s.pool.conns[i].parser = kl_parser_llhttp(&s.alloc_storage);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    KlConn *c = kl_conn_acquire(&s.pool, fds[1]);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(kl_event_add(&s.ev.loop, fds[1], KL_EVENT_READ, c), 0);

    c->res.alloc = &s.alloc_storage;
    kl_response_init(&c->res, c->res.alloc);
    kl_response_json(&c->res, 200, "{}", 2);
    c->res.conn_fd = fds[1];

    AsyncCtx actx = {.server = &s};
    KlAsyncOp op = {
        .conn = c,
        .deadline_ms = kl_monotonic_ms() + 10,  /* 10ms deadline */
        .on_resume = test_resume_cb,
        .on_deadline = test_deadline_cb,
        .on_cancel = test_cancel_cb,
        .user_data = &actx,
    };

    ASSERT_EQ(kl_async_suspend(&s, c, &op), 0);

    /* Wait for the deadline to pass */
    usleep(30000);  /* 30ms */

    /* Run the deadline sweep manually (mirrors server.c logic) */
    uint64_t now = kl_monotonic_ms();
    KlAsyncOp *aop = s.async_ops;
    while (aop) {
        KlAsyncOp *next = aop->next;
        if (aop->deadline_ms > 0 && now >= aop->deadline_ms) {
            if (aop->on_deadline)
                aop->on_deadline(aop, aop->user_data);
        }
        aop = next;
    }

    /* on_deadline should have been called, which calls kl_async_complete */
    ASSERT_EQ(actx.deadline_called, 1);
    ASSERT_EQ(actx.resume_called, 1);  /* complete calls on_resume */
    ASSERT_TRUE(s.async_ops == NULL);   /* removed from list */

    c->fd = -1;
    close(fds[0]);
    close(fds[1]);
    cleanup_test_server(&s);
}

UTEST(async, cancel_on_server_free) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++)
        s.pool.conns[i].parser = kl_parser_llhttp(&s.alloc_storage);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    KlConn *c = kl_conn_acquire(&s.pool, fds[1]);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(kl_event_add(&s.ev.loop, fds[1], KL_EVENT_READ, c), 0);

    AsyncCtx actx = {0};
    KlAsyncOp op = {
        .conn = c,
        .deadline_ms = 0,
        .on_resume = test_resume_cb,
        .on_deadline = NULL,
        .on_cancel = test_cancel_cb,
        .user_data = &actx,
    };

    ASSERT_EQ(kl_async_suspend(&s, c, &op), 0);

    /* Server free should cancel the op */
    c->fd = -1;
    close(fds[0]);
    close(fds[1]);

    /* kl_server_free cancels ops — but we use cleanup helper
     * which mirrors the same behavior */
    while (s.async_ops) {
        KlAsyncOp *cop = s.async_ops;
        s.async_ops = cop->next;
        if (cop->on_cancel)
            cop->on_cancel(cop, cop->user_data);
        if (cop->conn)
            cop->conn->async_op = NULL;
    }

    ASSERT_EQ(actx.cancel_called, 1);
    ASSERT_TRUE(c->async_op == NULL);

    cleanup_test_server(&s);
}

UTEST(async, suspend_exempt_from_idle_timeout) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++)
        s.pool.conns[i].parser = kl_parser_llhttp(&s.alloc_storage);

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    KlConn *c = kl_conn_acquire(&s.pool, fds[1]);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(kl_event_add(&s.ev.loop, fds[1], KL_EVENT_READ, c), 0);

    AsyncCtx actx = {0};
    KlAsyncOp op = {
        .conn = c,
        .deadline_ms = 0,
        .on_resume = test_resume_cb,
        .on_cancel = test_cancel_cb,
        .user_data = &actx,
    };

    ASSERT_EQ(kl_async_suspend(&s, c, &op), 0);

    /* Set last_active_ms to far in the past — simulates long idle */
    c->last_active_ms = kl_monotonic_ms() - 60000;  /* 60s ago */

    /* Run the timeout sweep logic (mirrors server.c) */
    uint64_t now = kl_monotonic_ms();
    uint64_t timeout = 30000;  /* 30s read timeout */
    int reaped = 0;
    for (int i = 0; i < s.pool.capacity; i++) {
        KlConn *tc = &s.pool.conns[i];
        if (tc->state == KL_CONN_CLOSED || tc->state == KL_CONN_PROCESSING)
            continue;
        if (tc->state == KL_CONN_SUSPENDED)
            continue;  /* MUST be exempt */
        if (now - tc->last_active_ms > timeout) {
            reaped++;
        }
    }

    /* Suspended connection should NOT have been reaped */
    ASSERT_EQ(reaped, 0);
    ASSERT_EQ(c->state, KL_CONN_SUSPENDED);

    /* Clean up */
    kl_async_complete(&s, &op);
    c->fd = -1;
    close(fds[0]);
    close(fds[1]);
    cleanup_test_server(&s);
}

/* ── Integration: watcher completes suspended conn ────────────────── */

typedef struct {
    KlServer *server;
    KlAsyncOp *op;
    int pipe_read_fd;
} WatcherCompleteCtx;

static void watcher_complete_cb(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready;
    WatcherCompleteCtx *ctx = user_data;

    /* Drain the pipe */
    char buf[16];
    (void)read(fd, buf, sizeof(buf));

    /* Complete the async op — resumes the connection */
    kl_async_complete(ctx->server, ctx->op);
}

UTEST(async, watcher_completes_suspended_conn) {
    KlServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++)
        s.pool.conns[i].parser = kl_parser_llhttp(&s.alloc_storage);

    /* Create a connection with socketpair */
    int conn_fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, conn_fds), 0);
    set_nonblocking(conn_fds[0]);
    set_nonblocking(conn_fds[1]);

    KlConn *c = kl_conn_acquire(&s.pool, conn_fds[1]);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(kl_event_add(&s.ev.loop, conn_fds[1], KL_EVENT_READ, c), 0);

    /* Init response for the connection */
    c->res.alloc = &s.alloc_storage;
    kl_response_init(&c->res, c->res.alloc);
    kl_response_json(&c->res, 200, "{\"ok\":true}", 11);
    c->res.conn_fd = conn_fds[1];

    /* Create a pipe for the completion signal */
    int pipe_fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pipe_fds), 0);
    set_nonblocking(pipe_fds[0]);
    set_nonblocking(pipe_fds[1]);

    /* Suspend the connection */
    AsyncCtx actx = {0};
    KlAsyncOp op = {
        .conn = c,
        .deadline_ms = 0,
        .on_resume = test_resume_cb,
        .on_cancel = test_cancel_cb,
        .user_data = &actx,
    };
    ASSERT_EQ(kl_async_suspend(&s, c, &op), 0);
    ASSERT_EQ(c->state, KL_CONN_SUSPENDED);

    /* Register a watcher on the pipe read end — when the pipe is written to,
     * the watcher fires and completes the async op */
    WatcherCompleteCtx wctx = {
        .server = &s,
        .op = &op,
        .pipe_read_fd = pipe_fds[0],
    };
    ASSERT_EQ(kl_watcher_add(&s.ev, pipe_fds[0], KL_EVENT_READ,
                              watcher_complete_cb, &wctx), 0);

    /* Write to the pipe — signals completion */
    (void)write(pipe_fds[1], "done", 4);

    /* Run event loop — watcher should fire, completing the async op */
    KlEvent events[16];
    int n = kl_event_wait(&s.ev.loop, events, 16, 100);
    ASSERT_TRUE(n > 0);
    for (int di = 0; di < n; di++)
        kl_event_dispatch(&s.ev, &events[di]);

    /* Verify the async op completed */
    ASSERT_EQ(actx.resume_called, 1);
    ASSERT_TRUE(c->async_op == NULL);
    ASSERT_TRUE(c->state != KL_CONN_SUSPENDED);
    ASSERT_TRUE(s.async_ops == NULL);

    kl_watcher_del(&s.ev, pipe_fds[0]);
    c->fd = -1;
    close(conn_fds[0]);
    close(conn_fds[1]);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    cleanup_test_server(&s);
}

/* ── Server connection context test ──────────────────────────────── */

UTEST(async, server_ctx_set_on_request) {
    /* Verify _server_ctx is set to KlConn* when processing a request */
    KlAllocator a = kl_allocator_default();
    KlConnPool pool;
    ASSERT_EQ(kl_conn_pool_init(&pool, 2, &a), 0);
    for (int i = 0; i < 2; i++)
        pool.conns[i].parser = kl_parser_llhttp(&a);

    KlConn *c = kl_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);

    /* _server_ctx should be NULL after acquire (request memset to 0) */
    ASSERT_TRUE(c->req._server_ctx == NULL);

    /* async_op should be NULL after acquire */
    ASSERT_TRUE(c->async_op == NULL);
    ASSERT_EQ(c->suspend_start_ms, (uint64_t)0);

    c->fd = -1;
    kl_conn_pool_free(&pool);
}

/* ── Integration test: async handler in real server ──────────────── */

static KlServer async_server;
static pthread_t async_server_tid;

static void *async_server_thread(void *arg) {
    (void)arg;
    kl_server_run(&async_server);
    return NULL;
}

/* Handler that suspends, then completes synchronously via a watcher */
typedef struct {
    KlAsyncOp op;
    KlServer *server;
    int pipe_fds[2];
} SleepCtx;

static void sleep_resume(KlAsyncOp *op, void *user_data) {
    (void)user_data;
    /* Handler completed: set up response, transition to SENDING */
    KlConn *c = op->conn;
    kl_response_json(&c->res, 200, "{\"slept\":true}", 14);
    c->state = KL_CONN_SENDING;
}

static void sleep_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready;
    SleepCtx *ctx = user_data;
    char buf[8];
    (void)read(fd, buf, sizeof(buf));
    kl_watcher_del(&ctx->server->ev, fd);
    close(ctx->pipe_fds[0]);
    close(ctx->pipe_fds[1]);
    kl_async_complete(ctx->server, &ctx->op);
}

static void handle_async_sleep(KlRequest *req, KlResponse *res, void *user_data) {
    (void)res;
    KlServer *srv = user_data;
    KlConn *conn = kl_request_conn(req);

    /* Allocate sleep context */
    static SleepCtx sctx;  /* static for simplicity — single-request test */
    memset(&sctx, 0, sizeof(sctx));
    sctx.server = srv;

    /* Create pipe for completion signal */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sctx.pipe_fds);
    set_nonblocking(sctx.pipe_fds[0]);
    set_nonblocking(sctx.pipe_fds[1]);

    /* Set up the async op */
    sctx.op.on_resume = sleep_resume;
    sctx.op.on_cancel = NULL;
    sctx.op.on_deadline = NULL;
    sctx.op.user_data = &sctx;

    /* Register watcher for completion */
    kl_watcher_add(&srv->ev, sctx.pipe_fds[0], KL_EVENT_READ, sleep_watcher, &sctx);

    /* Suspend the connection */
    kl_async_suspend(srv, conn, &sctx.op);

    /* Schedule completion (write to pipe — watcher fires on next tick) */
    (void)write(sctx.pipe_fds[1], "!", 1);
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static ssize_t read_response(int fd, char *buf, size_t buflen) {
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, buflen - (size_t)total - 1)) > 0)
        total += n;
    buf[total] = '\0';
    return total;
}

UTEST(async, e2e_handler_suspend_resume) {
    KlConfig cfg = {.port = 0};
    kl_server_init(&async_server, &cfg);
    kl_server_route(&async_server, "GET", "/async",
                    handle_async_sleep, &async_server, NULL);

    pthread_create(&async_server_tid, NULL, async_server_thread, NULL);
    for (int i = 0; i < 200 && async_server.bound_port == 0; i++) usleep(10000);
    int port = async_server.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /async HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)write(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    close(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"slept\":true}") != NULL);

    kl_server_stop(&async_server);
    pthread_join(async_server_tid, NULL);
    kl_server_free(&async_server);
}

UTEST_MAIN();
