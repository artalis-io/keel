#ifndef KEEL_ASYNC_H
#define KEEL_ASYNC_H

#include <keel/event_ctx.h>
#include <stdint.h>

typedef struct KlHttpServer KlHttpServer;
typedef struct KlHttpConn KlHttpConn;

/* ── KlAsyncOp: connection suspension ────────────────────────────── */

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
 *
 * **Exactly-one-terminal contract.** An op is *pending* from kl_async_suspend()
 * until exactly one terminal transition retires it:
 *   - resume:  kl_async_complete() (fires on_resume), or
 *   - cancel:  kl_async_cancel()   (fires on_cancel).
 * on_deadline is a *trigger*, not a terminal: the deadline callback must resolve
 * the op by calling kl_async_complete() (deadline-as-success, e.g. sleep) or
 * kl_async_cancel() (deadline-as-failure, e.g. HTTP timeout). It fires at most
 * once. All three entry points are idempotent (a second call on an
 * already-retired op is a no-op), so a cancel racing a completion (or a double
 * completion) can never double-fire a callback, double-release, or use-after-free.
 */
struct KlAsyncOp {
    KlHttpConn *conn;              /**< Suspended connection */
    uint64_t deadline_ms;      /**< Absolute deadline (0 = no deadline) */
    KlAsyncFn on_resume;       /**< Called by kl_async_complete */
    KlAsyncFn on_deadline;     /**< Called when deadline_ms reached */
    KlAsyncFn on_cancel;       /**< Called if connection dies while suspended */
    void *user_data;           /**< Opaque (e.g. HlAsyncCtx*) */
    struct KlAsyncOp *next;    /**< Active ops list (server-owned) */
    int _terminal;             /**< Internal: 1 once retired (do not set). */
};

/**
 * @brief Suspend a connection for an async operation.
 *
 * Sets conn->state to KL_HTTP_CONN_SUSPENDED, removes the client FD from
 * the event loop, and adds the op to the server's active ops list.
 * The connection is exempt from idle timeouts while suspended.
 *
 * @param s    Server instance.
 * @param conn Connection to suspend.
 * @param op   Caller-owned async op (must remain valid until completion).
 * @return 0 on success, -1 on failure.
 */
int  kl_async_suspend(KlHttpServer *s, KlHttpConn *conn, KlAsyncOp *op);

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
void kl_async_complete(KlHttpServer *s, KlAsyncOp *op);

/**
 * @brief Cancel an async operation without resuming the connection.
 *
 * The abnormal-termination terminal: fires op->on_cancel (so the caller can free
 * its async context), removes the op from the active list, and clears the
 * connection's async_op. Does NOT re-arm the fd or drive the state machine; the
 * caller is expected to be tearing the connection down. Idempotent: a no-op if
 * the op was already retired by kl_async_complete() or a prior cancel.
 *
 * Use for deadline-as-failure (HTTP timeout) and connection-death paths.
 *
 * @param s  Server instance.
 * @param op Async op to cancel.
 */
void kl_async_cancel(KlHttpServer *s, KlAsyncOp *op);

#endif
