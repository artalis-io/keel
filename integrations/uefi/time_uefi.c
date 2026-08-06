/*
 * time_uefi.c — mbedTLS clock hooks over EFI Runtime Services GetTime (U-8).
 *
 * Split out of the mbedTLS adapter (like entropy_uefi.c) so it carries no mbedTLS
 * dependency beyond the two well-known ALT hook names. Certificate validity-time
 * (notBefore/notAfter) is enforced through:
 *
 *   kl_uefi_mbedtls_time()        registered via mbedtls_platform_set_time() — returns the
 *                                 current UTC time (Unix seconds) from kl_uefi_wallclock(),
 *                                 or 0 (epoch 1970) when the RTC is untrustworthy so every
 *                                 real cert fails notBefore (FAIL-CLOSED HTTPS).
 *   mbedtls_platform_gmtime_r()   MBEDTLS_PLATFORM_GMTIME_R_ALT — breaks a Unix time down to
 *                                 struct tm via the pure civil_time math (no libc gmtime).
 */
#include "time_uefi.h"
#include "platform_uefi.h"       /* kl_uefi_wallclock */
#include "civil_time.h"          /* kl_unix_to_civil */
#include "../../src/platform.h"  /* kl_monotonic_ms (for mbedtls_ms_time) */

#include <time.h>            /* struct tm (mbedtls_shim/time.h) */
#include <stdint.h>

long long kl_uefi_mbedtls_time(long long *t) {
    int64_t u = 0;                                  /* fail-closed default: epoch */
    if (kl_uefi_wallclock(&u) != 0) u = 0;          /* untrustworthy clock → 1970 → certs fail */
    long long r = (long long)u;
    if (t) *t = r;
    return r;
}

/* mbedtls_time_t is `long long` (MBEDTLS_PLATFORM_TIME_TYPE_MACRO). */
struct tm *mbedtls_platform_gmtime_r(const long long *tt, struct tm *tm_buf);
struct tm *mbedtls_platform_gmtime_r(const long long *tt, struct tm *tm_buf) {
    if (!tt || !tm_buf) return NULL;
    KlCivil c;
    kl_unix_to_civil((int64_t)*tt, &c);
    tm_buf->tm_year  = c.year - 1900;
    tm_buf->tm_mon   = c.mon - 1;
    tm_buf->tm_mday  = c.day;
    tm_buf->tm_hour  = c.hour;
    tm_buf->tm_min   = c.min;
    tm_buf->tm_sec   = c.sec;
    tm_buf->tm_wday  = 0;      /* mbedTLS x509 time compare does not use wday/yday/isdst */
    tm_buf->tm_yday  = 0;
    tm_buf->tm_isdst = 0;
    return tm_buf;
}

/* mbedtls_ms_time_t is int64_t (default). This is the MONOTONIC millisecond timer mbedTLS
 * uses for internal timing (distinct from the wall clock) — back it with our monotonic tick
 * (MBEDTLS_PLATFORM_MS_TIME_ALT), avoiding the windows-target GetSystemTimeAsFileTime path. */
int64_t mbedtls_ms_time(void);
int64_t mbedtls_ms_time(void) {
    return (int64_t)kl_monotonic_ms();
}
