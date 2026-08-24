#ifndef KEEL_DECOMPRESS_H
#define KEEL_DECOMPRESS_H

#include <keel/allocator.h>
#include <keel/compress.h>
#include <stddef.h>

/**
 * @brief Pluggable decompression vtable.
 *
 * Users implement this interface to provide decompression (gzip, deflate, zstd).
 * One KlDecompress instance is created per decompress operation via the factory.
 */
typedef struct KlDecompress KlDecompress;

struct KlDecompress {
    /**
     * @brief Single-shot: decompress entire buffer into *out.
     *
     * Allocates output via alloc. Caller owns *out and must
     * kl_free(alloc, *out, *out_len).
     *
     * @param self    Decompression session.
     * @param in      Compressed input data.
     * @param in_len  Input length.
     * @param out     Receives allocated decompressed output.
     * @param out_len Receives decompressed output length.
     * @param alloc   Allocator for output buffer.
     * @return 0 on success, -1 on error.
     */
    int (*decompress)(KlDecompress *self, const char *in, size_t in_len,
                      char **out, size_t *out_len, KlAllocator *alloc);

    /**
     * @brief Streaming: feed compressed input, emit decompressed output.
     *
     * @param self     Decompression session.
     * @param data     Compressed input data (NULL when flush=1 for final).
     * @param len      Input length (0 when flush=1 for final).
     * @param flush    0 for intermediate chunks, 1 for final (validate trailer).
     * @param emit     Callback to receive decompressed output chunks.
     * @param emit_ctx Context passed to emit callback.
     * @return 0 on success, -1 on error.
     */
    int (*dfeed)(KlDecompress *self, const char *data, size_t len, int flush,
                 int (*emit)(void *ctx, const char *data, size_t len),
                 void *emit_ctx);

    /**
     * @brief Content-Encoding value this decompressor handles, e.g. "gzip".
     * @return Static string, never NULL.
     */
    const char *(*encoding)(KlDecompress *self);

    /** @brief Reset for reuse (may be NULL if not reusable). */
    void (*reset)(KlDecompress *self);

    /** @brief Free all resources. */
    void (*destroy)(KlDecompress *self);
};

/**
 * @brief Factory creates a KlDecompress session from the shared context.
 *
 * Shares KlCompressCtx with compression: same algorithm configuration.
 *
 * @param ctx   Shared compression context (algorithm, level).
 * @param alloc Allocator for session resources.
 * @return New decompression session, or NULL on failure.
 */
typedef KlDecompress *(*KlDecompressFactory)(KlCompressCtx *ctx,
                                              KlAllocator *alloc);

/**
 * @brief Decompression configuration.
 *
 * Shares KlCompressCtx with KlCompressConfig for algorithm configuration.
 */
typedef struct KlDecompressConfig {
    KlCompressCtx       *ctx;         /**< Shared context: user-owned */
    KlDecompressFactory  factory;     /**< Creates per-operation KlDecompress */
    void (*ctx_destroy)(KlCompressCtx *ctx);  /**< Optional cleanup */
} KlDecompressConfig;

/**
 * @brief Decompression stream handle.
 *
 * Wraps a decompression session for streaming use.
 * Caller-owned struct initialized by kl_decompress_stream_init.
 */
typedef struct {
    KlDecompress *decomp;     /**< Decompression session (owned) */
    KlAllocator  *alloc;      /**< For destroying decomp */
    int           error;      /**< Sticky error flag */
} KlDecompressStream;

/**
 * @brief Decompress a buffer body in one shot.
 *
 * Creates a decompression session, decompresses data, destroys session.
 * Caller owns *out and must kl_free(alloc, *out, *out_len).
 *
 * @param cfg     Decompression config (factory + context).
 * @param in      Compressed input data.
 * @param in_len  Input length.
 * @param out     Receives allocated decompressed output.
 * @param out_len Receives decompressed output length.
 * @param alloc   Allocator for output buffer.
 * @return 0 on success, -1 on error.
 */
int kl_decompress_body(KlDecompressConfig *cfg, const char *in, size_t in_len,
                        char **out, size_t *out_len, KlAllocator *alloc);

/**
 * @brief Initialize a decompression stream.
 *
 * Creates a decompression session via the factory and stores it in the
 * stream handle.
 *
 * @param ds    Decompression stream handle (caller-owned).
 * @param cfg   Decompression config.
 * @param alloc Allocator for session resources.
 * @return 0 on success, -1 on error.
 */
int kl_decompress_stream_init(KlDecompressStream *ds, KlDecompressConfig *cfg,
                               KlAllocator *alloc);

/**
 * @brief Feed compressed data through the decompression stream.
 *
 * @param ds       Decompression stream handle.
 * @param data     Compressed data.
 * @param len      Data length.
 * @param flush    0 for intermediate, 1 for final.
 * @param emit     Callback to receive decompressed output.
 * @param emit_ctx Context passed to emit callback.
 * @return 0 on success, -1 on error.
 */
int kl_decompress_stream_feed(KlDecompressStream *ds,
                               const char *data, size_t len, int flush,
                               int (*emit)(void *ctx, const char *data,
                                           size_t len),
                               void *emit_ctx);

/**
 * @brief Free decompression stream resources.
 *
 * Destroys the decompression session. Safe to call multiple times.
 *
 * @param ds  Decompression stream handle.
 */
void kl_decompress_stream_free(KlDecompressStream *ds);

#endif /* KEEL_DECOMPRESS_H */
