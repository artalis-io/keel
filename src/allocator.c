/*
 * allocator.c: allocator DISPATCH only (no default implementation).
 *
 * This TU is pure vtable dispatch over KlAllocator and pulls in NO libc: a
 * bring-your-own-allocator build (and a future freestanding/UEFI profile) links
 * this without dragging in a hosted heap. The stdlib default allocator lives in
 * its own TU (allocator_default_stdlib.c) so it can be excluded, and alternate
 * defaults (e.g. a UEFI AllocatePool backing) can be swapped in at link time.
 */
#include <keel/allocator.h>

/*
 * Precondition (all three): `a` is a valid allocator, i.e. non-NULL with non-NULL malloc, realloc, and
 * free ops (`ctx` may be NULL). These are the hot-path primitives and deliberately do NOT re-validate:
 * every public boundary that accepts an allocator validates it once, before the first allocation
 * (see src/allocator_validate.h). Passing a NULL or malformed allocator here is undefined.
 */
void *kl_malloc(KlAllocator *a, size_t size) {
    return a->malloc(a->ctx, size);
}

void *kl_realloc(KlAllocator *a, void *ptr, size_t old_size, size_t new_size) {
    return a->realloc(a->ctx, ptr, old_size, new_size);
}

void kl_free(KlAllocator *a, void *ptr, size_t size) {
    a->free(a->ctx, ptr, size);
}
