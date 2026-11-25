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
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <stdint.h>
#include "http_internal.h"
#include "event_caps.h"
#include "socket.h"           /* kl_sock_recv_peek / kl_sock_io_status: the sweep's probe */
#include "completion_io.h"   /* kl_comp_run (neutral generic tick, via kl_event_ctx_run) */
#include "completion_http.h" /* kl_http_comp_resume: completion resume */
#include "http_response_internal.h" /* kl_http_response_stream_progress: the writable edge */

/* ── Client hang-up while suspended ────────────────────────────────────
 * HTTP decides what abandonment means; the event layer only reports it. While a connection is
 * suspended its fd is out of the loop for data, but on a loop with KL_EVENT_CAP_HANGUP a
 * KL_EVENT_HANGUP watcher observes the peer's FIN or a reset without reading (a pipelined request
 * or unread body stays where it is). The watch is removed on its first report, whatever the
 * policy: a level-triggered or one-shot backend would report it again. Every terminal transition
 * of the op (resume, cancel, connection death, server free) goes through async_retire, which
 * removes the watch before anything re-registers or closes the fd. */

static void async_on_hangup(KlSocketHandle fd, KlEventMask ready, void *user_data);
static void async_hup_sweep(void *user_data);

/* ── The sweep: loops without KL_EVENT_CAP_HANGUP ──
 * One timer per server, pending only while at least one suspended connection needs it; never per
 * loop iteration. Each tick peeks every swept connection once (recv_peek: never blocks, never
 * consumes) and cancels those whose client is gone. BEST EFFORT: a FIN or reset behind unread bytes
 * is invisible to a peek, so such a connection is bounded only by its op's deadline (and Keel does
 * not require one). */
#define KL_HUP_SWEEP_MS 1000

/* Can this server's sockets be peeked without risk? A provider's own recv_peek, or the built-in
 * default on a real descriptor. A provider without recv_peek whose handles are not OS descriptors
 * (lwIP raw: a tcp_pcb pointer) must never reach the default recv(). */
static int async_hup_can_peek(const KlHttpServer *s) {
    const KlSocketProvider *p = s->ev.sockets;
    return !p || (p->ops && p->ops->recv_peek) || kl_socket_provider_has_cap(p, KL_SOCK_CAP_NATIVE_FD);
}

static void async_hup_sweep_arm(KlHttpServer *s) {
    if (s->hup_sweep_armed || s->hup_sweep_conns <= 0) return;
    int64_t id = kl_timer_add(&s->ev, KL_HUP_SWEEP_MS, async_hup_sweep, s);
    if (id < 0) return;            /* no timer: the op's deadline remains the bound */
    s->hup_sweep_timer = id;
    s->hup_sweep_armed = 1;
}

static void async_watch_hangup(KlHttpServer *s, KlHttpConn *conn) {
    conn->hup_state = KL_HUP_NONE;
    conn->hup_server = s;
    if (s->config.suspended_hangup == KL_HTTP_SUSPEND_HANGUP_OFF)
        return;
    if ((kl_event_caps(&s->ev.loop) & KL_EVENT_CAP_HANGUP) &&
        kl_watcher_add(&s->ev, conn->stream.fd, KL_EVENT_HANGUP, async_on_hangup, conn) == 0) {
        conn->hup_state = KL_HUP_WATCHED;
        return;
    }
    if (!async_hup_can_peek(s))
        return;
    conn->hup_state = KL_HUP_SWEPT;
    s->hup_sweep_conns++;
    async_hup_sweep_arm(s);
}

static void async_unwatch_hangup(KlHttpServer *s, KlHttpConn *conn) {
    /* One watcher carries the hang-up watch and a suspended stream's write watch (async_stream_watch_write). */
    if (conn->hup_state == KL_HUP_WATCHED || conn->stream_write_watched)
        kl_watcher_del(&s->ev, conn->stream.fd);
    conn->stream_write_watched = 0;
    if (conn->hup_state == KL_HUP_SWEPT || conn->hup_state == KL_HUP_SWEEP_HIT) {
        if (--s->hup_sweep_conns <= 0) {
            s->hup_sweep_conns = 0;
            if (s->hup_sweep_armed) {   /* nothing left to sweep: no probe after this */
                (void)kl_timer_cancel(&s->ev, s->hup_sweep_timer);
                s->hup_sweep_armed = 0;
            }
        }
    }
    conn->hup_state = KL_HUP_NONE;
}

/* One non-consuming probe: 1 = FIN seen (nothing queued ahead of it), 2 = reset, 0 = nothing
 * terminal observed or indeterminate (bytes queued, would-block, any other error). */
static int async_hup_probe(const KlHttpServer *s, const KlHttpConn *conn) {
    char b;
    kl_ssize_t r = kl_sock_recv_peek(s->ev.sockets, conn->stream.fd, &b, 1);
    if (r == 0) return 1;
    if (r < 0 && kl_sock_io_status(s->ev.sockets) == KL_IO_RESET) return 2;
    return 0;
}

static void async_hup_sweep(void *user_data) {
    KlHttpServer *s = user_data;
    s->hup_sweep_armed = 0;
    /* First pass, no list changes: probe every swept connection once. */
    for (KlAsyncOp *op = s->async_ops; op; op = op->next) {
        KlHttpConn *c = op->conn;
        if (!c || c->async_op != op || c->hup_state != KL_HUP_SWEPT) continue;
        int r = async_hup_probe(s, c);
        if (r == 1 && s->config.suspended_hangup == KL_HTTP_SUSPEND_HANGUP_RESET_ONLY) {
            async_unwatch_hangup(s, c);     /* leaves the sweep; the op runs on */
            c->hup_state = KL_HUP_FIN_KEPT;
        } else if (r != 0) {
            c->hup_state = KL_HUP_SWEEP_HIT;
        }
    }
    /* Second pass: cancel the hits. on_cancel is user code and may change the list, so restart from the
     * head after each cancel; each one retires its op, so this terminates. */
    for (;;) {
        KlAsyncOp *hit = NULL;
        for (KlAsyncOp *op = s->async_ops; op; op = op->next)
            if (op->conn && op->conn->async_op == op && op->conn->hup_state == KL_HUP_SWEEP_HIT) {
                hit = op;
                break;
            }
        if (!hit) break;
        kl_async_cancel(s, hit);
    }
    async_hup_sweep_arm(s);   /* again only if a swept connection remains */
}

static void async_on_hangup(KlSocketHandle fd, KlEventMask ready, void *user_data);

/* ── Suspended streams on a readiness loop ───────────────────────────────
 * A streamed response written while suspended goes through its drain: what the socket does not take at
 * once waits there. kl_async_suspend took the fd out of the loop, so nothing would move it until the
 * producer wrote again, and on_drain would never fire: a producer pacing itself on on_drain stalled for
 * good. While output waits, the suspended connection's fd is therefore also watched for writability,
 * on the same watcher as the hang-up watch (combined, the hang-up interest is not reported on its own:
 * a failed send then ends the op instead), and each writable event flushes the drain, which fires
 * on_drain once it empties. A completion loop needs none of this: its write completion moves the drain. */

static void async_stream_watch_write(KlHttpServer *s, KlHttpConn *conn, int on) {
    if (!!on == conn->stream_write_watched) return;
    const int hup = conn->hup_state == KL_HUP_WATCHED;
    int rc;
    if (on)
        rc = hup ? kl_watcher_mod(&s->ev, conn->stream.fd, (KlEventMask)(KL_EVENT_HANGUP | KL_EVENT_WRITE))
                 : kl_watcher_add(&s->ev, conn->stream.fd, KL_EVENT_WRITE, async_on_hangup, conn);
    else if (hup)
        rc = kl_watcher_mod(&s->ev, conn->stream.fd, KL_EVENT_HANGUP);
    else {
        kl_watcher_del(&s->ev, conn->stream.fd);
        rc = 0;
    }
    if (rc == 0) conn->stream_write_watched = on ? 1 : 0;
}

/* The suspended connection's streamed response has output waiting (readiness). */
static int async_stream_pending(const KlHttpConn *conn) {
    return conn->res.body_mode == KL_HTTP_BODY_STREAM && conn->res.drain_enabled &&
           kl_drain_pending(&conn->res.drain);
}

/* Writable while suspended with stream output waiting: flush it. kl_drain_flush fires on_drain when the
 * drain empties; that is user code, and may write more, end the stream, complete or cancel the op. */
static void async_stream_progress(KlHttpServer *s, KlHttpConn *conn) {
    KlAsyncOp *op = conn->async_op;
    if (!op || conn->state != KL_HTTP_CONN_SUSPENDED) return;
    int r = kl_drain_flush(&conn->res.drain);
    if (conn->async_op != op || conn->state != KL_HTTP_CONN_SUSPENDED)
        return;                                 /* on_drain completed or cancelled the op */
    if (r < 0) {
        kl_async_cancel(s, op);                 /* the send failed: the client is gone */
        return;
    }
    /* Output moved: a producer refused with WOULD_BLOCK may write again (bounded streaming). */
    if (conn->res.stream_writable_armed) {
        kl_http_response_stream_progress(&conn->res);
        if (conn->async_op != op || conn->state != KL_HTTP_CONN_SUSPENDED)
            return;                             /* the callback completed or cancelled the op */
    }
    if (!async_stream_pending(conn) &&
        !(conn->res.stream_writable_armed && conn->res.stream_writable_fn))
        async_stream_watch_write(s, conn, 0);
}

/* http_internal.h: the readiness loop, once per tick just before it waits: a suspended connection whose
 * streamed response has output waiting is watched for writability, and one that has none is not. Writes
 * happen only on the loop thread, in the tick's dispatch, sweeps and timers, so whatever buffered output
 * arms the watch before the loop waits again: no wakeup is missed and nothing polls. */
void kl_async_sync_stream_interest(KlHttpServer *s) {
    for (KlAsyncOp *op = s->async_ops; op; op = op->next) {
        KlHttpConn *c = op->conn;
        if (!c || c->async_op != op || c->state != KL_HTTP_CONN_SUSPENDED) continue;
        /* Also while the writable edge is armed with nothing pending: a WOULD_BLOCK followed by a
         * write through the legacy writer can empty the drain, and with no watch no progress point
         * would come to fire the edge. A writable socket then reports at once. Only with a callback
         * to fire: an armed edge nobody listens for would hold a watch on an always-writable socket. */
        async_stream_watch_write(s, c, async_stream_pending(c) ||
                                       (c->res.stream_writable_armed && c->res.stream_writable_fn));
    }
}

/* The client hung up (or the connection failed) while suspended. Runs from the loop's I/O
 * processing, outside any frame driving the connection. */
static void async_on_hangup(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    KlHttpConn *conn = user_data;
    KlHttpServer *s = conn->hup_server;
    (void)fd;
    if (conn->stream_write_watched && (ready & KL_EVENT_WRITE)) {
        async_stream_progress(s, conn);   /* writable: move the suspended stream's output */
        return;
    }
    if (!(ready & (KL_EVENT_HANGUP | KL_EVENT_READ)))
        return;
    /* A hang-up registration has no READ interest, so a READ report can only be a backend's legacy
     * mapping of HUP/ERR (an error that did not also shut the receive side): a failure. */
    if (!(ready & KL_EVENT_HANGUP))
        ready = (KlEventMask)(KL_EVENT_HANGUP | KL_EVENT_ERROR);
    /* First report: stop watching, whatever follows. The watcher carries the write watch too, and a
     * connection on the sweep (a write watch with no hang-up registration) leaves the sweep here, so
     * its count stays right. The next tick's kl_async_sync_stream_interest re-arms the write watch
     * while output waits. */
    async_unwatch_hangup(s, conn);
    KlAsyncOp *op = conn->async_op;
    if (!op || conn->state != KL_HTTP_CONN_SUSPENDED)
        return;
    if (!(ready & KL_EVENT_ERROR) &&
        s->config.suspended_hangup == KL_HTTP_SUSPEND_HANGUP_RESET_ONLY) {
        /* An orderly FIN, which may be a half-close: the op runs on and its response is delivered.
         * No hang-up is watched after it: a reset after this FIN ends the op only while stream
         * output waits (the write watch, re-armed next tick, sees it); otherwise it is bounded by
         * the op's own deadline. */
        conn->hup_state = KL_HUP_FIN_KEPT;
        return;
    }
    kl_async_cancel(s, op);   /* on_cancel once, the connection closed, nothing written */
}

/* ── Deadline sweep ─────────────────────────────────────────────────── */

/* on_deadline is a trigger that usually completes or cancels its own op, and may complete, cancel or
 * free any other (an op's on_cancel commonly frees its context). The scan keeps its place in
 * s->deadline_sweep_next rather than in a local: async_retire advances that cursor past an op it
 * unlinks, before the op's on_cancel can free it, so the scan never follows a freed pointer. As
 * before, ops are visited in list order, each at most once per sweep: an op suspended during the
 * sweep (added at the head) or a deadline set again from a callback waits for the next tick. */
void kl_async_sweep_deadlines(KlHttpServer *s, uint64_t now) {
    s->deadline_sweep_next = s->async_ops;
    while (s->deadline_sweep_next) {
        KlAsyncOp *op = s->deadline_sweep_next;
        s->deadline_sweep_next = op->next;
        if (op->_terminal || op->deadline_ms == 0 || now < op->deadline_ms)
            continue;
        op->deadline_ms = 0;   /* fire on_deadline at most once */
        if (op->on_deadline)
            op->on_deadline(op, op->user_data);   /* may retire or free any op; the cursor follows */
    }
}

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

    async_watch_hangup(s, conn);
    return 0;
}

/* Idempotently retire an op: remove it from the active list, clear the
 * connection's back-pointer, and mark it terminal. Returns the connection it was
 * attached to on the first (pending→terminal) call, or NULL if the op was already
 * retired, the exactly-one-terminal guard shared by complete and cancel. */
static KlHttpConn *async_retire(KlHttpServer *s, KlAsyncOp *op) {
    if (op->_terminal) return NULL;
    op->_terminal = 1;

    if (s->deadline_sweep_next == op)   /* a deadline sweep's next op: keep its place */
        s->deadline_sweep_next = op->next;
    KlAsyncOp **pp = &s->async_ops;
    while (*pp) {
        if (*pp == op) { *pp = op->next; break; }
        pp = &(*pp)->next;
    }
    KlHttpConn *conn = op->conn;
    if (conn && conn->async_op == op) {
        conn->async_op = NULL;
        async_unwatch_hangup(s, conn);   /* before anything re-registers or closes the fd */
    }
    return conn;
}

/* Register a resumed connection's fd for `mask`. A nested complete inside on_resume may already
 * have registered it, and epoll refuses a second add: modify it instead. An add for no interest
 * (a paused body read) need not clear what that nested complete registered (on kqueue it is an
 * empty change list, leaving READ enabled), so set it explicitly with a modify as well. 0, or -1
 * if neither worked. */
static int async_rearm(KlHttpServer *s, KlHttpConn *conn, KlEventMask mask) {
    if (kl_event_add(&s->ev.loop, conn->stream.fd, mask, &conn->stream) == 0) {
        if (mask == 0)
            return kl_event_mod(&s->ev.loop, conn->stream.fd, mask, &conn->stream);
        return 0;
    }
    return kl_event_mod(&s->ev.loop, conn->stream.fd, mask, &conn->stream);
}

/* Release a resumed connection on a readiness loop. Its fd is not necessarily out of the loop: a
 * nested complete inside on_resume registered it again. Deleting an fd that is not registered is
 * harmless; leaving a closed one registered is not (poll and the Windows select() backend keep its entry, and poll
 * reports it on every wait). */
static void async_release(KlHttpServer *s, KlHttpConn *conn) {
    kl_event_del(&s->ev.loop, conn->stream.fd);
    kl_http_server_conn_release(s, conn);
}

void kl_async_complete(KlHttpServer *s, KlAsyncOp *op) {
    if (!s || !op) return;

    /* Exactly-one-terminal: a second complete (or a complete racing a cancel)
     * is a no-op: no double on_resume, no double release. */
    KlHttpConn *conn = async_retire(s, op);
    if (!conn) return;

    /* The absolute body deadline measures the client's upload, not the server's own work: move its
     * start past the time spent suspended, so a resume that awaits the body is not 408'd for it. */
    if (conn->body_start_ms > 0) {
        uint64_t now = kl_monotonic_ms();
        if (now > conn->suspend_start_ms) conn->body_start_ms += now - conn->suspend_start_ms;
        if (conn->body_start_ms > now) conn->body_start_ms = now;
    }

    /* Transition back from SUSPENDED so on_resume can set the final state */
    conn->state = KL_HTTP_CONN_PROCESSING;

    /* Call the resume callback. It is a frame driving this connection: an op it starts and ends
     * at once (a cancel because the work could not be started) is left to this function. */
    if (op->on_resume) {
        conn->dispatch_depth++;
        op->on_resume(op, op->user_data);
        conn->dispatch_depth--;
    }

    /* If the handler suspended again, a new op is active, nothing to do */
    if (conn->state == KL_HTTP_CONN_SUSPENDED)
        return;

    /* Default the post-resume state the same way conn_process() defaults it after a
     * synchronous handler returns: a resume that only built the response (the common
     * shape, where done_fn writes it and on_resume has nothing left to do) leaves the
     * connection in PROCESSING, which neither drive path acts on. The readiness path
     * below would re-register no fd (silent hang) and the completion path's
     * comp_after_state() would take its CLOSED arm (socket closed, no response). A
     * streaming response was already written by the handler, so it follows the
     * streaming transition instead: keep SENDING only while a drain is pending. */
    if (conn->state == KL_HTTP_CONN_PROCESSING) {
        if (conn->res.body_mode == KL_HTTP_BODY_STREAM &&
            !(conn->res.drain_enabled && kl_drain_pending(&conn->res.drain)))
            conn->state = KL_HTTP_CONN_CLOSED;
        else
            conn->state = KL_HTTP_CONN_SENDING;
    }

    /* Completed inside a frame that is already driving this connection (the handler that
     * suspended it, a body reader's on_data, an outer resume): that frame sends or closes it when
     * it returns, exactly as if the op had never been started. Driving it here as well sent the
     * response twice, or released the slot twice. Readiness: the suspend took the fd out of the
     * loop, and the frame's transition modifies the registration, so put it back. */
    if (conn->dispatch_depth > 0) {
        if (conn->state != KL_HTTP_CONN_CLOSED &&
            !(kl_event_caps(&s->ev.loop) & KL_EVENT_CAP_COMPLETION))
            (void)kl_event_add(&s->ev.loop, conn->stream.fd, KL_EVENT_READ, &conn->stream);
        return;
    }

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

    /* Resumed into the body phase (kl_http_request_await_body): the body bytes kept across the
     * suspend go to the body core first; they may finish the body, or end the request. */
    if (new_state == KL_HTTP_CONN_READING_BODY)
        new_state = kl_http_conn_resume_body(conn);

    /* With TLS, the rest of the record the headers came in may be held decrypted in the engine,
     * where no socket readiness will report it: read it on now. */
    if (kl_http_conn_tls_body_pending(conn, new_state))
        new_state = kl_http_conn_on_readable(conn, &s->router);

    /* Try immediate send if response is ready */
    if (new_state == KL_HTTP_CONN_SENDING)
        new_state = kl_http_conn_on_writable(conn);

    /* Re-register FD with appropriate mask. Exhaustive over KlHttpConnState with no default: a new
     * member must be a compile error here, not a connection that matches no arm (which is precisely
     * what DRAINING did before it was added). */
    switch (new_state) {
    case KL_HTTP_CONN_SENDING:
        if (async_rearm(s, conn, KL_EVENT_WRITE) < 0)
            async_release(s, conn);
        break;
    case KL_HTTP_CONN_DRAINING:
        /* The handler answered without consuming the request body, so the response is flushed with
         * input still unread (#278). Arm READ so each readable tick discards one bounded chunk,
         * exactly as the main readiness transition does; the idle sweep enforces the byte and time
         * bounds and releases. */
        if (async_rearm(s, conn, KL_EVENT_READ) < 0)
            async_release(s, conn);
        break;
    case KL_HTTP_CONN_READING:
        if (async_rearm(s, conn, KL_EVENT_READ) < 0)
            async_release(s, conn);
        break;
    case KL_HTTP_CONN_READING_BODY:
        /* The resume awaits (more of) the body: read on, as the main readiness transition does.
         * A paused body read (kl_http_request_pause_body) is registered with no interest, so the
         * resume's kl_event_mod finds it. */
        if (async_rearm(s, conn, conn->stream.read_paused ? 0 : KL_EVENT_READ) < 0)
            async_release(s, conn);
        break;
    case KL_HTTP_CONN_SUSPENDED:
        /* Suspended again by the resume callback (a handler chaining a second async op): it holds
         * its own op and is exempt from the idle sweep, so leave it parked and unregistered. */
        break;
    case KL_HTTP_CONN_PROCESSING:
        /* Normalised to SENDING or CLOSED a few lines above, precisely because no drive path acts
         * on it, so this is unreachable. */
        break;
    /* No resume leaves a connection in these (they precede a request, or are entered only from the
     * dispatch), so nothing is registered; named so the no-arm case is deliberate rather than an
     * accident of an if-chain. */
    case KL_HTTP_CONN_PROXY_HEADER:
    case KL_HTTP_CONN_TLS_HANDSHAKE:
    case KL_HTTP_CONN_WEBSOCKET:
    case KL_HTTP_CONN_HTTP2:
        break;
    case KL_HTTP_CONN_CLOSED:
        async_release(s, conn);
        break;
    }
}

/* Retire the op and fire on_cancel. The connection it was attached to (still SUSPENDED unless
 * on_cancel suspended it again), or NULL if the op was already retired. on_cancel may free the op:
 * nothing reads it afterwards. */
static KlHttpConn *async_cancel_op(KlHttpServer *s, KlAsyncOp *op) {
    /* Exactly-one-terminal: a cancel racing a completion (or a double cancel) is
     * a no-op: on_cancel fires at most once, and never after on_resume. */
    KlHttpConn *conn = async_retire(s, op);
    if (!conn) return NULL;
    if (op->on_cancel)
        op->on_cancel(op, op->user_data);
    return conn;
}

void kl_async_cancel_detached(KlHttpServer *s, KlAsyncOp *op) {
    if (!s || !op) return;
    (void)async_cancel_op(s, op);
}

void kl_async_cancel(KlHttpServer *s, KlAsyncOp *op) {
    if (!s || !op) return;
    KlHttpConn *conn = async_cancel_op(s, op);
    /* The connection was waiting on the op and nothing else will end it: it is out of the
     * readiness loop, has nothing posted on a completion loop, and the sweep exempts a suspended
     * connection. Close it. Inside a frame already driving the connection (the handler, on_data,
     * a resume), that frame closes it when it returns, so it is released once. */
    if (!conn || conn->state != KL_HTTP_CONN_SUSPENDED) return;
    conn->state = KL_HTTP_CONN_CLOSED;
    if (conn->dispatch_depth == 0)
        kl_http_server_conn_release(s, conn);
}
