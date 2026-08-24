/*
 * datagram_batch.c — the batch / GSO / GRO extension object lifecycle.
 *
 * Scope: capability-gated, caller-preallocated batch create/free with overflow-safe allocation,
 * owner/direction binding, the completion-creation rules, and full allocation-failure unwind. The
 * send/recv wiring lives elsewhere.
 *
 * A KlDatagramBatch is layout-neutral (KlSockAddr-free) and touches no platform networking header — it
 * only drives the provider's KlDatagramOps batch-block allocators + the KlAllocator. It reads the
 * owning KlDatagram's borrowed handles (alloc / provider / provider_caps / completion) via the opt-in
 * facade layout.
 */

#include <keel/datagram_batch.h>
#include <keel/datagram_detail.h>   /* the KlDatagram facade layout (alloc/sockets/provider_caps/...) */
#include <keel/socket.h>            /* KlSocketProvider + KlDatagramOps + KL_DGRAM_CAP_* */
#include <keel/allocator.h>

#include "datagram_batch.h"         /* struct KlDatagramBatch */
#include "socket.h"                 /* kl_sockdef_dgram() */

#include <stddef.h>
#include <string.h>

/* The provider datagram ops for `dg` (its provider's, or the default POSIX ops). */
static const KlDatagramOps *batch_ops(const KlDatagram *dg) {
    return (dg->sockets && dg->sockets->dgram) ? dg->sockets->dgram : kl_sockdef_dgram();
}

/* A direction is NATIVELY batch-capable only when its COMPLETE op set exists — otherwise a `*_new`
 * block could be allocated with no `*_free` to release it (leak) or no `recv/send_batch` to use it.
 * A partial vtable (or a falsely-reported cap bit) ⇒ NULL block ⇒ the portable single-recv/-send
 * fallback. */
static int rx_batch_capable(const KlDatagramOps *ops) {
    return ops && ops->rx_batch_new && ops->rx_batch_free && ops->recv_batch;
}
static int tx_batch_capable(const KlDatagramOps *ops) {
    return ops && ops->tx_batch_new && ops->tx_batch_free && ops->send_batch;
}

/* Free every allocation a (partially-built) batch holds, then the batch. Safe on a memset-zero batch
 * (NULL blocks are skipped) — so it doubles as the allocation-failure unwind. Cleanup uses the VALIDATED
 * byte sizes stored at create (never recomputes unchecked arithmetic). A block is only present when its
 * direction was batch-capable, so its `*_batch_free` is guaranteed non-NULL. */
static void batch_destroy(KlDatagramBatch *b) {
    KlAllocator *a = b->alloc;
    if (b->rx_block) b->ops->rx_batch_free(a, b->rx_block);
    if (b->tx_block) b->ops->tx_batch_free(a, b->tx_block);
    if (b->rx_slots)    kl_free(a, b->rx_slots, b->rx_slots_bytes);
    if (b->rx_fallback) kl_free(a, b->rx_fallback, b->rx_fallback_bytes);
    if (b->tx_descs) kl_free(a, b->tx_descs, b->tx_descs_bytes);
    if (b->gso_buf)  kl_free(a, b->gso_buf, b->gso_bytes);
    kl_free(a, b, sizeof(*b));
}

KlDatagramBatch *kl_datagram_batch_create(KlDatagram *dg, KlDgramBatchDir dir,
                                          int n_slots, size_t slot_bufsz) {
    if (!dg || !dg->core) return NULL;
    if (dir != KL_DGRAM_BATCH_RECV && dir != KL_DGRAM_BATCH_SEND && dir != KL_DGRAM_BATCH_BOTH)
        return NULL;
    /* Allocation contract: positive sizes + overflow-safe products, ALL computed and guarded BEFORE any
     * allocation. Both the GSO buffer (n_slots * slot_bufsz) and the recv-slot staging (n_slots *
     * sizeof(KlDgramRxSlot)) are guarded — the latter matters on 32-bit targets where the payload
     * product alone does not protect it. */
    if (n_slots <= 0 || slot_bufsz == 0) return NULL;
    if ((size_t)n_slots > SIZE_MAX / slot_bufsz) return NULL;
    if ((size_t)n_slots > SIZE_MAX / sizeof(KlDgramRxSlot)) return NULL;
    if ((size_t)n_slots > SIZE_MAX / sizeof(KlDgramTxDesc)) return NULL;
    const size_t gso_bytes = (size_t)n_slots * slot_bufsz;
    const size_t rx_slots_bytes = (size_t)n_slots * sizeof(KlDgramRxSlot);
    const size_t tx_descs_bytes = (size_t)n_slots * sizeof(KlDgramTxDesc);

    const int want_recv = (dir & KL_DGRAM_BATCH_RECV) != 0;
    const int want_send = (dir & KL_DGRAM_BATCH_SEND) != 0;
    /* A receive direction (RECV or BOTH) requires a readiness datagram; SEND works on both. */
    if (dg->completion && want_recv) return NULL;

    KlAllocator *a = dg->alloc;
    const KlDatagramOps *ops = batch_ops(dg);

    KlDatagramBatch *b = kl_malloc(a, sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->owner = dg; b->dir = dir; b->n_slots = n_slots; b->slot_bufsz = slot_bufsz;
    b->rx_slots_bytes = rx_slots_bytes; b->tx_descs_bytes = tx_descs_bytes; b->gso_bytes = gso_bytes;
    b->alloc = a; b->ops = ops;
    b->reclaim = batch_destroy;   /* the facade's indirect RECV-batch teardown (keeps datagram.c
                                   * free of a static datagram_batch.c reference → freestanding-clean) */

    if (want_recv) {
        /* Provider recvmmsg block ONLY when RX_BATCH is reported AND the COMPLETE rx batch vtable exists
         * (else NULL ⇒ single-recv fallback — never a new-without-free leak). */
        if ((dg->provider_caps & KL_DGRAM_CAP_RX_BATCH) && rx_batch_capable(ops)) {
            b->rx_block = ops->rx_batch_new(a, n_slots, slot_bufsz);
            if (!b->rx_block) { batch_destroy(b); return NULL; }
        }
        b->rx_slots = kl_malloc(a, rx_slots_bytes);
        if (!b->rx_slots) { batch_destroy(b); return NULL; }
        if (!b->rx_block) {   /* no provider recvmmsg block ⇒ single-recv fallback needs payload storage */
            b->rx_fallback = kl_malloc(a, slot_bufsz);
            if (!b->rx_fallback) { batch_destroy(b); return NULL; }
            b->rx_fallback_bytes = slot_bufsz;
        }
    }
    if (want_send) {
        /* Provider sendmmsg block ONLY when TX_BATCH is reported AND the COMPLETE tx batch vtable exists
         * (else NULL ⇒ single-send fallback). */
        if ((dg->provider_caps & KL_DGRAM_CAP_TX_BATCH) && tx_batch_capable(ops)) {
            b->tx_block = ops->tx_batch_new(a, n_slots);
            if (!b->tx_block) { batch_destroy(b); return NULL; }
        }
        /* Neutral staging the send machine fills from the FIFO head-run for one flush (input to
         * send_batch / the portable loop). Always present for a send batch — independent of caps. */
        b->tx_descs = kl_malloc(a, tx_descs_bytes);
        if (!b->tx_descs) { batch_destroy(b); return NULL; }
        b->gso_buf = kl_malloc(a, gso_bytes);   /* copy-once GSO group buffer */
        if (!b->gso_buf) { batch_destroy(b); return NULL; }
    }
    return b;
}

int kl_datagram_batch_free(KlDatagramBatch *b) {
    if (!b) return 0;
    if (b->gso_busy) return -1;   /* E1: a GSO group still reads b's buffer asynchronously */
    batch_destroy(b);
    return 0;
}


/* kl_datagram_send_batch lives in datagram.c (the facade home) — it needs facade internals
 * (dg_reconcile_write to arm WRITE on a retained remainder) + the core send machine. */
