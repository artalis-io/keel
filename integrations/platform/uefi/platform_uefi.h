/*
 * platform_uefi.h: UEFI backing for KEEL's freestanding platform seams.
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
#include <stdint.h>

/* Cert validity-time sanity floor: a GetTime year below this is treated as an
 * unset/stuck RTC and rejected (fail-closed) rather than trusted to validate cert expiry.
 * Bump at/above the build year; certs minted before this are vanishingly rare in practice
 * and a too-old clock is the failure mode we are guarding against. */
#ifndef KL_UEFI_TIME_FLOOR_YEAR
#define KL_UEFI_TIME_FLOOR_YEAR 2025
#endif

/* Install the UEFI backing for kl_monotonic_ms / kl_plat_random. Creates the
 * periodic 1 ms EVT_TIMER that drives the monotonic tick counter and locates
 * EFI_RNG_PROTOCOL (if present). Call exactly once, early in efi_main, before
 * any KEEL clock/random use. Returns 0 on success, -1 if the timer could not be
 * created (the clock would then be stuck at 0). Idempotent after first success.
 */
int kl_uefi_platform_init(EFI_BOOT_SERVICES *bs, EFI_SYSTEM_TABLE *st);

/* 1 iff kl_uefi_platform_init() FULLY succeeded (the periodic monotonic timer is armed, so
 * kl_monotonic_ms advances). 0 after a failed or not-yet-run init; in which case the module
 * has torn down its installed state (GetTime unreachable), so the TLS cert clock (a snapshot
 * advanced by the monotonic tick) cannot be captured and TLS bring-up refuses. */
int kl_uefi_platform_ready(void);

/* Optional bring-up aid: route kl_uefi_platform_init's step trace to @out (a
 * console). Pass NULL to silence. Call before kl_uefi_platform_init. */
void kl_uefi_platform_trace(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out);

/* Query whether kl_plat_random is drawing from a real entropy source
 * (EFI_RNG_PROTOCOL). Returns 1 if EFI_RNG is available, 0 if the platform is in
 * the FAIL-CLOSED state (no entropy source; kl_plat_random zeroes its buffer).
 * Security-sensitive callers (TLS, later) MUST check this and refuse to proceed
 * when it returns 0. */
int kl_uefi_have_entropy(void);

/* ── ExitBootServices lifetime ───────────────────────────────────────────────
 * The EFI network provider is BOOT-SERVICES-ONLY: EFI_TCP4/UDP4, AllocatePool, and
 * events all vanish at ExitBootServices(). kl_uefi_platform_init registers an
 * EVT_SIGNAL_EXIT_BOOT_SERVICES notify so KEEL degrades fail-closed the instant the
 * app leaves boot services. */

/* 1 once ExitBootServices() has fired; KEEL's EFI providers then refuse boot-service
 * I/O. kl_uefi_have_entropy() also returns 0 after EBS. */
int kl_uefi_after_ebs(void);

/* Cert validity-time clock: fill *out_unix with the current UTC time in seconds since
 * the Unix epoch, read from Runtime Services GetTime. Returns 0 on success, -1 FAIL-CLOSED
 * (no GetTime, GetTime error, or year < KL_UEFI_TIME_FLOOR_YEAR: an untrustworthy RTC). Valid
 * before AND after ExitBootServices (GetTime is a runtime service). This backs the mbedTLS
 * clock (time_uefi.c) so TLS enforces certificate notBefore/notAfter. */
int kl_uefi_wallclock(int64_t *out_unix);

/* Cert-clock gate (fail-closed): 1 iff kl_uefi_wallclock() currently yields a trustworthy
 * UTC time (GetTime present + succeeds, fields valid, year >= floor). TLS bring-up
 * (kl_uefi_mbedtls_platform_init) calls this and REFUSES to initialise when it returns 0, so
 * HTTPS cannot start without a trustworthy clock, independent of any certificate's dates
 * (a returned epoch-0 fallback alone could still admit a cert whose window spans 1970). */
int kl_uefi_have_trustworthy_wallclock(void);

/* Release the platform's boot-services resources (periodic timer + EBS event). Call
 * BEFORE ExitBootServices for a clean teardown; idempotent. Afterwards
 * kl_monotonic_ms freezes and kl_plat_random fail-closes. See kl_uefi_shutdown()
 * (lifecycle_uefi.h) for the full net-stack teardown. */
void kl_uefi_platform_shutdown(void);

#endif /* KEEL_UEFI_PLATFORM_UEFI_H */
