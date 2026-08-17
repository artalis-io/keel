/*
 * event_ctx.c — KlEventCtx lifecycle + the generic KlWatcher FD-callback API +
 * kl_event_ctx_run (the portable wait/dispatch or completion tick).
 *
 * Split out of async.c in the freestanding phase (F0): this is the CLIENT-usable
 * half of the old async.c — it references only the event/timer/socket-provider
 * seams (no KlServer / KlConn / connection state machine), so a completion HTTP
 * client can link it WITHOUT dragging in the server connection driver. The
 * server-side connection-suspension API (kl_async_suspend/complete/cancel) stays
 * in async.c, which references kl_conn_on_writable / kl_server_conn_release /
 * kl_io_engine_resume_completion — the exact server symbols that must not leak
 * into the freestanding client archive.
 */
#include <keel/event_ctx.h>
#include <keel/timer.h>
#include <stdint.h>
#include <limits.h>            /* INT_MAX — dispatch-depth overflow guard (R3b-W) */
#ifndef KEEL_FREESTANDING
#include <assert.h>            /* debug precondition in kl_event_ctx_free (hosted only) */
#endif
#include "socket.h"        /* kl_socket_provider_has_cap + KL_SOCK_CAP_* (caps negotiation) */
#include "event_caps.h"
#include "io_engine.h"   /* kl_comp_run — the generic completion tick */
#include "watcher_internal.h"   /* kl_watcher_add_detached — completion connect (LC-0) */

/* ── Tagged pointer helpers ────────────────────────────────────────── */

static void *watcher_tag(const KlWatcher *w) {
    return (void *)((uintptr_t)w | 1);
}

/* ── KlEventCtx ───────────────────────────────────────────────────── */

int kl_event_ctx_init_ex(KlEventCtx *ctx, KlAllocator *alloc,
                         const KlEventProvider *event_provider) {
    if (!ctx || !alloc) {
        if (ctx) ctx->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    ctx->alloc = alloc;
    ctx->watchers = NULL;
    ctx->dispatch_dirty = 0;
    ctx->dispatch_depth = 0;          /* R3b-W: batch-dispatch reclamation */
    ctx->retired = NULL;
    ctx->last_error = KL_ERR_NONE;
    ctx->timers = NULL;
    ctx->timer_count = 0;
    ctx->timer_cap = 0;
    ctx->timer_next_id = 0;
    ctx->sockets = NULL;              /* POSIX by default (internal seam) */
    ctx->comp_conn_dispatch = NULL;   /* set by the server on its completion path */
    ctx->loop.alloc = alloc;
    ctx->loop.ops = NULL;             /* set by kl_event_init[_provider] below */
    /* A runtime provider (e.g. lwIP) supplies its own event backend; NULL uses
     * the compiled-in default. Both leave the rest of the ctx identical. */
    int r = event_provider ? kl_event_init_provider(&ctx->loop, event_provider)
                           : kl_event_init(&ctx->loop);
    if (r < 0) {
        ctx->last_error = KL_ERR_EVENT_INIT;
        return -1;
    }
    return 0;
}

int kl_event_ctx_init(KlEventCtx *ctx, KlAllocator *alloc) {
    return kl_event_ctx_init_ex(ctx, alloc, NULL);
}

void kl_event_ctx_free(KlEventCtx *ctx) {
    if (!ctx) return;
    if (ctx->timers)
        kl_free(ctx->alloc, ctx->timers,
                (size_t)ctx->timer_cap * sizeof(KlTimerEntry));
    ctx->timers = NULL;
    ctx->timer_count = 0;
    ctx->timer_cap = 0;
    /* R3b-W: freeing a ctx during batch dispatch is a CALLER BUG — captured events on the live
     * dispatch stack may still reference retired nodes, so reclaiming them here would be unsafe.
     * The precondition is dispatch_depth == 0 (asserted in hosted debug builds). Defensive draining
     * of `retired` (normally already empty, drained when the outermost bracket closed) is valid ONLY
     * at depth 0; at nonzero depth we deliberately do NOT touch it. */
#ifndef KEEL_FREESTANDING
    assert(ctx->dispatch_depth == 0);
#endif
    if (ctx->dispatch_depth == 0) {
        while (ctx->retired) {
            KlWatcher *w = ctx->retired;
            ctx->retired = w->next;
            kl_free(ctx->alloc, w, sizeof(KlWatcher));
        }
    }
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
         * loop's overlapped submit path rather than synchronous send/recv. Also reject it
         * fail-loud when the completion axis was compiled out (KEEL_NO_COMPLETION): the
         * driver/dispatch are absent, so a completion loop cannot be driven. Keeps that
         * build knob out of the shared negotiation — the axis TU reports its presence. */
        if (!kl_completion_axis_available()) return 0;
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

/* Detached watcher (LC-0): the ctx-owned node without a kl_event_add. See watcher_internal.h.
 * The mask is stored (KL_EVENT_WRITE — the connect result) but no readiness watch is armed;
 * the completion connect fires the tagged pointer directly. A later kl_watcher_mod arms the
 * real readiness relay for the send/recv phase. Idempotent per fd. */
void *kl_watcher_add_detached(KlEventCtx *ctx, KlSocketHandle fd,
                              KlWatcherFn on_ready, void *user_data)
{
    if (!ctx || !kl_handle_valid(fd) || !on_ready) return NULL;

    for (KlWatcher *e = ctx->watchers; e; e = e->next) {
        if (e->fd == fd) {
            e->mask = KL_EVENT_WRITE;
            e->on_ready = on_ready;
            e->user_data = user_data;
            return watcher_tag(e);
        }
    }

    KlAllocator *a = ctx->alloc;
    KlWatcher *w = kl_malloc(a, sizeof(KlWatcher));
    if (!w) return NULL;

    w->fd = fd;
    w->mask = KL_EVENT_WRITE;
    w->on_ready = on_ready;
    w->user_data = user_data;
    w->next = ctx->watchers;
    ctx->watchers = w;
    return watcher_tag(w);
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

    const KlWatcher *w = ctx->watchers;
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
            if (ctx->dispatch_depth > 0) {
                /* R3b-W: deleted DURING a batch — defer the free so this node's address cannot be
                 * reused by a mid-batch kl_watcher_add before the batch's remaining events are
                 * dispatched (that reuse would let a stale event's pointer scan match the new
                 * watcher). Thread onto `retired` via the node's own next link; reclaimed when the
                 * outermost dispatch bracket closes. */
                w->next = ctx->retired;
                ctx->retired = w;
            } else {
                kl_free(a, w, sizeof(KlWatcher));
            }
            ctx->dispatch_dirty = 1;  /* suppress auto-rearm */
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ── batch-dispatch reclamation (R3b-W watcher pointer-reuse ABA remedy) ─── */

/* Free every watcher node deferred during a batch (chained through KlWatcher.next). */
static void kl_ctx_reclaim_retired(KlEventCtx *ctx) {
    while (ctx->retired) {
        KlWatcher *w = ctx->retired;
        ctx->retired = w->next;
        kl_free(ctx->alloc, w, sizeof(KlWatcher));
    }
}

/* Returns 0 on success (batch may be dispatched), -1 if the maximum nesting depth is reached
 * (pathological/unbalanced nesting). On -1 the caller MUST NOT dispatch the batch and MUST NOT call
 * kl_event_ctx_dispatch_end (the depth was not incremented). Guards against signed-overflow UB. */
int kl_event_ctx_dispatch_begin(KlEventCtx *ctx) {
    if (!ctx) return -1;
    if (ctx->dispatch_depth == INT_MAX) return -1;   /* refuse to overflow — do not dispatch */
    ctx->dispatch_depth++;
    return 0;
}

void kl_event_ctx_dispatch_end(KlEventCtx *ctx) {
    if (!ctx || ctx->dispatch_depth == 0) return;   /* underflow guard: an unmatched end is a no-op */
    if (--ctx->dispatch_depth == 0)                 /* reclaim only when the OUTERMOST bracket closes */
        kl_ctx_reclaim_retired(ctx);
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
    int begin_failed = 0;
    if (n > 0) {
        /* R3b-W: batch bracket (defer mid-batch watcher frees). If begin fails (max nesting depth),
         * do NOT dispatch this batch and propagate failure. */
        if (kl_event_ctx_dispatch_begin(ctx) < 0) {
            begin_failed = 1;
        } else {
            for (int i = 0; i < n; i++)
                kl_event_dispatch(ctx, &events[i]);
            kl_event_ctx_dispatch_end(ctx);
        }
    }

    if (events != stack_buf)
        kl_free(ctx->alloc, events, (size_t)max_events * sizeof(KlEvent));

    /* Fire expired timers */
    kl_timer_fire(ctx);

    if (begin_failed) return -1;
    return n < 0 ? -1 : n;
}
