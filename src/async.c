#include <keel/async.h>
#include <keel/server.h>
#include <keel/connection.h>
#include <stdint.h>
#include "internal.h"

/* ── Tagged pointer helpers ────────────────────────────────────────── */

static void *watcher_tag(KlWatcher *w) {
    return (void *)((uintptr_t)w | 1);
}

/* ── KlWatcher ─────────────────────────────────────────────────────── */

int kl_watcher_add(KlServer *s, int fd, KlEventMask mask,
                   KlWatcherFn on_ready, void *user_data)
{
    if (!s || fd < 0 || !on_ready) return -1;

    KlAllocator *a = &s->alloc_storage;
    KlWatcher *w = kl_malloc(a, sizeof(KlWatcher));
    if (!w) return -1;

    w->fd = fd;
    w->mask = mask;
    w->on_ready = on_ready;
    w->user_data = user_data;
    w->next = s->watchers;
    s->watchers = w;

    if (kl_event_add(&s->loop, fd, mask, watcher_tag(w)) < 0) {
        s->watchers = w->next;
        kl_free(a, w, sizeof(KlWatcher));
        return -1;
    }

    return 0;
}

int kl_watcher_mod(KlServer *s, int fd, KlEventMask mask) {
    if (!s || fd < 0) return -1;

    KlWatcher *w = s->watchers;
    while (w && w->fd != fd) w = w->next;
    if (!w) return -1;

    w->mask = mask;
    return kl_event_mod(&s->loop, fd, mask, watcher_tag(w));
}

void kl_watcher_rearm(KlServer *s, int fd) {
    if (!s || fd < 0) return;

    KlWatcher *w = s->watchers;
    while (w && w->fd != fd) w = w->next;
    if (!w) return;

    kl_event_mod(&s->loop, fd, w->mask, watcher_tag(w));
}

void kl_watcher_del(KlServer *s, int fd) {
    if (!s || fd < 0) return;

    KlAllocator *a = &s->alloc_storage;
    KlWatcher **pp = &s->watchers;
    while (*pp) {
        if ((*pp)->fd == fd) {
            KlWatcher *w = *pp;
            *pp = w->next;
            kl_event_del(&s->loop, fd);
            kl_free(a, w, sizeof(KlWatcher));
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ── KlAsyncOp ─────────────────────────────────────────────────────── */

int kl_async_suspend(KlServer *s, KlConn *conn, KlAsyncOp *op) {
    if (!s || !conn || !op) return -1;
    if (conn->state == KL_CONN_SUSPENDED) return -1;

    /* Remove client FD from event loop */
    kl_event_del(&s->loop, conn->fd);

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
    if (new_state == KL_CONN_SENDING) {
        new_state = kl_conn_on_writable(conn);
    }

    /* Re-register FD with appropriate mask */
    if (new_state == KL_CONN_SENDING) {
        kl_event_add(&s->loop, conn->fd, KL_EVENT_WRITE, conn);
    } else if (new_state == KL_CONN_READING) {
        kl_event_add(&s->loop, conn->fd, KL_EVENT_READ, conn);
    } else if (new_state == KL_CONN_CLOSED) {
        kl_server_conn_release(s, conn);
    }
}
