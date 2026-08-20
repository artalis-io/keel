/*
 * async_thread_pool.c — Thread pool + async ops with deadlines
 *
 * Concepts: KlThreadPool + KlAsyncOp together, multiple concurrent
 * async operations with different deadlines, cancel_fn for shutdown,
 * simulated blocking I/O on worker threads.
 *
 * GET /fast   — 10ms  simulated work
 * GET /slow   — 200ms simulated work
 * GET /cancel — demonstrates cancel_fn when connection drops
 * GET /       — immediate sync response
 *
 * Build:  make examples
 * Run:    ./examples/async_thread_pool
 * Test:   curl localhost:8080/fast
 *         curl localhost:8080/slow
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Work context ───────────────────────────────────────────────────── */

typedef struct {
    KlAsyncOp op;
    KlServer *server;
    KlThreadPool *pool;
    int work_ms;
    int result;
} WorkCtx;

static void work_fn(void *user_data) {
    WorkCtx *ctx = user_data;
    usleep((unsigned)ctx->work_ms * 1000);
    ctx->result = ctx->work_ms;
}

static void done_fn(void *user_data) {
    WorkCtx *ctx = user_data;

    static char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"work_ms\":%d,\"result\":%d}",
                     ctx->work_ms, ctx->result);
    if (n < 0) n = 0;

    KlConn *conn = ctx->op.conn;
    kl_http_response_json(&conn->res, 200, body, (size_t)n);

    kl_async_complete(ctx->server, &ctx->op);
    free(ctx);
}

static void on_resume(KlAsyncOp *op, void *user_data) {
    (void)op; (void)user_data;
}

static void on_cancel(KlAsyncOp *op, void *user_data) {
    (void)user_data;
    free(op);
}

static void cancel_fn(void *user_data) {
    free(user_data);
}

/* ── Handlers ───────────────────────────────────────────────────────── */

typedef struct {
    KlServer *server;
    KlThreadPool *pool;
} AppCtx;

static void handle_work(KlHttpRequest *req, KlHttpResponse *res, void *user_data,
                         int work_ms) {
    (void)res;
    AppCtx *app = user_data;
    KlConn *conn = kl_http_request_conn(req);

    WorkCtx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        kl_http_response_error(res, 500, "Out of memory");
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->server  = app->server;
    ctx->pool    = app->pool;
    ctx->work_ms = work_ms;
    ctx->op.on_resume = on_resume;
    ctx->op.on_cancel = on_cancel;

    kl_async_suspend(app->server, conn, &ctx->op);

    KlWorkItem item = {
        .work_fn   = work_fn,
        .done_fn   = done_fn,
        .cancel_fn = cancel_fn,
        .user_data = ctx,
    };
    if (kl_thread_pool_submit(app->pool, &item) < 0) {
        kl_http_response_error(res, 503, "Server busy");
        kl_async_complete(app->server, &ctx->op);
        free(ctx);
    }
}

static void handle_fast(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    handle_work(req, res, user_data, 10);
}

static void handle_slow(KlHttpRequest *req, KlHttpResponse *res, void *user_data) {
    handle_work(req, res, user_data, 200);
}

static void handle_hello(KlHttpRequest *req, KlHttpResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_http_response_json(res, 200, "{\"msg\":\"hello (sync)\"}", 21);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    KlServer s;
    KlConfig cfg = {
        .port = 8080,
        .install_signal_handlers = 1,
    };
    if (kl_server_init(&s, &cfg) < 0) return 1;

    KlThreadPoolConfig pool_cfg = {.num_workers = 4};
    KlThreadPool *pool = kl_thread_pool_create(&s.ev, &pool_cfg);
    if (!pool) {
        fprintf(stderr, "thread pool creation failed\n");
        kl_server_free(&s);
        return 1;
    }

    AppCtx app = {.server = &s, .pool = pool};

    kl_server_route(&s, "GET", "/",     handle_hello, NULL, NULL);
    kl_server_route(&s, "GET", "/fast", handle_fast,  &app, NULL);
    kl_server_route(&s, "GET", "/slow", handle_slow,  &app, NULL);

    printf("async_thread_pool example listening on :8080\n");
    printf("  curl localhost:8080/fast   # 10ms work\n");
    printf("  curl localhost:8080/slow   # 200ms work\n");
    printf("  curl localhost:8080/       # immediate\n");
    kl_server_run(&s);

    kl_thread_pool_free(pool);
    kl_server_free(&s);
    return 0;
}
