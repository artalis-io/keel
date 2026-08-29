/*
 * event_ctx_internal.h: INTERNAL substrate-private layouts for the event context.
 *
 * Holds the concrete definitions of KlWatcher and KlTimerEntry, which are opaque handles on the
 * public surface (forward-declared in <keel/event_ctx.h>). Include this ONLY from substrate
 * implementation TUs that manage the watcher list or timer heap (src/event_ctx.c, src/timer.c) and
 * from explicitly-justified white-box tests. No public header, protocol TU, or integration TU needs
 * these layouts: a watcher travels as a tagged pointer (LSB=1) resolved inside the substrate, and a
 * timer is referenced by its int64 id.
 */
#ifndef KEEL_SRC_EVENT_CTX_INTERNAL_H
#define KEEL_SRC_EVENT_CTX_INTERNAL_H

#include <keel/event_ctx.h>   /* KlWatcher/KlTimerEntry forward decls, KlWatcherFn, KlSocketHandle, KlEventMask */
#include <keel/timer.h>       /* KlTimerFn */
#include <stdint.h>

/* A registered FD watcher: heap-allocated, ctx-owned, singly linked. */
struct KlWatcher {
    KlSocketHandle fd;       /* Watched file descriptor */
    KlEventMask mask;        /* Event interest mask */
    KlWatcherFn on_ready;    /* Callback when FD is ready */
    void *user_data;         /* Opaque user pointer */
    struct KlWatcher *next;  /* Next watcher in the ctx-owned list */
};

/* Timer heap entry: stored in the KlEventCtx.timers min-heap array. */
struct KlTimerEntry {
    uint64_t  deadline_ms;   /* Absolute monotonic deadline. */
    KlTimerFn cb;            /* Callback invoked on expiry. */
    void     *user_data;     /* Opaque pointer passed to cb. */
    int64_t   id;            /* Monotonic timer ID. */
};

#endif /* KEEL_SRC_EVENT_CTX_INTERNAL_H */
