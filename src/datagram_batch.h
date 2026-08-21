#ifndef KEEL_SRC_DATAGRAM_BATCH_H
#define KEEL_SRC_DATAGRAM_BATCH_H

/*
 * datagram_batch.h — INTERNAL layout of KlDatagramBatch (datagram M5). Not installed, not ABI-stable.
 * The public API is <keel/datagram_batch.h>; this header exists only so the wiring TUs (M5.2/M5.3) and
 * the whitebox tests can see the fields. See docs/datagram_m5_batch_extension_design.md.
 *
 * M5.1 carries the batch OBJECT lifecycle only — the preallocated storage + the ownership/direction/
 * gso_busy bookkeeping. The receive cursor (KlDgramRxSlot walk, GRO offset) and the GSO group record
 * are added by M5.3/M5.2.
 */

#include <keel/datagram_batch.h>   /* the public opaque type + KlDgramBatchDir */
#include <keel/socket_dgram.h>     /* KlDgramRxSlot, KlDatagramOps */
#include <keel/allocator.h>

#include <stddef.h>

struct KlDatagram;   /* the owning facade handle (keel/datagram_detail.h) */


struct KlDatagramBatch {
    struct KlDatagram   *owner;      /* the datagram this batch is bound to (E1 owner check) */
    KlDgramBatchDir      dir;        /* RECV / SEND / BOTH */
    /* Set at create (in datagram_batch.c) to the unconditional teardown. The FACADE reclaims a
     * core-adopted RECV batch by calling THIS pointer — an INDIRECT call, so the always-linked facade
     * (datagram.c) carries no static reference to datagram_batch.c (keeps the freestanding lib, which
     * excludes this extension TU, closed). */
    void               (*reclaim)(struct KlDatagramBatch *b);
    int                  gso_busy;   /* E1: a GSO group from this batch is in flight (free refused) */
    int                  n_slots;    /* validated > 0 */
    size_t               slot_bufsz; /* validated > 0; n_slots * slot_bufsz does not overflow */

    KlAllocator         *alloc;      /* borrowed from the owner (frees the blocks below) */
    const KlDatagramOps *ops;        /* the owner's provider datagram ops (batch-block alloc/free) */

    /* Receive side (RECV/BOTH). `rx_block` is the provider recvmmsg block, or NULL when RX_BATCH is
     * unsupported OR the provider's rx batch vtable is incomplete (a single-recv refill is the
     * fallback). `rx_slots` is the per-refill slot staging; `rx_slots_bytes` is its VALIDATED byte size
     * (computed with an overflow guard at create) so cleanup never recomputes unchecked arithmetic. */
    void                *rx_block;
    KlDgramRxSlot       *rx_slots;
    size_t               rx_slots_bytes;
    /* Fallback single-recv payload storage (`slot_bufsz` bytes), allocated ONLY when the recv side has
     * no provider rx_block (RX_BATCH absent) — one recv writes here, then rx_slots[0] points at it. */
    unsigned char       *rx_fallback;
    size_t               rx_fallback_bytes;

    /* M5.3 recv batch cursor (readiness; valid only after attach). One recv_batch refill fills
     * rx_slots[0..n_filled); the machine yields one logical datagram per view_pull. `i` is the next
     * slot, `seg_off` the byte offset within a GRO-coalesced slot i (mid-split). Retained across a
     * mid-buffer pause (held buffer, §5.3); discarded on stop/close. */
    int                  attached;   /* 1 once the core adopted this batch (recv attach) */
    int                  gro_active;  /* M5.3 §6.2: GRO split ON iff provider CAP_GRO AND accepted RX_GRO
                                       * — set at attach; when 0 the provider meta.gro_seg is IGNORED. */
    int                  n_filled;
    int                  cursor_i;
    size_t               seg_off;
    /* M5.3: the GRO delivery mode LATCHED when a slot is first entered (seg_off == 0) and kept until the
     * slot advances — so a callback that (un)registers on_recv_segments mid-split does not change the
     * mode for the remaining segments of the current slot. `slot_gro` = this slot's segment size (0 =
     * plain); `slot_split` = 1 to split into per-segment on_recv, 0 to deliver whole. */
    int                  slot_gro;
    int                  slot_split;

    /* Send side (SEND/BOTH). `tx_block` is the provider sendmmsg block, or NULL when TX_BATCH is
     * unsupported OR the provider's tx batch vtable is incomplete (a single-send loop is the fallback).
     * `tx_descs` is the neutral KlDgramTxDesc[] staging the send machine fills from the FIFO head-run
     * for one flush (input to send_batch / the portable loop); `tx_descs_bytes` is its validated size.
     * `gso_buf` is the copy-once GSO group buffer (used in M5.2b); `gso_bytes` is its VALIDATED byte
     * size (`n_slots * slot_bufsz`, overflow-guarded at create) — used for both the alloc and the free. */
    void                *tx_block;
    KlDgramTxDesc       *tx_descs;
    size_t               tx_descs_bytes;
    unsigned char       *gso_buf;
    size_t               gso_bytes;
};

#endif /* KEEL_SRC_DATAGRAM_BATCH_H */
