#ifndef KEEL_COMPRESS_H
#define KEEL_COMPRESS_H

#include <keel/allocator.h>
#include <keel/http_response.h>
#include <stddef.h>

/**
 * @brief Pluggable compression vtable.
 *
 * Users implement this interface to provide compression (gzip, deflate, zstd).
 * One KlCompress instance is created per compress operation via the factory —
 * not pre-allocated per connection (deflate state is ~300KB).
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
 * User-owned — KEEL never inspects or modifies this.
 */
typedef struct KlCompressCtx KlCompressCtx;

/**
 * @brief Factory creates a KlCompress session from the shared context.
 * @param ctx   Shared compression context (algorithm, level).
 * @param alloc Allocator for session resources.
 * @return New compression session, or NULL on failure.
 */
typedef KlCompress *(*KlCompressFactory)(KlCompressCtx *ctx,
                                          KlAllocator *alloc);

/**
 * @brief Compression configuration for KlHttpServerConfig.
 */
typedef struct KlCompressConfig {
    KlCompressCtx     *ctx;         /**< Shared context — user-owned */
    KlCompressFactory  factory;     /**< Creates per-operation KlCompress */
    void (*ctx_destroy)(KlCompressCtx *ctx);  /**< Optional cleanup */
} KlCompressConfig;

/**
 * @brief Compressed streaming handle.
 *
 * Wraps a chunked streaming response with compression.
 * Like KlHttpSse, this is a caller-owned struct initialized by
 * kl_http_compress_stream_begin.
 */
typedef struct {
    KlCompress  *comp;       /**< Compression session (owned) */
    KlHttpResponseWriteFn    write_fn;   /**< Underlying chunked stream write */
    void        *write_ctx;  /**< Underlying chunked stream context */
    KlHttpResponse  *res;        /**< Response (for end_stream) */
    KlAllocator *alloc;      /**< For destroying comp */
    int          error;      /**< Sticky error flag */
} KlHttpCompressStream;

/**
 * @brief Compress a buffer body and set it on the response.
 *
 * Creates a compression session, compresses data, sets Content-Encoding
 * and Vary: Accept-Encoding headers. If compression expands the data,
 * falls back to uncompressed (no Content-Encoding header).
 *
 * @param res  Response to set body on.
 * @param cfg  Compression config (factory + context).
 * @param data Body data to compress.
 * @param len  Body data length.
 * @return 0 on success, -1 on error.
 */
int kl_http_response_body_compress(KlHttpResponse *res, KlCompressConfig *cfg,
                               const char *data, size_t len);

/**
 * @brief Begin a compressed chunked streaming response.
 *
 * Sets Content-Encoding and Vary headers, starts chunked stream,
 * and initializes the compress stream handle.
 *
 * @param res    Response.
 * @param cfg    Compression config.
 * @param status HTTP status code.
 * @param cs     Compress stream handle to initialize (caller-owned).
 * @return 0 on success, -1 on error.
 */
int kl_http_compress_stream_begin(KlHttpResponse *res, KlCompressConfig *cfg,
                              int status, KlHttpCompressStream *cs);

/**
 * @brief Write data to a compressed stream.
 *
 * Feeds data through the compressor and emits compressed chunks
 * to the underlying chunked stream.
 *
 * @param cs   Compress stream handle.
 * @param data Data to compress and write.
 * @param len  Data length.
 * @return 0 on success, -1 on error.
 */
int kl_http_compress_stream_write(KlHttpCompressStream *cs, const char *data,
                              size_t len);

/**
 * @brief End a compressed stream.
 *
 * Flushes remaining compressed data (trailer), destroys the compression
 * session, and ends the underlying chunked stream.
 *
 * @param cs  Compress stream handle.
 * @return 0 on success, -1 on error.
 */
int kl_http_compress_stream_end(KlHttpCompressStream *cs);

#endif /* KEEL_COMPRESS_H */
