#ifndef KEEL_SRC_DATAGRAM_SEND_H
#define KEEL_SRC_DATAGRAM_SEND_H

/*
 * datagram_send.h — INTERNAL atomic send machinery over KlDgramSlots.
 *
 * The send-side state machine the frozen Tier-1 contract requires (docs/contracts/datagram.md §1):
 * a whole datagram is accepted (copied into a preallocated outbound slot) or refused — never
 * partial — with exact status mapping, FIFO send order over the LIFO free-slot allocator, a sticky
 * error state, on_writable (full→non-full) / on_drain (non-empty→empty) edges, and free refused
 * while a provider-referenced send is outstanding.
 *
 * SINGLE-FLIGHT (Tier-1). At most ONE send is in flight at a time (contract §1: "exactly one send
 * batch submitted at a time"). The oldest queued datagram is the only one submitted; the next is
 * pumped when it retires. A bounded-N>1 window would require a per-submission stable token so
 * out-of-order completions retire the exact slot — that is a future capability, not this API.
 *
 * INLINE COMPLETION SAFE. The submit hook MAY synchronously call kl_dgram_send_on_complete (the op
 * is armed before submit); a synchronous readiness provider signals completion via DONE instead.
 *
 * CALLBACK DISCIPLINE. All state is updated before any callback fires; callbacks are deferred to
 * the outermost public call (a depth guard makes reentrant sends defer their callbacks back).
 * on_writable is reentrant but NON-DESTRUCTIVE. on_drain is the DESTRUCTIVE TAIL: it fires only if
 * the queue is STILL empty (a reentrant on_writable send may have refilled it), and the machine
 * makes no further access to itself afterwards — the callback may free/tear down the object.
 *
 * INTERNAL and NOT exposed publicly — it drives an abstract
 * `submit` hook (a mock in tests) over a BORROWED KlDgramSlots; it owns only its FIFO ring.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include "datagram_slots.h"

#include <keel/allocator.h>
#include <keel/sockaddr.h>
#include <keel/datagram.h>   /* KlDatagramSendStatus, KlDatagramMessage, KL_DGRAM_CAP_* */

#include <stddef.h>

/* KlDatagramSendStatus (§1 accept/refuse contract), KL_DGRAM_CAP_* (send UNSUPPORTED mapping), and
 * KlDatagramMessage (one outbound datagram, copied before kl_dgram_send returns) are defined in the
 * public <keel/datagram.h>; this machine consumes them. */

/* Result of one provider submit attempt. A provider is EITHER synchronous (readiness: DONE /
 * WOULD_BLOCK) OR asynchronous (completion: INFLIGHT), never both. */
typedef enum {
    KL_DGRAM_SUBMIT_DONE = 0,    /* sent synchronously — retire now (readiness) */
    KL_DGRAM_SUBMIT_INFLIGHT,    /* accepted async — retire via kl_dgram_send_on_complete (completion) */
    KL_DGRAM_SUBMIT_WOULDBLOCK,  /* provider cannot accept now — keep queued (readiness EAGAIN) */
    KL_DGRAM_SUBMIT_ERROR,       /* hard failure — sticky error; datagram retained */
    KL_DGRAM_SUBMIT_UNSUPPORTED  /* the op is unavailable on this fd (GSO whole-submit) — the
                                  * caller latches + falls back; nothing was sent so retransmit is safe */
} KlDgramSubmitResult;

/* Submit one datagram to the provider (fields, not a slot — so the readiness fast path can submit
 * the caller's buffer directly). `peer` is NULL for a connected-mode send. */
typedef KlDgramSubmitResult (*KlDgramSubmitFn)(void *ctx, const void *data, size_t len,
                                               const KlSockAddr *peer, const KlSockAddr *local,
                                               int tos);
typedef void (*KlDgramWritableFn)(void *ctx);   /* full→non-full edge (reentrant, non-destructive) */
typedef void (*KlDgramDrainFn)(void *ctx);      /* non-empty→empty edge (destructive tail) */
typedef void (*KlDgramDropFn)(void *ctx);       /* a recoverable per-datagram send drop occurred
                                                 * (the facade records last_error + a dropped count) */

/* Result of one batch submit (readiness sendmmsg, or the portable single-send loop). The adapter has
 * the provider, so it classifies a -1 into WOULD_BLOCK vs a hard error (via kl_sock_io_status) BEFORE
 * returning — the machine stays transport-neutral. On SENT, `*sent` ∈ [0, n] is the
 * prefix actually transmitted; WOULDBLOCK means nothing was sent (retain all); ERROR is a hard error on
 * the head descriptor (the machine isolates it via the single submit hook). */
typedef enum {
    KL_DGRAM_BATCH_SENT = 0,     /* *sent datagrams of the prefix went out */
    KL_DGRAM_BATCH_WOULDBLOCK,   /* EAGAIN with nothing sent — retain, re-arm */
    KL_DGRAM_BATCH_ERROR         /* hard error on the head — isolate the head via the single hook */
} KlDgramBatchResult;
typedef KlDgramBatchResult (*KlDgramSubmitBatchFn)(void *ctx, const KlDgramTxDesc *descs, int n,
                                                   int *sent);

/* Submit a WHOLE GSO group in ONE send_gso (readiness). `owner` is the group's KlDatagramBatch
 * (opaque here); `buf`/`total`/`seg` are the group buffer + payload. Returns DONE (the whole group went)
 * / WOULDBLOCK (retain) / UNSUPPORTED (latch + fall back) / ERROR (hard). */
typedef KlDgramSubmitResult (*KlDgramSubmitGsoFn)(void *ctx, void *owner, const void *buf,
                                                  size_t total, size_t seg,
                                                  const KlSockAddr *peer, int tos);
/* A GSO group's LAST segment retired: the batch group buffer is free (clears gso_busy). */
typedef void (*KlDgramGsoDoneFn)(void *ctx, void *owner);

typedef struct {
    KlDgramSlots     *slots;         /* borrowed — the OBJECT-owned outbound send pool */
    KlAllocator      *alloc;         /* the FIFO ring's allocator (init/free only) */
    KlDgramSlot     **ring;          /* [ring_cap] occupied slots in FIFO send order */
    size_t            ring_cap;      /* == slots->slot_count */
    size_t            head;          /* index of the oldest occupied slot */
    size_t            count;         /* occupied slots (queued + in-flight) */
    size_t            inflight_n;    /* 0 or 1 — single-flight (Tier-1) */
    /* BOTH queue policy: a scalar byte admission gate over the slot array. `byte_budget` == 0 is the
     * default SLOT policy (gate off). `bytes_used` == Σ slot->len over every OCCUPIED slot (queued +
     * in-flight); it is ONLY an admission accumulator — NOT an emptiness predicate (a zero-length slot
     * contributes 0 bytes yet occupies a slot, so bytes_used == 0 can hold with count > 0; `count`
     * remains the sole drain predicate). See docs/archive/designs/datagram_m1_queue_policy_design.md. */
    size_t            byte_budget;   /* 0 = SLOT (off); >0 = BOTH with this byte budget */
    size_t            bytes_used;    /* Σ occupied-slot len (admission gate only) */
    int               completion;    /* 1 = async (submit→INFLIGHT); 0 = readiness (submit→DONE) */
    unsigned          caps;          /* KL_DGRAM_CAP_* for UNSUPPORTED mapping */
    int               connected;     /* the ACTUAL connected state (set after a successful connect).
                                      * A peerless send needs BOTH (caps & CAP_CONNECTED) AND this — the
                                      * single admission rule for kl_dgram_send / _enqueue / _enqueue_gso. */
    KlDgramSubmitFn   submit;   void *submit_ctx;
    KlDgramWritableFn on_writable; void *writable_ctx;
    KlDgramDrainFn    on_drain;  void *drain_ctx;
    KlDgramDropFn     on_drop;   void *drop_ctx;   /* fired at each recoverable drop (non-destructive) */
    KlDgramSubmitGsoFn submit_gso; void *submit_gso_ctx;   /* whole-group send_gso (readiness) */
    KlDgramGsoDoneFn  on_gso_done; void *gso_done_ctx;     /* a GSO group fully retired */
    int               err;           /* sticky transport error */
    int               closing;       /* refuse new sends with CLOSED */
    /* Close busy handshake: fired +1 on entering / -1 on leaving (as the LAST action) any public op
     * that can retire or fire a callback, so the close coordinator only detaches once the outermost
     * machine/coordinator frame unwinds. */
    void            (*on_activity)(void *ctx, int delta); void *activity_ctx;
    int               full;          /* latched: the queue is full (for the full→non-full edge) */
    /* callback-deferral + inline-completion sentinels */
    int               in_dispatch;   /* depth of public calls that may fire callbacks */
    int               pend_writable; /* a full→non-full edge is pending */
    int               pend_drain;    /* a retirement emptied the queue (re-checked before firing) */
    int               in_submit;     /* a submit() call is on the stack (inline-completion window) */
    int               submit_retired;/* an inline on_complete retired the in-flight op */
    size_t            dropped;       /* datagrams dropped by the recoverable per-datagram send-error
                                      * policy (a hard error on an isolated head — NOT the sticky `err`).
                                      * The facade reads the delta to set its last_error + dropped count. */
} KlDgramSend;

/* Wire the send machine over a borrowed, already-inited KlDgramSlots. `completion` selects the I/O
 * model (1 = async submit/INFLIGHT, 0 = readiness sync submit/DONE). Single-flight (Tier-1).
 * Allocates only the FIFO ring (one init-time allocation). Returns 0, or -1 on bad argument /
 * overflow / allocation failure (object left zeroed, reusable). */
int  kl_dgram_send_init(KlDgramSend *s, KlDgramSlots *slots, KlAllocator *ring_alloc,
                        int completion, unsigned caps, size_t byte_budget,
                        KlDgramSubmitFn submit, void *submit_ctx);

void kl_dgram_send_set_writable_cb(KlDgramSend *s, KlDgramWritableFn cb, void *ctx);
void kl_dgram_send_set_drain_cb(KlDgramSend *s, KlDgramDrainFn cb, void *ctx);
void kl_dgram_send_set_drop_cb(KlDgramSend *s, KlDgramDropFn cb, void *ctx);
void kl_dgram_send_set_gso_cbs(KlDgramSend *s, KlDgramSubmitGsoFn submit_gso, void *submit_ctx,
                               KlDgramGsoDoneFn on_gso_done, void *done_ctx);

/* Atomically admit a GSO group of `nseg` contiguous segment slots (all-or-nothing against the
 * free-slot count AND the byte budget). Each segment references the caller's group buffer (`buf`, valid
 * until the group retires) via `gso_ext` at its offset — nothing is copied into the pool. `mode` = 0
 * (GSO whole-submit) / 1 (FALLBACK per-segment). `owner` is the KlDatagramBatch whose gso_busy clears
 * when the group retires (on_gso_done). Returns ACCEPTED / WOULD_BLOCK (not enough free slots or byte
 * gate) / TOO_LARGE (nseg > slot count, or total > budget) / CLOSED / ERROR. Fires no callbacks. */
KlDatagramSendStatus kl_dgram_send_enqueue_gso(KlDgramSend *s, const void *buf, size_t total,
                                               size_t seg, size_t nseg, const KlSockAddr *peer, int tos,
                                               int mode, void *owner);

/* Accept or refuse one whole datagram (never partial). Copies data + metadata before returning. */
KlDatagramSendStatus kl_dgram_send(KlDgramSend *s, const KlDatagramMessage *m);

/* ENQUEUE-ONLY: the same up-front validation + byte-gate + slot-acquire + copy + FIFO enqueue
 * as kl_dgram_send, but with NO readiness fast path and NO submit/pump (fires no callbacks). Returns
 * ACCEPTED / WOULD_BLOCK / TOO_LARGE / UNSUPPORTED / CLOSED / ERROR. The batch transaction admits an
 * accepted prefix via this, then drains once (kl_dgram_send_flush_batch on readiness, or
 * kl_dgram_send_flush single-flight on completion). */
KlDatagramSendStatus kl_dgram_send_enqueue(KlDgramSend *s, const KlDatagramMessage *m);

/* READINESS batch-drain over the head-run. Stages up to min(descs_cap, count) contiguous
 * queued slots into the caller-owned `descs` scratch and submits them in one `submit_batch` call; then:
 * SENT → retire exactly the reported prefix (short/partial ⇒ retain the remainder + return, the facade
 * re-arms WRITE); WOULDBLOCK → retain all + return; ERROR → isolate the head via the single submit hook
 * (`s->submit`): DONE retires it and continues; WOULD_BLOCK retains + returns; a hard error DROPS that
 * one head slot under the recoverable send-error policy (`dropped++`, NOT sticky `err`) and continues.
 * Readiness-only (synchronous; inflight_n stays 0). Returns 0, or -1 if a sticky error is set. */
int  kl_dgram_send_flush_batch(KlDgramSend *s, KlDgramTxDesc *descs, int descs_cap,
                               KlDgramSubmitBatchFn submit_batch, void *submit_ctx);

/* Async retirement of the single in-flight send (completion mode; may be called inline from the
 * submit hook). ok=0 → sticky error. Returns 0, or -1 if a sticky error is set. Spurious/duplicate
 * completions (none in flight) are ignored. */
int  kl_dgram_send_on_complete(KlDgramSend *s, int ok);

/* Readiness: retry draining queued datagrams after a WOULD_BLOCK, on a writable event. 0 / -1. */
int  kl_dgram_send_flush(KlDgramSend *s);

/* Mark the send side closing (subsequent kl_dgram_send returns CLOSED). */
void kl_dgram_send_set_closing(KlDgramSend *s, int closing);

/* Close hooks. on_activity(delta) brackets every public op that can retire or fire a
 * callback (+1 entry, -1 as the LAST action) — the close coordinator counts frames and detaches
 * only when the outermost unwinds. When an activity cb is set, any on_drain callback MUST be
 * non-destructive (the coordinator's on_close is the sole destructive tail). */
void kl_dgram_send_set_activity_cb(KlDgramSend *s, void (*cb)(void *ctx, int delta), void *ctx);
/* Abortive close: discard every queued-but-UNSUBMITTED datagram (release their slots), keeping the
 * in-flight prefix (which the cancel hook retires). Fires no callbacks. */
void kl_dgram_send_discard_queued(KlDgramSend *s);

/* Free the FIFO ring and zero the object (the borrowed slots pool is NOT freed here, but every
 * queued-but-unsubmitted slot is RELEASED back to it first — symmetric teardown, so reuse of the
 * pool sees full capacity). REFUSES with -1 while the in-flight send is outstanding (slot storage
 * must not be freed under the provider). */
int  kl_dgram_send_free(KlDgramSend *s);

/* Owner-destruction teardown (docs/archive/designs/datagram_sync_teardown_design.md): free the FIFO ring +
 * zero the machine REGARDLESS of inflight_n. Safe ONLY under the abandon preconditions — a dead life
 * token (no late completion re-enters send_on_complete) AND the frozen copy-at-submit contract
 * (no backend references a slot after submit). The borrowed slot pool is freed separately by the caller
 * (kl_dgram_slots_free), which reclaims any still-occupied in-flight slot; this touches only the ring.
 * The ordinary kl_dgram_send_free (with its inflight_n guard) is UNCHANGED for the confirmed-detachment
 * path — abandon is an additional owner-destruction entry, never a substitute. */
void kl_dgram_send_abandon(KlDgramSend *s);

/* Set the connected state (after a successful connect) — governs peerless-send admission for all of
 * kl_dgram_send / _enqueue / _enqueue_gso. */
void kl_dgram_send_set_connected(KlDgramSend *s, int on);

static inline size_t kl_dgram_send_inflight(const KlDgramSend *s) { return s ? s->inflight_n : 0; }
static inline size_t kl_dgram_send_queued(const KlDgramSend *s)   { return s ? s->count : 0; }
static inline int    kl_dgram_send_connected(const KlDgramSend *s) { return s ? s->connected : 0; }
/* Σ payload bytes over every OCCUPIED slot (queued + in-flight) — the byte view of the send backlog,
 * distinct from the slot COUNT above. Exposed publicly as kl_datagram_send_queued_bytes. */
static inline size_t kl_dgram_send_queued_bytes(const KlDgramSend *s) { return s ? s->bytes_used : 0; }
static inline int    kl_dgram_send_error(const KlDgramSend *s)    { return s ? s->err : 1; }
static inline size_t kl_dgram_send_dropped(const KlDgramSend *s)  { return s ? s->dropped : 0; }

#endif /* KEEL_SRC_DATAGRAM_SEND_H */
