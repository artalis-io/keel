/*
 * datagram_send.c — INTERNAL atomic send machinery over KlDgramSlots (Phase B, step 2).
 *
 * See datagram_send.h. A whole datagram is copied into a preallocated outbound slot and queued in
 * a FIFO ring (send order), decoupled from the LIFO free-slot allocator. Readiness providers drain
 * synchronously (submit→DONE); completion providers submit up to `max_inflight` async sends
 * (submit→INFLIGHT) retired via on_complete. All state is updated before any callback fires.
 */
#include "datagram_send.h"

#include <stdint.h>   /* SIZE_MAX */
#include <string.h>   /* memcpy / memset */

/* Normalize an address to NULL when unset (KL_AF_UNSPEC) so the provider sees a connected send /
 * no source-pin as NULL rather than a zeroed sockaddr. */
static const KlSockAddr *addr_or_null(const KlSockAddr *a) {
    return (a && kl_sockaddr_family(a) != KL_AF_UNSPEC) ? a : NULL;
}

int kl_dgram_send_init(KlDgramSend *s, KlDgramSlots *slots, KlAllocator *ring_alloc,
                       int completion, size_t max_inflight, unsigned caps,
                       KlDgramSubmitFn submit, void *submit_ctx) {
    if (!s)
        return -1;
    memset(s, 0, sizeof(*s));
    if (!slots || !ring_alloc || !submit || slots->slot_count == 0)
        return -1;

    size_t cap = slots->slot_count;
    size_t ring_bytes;
    if (cap > SIZE_MAX / sizeof(KlDgramSlot *))   /* overflow-safe ring sizing */
        return -1;
    ring_bytes = cap * sizeof(KlDgramSlot *);

    KlDgramSlot **ring = kl_malloc(ring_alloc, ring_bytes);
    if (!ring)
        return -1;
    memset(ring, 0, ring_bytes);

    s->slots        = slots;
    s->alloc        = ring_alloc;
    s->ring         = ring;
    s->ring_cap     = cap;
    s->completion   = completion ? 1 : 0;
    s->max_inflight = max_inflight < 1 ? 1 : max_inflight;
    s->caps         = caps;
    s->submit       = submit;
    s->submit_ctx   = submit_ctx;
    return 0;
}

void kl_dgram_send_set_writable_cb(KlDgramSend *s, KlDgramWritableFn cb, void *ctx) {
    if (!s) return;
    s->on_writable = cb;
    s->writable_ctx = ctx;
}
void kl_dgram_send_set_drain_cb(KlDgramSend *s, KlDgramDrainFn cb, void *ctx) {
    if (!s) return;
    s->on_drain = cb;
    s->drain_ctx = ctx;
}
void kl_dgram_send_set_closing(KlDgramSend *s, int closing) {
    if (s) s->closing = closing ? 1 : 0;
}

/* Pop+release the oldest occupied slot; update FIFO + inflight + edge state. Returns the edges to
 * fire via *fire_writable / *fire_drain — the caller fires them AFTER all state (incl. a sticky
 * error) is updated, so a reentrant callback observes fully-updated state. */
static void send_retire_head(KlDgramSend *s, int was_inflight,
                             int *fire_writable, int *fire_drain) {
    KlDgramSlot *slot = s->ring[s->head];
    s->ring[s->head] = NULL;
    s->head = (s->head + 1) % s->ring_cap;
    s->count--;
    if (was_inflight && s->inflight_n > 0)
        s->inflight_n--;
    kl_dgram_slots_release(s->slots, slot);   /* a free slot appears */

    *fire_writable = 0;
    *fire_drain    = 0;
    if (s->full) { s->full = 0; *fire_writable = 1; }   /* full → non-full (exactly once) */
    if (s->count == 0) *fire_drain = 1;                 /* non-empty → empty */
}

static void send_fire(KlDgramSend *s, int fire_writable, int fire_drain) {
    if (fire_writable && s->on_writable) s->on_writable(s->writable_ctx);
    if (fire_drain && s->on_drain) s->on_drain(s->drain_ctx);
}

/* Submit the slot at FIFO position `off` from the head. */
static KlDgramSubmitResult send_submit_slot(KlDgramSend *s, size_t off) {
    KlDgramSlot *slot = s->ring[(s->head + off) % s->ring_cap];
    return s->submit(s->submit_ctx, slot->data, slot->len,
                     addr_or_null(&slot->peer), addr_or_null(&slot->local), slot->tos);
}

/* Drain queued datagrams: readiness submits (DONE) until WOULD_BLOCK/empty; completion submits up
 * to max_inflight (INFLIGHT). Returns 0, or -1 once a sticky error is set. */
static int send_pump(KlDgramSend *s) {
    if (s->err) return -1;
    while (s->count > s->inflight_n && s->inflight_n < s->max_inflight) {
        KlDgramSubmitResult r = send_submit_slot(s, s->inflight_n);
        if (r == KL_DGRAM_SUBMIT_INFLIGHT) {
            s->inflight_n++;                       /* completion: one more async op outstanding */
        } else if (r == KL_DGRAM_SUBMIT_DONE) {
            /* readiness sync send: valid only at the head (a sync provider never has async ops
             * ahead). Reject a provider that mixes the two rather than corrupt FIFO retirement. */
            if (s->inflight_n != 0) { s->err = 1; return -1; }
            int fw, fd;
            send_retire_head(s, 0, &fw, &fd);
            send_fire(s, fw, fd);
        } else if (r == KL_DGRAM_SUBMIT_WOULDBLOCK) {
            break;                                 /* keep queued; a writable event resumes (flush) */
        } else {                                   /* KL_DGRAM_SUBMIT_ERROR */
            s->err = 1;                            /* sticky; datagram retained (owned) in its slot */
            return -1;
        }
    }
    return 0;
}

KlDatagramSendStatus kl_dgram_send(KlDgramSend *s, const KlDatagramMessage *m) {
    if (!s || !m || !s->submit)
        return KL_DATAGRAM_ERROR;
    if (s->err)
        return KL_DATAGRAM_ERROR;                  /* sticky */
    if (s->closing)
        return KL_DATAGRAM_CLOSED;
    if (m->len && !m->data)
        return KL_DATAGRAM_ERROR;

    /* Requested-but-unsupported features fail; nothing is taken (§9). */
    if (addr_or_null(m->local) && !(s->caps & KL_DGRAM_CAP_SOURCE_PIN))
        return KL_DATAGRAM_UNSUPPORTED;
    if (m->tos >= 0 && !(s->caps & KL_DGRAM_CAP_TOS))
        return KL_DATAGRAM_UNSUPPORTED;
    if (!addr_or_null(m->peer) && !(s->caps & KL_DGRAM_CAP_CONNECTED))
        return KL_DATAGRAM_UNSUPPORTED;            /* connected-mode send unsupported */

    /* Permanent: a datagram that can never fit one slot. Nothing taken. */
    if (m->len > s->slots->out_cap)
        return KL_DATAGRAM_TOO_LARGE;

    /* Readiness fast path: an empty queue tries a direct synchronous send with NO slot consumed —
     * so a synchronously-sent datagram never creates a transient non-empty→empty transition. */
    if (!s->completion && s->count == 0) {
        KlDgramSubmitResult r = s->submit(s->submit_ctx, m->data, m->len,
                                          addr_or_null(m->peer), addr_or_null(m->local), m->tos);
        if (r == KL_DGRAM_SUBMIT_DONE)
            return KL_DATAGRAM_ACCEPTED;           /* sent; nothing queued, no ownership retained */
        if (r == KL_DGRAM_SUBMIT_ERROR) {
            s->err = 1;
            return KL_DATAGRAM_ERROR;              /* refusal: nothing taken (no slot acquired) */
        }
        /* WOULD_BLOCK: fall through and queue it. (A readiness provider never returns INFLIGHT.) */
    }

    /* Acquire a slot only once acceptance is certain — every refusal above takes no ownership and
     * mutates no queue state. */
    KlDgramSlot *slot = kl_dgram_slots_acquire(s->slots);
    if (!slot) {
        s->full = 1;                               /* full — arm the full→non-full edge */
        return KL_DATAGRAM_WOULD_BLOCK;            /* transient; nothing taken */
    }

    /* Copy the whole datagram + its metadata before returning (caller owns nothing after). */
    if (m->len)
        memcpy(slot->data, m->data, m->len);
    slot->len   = m->len;
    slot->tos   = m->tos;
    slot->flags = m->flags;
    if (addr_or_null(m->peer))  slot->peer  = *m->peer;  else memset(&slot->peer,  0, sizeof(slot->peer));
    if (addr_or_null(m->local)) slot->local = *m->local; else memset(&slot->local, 0, sizeof(slot->local));

    /* Enqueue FIFO (tail). */
    s->ring[(s->head + s->count) % s->ring_cap] = slot;
    s->count++;
    if (kl_dgram_slots_free_count(s->slots) == 0)
        s->full = 1;                               /* filled the last slot */

    (void)send_pump(s);                            /* submit failure is sticky; the datagram stays owned */
    return KL_DATAGRAM_ACCEPTED;
}

int kl_dgram_send_on_complete(KlDgramSend *s, int ok) {
    if (!s)
        return -1;
    if (s->inflight_n == 0)
        return s->err ? -1 : 0;                    /* spurious / duplicate completion */

    int fw, fd;
    send_retire_head(s, 1, &fw, &fd);
    if (!ok)
        s->err = 1;                                /* op physically retired but failed — sticky */
    send_fire(s, fw, fd);                          /* state (incl. err) fully updated before callbacks */
    if (s->err)
        return -1;
    return send_pump(s);                           /* submit the next queued datagram(s) */
}

int kl_dgram_send_flush(KlDgramSend *s) {
    if (!s)
        return -1;
    return send_pump(s);
}

int kl_dgram_send_free(KlDgramSend *s) {
    if (!s)
        return 0;
    if (s->inflight_n > 0)
        return -1;   /* provider-referenced sends outstanding — slot storage must not be freed */
    if (s->ring && s->alloc)
        kl_free(s->alloc, s->ring, s->ring_cap * sizeof(KlDgramSlot *));
    memset(s, 0, sizeof(*s));   /* reusable (the borrowed slots are the caller's to free) */
    return 0;
}
