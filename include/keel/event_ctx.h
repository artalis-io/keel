#ifndef KEEL_EVENT_CTX_H
#define KEEL_EVENT_CTX_H

#include <keel/allocator.h>
#include <keel/error.h>
#include <keel/event.h>
#include <keel/handle.h>
#include <stdint.h>

/* ── KlWatcher — generic FD callback ──────────────────────────────── */

/**
 * @brief Callback invoked when a watched FD becomes ready.
 * @param fd     File descriptor that fired.
 * @param ready  Bitmask of ready events (KL_EVENT_READ, KL_EVENT_WRITE).
 * @param user_data Opaque pointer passed to kl_watcher_add.
 */
typedef void (*KlWatcherFn)(KlSocketHandle fd, KlEventMask ready, void *user_data);

/**
 * @brief A registered FD watcher (heap-allocated, ctx-owned list).
 */
typedef struct KlWatcher {
    KlSocketHandle fd;       /**< Watched file descriptor */
    KlEventMask mask;        /**< Event interest mask */
    KlWatcherFn on_ready;    /**< Callback when FD is ready */
    void *user_data;         /**< Opaque user pointer */
    struct KlWatcher *next;  /**< Next watcher in ctx-owned list */
} KlWatcher;

/* ── KlEventCtx — composable event loop + watcher context ────────── */

/** @brief Timer heap entry (defined in src/timer.c). */
typedef struct KlTimerEntry KlTimerEntry;

/* Internal socket provider (src/socket.h). Carried as an opaque pointer so the
 * provider vtable stays internal — this is NOT the public provider-selection
 * API (that is a later phase). NULL selects the built-in POSIX path. */
struct KlSocketProvider;

/**
 * @brief Composable event loop context.
 *
 * Contains the platform event loop, allocator, and watcher list.
 * Embedded in KlServer via composition. Can also be used standalone
 * (e.g. by KlClient, KlThreadPool) without requiring a full server.
 */
typedef struct KlEventCtx {
    KlEventLoop loop;             /**< Platform event loop */
    KlAllocator *alloc;           /**< borrowed — must outlive ctx */
    KlWatcher *watchers;          /**< Head of ctx-owned watcher list */
    int dispatch_dirty;       /**< set by kl_watcher_mod/del during callback */
    KlError last_error;       /**< diagnostic: set at point of return -1 */
    /* Timer heap */
    KlTimerEntry *timers;     /**< min-heap array (NULL until first add) */
    int timer_count;          /**< entries in heap */
    int timer_cap;            /**< allocated slots */
    int64_t timer_next_id;    /**< monotonic ID counter */
    /* Internal: socket provider for transports created on this ctx (opaque;
     * NULL = POSIX). Set by tests / future providers, not public API. */
    const struct KlSocketProvider *sockets;
} KlEventCtx;

/**
 * @brief Initialize an event context with the given allocator.
 * @param ctx   Event context to initialize.
 * @param alloc Allocator (borrowed — must outlive ctx).
 * @return 0 on success, -1 on failure.
 */
int  kl_event_ctx_init(KlEventCtx *ctx, KlAllocator *alloc);

/**
 * @brief Initialize an event context on a runtime event backend.
 *
 * Like kl_event_ctx_init, but installs @p event_provider (e.g. lwIP) as the
 * loop's backend instead of the compiled-in default. A NULL provider is
 * identical to kl_event_ctx_init. The provider must be paired with a compatible
 * socket provider (KlEventCtx.sockets / KlConfig.sockets) that it can poll.
 *
 * @param ctx            Event context to initialize.
 * @param alloc          Allocator (borrowed — must outlive ctx).
 * @param event_provider Runtime event backend, or NULL for the default.
 * @return 0 on success, -1 on failure.
 */
int  kl_event_ctx_init_ex(KlEventCtx *ctx, KlAllocator *alloc,
                          const KlEventProvider *event_provider);

/**
 * @brief Free all watchers and close the event loop.
 */
void kl_event_ctx_free(KlEventCtx *ctx);

/* ── Watcher API (operates on KlEventCtx, not KlServer) ──────────── */

/**
 * @brief Register a file descriptor with the event loop.
 *
 * When the FD becomes ready (readable/writable per mask), on_ready is
 * called on the event loop thread.  The watcher is heap-allocated and
 * owned by the context — call kl_watcher_del to remove and free.
 *
 * @return 0 on success, -1 on failure.
 */
int  kl_watcher_add(KlEventCtx *ctx, KlSocketHandle fd, KlEventMask mask,
                    KlWatcherFn on_ready, void *user_data);

/**
 * @brief Change the event interest mask for a registered watcher.
 * @return 0 on success, -1 if fd not found or event_mod fails.
 */
int  kl_watcher_mod(KlEventCtx *ctx, KlSocketHandle fd, KlEventMask mask);

/**
 * @brief Remove a watcher and deregister its FD from the event loop.
 */
void kl_watcher_del(KlEventCtx *ctx, KlSocketHandle fd);

/**
 * @brief Re-arm a watcher after its callback fires.
 *
 * Required for one-shot backends (io_uring POLL_ADD).  Safe no-op if
 * the watcher was removed during the callback.  On persistent backends
 * (epoll, kqueue) this is a harmless re-register.
 */
int  kl_watcher_rearm(KlEventCtx *ctx, KlSocketHandle fd);

/* ── Dispatch helpers ─────────────────────────────────────────────── */

/**
 * @brief Dispatch a single event if it is a watcher (tagged pointer).
 *
 * Checks the LSB tag on event->udata.  If set, unmasks the KlWatcher*,
 * calls on_ready, re-arms for one-shot backends, and returns 1.
 * If the tag is clear (connection, listen socket, etc.) returns 0 —
 * the caller handles it.
 *
 * @return 1 if a watcher was dispatched, 0 otherwise.
 */
static inline int kl_event_dispatch(KlEventCtx *ctx, const KlEvent *event) {
    uintptr_t tag = (uintptr_t)event->udata;
    if (!(tag & 1))
        return 0;  /* not a watcher — caller handles */
    KlWatcher *w = (KlWatcher *)(tag & ~(uintptr_t)1);
    KlSocketHandle wfd = w->fd;   /* full handle width: intptr_t, NOT int — a completion backend
                                   * whose handle is a pointer (lwip-raw: a tcp_pcb*) would be
                                   * truncated + sign-extended by an int, breaking watcher rearm. */
    ctx->dispatch_dirty = 0;
    w->on_ready(wfd, event->ready, w->user_data);
    // cppcheck-suppress knownConditionTrueFalse
    if (!ctx->dispatch_dirty)
        kl_watcher_rearm(ctx, wfd);
    return 1;
}

/**
 * @brief Run one tick of the event loop, dispatching all watcher events.
 *
 * Calls kl_event_wait, then kl_event_dispatch for each returned event.
 * Non-watcher events are silently skipped — this is intended for
 * standalone KlEventCtx usage (clients, thread pools) where all FDs
 * are watcher-owned.
 *
 * Uses a stack buffer for up to 64 events; heap-allocates via ctx->alloc
 * for larger requests.
 *
 * @param ctx        Event context (loop + allocator + watchers).
 * @param max_events Maximum events to process per tick.
 * @param timeout_ms Timeout in milliseconds (-1 for infinite).
 * @return Number of events returned by kl_event_wait, or -1 on error.
 */
int kl_event_ctx_run(KlEventCtx *ctx, int max_events, int timeout_ms);

#endif
