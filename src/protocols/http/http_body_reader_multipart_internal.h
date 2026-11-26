/*
 * http_body_reader_multipart_internal.h: INTERNAL. A white-box probe of the multipart reader for tests.
 *
 * Not part of the public surface and not used by the library itself. A test that must show the parser's
 * work is linear in the bytes received (not quadratic in the number of fragments or events) reads this
 * work counter instead of timing the parser.
 */
#ifndef KEEL_SRC_HTTP_BODY_READER_MULTIPART_INTERNAL_H
#define KEEL_SRC_HTTP_BODY_READER_MULTIPART_INTERNAL_H

#include <keel/http_body_reader.h>
#include <stddef.h>

/* Bytes the reader has searched (delimiters, part-header terminators) or moved (compacting its input
 * buffer) over its life; 0 for a reader that is not a multipart reader. */
size_t keel__http_multipart_work(const KlHttpBodyReader *br);

#endif /* KEEL_SRC_HTTP_BODY_READER_MULTIPART_INTERNAL_H */
