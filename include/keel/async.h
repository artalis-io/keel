#ifndef KEEL_ASYNC_H
#define KEEL_ASYNC_H

#include <keel/event.h>
#include <stdint.h>

typedef struct KlServer KlServer;
typedef struct KlConn KlConn;

/* ── KlWatcher — generic FD callback ──────────────────────────────── */

/**
 * @brief Callback invoked when a watched FD becomes ready.
 * @param fd     File descriptor that fired.
 * @param ready  Bitmask of ready events (KL_EVENT_READ, KL_EVENT_WRITE).
 * @param user_data Opaque pointer passed to kl_watcher_add.
 */
typedef void (*KlWatcherFn)(int fd, KlEventMask ready, void *user_data);

/**
 * @brief A registered FD watcher (heap-allocated, server-owned list).
 */
typedef struct KlWatcher {
    int fd;
    KlWatcherFn on_ready;
    void *user_data;
    struct KlWatcher *next;   /* server's watcher list */
} KlWatcher;

/**
 * @brief Register a file descriptor with the event loop.
 *
 * When the FD becomes ready (readable/writable per mask), on_ready is
 * called on the event loop thread.  The watcher is heap-allocated and
 * owned by the server — call kl_watcher_del to remove and free.
 *
 * @return 0 on success, -1 on failure.
 */
int  kl_watcher_add(KlServer *s, int fd, KlEventMask mask,
                    KlWatcherFn on_ready, void *user_data);

/**
 * @brief Change the event interest mask for a registered watcher.
 * @return 0 on success, -1 if fd not found or event_mod fails.
 */
int  kl_watcher_mod(KlServer *s, int fd, KlEventMask mask);

/**
 * @brief Remove a watcher and deregister its FD from the event loop.
 */
void kl_watcher_del(KlServer *s, int fd);

/* ── KlAsyncOp — connection suspension ────────────────────────────── */

typedef struct KlAsyncOp KlAsyncOp;

/**
 * @brief Callback for async operation lifecycle events.
 * @param op        The async operation.
 * @param user_data Opaque pointer from op->user_data.
 */
typedef void (*KlAsyncFn)(KlAsyncOp *op, void *user_data);

/**
 * @brief An in-flight async operation that suspends a connection.
 *
 * Three separate callbacks because deadline semantics differ per operation:
 * - Sleep:  on_deadline = success (timer fired)
 * - HTTP:   on_deadline = error (timeout)
 * - Gather: on_deadline = cancel remaining sub-ops, return partial results
 */
struct KlAsyncOp {
    KlConn *conn;              /**< Suspended connection */
    uint64_t deadline_ms;      /**< Absolute deadline (0 = no deadline) */
    KlAsyncFn on_resume;       /**< Called by kl_async_complete */
    KlAsyncFn on_deadline;     /**< Called when deadline_ms reached */
    KlAsyncFn on_cancel;       /**< Called if connection dies while suspended */
    void *user_data;           /**< Opaque (e.g. HlAsyncCtx*) */
    struct KlAsyncOp *next;    /**< Active ops list (server-owned) */
};

/**
 * @brief Suspend a connection for an async operation.
 *
 * Sets conn->state to KL_CONN_SUSPENDED, removes the client FD from
 * the event loop, and adds the op to the server's active ops list.
 * The connection is exempt from idle timeouts while suspended.
 *
 * @param s    Server instance.
 * @param conn Connection to suspend.
 * @param op   Caller-owned async op (must remain valid until completion).
 * @return 0 on success, -1 on failure.
 */
int  kl_async_suspend(KlServer *s, KlConn *conn, KlAsyncOp *op);

/**
 * @brief Complete an async operation and resume the connection.
 *
 * Calls op->on_resume, re-registers the client FD in the event loop,
 * and drives the connection state machine forward (try immediate send,
 * transition to SENDING/READING/CLOSED as appropriate).
 *
 * If on_resume triggers another suspension (e.g. handler yields again),
 * the new suspension takes effect and this function is a no-op after
 * the on_resume call.
 *
 * @param s  Server instance.
 * @param op Async op to complete (removed from active list).
 */
void kl_async_complete(KlServer *s, KlAsyncOp *op);

#endif
