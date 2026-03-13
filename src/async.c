#include <keel/async.h>
#include <keel/server.h>
#include <keel/connection.h>
#include <stdint.h>
#include "internal.h"

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
    ctx->loop.alloc = alloc;
    if (kl_event_init(&ctx->loop) < 0) {
        ctx->last_error = KL_ERR_EVENT_INIT;
        return -1;
    }
    return 0;
}

void kl_event_ctx_free(KlEventCtx *ctx) {
    if (!ctx) return;
    while (ctx->watchers) {
        KlWatcher *w = ctx->watchers;
        ctx->watchers = w->next;
        kl_event_del(&ctx->loop, w->fd);
        kl_free(ctx->alloc, w, sizeof(KlWatcher));
    }
    kl_event_close(&ctx->loop);
}

/* ── KlWatcher ─────────────────────────────────────────────────────── */

int kl_watcher_add(KlEventCtx *ctx, int fd, KlEventMask mask,
                   KlWatcherFn on_ready, void *user_data)
{
    if (!ctx || fd < 0 || !on_ready) return -1;

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

int kl_watcher_mod(KlEventCtx *ctx, int fd, KlEventMask mask) {
    if (!ctx || fd < 0) return -1;

    KlWatcher *w = ctx->watchers;
    while (w && w->fd != fd) w = w->next;
    if (!w) return -1;

    w->mask = mask;
    ctx->dispatch_dirty = 1;  /* suppress auto-rearm in kl_event_dispatch */
    return kl_event_mod(&ctx->loop, fd, mask, watcher_tag(w));
}

int kl_watcher_rearm(KlEventCtx *ctx, int fd) {
    if (!ctx || fd < 0) return -1;

    KlWatcher *w = ctx->watchers;
    while (w && w->fd != fd) w = w->next;
    if (!w) return 0;  /* removed during callback — safe no-op */

    return kl_event_mod(&ctx->loop, fd, w->mask, watcher_tag(w));
}

void kl_watcher_del(KlEventCtx *ctx, int fd) {
    if (!ctx || fd < 0) return;

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

    KlEvent stack_buf[KL_CTX_STACK_EVENTS];
    KlEvent *events = stack_buf;

    if (max_events > KL_CTX_STACK_EVENTS) {
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

    /* Add to server's active ops list */
    op->next = s->async_ops;
    s->async_ops = op;

    return 0;
}

void kl_async_complete(KlServer *s, KlAsyncOp *op) {
    if (!s || !op || !op->conn) return;

    KlConn *conn = op->conn;

    /* Remove from active ops list */
    KlAsyncOp **pp = &s->async_ops;
    while (*pp) {
        if (*pp == op) {
            *pp = op->next;
            break;
        }
        pp = &(*pp)->next;
    }

    conn->async_op = NULL;

    /* Transition back from SUSPENDED so on_resume can set the final state */
    conn->state = KL_CONN_PROCESSING;

    /* Call the resume callback */
    if (op->on_resume)
        op->on_resume(op, op->user_data);

    /* If the handler suspended again, a new op is active — nothing to do */
    if (conn->state == KL_CONN_SUSPENDED)
        return;

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
