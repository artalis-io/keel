#ifndef KEEL_SRC_DATAGRAM_SLOTS_H
#define KEEL_SRC_DATAGRAM_SLOTS_H

/*
 * datagram_slots.h — INTERNAL packet-slot storage for the datagram transport (Phase B, step 1).
 *
 * The frozen Tier-1 datagram contract (docs/datagram_contract.md §2/§3) requires bounded,
 * preallocated, allocation-free packet storage: a fixed number of OUTBOUND send slots plus ONE
 * dedicated INBOUND slot, kept separate so a full send queue never starves the posted receive.
 *
 * This module owns ONLY that storage and its invariants — it is NOT yet wired into the live
 * send/receive path (no queueing, pause, truncation, or send-status semantics here; those are
 * later Phase-B steps). The whole pool (outbound slot metadata + a free-list + every slot's
 * payload region, outbound and inbound) is ONE init-time allocation; acquire/release never touch
 * the allocator. Initialization is overflow-safe and unwinds completely on failure, leaving the
 * object zeroed and safely reusable.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include <keel/allocator.h>
#include <keel/sockaddr.h>

#include <stddef.h>

/* Per-datagram metadata flags carried in `KlDgramSlot.flags` and passed to the receive delivery
 * callback's `flags`. Providers set the raw hints (a completed WSAMSG.dwFlags MSG_TRUNC / WSAEMSGSIZE
 * on IOCP, msg_flags & MSG_TRUNC on recvmsg, an active pktinfo local); the receive machine (step 5)
 * canonicalizes them per the frozen contract (docs/datagram_contract.md §4/§8). */
#define KL_DGRAM_TRUNCATED  (1u << 0)  /* captured prefix: the datagram exceeded inbound capacity */
#define KL_DGRAM_HAS_LOCAL  (1u << 1)  /* `local` (destination / receiving interface) is valid */
/* (1u << 2) _BROADCAST, (1u << 3) _MULTICAST reserved for a later step. */

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
    int            in_use;    /* 1 while acquired (outbound); guards double-release */
    unsigned char *data;      /* payload region inside the pool block (never separately freed) */
} KlDgramSlot;

/* Fixed preallocated packet-slot pool: `slot_count` outbound slots (a reserve/release free-list)
 * plus one dedicated inbound slot, all backed by a SINGLE allocation. */
typedef struct {
    KlAllocator   *alloc;      /* borrowed; used only by init/free (never on acquire/release) */
    unsigned char *block;      /* the one allocation: slot array + free-list + all payloads */
    size_t         block_size; /* for kl_free */
    KlDgramSlot   *out;        /* outbound slot metadata array [slot_count] (into block) */
    size_t         slot_count; /* number of outbound slots */
    size_t         out_cap;    /* per-outbound-slot payload capacity (bytes) */
    size_t        *freelist;   /* [slot_count] free outbound indices, LIFO (into block) */
    size_t         free_n;     /* number of outbound slots currently free */
    KlDgramSlot    inbound;    /* the dedicated inbound slot (data → into block) */
    size_t         in_cap;     /* inbound payload capacity (bytes) */
} KlDgramSlots;

/* Allocate the pool in one block. Requires slot_count > 0, out_cap > 0, in_cap > 0. Returns 0 on
 * success; -1 on a bad argument, size-arithmetic overflow, or allocation failure — on any failure
 * the object is left zeroed (block == NULL) and safe to re-init. */
int  kl_dgram_slots_init(KlDgramSlots *s, KlAllocator *a,
                         size_t slot_count, size_t out_cap, size_t in_cap);

/* Free the pool block and zero the object (safe to re-init; idempotent; NULL-safe). */
void kl_dgram_slots_free(KlDgramSlots *s);

/* Reserve one outbound slot (its transient metadata reset: len 0, tos -1, flags 0, addrs zeroed).
 * Returns NULL when all `slot_count` outbound slots are in use. Never allocates. */
KlDgramSlot *kl_dgram_slots_acquire(KlDgramSlots *s);

/* Return a previously-acquired outbound slot to the pool. No-op if `slot` is NULL, not one of this
 * pool's outbound slots, or not currently in use (double-release is safe). */
void kl_dgram_slots_release(KlDgramSlots *s, KlDgramSlot *slot);

/* The dedicated inbound slot (always available; independent of outbound reservation). */
static inline KlDgramSlot *kl_dgram_slots_inbound(KlDgramSlots *s) { return &s->inbound; }

/* Number of outbound slots currently free (slot_count when idle, 0 when exhausted). */
static inline size_t kl_dgram_slots_free_count(const KlDgramSlots *s) { return s->free_n; }

#endif /* KEEL_SRC_DATAGRAM_SLOTS_H */
