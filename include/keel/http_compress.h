#ifndef KEEL_HTTP_COMPRESS_H
#define KEEL_HTTP_COMPRESS_H

#include <keel/compress.h>
#include <keel/http_response.h>
#include <keel/allocator.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file http_compress.h
 * @brief HTTP response-compression adapter over the generic codec (compress.h).
 *
 * The generic byte-in/byte-out codec vtable lives in <keel/compress.h>; this
 * header is the HTTP-specific adapter that drives it against a KlHttpResponse
 * (single-shot body compression + chunked compressed streaming, with
 * Content-Encoding/Vary handling). Implementation: src/protocols/http/http_compress.c.
 */

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

#ifdef __cplusplus
}
#endif

#endif /* KEEL_HTTP_COMPRESS_H */
