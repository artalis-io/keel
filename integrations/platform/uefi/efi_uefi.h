/*
 * efi_uefi.h: UEFI platform+allocator shim base types.
 *
 * Extends the minimal vendored UEFI surface (efi_min.h)
 * with the handful of extra declarations the shims need that efi_min.h does
 * not carry: the EfiBootServicesData pool type, the EVT_TIMER notify-callback
 * signature, and the EFI_RNG_PROTOCOL. All decls are vendored from the UEFI
 * Specification 2.10 and cite the relevant section.
 *
 * We include efi_min.h verbatim (it already declares the boot-
 * services table with AllocatePool/FreePool/CreateEvent/SetTimer/Stall/
 * LocateProtocol and the system table + console) and add ONLY the missing bits
 * here, so the two stay in sync and we never re-vendor what efi_min.h provides.
 *
 * EFIAPI == Microsoft x64 calling convention (UEFI ABI), honored by clang
 * --target=x86_64-unknown-windows via __attribute__((ms_abi)).
 */
#ifndef KEEL_UEFI_EFI_UEFI_H
#define KEEL_UEFI_EFI_UEFI_H

#include "efi_min.h"

/* ---- Memory pool types for AllocatePool (UEFI 2.10 §7.2, EFI_MEMORY_TYPE) ----
 * efi_min.h defines EfiLoaderData (2). The KEEL allocator uses
 * EfiBootServicesData (4): data owned by a boot-services application, freed
 * automatically at ExitBootServices, the correct class for a transient
 * networking heap that lives only in the pre-boot environment. */
#ifndef EfiBootServicesData
#define EfiBootServicesData 4
#endif

/* ---- EVT_TIMER notify callback signature (UEFI 2.10 §7.1 CreateEvent) ----
 * efi_min.h declares CreateEvent with NotifyFunction as a bare VOID* (it did
 * not need the type). The monotonic-clock shim installs a real notify function,
 * so we give it the spec-correct prototype:
 *   VOID (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, VOID *Context); */
typedef VOID (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, VOID *Context);

/* ---- EFI_RNG_PROTOCOL (UEFI 2.10 §37.5) ----
 * GUID {3152BCA5-EADE-433D-862E-C01CDC291F44}. GetRNG(This, RNGAlgorithm,
 * RNGValueLength, RNGValue): fills RNGValueLength bytes into RNGValue using the
 * platform default algorithm when RNGAlgorithm is NULL. GetInfo enumerates
 * supported algorithms (unused here). */
#define EFI_RNG_PROTOCOL_GUID \
    { 0x3152bca5, 0xeade, 0x433d, { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 } }

typedef EFI_GUID EFI_RNG_ALGORITHM;

typedef struct _EFI_RNG_PROTOCOL EFI_RNG_PROTOCOL;
struct _EFI_RNG_PROTOCOL {
    EFI_STATUS (EFIAPI *GetInfo)(EFI_RNG_PROTOCOL *This,
                                 UINTN *RNGAlgorithmListSize,
                                 EFI_RNG_ALGORITHM *RNGAlgorithmList);
    EFI_STATUS (EFIAPI *GetRNG)(EFI_RNG_PROTOCOL *This,
                                EFI_RNG_ALGORITHM *RNGAlgorithm,
                                UINTN RNGValueLength,
                                UINT8 *RNGValue);
};

#endif /* KEEL_UEFI_EFI_UEFI_H */
