/*
 * wallclock_uefi.h — EFI_TIME → UTC-seconds decode + the unspecified-timezone policy.
 *
 * Kept in its own TU so the GetTime→decode→gate path is host-unit-testable with a
 * fabricated EFI_TIME (the mock harness links this TU; platform_uefi.c only supplies the
 * actual GetTime call). Pure apart from the app-configurable timezone offset below.
 *
 * UNSPECIFIED-TIMEZONE POLICY (UEFI 2.10 §8.3): a GetTime whose TimeZone is
 * EFI_UNSPECIFIED_TIMEZONE returns LOCAL time, not UTC. We must NOT silently treat it as UTC.
 *   - Default: REJECT (fail-closed) — a local time with an unknown offset cannot validate cert
 *     expiry to within the day.
 *   - kl_uefi_set_unspecified_tz(minutes): the app declares the machine's offset from UTC (UEFI
 *     sign: UTC = local + minutes), used only when the RTC reports unspecified.
 *   - Compile flag KL_UEFI_ASSUME_UNSPECIFIED_UTC (test/OVMF ONLY, never the production default):
 *     assume an unspecified zone already reads UTC (OVMF/QEMU convention).
 */
#ifndef KEEL_UEFI_WALLCLOCK_UEFI_H
#define KEEL_UEFI_WALLCLOCK_UEFI_H

#include "efi_uefi.h"     /* EFI_TIME */
#include <stdint.h>

/* Decode a GetTime result into UTC Unix seconds. @gettime_ok is 0 if the GetTime call itself
 * failed/was unavailable. Applies: gettime_ok check → field validation (kl_civil_valid) →
 * unspecified-timezone policy (above) → sanity floor + local→UTC conversion. Returns 0 with
 * *out_unix set, or -1 (fail-closed) on any failure. Host-unit-testable with a fabricated
 * EFI_TIME. */
int kl_uefi_wallclock_from_efi(int gettime_ok, const EFI_TIME *t, int64_t *out_unix);

/* Configure the offset (minutes, UEFI sign: UTC = local + minutes) applied when the RTC reports
 * EFI_UNSPECIFIED_TIMEZONE. Until set, an unspecified zone is REJECTED (unless the build defines
 * KL_UEFI_ASSUME_UNSPECIFIED_UTC). */
void kl_uefi_set_unspecified_tz(int minutes);

/* Test helper: revert to the default (reject unspecified). */
void kl_uefi_clear_unspecified_tz(void);

#endif /* KEEL_UEFI_WALLCLOCK_UEFI_H */
