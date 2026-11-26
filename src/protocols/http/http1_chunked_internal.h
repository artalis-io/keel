/*
 * http1_chunked_internal.h: INTERNAL. The chunked decoder with the position where its message ended.
 */
#ifndef KEEL_SRC_HTTP1_CHUNKED_INTERNAL_H
#define KEEL_SRC_HTTP1_CHUNKED_INTERNAL_H

#include <keel/http1_chunked.h>
#include <stddef.h>

/* kl_http1_chunked_decode(), also reporting in *used (when non-NULL) how many of the `len` bytes
 * belong to this body: on a 1 (terminal chunk and trailers done) the bytes from data + *used on are
 * past the end of the request (the start of a pipelined one); otherwise *used is `len`. */
int kl_http1_chunked_decode_used(KlHttp1ChunkedDecoder *dec, const char *data, size_t len,
                                 KlHttpBodyReader *reader, size_t *used);

#endif /* KEEL_SRC_HTTP1_CHUNKED_INTERNAL_H */
