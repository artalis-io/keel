/*
 * allocator_default_stdlib.c — the default KlAllocator backed by the C stdlib.
 *
 * Kept in its own TU so a bring-your-own-allocator or
 * freestanding/UEFI build can exclude the hosted heap entirely. A freestanding
 * profile supplies its own default (e.g. AllocatePool over UEFI boot services)
 * or requires the caller to pass an explicit KlAllocator.
 */
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
