#ifndef KEEL_SRC_DATAGRAM_RECV_H
#define KEEL_SRC_DATAGRAM_RECV_H

/*
 * datagram_recv.h — INTERNAL serial receive + strict pause/resume over the dedicated inbound slot
 * (Phase B, step 3). See docs/datagram_contract.md §2/§4.
 *
 * Exactly one receive is outstanding at a time: completion posts ONE recv op; readiness arms ONE
 * READ source and performs serial provider receives (one datagram per pull), re-checking
 * paused/stopped after every delivery. Received datagrams land in the step-1 DEDICATED INBOUND SLOT
 * (never an outbound slot). Pause is STRICT: readiness drops interest (nothing completes); a
 * completion recv already posted stays in flight and, when it completes, is HELD in the inbound
 * slot; resume delivers the held datagram exactly once, then re-arms. An arm hook MAY complete
 * synchronously (the op is pre-armed and an iterative trampoline bounds the C stack). Duplicate /
 * spurious completions are dropped without disturbing held data. The delivery callback may
 * pause/stop, but MUST NOT free or re-init the object before the (step-4) confirmed-detachment
 * callback — kl_dgram_recv_free refuses while a receive is outstanding.
 *
 * No allocation at all (uses the borrowed inbound slot); no interaction with outbound slot
 * availability. INTERNAL, NOT wired to a live provider or exposed publicly. Truncation delivery
 * semantics are deferred to step 5 — here an over-capacity provider length FAILS SAFELY (the recv
 * is stopped; nothing oversized is delivered).
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include "datagram_slots.h"

#include <keel/sockaddr.h>

#include <stddef.h>

/* Arm the next receive: completion = post ONE recv op into the inbound slot (may synchronously call
 * kl_dgram_recv_on_complete); readiness = add READ interest. Returns 0 armed/posted, -1 on failure. */
typedef int  (*KlDgramRecvArmFn)(void *ctx);
/* Readiness only: remove READ interest (idempotent at the provider). */
typedef void (*KlDgramRecvDisarmFn)(void *ctx);
/* Readiness only: receive ONE datagram into the inbound slot (fill data/len/peer/local/flags).
 * Returns 1 = one datagram (bytes in *out_len), 0 = would-block (drained), -1 = fatal error. */
typedef int  (*KlDgramRecvPullFn)(void *ctx, size_t *out_len);
/* Deliver one received datagram (borrowed from the inbound slot; valid only for the call). */
typedef void (*KlDgramRecvDeliverFn)(void *ctx, const void *data, size_t len,
                                     const KlSockAddr *peer, const KlSockAddr *local, unsigned flags);

typedef struct {
    KlDgramSlots       *slots;        /* borrowed — the INBOUND slot only */
    int                 completion;   /* 1 = completion (post/on_complete); 0 = readiness (arm/pull) */
    KlDgramRecvArmFn    arm;
    KlDgramRecvDisarmFn disarm;       /* required for readiness */
    KlDgramRecvPullFn   pull;         /* required for readiness */
    void               *hook_ctx;     /* arm/disarm/pull share this */
    KlDgramRecvDeliverFn deliver; void *deliver_ctx;
    /* state */
    int    inited;
    int    paused;
    int    stopped;                   /* logical stop (no future delivery/re-arm) */
    int    recv_inflight;             /* completion: op posted; readiness: interest armed */
    /* held completion (completion mode, paused) */
    int    held;
    size_t held_len;
    int    held_ok;
    /* inline-completion iterative trampoline (mirror stream read) */
    int    arming;
    int    rearm_pending;
    int    completed_inline;
} KlDgramRecv;

/* Wire the receive machine over a borrowed KlDgramSlots. `completion` selects the model. Requires
 * deliver + arm; readiness additionally requires disarm + pull. No allocation. 0 / -1. */
int  kl_dgram_recv_init(KlDgramRecv *r, KlDgramSlots *slots, int completion,
                        KlDgramRecvDeliverFn deliver, void *deliver_ctx,
                        KlDgramRecvArmFn arm, KlDgramRecvDisarmFn disarm,
                        KlDgramRecvPullFn pull, void *hook_ctx);

/* Begin receiving (arm the first receive). 0 / -1. */
int  kl_dgram_recv_start(KlDgramRecv *r);

/* Completion-mode: a posted recv finished with `len` bytes in the inbound slot (ok=0 = failed).
 * May be called inline from the arm hook. Duplicate/spurious (none in flight) are dropped. 0 / -1. */
int  kl_dgram_recv_on_complete(KlDgramRecv *r, size_t len, int ok);

/* Readiness-mode: a READ event fired — drain serially (pull→deliver), re-checking paused/stopped
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

/* Free/zero the object (borrows no heap). REFUSES with -1 while a receive is outstanding (a posted
 * completion op / armed readiness interest references the inbound slot). */
int  kl_dgram_recv_free(KlDgramRecv *r);

static inline int kl_dgram_recv_held(const KlDgramRecv *r)     { return (r && r->held) ? 1 : 0; }
static inline int kl_dgram_recv_inflight(const KlDgramRecv *r) { return (r && r->recv_inflight) ? 1 : 0; }

#endif /* KEEL_SRC_DATAGRAM_RECV_H */
