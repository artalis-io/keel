/*
 * time_uefi.h: the mbedTLS gmtime_r + ms_time hooks for the freestanding UEFI build.
 *
 * time_uefi.c defines mbedtls_platform_gmtime_r (MBEDTLS_PLATFORM_GMTIME_R_ALT) and
 * mbedtls_ms_time (MBEDTLS_PLATFORM_MS_TIME_ALT). The wall-clock time source
 * (mbedtls_time == MBEDTLS_PLATFORM_TIME_MACRO -> kl_uefi_mbedtls_time) is the per-session
 * SNAPSHOT in clock_snapshot.c. Both mbedTLS hook functions are named by mbedTLS itself, so
 * this header carries no public declarations; it exists only to document the split.
 */
#ifndef KEEL_UEFI_TIME_UEFI_H
#define KEEL_UEFI_TIME_UEFI_H

#endif /* KEEL_UEFI_TIME_UEFI_H */
