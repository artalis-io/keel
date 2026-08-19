#ifndef KEEL_DATAGRAM_BATCH_H
#define KEEL_DATAGRAM_BATCH_H

/*
 * keel/datagram_batch.h — the OPTIONAL batch / GSO / GRO high-throughput extension for KlDatagram
 * (datagram M5). A strictly-additive layer ABOVE the single-datagram KlDatagram core; a consumer that
 * never includes this header is unaffected. See docs/datagram_m5_batch_extension_design.md.
 *
 * M5.1 (this increment) ships ONLY the capability-gated, caller-preallocated batch OBJECT lifecycle
 * (create/free) — no send/recv wiring yet. The send (kl_datagram_send_batch / _send_gso) and receive
 * (kl_datagram_recv_attach_batch / _recv_segments) APIs land in M5.2 / M5.3.
 *
 * Availability of the fast paths is provider support (kl_datagram_provider_caps): KL_DGRAM_CAP_RX_BATCH
 * / _TX_BATCH / _GSO / _GRO. Creation NEVER fails on capability absence — a missing provider block just
 * means the eventual send/recv takes a portable fallback; create fails only on invalid sizes, a
 * completion datagram requesting a receive direction, or OOM (§6.4).
 */

#include <keel/datagram.h>   /* KlDatagram, the KL_DGRAM_CAP_* bits */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An opaque, caller-preallocated batch context bound to one KlDatagram. */
typedef struct KlDatagramBatch KlDatagramBatch;

/** @brief The direction(s) a batch serves. RECV requires a readiness datagram; SEND works on both
 *  readiness and completion; BOTH is refused on a completion datagram (create SEND explicitly). */
typedef enum {
    KL_DGRAM_BATCH_RECV = 1,
    KL_DGRAM_BATCH_SEND = 2,
    KL_DGRAM_BATCH_BOTH = 3
} KlDgramBatchDir;

/**
 * @brief Create a batch bound to @p dg, sized for @p n_slots datagrams of up to @p slot_bufsz payload.
 *
 * Allocates ONCE (no data-path allocation): the batch object, the provider recvmmsg/sendmmsg block for
 * each requested-and-SUPPORTED direction (absent ⇒ NULL ⇒ portable fallback later), the receive slot
 * staging (RECV/BOTH), and the GSO group buffer of `n_slots * slot_bufsz` (SEND/BOTH).
 *
 * @return the batch, or NULL if: @p dg is invalid; @p dir is out of range; @p n_slots <= 0 or
 *  @p slot_bufsz == 0 or `n_slots * slot_bufsz` overflows; @p dg is a completion datagram and @p dir
 *  includes a receive direction (RECV or BOTH); or an allocation failed (every prior allocation is
 *  unwound — no leak). Capability absence alone NEVER fails creation.
 */
KlDatagramBatch *kl_datagram_batch_create(KlDatagram *dg, KlDgramBatchDir dir,
                                          int n_slots, size_t slot_bufsz);

/**
 * @brief Free a caller-owned batch (a NULL @p b is a no-op).
 *
 * Returns 0 on success, or -1 while a GSO group from @p b is still in flight (it reads the group
 * buffer asynchronously — M5.2 §4.5). NEVER call on a batch consumed by a successful
 * kl_datagram_recv_attach_batch (the core owns it — M5.3 §5.4).
 */
int kl_datagram_batch_free(KlDatagramBatch *b);

/**
 * @brief Send up to @p n datagrams through the core send queue as one transaction (M5.2a).
 *
 * Admits an accepted prefix of @p descs into the datagram's send queue (each datagram atomically
 * accepted or refused, subject to the configured slot-count / byte backpressure), then drains once —
 * on a readiness datagram via one sendmmsg over the head-run (or a portable single-send loop when
 * TX_BATCH is absent), on a completion datagram via the single-flight pump. @p b must be a SEND/BOTH
 * batch owned by @p dg (else -1 with @p stop = KL_DATAGRAM_ERROR).
 *
 * @return the number of datagrams ACCEPTED (0..n; the caller retains descs[accepted..n)), or -1 on a
 *  bad argument. On return @p stop (if non-NULL) classifies descs[accepted]: KL_DATAGRAM_ACCEPTED if
 *  all @p n were taken, else the refusal (WOULD_BLOCK = retry later / TOO_LARGE = permanent / ...).
 *  A per-datagram hard error while draining drops that datagram and is reported via
 *  kl_datagram_last_error — it does not fail the call.
 */
int kl_datagram_send_batch(KlDatagram *dg, KlDatagramBatch *b, const KlDgramTxDesc *descs, int n,
                           KlDatagramSendStatus *stop);

#ifdef __cplusplus
}
#endif

#endif /* KEEL_DATAGRAM_BATCH_H */
