#ifndef KEEL_SRC_DATAGRAM_CLOSE_H
#define KEEL_SRC_DATAGRAM_CLOSE_H

/*
 * datagram_close.h — INTERNAL close/cancel + confirmed-detachment lifecycle over KlDgramSend and
 * KlDgramRecv (Phase B, step 4). See docs/datagram_contract.md §6.
 *
 * Coordinates the terminal lifecycle of the (borrowed) send + receive machines. Its one invariant:
 * on_close fires EXACTLY ONCE, only after the machine gate holds — recv in-flight 0, send in-flight 0,
 * and (for a GRACEFUL close) the outbound queue drained (or undrainable via a sticky error, so a
 * failure never wedges the close). on_close carries a TERMINAL CLASSIFICATION (§4.3), NOT a single
 * "detached" fact: KL_DGRAM_DETACHED is confirmed physical retirement of every op; KL_DGRAM_QUARANTINED
 * means an op could NOT be confirmed retired (fail-closed) — the wrapper is safe to release but the
 * op's storage stays pinned; KL_DGRAM_CLOSE_ERROR is a transport error with every op nonetheless
 * RETIRED. A still-PENDING classifier keeps the state CLOSING (see kl_dgram_close_retry).
 *
 *  - Graceful close (kl_dgram_close_begin): refuse new sends, stop receiving, drain queued output.
 *  - Abortive cancel (kl_dgram_close_cancel): discard queued-but-unsubmitted datagrams and request
 *    cancellation of any physically-outstanding recv/send op (each at most once). Graceful may
 *    escalate to abortive.
 *  - Cancellation hooks may retire their op synchronously (inline) or re-enter close/cancel; a depth
 *    guard (in_close_cancel) defers detachment until the outermost cancel frame unwinds, so
 *    detachment never happens inside a provider hook.
 *  - on_close is a DESTRUCTIVE TAIL: state is CLOSED before it fires and the coordinator makes no
 *    self-access afterwards (the callback may free/tear down everything).
 *  - Free is refused before a terminal classification (state CLOSED) — which DETACHED and
 *    QUARANTINED both reach (a quarantine releases the wrapper without physical retirement).
 *
 * Readiness disarm (recv pause/stop) is a SYNCHRONOUS physical retirement; a completion op requires
 * its actual terminal completion (on_complete) — surfaced here via each machine's on_retire hook.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include "datagram_send.h"
#include "datagram_recv.h"
#include "datagram_life.h"   /* KlDgramOpKind + KlDgramRetireResult (shared with the completion seam) */

typedef enum {
    KL_DGRAM_CLOSE_OPEN = 0,       /* lifecycle PHASE (not the terminal result) */
    KL_DGRAM_CLOSE_CLOSING,
    KL_DGRAM_CLOSE_CLOSED
} KlDgramCloseState;

/* Terminal CLASSIFICATION of a close (docs/datagram_step7_public_api_design.md §4.2). NONE=0 is the
 * sentinel: a not-yet-CLOSED (or zeroed/reused) object reports NONE, never a spurious DETACHED. */
typedef enum {
    KL_DGRAM_CLOSE_NONE = 0,   /* no terminal reached yet (also the zeroed-object value) */
    KL_DGRAM_DETACHED,         /* CONFIRMED physical retirement of every op (the strong guarantee) */
    KL_DGRAM_QUARANTINED,      /* wrapper-safe but an op could NOT be confirmed retired (fail-closed) */
    KL_DGRAM_CLOSE_ERROR       /* terminal transport error AND every op nevertheless RETIRED (§4.3) */
} KlDatagramCloseResult;

/* Per-op retirement classifier the backend reports to the coordinator (§4.3): the coordinator joins
 * only once no op is PENDING. KlDgramRetireResult + KlDgramOpKind now live in datagram_life.h (shared
 * with the completion seam KlCompletionOps.retire_dgram). */
typedef KlDgramRetireResult (*KlDgramRetireFn)(void *ctx, KlDgramOpKind kind, int *transport_err);

typedef void (*KlDgramCloseFn)(void *ctx, KlDatagramCloseResult result);  /* detachment (fires once) */
typedef void (*KlDgramCancelFn)(void *ctx);   /* request cancel of the outstanding recv / send op */
/* Backend retirement + fd close — the single point that physically retires the transport and closes
 * the fd, run EXACTLY ONCE once the outbound queue has drained (graceful) or been discarded (abortive).
 * For a completion backend this is where the outstanding recv (a datagram recv has no natural EOF)
 * is finally retired and, for EFI, where RETIRED vs QUARANTINED is decided. */
typedef void (*KlDgramBackendCloseFn)(void *ctx);

typedef struct {
    KlDgramSend *send;   /* borrowed (may be NULL) */
    KlDgramRecv *recv;   /* borrowed (may be NULL) */
    int          state;
    int          abort;
    int          notified;
    int          busy;         /* active send/recv/coordinator frames — detach only at 0 (handshake) */
    int          detaching;    /* reentrancy guard while the terminal logic runs */
    int          recv_cancel_requested, send_cancel_requested;
    int          backend_retired;   /* backend-close/retirement step ran (exactly once) */
    KlDgramCancelFn cancel_recv, cancel_send; void *cancel_ctx;
    KlDgramBackendCloseFn backend_close; void *backend_ctx;   /* fd close + transport retirement, once */
    KlDgramRetireFn retire; void *retire_ctx;   /* per-op backend classifier (NULL → always DETACHED) */
    KlDatagramCloseResult result;               /* the terminal classification (valid once CLOSED) */
    KlDgramCloseFn  on_close; void *close_ctx;
} KlDgramClose;

/* Wire the coordinator over the send + recv machines (at least one required). Installs each
 * machine's retire hook so async retirements drive finalization. 0 / -1. */
int  kl_dgram_close_init(KlDgramClose *c, KlDgramSend *send, KlDgramRecv *recv,
                         KlDgramCloseFn on_close, void *close_ctx);

/* Cancellation hooks for abortive cancel (frozen once close begins). 0 / -1. */
int  kl_dgram_close_set_cancel(KlDgramClose *c, KlDgramCancelFn cancel_recv,
                               KlDgramCancelFn cancel_send, void *ctx);

/* Backend per-op retirement classifier (§4.3). Frozen once close begins. If unset, every op is
 * treated as confirmed RETIRED → the terminal result is DETACHED (the readiness/pure-machine model,
 * and KlUdp's legacy path). 0 / -1. */
int  kl_dgram_close_set_retire(KlDgramClose *c, KlDgramRetireFn retire, void *ctx);

/* Backend retirement + fd-close hook (§4.1), run EXACTLY ONCE once the outbound queue has drained
 * (graceful) or been discarded (abortive) — regardless of recv state, since a datagram recv has no
 * natural EOF and this step is what retires it. Owns the physical fd close. Frozen once close begins.
 * If unset (KlUdp's legacy path, or a coordinator driven purely by machine retirements), no fd is
 * closed here. 0 / -1. */
int  kl_dgram_close_set_backend_close(KlDgramClose *c, KlDgramBackendCloseFn fn, void *ctx);

/* Graceful close (drain queued output) / abortive cancel (discard + cancel outstanding). Idempotent;
 * graceful may later escalate via cancel. 0 / -1. */
int  kl_dgram_close_begin(KlDgramClose *c);
int  kl_dgram_close_cancel(KlDgramClose *c);

/* Re-evaluate the terminal classification (the backend-drain PROGRESS hook). The machine gate
 * (close_fully_retired) can hold while the §4.3 classifier still returns PENDING — the send/recv
 * machines are then already at zero, so NO machine retirement is guaranteed to wake the close. A
 * backend that later resolves that PENDING op's retirement (e.g. a quarantine finally confirmed, or a
 * cancel-token terminal drained out-of-band) calls this to re-run the terminal logic. Idempotent and
 * safe any time: a no-op unless the state is CLOSING. 0 / -1. */
int  kl_dgram_close_retry(KlDgramClose *c);

KlDgramCloseState kl_dgram_close_state(const KlDgramClose *c);
int  kl_dgram_close_is_detached(const KlDgramClose *c);

/* The terminal classification (§4.2). KL_DGRAM_CLOSE_NONE until state == CLOSED; then DETACHED,
 * QUARANTINED, or CLOSE_ERROR. Also delivered as the on_close argument. */
KlDatagramCloseResult kl_dgram_close_result(const KlDgramClose *c);

/* Free/zero (borrows no heap). REFUSED with -1 before a terminal classification (state CLOSED —
 * DETACHED or QUARANTINED, both wrapper-safe to release). After CLOSED, tear
 * down send + recv + this together with no further operations (the retire wiring becomes inert). */
int  kl_dgram_close_free(KlDgramClose *c);

#endif /* KEEL_SRC_DATAGRAM_CLOSE_H */
