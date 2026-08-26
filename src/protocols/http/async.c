/*
 * async.c: KlAsyncOp connection suspension (server-side).
 *
 * The KlEventCtx + KlWatcher API + kl_event_ctx_run live in event_ctx.c; that
 * half is client-usable and server-free. This TU
 * keeps only the connection-suspension machinery, which reaches into the server
 * connection driver (kl_http_conn_on_writable / kl_http_server_conn_release /
 * kl_http_comp_resume) and so is not part of the freestanding client.
 */
#include <keel/clock.h>
#include <keel/async.h>
#include <keel/http_server.h>
#include <keel/http_connection.h>
#include <stdint.h>
#include "http_internal.h"
#include "event_caps.h"
#include "completion_io.h"   /* kl_comp_run (neutral generic tick, via kl_event_ctx_run) */
#include "completion_http.h" /* kl_http_comp_resume: completion resume */

/* ── KlAsyncOp ─────────────────────────────────────────────────────── */

int kl_async_suspend(KlHttpServer *s, KlHttpConn *conn, KlAsyncOp *op) {
    if (!s || !conn || !op) return -1;
    if (conn->state == KL_HTTP_CONN_SUSPENDED) return -1;

    /* Remove client FD from event loop */
    kl_event_del(&s->ev.loop, conn->stream.fd);

    /* Park the connection */
    conn->state = KL_HTTP_CONN_SUSPENDED;
    conn->async_op = op;
    conn->suspend_start_ms = kl_monotonic_ms();
    op->conn = conn;
    op->_terminal = 0;         /* fresh suspension → pending (allows op reuse) */

    /* Add to server's active ops list */
    op->next = s->async_ops;
    s->async_ops = op;

    return 0;
}

/* Idempotently retire an op: remove it from the active list, clear the
 * connection's back-pointer, and mark it terminal. Returns the connection it was
 * attached to on the first (pending→terminal) call, or NULL if the op was already
 * retired, the exactly-one-terminal guard shared by complete and cancel. */
static KlHttpConn *async_retire(KlHttpServer *s, KlAsyncOp *op) {
    if (op->_terminal) return NULL;
    op->_terminal = 1;

    KlAsyncOp **pp = &s->async_ops;
    while (*pp) {
        if (*pp == op) { *pp = op->next; break; }
        pp = &(*pp)->next;
    }
    KlHttpConn *conn = op->conn;
    if (conn && conn->async_op == op)
        conn->async_op = NULL;
    return conn;
}

void kl_async_complete(KlHttpServer *s, KlAsyncOp *op) {
    if (!s || !op) return;

    /* Exactly-one-terminal: a second complete (or a complete racing a cancel)
     * is a no-op: no double on_resume, no double release. */
    KlHttpConn *conn = async_retire(s, op);
    if (!conn) return;

    /* Transition back from SUSPENDED so on_resume can set the final state */
    conn->state = KL_HTTP_CONN_PROCESSING;

    /* Call the resume callback */
    if (op->on_resume)
        op->on_resume(op, op->user_data);

    /* If the handler suspended again, a new op is active, nothing to do */
    if (conn->state == KL_HTTP_CONN_SUSPENDED)
        return;

    /* On a completion loop, drive the completion send path instead of re-arming the fd;
     * the readiness re-register + kl_http_conn_on_writable below is a no-op there (kl_event_add
     * is inert, kl_http_conn_on_writable does readiness socket writes). Branch on the abstract
     * event axis (not the backend); the completion send lives behind the completion seam so
     * async.c stays free of completion internals. */
    if (kl_event_caps(&s->ev.loop) & KL_EVENT_CAP_COMPLETION) {
        kl_http_comp_resume(s, conn);
        return;
    }

    /* Handler completed: re-register FD and drive state machine */
    KlHttpConnState new_state = conn->state;

    /* Try immediate send if response is ready */
    if (new_state == KL_HTTP_CONN_SENDING)
        new_state = kl_http_conn_on_writable(conn);

    /* Re-register FD with appropriate mask */
    if (new_state == KL_HTTP_CONN_SENDING) {
        if (kl_event_add(&s->ev.loop, conn->stream.fd, KL_EVENT_WRITE, &conn->stream) < 0)
            kl_http_server_conn_release(s, conn);
    } else if (new_state == KL_HTTP_CONN_READING) {
        if (kl_event_add(&s->ev.loop, conn->stream.fd, KL_EVENT_READ, &conn->stream) < 0)
            kl_http_server_conn_release(s, conn);
    } else if (new_state == KL_HTTP_CONN_CLOSED) {
        kl_http_server_conn_release(s, conn);
    }
}

void kl_async_cancel(KlHttpServer *s, KlAsyncOp *op) {
    if (!s || !op) return;

    /* Exactly-one-terminal: a cancel racing a completion (or a double cancel) is
     * a no-op: on_cancel fires at most once, and never after on_resume. */
    if (async_retire(s, op) == NULL) return;

    if (op->on_cancel)
        op->on_cancel(op, op->user_data);
}
