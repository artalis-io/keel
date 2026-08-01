#include <keel/async.h>
#include <keel/timer.h>
#include <keel/server.h>
#include <keel/connection.h>
#include <stdint.h>
#include "internal.h"
#include "event_caps.h"
#include "io_engine.h"   /* kl_comp_run — the generic completion tick */

/* ── Tagged pointer helpers ────────────────────────────────────────── */

static void *watcher_tag(KlWatcher *w) {
    return (void *)((uintptr_t)w | 1);
}

/* ── KlEventCtx ───────────────────────────────────────────────────── */

int kl_event_ctx_init(KlEventCtx *ctx, KlAllocator *alloc) {
    if (!ctx || !alloc) {
        if (ctx) ctx->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    ctx->alloc = alloc;
    ctx->watchers = NULL;
    ctx->dispatch_dirty = 0;
    ctx->last_error = KL_ERR_NONE;
    ctx->timers = NULL;
    ctx->timer_count = 0;
    ctx->timer_cap = 0;
    ctx->timer_next_id = 0;
    ctx->sockets = NULL;              /* POSIX by default (internal seam) */
    ctx->loop.alloc = alloc;
    ctx->loop.ops = NULL;             /* compiled-in backend (no runtime provider) */
    if (kl_event_init(&ctx->loop) < 0) {
        ctx->last_error = KL_ERR_EVENT_INIT;
        return -1;
    }
    return 0;
}

void kl_event_ctx_free(KlEventCtx *ctx) {
    if (!ctx) return;
    if (ctx->timers)
        kl_free(ctx->alloc, ctx->timers,
                (size_t)ctx->timer_cap * sizeof(KlTimerEntry));
    ctx->timers = NULL;
    ctx->timer_count = 0;
    ctx->timer_cap = 0;
    while (ctx->watchers) {
        KlWatcher *w = ctx->watchers;
        ctx->watchers = w->next;
        kl_event_del(&ctx->loop, w->fd);
        kl_free(ctx->alloc, w, sizeof(KlWatcher));
    }
    kl_event_close(&ctx->loop);
}

/* PAL Phase 7/8: negotiate an event loop against a socket provider. Pure over the
 * two axes (event model × handle domain), taking the loop caps explicitly so it is
 * unit-testable without a real backend. F3-safe: reads only capability bits, never
 * crosses the socket↔event seam. A NULL provider is the built-in POSIX default. */
int kl_caps_compatible(unsigned ev_caps, const struct KlSocketProvider *sockets) {
    if (ev_caps & KL_EVENT_CAP_COMPLETION) {
        /* Completion model (Phase 8, IOCP): the provider must route I/O through the
         * loop's overlapped submit path rather than synchronous send/recv. */
        return kl_socket_provider_has_cap(sockets, KL_SOCK_CAP_OVERLAPPED);
    }
    /* Readiness model (shipped): provider exposes native fds AND the loop is a
     * native-fd readiness poller. */
    int provider_native = kl_socket_provider_has_cap(sockets, KL_SOCK_CAP_NATIVE_FD);
    int loop_watches_fds =
        (ev_caps & (KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD)) ==
        (KL_EVENT_CAP_READINESS | KL_EVENT_CAP_NATIVE_FD);
    return provider_native && loop_watches_fds;
}

/* PAL Phase 7: negotiate the ctx's event loop against its socket provider. The
 * neutral meeting point — this ctx already holds both the loop and the provider, so
 * the socket seam and the event axis stay decoupled (F3). Rejects an incoherent
 * pairing here rather than failing obscurely at kl_event_add. */
int kl_event_ctx_sockets_compatible(const struct KlEventCtx *ctx) {
    if (!ctx) return 0;
    return kl_caps_compatible(kl_event_caps(&ctx->loop), ctx->sockets);
}

/* ── KlWatcher ─────────────────────────────────────────────────────── */

int kl_watcher_add(KlEventCtx *ctx, KlSocketHandle fd, KlEventMask mask,
                   KlWatcherFn on_ready, void *user_data)
{
    if (!ctx || !kl_handle_valid(fd) || !on_ready) return -1;

    /* Idempotent: if fd is already watched, update the existing watcher in
     * place rather than prepending a duplicate node.  A duplicate would
     * orphan the first node (leaked, never dispatched) on backends whose
     * kl_event_add tolerates re-add (kqueue, poll).  Mirrors the event_poll
     * upsert semantics. */
    for (KlWatcher *e = ctx->watchers; e; e = e->next) {
        if (e->fd == fd) {
            e->mask = mask;
            e->on_ready = on_ready;
            e->user_data = user_data;
            ctx->dispatch_dirty = 1;  /* suppress auto-rearm if in a callback */
            return kl_event_mod(&ctx->loop, fd, mask, watcher_tag(e));
        }
    }

    KlAllocator *a = ctx->alloc;
    KlWatcher *w = kl_malloc(a, sizeof(KlWatcher));
    if (!w) return -1;

    w->fd = fd;
    w->mask = mask;
    w->on_ready = on_ready;
    w->user_data = user_data;
    w->next = ctx->watchers;
    ctx->watchers = w;

    if (kl_event_add(&ctx->loop, fd, mask, watcher_tag(w)) < 0) {
        ctx->watchers = w->next;
        kl_free(a, w, sizeof(KlWatcher));
        return -1;
    }

    return 0;
}

int kl_watcher_mod(KlEventCtx *ctx, KlSocketHandle fd, KlEventMask mask) {
    if (!ctx || !kl_handle_valid(fd)) return -1;

    KlWatcher *w = ctx->watchers;
    while (w && w->fd != fd) w = w->next;
    if (!w) return -1;

    w->mask = mask;
    ctx->dispatch_dirty = 1;  /* suppress auto-rearm in kl_event_dispatch */
    return kl_event_mod(&ctx->loop, fd, mask, watcher_tag(w));
}

int kl_watcher_rearm(KlEventCtx *ctx, KlSocketHandle fd) {
    if (!ctx || !kl_handle_valid(fd)) return -1;

    KlWatcher *w = ctx->watchers;
    while (w && w->fd != fd) w = w->next;
    if (!w) return 0;  /* removed during callback — safe no-op */

    return kl_event_mod(&ctx->loop, fd, w->mask, watcher_tag(w));
}

void kl_watcher_del(KlEventCtx *ctx, KlSocketHandle fd) {
    if (!ctx || !kl_handle_valid(fd)) return;

    KlAllocator *a = ctx->alloc;
    KlWatcher **pp = &ctx->watchers;
    while (*pp) {
        if ((*pp)->fd == fd) {
            KlWatcher *w = *pp;
            *pp = w->next;
            kl_event_del(&ctx->loop, fd);
            kl_free(a, w, sizeof(KlWatcher));
            ctx->dispatch_dirty = 1;  /* suppress auto-rearm */
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ── kl_event_ctx_run ──────────────────────────────────────────────── */

#define KL_CTX_STACK_EVENTS 64

int kl_event_ctx_run(KlEventCtx *ctx, int max_events, int timeout_ms) {
    if (!ctx || max_events <= 0) return -1;

    /* Clamp timeout to next timer deadline */
    timeout_ms = kl_timer_next_timeout(ctx, timeout_ms);

    /* Completion loop (IOCP): drive the generic completion tick instead of the
     * readiness wait/dispatch, so standalone consumers (UDP/DNS in 8b-4c) run on a
     * completion loop too. Dead on readiness backends (none advertise COMPLETION),
     * so the path below is unchanged there (byte-identical). */
    if (kl_event_caps(&ctx->loop) & KL_EVENT_CAP_COMPLETION)
        return kl_comp_run(ctx, max_events, timeout_ms);

    KlEvent stack_buf[KL_CTX_STACK_EVENTS];
    KlEvent *events = stack_buf;

    if (max_events > KL_CTX_STACK_EVENTS) {
        if ((size_t)max_events > SIZE_MAX / sizeof(KlEvent)) return -1;
        events = kl_malloc(ctx->alloc, (size_t)max_events * sizeof(KlEvent));
        if (!events) return -1;
    }

    int n = kl_event_wait(&ctx->loop, events, max_events, timeout_ms);
    if (n > 0) {
        for (int i = 0; i < n; i++)
            kl_event_dispatch(ctx, &events[i]);
    }

    if (events != stack_buf)
        kl_free(ctx->alloc, events, (size_t)max_events * sizeof(KlEvent));

    /* Fire expired timers */
    kl_timer_fire(ctx);

    return n < 0 ? -1 : n;
}

/* ── KlAsyncOp ─────────────────────────────────────────────────────── */

int kl_async_suspend(KlServer *s, KlConn *conn, KlAsyncOp *op) {
    if (!s || !conn || !op) return -1;
    if (conn->state == KL_CONN_SUSPENDED) return -1;

    /* Remove client FD from event loop */
    kl_event_del(&s->ev.loop, conn->fd);

    /* Park the connection */
    conn->state = KL_CONN_SUSPENDED;
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
 * retired — the exactly-one-terminal guard shared by complete and cancel. */
static KlConn *async_retire(KlServer *s, KlAsyncOp *op) {
    if (op->_terminal) return NULL;
    op->_terminal = 1;

    KlAsyncOp **pp = &s->async_ops;
    while (*pp) {
        if (*pp == op) { *pp = op->next; break; }
        pp = &(*pp)->next;
    }
    KlConn *conn = op->conn;
    if (conn && conn->async_op == op)
        conn->async_op = NULL;
    return conn;
}

void kl_async_complete(KlServer *s, KlAsyncOp *op) {
    if (!s || !op) return;

    /* Exactly-one-terminal: a second complete (or a complete racing a cancel)
     * is a no-op — no double on_resume, no double release. */
    KlConn *conn = async_retire(s, op);
    if (!conn) return;

    /* Transition back from SUSPENDED so on_resume can set the final state */
    conn->state = KL_CONN_PROCESSING;

    /* Call the resume callback */
    if (op->on_resume)
        op->on_resume(op, op->user_data);

    /* If the handler suspended again, a new op is active — nothing to do */
    if (conn->state == KL_CONN_SUSPENDED)
        return;

    /* On a completion loop, drive the completion send path instead of re-arming the fd —
     * the readiness re-register + kl_conn_on_writable below is a no-op there (kl_event_add
     * is inert, kl_conn_on_writable does readiness socket writes). Branch on the abstract
     * event axis (not the backend); the completion send lives behind an io_engine seam so
     * async.c stays free of completion internals (8e-2). */
    if (kl_event_caps(&s->ev.loop) & KL_EVENT_CAP_COMPLETION) {
        kl_io_engine_resume_completion(s, conn);
        return;
    }

    /* Handler completed — re-register FD and drive state machine */
    KlConnState new_state = conn->state;

    /* Try immediate send if response is ready */
    if (new_state == KL_CONN_SENDING)
        new_state = kl_conn_on_writable(conn);

    /* Re-register FD with appropriate mask */
    if (new_state == KL_CONN_SENDING) {
        if (kl_event_add(&s->ev.loop, conn->fd, KL_EVENT_WRITE, conn) < 0)
            kl_server_conn_release(s, conn);
    } else if (new_state == KL_CONN_READING) {
        if (kl_event_add(&s->ev.loop, conn->fd, KL_EVENT_READ, conn) < 0)
            kl_server_conn_release(s, conn);
    } else if (new_state == KL_CONN_CLOSED) {
        kl_server_conn_release(s, conn);
    }
}

void kl_async_cancel(KlServer *s, KlAsyncOp *op) {
    if (!s || !op) return;

    /* Exactly-one-terminal: a cancel racing a completion (or a double cancel) is
     * a no-op — on_cancel fires at most once, and never after on_resume. */
    if (async_retire(s, op) == NULL) return;

    if (op->on_cancel)
        op->on_cancel(op, op->user_data);
}
