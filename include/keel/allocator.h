#ifndef KEEL_ALLOCATOR_H
#define KEEL_ALLOCATOR_H

#include <stddef.h>

typedef struct {
    void *(*malloc)(void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)(void *ctx, void *ptr, size_t size);
    void *ctx;
} KlAllocator;

/* Default stdlib allocator (ignores ctx and sizes) */
KlAllocator kl_allocator_default(void);

/* Internal helpers used by all modules */
void *kl_malloc(KlAllocator *a, size_t size);
void *kl_realloc(KlAllocator *a, void *ptr, size_t old_size, size_t new_size);
void  kl_free(KlAllocator *a, void *ptr, size_t size);

#endif
