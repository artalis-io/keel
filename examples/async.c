/*
 * async.c — Async suspend/resume with FD watchers
 *
 * Concepts: KlWatcher, KlAsyncOp, kl_async_suspend, kl_async_complete,
 * pipe-based completion signaling, req->_server_ctx (KlConn*).
 *
 * GET /delay/:ms suspends the connection, spawns a thread that sleeps
 * for the requested duration, then signals completion via a pipe.
 * The watcher callback fires on the event loop thread and resumes
 * the connection.
 *
 * IMPORTANT: kl_async_complete() must be called from the event loop
 * thread. Never call it directly from a worker thread — use a pipe
 * or the thread pool's done_fn to marshal back.
 *
 * Build:  make examples
 * Run:    ./examples/async
 * Test:   curl localhost:8080/delay/200   # 200ms delayed response
 *         curl localhost:8080/            # immediate response
 */

#include <keel/keel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* ── Async context ──────────────────────────────────────────────────── */

typedef struct {
    KlAsyncOp op;
    KlServer *server;
    int pipe_fds[2];       /* [0]=read (watcher), [1]=write (thread) */
    int delay_ms;
} DelayCtx;

/* Called on event loop thread when async op completes */
static void delay_on_resume(KlAsyncOp *op, void *user_data) {
    (void)user_data;
    DelayCtx *ctx = (DelayCtx *)op;

    static char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"delayed_ms\":%d}", ctx->delay_ms);
    if (n < 0) n = 0;

    KlConn *conn = op->conn;
    kl_response_json(&conn->res, 200, body, (size_t)n);

    close(ctx->pipe_fds[0]);
    close(ctx->pipe_fds[1]);
    free(ctx);
}

/* Called if connection dies while suspended */
static void delay_on_cancel(KlAsyncOp *op, void *user_data) {
    (void)user_data;
    DelayCtx *ctx = (DelayCtx *)op;
    close(ctx->pipe_fds[0]);
    close(ctx->pipe_fds[1]);
    free(ctx);
}

/* Watcher callback: pipe became readable → complete the async op */
static void delay_watcher(int fd, KlEventMask ready, void *user_data) {
    (void)fd; (void)ready;
    DelayCtx *ctx = user_data;

    /* Drain the pipe */
    char buf[1];
    if (read(ctx->pipe_fds[0], buf, 1) < 0) { /* ignore */ }

    /* Remove watcher before completing */
    kl_watcher_del(&ctx->server->ev, ctx->pipe_fds[0]);

    /* Resume connection (must be on event loop thread — we are) */
    kl_async_complete(ctx->server, &ctx->op);
}

/* Worker thread: sleep then signal via pipe */
static void *delay_thread(void *arg) {
    DelayCtx *ctx = arg;
    usleep((unsigned)ctx->delay_ms * 1000);

    /* Signal the event loop */
    char c = 1;
    if (write(ctx->pipe_fds[1], &c, 1) < 0) { /* ignore */ }
    return NULL;
}

/* ── Handlers ───────────────────────────────────────────────────────── */

static void handle_delay(KlRequest *req, KlResponse *res, void *user_data) {
    (void)res;
    KlServer *srv = user_data;
    KlConn *conn = req->_server_ctx;

    /* Parse delay from route param */
    size_t ms_len;
    const char *ms_str = kl_request_param(req, "ms", &ms_len);
    int delay_ms = 100;
    if (ms_str) {
        char tmp[16];
        size_t copy = ms_len < sizeof(tmp) - 1 ? ms_len : sizeof(tmp) - 1;
        memcpy(tmp, ms_str, copy);
        tmp[copy] = '\0';
        delay_ms = atoi(tmp);
        if (delay_ms < 0) delay_ms = 0;
        if (delay_ms > 5000) delay_ms = 5000;
    }

    /* Allocate context (freed in on_resume or on_cancel) */
    DelayCtx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        kl_response_error(res, 500, "Out of memory");
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->server   = srv;
    ctx->delay_ms = delay_ms;
    ctx->op.on_resume = delay_on_resume;
    ctx->op.on_cancel = delay_on_cancel;

    /* Create pipe for signaling */
    if (pipe(ctx->pipe_fds) < 0) {
        free(ctx);
        kl_response_error(res, 500, "pipe() failed");
        return;
    }

    /* Register read end with event loop */
    if (kl_watcher_add(&srv->ev, ctx->pipe_fds[0], KL_EVENT_READ,
                        delay_watcher, ctx) < 0) {
        close(ctx->pipe_fds[0]);
        close(ctx->pipe_fds[1]);
        free(ctx);
        kl_response_error(res, 500, "watcher_add failed");
        return;
    }

    /* Suspend connection */
    kl_async_suspend(srv, conn, &ctx->op);

    /* Spawn detached thread to do the "work" */
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attr, delay_thread, ctx);
    pthread_attr_destroy(&attr);
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

    kl_server_route(&s, "GET", "/",          handle_hello, NULL, NULL);
    kl_server_route(&s, "GET", "/delay/:ms", handle_delay, &s,   NULL);

    printf("async example listening on :8080\n");
    printf("  curl localhost:8080/delay/200\n");
    printf("  curl localhost:8080/\n");
    kl_server_run(&s);
    kl_server_free(&s);
    return 0;
}
