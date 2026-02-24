#ifndef KEEL_CHUNKED_H
#define KEEL_CHUNKED_H

#include <keel/body_reader.h>
#include <stddef.h>

typedef enum {
    KL_CHUNK_SIZE,       /* reading hex chunk-size */
    KL_CHUNK_EXT,        /* skipping chunk extensions after ';' */
    KL_CHUNK_SIZE_CR,    /* saw CR, expecting LF after chunk-size line */
    KL_CHUNK_DATA,       /* reading chunk-data bytes */
    KL_CHUNK_DATA_CR,    /* saw CR after chunk-data, expecting LF */
    KL_CHUNK_TRAILER,    /* reading/skipping trailer headers */
    KL_CHUNK_TRAILER_CR, /* saw CR in trailer, expecting LF (could be final) */
    KL_CHUNK_DONE,
    KL_CHUNK_ERROR
} KlChunkedState;

typedef struct {
    KlChunkedState state;
    size_t chunk_remaining;  /* bytes left in current chunk */
    size_t total_body;       /* total de-chunked bytes received */
    size_t size_accum;       /* hex accumulator for chunk size */
    int    size_digits;      /* hex digits seen (max 16) */
    int    trailer_cr;       /* saw CR on an empty trailer line (final \r\n) */
} KlChunkedDecoder;

/** Initialize/reset decoder state. */
void kl_chunked_init(KlChunkedDecoder *dec);

/**
 * Feed raw bytes from socket, de-chunk, and forward to body reader.
 *
 * @param dec    Decoder state (must be initialized with kl_chunked_init).
 * @param data   Raw chunked-encoded bytes from socket.
 * @param len    Number of bytes.
 * @param reader Body reader to forward de-chunked data to (NULL to discard).
 * @return  0  need more data (INCOMPLETE)
 *          1  terminal chunk received (DONE)
 *         -1  parse error or body reader rejected
 */
int kl_chunked_decode(KlChunkedDecoder *dec, const char *data, size_t len,
                      KlBodyReader *reader);

#endif
