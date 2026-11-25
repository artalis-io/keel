#ifndef KEEL_COMPRESS_H
#define KEEL_COMPRESS_H

#include <keel/allocator.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Pluggable compression vtable.
 *
 * Users implement this interface to provide compression (gzip, deflate, zstd).
 * One KlCompress instance is created per compress operation via the factory,
 * not pre-allocated per connection (deflate state is ~300KB).
 *
 * Append-only vtable (see docs/contracts/compatibility.md): implementers
 * zero-initialize and recompile against the header they build with (an appended
 * op grows the struct, so an object compiled against an older header must be
 * rebuilt even if it supplies no new op). Required (core calls them):
 * compress, feed, encoding, destroy. Optional (NULL = not supplied): reset,
 * flush_bound. New ops are appended after flush_bound.
 */
typedef struct KlCompress KlCompress;

struct KlCompress {
    /**
     * @brief Single-shot: compress entire buffer into *out.
     *
     * Allocates output via alloc. Caller owns *out and must
     * kl_free(alloc, *out, *out_len).
     *
     * @param self    Compression session.
     * @param in      Input data.
     * @param in_len  Input length.
     * @param out     Receives allocated compressed output.
     * @param out_len Receives compressed output length.
     * @param alloc   Allocator for output buffer.
     * @return 0 on success, -1 on error.
     */
    int (*compress)(KlCompress *self, const char *in, size_t in_len,
                    char **out, size_t *out_len, KlAllocator *alloc);

    /**
     * @brief Streaming: feed input, emit compressed output via callback.
     *
     * @param self     Compression session.
     * @param data     Input data (NULL when flush=1 for final).
     * @param len      Input length (0 when flush=1 for final).
     * @param flush    0 for intermediate chunks, 1 for final flush + trailer,
     *                 KL_COMPRESS_FLUSH_SYNC to flush: see flush_bound, the only
     *                 codecs ever passed it.
     * @param emit     Callback to receive compressed output chunks.
     * @param emit_ctx Context passed to emit callback.
     * @return 0 on success, -1 on error.
     */
    int (*feed)(KlCompress *self, const char *data, size_t len, int flush,
                int (*emit)(void *ctx, const char *data, size_t len),
                void *emit_ctx);

    /**
     * @brief Content-Encoding value, e.g. "gzip", "deflate", "zstd".
     * @return Static string, never NULL.
     */
    const char *(*encoding)(KlCompress *self);

    /** @brief Reset for reuse (may be NULL if not reusable). */
    void (*reset)(KlCompress *self);

    /** @brief Free all resources. */
    void (*destroy)(KlCompress *self);
    /**
     * @brief Optional: the most output one feed() can emit now (bounded, flushing streams).
     *
     * Supplying this op declares that feed() also accepts flush == KL_COMPRESS_FLUSH_SYNC: consume
     * all of the input and emit everything needed to decode all input fed so far, leaving the
     * stream open, so the codec retains no output for later. It is what a bounded compressed
     * stream admits against (kl_http_compress_stream_try_write); a codec without it cannot be used
     * for one, and nothing else changes.
     *
     * Returns an upper bound on the bytes that feed(self, data, len, flush) would emit, with flush
     * KL_COMPRESS_FLUSH_SYNC when @p final is 0 and 1 when it is 1, given the session's current
     * state: any header not yet emitted, the output still owed for input an earlier non-flushing
     * feed() retained, the flush or finish overhead and, when final, the trailer. It must not
     * modify the session, must not depend on the bytes of the input (only on @p len), and returns
     * SIZE_MAX when the bound does not fit a size_t. It knows nothing of where the output goes.
     *
     * A feed() that would emit more than this bound is a codec defect: a bounded stream detects it
     * and fails (the stream ends in error) rather than exceed its capacity.
     */
    size_t (*flush_bound)(KlCompress *self, size_t len, int final);
};

/** feed() flush mode: flush everything so far, keep the stream open (codecs with flush_bound). */
#define KL_COMPRESS_FLUSH_SYNC 2

/**
 * @brief Opaque per-server compression context (algorithm config, level).
 * User-owned: KEEL never inspects or modifies this.
 */
typedef struct KlCompressCtx KlCompressCtx;

/**
 * @brief Factory creates a KlCompress session from the shared context.
 *
 * The factory signature is frozen (see docs/contracts/compatibility.md): pass new
 * construction inputs through KlCompressCtx or KlCompressConfig, never by changing
 * this signature.
 *
 * @param ctx   Shared compression context (algorithm, level).
 * @param alloc Allocator for session resources.
 * @return New compression session, or NULL on failure.
 */
typedef KlCompress *(*KlCompressFactory)(KlCompressCtx *ctx,
                                          KlAllocator *alloc);

/**
 * @brief Compression configuration (shared context + per-operation factory).
 *
 * Append-only config (see docs/contracts/compatibility.md): callers
 * zero-initialize and recompile per major version; every member is optional and
 * its zero/NULL value selects the built-in default (a NULL factory leaves
 * compression disabled). New members are appended after ctx_destroy.
 */
typedef struct KlCompressConfig {
    KlCompressCtx     *ctx;         /**< Shared context: user-owned */
    KlCompressFactory  factory;     /**< Creates per-operation KlCompress */
    void (*ctx_destroy)(KlCompressCtx *ctx);  /**< Optional cleanup */
} KlCompressConfig;

#ifdef __cplusplus
}
#endif

#endif /* KEEL_COMPRESS_H */
