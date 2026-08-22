/*
 * allocator_uefi.h — a KlAllocator backed by UEFI Boot Services.
 *
 * The first real UEFI client requires an EXPLICIT allocator (the Phase-10
 * design rule, finding 7): there is no global heap in the pre-boot environment,
 * so a KEEL freestanding client is handed a KlAllocator* built from the boot
 * services it was launched with. This is a FACTORY, not a global singleton.
 *
 * Backing (UEFI 2.10 §7.2):
 *   malloc(size)                 -> AllocatePool(EfiBootServicesData, size)
 *   free(ptr, size)              -> FreePool(ptr)                 (size ignored)
 *   realloc(ptr, old, new)       -> AllocatePool(new) + copy min(old,new) + FreePool(old)
 *
 * AllocatePool failure returns NULL (the KlAllocator contract). The EFI_BOOT_
 * SERVICES* is carried in KlAllocator.ctx.
 */
#ifndef KEEL_UEFI_ALLOCATOR_UEFI_H
#define KEEL_UEFI_ALLOCATOR_UEFI_H

#include <keel/allocator.h>
#include "efi_uefi.h"

/* Build a KlAllocator whose backing store is @bs (EFI Boot Services). The
 * returned allocator borrows @bs; @bs must outlive every allocation made
 * through it (i.e. until ExitBootServices). Returns a by-value KlAllocator (the
 * vtable + ctx=bs), mirroring kl_allocator_default(). */
KlAllocator kl_uefi_allocator(EFI_BOOT_SERVICES *bs);

#endif /* KEEL_UEFI_ALLOCATOR_UEFI_H */
