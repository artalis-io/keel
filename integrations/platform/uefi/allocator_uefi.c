/*
 * allocator_uefi.c: KlAllocator over UEFI Boot Services AllocatePool/FreePool.
 *
 * See allocator_uefi.h. This is the freestanding UEFI heap seam: every KEEL
 * allocation on a UEFI target flows through here, so it is the ONLY place the
 * pre-boot heap is touched. No libc, no global state; the EFI_BOOT_SERVICES*
 * lives in KlAllocator.ctx and is passed to every call.
 */

#include "allocator_uefi.h"

/* Local bounded byte copy: freestanding, no libc. Copies @n bytes src->dst.
 * (The self-contained archive provides memcpy, but the allocator TU must not
 * depend on the archive being present, so it carries its own.) */
static void uefi_bcopy(void *dst, const void *src, UINTN n) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    while (n--) *d++ = *s++;
}

static void *uefi_malloc(void *ctx, size_t size) {
    EFI_BOOT_SERVICES *bs = (EFI_BOOT_SERVICES *)ctx;
    VOID *buf = NULL;
    if (size == 0) size = 1; /* AllocatePool(0) is impl-defined; give a byte. */
    EFI_STATUS st = bs->AllocatePool(EfiBootServicesData, (UINTN)size, &buf);
    if (EFI_ERROR(st) || buf == NULL) return NULL;
    return buf;
}

static void uefi_free(void *ctx, void *ptr, size_t size) {
    EFI_BOOT_SERVICES *bs = (EFI_BOOT_SERVICES *)ctx;
    (void)size; /* FreePool takes no size; pool bookkeeping is firmware-owned. */
    if (ptr) bs->FreePool(ptr);
}

static void *uefi_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    EFI_BOOT_SERVICES *bs = (EFI_BOOT_SERVICES *)ctx;

    if (ptr == NULL) return uefi_malloc(ctx, new_size);
    if (new_size == 0) { if (ptr) bs->FreePool(ptr); return NULL; }

    VOID *nbuf = NULL;
    EFI_STATUS st = bs->AllocatePool(EfiBootServicesData, (UINTN)new_size, &nbuf);
    if (EFI_ERROR(st) || nbuf == NULL) return NULL; /* old buffer left intact */

    /* The KlAllocator realloc contract passes the old size; use it to bound
     * the copy (no header, no over-read past the smaller region). */
    UINTN copy = (old_size < new_size) ? (UINTN)old_size : (UINTN)new_size;
    if (copy) uefi_bcopy(nbuf, ptr, copy);
    bs->FreePool(ptr);
    return nbuf;
}

KlAllocator kl_uefi_allocator(EFI_BOOT_SERVICES *bs) {
    KlAllocator a;
    a.malloc  = uefi_malloc;
    a.realloc = uefi_realloc;
    a.free    = uefi_free;
    a.ctx     = bs;
    return a;
}
