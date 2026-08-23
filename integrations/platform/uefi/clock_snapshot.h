/*
 * clock_snapshot.h — the TLS-platform-lifetime UTC snapshot that backs mbedTLS's clock (U-8).
 *
 * Rationale (trust boundary): mbedTLS's time callback is GLOBAL and has no per-connection
 * context, and Keel can interleave several async TLS sessions on one event loop. So a snapshot
 * cannot be "per session" — a second session's capture/failure would clobber a first session's
 * time basis (re-opening the epoch-0 / cert-spanning-1970 loophole). Instead there is ONE
 * snapshot for the whole TLS-platform lifetime:
 *
 *   1. kl_uefi_clock_snapshot() validates + captures UTC ONCE, from inside
 *      kl_uefi_mbedtls_platform_init() (the mandatory TLS bring-up). If it fails, TLS platform
 *      init fails and NO KlTlsCtx can be created — so enforcement does not depend on the
 *      application remembering an extra call.
 *   2. kl_uefi_mbedtls_time() (the mbedTLS callback) returns that snapshot advanced by the
 *      monotonic clock. It NEVER calls GetTime, and the snapshot is NEVER refreshed or
 *      invalidated while TLS is active — so a later GetTime failure cannot affect verification,
 *      and concurrent sessions all read the same forward-moving basis.
 *   3. kl_uefi_clock_snapshot_reset() clears it, called only from
 *      kl_uefi_mbedtls_platform_shutdown().
 *
 * A UEFI application's TLS-platform lifetime is boot → ExitBootServices, so a single snapshot +
 * monotonic advance is both concurrency-safe and accurate to well within cert day-granularity.
 */
#ifndef KEEL_UEFI_CLOCK_SNAPSHOT_H
#define KEEL_UEFI_CLOCK_SNAPSHOT_H

/* Validate + capture UTC (via kl_uefi_wallclock) paired with the monotonic tick. Returns 0 on
 * success, -1 (leaving no valid snapshot) if the clock is untrustworthy. Called once, from
 * kl_uefi_mbedtls_platform_init(); applications do NOT call this directly. */
int kl_uefi_clock_snapshot(void);

/* Clear the snapshot (kl_uefi_mbedtls_platform_shutdown / tests). After this,
 * kl_uefi_mbedtls_time returns epoch 0 until the next capture. */
void kl_uefi_clock_snapshot_reset(void);

/* mbedTLS time callback (MBEDTLS_PLATFORM_TIME_MACRO). Returns the snapshot UTC advanced by the
 * monotonic tick, or 0 (epoch, fail-closed) if there is no valid snapshot. Never calls GetTime. */
long long kl_uefi_mbedtls_time(long long *t);

#endif /* KEEL_UEFI_CLOCK_SNAPSHOT_H */
