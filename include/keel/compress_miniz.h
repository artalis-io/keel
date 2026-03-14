#ifndef KEEL_COMPRESS_MINIZ_H
#define KEEL_COMPRESS_MINIZ_H

#include <keel/compress.h>

/**
 * @brief Create a miniz-based gzip compression context.
 *
 * The context holds the compression level and is shared across all
 * compression sessions created from it.
 *
 * @param level Compression level 1-9 (1=fastest, 9=best, 6=default).
 * @return New context, or NULL on failure.
 */
KlCompressCtx *kl_compress_miniz_ctx_create(int level);

/**
 * @brief Destroy a miniz compression context.
 *
 * Safe to pass as ctx_destroy in KlCompressConfig.
 *
 * @param ctx Context to destroy.
 */
void kl_compress_miniz_ctx_destroy(KlCompressCtx *ctx);

/**
 * @brief Create a miniz gzip compression session.
 *
 * Safe to pass as factory in KlCompressConfig.
 *
 * @param ctx   Shared context from kl_compress_miniz_ctx_create.
 * @param alloc Allocator for session resources.
 * @return New compression session, or NULL on failure.
 */
KlCompress *kl_compress_miniz_create(KlCompressCtx *ctx, KlAllocator *alloc);

#endif /* KEEL_COMPRESS_MINIZ_H */
