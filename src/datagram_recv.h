#ifndef KEEL_SRC_DATAGRAM_RECV_H
#define KEEL_SRC_DATAGRAM_RECV_H

/*
 * datagram_recv.h: INTERNAL serial receive + strict pause/resume + uniform truncation/metadata
 * over the dedicated inbound slot. See docs/contracts/datagram.md §2/§4/§8.
 *
 * Exactly one receive is outstanding at a time: completion posts ONE recv op; readiness arms ONE
 * READ source and performs serial provider receives (one datagram per pull), re-checking
 * paused/stopped after every delivery. Received datagrams land in the DEDICATED INBOUND SLOT
 * (never an outbound slot). Pause is STRICT: readiness drops interest (nothing completes); a
 * completion recv already posted stays in flight and, when it completes, is HELD in the inbound
 * slot; resume delivers the held datagram exactly once, then re-arms. An arm hook MAY complete
 * synchronously (the op is pre-armed and an iterative trampoline bounds the C stack). Duplicate /
 * spurious completions are dropped without disturbing held data. The delivery callback may
 * pause/stop, but MUST NOT free or re-init the object before the confirmed-detachment
 * callback; kl_dgram_recv_free refuses while a receive is outstanding.
 *
 * UNIFORM RECEIVE CONTRACT, enforced in the machine regardless of provider/backend:
 *  - The delivered `len` is the CAPTURED-PREFIX length and never exceeds inbound_payload_capacity.
 *  - An oversized datagram is delivered ONCE with KL_DGRAM_TRUNCATED (captured prefix); it is NOT
 *    treated as a fatal receive. Truncation is signalled either by a provider length > in_cap or by
 *    the provider setting KL_DGRAM_TRUNCATED in the inbound slot (e.g. IOCP parsing WSAMSG.dwFlags /
 *    WSAEMSGSIZE at exact capacity, or recvmsg's MSG_TRUNC).
 *  - `peer` (source) is MANDATORY on every successful delivery; a completion/pull that yields no
 *    peer is a provider contract violation and FAILS SAFELY (no callback, error state); it is not
 *    delivered as an anonymous datagram.
 *  - `local` (dest/interface) is delivered only when KL_DGRAM_HAS_LOCAL is set AND valid; otherwise
 *    it is passed as NULL and the slot's stale local is cleared. Metadata is copied into the inbound
 *    slot and canonicalized BEFORE delivery or pause-hold, and the inbound slot's metadata is reset
 *    before each arm/pull so a prior packet's addresses/flags never survive slot reuse.
 *  - A genuine zero-length datagram (ok, len 0, peer present) remains distinct from a failure (ok=0).
 *  - A held (paused) datagram preserves its complete metadata snapshot across pause/resume.
 *
 * No allocation at all (uses the borrowed inbound slot); no interaction with outbound slot
 * availability. INTERNAL, NOT wired to a live provider or exposed publicly.
 *
 * INTERNAL header: not installed, no ABI commitment.
 */

#include "datagram_slots.h"

#include <keel/sockaddr.h>

#include <stddef.h>

/* Arm the next receive: completion = post ONE recv op into the inbound slot (may synchronously call
 * kl_dgram_recv_on_complete); readiness = add READ interest. Returns 0 armed/posted, -1 on failure. */
typedef int  (*KlDgramRecvArmFn)(void *ctx);
/* Readiness only: remove READ interest (idempotent at the provider). */
typedef void (*KlDgramRecvDisarmFn)(void *ctx);
/* Readiness only: receive ONE datagram into the inbound slot (fill data + metadata: peer [mandatory],
 * local + KL_DGRAM_HAS_LOCAL when available, KL_DGRAM_TRUNCATED for an oversized/exact-capacity
 * truncation). *out_len = the datagram length (may exceed in_cap → captured-prefix truncation).
 * Returns 1 = one datagram, 0 = would-block (drained), -1 = fatal error. */
typedef int  (*KlDgramRecvPullFn)(void *ctx, size_t *out_len);
/* Deliver one received datagram (borrowed from the inbound slot; valid only for the call). `peer` is
 * always non-NULL; `local` is non-NULL iff KL_DGRAM_HAS_LOCAL is set in `flags`; `flags` may carry
 * KL_DGRAM_TRUNCATED. `len` is the captured-prefix length (≤ inbound_payload_capacity). */
typedef void (*KlDgramRecvDeliverFn)(void *ctx, const void *data, size_t len,
                                     const KlSockAddr *peer, const KlSockAddr *local, unsigned flags);

/* Borrowed-view delivery (the batch/GRO receive seam). A logical datagram yielded from the
 * batch cursor: `data`/`len` point into the ext rx-batch storage (valid ONLY for the delivery call),
 * NOT the owned inbound slot, so a GRO-coalesced buffer is delivered un-clamped by recv_cap
 * (truncation comes from the provider meta, carried in `flags`). `peer` is MANDATORY; `local` is valid
 * iff KL_DGRAM_HAS_LOCAL is set in `flags`. */
typedef struct {
    const void *data;
    size_t      len;
    KlSockAddr  peer;
    KlSockAddr  local;
    unsigned    flags;     /* KL_DGRAM_TRUNCATED / KL_DGRAM_HAS_LOCAL */
} KlDgramRxView;

/* Readiness batch mode: yield ONE logical datagram into `*v` from the batch cursor. When `allow_refill`
 * and the cursor is drained, refill via one recv_batch / single recv; when NOT allow_refill (the resume
 * held-drain), yield ONLY already-buffered datagrams and return 0 once the cursor is empty (no
 * kernel read). Returns 1 = a datagram, 0 = drained (would-block / cursor empty), -1 = fatal. When set
 * (kl_dgram_recv_set_view_pull), the readiness readable loop + resume drive this INSTEAD of `pull`. */
typedef int  (*KlDgramRecvViewFn)(void *ctx, KlDgramRxView *v, int allow_refill);

typedef struct {
    KlDgramInbound     *inbound;      /* borrowed: the dedicated inbound slot (life-token-ownable) */
    int                 completion;   /* 1 = completion (post/on_complete); 0 = readiness (arm/pull) */
    KlDgramRecvArmFn    arm;
    KlDgramRecvDisarmFn disarm;       /* required for readiness */
    KlDgramRecvPullFn   pull;         /* required for readiness */
    KlDgramRecvViewFn   view_pull;    /* readiness batch mode (borrowed-view); NULL = inbound-slot */
    void               *hook_ctx;     /* arm/disarm/pull/view_pull share this */
    KlDgramRecvDeliverFn deliver; void *deliver_ctx;
    /* state */
    int    inited;
    int    started;                   /* kl_dgram_recv_start was called (regardless of callback) */
    int    paused;
    int    stopped;                   /* logical stop (no future delivery/re-arm) */
    int    error;                     /* a failed/contract-violating receive (exposed to the close coordinator) */
    int    recv_inflight;             /* completion: op posted; readiness: interest armed */
    /* held completion (completion mode, paused): always a VALID datagram (failures are never held) */
    int    held;
    size_t held_len;
    /* inline-completion iterative trampoline (mirror stream read) */
    int    arming;
    int    rearm_pending;
    int    completed_inline;
    /* Close busy handshake (see datagram_send.h): +1 entry / -1 as the LAST action of any public op
     * that can retire or deliver, so the coordinator detaches only when the outermost frame unwinds. */
    void (*on_activity)(void *ctx, int delta); void *activity_ctx;
} KlDgramRecv;

/* Wire the receive machine over a borrowed KlDgramInbound (the dedicated inbound slot). `completion`
 * selects the model. Requires deliver + arm; readiness additionally requires disarm + pull. No
 * allocation. 0 / -1. */
int  kl_dgram_recv_init(KlDgramRecv *r, KlDgramInbound *inbound, int completion,
                        KlDgramRecvDeliverFn deliver, void *deliver_ctx,
                        KlDgramRecvArmFn arm, KlDgramRecvDisarmFn disarm,
                        KlDgramRecvPullFn pull, void *hook_ctx);

/* Enable the readiness borrowed-view batch mode: the readable loop + resume yield logical
 * datagrams via `view_pull` (from the batch cursor) instead of the inbound slot. Readiness only; NULL
 * reverts to the inbound-slot path. Set BEFORE kl_dgram_recv_start (at batch attach). */
void kl_dgram_recv_set_view_pull(KlDgramRecv *r, KlDgramRecvViewFn view_pull, void *ctx);

/* Begin receiving (arm the first receive). 0 / -1. */
int  kl_dgram_recv_start(KlDgramRecv *r);

/* Completion-mode: a posted recv finished with `len` bytes in the inbound slot (ok=0 = failed).
 * May be called inline from the arm hook. Duplicate/spurious (none in flight) are dropped. 0 / -1. */
int  kl_dgram_recv_on_complete(KlDgramRecv *r, size_t len, int ok);

/* Readiness-mode: a READ event fired; drain serially (pull→deliver), re-checking paused/stopped
 * after every delivery. 0 / -1. */
int  kl_dgram_recv_on_readable(KlDgramRecv *r);

/* Strict pause: readiness drops interest (nothing completes); completion leaves a posted recv in
 * flight (held on completion). Idempotent (incl. from the delivery callback). */
void kl_dgram_recv_pause(KlDgramRecv *r);
/* Resume: deliver a held datagram exactly once, then re-arm. Idempotent. 0 / -1. */
int  kl_dgram_recv_resume(KlDgramRecv *r);

/* Logical stop: no future delivery/re-arm; discard any held datagram; readiness drops interest.
 * A physically-outstanding completion recv retires on its (dropped) completion. Idempotent. */
void kl_dgram_recv_stop(KlDgramRecv *r);

/* Close: on_activity(delta) brackets every public op that can retire or deliver (+1 entry,
 * -1 as the LAST action); the coordinator detaches only once the outermost frame unwinds. */
void kl_dgram_recv_set_activity_cb(KlDgramRecv *r, void (*cb)(void *ctx, int delta), void *ctx);

/* Free/zero the object (borrows no heap). REFUSES with -1 while a receive is outstanding (a posted
 * completion op / armed readiness interest references the inbound slot). */
int  kl_dgram_recv_free(KlDgramRecv *r);

static inline int kl_dgram_recv_started(const KlDgramRecv *r)  { return (r && r->started) ? 1 : 0; }
static inline int kl_dgram_recv_held(const KlDgramRecv *r)     { return (r && r->held) ? 1 : 0; }
static inline int kl_dgram_recv_inflight(const KlDgramRecv *r) { return (r && r->recv_inflight) ? 1 : 0; }
/* 1 once a receive FAILED (provider error) or a delivery violated the contract (peer absent): the
 * recv side is stopped and NOTHING was delivered. Note an oversized datagram is NOT an error: it is
 * delivered as a captured prefix with KL_DGRAM_TRUNCATED. Confirmed detachment surfaces this. */
static inline int kl_dgram_recv_error(const KlDgramRecv *r)    { return (r && r->error) ? 1 : 0; }

#endif /* KEEL_SRC_DATAGRAM_RECV_H */
