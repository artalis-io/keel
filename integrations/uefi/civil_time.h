/*
 * civil_time.h — pure proleptic-Gregorian civil-date ⇄ Unix-seconds conversion.
 *
 * No EFI, no mbedTLS, no libc — just integer math (Howard Hinnant's days_from_civil /
 * civil_from_days). Split out so the error-prone leap-year / epoch arithmetic behind
 * UEFI cert validity-time (EFI GetTime → mbedTLS notBefore/notAfter checks) is unit-
 * tested exhaustively on the host, independent of firmware. UTC throughout.
 */
#ifndef KEEL_UEFI_CIVIL_TIME_H
#define KEEL_UEFI_CIVIL_TIME_H

#include <stdint.h>

typedef struct {
    int year;   /* full year, e.g. 2026 */
    int mon;    /* 1..12 */
    int day;    /* 1..31 */
    int hour;   /* 0..23 */
    int min;    /* 0..59 */
    int sec;    /* 0..60 (leap second tolerated) */
} KlCivil;

/* Civil UTC date/time → seconds since the Unix epoch (1970-01-01T00:00:00Z). */
int64_t kl_civil_to_unix(const KlCivil *c);

/* Inverse: Unix seconds (UTC) → civil date/time. */
void    kl_unix_to_civil(int64_t unix_sec, KlCivil *out);

/* Cert-clock policy applied to a decoded wall-clock reading (fail-closed + sanity floor).
 * Rejects @c->year < @floor_year (an unset/stuck RTC — return -1, do not fill *out_unix),
 * else normalises local→UTC by subtracting @tz_minutes (minutes local is offset from UTC;
 * ignored unless @tz_specified) and returns 0 with UTC seconds in *out_unix. Pure — no EFI,
 * so the floor/timezone policy is host-unit-testable independent of GetTime. */
int     kl_wallclock_from_fields(const KlCivil *c, int tz_minutes, int tz_specified,
                                 int floor_year, int64_t *out_unix);

#endif /* KEEL_UEFI_CIVIL_TIME_H */
