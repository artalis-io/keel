#ifndef KEEL_SRC_DATAGRAM_SEND_H
#define KEEL_SRC_DATAGRAM_SEND_H

/*
 * datagram_send.h — INTERNAL atomic send machinery over KlDgramSlots (Phase B, step 2).
 *
 * The send-side state machine the frozen Tier-1 contract requires (docs/datagram_contract.md §1):
 * a whole datagram is accepted (copied into a preallocated outbound slot) or refused — never
 * partial — with exact status mapping, FIFO send order over the LIFO free-slot allocator, at most
 * a provider-supported bounded number of sends in flight, on_writable (full→non-full) and on_drain
 * (non-empty→empty) edges, a sticky error state, and free refused while provider-referenced sends
 * are outstanding.
 *
 * This is INTERNAL and NOT yet wired to a live UDP/DNS provider or exposed publicly — it drives an
 * abstract `submit` hook (a mock in tests; the real udp/completion send in a later step). It
 * operates OVER a borrowed KlDgramSlots (step 1); it owns only its FIFO ring.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include "datagram_slots.h"

#include <keel/allocator.h>
#include <keel/sockaddr.h>

#include <stddef.h>

/* Send-status contract (§1). Named to match the eventual public KlDatagramSendStatus (step 7). */
typedef enum {
    KL_DATAGRAM_ACCEPTED = 0,   /* whole datagram taken (sent or slot-queued) */
    KL_DATAGRAM_WOULD_BLOCK,    /* transient: no free outbound slot; nothing taken */
    KL_DATAGRAM_TOO_LARGE,      /* permanent: datagram exceeds one slot; nothing taken */
    KL_DATAGRAM_UNSUPPORTED,    /* an explicitly-requested capability is unavailable; nothing sent */
    KL_DATAGRAM_CLOSED,         /* closing/closed: no further sends */
    KL_DATAGRAM_ERROR           /* bad argument or sticky transport error */
} KlDatagramSendStatus;

/* Requested-feature capabilities (subset needed for send UNSUPPORTED mapping in step 2; the full
 * set is exposed in step 7). A feature the caller requests but the object lacks fails. */
#define KL_DGRAM_CAP_SOURCE_PIN  (1u << 0)   /* msg.local source-pin on send */
#define KL_DGRAM_CAP_TOS         (1u << 1)   /* per-packet TOS/ECN on send */
#define KL_DGRAM_CAP_CONNECTED   (1u << 2)   /* connected-mode send (msg.peer == NULL) */

/* One outbound datagram to send (borrowed; copied before kl_dgram_send returns). */
typedef struct {
    const void       *data;
    size_t            len;
    const KlSockAddr *peer;    /* destination; NULL = connected-mode send */
    const KlSockAddr *local;   /* source/interface pin; NULL = none */
    int               tos;     /* per-packet TOS/ECN byte, or -1 = none */
    unsigned          flags;   /* stored on the slot; opaque to the machine in step 2 */
} KlDatagramMessage;

/* Result of one provider submit attempt. A provider is EITHER synchronous (readiness: DONE /
 * WOULD_BLOCK) OR asynchronous (completion: INFLIGHT), never both. */
typedef enum {
    KL_DGRAM_SUBMIT_DONE = 0,    /* sent synchronously — retire now (readiness) */
    KL_DGRAM_SUBMIT_INFLIGHT,    /* accepted async — retire via kl_dgram_send_on_complete (completion) */
    KL_DGRAM_SUBMIT_WOULDBLOCK,  /* provider cannot accept now — keep queued (readiness EAGAIN) */
    KL_DGRAM_SUBMIT_ERROR        /* hard failure — sticky error; datagram retained */
} KlDgramSubmitResult;

/* Submit one datagram to the provider (fields, not a slot — so the readiness fast path can submit
 * the caller's buffer directly). `peer` is NULL for a connected-mode send. */
typedef KlDgramSubmitResult (*KlDgramSubmitFn)(void *ctx, const void *data, size_t len,
                                               const KlSockAddr *peer, const KlSockAddr *local,
                                               int tos);
typedef void (*KlDgramWritableFn)(void *ctx);   /* full→non-full edge */
typedef void (*KlDgramDrainFn)(void *ctx);      /* non-empty→empty edge */

typedef struct {
    KlDgramSlots     *slots;         /* borrowed (step 1) — outbound slots + inbound slot */
    KlAllocator      *alloc;         /* the FIFO ring's allocator (init/free only) */
    KlDgramSlot     **ring;          /* [ring_cap] occupied slots in FIFO send order */
    size_t            ring_cap;      /* == slots->slot_count */
    size_t            head;          /* index of the oldest occupied slot */
    size_t            count;         /* occupied slots (queued + in-flight) */
    size_t            inflight_n;    /* head-prefix slots submitted, awaiting async retirement */
    size_t            max_inflight;  /* provider-supported bound (>= 1; readiness stays at 0) */
    int               completion;    /* 1 = async (submit→INFLIGHT); 0 = readiness (submit→DONE) */
    unsigned          caps;          /* KL_DGRAM_CAP_* for UNSUPPORTED mapping */
    KlDgramSubmitFn   submit;   void *submit_ctx;
    KlDgramWritableFn on_writable; void *writable_ctx;
    KlDgramDrainFn    on_drain;  void *drain_ctx;
    int               err;           /* sticky transport error */
    int               closing;       /* refuse new sends with CLOSED */
    int               full;          /* latched: the queue is full (for the full→non-full edge) */
} KlDgramSend;

/* Wire the send machine over a borrowed, already-inited KlDgramSlots. `completion` selects the I/O
 * model (1 = async submit/INFLIGHT, 0 = readiness sync submit/DONE). `max_inflight` (clamped >= 1)
 * bounds concurrent async sends. Allocates only the FIFO ring (one init-time allocation). Returns 0,
 * or -1 on bad argument / overflow / allocation failure (object left zeroed, reusable). */
int  kl_dgram_send_init(KlDgramSend *s, KlDgramSlots *slots, KlAllocator *ring_alloc,
                        int completion, size_t max_inflight, unsigned caps,
                        KlDgramSubmitFn submit, void *submit_ctx);

void kl_dgram_send_set_writable_cb(KlDgramSend *s, KlDgramWritableFn cb, void *ctx);
void kl_dgram_send_set_drain_cb(KlDgramSend *s, KlDgramDrainFn cb, void *ctx);

/* Accept or refuse one whole datagram (never partial). Copies data + metadata before returning. */
KlDatagramSendStatus kl_dgram_send(KlDgramSend *s, const KlDatagramMessage *m);

/* Async retirement of the oldest in-flight send (completion mode). ok=0 → sticky error. Returns 0,
 * or -1 if a sticky error is set. Spurious/duplicate completions (none in flight) are ignored. */
int  kl_dgram_send_on_complete(KlDgramSend *s, int ok);

/* Readiness: retry draining queued datagrams after a WOULD_BLOCK, on a writable event. Returns 0,
 * or -1 on sticky error. */
int  kl_dgram_send_flush(KlDgramSend *s);

/* Mark the send side closing (subsequent kl_dgram_send returns CLOSED). Full close/detachment is a
 * later step; this only gates new sends. */
void kl_dgram_send_set_closing(KlDgramSend *s, int closing);

/* Free the FIFO ring and zero the object (the borrowed slots are NOT freed here). REFUSES with -1
 * while any provider-referenced (in-flight) send is outstanding — slot storage must not be freed
 * underneath the provider. */
int  kl_dgram_send_free(KlDgramSend *s);

static inline size_t kl_dgram_send_inflight(const KlDgramSend *s) { return s ? s->inflight_n : 0; }
static inline size_t kl_dgram_send_queued(const KlDgramSend *s)   { return s ? s->count : 0; }
static inline int    kl_dgram_send_error(const KlDgramSend *s)    { return s ? s->err : 1; }

#endif /* KEEL_SRC_DATAGRAM_SEND_H */
