#ifndef KEEL_DECOMPRESS_H
#define KEEL_DECOMPRESS_H

#include <keel/allocator.h>
#include <keel/compress.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Pluggable decompression vtable.
 *
 * Users implement this interface to provide decompression (gzip, deflate, zstd).
 * One KlDecompress instance is created per decompress operation via the factory.
 *
 * Append-only vtable (see docs/contracts/compatibility.md): implementers
 * zero-initialize and recompile per major version. Required (core calls them):
 * decompress, dfeed, encoding, destroy. Optional (NULL = not supplied): reset,
 * dfeed_stoppable. New ops are appended after dfeed_stoppable.
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

    /**
     * @brief Optional streaming feed that the consumer can stop (NULL = not supplied).
     *
     * Same input and output as dfeed, except that @p emit may return 1 to ask the decoder to stop.
     * The fragment passed to that emit call is accepted (it is never offered again). The decoder
     * then returns 1 promptly, without calling @p emit again, after storing in @p *consumed how many
     * bytes of @p data it took. Output it has decoded but not emitted, and its own state, stay
     * inside the session. The next call continues exactly there, with the rest of the input
     * (`data + *consumed`, `len - *consumed`), which may be zero bytes: a decoder that still holds
     * output must make progress on a call with no input. A stop during the final call
     * (flush = 1) is continued by another flush call; the trailer is validated only when that
     * finishes.
     *
     * An emit return below 0 fails the stream, as with dfeed; 0 continues.
     *
     * The HTTP client uses this op, when supplied, to pause a decompressed response body without
     * holding decoded output (KL_HTTP_CLIENT_BODY_PAUSE). Without it the client falls back to dfeed
     * in bounded input slices and holds the rest of a slice's output, up to a fixed cap.
     *
     * @param consumed Receives the number of input bytes taken (on a return of 0 or 1).
     * @return 0 when all of @p data was taken and no output is held (for flush = 1: the stream
     *         ended and was validated); 1 when stopped by @p emit; -1 on error.
     */
    int (*dfeed_stoppable)(KlDecompress *self, const char *data, size_t len, int flush,
                           size_t *consumed,
                           int (*emit)(void *ctx, const char *data, size_t len),
                           void *emit_ctx);
};

/**
 * @brief Factory creates a KlDecompress session from the shared context.
 *
 * Shares KlCompressCtx with compression: same algorithm configuration.
 *
 * The factory signature is frozen (see docs/contracts/compatibility.md): pass new
 * construction inputs through KlCompressCtx or KlDecompressConfig, never by
 * changing this signature.
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
 *
 * Append-only config (see docs/contracts/compatibility.md): callers
 * zero-initialize and recompile per major version; every member is optional and
 * its zero/NULL value selects the built-in default (a NULL factory leaves
 * decompression disabled). New members are appended after ctx_destroy.
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

#ifdef __cplusplus
}
#endif

#endif /* KEEL_DECOMPRESS_H */
