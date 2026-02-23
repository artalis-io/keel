#include <keel/allocator.h>
#include <stdlib.h>

static void *stdlib_malloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void *stdlib_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void stdlib_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

KlAllocator kl_allocator_default(void) {
    return (KlAllocator){
        .malloc  = stdlib_malloc,
        .realloc = stdlib_realloc,
        .free    = stdlib_free,
        .ctx     = NULL,
    };
}

void *kl_malloc(KlAllocator *a, size_t size) {
    return a->malloc(a->ctx, size);
}

void *kl_realloc(KlAllocator *a, void *ptr, size_t old_size, size_t new_size) {
    return a->realloc(a->ctx, ptr, old_size, new_size);
}

void kl_free(KlAllocator *a, void *ptr, size_t size) {
    a->free(a->ctx, ptr, size);
}
