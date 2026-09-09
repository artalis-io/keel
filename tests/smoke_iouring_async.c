/*
 * smoke_iouring_async.c: async/thread-pool handler over the io_uring completion loop
 * (the watcher relay).
 *
 * Runtime-tests the io_uring watcher relay end to end: a KlHttpServer on the io_uring
 * completion loop (BACKEND=iouring), a KlThreadPool whose wakeup pipe is a kl_watcher
 * on the server ctx, and a handler that offloads blocking work and suspends the connection.
 * The worker finishes → writes the wakeup pipe → the single-shot IORING_OP_POLL_ADD armed
 * for that watch completes → kl_comp_drain surfaces a KL_COMP_WATCHER → the driver routes
 * it to the pool's done_fn → kl_async_complete → kl_http_comp_resume sends the
 * response. Proves POLL_ADD satisfies the same abstract watcher contract pollcomp meets
 * with a poll set and IOCP with an overlapped WSARecv. Linux-only; the Completion CI job
 * runs it. Sibling of smoke_pollcomp_async.c.
 */
#include <keel/keel.h>
#include "../src/protocols/http/http_conn_internal.h"  /* white-box: KlHttpConn layout + state enum */
/* No internal socket.h: this smoke sets no provider: it proves the auto-wire. */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#define PORT 18096
#define WANT "{\"async\":true,\"result\":42}"

static KlHttpServer g_srv;
static KlThreadPool *g_pool;

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* op MUST be first: on_cancel/cancel_fn free via this pointer. */
typedef struct { KlAsyncOp op; int result; } WorkCtx;

static void work_fn(void *ud) {          /* worker thread: simulate blocking work */
    WorkCtx *w = ud;
    usleep(20 * 1000);
    w->result = 42;
}
static void on_resume(KlAsyncOp *op, void *ud) {   /* event-loop thread: declare the send */
    (void)ud;
    op->conn->state = KL_HTTP_CONN_SENDING;
}
/* The other resume shape, and the only one a public consumer can write: it just builds
 * the response and leaves the connection state alone. KlHttpConn is opaque on the public
 * surface, so nothing outside the library can declare SENDING itself and kl_async_complete
 * has to default it. Without that default the completion driver takes its CLOSED arm and
 * shuts the socket without ever sending this route's reply. */
static void on_resume_nostate(KlAsyncOp *op, void *ud) { (void)op; (void)ud; }
static void on_cancel(KlAsyncOp *op, void *ud) { (void)ud; free(op); }
static void cancel_fn(void *ud) { free(ud); }
static void done_fn(void *ud) {          /* event-loop thread (via the wakeup watcher) */
    WorkCtx *w = ud;
    KlHttpConn *conn = w->op.conn;
    kl_http_response_json(&conn->res, 200, WANT, sizeof(WANT) - 1);
    kl_async_complete(&g_srv, &w->op);
    free(w);
}

static void handle_async(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    KlHttpConn *conn = kl_http_request_conn(req);
    WorkCtx *w = malloc(sizeof(*w));
    if (!w) { kl_http_response_error(res, 500, "oom"); return; }
    memset(w, 0, sizeof(*w));
    w->op.on_resume = ud ? on_resume : on_resume_nostate;   /* route user_data picks the shape */
    w->op.on_cancel = on_cancel;
    kl_async_suspend(&g_srv, conn, &w->op);
    KlWorkItem item = { .work_fn = work_fn, .done_fn = done_fn,
                        .cancel_fn = cancel_fn, .user_data = w };
    if (kl_thread_pool_submit(g_pool, &item) < 0) {
        kl_http_response_error(res, 503, "busy");
        kl_async_complete(&g_srv, &w->op);
        free(w);
    }
}

static void *server_thread(void *arg) { (void)arg; kl_http_server_run(&g_srv); return NULL; }

/* One request: 1 if it produced the expected 200 + body. */
static int fetch_ok(KlAllocator *alloc, const KlHttpClientConfig *ccfg, const char *url) {
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof(resp));
    if (kl_http_client_request(alloc, ccfg, "GET", url, NULL, 0, NULL, 0, &resp) != 0)
        return 0;
    int good = (resp.status == 200 && resp.body_len == sizeof(WANT) - 1 &&
                resp.body && memcmp(resp.body, WANT, sizeof(WANT) - 1) == 0);
    kl_http_client_response_free(&resp);
    return good;
}

int main(void) {
    /* No .sockets set on purpose: a completion loop rejects the default provider,
     * so kl_http_server_init must adopt the backend's native overlapped provider automatically
     * (kl_event_native_provider). This proves the completion backend is a source-compatible
     * drop-in: the whole async/thread-pool/watcher-relay surface then runs over the
     * auto-wired provider, no explicit provider anywhere. */
    KlHttpServerConfig cfg = { .port = PORT, .bind_addr = "127.0.0.1" };
    if (kl_http_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-iouring-async: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    KlThreadPoolConfig tpcfg = { .num_workers = 2, .queue_capacity = 16 };
    g_pool = kl_thread_pool_create(&g_srv.ev, &tpcfg);
    if (!g_pool) { fprintf(stderr, "smoke-iouring-async: thread pool create failed\n"); kl_http_server_free(&g_srv); return 1; }

    /* Both resume shapes must produce the same reply: one declares SENDING itself,
     * one only builds the response (all the public API can express). */
    kl_http_server_route(&g_srv, "GET", "/async",   handle_async, &g_srv, NULL);
    kl_http_server_route(&g_srv, "GET", "/nostate", handle_async, NULL,   NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        kl_thread_pool_free(g_pool);
        kl_http_server_free(&g_srv);
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlHttpClientConfig ccfg = { .timeout_ms = 2000 };
    int ok = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        nap_ms(50);
        ok = fetch_ok(&alloc, &ccfg, "http://127.0.0.1:18096/async");
    }
    int ok_nostate = ok && fetch_ok(&alloc, &ccfg, "http://127.0.0.1:18096/nostate");

    kl_http_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_thread_pool_free(g_pool);
    kl_http_server_free(&g_srv);

    if (!ok || !ok_nostate) {
        fprintf(stderr, "smoke-iouring-async: async/thread-pool roundtrip FAILED (explicit-resume=%d, response-only-resume=%d)\n",
                ok, ok_nostate);
        return 1;
    }
    printf("smoke-iouring-async: async/thread-pool over-io_uring-completion roundtrip OK\n");
    return 0;
}
