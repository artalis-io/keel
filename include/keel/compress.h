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
 * zero-initialize and recompile per major version. Required (core calls them):
 * compress, feed, encoding, destroy. Optional (NULL = not supplied): reset. New
 * ops are appended after destroy.
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
     * @param flush    0 for intermediate chunks, 1 for final flush + trailer.
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
};

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
