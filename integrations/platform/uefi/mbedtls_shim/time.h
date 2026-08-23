/*
 * mbedtls_shim/time.h — minimal <time.h> for the freestanding mbedTLS build.
 *
 * With MBEDTLS_HAVE_TIME_DATE, mbedTLS's x509.c / platform_util.c reference `struct tm`
 * (broken-down time) even though mbedtls_time_t is pinned to a concrete 64-bit type via
 * MBEDTLS_PLATFORM_TIME_TYPE_MACRO (so mbedtls does NOT include this for the type). We
 * provide just `struct tm` and a `time_t` typedef; the actual clock + gmtime_r are our
 * ALT hooks (time_uefi.c). No functions are implemented here.
 */
#ifndef KEEL_UEFI_MBEDTLS_SHIM_TIME_H
#define KEEL_UEFI_MBEDTLS_SHIM_TIME_H

typedef long long time_t;   /* matches MBEDTLS_PLATFORM_TIME_TYPE_MACRO */

struct tm {
    int tm_sec;    /* 0..60 */
    int tm_min;    /* 0..59 */
    int tm_hour;   /* 0..23 */
    int tm_mday;   /* 1..31 */
    int tm_mon;    /* 0..11 (Jan=0) */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0..6  (Sun=0) */
    int tm_yday;   /* 0..365 */
    int tm_isdst;  /* daylight saving flag */
};

#endif /* KEEL_UEFI_MBEDTLS_SHIM_TIME_H */
