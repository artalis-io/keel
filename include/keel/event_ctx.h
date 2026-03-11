#ifndef KEEL_EVENT_CTX_H
#define KEEL_EVENT_CTX_H

#include <keel/allocator.h>
#include <keel/event.h>

/* ── KlWatcher — generic FD callback ──────────────────────────────── */

/**
 * @brief Callback invoked when a watched FD becomes ready.
 * @param fd     File descriptor that fired.
 * @param ready  Bitmask of ready events (KL_EVENT_READ, KL_EVENT_WRITE).
 * @param user_data Opaque pointer passed to kl_watcher_add.
 */
typedef void (*KlWatcherFn)(int fd, KlEventMask ready, void *user_data);

/**
 * @brief A registered FD watcher (heap-allocated, ctx-owned list).
 */
typedef struct KlWatcher {
    int fd;
    KlEventMask mask;
    KlWatcherFn on_ready;
    void *user_data;
    struct KlWatcher *next;
} KlWatcher;

/* ── KlEventCtx — composable event loop + watcher context ────────── */

/**
 * @brief Composable event loop context.
 *
 * Contains the platform event loop, allocator, and watcher list.
 * Embedded in KlServer via composition. Can also be used standalone
 * (e.g. by KlClient, KlThreadPool) without requiring a full server.
 */
typedef struct KlEventCtx {
    KlEventLoop loop;
    KlAllocator *alloc;       /* borrowed — must outlive ctx */
    KlWatcher *watchers;
} KlEventCtx;

/**
 * @brief Initialize an event context with the given allocator.
 * @param ctx   Event context to initialize.
 * @param alloc Allocator (borrowed — must outlive ctx).
 * @return 0 on success, -1 on failure.
 */
int  kl_event_ctx_init(KlEventCtx *ctx, KlAllocator *alloc);

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
int  kl_watcher_add(KlEventCtx *ctx, int fd, KlEventMask mask,
                    KlWatcherFn on_ready, void *user_data);

/**
 * @brief Change the event interest mask for a registered watcher.
 * @return 0 on success, -1 if fd not found or event_mod fails.
 */
int  kl_watcher_mod(KlEventCtx *ctx, int fd, KlEventMask mask);

/**
 * @brief Remove a watcher and deregister its FD from the event loop.
 */
void kl_watcher_del(KlEventCtx *ctx, int fd);

/**
 * @brief Re-arm a watcher after its callback fires.
 *
 * Required for one-shot backends (io_uring POLL_ADD).  Safe no-op if
 * the watcher was removed during the callback.  On persistent backends
 * (epoll, kqueue) this is a harmless re-register.
 */
void kl_watcher_rearm(KlEventCtx *ctx, int fd);

#endif
