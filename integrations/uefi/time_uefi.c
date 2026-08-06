/*
 * time_uefi.c — the mbedTLS gmtime_r + ms_time hooks for the freestanding UEFI build (U-8).
 *
 * The wall-clock time source (mbedtls_time == MBEDTLS_PLATFORM_TIME_MACRO) is the per-session
 * SNAPSHOT in clock_snapshot.c (which never calls GetTime from the verification callback).
 * This TU provides the other two hooks:
 *   mbedtls_platform_gmtime_r()   MBEDTLS_PLATFORM_GMTIME_R_ALT — breaks a Unix time down to
 *                                 struct tm via the pure civil_time math (no libc gmtime).
 *   mbedtls_ms_time()             MBEDTLS_PLATFORM_MS_TIME_ALT — a monotonic ms timer.
 */
#include "time_uefi.h"
#include "civil_time.h"          /* kl_unix_to_civil */
#include "../../src/platform.h"  /* kl_monotonic_ms (for mbedtls_ms_time) */

#include <time.h>            /* struct tm (mbedtls_shim/time.h) */
#include <stdint.h>

/* mbedtls_time_t is `long long` (from the shim <time.h>). */
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
