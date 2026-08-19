#ifndef KEEL_SRC_DATAGRAM_SLOTS_H
#define KEEL_SRC_DATAGRAM_SLOTS_H

/*
 * datagram_slots.h — INTERNAL packet-slot storage for the datagram transport (Phase B).
 *
 * The frozen Tier-1 datagram contract (docs/datagram_contract.md §2/§3) requires bounded,
 * preallocated, allocation-free packet storage. Step-7A-1 (storage separation) splits this into two
 * INDEPENDENTLY-OWNED objects, matching the two lifetime owners the public KlDatagram needs
 * (docs/datagram_step7_public_api_design.md §5):
 *
 *   - KlDgramSlots   — the OUTBOUND send pool: a fixed number of equal-capacity slots + a free-list,
 *                      ONE init-time allocation, OBJECT-owned (freed at detachment). The send half is
 *                      payload-COPIED into backend op storage at submit, so the object may free this
 *                      pool once the single in-flight send retires.
 *   - KlDgramInbound — ONE dedicated inbound slot, its OWN allocation, LIFE-TOKEN-ownable (freed by
 *                      the B.6 token's on_final). The recv half is buffer-LENT (a backend writes into
 *                      it across the async op), so it must outlive any op that can be left dangling.
 *
 * Keeping them separate also means a full send pool never starves the posted receive. Both are
 * allocation-free after init; both init overflow-safe and unwind to a zeroed, reusable object.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include <keel/allocator.h>
#include <keel/sockaddr.h>
#include <keel/datagram.h>   /* KL_DGRAM_TRUNCATED / KL_DGRAM_HAS_LOCAL (promoted 7B-3) */

#include <stddef.h>

/* Per-datagram metadata flags (KL_DGRAM_TRUNCATED / KL_DGRAM_HAS_LOCAL) are carried in
 * `KlDgramSlot.flags` and passed to the receive delivery callback's `flags`. Providers set the raw
 * hints (a completed WSAMSG.dwFlags MSG_TRUNC / WSAEMSGSIZE on IOCP, msg_flags & MSG_TRUNC on recvmsg,
 * an active pktinfo local); the receive machine (step 5) canonicalizes them per the frozen contract
 * (docs/datagram_contract.md §4/§8). The flag definitions now live in <keel/datagram.h> (public).
 * (1u << 2) _BROADCAST, (1u << 3) _MULTICAST reserved for a later step. */

/* One packet slot: bounded payload storage (data points into the shared pool block) plus the
 * per-packet metadata the contract carries (peer/local addresses, length, flags, TOS). In step 1
 * these fields are STORAGE only — nothing interprets flags/tos yet. */
typedef struct {
    KlSockAddr     peer;      /* destination (outbound) / source (inbound) */
    KlSockAddr     local;     /* source-pin (outbound) / pktinfo dest (inbound) */
    size_t         len;       /* bytes currently held in `data` (0 when free/unused) */
    size_t         cap;       /* payload capacity of this slot (bytes) */
    int            tos;       /* per-packet TOS/ECN byte, or -1 (none) */
    unsigned       flags;     /* KL_DGRAM_* flags — storage only in step 1 */
    int            recoverable;/* M5.2a: outbound provenance — 1 = enqueued via a batch, so a hard send
                               * error DROPS this datagram (recoverable policy) instead of poisoning the
                               * queue with the sticky error. 0 = an ordinary single send (sticky). */
    /* M5.2b — GSO queued-group fields (outbound). A GSO request occupies `nseg` contiguous segment
     * slots; each references the caller-preallocated batch group buffer via `gso_ext` (NOT the pool
     * `data` — the payload is copied once into the group buffer, referenced until the group retires).
     * The FIRST segment carries the group record (`gso_head` + `gso_total`/`gso_seg`/`gso_mode`); the
     * LAST carries `gso_last` so its retirement clears the batch's gso_busy (on_gso_done). */
    const void    *gso_ext;   /* external segment payload (into the batch group buffer), or NULL */
    int            gso_head;   /* 1 = first segment: the whole-group send_gso submit point */
    int            gso_last;   /* 1 = last segment: its retire fires on_gso_done (clears gso_busy) */
    int            gso_mode;   /* head only: 0 = GSO (whole-buffer send_gso), 1 = FALLBACK (per-segment) */
    size_t         gso_total;  /* head only: whole GSO payload length (for the one-syscall send_gso) */
    size_t         gso_seg;    /* head only: segment size */
    void          *gso_owner;  /* head + last: the owning KlDatagramBatch (opaque; on_gso_done arg) */
    int            in_use;    /* 1 while acquired (outbound); guards double-release */
    unsigned char *data;      /* payload region inside the pool block (never separately freed) */
} KlDgramSlot;

/* ── Outbound send pool (OBJECT-owned) ─────────────────────────────────────────────────────────
 * `slot_count` equal-capacity outbound slots + a reserve/release free-list, ONE init-time
 * allocation. No inbound slot lives here (7A-1 storage separation). */
typedef struct {
    KlAllocator   *alloc;      /* borrowed; used only by init/free (never on acquire/release) */
    unsigned char *block;      /* the one allocation: slot array + free-list + outbound payloads */
    size_t         block_size; /* for kl_free */
    KlDgramSlot   *out;        /* outbound slot metadata array [slot_count] (into block) */
    size_t         slot_count; /* number of outbound slots */
    size_t         out_cap;    /* per-outbound-slot payload capacity (bytes) */
    size_t        *freelist;   /* [slot_count] free outbound indices, LIFO (into block) */
    size_t         free_n;     /* number of outbound slots currently free */
} KlDgramSlots;

/* Allocate the outbound pool in one block. Requires slot_count > 0, out_cap > 0. Returns 0 on
 * success; -1 on a bad argument, size-arithmetic overflow, or allocation failure — on any failure
 * the object is left zeroed (block == NULL) and safe to re-init. */
int  kl_dgram_slots_init(KlDgramSlots *s, KlAllocator *a, size_t slot_count, size_t out_cap);

/* Free the pool block and zero the object (safe to re-init; idempotent; NULL-safe). */
void kl_dgram_slots_free(KlDgramSlots *s);

/* Reserve one outbound slot (its transient metadata reset: len 0, tos -1, flags 0, addrs zeroed).
 * Returns NULL when all `slot_count` outbound slots are in use. Never allocates. */
KlDgramSlot *kl_dgram_slots_acquire(KlDgramSlots *s);

/* Return a previously-acquired outbound slot to the pool. No-op if `slot` is NULL, not one of this
 * pool's outbound slots, or not currently in use (double-release is safe). */
void kl_dgram_slots_release(KlDgramSlots *s, KlDgramSlot *slot);

/* Number of outbound slots currently free (slot_count when idle, 0 when exhausted). */
static inline size_t kl_dgram_slots_free_count(const KlDgramSlots *s) { return s->free_n; }

/* ── Inbound slot (LIFE-TOKEN-ownable) ─────────────────────────────────────────────────────────
 * ONE dedicated inbound slot with its OWN allocation, so a B.6 KlDgramLife on_final can free it
 * independently of (and outliving) the outbound pool + the KlUdp/KlUdpTransport wrapper. */
typedef struct {
    KlAllocator   *alloc;      /* borrowed; used only by init/free */
    unsigned char *block;      /* the one allocation: this slot's payload region */
    size_t         block_size; /* for kl_free */
    KlDgramSlot    slot;       /* the inbound slot (data → into block) */
} KlDgramInbound;

/* Allocate the inbound slot's payload. Requires cap > 0. Returns 0 on success; -1 on a bad
 * argument or allocation failure — object left zeroed (block == NULL), safe to re-init. */
int  kl_dgram_inbound_init(KlDgramInbound *in, KlAllocator *a, size_t cap);

/* Free the inbound payload and zero the object (safe to re-init; idempotent; NULL-safe). */
void kl_dgram_inbound_free(KlDgramInbound *in);

/* The inbound slot (always available). */
static inline KlDgramSlot *kl_dgram_inbound_slot(KlDgramInbound *in) { return &in->slot; }

#endif /* KEEL_SRC_DATAGRAM_SLOTS_H */
