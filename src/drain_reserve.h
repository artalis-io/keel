/*
 * drain_reserve.h — INTERNAL. Phase-B KlDrain extension: a preallocated,
 * allocation-free write reservation + a low-water writable notification.
 *
 * This is the bounded-queue substrate the generic KlStream write path is built on
 * (docs/stream_contract.md §2, Findings 3/G): atomic all-or-none acceptance over a
 * capacity that is reserved at init, so kl_drain_reserve_write() performs NO allocation
 * and an oversized write is a permanent KL_DRAIN_TOO_LARGE rather than transient
 * backpressure. Kept internal (not in public <keel/drain.h>) until the Phase-B public
 * KlStream/KlListener surface is finalized. The extra state lives on KlDrain (drain.h),
 * dormant unless kl_drain_prealloc()/kl_drain_set_low_water() are called.
 */
#ifndef KEEL_SRC_DRAIN_RESERVE_H
#define KEEL_SRC_DRAIN_RESERVE_H

#include <keel/drain.h>

/* Result of a reserved (atomic) write — all-or-none acceptance. */
typedef enum {
    KL_DRAIN_ACCEPTED = 0,   /* all `len` bytes taken (sent inline and/or buffered) */
    KL_DRAIN_WOULD_BLOCK,    /* len <= capacity but insufficient free space now; NOTHING taken */
    KL_DRAIN_TOO_LARGE,      /* len > total capacity; permanent — caller must chunk */
    KL_DRAIN_WERROR          /* underlying writer failed / sticky error */
} KlDrainWriteStatus;

/* Preallocate the drain's buffer to a FIXED `capacity` bytes and enter reservation mode:
 * kl_drain_reserve_write() then never allocates (capacity is a check, not a realloc), and
 * `capacity` is the hard cap (sets max_size). One allocation, here, at init. Returns 0 on
 * success, -1 on allocation failure (the drain is left usable in non-prealloc mode).
 * capacity must be > 0. */
int kl_drain_prealloc(KlDrain *d, size_t capacity);

/* Atomic, allocation-free write against the reserved capacity (reservation mode only):
 *   - len == 0                                  -> KL_DRAIN_ACCEPTED (no-op)
 *   - len > total capacity                      -> KL_DRAIN_TOO_LARGE (permanent)
 *   - len > (capacity - buffered) i.e. no room  -> KL_DRAIN_WOULD_BLOCK (nothing taken)
 *   - otherwise: reserve, try a direct send of the prefix when the buffer is empty, copy
 *     the remainder into the reserved space                        -> KL_DRAIN_ACCEPTED
 * The remainder copy never allocates (space was reserved). Ordering is preserved: while the
 * buffer is non-empty the whole write is appended (no direct send). */
KlDrainWriteStatus kl_drain_reserve_write(KlDrain *d, const char *data, size_t len);

/* Set the low-water mark. When buffered output crosses from above to <= `low_water` (via
 * kl_drain_flush / kl_drain_consume), the on_writable callback fires ONCE for that crossing.
 * 0 disables it.
 *
 * Callback sequencing (both flush and consume): internal state is fully updated first, then
 * AT MOST ONE externally-reentrant callback fires. A low-water crossing (on_writable) takes
 * precedence over and SUPPRESSES on_drain for the same transition — a caller registering both
 * gets the writable signal, not a duplicate empty one. on_writable is a *writable* signal: it
 * MAY synchronously issue more writes (refilling the buffer) but MUST NOT free the drain (use
 * the close path for teardown); flush honors a refill by returning 1. on_drain (the legacy
 * empty callback) is treated as a destructive tail — nothing touches the drain after it. */
void kl_drain_set_low_water(KlDrain *d, size_t low_water);

/* Register the low-water writable callback (NULL to clear). Must not free the drain. */
void kl_drain_on_writable(KlDrain *d, KlDrainCb cb, void *ctx);

#endif /* KEEL_SRC_DRAIN_RESERVE_H */
