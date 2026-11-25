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
    /* Appended after 3.5.0: bounded streaming (kl_http_compress_stream_try_write). Keel-owned. */
    char        *stage;      /**< One flush's output before it is admitted as a chunk */
    size_t       stage_cap;  /**< Its size: the largest bound admitted so far */
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
 * It releases the session in every state, but it also writes the finish and the
 * terminal chunk through the legacy writer, outside the capacity. To abandon a
 * bounded stream use kl_http_compress_stream_release(), which writes nothing.
 *
 * @param cs  Compress stream handle.
 * @return 0 on success, -1 on error.
 */
int kl_http_compress_stream_end(KlHttpCompressStream *cs);

/* ── Bounded compressed streaming ────────────────────────────────────────────────────────────────
 *
 * kl_http_response_stream_try_write() for a compressed stream: the same statuses, the same writable
 * edge (kl_http_response_stream_on_writable on cs->res), the same capacity. Each accepted write is
 * compressed and flushed whole (KL_COMPRESS_FLUSH_SYNC), so its bytes reach the client without
 * waiting for later input and the codec holds nothing back between writes; the output goes out as
 * one chunk. Admission is decided before the codec is touched, against the codec's worst case for
 * this input (KlCompress.flush_bound) framed as one chunk: a refused write leaves the codec and the
 * output exactly as they were. Only a codec that supplies flush_bound can be used
 * (kl_http_compress_stream_bounded); any other gets ERROR and is never fed.
 *
 * Flushing every write costs some ratio on very small writes (each flush ends a deflate block);
 * a producer of many tiny fragments that can wait may batch them into fewer writes. Memory: one
 * staging buffer per stream, at most the largest admitted bound (never more than the capacity),
 * freed when the stream ends. Do not mix with kl_http_compress_stream_write on one stream if the
 * bound matters: an unflushed write leaves output owed, which the next bound has to cover.
 */

/**
 * @brief Release the stream's codec and staging buffer, writing nothing.
 *
 * For a stream that will not be finished: from the op's on_cancel, or after try_end answered
 * TOO_LARGE. The response is left as it is (end or reset it as usual). Safe on a released stream.
 */
void kl_http_compress_stream_release(KlHttpCompressStream *cs);

/** @brief 1 when the stream's codec supplies flush_bound (bounded writes are possible), else 0. */
int kl_http_compress_stream_bounded(const KlHttpCompressStream *cs);

/**
 * @brief Compress and write @p data as one flushed chunk, all or nothing, without blocking.
 *
 * - ACCEPTED: compressed, flushed and queued. A zero-length write is ACCEPTED and feeds nothing.
 * - WOULD_BLOCK: the worst case fits the capacity but not the room left; nothing was fed. Arms the
 *   writable edge.
 * - TOO_LARGE: the worst case exceeds the capacity (or does not fit a size_t): split the input.
 * - CLOSED: the stream has ended or the response is gone.
 * - ERROR: sticky. The codec lacks flush_bound or data is NULL with a length (nothing was fed); or,
 *   once admitted, the staging buffer could not be allocated, the codec failed or emitted more than
 *   its bound: the response's stream then fails as well and the connection closes. Also when the
 *   response's stream had already failed. Release with kl_http_compress_stream_release().
 *
 * On a HEAD response the write is ACCEPTED without running the codec (no body is sent).
 * KlCompress.flush_bound answering SIZE_MAX (its overflow value) is TOO_LARGE.
 */
KlHttpStreamStatus kl_http_compress_stream_try_write(KlHttpCompressStream *cs, const char *data,
                                                      size_t len);

/**
 * @brief Finish a bounded compressed stream: its final output, then the terminal chunk.
 *
 * The codec's finish output is admitted like a write (its flush_bound with final set), so a full
 * stream answers WOULD_BLOCK and ends once room frees: finishing never deadlocks on a stream that
 * is still moving, and the terminal chunk itself is never refused. On ACCEPTED, ERROR or CLOSED the
 * session and staging buffer are released (do not call kl_http_compress_stream_end() afterwards);
 * on WOULD_BLOCK or TOO_LARGE nothing changed. WOULD_BLOCK arms the writable edge, and retries
 * leave the codec untouched until one fits; the finish's worst case is fixed once nothing more is
 * written, so it fits as soon as pending output drains far enough. TOO_LARGE means it can never fit:
 * the stream's capacity is smaller than the codec's finish (a few dozen bytes for gzip). That is
 * permanent; release the stream with kl_http_compress_stream_release().
 */
KlHttpStreamStatus kl_http_compress_stream_try_end(KlHttpCompressStream *cs);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_HTTP_COMPRESS_H */
