/*
 * civil_time.c: see civil_time.h. Howard Hinnant's public-domain algorithms
 * (http://howardhinnant.github.io/date_algorithms.html), which are exact for the
 * proleptic Gregorian calendar over the entire int64 range (no libc, no tables).
 */
#include "civil_time.h"

int64_t kl_civil_to_unix(const KlCivil *c) {
    int64_t y = c->year;
    unsigned m = (unsigned)c->mon;
    unsigned d = (unsigned)c->day;

    /* days_from_civil: days since 1970-01-01 (may be negative). */
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                 /* [0, 399]           */
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u; /* [0, 365] */
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;  /* [0, 146096]        */
    int64_t days = era * 146097 + (int64_t)doe - 719468;

    return days * 86400
         + (int64_t)c->hour * 3600
         + (int64_t)c->min * 60
         + (int64_t)c->sec;
}

void kl_unix_to_civil(int64_t unix_sec, KlCivil *out) {
    /* Split into whole days + intra-day seconds, flooring toward negative infinity. */
    int64_t days = unix_sec / 86400;
    int64_t rem  = unix_sec % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }

    out->hour = (int)(rem / 3600);
    out->min  = (int)((rem % 3600) / 60);
    out->sec  = (int)(rem % 60);

    /* civil_from_days. */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                     /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u; /* [0,399] */
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);       /* [0, 365]    */
    unsigned mp = (5u * doy + 2u) / 153u;                            /* [0, 11]     */
    out->day  = (int)(doy - (153u * mp + 2u) / 5u + 1u);            /* [1, 31]     */
    out->mon  = (int)(mp < 10u ? mp + 3u : mp - 9u);               /* [1, 12]     */
    out->year = (int)(y + (out->mon <= 2));
}

int kl_wallclock_from_fields(const KlCivil *c, int tz_minutes, int tz_specified,
                             int floor_year, int64_t *out_unix) {
    if (c->year < floor_year) return -1;             /* fail-closed: untrustworthy RTC */
    int64_t u = kl_civil_to_unix(c);
    /* UEFI §8.3: UTC = LocalTime + TimeZone (TimeZone is the offset from UTC in minutes,
     * e.g. PST = +480). So ADD it to recover UTC from the stored local time. */
    if (tz_specified) u += (int64_t)tz_minutes * 60;
    if (out_unix) *out_unix = u;
    return 0;
}

static int days_in_month(int year, int mon) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon < 1 || mon > 12) return 0;
    if (mon == 2) {
        int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        return leap ? 29 : 28;
    }
    return d[mon - 1];
}

int kl_civil_valid(const KlCivil *c, uint32_t nanosecond,
                   int tz_minutes, int tz_specified, unsigned daylight) {
    if (c->year < 1900 || c->year > 9999) return -1;
    int dim = days_in_month(c->year, c->mon);          /* 0 if month out of range */
    if (dim == 0) return -1;
    if (c->day  < 1 || c->day  > dim) return -1;        /* leap-year-aware */
    if (c->hour < 0 || c->hour > 23)  return -1;
    if (c->min  < 0 || c->min  > 59)  return -1;
    if (c->sec  < 0 || c->sec  > 59)  return -1;        /* UEFI §8.3: Second 0..59 */
    if (nanosecond > 999999999u)      return -1;
    if (tz_specified && (tz_minutes < -1440 || tz_minutes > 1440)) return -1;
    if (daylight & ~0x03u)            return -1;        /* only ADJUST_DAYLIGHT|IN_DAYLIGHT */
    return 0;
}
