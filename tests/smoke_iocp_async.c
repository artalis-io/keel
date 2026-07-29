/*
 * smoke_iocp_async.c — async/thread-pool handler over IOCP on Windows (8e-2c).
 *
 * Runtime-tests the IOCP watcher relay: a KlServer on the IOCP completion loop, a
 * KlThreadPool whose loopback-socket wakeup is registered as a watcher, and a handler
 * that offloads blocking work and suspends. The worker finishes → writes the wakeup
 * socket → the overlapped WSARecv posted in event_iocp.c completes → the drain surfaces
 * KL_COMP_WATCHER → the driver routes it to the pool's done_fn → kl_async_complete →
 * the response is sent. Windows-only; the Windows-IOCP CI job runs it. POSIX sibling:
 * smoke_pollcomp_async.c.
 */
#include <winsock2.h>
#include <keel/keel.h>
#include "../src/socket.h"     /* internal kl_socket_provider_iocp() */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define PORT 18100
#define WANT "{\"async\":true,\"result\":42}"

static KlServer g_srv;
static KlThreadPool *g_pool;

typedef struct { KlAsyncOp op; int result; } WorkCtx;   /* op MUST be first */

static void work_fn(void *ud) {          /* worker thread: simulate blocking work */
    WorkCtx *w = ud;
    Sleep(20);
    w->result = 42;
}
static void on_resume(KlAsyncOp *op, void *ud) { (void)ud; op->conn->state = KL_CONN_SENDING; }
static void on_cancel(KlAsyncOp *op, void *ud) { (void)ud; free(op); }
static void cancel_fn(void *ud) { free(ud); }
static void done_fn(void *ud) {          /* event-loop thread (via the wakeup watcher) */
    WorkCtx *w = ud;
    KlConn *conn = w->op.conn;
    kl_response_json(&conn->res, 200, WANT, sizeof(WANT) - 1);
    kl_async_complete(&g_srv, &w->op);
    free(w);
}

static void handle_async(KlRequest *req, KlResponse *res, void *ud) {
    (void)ud;
    KlConn *conn = kl_request_conn(req);
    WorkCtx *w = malloc(sizeof(*w));
    if (!w) { kl_response_error(res, 500, "oom"); return; }
    memset(w, 0, sizeof(*w));
    w->op.on_resume = on_resume;
    w->op.on_cancel = on_cancel;
    kl_async_suspend(&g_srv, conn, &w->op);
    KlWorkItem item = { .work_fn = work_fn, .done_fn = done_fn,
                        .cancel_fn = cancel_fn, .user_data = w };
    if (kl_thread_pool_submit(g_pool, &item) < 0) {
        kl_response_error(res, 503, "busy");
        kl_async_complete(&g_srv, &w->op);
        free(w);
    }
}

static void *server_thread(void *arg) { (void)arg; kl_server_run(&g_srv); return NULL; }

int main(void) {
    KlConfig cfg = { .port = PORT, .bind_addr = "127.0.0.1",
                     .sockets = kl_socket_provider_iocp() };
    if (kl_server_init(&g_srv, &cfg) < 0) {
        fprintf(stderr, "smoke-iocp-async: server init failed (err=%d)\n", g_srv.last_error);
        return 1;
    }
    KlThreadPoolConfig tpcfg = { .num_workers = 2, .queue_capacity = 16 };
    g_pool = kl_thread_pool_create(&g_srv.ev, &tpcfg);
    if (!g_pool) { fprintf(stderr, "smoke-iocp-async: thread pool create failed\n"); kl_server_free(&g_srv); return 1; }

    kl_server_route(&g_srv, "GET", "/async", handle_async, NULL, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, server_thread, NULL) != 0) {
        kl_thread_pool_free(g_pool);
        kl_server_free(&g_srv);
        return 1;
    }

    KlAllocator alloc = kl_allocator_default();
    KlClientConfig ccfg = { .timeout_ms = 2000 };
    int ok = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        Sleep(50);
        KlClientResponse resp;
        memset(&resp, 0, sizeof(resp));
        int rc = kl_client_request(&alloc, &ccfg, "GET", "http://127.0.0.1:18100/async",
                                   NULL, 0, NULL, 0, &resp);
        if (rc == 0) {
            ok = (resp.status == 200 && resp.body_len == sizeof(WANT) - 1 &&
                  resp.body && memcmp(resp.body, WANT, sizeof(WANT) - 1) == 0);
            kl_client_response_free(&resp);
        }
    }

    kl_server_stop(&g_srv);
    pthread_join(th, NULL);
    kl_thread_pool_free(g_pool);
    kl_server_free(&g_srv);

    if (!ok) {
        fprintf(stderr, "smoke-iocp-async: async/thread-pool roundtrip FAILED\n");
        return 1;
    }
    printf("smoke-iocp-async: async/thread-pool over-IOCP roundtrip OK\n");
    return 0;
}
