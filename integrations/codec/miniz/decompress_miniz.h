#ifndef KEEL_DECOMPRESS_MINIZ_H
#define KEEL_DECOMPRESS_MINIZ_H

#include <keel/decompress.h>
#include <keel/compress.h>  /* for KlCompressCtx */

/**
 * @brief Create a miniz-based gzip decompression session.
 *
 * Safe to pass as factory in KlDecompressConfig.
 * Shares KlCompressCtx with kl_compress_miniz_create: use
 * kl_compress_miniz_ctx_create/destroy for context lifecycle.
 *
 * @param ctx   Shared context from kl_compress_miniz_ctx_create.
 * @param alloc Allocator for session resources.
 * @return New decompression session, or NULL on failure.
 */
KlDecompress *kl_decompress_miniz_create(KlCompressCtx *ctx,
                                          KlAllocator *alloc);

#endif /* KEEL_DECOMPRESS_MINIZ_H */
