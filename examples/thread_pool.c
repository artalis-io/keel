/*
 * thread_pool.c — Blocking work offloaded to worker threads
 *
 * Concepts: KlThreadPool, KlWorkItem, kl_thread_pool_create/submit/free,
 * KlAsyncOp + thread pool integration (work_fn / done_fn / cancel_fn).
 *
 * GET /query suspends the connection, submits a work item to the thread
 * pool. The work_fn simulates a 50ms blocking query on a worker thread.
 * When done, done_fn fires on the event loop thread and resumes the
 * connection via kl_async_complete().
 *
 * Three-callback model:
 *   work_fn   — runs on worker thread (do blocking I/O here)
 *   done_fn   — runs on event loop thread (safe to call kl_async_complete)
 *   cancel_fn — runs on shutdown for items that never started
 *
 * Build:  make examples
 * Run:    ./examples/thread_pool
 * Test:   curl localhost:8080/query
 *         curl localhost:8080/
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
    int result;            /* set by work_fn, read by done_fn */
} QueryCtx;

/* Worker thread: simulate a blocking database query */
static void query_work_fn(void *user_data) {
    QueryCtx *ctx = user_data;
    usleep(50 * 1000); /* 50ms "query" */
    ctx->result = 42;
}

/* Event loop thread: resume the suspended connection */
static void query_done_fn(void *user_data) {
    QueryCtx *ctx = user_data;

    static char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"answer\":%d}", ctx->result);
    if (n < 0) n = 0;

    KlConn *conn = ctx->op.conn;
    kl_response_json(&conn->res, 200, body, (size_t)n);

    kl_async_complete(ctx->server, &ctx->op);
    free(ctx);
}

/* Called on resume (no-op since done_fn already set the response) */
static void query_on_resume(KlAsyncOp *op, void *user_data) {
    (void)op; (void)user_data;
}

/* Called if connection dies while work is in flight */
static void query_on_cancel(KlAsyncOp *op, void *user_data) {
    (void)user_data;
    free(op);
}

/* Shutdown cleanup for items that never started */
static void query_cancel_fn(void *user_data) {
    free(user_data);
}

/* ── Handlers ───────────────────────────────────────────────────────── */

typedef struct {
    KlServer *server;
    KlThreadPool *pool;
} AppCtx;

static void handle_query(KlRequest *req, KlResponse *res, void *user_data) {
    (void)res;
    AppCtx *app = user_data;
    KlConn *conn = req->_server_ctx;

    QueryCtx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        kl_response_error(res, 500, "Out of memory");
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->server = app->server;
    ctx->op.on_resume = query_on_resume;
    ctx->op.on_cancel = query_on_cancel;

    /* Suspend connection */
    kl_async_suspend(app->server, conn, &ctx->op);

    /* Submit work to thread pool */
    KlWorkItem item = {
        .work_fn   = query_work_fn,
        .done_fn   = query_done_fn,
        .cancel_fn = query_cancel_fn,
        .user_data = ctx,
    };
    if (kl_thread_pool_submit(app->pool, &item) < 0) {
        /* Queue full — resume and send error */
        kl_response_error(res, 503, "Server busy");
        kl_async_complete(app->server, &ctx->op);
        free(ctx);
    }
}

static void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"msg\":\"hello (sync)\"}", 21);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    KlServer s;
    KlConfig cfg = {
        .port = 8080,
        .install_signal_handlers = 1,
    };
    if (kl_server_init(&s, &cfg) < 0) return 1;

    /* Create thread pool (4 workers, default queue) */
    KlThreadPoolConfig pool_cfg = {.num_workers = 4};
    KlThreadPool *pool = kl_thread_pool_create(&s, &pool_cfg);
    if (!pool) {
        fprintf(stderr, "thread pool creation failed\n");
        kl_server_free(&s);
        return 1;
    }

    AppCtx app = {.server = &s, .pool = pool};

    kl_server_route(&s, "GET", "/",      handle_hello, NULL, NULL);
    kl_server_route(&s, "GET", "/query", handle_query, &app, NULL);

    printf("thread_pool example listening on :8080\n");
    printf("  curl localhost:8080/query\n");
    printf("  curl localhost:8080/\n");
    kl_server_run(&s);

    kl_thread_pool_free(pool);
    kl_server_free(&s);
    return 0;
}
