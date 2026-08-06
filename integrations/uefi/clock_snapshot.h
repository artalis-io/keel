/*
 * clock_snapshot.h — the per-session UTC snapshot that backs mbedTLS's clock (U-8).
 *
 * Rationale (trust boundary): mbedTLS calls its time function repeatedly DURING certificate
 * verification. If that function called EFI GetTime each time, a GetTime that succeeded at
 * setup but failed or returned malformed data mid-handshake would fall back to epoch 0 and
 * could admit a certificate whose validity window spans 1970. So instead:
 *
 *   1. kl_uefi_clock_snapshot() validates + captures UTC ONCE, at TLS session creation,
 *      pairing it with the monotonic tick. It REFUSES (-1) if the clock is untrustworthy.
 *   2. kl_uefi_mbedtls_time() (the mbedTLS callback) NEVER calls GetTime — it returns the
 *      snapshot advanced by the monotonic clock. Monotonic cannot fail, so there is no
 *      mid-handshake failure path. With no valid snapshot it returns epoch 0 (fail-closed).
 *
 * The application must call kl_uefi_clock_snapshot() before creating each TLS session and
 * refuse the session if it returns -1.
 */
#ifndef KEEL_UEFI_CLOCK_SNAPSHOT_H
#define KEEL_UEFI_CLOCK_SNAPSHOT_H

/* Validate + capture the current UTC (via kl_uefi_wallclock) paired with the monotonic tick.
 * Returns 0 on success, -1 (and invalidates any prior snapshot) if the clock is untrustworthy.
 * Call once per TLS session, before session creation; refuse the session on -1. */
int kl_uefi_clock_snapshot(void);

/* Invalidate the snapshot (e.g. between sessions / for tests). After this, kl_uefi_mbedtls_time
 * returns epoch 0 until the next successful snapshot. */
void kl_uefi_clock_snapshot_reset(void);

/* mbedTLS time callback (MBEDTLS_PLATFORM_TIME_MACRO). Returns the snapshot UTC advanced by the
 * monotonic tick, or 0 (epoch, fail-closed) if there is no valid snapshot. Never calls GetTime. */
long long kl_uefi_mbedtls_time(long long *t);

#endif /* KEEL_UEFI_CLOCK_SNAPSHOT_H */
