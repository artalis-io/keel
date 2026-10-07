#ifndef KEEL_HTTP1_CHUNKED_H
#define KEEL_HTTP1_CHUNKED_H

#include <keel/http_body_reader.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Chunk extensions and trailers are framing the decoder skips: they reach no body reader and count
 * toward no body limit, so they are bounded here. Both are fixed caps (the decoder carries no server
 * configuration); a request past either fails like any other malformed chunk. */

/** @brief Cap on the bytes of one chunk-size line's extensions (after the ';', before the CR). */
#define KL_HTTP1_CHUNK_EXT_MAX 4096

/** @brief Cap on the bytes of the whole trailer section (every trailer line, its CRLF included). */
#define KL_HTTP1_CHUNK_TRAILER_MAX 8192

typedef enum {
    KL_HTTP1_CHUNK_SIZE,       /**< reading hex chunk-size */
    KL_HTTP1_CHUNK_EXT,        /**< skipping chunk extensions after ';' */
    KL_HTTP1_CHUNK_SIZE_CR,    /**< saw CR, expecting LF after chunk-size line */
    KL_HTTP1_CHUNK_DATA,       /**< reading chunk-data bytes */
    KL_HTTP1_CHUNK_DATA_CR,    /**< saw CR after chunk-data, expecting LF */
    KL_HTTP1_CHUNK_TRAILER,    /**< reading/skipping trailer headers */
    KL_HTTP1_CHUNK_TRAILER_CR, /**< saw CR in trailer, expecting LF (could be final) */
    KL_HTTP1_CHUNK_DONE,
    KL_HTTP1_CHUNK_ERROR
} KlHttp1ChunkedState;

typedef struct {
    KlHttp1ChunkedState state;
    size_t chunk_remaining;  /**< bytes left in current chunk */
    size_t total_body;       /**< total de-chunked bytes received */
    size_t size_accum;       /**< hex accumulator for chunk size */
    int    size_digits;      /**< hex digits seen (max 16) */
    int    trailer_cr;       /**< saw CR on an empty trailer line (final CRLF) */
    size_t meta_len;         /**< bytes of the current extension list, or of the trailer section */
} KlHttp1ChunkedDecoder;

/** @brief Initialize/reset decoder state. */
void kl_http1_chunked_init(KlHttp1ChunkedDecoder *dec);

/**
 * @brief Feed raw bytes from socket, de-chunk, and forward to body reader.
 *
 * @param dec    Decoder state (must be initialized with kl_http1_chunked_init).
 * @param data   Raw chunked-encoded bytes from socket.
 * @param len    Number of bytes.
 * @param reader Body reader to forward de-chunked data to (NULL to discard).
 * @return  0  need more data (INCOMPLETE)
 *          1  terminal chunk received (DONE)
 *         -1  parse error or body reader rejected
 */
int kl_http1_chunked_decode(KlHttp1ChunkedDecoder *dec, const char *data, size_t len,
                      KlHttpBodyReader *reader);

#ifdef __cplusplus
}
#endif

#endif
