/*
 * mbedtls_platform_uefi.h — install the UEFI backing for the freestanding mbedTLS
 * build used by U-4 (heap + entropy).
 *
 * mbedTLS, built with MBEDTLS_PLATFORM_MEMORY, calls a registered calloc/free pair
 * for ALL its heap. We register a pair backed by EFI AllocatePool(EfiBootServicesData)
 * + FreePool. It also draws entropy through mbedtls_hardware_poll() (enabled via
 * MBEDTLS_ENTROPY_HARDWARE_ALT), which mbedtls_platform_uefi.c implements over
 * kl_plat_random (EFI_RNG) with a loudly-documented weak fallback.
 *
 * Call kl_uefi_mbedtls_platform_init(bs) ONCE, after kl_uefi_platform_init(), before
 * creating any KlTlsCtx.
 */
#ifndef KEEL_UEFI_MBEDTLS_PLATFORM_UEFI_H
#define KEEL_UEFI_MBEDTLS_PLATFORM_UEFI_H

#include "efi_uefi.h"

/* Register EFI AllocatePool/FreePool as mbedTLS's calloc/free (borrows @bs; it must
 * outlive every TLS allocation). Returns 0 on success, -1 if registration failed.
 * Idempotent after first success. */
int kl_uefi_mbedtls_platform_init(EFI_BOOT_SERVICES *bs);

/* F5: release the borrowed Boot Services pointer so no subsequent mbedTLS heap call
 * touches firmware. After this, calloc returns NULL and free is a no-op. Call on the
 * shutdown path AFTER destroying every KlTlsCtx/KlTls and BEFORE ExitBootServices.
 * Idempotent; safe to call even if init was never run. */
void kl_uefi_mbedtls_platform_shutdown(void);

#endif /* KEEL_UEFI_MBEDTLS_PLATFORM_UEFI_H */
