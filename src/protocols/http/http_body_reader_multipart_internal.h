/*
 * http_body_reader_multipart_internal.h: INTERNAL. A white-box probe of the multipart reader for tests.
 *
 * Not part of the public surface and not used by the library itself. A test that must show the per-part
 * header search is linear in the bytes received (not quadratic in the number of fragments) reads this
 * work counter instead of timing the parser.
 */
#ifndef KEEL_SRC_HTTP_BODY_READER_MULTIPART_INTERNAL_H
#define KEEL_SRC_HTTP_BODY_READER_MULTIPART_INTERNAL_H

#include <keel/http_body_reader.h>
#include <stddef.h>

/* Bytes the part-header terminator search has examined over the reader's life (0 for a reader that is
 * not a multipart reader). */
size_t keel__http_multipart_hdr_scan_work(const KlHttpBodyReader *br);

#endif /* KEEL_SRC_HTTP_BODY_READER_MULTIPART_INTERNAL_H */
