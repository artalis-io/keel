/*
 * civil_time.h — pure proleptic-Gregorian civil-date ⇄ Unix-seconds conversion.
 *
 * No EFI, no mbedTLS, no libc — just integer math (Howard Hinnant's days_from_civil /
 * civil_from_days). Kept in its own TU so the error-prone leap-year / epoch arithmetic behind
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
 * else normalises local→UTC and returns 0 with UTC seconds in *out_unix. Per UEFI 2.10 §8.3,
 * @tz_minutes is the offset such that UTC = LocalTime + TimeZone (e.g. PST = +480), so we ADD
 * it (ignored unless @tz_specified). Pure — no EFI, so the floor/timezone policy is
 * host-unit-testable independent of GetTime. Assumes @c is already field-validated
 * (kl_civil_valid). */
int     kl_wallclock_from_fields(const KlCivil *c, int tz_minutes, int tz_specified,
                                 int floor_year, int64_t *out_unix);

/* Validate a decoded EFI wall-clock reading's fields against the UEFI 2.10 §8.3 permitted
 * ranges BEFORE conversion (malformed firmware values must be rejected, not normalised into
 * a different timestamp): Month 1..12, Day 1..(leap-aware days-in-month), Hour 0..23,
 * Minute/Second 0..59, @nanosecond 0..999,999,999, @tz_minutes -1440..1440 (when
 * @tz_specified), and @daylight only the defined bits (ADJUST_DAYLIGHT|IN_DAYLIGHT = 0x03;
 * reserved bits must be zero). @c->year must be 1900..9999. Returns 0 if valid, -1 otherwise.
 * Pure — host-unit-testable. */
int     kl_civil_valid(const KlCivil *c, uint32_t nanosecond,
                       int tz_minutes, int tz_specified, unsigned daylight);

#endif /* KEEL_UEFI_CIVIL_TIME_H */
