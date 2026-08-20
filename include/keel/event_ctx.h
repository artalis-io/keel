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
    /* Internal completion-dispatch hook (opaque; set by the server when its features are used,
     * NULL otherwise). kl_comp_run routes the non-generic connection completion kinds through it
     * so a client-only build links neither the server nor the datagram stack. The event arg is a
     * const KlCompletionEvent* (opaque here, exactly as KlEventOps.completion). Additive — appended
     * after `sockets`, do not reorder. */
    void (*comp_conn_dispatch)(struct KlEventCtx *ctx, const void *ev);  /* ACCEPT/READ/WRITE → server */
    /* R3b-W (watcher pointer-reuse ABA remedy). `dispatch_depth` counts nested
     * kl_event_ctx_dispatch_begin/end brackets; `retired` chains watcher nodes deleted DURING a
     * bracketed batch (threaded through KlWatcher.next), so a freed node's address cannot be reused
     * by a mid-batch kl_watcher_add before the batch's remaining events are dispatched. Reclaimed
     * when the outermost bracket closes (depth → 0). Appended fields: an ADDITIVE pre-release layout
     * change — consumers of KlEventCtx must recompile. */
    int         dispatch_depth;
    KlWatcher  *retired;
    /* 7B-2a removed the datagram completion hook `comp_udp_dispatch` that sat here: datagram completions
     * (DGRAM_RECV/DGRAM_SEND) are now routed by each token's own KlDgramDispatchFn (life->dispatch),
     * not a ctx-global hook. This is an INTENTIONAL pre-release ABI break — the datagram API was
     * reshaped in this branch and no released consumer holds this slot — so the field is dropped
     * outright rather than kept as dead reserved clutter. */
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
 * BATCH CONTRACT (watcher ABA safety, R3b-W). A single kl_event_dispatch call — one event, not part
 * of a drained batch — is always safe: a watcher deleted in its callback is freed immediately (no
 * sibling event can reference it). A caller that drains MULTIPLE events (kl_event_wait /
 * kl_comp_drain) and dispatches them one-by-one MUST bracket the dispatch loop with
 * kl_event_ctx_dispatch_begin/end. The bracket defers freeing of watchers deleted mid-batch until
 * the batch ends, so a deleted node's address cannot be reused by a mid-batch kl_watcher_add and
 * then matched by a later stale event's pointer scan (which would misdeliver it to the new watcher).
 * Keel's own loops (kl_event_ctx_run, kl_comp_run, the server readiness loop) do this internally;
 * external custom batch dispatchers are responsible for the brackets.
 *
 * @return 1 if a watcher was dispatched, 0 otherwise.
 */
static inline int kl_event_dispatch(KlEventCtx *ctx, const KlEvent *event) {
    uintptr_t tag = (uintptr_t)event->udata;
    if (!(tag & 1))
        return 0;  /* not a watcher — caller handles */
    KlWatcher *w = (KlWatcher *)(tag & ~(uintptr_t)1);
    /* A prior event in the SAME drain batch may already have freed this watcher.
     * kl_comp_run (and kl_event_ctx_run) drain a batch of events, then dispatch
     * them one by one; dispatching event i can retire watcher j>i. The canonical
     * case is Happy-Eyeballs: the winning connect's KL_COMP_CONNECT runs
     * he_close_attempts → kl_watcher_del, freeing the losing attempts' nodes,
     * whose own KL_COMP_CONNECT may still sit later in this batch. Confirm the
     * node is still linked before touching it — we compare pointer identity
     * against the live list and NEVER dereference a possibly-freed node. */
    int w_live = 0;
    for (const KlWatcher *it = ctx->watchers; it; it = it->next)
        if (it == w) { w_live = 1; break; }
    if (!w_live)
        return 1;  /* watcher retired earlier in this batch — stale event, consumed */
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
 * @brief Open/close a batch-dispatch bracket (watcher ABA remedy, R3b-W).
 *
 * Bracket a loop that drains and dispatches multiple events. `begin` increments the nesting depth;
 * `end` decrements it and, when the OUTERMOST bracket closes (depth returns to 0), reclaims every
 * watcher node deleted during the batch. Nesting (a callback that runs its own tick) is supported —
 * reclamation happens only at depth 0. `end` guards against underflow (an unmatched end is a no-op).
 * A single unbatched kl_event_dispatch needs no bracket. See the kl_event_dispatch batch contract.
 *
 * `begin` RETURN CONTRACT: 0 on success (the batch may be dispatched); -1 if the maximum nesting
 * depth is reached (pathological/unbalanced nesting). On -1 the caller MUST NOT dispatch the batch
 * and MUST NOT call `end` — the depth was not incremented. This makes the increment overflow-safe.
 */
int  kl_event_ctx_dispatch_begin(KlEventCtx *ctx);
void kl_event_ctx_dispatch_end(KlEventCtx *ctx);

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
