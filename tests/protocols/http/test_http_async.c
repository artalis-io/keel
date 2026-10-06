/* test_http_async.c: HTTP KlAsyncOp/server-suspension slice of the async tests.
 *
 * This is the HTTP half of the async test family: the cases here
 * exercise connection suspension/resume, deadlines, cancellation, the
 * exactly-one-terminal guarantees, and an end-to-end async handler in a real
 * KlHttpServer. The generic KlWatcher cases (which touch no HTTP connection
 * state) live in tests/test_async.c.
 */

#include "utest.h"
#include "../../../src/protocols/http/http_conn_internal.h"
#include <keel/clock.h>
#include <keel/keel.h>
#include <keel/async.h>
#include "net_compat.h"
#include <string.h>
#include "platform_thread.h"   /* Keel PAL threads: portable to MSVC */

/* ── Helpers ─────────────────────────────────────────────────────── */

static int set_nonblocking(int fd) {
    return kl_test_set_nonblock(fd);
}

/* Minimal server init: event loop + pool, no listen socket */
static void init_test_server(KlHttpServer *s) {
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;
    s->alloc_storage = kl_allocator_default();
    s->ev.alloc = &s->alloc_storage;
}

static void cleanup_test_server(KlHttpServer *s) {
    /* Free watchers + close event loop */
    kl_event_ctx_free(&s->ev);
    /* Cancel ops */
    while (s->async_ops) {
        KlAsyncOp *op = s->async_ops;
        s->async_ops = op->next;
        if (op->conn) op->conn->async_op = NULL;
    }
    kl_http_conn_pool_free(&s->pool);
}

/* Pump the event loop until *flag reaches `want` (or attempts run out). Uses
 * kl_event_ctx_run: the portable tick (wait + dispatch watchers) that works on
 * BOTH the readiness and completion event models, so these watcher/async tests are
 * backend-agnostic instead of hard-coding a readiness kl_event_wait loop. */
static void pump_until(KlEventCtx *ev, const int *flag, int want) {
    for (int a = 0; a < 20 && *flag < want; a++)
        kl_event_ctx_run(ev, 16, 50);
}

/* ── Async callback context ───────────────────────────────────────── */

typedef struct {
    int resume_called;
    int deadline_called;
    int cancel_called;
    KlHttpServer *server;         /* for kl_async_complete in deadline cb */
} AsyncCtx;

static void test_resume_cb(KlAsyncOp *op, void *user_data) {
    AsyncCtx *ctx = user_data;
    ctx->resume_called++;
    /* Simulate handler completion: set state to SENDING */
    if (op->conn)
        op->conn->state = KL_HTTP_CONN_SENDING;
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

/* ── Suspend / Resume Tests ───────────────────────────────────────── */

UTEST(async, suspend_sets_state) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_http_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++) {
        s.pool.conns[i].parser = kl_http1_parser_llhttp(&s.alloc_storage);
        s.pool.conns[i].stream.ctx = &s.ev;   /* mirror http_server.c:419: a conn must know its event ctx */
    }

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    /* Use fds[1] as the "server side" connection */
    KlHttpConn *c = kl_http_conn_acquire(&s.pool, fds[1]);
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
    ASSERT_EQ(c->state, KL_HTTP_CONN_SUSPENDED);
    ASSERT_TRUE(c->async_op == &op);
    ASSERT_TRUE(c->suspend_start_ms > 0);

    /* Verify op is in the active list */
    ASSERT_TRUE(s.async_ops == &op);

    /* Suspending again should fail */
    KlAsyncOp op2 = {0};
    ASSERT_EQ(kl_async_suspend(&s, c, &op2), -1);

    /* Clean up: complete the op before releasing */
    kl_async_complete(&s, &op);
    c->stream.fd = -1;
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    cleanup_test_server(&s);
}

UTEST(async, complete_calls_on_resume) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_http_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++) {
        s.pool.conns[i].parser = kl_http1_parser_llhttp(&s.alloc_storage);
        s.pool.conns[i].stream.ctx = &s.ev;   /* mirror http_server.c:419: a conn must know its event ctx */
    }

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    KlHttpConn *c = kl_http_conn_acquire(&s.pool, fds[1]);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(kl_event_add(&s.ev.loop, fds[1], KL_EVENT_READ, c), 0);

    /* Need response initialized for kl_http_conn_on_writable to work */
    c->res.alloc = &s.alloc_storage;
    kl_http_response_init(&c->res, c->res.alloc);
    kl_http_response_json(&c->res, 200, "{}", 2);
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
    ASSERT_EQ(c->state, KL_HTTP_CONN_SUSPENDED);

    /* Complete the op */
    kl_async_complete(&s, &op);

    /* on_resume should have been called */
    ASSERT_EQ(actx.resume_called, 1);

    /* Connection should no longer be suspended */
    ASSERT_TRUE(c->async_op == NULL);
    ASSERT_TRUE(c->state != KL_HTTP_CONN_SUSPENDED);

    /* Op should be removed from active list */
    ASSERT_TRUE(s.async_ops == NULL);

    c->stream.fd = -1;
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    cleanup_test_server(&s);
}

UTEST(async, deadline_fires_on_timeout) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_http_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++) {
        s.pool.conns[i].parser = kl_http1_parser_llhttp(&s.alloc_storage);
        s.pool.conns[i].stream.ctx = &s.ev;   /* mirror http_server.c:419: a conn must know its event ctx */
    }

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    KlHttpConn *c = kl_http_conn_acquire(&s.pool, fds[1]);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(kl_event_add(&s.ev.loop, fds[1], KL_EVENT_READ, c), 0);

    c->res.alloc = &s.alloc_storage;
    kl_http_response_init(&c->res, c->res.alloc);
    kl_http_response_json(&c->res, 200, "{}", 2);
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
    kl_test_sleep_ms(30);  /* 30ms */

    /* Run the deadline sweep manually (mirrors http_server.c logic) */
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

    c->stream.fd = -1;
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    cleanup_test_server(&s);
}

UTEST(async, cancel_on_server_free) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_http_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++) {
        s.pool.conns[i].parser = kl_http1_parser_llhttp(&s.alloc_storage);
        s.pool.conns[i].stream.ctx = &s.ev;   /* mirror http_server.c:419: a conn must know its event ctx */
    }

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    KlHttpConn *c = kl_http_conn_acquire(&s.pool, fds[1]);
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
    c->stream.fd = -1;
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);

    /* kl_http_server_free cancels ops: but we use cleanup helper
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
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_http_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++) {
        s.pool.conns[i].parser = kl_http1_parser_llhttp(&s.alloc_storage);
        s.pool.conns[i].stream.ctx = &s.ev;   /* mirror http_server.c:419: a conn must know its event ctx */
    }

    int fds[2];
    ASSERT_EQ(kl_test_socketpair(fds), 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);

    KlHttpConn *c = kl_http_conn_acquire(&s.pool, fds[1]);
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

    /* Set last_active_ms to far in the past: simulates long idle */
    c->last_active_ms = kl_monotonic_ms() - 60000;  /* 60s ago */

    /* Run the timeout sweep logic (mirrors http_server.c) */
    uint64_t now = kl_monotonic_ms();
    uint64_t timeout = 30000;  /* 30s read timeout */
    int reaped = 0;
    for (int i = 0; i < s.pool.capacity; i++) {
        KlHttpConn *tc = &s.pool.conns[i];
        if (tc->state == KL_HTTP_CONN_CLOSED || tc->state == KL_HTTP_CONN_PROCESSING)
            continue;
        if (tc->state == KL_HTTP_CONN_SUSPENDED)
            continue;  /* MUST be exempt */
        if (now - tc->last_active_ms > timeout) {
            reaped++;
        }
    }

    /* Suspended connection should NOT have been reaped */
    ASSERT_EQ(reaped, 0);
    ASSERT_EQ(c->state, KL_HTTP_CONN_SUSPENDED);

    /* Clean up */
    kl_async_complete(&s, &op);
    c->stream.fd = -1;
    kl_test_closesock(fds[0]);
    kl_test_closesock(fds[1]);
    cleanup_test_server(&s);
}

/* ── Integration: watcher completes suspended conn ────────────────── */

typedef struct {
    KlHttpServer *server;
    KlAsyncOp *op;
    int pipe_read_fd;
} WatcherCompleteCtx;

static void watcher_complete_cb(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready;
    WatcherCompleteCtx *ctx = user_data;

    /* Drain the pipe */
    char buf[16];
    (void)kl_test_sockread(fd, buf, sizeof(buf));

    /* Complete the async op: resumes the connection */
    kl_async_complete(ctx->server, ctx->op);
}

UTEST(async, watcher_completes_suspended_conn) {
    KlHttpServer s;
    init_test_server(&s);
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);
    ASSERT_EQ(kl_http_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);
    for (int i = 0; i < 4; i++) {
        s.pool.conns[i].parser = kl_http1_parser_llhttp(&s.alloc_storage);
        s.pool.conns[i].stream.ctx = &s.ev;   /* mirror http_server.c:419: a conn must know its event ctx */
    }

    /* Create a connection with socketpair */
    int conn_fds[2];
    ASSERT_EQ(kl_test_socketpair(conn_fds), 0);
    set_nonblocking(conn_fds[0]);
    set_nonblocking(conn_fds[1]);

    KlHttpConn *c = kl_http_conn_acquire(&s.pool, conn_fds[1]);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ(kl_event_add(&s.ev.loop, conn_fds[1], KL_EVENT_READ, c), 0);

    /* Init response for the connection */
    c->res.alloc = &s.alloc_storage;
    kl_http_response_init(&c->res, c->res.alloc);
    kl_http_response_json(&c->res, 200, "{\"ok\":true}", 11);
    c->res.conn_fd = conn_fds[1];

    /* Create a pipe for the completion signal */
    int pipe_fds[2];
    ASSERT_EQ(kl_test_socketpair(pipe_fds), 0);
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
    ASSERT_EQ(c->state, KL_HTTP_CONN_SUSPENDED);

    /* Register a watcher on the pipe read end: when the pipe is written to,
     * the watcher fires and completes the async op */
    WatcherCompleteCtx wctx = {
        .server = &s,
        .op = &op,
        .pipe_read_fd = pipe_fds[0],
    };
    ASSERT_EQ(kl_watcher_add(&s.ev, pipe_fds[0], KL_EVENT_READ,
                              watcher_complete_cb, &wctx), 0);

    /* Write to the pipe: signals completion */
    (void)kl_test_sockwrite(pipe_fds[1], "done", 4);

    /* Run the loop until the watcher fires + completes the async op (portable). */
    pump_until(&s.ev, &actx.resume_called, 1);

    /* Verify the async op completed */
    ASSERT_EQ(actx.resume_called, 1);
    ASSERT_TRUE(c->async_op == NULL);
    ASSERT_TRUE(c->state != KL_HTTP_CONN_SUSPENDED);
    ASSERT_TRUE(s.async_ops == NULL);

    kl_watcher_del(&s.ev, pipe_fds[0]);
    c->stream.fd = -1;
    kl_test_closesock(conn_fds[0]);
    kl_test_closesock(conn_fds[1]);
    kl_test_closesock(pipe_fds[0]);
    kl_test_closesock(pipe_fds[1]);
    cleanup_test_server(&s);
}

/* ── Server connection context test ──────────────────────────────── */

UTEST(async, server_ctx_set_on_request) {
    /* Verify _server_ctx is set to KlHttpConn* when processing a request */
    KlAllocator a = kl_allocator_default();
    KlHttpConnPool pool;
    ASSERT_EQ(kl_http_conn_pool_init(&pool, 2, &a), 0);
    for (int i = 0; i < 2; i++)
        pool.conns[i].parser = kl_http1_parser_llhttp(&a);

    KlHttpConn *c = kl_http_conn_acquire(&pool, 100);
    ASSERT_TRUE(c != NULL);

    /* _server_ctx should be NULL after acquire (request memset to 0) */
    ASSERT_TRUE(c->req._server_ctx == NULL);

    /* async_op should be NULL after acquire */
    ASSERT_TRUE(c->async_op == NULL);
    ASSERT_EQ(c->suspend_start_ms, (uint64_t)0);

    c->stream.fd = -1;
    kl_http_conn_pool_free(&pool);
}

/* ── Integration test: async handler in real server ──────────────── */

static KlHttpServer async_server;
static KlPlatThread async_server_tid;

static void async_server_thread(void *arg) {
    (void)arg;
    kl_http_server_run(&async_server);
}

/* Handler that suspends, then completes synchronously via a watcher */
typedef struct {
    KlAsyncOp op;
    KlHttpServer *server;
    int pipe_fds[2];
} SleepCtx;

static void sleep_resume(KlAsyncOp *op, void *user_data) {
    (void)user_data;
    /* Handler completed: set up response, transition to SENDING */
    KlHttpConn *c = op->conn;
    kl_http_response_json(&c->res, 200, "{\"slept\":true}", 14);
    c->state = KL_HTTP_CONN_SENDING;
}

static void sleep_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready;
    SleepCtx *ctx = user_data;
    char buf[8];
    (void)kl_test_sockread(fd, buf, sizeof(buf));
    kl_watcher_del(&ctx->server->ev, fd);
    kl_test_closesock(ctx->pipe_fds[0]);
    kl_test_closesock(ctx->pipe_fds[1]);
    kl_async_complete(ctx->server, &ctx->op);
}

static void handle_async_sleep(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    (void)res;
    KlHttpServer *srv = user_data;
    KlHttpConn *conn = kl_http_request_conn(req);

    /* Allocate sleep context */
    static SleepCtx sctx;  /* static for simplicity: single-request test */
    memset(&sctx, 0, sizeof(sctx));
    sctx.server = srv;

    /* Create pipe for completion signal */
    kl_test_socketpair(sctx.pipe_fds);
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

    /* Schedule completion (write to pipe: watcher fires on next tick) */
    (void)kl_test_sockwrite(sctx.pipe_fds[1], "!", 1);
}

static int connect_to(int port) {
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kl_test_closesock(fd);
        return -1;
    }
    return fd;
}

static kl_ssize_t read_response(int fd, char *buf, size_t buflen) {
    kl_ssize_t total = 0;
    kl_ssize_t n;
    while ((n = kl_test_sockread(fd, buf + total, buflen - (size_t)total - 1)) > 0)
        total += n;
    buf[total] = '\0';
    return total;
}

UTEST(async, e2e_handler_suspend_resume) {
    KlHttpServerConfig cfg = {.port = 0};
    kl_http_server_init(&async_server, &cfg);
    kl_http_server_route(&async_server, "GET", "/async",
                    handle_async_sleep, &async_server, NULL);

    kl_plat_thread_create(&async_server_tid, async_server_thread, NULL);
    for (int i = 0; i < 200 && async_server.bound_port == 0; i++) kl_test_sleep_ms(10);
    int port = async_server.bound_port;

    int fd = connect_to(port);
    ASSERT_TRUE(fd >= 0);

    const char *req = "GET /async HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)kl_test_sockwrite(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    kl_test_closesock(fd);

    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"slept\":true}") != NULL);

    kl_http_server_stop(&async_server);
    kl_plat_thread_join(&async_server_tid);
    kl_http_server_free(&async_server);
}

/* Regression: a resume that only builds a response, the shape every async example
 * and every public consumer has to use. done_fn writes the response through the
 * public kl_http_conn_response() accessor and on_resume has nothing left to do.
 * Nothing below reaches into KlHttpConn, deliberately: the type is opaque, so a
 * consumer has no way to set the connection state and kl_async_complete has to
 * default it. Without that default the readiness path re-registers no fd (the
 * request hangs) and the completion path closes the socket with no response. */
typedef struct {
    KlAsyncOp op;
    KlHttpServer *server;
    int signal_fds[2];
    int resume_called;
} NoStateCtx;

static NoStateCtx nostate_ctx;

static void nostate_resume(KlAsyncOp *op, void *user_data) {
    (void)op;
    ((NoStateCtx *)user_data)->resume_called++;   /* no state, no response: nothing left to do */
}

static void nostate_watcher(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready;
    NoStateCtx *ctx = user_data;
    char buf[8];
    (void)kl_test_sockread(fd, buf, sizeof(buf));
    kl_watcher_del(kl_http_server_event_ctx(ctx->server), fd);
    kl_test_closesock(ctx->signal_fds[0]);
    kl_test_closesock(ctx->signal_fds[1]);

    /* Public surface only: the response goes through the borrowed-handle accessor. */
    kl_http_response_json(kl_http_conn_response(ctx->op.conn), 200,
                          "{\"resumed\":true}", 16);
    kl_async_complete(ctx->server, &ctx->op);
}

static void handle_async_nostate(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    (void)res;
    KlHttpServer *srv = user_data;
    KlHttpConn *conn = kl_http_request_conn(req);

    memset(&nostate_ctx, 0, sizeof(nostate_ctx));
    nostate_ctx.server = srv;
    nostate_ctx.op.on_resume = nostate_resume;
    nostate_ctx.op.user_data = &nostate_ctx;

    kl_test_socketpair(nostate_ctx.signal_fds);
    set_nonblocking(nostate_ctx.signal_fds[0]);
    set_nonblocking(nostate_ctx.signal_fds[1]);
    kl_watcher_add(kl_http_server_event_ctx(srv), nostate_ctx.signal_fds[0],
                   KL_EVENT_READ, nostate_watcher, &nostate_ctx);

    kl_async_suspend(srv, conn, &nostate_ctx.op);
    (void)kl_test_sockwrite(nostate_ctx.signal_fds[1], "!", 1);
}

UTEST(async, e2e_resume_without_state_still_sends) {
    KlHttpServerConfig cfg = {.port = 0};
    kl_http_server_init(&async_server, &cfg);
    kl_http_server_route(&async_server, "GET", "/nostate",
                    handle_async_nostate, &async_server, NULL);

    kl_plat_thread_create(&async_server_tid, async_server_thread, NULL);
    for (int i = 0; i < 200 && async_server.bound_port == 0; i++) kl_test_sleep_ms(10);

    int fd = connect_to(async_server.bound_port);
    ASSERT_TRUE(fd >= 0);
    /* Bound the read: a regression here stalls the connection forever, and a hung
     * CI job is a worse signal than a failed assertion. */
    kl_test_set_rcvtimeo(fd, 3000);

    const char *req = "GET /nostate HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)kl_test_sockwrite(fd, req, strlen(req));

    char buf[4096];
    read_response(fd, buf, sizeof(buf));
    kl_test_closesock(fd);

    ASSERT_EQ(nostate_ctx.resume_called, 1);
    ASSERT_TRUE(strstr(buf, "200 OK") != NULL);
    ASSERT_TRUE(strstr(buf, "{\"resumed\":true}") != NULL);

    kl_http_server_stop(&async_server);
    kl_plat_thread_join(&async_server_tid);
    kl_http_server_free(&async_server);
}

/* ── Exactly-one-terminal guarantees ─────────────────────────
 * These test the terminal *guard* in async.c, not the post-resume state drive.
 * The resume callback only counts, so kl_async_complete defaults the connection
 * to SENDING and flushes: the setup gives it a real response and a keep-alive
 * request so the drive parks in READING (conn + op stay valid for the
 * idempotency assertions) instead of releasing the connection. */
static void terminal_resume_cb(KlAsyncOp *op, void *ud) {
    (void)op; ((AsyncCtx *)ud)->resume_called++;
}
#define RFC_TERMINAL_SETUP()                                                   \
    KlHttpServer s; init_test_server(&s);                                          \
    ASSERT_EQ(kl_event_ctx_init(&s.ev, &s.alloc_storage), 0);                  \
    ASSERT_EQ(kl_http_conn_pool_init(&s.pool, 4, &s.alloc_storage), 0);             \
    for (int i = 0; i < 4; i++) {                                              \
        s.pool.conns[i].parser = kl_http1_parser_llhttp(&s.alloc_storage);          \
        s.pool.conns[i].stream.ctx = &s.ev;   /* mirror http_server.c:419 */               \
    }                                                                          \
    int fds[2]; ASSERT_EQ(kl_test_socketpair(fds), 0);                         \
    set_nonblocking(fds[0]); set_nonblocking(fds[1]);                          \
    KlHttpConn *c = kl_http_conn_acquire(&s.pool, fds[1]);                              \
    ASSERT_TRUE(c != NULL);                                                    \
    ASSERT_EQ(kl_event_add(&s.ev.loop, fds[1], KL_EVENT_READ, c), 0);          \
    c->res.alloc = &s.alloc_storage;                                           \
    ASSERT_EQ(kl_http_response_init(&c->res, c->res.alloc), 0);                \
    kl_http_response_json(&c->res, 200, "{}", 2);                              \
    c->res.conn_fd = fds[1];                                                   \
    c->req.keep_alive = 1;  /* a completed send parks in READING, not CLOSED */\
    AsyncCtx actx = {0};                                                       \
    KlAsyncOp op = { .conn = c, .on_resume = terminal_resume_cb,              \
                     .on_cancel = test_cancel_cb, .user_data = &actx };        \
    ASSERT_EQ(kl_async_suspend(&s, c, &op), 0)

#define RFC_TERMINAL_TEARDOWN()                                                \
    c->stream.fd = -1; kl_test_closesock(fds[0]); kl_test_closesock(fds[1]);          \
    cleanup_test_server(&s)

/* A: double complete fires on_resume exactly once. */
UTEST(async, complete_twice_is_idempotent) {
    RFC_TERMINAL_SETUP();
    kl_async_complete(&s, &op);
    kl_async_complete(&s, &op);                 /* second call: no-op */
    ASSERT_EQ(actx.resume_called, 1);
    ASSERT_EQ(actx.cancel_called, 0);
    ASSERT_TRUE(s.async_ops == NULL);
    ASSERT_TRUE(op._terminal == 1);
    RFC_TERMINAL_TEARDOWN();
}

/* Cancel is idempotent: on_cancel fires exactly once. */
UTEST(async, cancel_twice_is_idempotent) {
    RFC_TERMINAL_SETUP();
    kl_async_cancel(&s, &op);
    kl_async_cancel(&s, &op);                   /* second call: no-op */
    ASSERT_EQ(actx.cancel_called, 1);
    ASSERT_EQ(actx.resume_called, 0);
    ASSERT_TRUE(s.async_ops == NULL);
    ASSERT_TRUE(c->async_op == NULL);
    RFC_TERMINAL_TEARDOWN();
}

/* C, cancel racing a completion: complete wins, later cancel is a no-op. */
UTEST(async, cancel_after_complete_is_noop) {
    RFC_TERMINAL_SETUP();
    kl_async_complete(&s, &op);
    kl_async_cancel(&s, &op);                   /* already terminal → no-op */
    ASSERT_EQ(actx.resume_called, 1);
    ASSERT_EQ(actx.cancel_called, 0);
    RFC_TERMINAL_TEARDOWN();
}

/* C (other order): cancel wins, later completion is a no-op (no resume). */
UTEST(async, complete_after_cancel_is_noop) {
    RFC_TERMINAL_SETUP();
    kl_async_cancel(&s, &op);
    kl_async_complete(&s, &op);                 /* already terminal → no-op */
    ASSERT_EQ(actx.cancel_called, 1);
    ASSERT_EQ(actx.resume_called, 0);
    RFC_TERMINAL_TEARDOWN();
}

/* Op reuse: after a terminal transition, a fresh suspend makes it pending again. */
UTEST(async, resuspend_after_terminal_is_pending) {
    RFC_TERMINAL_SETUP();
    kl_async_complete(&s, &op);
    ASSERT_EQ(actx.resume_called, 1);
    ASSERT_TRUE(op._terminal == 1);
    /* Reuse the same op struct for a new suspension (conn is back to PROCESSING). */
    ASSERT_EQ(kl_async_suspend(&s, c, &op), 0);
    ASSERT_TRUE(op._terminal == 0);             /* suspend re-armed it */
    kl_async_complete(&s, &op);
    ASSERT_EQ(actx.resume_called, 2);           /* fired again on reuse */
    RFC_TERMINAL_TEARDOWN();
}

/* ── Ending an op inside the handler, or by cancel ──────────────────────────────────────────────
 * Two call patterns the API allows. A handler may suspend and then end the op before it returns
 * (the work could not be started, say): kl_async_complete drove the connection, and then the
 * dispatch drove it again, which sent a second response (keep-alive) or released the slot twice
 * (close). And kl_async_cancel, documented for deadline-as-failure, only retired the op: the
 * connection stayed suspended, out of the loop, exempt from the sweep, until the server was freed. */
static KlHttpServer end_srv;
static KlPlatThread end_tid;
static int g_end_cancels;

static void end_server_thread(void *arg) {
    (void)arg;
    kl_http_server_run(&end_srv);
}
static void end_noop_resume(KlAsyncOp *op, void *ud) { (void)op; (void)ud; }
static void end_count_cancel(KlAsyncOp *op, void *ud) { (void)op; (void)ud; g_end_cancels++; }

static KlAsyncOp g_end_op;

/* Suspend, then complete before returning (the response is already built). */
static void handle_suspend_then_complete(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    kl_http_response_json(res, 200, "{\"sc\":1}", 8);
    memset(&g_end_op, 0, sizeof g_end_op);
    g_end_op.on_resume = end_noop_resume;
    if (kl_async_suspend(&end_srv, kl_http_request_conn(req), &g_end_op) < 0) return;
    kl_async_complete(&end_srv, &g_end_op);
}

/* Suspend, then cancel before returning. */
static void handle_suspend_then_cancel(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)res; (void)ud;
    memset(&g_end_op, 0, sizeof g_end_op);
    g_end_op.on_resume = end_noop_resume;
    g_end_op.on_cancel = end_count_cancel;
    if (kl_async_suspend(&end_srv, kl_http_request_conn(req), &g_end_op) < 0) return;
    kl_async_cancel(&end_srv, &g_end_op);
}

/* Suspend; a timer cancels it later (a deadline handled as failure). */
static void end_cancel_timer(void *ud) {
    (void)ud;
    kl_async_cancel(&end_srv, &g_end_op);
}
static void handle_suspend_cancel_later(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)res; (void)ud;
    memset(&g_end_op, 0, sizeof g_end_op);
    g_end_op.on_resume = end_noop_resume;
    g_end_op.on_cancel = end_count_cancel;
    if (kl_async_suspend(&end_srv, kl_http_request_conn(req), &g_end_op) < 0) return;
    (void)kl_timer_add(kl_http_server_event_ctx(&end_srv), 50, end_cancel_timer, NULL);
}

static void handle_end_hello(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_json(res, 200, "{\"hi\":1}", 8);
}

static void handle_chain(KlHttpRequest *req, KlHttpResponse *res, void *ud);
static void handle_nest(KlHttpRequest *req, KlHttpResponse *res, void *ud);
static void handle_od(KlHttpRequest *req, KlHttpResponse *res, void *ud);
static KlHttpBodyReader *od_factory(KlAllocator *alloc, const KlHttpRequest *req, void *ud);
static KlHttpRequest *g_end_req;

static void end_start(void) {
    KlHttpServerConfig cfg = { .port = 0, .max_connections = 4 };
    kl_http_server_init(&end_srv, &cfg);
    kl_http_server_route(&end_srv, "GET", "/sc", handle_suspend_then_complete, NULL, NULL);
    kl_http_server_route(&end_srv, "GET", "/cancel-now", handle_suspend_then_cancel, NULL, NULL);
    kl_http_server_route(&end_srv, "GET", "/cancel-later", handle_suspend_cancel_later, NULL, NULL);
    kl_http_server_route(&end_srv, "GET", "/hello", handle_end_hello, NULL, NULL);
    kl_http_server_route(&end_srv, "GET", "/chain", handle_chain, NULL, NULL);
    kl_http_server_route(&end_srv, "GET", "/nest", handle_nest, NULL, NULL);
    kl_http_server_route_streaming_async(&end_srv, "POST", "/od", handle_od, NULL, od_factory);
    g_end_cancels = 0;
    kl_plat_thread_create(&end_tid, end_server_thread, NULL);
    for (int i = 0; i < 200 && end_srv.bound_port == 0; i++) kl_test_sleep_ms(10);
}
static void end_stop(void) {
    kl_http_server_stop(&end_srv);
    kl_plat_thread_join(&end_tid);
    kl_http_server_free(&end_srv);
}

/* Read for up to `ms`, or until the peer closes. Returns the bytes; *closed on EOF. */
static size_t read_for(int fd, char *buf, size_t cap, int ms, int *closed) {
    size_t have = 0;
    *closed = 0;
    uint64_t start = kl_monotonic_ms();
    while (kl_monotonic_ms() - start < (uint64_t)ms && have + 1 < cap) {
        if (kl_test_poll1(fd, 0, 20) <= 0) continue;
        kl_ssize_t n = kl_test_sockread(fd, buf + have, cap - 1 - have);
        if (n <= 0) { *closed = 1; break; }
        have += (size_t)n;
    }
    buf[have] = '\0';
    return have;
}
static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}
static int end_hellos(int port) {
    int ok = 0;
    for (int i = 0; i < 4; i++) {
        int fd = connect_to(port);
        if (fd < 0) continue;
        const char *rq = "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        char b[512];
        int closed = 0;
        (void)read_for(fd, b, sizeof b, 1000, &closed);
        ok += strstr(b, "{\"hi\":1}") != NULL;
        kl_test_closesock(fd);
    }
    return ok;
}

UTEST(async, complete_inside_the_handler_sends_one_response) {
    end_start();
    int port = end_srv.bound_port;
    char buf[2048];
    int closed = 0, responses = -1;
    int fd = connect_to(port);
    if (fd >= 0) {                                         /* keep-alive: no phantom second response */
        const char *rq = "GET /sc HTTP/1.1\r\nHost: x\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        (void)read_for(fd, buf, sizeof buf, 500, &closed);
        responses = count_of(buf, "HTTP/1.1 ");
        kl_test_closesock(fd);
    }
    fd = connect_to(port);                                 /* close: the slot is released once */
    if (fd >= 0) {
        const char *rq = "GET /sc HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        (void)read_for(fd, buf, sizeof buf, 1000, &closed);
        kl_test_closesock(fd);
    }
    kl_test_sleep_ms(200);
    KlHttpServerStats st;
    kl_http_server_stats(&end_srv, &st);
    int served = end_hellos(port);
    end_stop();
    ASSERT_EQ(responses, 1);                               /* was 2 (keep-alive): a phantom response */
    ASSERT_EQ(st.active_connections, 0);
    ASSERT_EQ(served, 4);                                  /* was: two accepts shared one slot */
}

UTEST(async, cancel_inside_the_handler_closes_the_connection) {
    end_start();
    int port = end_srv.bound_port;
    char buf[512];
    int closed = 0;
    int fd = connect_to(port);
    if (fd >= 0) {
        const char *rq = "GET /cancel-now HTTP/1.1\r\nHost: x\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        (void)read_for(fd, buf, sizeof buf, 1500, &closed);
        kl_test_closesock(fd);
    }
    kl_test_sleep_ms(200);
    KlHttpServerStats st;
    kl_http_server_stats(&end_srv, &st);
    int cancels = g_end_cancels;
    end_stop();
    ASSERT_EQ(cancels, 1);
    ASSERT_TRUE(closed);                                   /* was: left suspended, never closed */
    ASSERT_EQ(st.active_connections, 0);
}

UTEST(async, a_cancelled_op_releases_its_connection) {
    end_start();
    int port = end_srv.bound_port;
    char buf[512];
    int closed = 0;
    int fd = connect_to(port);
    if (fd >= 0) {
        const char *rq = "GET /cancel-later HTTP/1.1\r\nHost: x\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        (void)read_for(fd, buf, sizeof buf, 1500, &closed);
        kl_test_closesock(fd);
    }
    kl_test_sleep_ms(200);
    KlHttpServerStats st;
    kl_http_server_stats(&end_srv, &st);
    int cancels = g_end_cancels;
    end_stop();
    ASSERT_EQ(cancels, 1);
    ASSERT_TRUE(closed);                                   /* was: the slot and fd leaked */
    ASSERT_EQ(st.active_connections, 0);
}

/* ── A cancel inside on_resume releases the connection once ─────────────────────────────────────
 * A resume may suspend again on a second op; when that op's work cannot be started it is cancelled
 * at once. The cancel closed the connection, and then the kl_async_complete that called on_resume
 * closed it again: the slot went onto the free list twice and two later connections shared it. */
static KlAsyncOp g_chain_op2;

static void chain_resume(KlAsyncOp *op, void *ud) {
    (void)ud;
    memset(&g_chain_op2, 0, sizeof g_chain_op2);
    g_chain_op2.on_resume = end_noop_resume;
    g_chain_op2.on_cancel = end_count_cancel;
    if (kl_async_suspend(&end_srv, op->conn, &g_chain_op2) < 0) return;
    kl_async_cancel(&end_srv, &g_chain_op2);              /* the second op could not be started */
}
static void chain_complete_timer(void *ud) {
    (void)ud;
    kl_async_complete(&end_srv, &g_end_op);
}
static void handle_chain(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)res; (void)ud;
    memset(&g_end_op, 0, sizeof g_end_op);
    g_end_op.on_resume = chain_resume;
    if (kl_async_suspend(&end_srv, kl_http_request_conn(req), &g_end_op) < 0) return;
    (void)kl_timer_add(kl_http_server_event_ctx(&end_srv), 50, chain_complete_timer, NULL);
}

UTEST(async, a_cancel_inside_on_resume_releases_once) {
    end_start();
    int port = end_srv.bound_port;
    char buf[512];
    int closed = 0;
    int fd = connect_to(port);
    if (fd >= 0) {
        const char *rq = "GET /chain HTTP/1.1\r\nHost: x\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        (void)read_for(fd, buf, sizeof buf, 1500, &closed);
        kl_test_closesock(fd);
    }
    kl_test_sleep_ms(200);
    KlHttpServerStats st;
    kl_http_server_stats(&end_srv, &st);
    int served = end_hellos(port);
    int cancels = g_end_cancels;
    end_stop();
    ASSERT_EQ(cancels, 1);
    ASSERT_TRUE(closed);
    ASSERT_EQ(st.active_connections, 0);                   /* was: released twice */
    ASSERT_EQ(served, 4);
}

/* ── A nested complete inside on_resume keeps the connection ────────────────────────────────────
 * A resume that suspends on a second op and completes it at once: the inner complete re-registered
 * the fd, and the outer one registered it again. epoll refuses a second add, and that refusal
 * released the live connection: the keep-alive connection was dropped after its response. */
static KlAsyncOp g_nest_op2;

static void nest_resume(KlAsyncOp *op, void *ud) {
    (void)ud;
    memset(&g_nest_op2, 0, sizeof g_nest_op2);
    g_nest_op2.on_resume = end_noop_resume;
    if (kl_async_suspend(&end_srv, op->conn, &g_nest_op2) < 0) return;
    kl_http_response_json(kl_http_conn_response(op->conn), 200, "{\"nest\":1}", 10);
    kl_async_complete(&end_srv, &g_nest_op2);             /* the second op finished at once */
}
static void handle_nest(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)res; (void)ud;
    memset(&g_end_op, 0, sizeof g_end_op);
    g_end_op.on_resume = nest_resume;
    if (kl_async_suspend(&end_srv, kl_http_request_conn(req), &g_end_op) < 0) return;
    (void)kl_timer_add(kl_http_server_event_ctx(&end_srv), 50, chain_complete_timer, NULL);
}

UTEST(async, a_nested_complete_inside_on_resume_keeps_the_connection) {
    end_start();
    int port = end_srv.bound_port;
    char buf[1024];
    int closed = 0, first = 0, second = 0;
    int fd = connect_to(port);
    if (fd >= 0) {
        const char *rq = "GET /nest HTTP/1.1\r\nHost: x\r\n\r\n";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        (void)read_for(fd, buf, sizeof buf, 1000, &closed);
        first = strstr(buf, "{\"nest\":1}") != NULL;
        if (!closed) {                                     /* keep-alive: a second request */
            const char *rq2 = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
            (void)kl_test_sockwrite(fd, rq2, strlen(rq2));
            (void)read_for(fd, buf, sizeof buf, 1000, &closed);
            second = strstr(buf, "{\"hi\":1}") != NULL;
        }
        kl_test_closesock(fd);
    }
    end_stop();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);                                   /* was (epoll): the connection was released */
}

/* ── An op ended inside on_data, for body bytes read with the headers ───────────────────────────
 * A streaming-async route feeds the body bytes that arrived with the headers to the reader's on_data
 * from the dispatch itself. An op started and cancelled there released the connection under on_data
 * (the reader was destroyed while it ran) and the dispatch then released it again. And an op left
 * pending there was overwritten by the dispatch's READING_BODY, so the connection read while
 * suspended (or, on epoll, was released). */
typedef struct {
    KlHttpBodyReader base;
    KlAllocator *alloc;
    int fired;
} OdReader;
static int g_od_cancel_now;
static int od_on_data(KlHttpBodyReader *self, const char *d, size_t n) {
    OdReader *r = (OdReader *)self;
    (void)d; (void)n;
    if (r->fired || !g_end_req) return 0;
    r->fired = 1;
    memset(&g_end_op, 0, sizeof g_end_op);
    g_end_op.on_resume = end_noop_resume;
    g_end_op.on_cancel = end_count_cancel;
    if (kl_async_suspend(&end_srv, kl_http_request_conn(g_end_req), &g_end_op) < 0) return 0;
    if (g_od_cancel_now) {
        kl_async_cancel(&end_srv, &g_end_op);              /* the work could not be started */
    } else {
        kl_http_response_json(kl_http_conn_response(kl_http_request_conn(g_end_req)), 200,
                              "{\"od\":1}", 8);
        (void)kl_timer_add(kl_http_server_event_ctx(&end_srv), 50, chain_complete_timer, NULL);
    }
    return 0;
}
static void od_on_complete(KlHttpBodyReader *self) { (void)self; }
static void od_on_error(KlHttpBodyReader *self) { (void)self; }
static void od_destroy(KlHttpBodyReader *self) {
    OdReader *r = (OdReader *)self;
    kl_free(r->alloc, r, sizeof *r);
}
static KlHttpBodyReader *od_factory(KlAllocator *alloc, const KlHttpRequest *req, void *ud) {
    (void)req; (void)ud;
    OdReader *r = kl_malloc(alloc, sizeof *r);
    if (!r) return NULL;
    memset(r, 0, sizeof *r);
    r->base.on_data = od_on_data;
    r->base.on_complete = od_on_complete;
    r->base.on_error = od_on_error;
    r->base.destroy = od_destroy;
    r->alloc = alloc;
    return &r->base;
}
static void handle_od(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)res; (void)ud;
    g_end_req = req;
    kl_http_request_await_body(req);                       /* on_data resumes "us" */
}

static void od_case(int *utest_result, int cancel_now) {
    g_od_cancel_now = cancel_now;
    end_start();
    int port = end_srv.bound_port;
    char buf[1024];
    int closed = 0;
    int fd = connect_to(port);
    if (fd >= 0) {                                         /* half the body arrives with the headers */
        const char *rq = "POST /od HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n"
                         "Connection: close\r\n\r\n01234";
        (void)kl_test_sockwrite(fd, rq, strlen(rq));
        (void)read_for(fd, buf, sizeof buf, 1500, &closed);
        kl_test_closesock(fd);
    }
    kl_test_sleep_ms(200);
    KlHttpServerStats st;
    kl_http_server_stats(&end_srv, &st);
    int served = end_hellos(port);
    int cancels = g_end_cancels;
    end_stop();
    g_end_req = NULL;
    if (cancel_now) {
        ASSERT_EQ(cancels, 1);
        ASSERT_TRUE(closed);
    } else {
        ASSERT_TRUE(strstr(buf, "{\"od\":1}") != NULL);    /* was: overwritten by READING_BODY */
    }
    ASSERT_EQ(st.active_connections, 0);                   /* was (cancel): released twice */
    ASSERT_EQ(served, 4);
}

UTEST(async, a_cancel_inside_on_data_for_leftover_body_releases_once) { od_case(utest_result, 1); }
UTEST(async, an_op_left_pending_inside_on_data_is_not_overwritten) { od_case(utest_result, 0); }

UTEST_MAIN();
