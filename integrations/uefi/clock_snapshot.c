/*
 * clock_snapshot.c — see clock_snapshot.h. The per-session UTC snapshot + monotonic advance
 * that backs mbedTLS's clock, so GetTime is consulted exactly once (at snapshot), never from
 * the verification callback. No mbedTLS / <time.h> dependency, so the host mock harness links
 * and exercises this directly (with a fake kl_uefi_wallclock).
 */
#include "clock_snapshot.h"
#include "platform_uefi.h"       /* kl_uefi_wallclock */
#include "../../src/platform.h"  /* kl_monotonic_ms */

#include <stdint.h>

static int64_t  g_snap_utc;    /* captured UTC seconds */
static uint64_t g_snap_mono;   /* monotonic tick (ms) at capture */
static int      g_snap_valid;

int kl_uefi_clock_snapshot(void) {
    int64_t utc;
    if (kl_uefi_wallclock(&utc) != 0) {   /* validates + floors + tz-normalises, fail-closed */
        g_snap_valid = 0;                 /* an untrustworthy refresh invalidates the snapshot */
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
