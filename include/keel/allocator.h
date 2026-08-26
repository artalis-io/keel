#ifndef KEEL_ALLOCATOR_H
#define KEEL_ALLOCATOR_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Bring-your-own allocator vtable.
 *
 * All KEEL allocations go through this interface. Provide custom
 * implementations for arena/pool allocation, tracking, or sandboxing.
 */
typedef struct {
    void *(*malloc)(void *ctx, size_t size);                        /**< Allocate size bytes */
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size); /**< Resize allocation */
    void  (*free)(void *ctx, void *ptr, size_t size);              /**< Free allocation */
    void *ctx;                                                     /**< User-provided context */
} KlAllocator;

/**
 * @brief Return the default stdlib-backed allocator.
 * @return Allocator wrapping malloc/realloc/free.
 */
KlAllocator kl_allocator_default(void);

/**
 * @brief Allocate memory through the given allocator.
 * @param a    Allocator to use.
 * @param size Bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure.
 */
void *kl_malloc(KlAllocator *a, size_t size);

/**
 * @brief Reallocate memory through the given allocator.
 * @param a        Allocator to use.
 * @param ptr      Existing allocation (may be NULL).
 * @param old_size Previous allocation size.
 * @param new_size Desired new size.
 * @return Pointer to reallocated memory, or NULL on failure.
 */
void *kl_realloc(KlAllocator *a, void *ptr, size_t old_size, size_t new_size);

/**
 * @brief Free memory through the given allocator.
 * @param a    Allocator to use.
 * @param ptr  Pointer to free.
 * @param size Size of the allocation being freed.
 */
void  kl_free(KlAllocator *a, void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif
