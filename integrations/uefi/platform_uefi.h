/*
 * platform_uefi.h — UEFI backing for KEEL's freestanding platform seams.
 *
 * The freestanding archive leaves two OS-specific hooks undefined (see
 * src/platform.h + tests/freestanding_symbol_gate.sh whitelist):
 *   uint64_t kl_monotonic_ms(void);
 *   void     kl_plat_random(void *buf, size_t len);
 * platform_uefi.c DEFINES both over UEFI Boot Services / EFI_RNG_PROTOCOL.
 *
 * Because both are free functions with fixed C signatures (no ctx pointer), the
 * boot-services + system-table handles are installed ONCE, up front, via
 * kl_uefi_platform_init(). The app MUST call it before any KEEL code that reads
 * the clock or draws randomness.
 */
#ifndef KEEL_UEFI_PLATFORM_UEFI_H
#define KEEL_UEFI_PLATFORM_UEFI_H

#include "efi_uefi.h"

/* Install the UEFI backing for kl_monotonic_ms / kl_plat_random. Creates the
 * periodic 1 ms EVT_TIMER that drives the monotonic tick counter and locates
 * EFI_RNG_PROTOCOL (if present). Call exactly once, early in efi_main, before
 * any KEEL clock/random use. Returns 0 on success, -1 if the timer could not be
 * created (the clock would then be stuck at 0). Idempotent after first success.
 */
int kl_uefi_platform_init(EFI_BOOT_SERVICES *bs, EFI_SYSTEM_TABLE *st);

/* Optional bring-up aid: route kl_uefi_platform_init's step trace to @out (a
 * console). Pass NULL to silence. Call before kl_uefi_platform_init. */
void kl_uefi_platform_trace(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out);

/* Query whether kl_plat_random is drawing from a real entropy source
 * (EFI_RNG_PROTOCOL). Returns 1 if EFI_RNG is available, 0 if the platform is in
 * the FAIL-CLOSED state (no entropy source — kl_plat_random zeroes its buffer).
 * Security-sensitive callers (TLS, later) MUST check this and refuse to proceed
 * when it returns 0. */
int kl_uefi_have_entropy(void);

#endif /* KEEL_UEFI_PLATFORM_UEFI_H */
