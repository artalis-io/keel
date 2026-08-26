/*
 * clock_snapshot.c: see clock_snapshot.h. ONE TLS-platform-lifetime UTC snapshot + monotonic
 * advance that backs mbedTLS's clock. GetTime is consulted exactly once (captured inside
 * kl_uefi_mbedtls_platform_init()), never from the verification callback; the snapshot is shared
 * by all sessions, never refreshed while TLS is active, and cleared only at platform shutdown.
 * No mbedTLS / <time.h> dependency, so the host mock harness links and exercises this directly
 * (with a fake kl_uefi_wallclock).
 */
#include "clock_snapshot.h"
#include "platform_uefi.h"       /* kl_uefi_wallclock */
#include "../../../src/platform.h"  /* kl_monotonic_ms */

#include <stdint.h>

static int64_t  g_snap_utc;    /* captured UTC seconds */
static uint64_t g_snap_mono;   /* monotonic tick (ms) at capture */
static int      g_snap_valid;

int kl_uefi_clock_snapshot(void) {
    /* The snapshot is only meaningful if the monotonic clock ADVANCES (kl_uefi_mbedtls_time
     * advances the snapshot by it). If kl_uefi_platform_init() did not fully succeed, the timer
     * is not armed and the monotonic clock is frozen; capturing a snapshot then would freeze
     * cert time for the TLS-platform lifetime (accepting expired certs). Refuse. */
    if (!kl_uefi_platform_ready()) {
        g_snap_valid = 0;
        return -1;
    }
    int64_t utc;
    if (kl_uefi_wallclock(&utc) != 0) {   /* validates + floors + tz-normalises, fail-closed */
        g_snap_valid = 0;                 /* an untrustworthy capture invalidates the snapshot */
        return -1;
    }
    g_snap_utc  = utc;
    g_snap_mono = kl_monotonic_ms();
    g_snap_valid = 1;
    return 0;
}

void kl_uefi_clock_snapshot_reset(void) {
    g_snap_valid = 0;
    g_snap_utc = 0;
    g_snap_mono = 0;
}

long long kl_uefi_mbedtls_time(long long *t) {
    long long r;
    if (!g_snap_valid) {
        r = 0;   /* fail-closed: no trustworthy snapshot -> epoch -> certs fail notBefore */
    } else {
        uint64_t now = kl_monotonic_ms();
        uint64_t dms = (now >= g_snap_mono) ? (now - g_snap_mono) : 0;  /* monotonic; guard */
        r = (long long)(g_snap_utc + (int64_t)(dms / 1000));
    }
    if (t) *t = r;
    return r;
}
