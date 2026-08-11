#ifndef KEEL_SRC_DATAGRAM_CLOSE_H
#define KEEL_SRC_DATAGRAM_CLOSE_H

/*
 * datagram_close.h — INTERNAL close/cancel + confirmed-detachment lifecycle over KlDgramSend and
 * KlDgramRecv (Phase B, step 4). See docs/datagram_contract.md §6.
 *
 * Coordinates the terminal lifecycle of the (borrowed) send + receive machines. Its one invariant:
 * on_close fires EXACTLY ONCE, only after the receive is physically retired (recv in-flight 0), the
 * send is physically retired (send in-flight 0), and — for a GRACEFUL close — the outbound queue has
 * drained (or become undrainable via a sticky error, so a failure never wedges the close).
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
 *  - Free is refused before confirmed detachment.
 *
 * Readiness disarm (recv pause/stop) is a SYNCHRONOUS physical retirement; a completion op requires
 * its actual terminal completion (on_complete) — surfaced here via each machine's on_retire hook.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include "datagram_send.h"
#include "datagram_recv.h"

typedef enum {
    KL_DGRAM_CLOSE_OPEN = 0,
    KL_DGRAM_CLOSE_CLOSING,
    KL_DGRAM_CLOSE_CLOSED
} KlDgramCloseState;

typedef void (*KlDgramCloseFn)(void *ctx);    /* detachment (fires exactly once) */
typedef void (*KlDgramCancelFn)(void *ctx);   /* request cancel of the outstanding recv / send op */

typedef struct {
    KlDgramSend *send;   /* borrowed (may be NULL) */
    KlDgramRecv *recv;   /* borrowed (may be NULL) */
    int          state;
    int          abort;
    int          notified;
    int          in_close_cancel;             /* depth guard — defer detach past cancel frames */
    int          recv_cancel_requested, send_cancel_requested;
    KlDgramCancelFn cancel_recv, cancel_send; void *cancel_ctx;
    KlDgramCloseFn  on_close; void *close_ctx;
} KlDgramClose;

/* Wire the coordinator over the send + recv machines (at least one required). Installs each
 * machine's retire hook so async retirements drive finalization. 0 / -1. */
int  kl_dgram_close_init(KlDgramClose *c, KlDgramSend *send, KlDgramRecv *recv,
                         KlDgramCloseFn on_close, void *close_ctx);

/* Cancellation hooks for abortive cancel (frozen once close begins). 0 / -1. */
int  kl_dgram_close_set_cancel(KlDgramClose *c, KlDgramCancelFn cancel_recv,
                               KlDgramCancelFn cancel_send, void *ctx);

/* Graceful close (drain queued output) / abortive cancel (discard + cancel outstanding). Idempotent;
 * graceful may later escalate via cancel. 0 / -1. */
int  kl_dgram_close_begin(KlDgramClose *c);
int  kl_dgram_close_cancel(KlDgramClose *c);

KlDgramCloseState kl_dgram_close_state(const KlDgramClose *c);
int  kl_dgram_close_is_detached(const KlDgramClose *c);

/* Free/zero (borrows no heap). REFUSED with -1 before confirmed detachment. After detachment, tear
 * down send + recv + this together with no further operations (the retire wiring becomes inert). */
int  kl_dgram_close_free(KlDgramClose *c);

#endif /* KEEL_SRC_DATAGRAM_CLOSE_H */
