#ifndef KEEL_DRAIN_H
#define KEEL_DRAIN_H

#include <keel/allocator.h>
#include <keel/handle.h>   /* kl_ssize_t */
#include <stddef.h>

/**
 * @brief Write function for KlDrain.
 *
 * @param data Data to write.
 * @param len  Data length.
 * @param ctx  Writer context.
 * @return >0 bytes written, 0 would-block, -1 error.
 */
typedef kl_ssize_t (*KlDrainWriteFn)(const char *data, size_t len, void *ctx);

/**
 * @brief Drain callback — fired when buffer transitions non-empty → empty.
 */
typedef void (*KlDrainCb)(void *ctx);

/**
 * @brief Backpressure write buffer.
 *
 * Sits between a producer (SSE, compress stream, WebSocket, etc.) and a
 * writer function. When the writer returns would-block (0), KlDrain buffers
 * the remaining data. The caller composes KlDrain with KlWatcher / KlAsyncOp
 * to resume on write-readiness — KlDrain itself has zero event loop dependency.
 *
 * Caller-owned struct (stack-allocatable). NOT thread-safe.
 */
typedef struct {
    KlDrainWriteFn  write_fn;    /**< Underlying writer */
    void           *write_ctx;   /**< Writer context */
    KlDrainCb       on_drain;    /**< Optional: buffer-drained callback */
    void           *drain_ctx;   /**< Callback context */
    KlAllocator    *alloc;       /**< For buffer allocation */
    char           *buf;         /**< Pending data (lazy-allocated) */
    size_t          buf_len;     /**< Bytes pending */
    size_t          buf_cap;     /**< Buffer capacity */
    size_t          max_size;    /**< 0 = unlimited, else hard cap */
    int             error;       /**< Sticky error flag */
} KlDrain;

/**
 * @brief Initialize a drain buffer.
 *
 * Buffer is lazy-allocated on first would-block.
 *
 * @param d        Drain handle (caller-owned).
 * @param write_fn Underlying writer.
 * @param write_ctx Writer context.
 * @param alloc    Allocator for buffer.
 */
void kl_drain_init(KlDrain *d, KlDrainWriteFn write_fn, void *write_ctx,
                   KlAllocator *alloc);

/**
 * @brief Set maximum buffer size.
 *
 * @param d        Drain handle.
 * @param max_size Hard cap on buffered bytes (0 = unlimited).
 */
void kl_drain_set_max_size(KlDrain *d, size_t max_size);

/**
 * @brief Register a drain callback.
 *
 * Fires when buffer transitions from non-empty to empty.
 *
 * @param d   Drain handle.
 * @param cb  Callback (NULL to clear).
 * @param ctx Callback context.
 */
void kl_drain_on_drain(KlDrain *d, KlDrainCb cb, void *ctx);

/**
 * @brief Write data through the drain.
 *
 * If buffer is non-empty, appends (preserves ordering). Otherwise writes
 * directly; buffers only the unaccepted remainder on partial/EAGAIN.
 *
 * @param d    Drain handle.
 * @param data Data to write.
 * @param len  Data length.
 * @return 0 on success (all written or buffered), -1 on error.
 */
int kl_drain_write(KlDrain *d, const char *data, size_t len);

/**
 * @brief Flush buffered data.
 *
 * Loops write_fn on buffered data. Fires on_drain when buffer empties.
 *
 * @param d Drain handle.
 * @return 0=drained, 1=more pending, -1=error.
 */
int kl_drain_flush(KlDrain *d);

/**
 * @brief Check if data is pending.
 *
 * @param d Drain handle.
 * @return 1 if data pending, 0 if empty.
 */
int kl_drain_pending(const KlDrain *d);

/**
 * @brief Get number of buffered bytes.
 *
 * @param d Drain handle.
 * @return Bytes pending in buffer.
 */
size_t kl_drain_buffered(const KlDrain *d);

/**
 * @brief Peek at the pending bytes (no copy).
 *
 * Returns a pointer to the contiguous buffered bytes (length = kl_drain_buffered()).
 * Valid until the next kl_drain_write/flush/consume/free. Used by a transport that
 * flushes the buffer itself (e.g. the completion loop posting an overlapped send) rather
 * than via the write_fn. Returns NULL if empty.
 *
 * @param d Drain handle.
 * @return Pointer to pending bytes, or NULL.
 */
const char *kl_drain_data(const KlDrain *d);

/**
 * @brief Drop @p n bytes from the front of the buffer (they were sent by other means).
 *
 * The counterpart to kl_drain_data(): after a transport flushes some/all pending bytes
 * itself, it consumes them here. Clamped to the buffered count; fires the on_drain
 * callback if the buffer becomes empty (parity with kl_drain_flush).
 *
 * @param d Drain handle.
 * @param n Bytes to drop from the front.
 */
void kl_drain_consume(KlDrain *d, size_t n);

/**
 * @brief Free drain buffer.
 *
 * Frees the internal buffer. Does not free the drain struct itself
 * (caller-owned). Safe to call on an already-freed drain.
 *
 * @param d Drain handle.
 */
void kl_drain_free(KlDrain *d);

#endif /* KEEL_DRAIN_H */
