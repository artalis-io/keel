/*
 * wallclock_uefi.c: see wallclock_uefi.h. The EFI_TIME → UTC decode + unspecified-timezone
 * policy behind cert validity-time. Pure apart from the app-configured unspecified-TZ offset.
 */
#include "wallclock_uefi.h"
#include "platform_uefi.h"   /* KL_UEFI_TIME_FLOOR_YEAR */
#include "civil_time.h"

static int g_unspec_tz_min;   /* offset applied when the RTC reports unspecified TZ */
static int g_unspec_tz_set;

void kl_uefi_set_unspecified_tz(int minutes) { g_unspec_tz_min = minutes; g_unspec_tz_set = 1; }
void kl_uefi_clear_unspecified_tz(void)      { g_unspec_tz_min = 0;       g_unspec_tz_set = 0; }

int kl_uefi_wallclock_from_efi(int gettime_ok, const EFI_TIME *t, int64_t *out_unix) {
    /* Defensive: a reusable API must not dereference NULL, and its contract is to PRODUCE a
     * result; reject a NULL out rather than reporting success without one. */
    if (!gettime_ok || !t || !out_unix) return -1;

    KlCivil c = { (int)t->Year, (int)t->Month, (int)t->Day,
                  (int)t->Hour, (int)t->Minute, (int)t->Second };

    /* Resolve the timezone to use for local→UTC. */
    int tz_min, tz_use;
    if (t->TimeZone != (INT16)EFI_UNSPECIFIED_TIMEZONE) {
        tz_min = (int)t->TimeZone; tz_use = 1;          /* firmware gave an explicit offset */
    } else if (g_unspec_tz_set) {
        tz_min = g_unspec_tz_min;  tz_use = 1;          /* app-declared local offset */
    } else {
#ifdef KL_UEFI_ASSUME_UNSPECIFIED_UTC
        tz_min = 0; tz_use = 0;                          /* test/OVMF: RTC already reads UTC */
#else
        return -1;   /* fail-closed: unspecified zone = LOCAL time with unknown offset (§8.3) */
#endif
    }

    /* Reject malformed firmware fields BEFORE conversion (month 13, Feb 30, bad TZ/Daylight…). */
    if (kl_civil_valid(&c, (uint32_t)t->Nanosecond, tz_min, tz_use, (unsigned)t->Daylight) != 0)
        return -1;

    /* Sanity floor + local→UTC + convert (fail-closed on an untrustworthy year). */
    return kl_wallclock_from_fields(&c, tz_min, tz_use, KL_UEFI_TIME_FLOOR_YEAR, out_unix);
}
