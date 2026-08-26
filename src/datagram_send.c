/*
 * datagram_send.c: INTERNAL atomic send machinery over KlDgramSlots.
 *
 * See datagram_send.h. Single-flight (Tier-1): the oldest queued datagram is the only one
 * submitted; the next is pumped when it retires. The submit hook is armed BEFORE it is called so an
 * inline kl_dgram_send_on_complete retires correctly. All state is updated before callbacks, which
 * are deferred to the outermost public call; on_writable is reentrant/non-destructive, on_drain is
 * the destructive tail (re-checked, then no self-access).
 */
#include "datagram_send.h"

#include <stdint.h>   /* SIZE_MAX */
#include <string.h>   /* memcpy / memset */

static const KlSockAddr *addr_or_null(const KlSockAddr *a) {
    return (a && kl_sockaddr_family(a) != KL_AF_UNSPEC) ? a : NULL;
}

int kl_dgram_send_init(KlDgramSend *s, KlDgramSlots *slots, KlAllocator *ring_alloc,
                       int completion, unsigned caps, size_t byte_budget,
                       KlDgramSubmitFn submit, void *submit_ctx) {
    if (!s)
        return -1;
    memset(s, 0, sizeof(*s));
    if (!slots || !ring_alloc || !submit || slots->slot_count == 0)
        return -1;

    size_t cap = slots->slot_count;
    if (cap > SIZE_MAX / sizeof(KlDgramSlot *))   /* overflow-safe ring sizing */
        return -1;
    size_t ring_bytes = cap * sizeof(KlDgramSlot *);

    KlDgramSlot **ring = kl_malloc(ring_alloc, ring_bytes);
    if (!ring)
        return -1;
    memset(ring, 0, ring_bytes);

    s->slots      = slots;
    s->alloc      = ring_alloc;
    s->ring       = ring;
    s->ring_cap   = cap;
    s->completion = completion ? 1 : 0;
    s->caps       = caps;
    s->byte_budget = byte_budget;   /* 0 = SLOT policy (gate off); >0 = BOTH */
    s->submit     = submit;
    s->submit_ctx = submit_ctx;
    return 0;
}

void kl_dgram_send_set_writable_cb(KlDgramSend *s, KlDgramWritableFn cb, void *ctx) {
    if (!s) return;
    s->on_writable = cb; s->writable_ctx = ctx;
}
void kl_dgram_send_set_drain_cb(KlDgramSend *s, KlDgramDrainFn cb, void *ctx) {
    if (!s) return;
    s->on_drain = cb; s->drain_ctx = ctx;
}
void kl_dgram_send_set_drop_cb(KlDgramSend *s, KlDgramDropFn cb, void *ctx) {
    if (!s) return;
    s->on_drop = cb; s->drop_ctx = ctx;
}
void kl_dgram_send_set_gso_cbs(KlDgramSend *s, KlDgramSubmitGsoFn submit_gso, void *submit_ctx,
                               KlDgramGsoDoneFn on_gso_done, void *done_ctx) {
    if (!s) return;
    s->submit_gso = submit_gso; s->submit_gso_ctx = submit_ctx;
    s->on_gso_done = on_gso_done; s->gso_done_ctx = done_ctx;
}
void kl_dgram_send_set_closing(KlDgramSend *s, int closing) {
    if (s) s->closing = closing ? 1 : 0;
}
void kl_dgram_send_set_connected(KlDgramSend *s, int on) {   /* set after a successful connect */
    if (s) s->connected = on ? 1 : 0;
}
void kl_dgram_send_set_activity_cb(KlDgramSend *s, void (*cb)(void *ctx, int delta), void *ctx) {
    if (!s) return;
    s->on_activity = cb; s->activity_ctx = ctx;
}
static void send_enter(KlDgramSend *s) { if (s->on_activity) s->on_activity(s->activity_ctx, +1); }
/* send_leave is the LAST action of a public op; it may detach + free the object via the coordinator. */
static void send_leave(KlDgramSend *s) { if (s->on_activity) s->on_activity(s->activity_ctx, -1); }

void kl_dgram_send_discard_queued(KlDgramSend *s) {
    if (!s) return;
    /* Release every queued-but-unsubmitted slot (from the tail), keeping the in-flight head prefix. */
    while (s->count > s->inflight_n) {
        size_t idx = (s->head + s->count - 1) % s->ring_cap;
        KlDgramSlot *slot = s->ring[idx];
        s->ring[idx] = NULL;
        if (s->bytes_used >= slot->len) s->bytes_used -= slot->len;   /* release only the QUEUED slots */
        else                            s->bytes_used = 0;
        int   gso_last  = slot->gso_last;    /* discarding a group's last segment frees its buffer */
        void *gso_owner = slot->gso_owner;
        kl_dgram_slots_release(s->slots, slot);
        s->count--;
        if (gso_last && s->on_gso_done) s->on_gso_done(s->gso_done_ctx, gso_owner);   /* clears gso_busy */
    }
    /* The in-flight head (if any) is retained, so bytes_used now == its len (== 0 when nothing is in
     * flight); it drops to 0 only when the terminal completion retires it. */
    s->full = 0;   /* slots freed; no on_writable; this is close-initiated */
}

/* ── Deferred callbacks (depth-guarded; reentrancy-safe) ─────────────────────────────────────
 * dispatch_enter/leave bracket any public call that can retire a slot. Callbacks fire only at the
 * outermost frame, so a reentrant send from a callback defers ITS callbacks back here. */
static void dispatch_enter(KlDgramSend *s) { s->in_dispatch++; }

static void dispatch_leave(KlDgramSend *s) {
    if (s->in_dispatch > 1) { s->in_dispatch--; return; }   /* nested; defer to the outer frame */

    /* on_writable is reentrant/non-destructive: fire while depth is still 1 (so a reentrant send
     * defers its callbacks back to us), looping to absorb a refill+re-drain it may cause. */
    while (s->pend_writable) {
        s->pend_writable = 0;
        if (s->on_writable) s->on_writable(s->writable_ctx);
    }

    /* on_drain is the DESTRUCTIVE tail: fire only if the queue is STILL empty (a reentrant
     * on_writable send may have refilled it). Drop to depth 0 BEFORE firing and make no further
     * access to `s`; the callback may free/tear down the object. */
    int fire_drain = (s->pend_drain && s->count == 0 && s->on_drain != NULL);
    s->pend_drain = 0;
    s->in_dispatch = 0;
    if (fire_drain)
        s->on_drain(s->drain_ctx);   /* NO access to `s` after this line */
}

/* Pop+release the oldest occupied slot; update FIFO + inflight + edge flags. No callbacks here;
 * they are deferred (dispatch_leave), so a caller can update a sticky error before they fire. */
static void send_retire_head(KlDgramSend *s, int was_inflight) {
    KlDgramSlot *slot = s->ring[s->head];
    int   gso_last  = slot->gso_last;      /* capture before release (slot returns to the pool) */
    void *gso_owner = slot->gso_owner;
    s->ring[s->head] = NULL;
    s->head = (s->head + 1) % s->ring_cap;
    s->count--;
    if (was_inflight && s->inflight_n > 0)
        s->inflight_n--;
    if (s->bytes_used >= slot->len) s->bytes_used -= slot->len;   /* byte-gate release (both retire paths) */
    else                            s->bytes_used = 0;            /* defensive; invariant keeps ≥ */
    kl_dgram_slots_release(s->slots, slot);   /* a free slot appears */
    if (s->full) { s->full = 0; s->pend_writable = 1; }   /* full → non-full */
    if (s->count == 0) s->pend_drain = 1;                 /* non-empty → empty (re-checked at leave) */
    if (gso_last && s->on_gso_done)                       /* the GSO group's last segment retired */
        s->on_gso_done(s->gso_done_ctx, gso_owner);       /* clears the batch's gso_busy (non-destructive) */
}

/* The recoverable per-datagram send-error policy: the head hard-errored but is a batch-enqueued
 * (recoverable) datagram: drop it (release the slot + FIFO/edge accounting via send_retire_head), count
 * it, and report it (on_drop), WITHOUT setting the sticky `err`. Used by every drain path (flush_batch
 * isolation, the readiness pump, and completion on_complete) so a batch slot keeps its recoverable
 * provenance wherever it is finally submitted. */
static void send_drop_head(KlDgramSend *s, int was_inflight) {
    send_retire_head(s, was_inflight);
    s->dropped++;
    if (s->on_drop) s->on_drop(s->drop_ctx);
}

/* Submit the queued slot at FIFO position `off` from the head. A GSO segment sends from the batch group
 * buffer (`gso_ext`), an ordinary datagram from the copied pool payload (`data`). */
static KlDgramSubmitResult send_submit_slot(KlDgramSend *s, size_t off) {
    KlDgramSlot *slot = s->ring[(s->head + off) % s->ring_cap];
    const void *data = slot->gso_ext ? slot->gso_ext : slot->data;
    return s->submit(s->submit_ctx, data, slot->len,
                     addr_or_null(&slot->peer), addr_or_null(&slot->local), slot->tos);
}

/* Retire the whole contiguous GSO group at the head (head .. gso_last), applying `fn` to each slot
 * (send_retire_head for a clean retire, send_drop_head for a recoverable whole-group drop). */
static void send_retire_gso_group(KlDgramSend *s, void (*fn)(KlDgramSend *, int)) {
    for (;;) {
        int last = s->ring[s->head]->gso_last;
        fn(s, 0);
        if (last || s->count == 0) break;
    }
}

/* If the FIFO head is a GSO group in GSO mode, submit the WHOLE group in one send_gso (readiness) and
 * handle the outcome. Returns 1 if handled (caller continues its drain), 0 if the head is not a
 * GSO-mode group. On WOULD_BLOCK sets *retain = 1 (the caller stops draining and re-arms). */
static int send_gso_head(KlDgramSend *s, int *retain) {
    KlDgramSlot *head = s->ring[s->head];
    if (!head->gso_head || head->gso_mode != 0 || !s->submit_gso)
        return 0;
    KlDgramSubmitResult r = s->submit_gso(s->submit_gso_ctx, head->gso_owner, head->gso_ext,
                                          head->gso_total, head->gso_seg, addr_or_null(&head->peer), head->tos);
    if (r == KL_DGRAM_SUBMIT_DONE) {
        send_retire_gso_group(s, send_retire_head);          /* whole group sent → retire atomically */
    } else if (r == KL_DGRAM_SUBMIT_WOULDBLOCK) {
        *retain = 1;                                          /* retain the whole group; re-arm WRITE */
    } else if (r == KL_DGRAM_SUBMIT_UNSUPPORTED) {
        head->gso_mode = 1;                                   /* latch/fallback: segments drain per-send */
    } else {                                                  /* hard error → recoverable whole-group drop */
        send_retire_gso_group(s, send_drop_head);
    }
    return 1;
}

/* Single-flight pump: while nothing is in flight and a queued datagram exists, submit the head.
 * The op is ARMED (inflight_n = 1) before submit so an inline on_complete retires it. Returns 0, or
 * -1 once a sticky error is set. Must be called inside a dispatch. */
static int send_pump(KlDgramSend *s) {
    if (s->err) return -1;
    while (s->inflight_n == 0 && s->count > 0) {
        int retain = 0;
        if (send_gso_head(s, &retain)) {   /* GSO-mode group head → one whole send_gso (readiness) */
            if (retain) return 0;          /* WOULD_BLOCK; keep the group queued */
            continue;                      /* retired / dropped / switched-to-fallback → next head */
        }
        s->inflight_n     = 1;    /* arm before submit (inline-completion window) */
        s->in_submit      = 1;
        s->submit_retired = 0;
        KlDgramSubmitResult r = send_submit_slot(s, 0);   /* head is the only un-submitted slot */
        s->in_submit = 0;

        if (s->submit_retired) {
            /* an inline on_complete already retired this op (inflight_n back to 0, head popped). */
            if (r == KL_DGRAM_SUBMIT_ERROR) { s->err = 1; return -1; }
            continue;             /* submit the next queued datagram, if any */
        }
        if (r == KL_DGRAM_SUBMIT_INFLIGHT) {
            return 0;             /* stays in flight (inflight_n == 1); retire via on_complete */
        } else if (r == KL_DGRAM_SUBMIT_DONE) {
            s->inflight_n = 0;    /* not async; un-arm, then retire the head synchronously */
            send_retire_head(s, 0);
        } else if (r == KL_DGRAM_SUBMIT_WOULDBLOCK) {
            s->inflight_n = 0;    /* un-arm; keep queued (a writable event resumes via flush) */
            return 0;
        } else {                  /* KL_DGRAM_SUBMIT_ERROR */
            s->inflight_n = 0;    /* un-arm */
            if (s->ring[s->head]->recoverable) {
                send_drop_head(s, 0);   /* batch provenance: drop this one, keep draining the rest */
                continue;
            }
            s->err = 1;           /* ordinary single send: sticky; datagram retained (owned) in its slot */
            return -1;
        }
    }
    return 0;
}

/* Up-front validation shared by kl_dgram_send + kl_dgram_send_enqueue. Returns KL_DATAGRAM_ACCEPTED if
 * the datagram may proceed to admission, else the refusal status (UNSUPPORTED / TOO_LARGE / ...). */
static KlDatagramSendStatus send_validate(const KlDgramSend *s, const KlDatagramMessage *m) {
    if (!s || !m || !s->submit)
        return KL_DATAGRAM_ERROR;
    if (s->err)
        return KL_DATAGRAM_ERROR;                  /* sticky */
    if (s->closing)
        return KL_DATAGRAM_CLOSED;
    if (m->len && !m->data)
        return KL_DATAGRAM_ERROR;
    /* Requested-but-unsupported features fail; nothing is taken. */
    if (addr_or_null(m->local) && !(s->caps & KL_DGRAM_CAP_SOURCE_PIN))
        return KL_DATAGRAM_UNSUPPORTED;
    if (m->tos >= 0 && !(s->caps & KL_DGRAM_CAP_TOS))
        return KL_DATAGRAM_UNSUPPORTED;
    /* Peerless (connected-mode) send: needs BOTH the granted CAP_CONNECTED AND an ACTUAL successful
     * connect. Shared by kl_dgram_send AND kl_dgram_send_enqueue (batch), so a peerless send can
     * never reach the provider before kl_datagram_connect. */
    if (!addr_or_null(m->peer) && !((s->caps & KL_DGRAM_CAP_CONNECTED) && s->connected))
        return KL_DATAGRAM_UNSUPPORTED;            /* connected-mode send unsupported / not yet connected */
    /* Permanent: a datagram that can never fit one slot. Nothing taken. */
    if (m->len > s->slots->out_cap)
        return KL_DATAGRAM_TOO_LARGE;
    return KL_DATAGRAM_ACCEPTED;
}

/* Byte-admission gate (BOTH policy) + slot acquire + copy + FIFO enqueue. NO fast path, NO submit,
 * NO callbacks. Assumes send_validate already passed. `recoverable` stamps the slot's send-error
 * provenance (1 = a batch datagram → drop on hard error; 0 = single send → sticky). Returns ACCEPTED /
 * WOULD_BLOCK / TOO_LARGE. */
static KlDatagramSendStatus send_admit(KlDgramSend *s, const KlDatagramMessage *m, int recoverable) {
    /* Byte-admission gate (inert when byte_budget == 0). Two outcomes, split on whether the datagram
     * ALONE exceeds the whole budget. */
    if (s->byte_budget) {
        if (m->len > s->byte_budget)
            return KL_DATAGRAM_TOO_LARGE;          /* (a) exceeds the whole budget → permanent */
        if (m->len > s->byte_budget - s->bytes_used) {   /* overflow-safe (bytes_used ≤ budget) */
            s->full = 1;                           /* (b) transient: arm full→non-full */
            return KL_DATAGRAM_WOULD_BLOCK;
        }
    }
    /* Acquire a slot only once acceptance is certain; every refusal takes no ownership and mutates no
     * queue state. */
    KlDgramSlot *slot = kl_dgram_slots_acquire(s->slots);
    if (!slot) {
        s->full = 1;                               /* full: arm full→non-full */
        return KL_DATAGRAM_WOULD_BLOCK;            /* transient; nothing taken */
    }
    /* Copy the whole datagram + its metadata before returning (caller owns nothing after). */
    if (m->len)
        memcpy(slot->data, m->data, m->len);
    slot->len   = m->len;
    slot->tos   = m->tos;
    slot->flags = m->flags;
    slot->recoverable = recoverable;
    if (addr_or_null(m->peer))  slot->peer  = *m->peer;  else memset(&slot->peer,  0, sizeof(slot->peer));
    if (addr_or_null(m->local)) slot->local = *m->local; else memset(&slot->local, 0, sizeof(slot->local));
    s->ring[(s->head + s->count) % s->ring_cap] = slot;   /* enqueue FIFO (tail) */
    s->count++;
    s->bytes_used += m->len;                       /* byte-gate accounting (paired with every release) */
    if (kl_dgram_slots_free_count(s->slots) == 0)
        s->full = 1;                               /* filled the last slot */
    return KL_DATAGRAM_ACCEPTED;
}

KlDatagramSendStatus kl_dgram_send(KlDgramSend *s, const KlDatagramMessage *m) {
    KlDatagramSendStatus v = send_validate(s, m);
    if (v != KL_DATAGRAM_ACCEPTED)
        return v;

    /* From here a provider submit hook OR a deferred callback may run and may REENTRANTLY request
     * close; hold ONE busy frame across the whole tail so detachment is deferred to the final leave
     * (which may free s; nothing may touch s after it). The readiness direct-submit participates in
     * the handshake like every other provider frame. */
    KlDatagramSendStatus status = KL_DATAGRAM_ACCEPTED;
    send_enter(s);

    /* Readiness fast path: an empty queue tries a direct synchronous send with NO slot consumed, so
     * a synchronously-sent datagram never creates a transient non-empty→empty transition. A
     * readiness provider returns DONE / WOULD_BLOCK / ERROR (never INFLIGHT). */
    if (!s->completion && s->count == 0) {
        KlDgramSubmitResult r = s->submit(s->submit_ctx, m->data, m->len,
                                          addr_or_null(m->peer), addr_or_null(m->local), m->tos);
        if (r == KL_DGRAM_SUBMIT_DONE)  { status = KL_DATAGRAM_ACCEPTED; goto leave; }
        if (r == KL_DGRAM_SUBMIT_ERROR) { s->err = 1; status = KL_DATAGRAM_ERROR; goto leave; }
        /* WOULD_BLOCK: queue it below, UNLESS the hook reentrantly errored / began closing (still
         * inside our frame, so detachment is deferred). NEVER enqueue after closing began. */
        if (s->err)     { status = KL_DATAGRAM_ERROR;  goto leave; }
        if (s->closing) { status = KL_DATAGRAM_CLOSED; goto leave; }
    }

    status = send_admit(s, m, 0);   /* ordinary single send → sticky on hard error */
    if (status == KL_DATAGRAM_ACCEPTED) {
        dispatch_enter(s);
        (void)send_pump(s);                        /* submit failure is sticky; the datagram stays owned */
        dispatch_leave(s);                         /* fires on_drain (non-destructive under a coordinator) */
    }

leave:
    send_leave(s);                                 /* LAST action; may detach + free s via the coordinator */
    return status;                                 /* local value; safe even if s was freed */
}

/* Enqueue-only: validate + admit, NO fast path, NO pump, NO callbacks. */
KlDatagramSendStatus kl_dgram_send_enqueue(KlDgramSend *s, const KlDatagramMessage *m) {
    KlDatagramSendStatus v = send_validate(s, m);
    if (v != KL_DATAGRAM_ACCEPTED)
        return v;
    return send_admit(s, m, 1);   /* batch datagram → recoverable (drop) on hard error */
}

/* Atomically admit a GSO group of `nseg` contiguous segment slots (all referencing `buf`). No
 * fast path, no submit, no callbacks. */
KlDatagramSendStatus kl_dgram_send_enqueue_gso(KlDgramSend *s, const void *buf, size_t total,
                                               size_t seg, size_t nseg, const KlSockAddr *peer, int tos,
                                               int mode, void *owner) {
    if (!s || !s->submit || nseg == 0 || seg == 0)
        return KL_DATAGRAM_ERROR;
    if (s->err)     return KL_DATAGRAM_ERROR;
    if (s->closing) return KL_DATAGRAM_CLOSED;
    /* Requested-but-unsupported features fail (matching send_validate). A GSO group carries no
     * per-packet source-pin / TOS (the public API has none), so only connected-mode applies: a
     * NULL/UNSPEC peer requests connected send, which must be granted. Nothing taken. */
    if (!addr_or_null(peer) && !((s->caps & KL_DGRAM_CAP_CONNECTED) && s->connected))
        return KL_DATAGRAM_UNSUPPORTED;   /* connected-mode GSO: cap granted AND actually connected */
    /* Permanent: more segments than the whole slot pool, or over the whole byte budget. */
    if (nseg > s->ring_cap)                            return KL_DATAGRAM_TOO_LARGE;
    if (s->byte_budget && total > s->byte_budget)      return KL_DATAGRAM_TOO_LARGE;
    /* Transient (all-or-nothing): not enough free slots now, or the byte gate. */
    if (kl_dgram_slots_free_count(s->slots) < nseg) { s->full = 1; return KL_DATAGRAM_WOULD_BLOCK; }
    if (s->byte_budget && total > s->byte_budget - s->bytes_used) {   /* overflow-safe */
        s->full = 1;
        return KL_DATAGRAM_WOULD_BLOCK;
    }
    /* Acquire + enqueue nseg segment slots; each references `buf` at its offset (nothing copied). */
    for (size_t i = 0; i < nseg; i++) {
        KlDgramSlot *slot = kl_dgram_slots_acquire(s->slots);   /* guaranteed: free_n >= nseg checked */
        size_t off  = i * seg;
        size_t slen = (total > off) ? (total - off) : 0;
        if (slen > seg) slen = seg;
        /* `buf + off` is UB when buf == NULL (a zero-length group: total 0, nseg 1, off 0). Reference buf
         * directly for offset 0 to avoid NULL pointer arithmetic; a real payload always has off < total. */
        slot->gso_ext     = (off == 0) ? buf : (const void *)((const unsigned char *)buf + off);
        slot->len         = slen;
        slot->tos         = tos;
        slot->recoverable = 1;                                  /* a GSO group is a batch → recoverable */
        slot->gso_owner   = owner;
        if (peer && kl_sockaddr_family(peer) != KL_AF_UNSPEC) slot->peer = *peer;
        else memset(&slot->peer, 0, sizeof(slot->peer));
        if (i == 0)        { slot->gso_head = 1; slot->gso_mode = mode ? 1 : 0;
                             slot->gso_total = total; slot->gso_seg = seg; }
        if (i == nseg - 1)   slot->gso_last = 1;
        s->ring[(s->head + s->count) % s->ring_cap] = slot;
        s->count++;
        s->bytes_used += slen;
    }
    if (kl_dgram_slots_free_count(s->slots) == 0)
        s->full = 1;
    return KL_DATAGRAM_ACCEPTED;
}

int kl_dgram_send_flush_batch(KlDgramSend *s, KlDgramTxDesc *descs, int descs_cap,
                              KlDgramSubmitBatchFn submit_batch, void *submit_ctx) {
    if (!s || !descs || descs_cap <= 0 || !submit_batch)
        return -1;
    /* Readiness-only + nothing in flight: batching does a SYNCHRONOUS drain over the queued head-run.
     * Reject a completion machine or an outstanding single-flight op; else a stray batch submit could
     * duplicate the in-flight head. */
    if (s->completion || s->inflight_n != 0)
        return -1;
    send_enter(s);
    dispatch_enter(s);
    while (!s->err && s->count > 0) {
        /* A GSO-mode group head is diverted out of the sendmmsg batch: submit the whole group in one
         * send_gso. */
        int retain = 0;
        if (send_gso_head(s, &retain)) {
            if (retain) break;   /* WOULD_BLOCK on the group; retain, re-arm */
            continue;
        }
        /* Reserve the head-run: up to min(descs_cap, count) contiguous queued slots, staged in FIFO
         * order into the caller's scratch, STOPPING before a GSO-mode group head (it is submitted on
         * its own next round). A GSO segment stages from its group buffer (`gso_ext`). Readiness send is
         * synchronous, so nothing is in flight and count == queued. */
        size_t k = s->count;
        if (k > (size_t)descs_cap) k = (size_t)descs_cap;
        {
            size_t staged = 0;
            for (size_t i = 0; i < k; i++) {
                const KlDgramSlot *slot = s->ring[(s->head + i) % s->ring_cap];
                if (i > 0 && slot->gso_head && slot->gso_mode == 0) break;   /* next GSO group; stop here */
                descs[i].data = slot->gso_ext ? slot->gso_ext : slot->data;
                descs[i].len = slot->len;
                descs[i].dest = slot->peer; descs[i].src = slot->local; descs[i].tos = slot->tos;
                staged++;
            }
            k = staged;
        }
        int sent = 0;
        KlDgramBatchResult br = submit_batch(submit_ctx, descs, (int)k, &sent);
        if (br == KL_DGRAM_BATCH_SENT) {
            if (sent < 0) sent = 0;
            if ((size_t)sent > k) sent = (int)k;
            for (int i = 0; i < sent; i++)
                send_retire_head(s, 0);            /* retire exactly the reported prefix */
            if ((size_t)sent < k)
                break;                             /* short/would-block on the remainder; retain, re-arm */
            /* sent == k: the whole head-run drained; loop for the next run */
        } else if (br == KL_DGRAM_BATCH_WOULDBLOCK) {
            break;                                 /* nothing sent; retain all, re-arm */
        } else {                                   /* KL_DGRAM_BATCH_ERROR: isolate the head */
            KlDgramSubmitResult r = send_submit_slot(s, 0);   /* single submit hook (io_status-classified) */
            if (r == KL_DGRAM_SUBMIT_DONE) {
                send_retire_head(s, 0);            /* the head was fine after all; retire, continue */
            } else if (r == KL_DGRAM_SUBMIT_WOULDBLOCK) {
                break;                             /* head can't go now; retain, re-arm */
            } else if (s->ring[s->head]->recoverable) {   /* hard error on a batch head → recoverable drop */
                send_drop_head(s, 0);              /* release the slot + edges + report, NOT sticky s->err */
            } else {                               /* non-recoverable head hard error → sticky (defensive) */
                s->err = 1;
            }
        }
    }
    int ret = s->err ? -1 : 0;
    dispatch_leave(s);
    send_leave(s);                                 /* LAST action; coordinator finalize may detach (frees s) */
    return ret;
}

int kl_dgram_send_on_complete(KlDgramSend *s, int ok) {
    if (!s)
        return -1;
    if (s->inflight_n == 0)
        return s->err ? -1 : 0;                    /* spurious / duplicate completion; no activity */

    send_enter(s);
    dispatch_enter(s);
    if (s->in_submit)
        s->submit_retired = 1;                     /* inline: tell the pump the op retired */
    int recov = s->ring[s->head] ? s->ring[s->head]->recoverable : 0;   /* the in-flight head's provenance */
    send_retire_head(s, 1);                         /* inflight_n 1 → 0 (the op physically retired) */
    if (!ok) {
        if (recov) { s->dropped++; if (s->on_drop) s->on_drop(s->drop_ctx); }  /* batch: recoverable drop */
        else       s->err = 1;                     /* ordinary send: sticky failure */
    }
    if (!s->err && !s->in_submit)
        (void)send_pump(s);                        /* async path pumps next; inline lets the outer pump continue */
    int ret = s->err ? -1 : 0;                     /* capture before callbacks (which may free s) */
    dispatch_leave(s);
    send_leave(s);                                 /* LAST action; coordinator finalize may detach (frees s) */
    return ret;
}

int kl_dgram_send_flush(KlDgramSend *s) {
    if (!s)
        return -1;
    send_enter(s);
    dispatch_enter(s);
    (void)send_pump(s);
    int ret = s->err ? -1 : 0;
    dispatch_leave(s);
    send_leave(s);                                 /* LAST action; coordinator finalize may detach (frees s) */
    return ret;
}

int kl_dgram_send_free(KlDgramSend *s) {
    if (!s)
        return 0;
    if (s->inflight_n > 0)
        return -1;   /* provider-referenced send outstanding; slot storage must not be freed */
    kl_dgram_send_discard_queued(s);   /* release queued-but-unsubmitted slots back to the borrowed
                                          pool (symmetric teardown; else reuse sees missing capacity) */
    if (s->ring && s->alloc)
        kl_free(s->alloc, s->ring, s->ring_cap * sizeof(KlDgramSlot *));
    memset(s, 0, sizeof(*s));   /* reusable (the borrowed slots are the caller's to free) */
    return 0;
}

void kl_dgram_send_abandon(KlDgramSend *s) {
    if (!s)
        return;
    /* Any GSO group in flight references a caller batch buffer: clear its gso_busy (on owner
     * destruction the ring is freed, so nothing reads the buffer anymore) before dropping the ring. */
    if (s->on_gso_done && s->ring) {
        for (size_t i = 0; i < s->count; i++) {
            KlDgramSlot *slot = s->ring[(s->head + i) % s->ring_cap];
            if (slot && slot->gso_last) s->on_gso_done(s->gso_done_ctx, slot->gso_owner);
        }
    }
    /* Free ONLY the FIFO ring (the object-owned storage that leaks otherwise), regardless of inflight_n.
     * No inflight_n guard: the caller has marked the life token dead (no late send completion re-enters
     * this machine) and the backend copied every submitted payload (no slot is referenced).
     * Do NOT release slots back to the pool; the caller frees the whole pool (kl_dgram_slots_free) next,
     * which reclaims any still-occupied slot; touching per-slot free-lists here would be wasted work. */
    if (s->ring && s->alloc)
        kl_free(s->alloc, s->ring, s->ring_cap * sizeof(KlDgramSlot *));
    memset(s, 0, sizeof(*s));   /* reusable */
}
