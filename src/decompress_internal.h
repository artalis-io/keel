/*
 * decompress_internal.h: INTERNAL. Shared required-subset validator for the KlDecompress vtable.
 *
 * A KlDecompress session is produced by a caller-supplied factory at four sites across two TUs
 * (decompress.c: kl_decompress_body / kl_decompress_stream_init; http_client_common.c: the buffered and
 * streaming response-decompression helpers). Core calls decompress (single-shot), dfeed (streaming),
 * encoding, and destroy unconditionally; reset is optional (documented "may be NULL", never called). A
 * session missing a required op is rejected right after the factory returns it (not in a hot path); its
 * destroy is guarded at the call site because a malformed table may omit destroy, in which case the
 * object cannot be freed generically. Header-only so both TUs share one definition; not installed, no
 * ABI commitment.
 */
#ifndef KEEL_SRC_DECOMPRESS_INTERNAL_H
#define KEEL_SRC_DECOMPRESS_INTERNAL_H

#include <keel/decompress.h>

static inline int kl_decompress_vtable_valid(const KlDecompress *d) {
    return d && d->decompress && d->dfeed && d->encoding && d->destroy;
}

#endif /* KEEL_SRC_DECOMPRESS_INTERNAL_H */
