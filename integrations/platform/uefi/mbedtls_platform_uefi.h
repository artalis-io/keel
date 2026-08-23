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

/* Bring up the mbedTLS platform: register EFI AllocatePool/FreePool as mbedTLS's calloc/free
 * (borrows @bs; it must outlive every TLS allocation) AND capture the U-8 cert-validity clock
 * snapshot (the fail-closed gate — see clock_snapshot.h). Call once, AFTER kl_uefi_platform_init()
 * (which installs Runtime Services GetTime). Idempotent after first success.
 *
 * Returns 0 on success, -1 if it CANNOT bring up TLS. Failure reasons include:
 *   - @bs is NULL;
 *   - no trustworthy wall clock: Runtime Services GetTime absent or failing, the returned EFI_TIME
 *     is malformed, its year is below KL_UEFI_TIME_FLOOR_YEAR, or its timezone is unspecified with
 *     no configured offset (see wallclock_uefi.h / kl_uefi_set_unspecified_tz);
 *   - the mbedTLS calloc/free registration failed.
 * On -1 NO KlTlsCtx may be created — this is the structural gate that makes certificate
 * validity-time enforceable, so a caller MUST treat -1 as "TLS unavailable" and not proceed. */
int kl_uefi_mbedtls_platform_init(EFI_BOOT_SERVICES *bs);

/* F5: release the borrowed Boot Services pointer so no subsequent mbedTLS heap call
 * touches firmware. After this, calloc returns NULL and free is a no-op. Call on the
 * shutdown path AFTER destroying every KlTlsCtx/KlTls and BEFORE ExitBootServices.
 * Idempotent; safe to call even if init was never run. */
void kl_uefi_mbedtls_platform_shutdown(void);

#endif /* KEEL_UEFI_MBEDTLS_PLATFORM_UEFI_H */
