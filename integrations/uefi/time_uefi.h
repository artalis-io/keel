/*
 * time_uefi.h — the mbedTLS clock hooks for the freestanding UEFI build (U-8).
 *
 * time_uefi.c defines the two functions mbedTLS uses for certificate validity-time when
 * MBEDTLS_HAVE_TIME_DATE is on: a time source (registered via mbedtls_platform_set_time)
 * and gmtime_r (MBEDTLS_PLATFORM_GMTIME_R_ALT). Both are thin wrappers over
 * kl_uefi_wallclock() (Runtime Services GetTime, fail-closed) + the pure civil_time math.
 * kl_uefi_mbedtls_platform_init() registers the time function.
 */
#ifndef KEEL_UEFI_TIME_UEFI_H
#define KEEL_UEFI_TIME_UEFI_H

/* mbedtls_time_t is pinned to `long long` (MBEDTLS_PLATFORM_TIME_TYPE_MACRO). This is the
 * function registered via mbedtls_platform_set_time(): returns UTC seconds since the Unix
 * epoch, or 0 (epoch) if the clock is untrustworthy — which makes every real cert fail its
 * notBefore check, i.e. HTTPS fails closed without a trustworthy RTC. */
long long kl_uefi_mbedtls_time(long long *t);

#endif /* KEEL_UEFI_TIME_UEFI_H */
