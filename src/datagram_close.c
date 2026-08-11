/*
 * datagram_close.c — INTERNAL close/cancel + confirmed-detachment lifecycle (Phase B, step 4).
 * See datagram_close.h. Mirrors stream_close.c: a fully-retired predicate over the borrowed send +
 * recv machines, an in_close_cancel depth guard that defers detachment past (possibly nested)
 * cancellation, cancel-at-most-once per op, graceful→abortive escalation, and a destructive
 * on_close tail. Retirements are surfaced by each machine's on_retire hook (installed at init).
 */
#include "datagram_close.h"

#include <string.h>

/* Fire detachment exactly once. State is CLOSED before the callback so any stray later retirement is
 * a no-op; the callback may free/tear everything down (no self-access after it returns). */
static void close_detach(KlDgramClose *c) {
    c->state = KL_DGRAM_CLOSE_CLOSED;
    if (!c->notified) {
        c->notified = 1;
        if (c->on_close) c->on_close(c->close_ctx);   /* DESTRUCTIVE TAIL — no c access after */
    }
}

/* Both operations physically retired? recv when no recv is armed/posted; send when no async send is
 * in flight. GRACEFUL additionally waits for the outbound queue to drain — UNLESS a sticky send
 * error makes it undrainable, so a failure never leaves the close permanently waiting. */
static int close_fully_retired(const KlDgramClose *c) {
    if (c->recv && kl_dgram_recv_inflight(c->recv)) return 0;
    if (c->send && kl_dgram_send_inflight(c->send)) return 0;
    if (!c->abort && c->send &&
        !kl_dgram_send_error(c->send) && kl_dgram_send_queued(c->send) > 0)
        return 0;                                     /* graceful: drain first (unless errored) */
    return 1;
}

/* Re-check the terminal condition and detach if it holds. Deferred while any cancel frame is on the
 * stack (in_close_cancel > 0). No-op unless CLOSING. */
static void close_finalize(KlDgramClose *c) {
    if (c->state != KL_DGRAM_CLOSE_CLOSING) return;
    if (c->in_close_cancel) return;
    if (!close_fully_retired(c)) return;
    close_detach(c);
}

/* Retire notification installed on the send + recv machines: an async retirement / drain step
 * settled — re-check finalization. */
static void close_on_retire(void *ctx) {
    close_finalize((KlDgramClose *)ctx);
}

int kl_dgram_close_init(KlDgramClose *c, KlDgramSend *send, KlDgramRecv *recv,
                        KlDgramCloseFn on_close, void *close_ctx) {
    if (!c || (!send && !recv))
        return -1;
    memset(c, 0, sizeof(*c));
    c->send      = send;
    c->recv      = recv;
    c->on_close  = on_close;
    c->close_ctx = close_ctx;
    c->state     = KL_DGRAM_CLOSE_OPEN;
    if (send) kl_dgram_send_set_retire_cb(send, close_on_retire, c);
    if (recv) kl_dgram_recv_set_retire_cb(recv, close_on_retire, c);
    return 0;
}

int kl_dgram_close_set_cancel(KlDgramClose *c, KlDgramCancelFn cancel_recv,
                              KlDgramCancelFn cancel_send, void *ctx) {
    if (!c) return -1;
    if (c->state != KL_DGRAM_CLOSE_OPEN) return -1;   /* frozen once closing begins */
    c->cancel_recv = cancel_recv;
    c->cancel_send = cancel_send;
    c->cancel_ctx  = ctx;
    return 0;
}

static int close_common(KlDgramClose *c, int abort) {
    if (!c) return -1;
    if (c->state == KL_DGRAM_CLOSE_CLOSED) return 0;   /* already detached — idempotent */

    if (abort) c->abort = 1;                            /* graceful → abortive escalation allowed */
    if (c->state == KL_DGRAM_CLOSE_OPEN) {
        c->state = KL_DGRAM_CLOSE_CLOSING;
        if (c->send) kl_dgram_send_set_closing(c->send, 1);   /* refuse new sends */
        if (c->recv) kl_dgram_recv_stop(c->recv);             /* stop receiving (readiness disarms) */
    }

    /* Abortive: discard queued-but-unsubmitted output, then request cancellation of any physically
     * outstanding op AT MOST ONCE. The requested flag is set BEFORE the hook so a synchronous
     * retirement (or a reentrant cancel) is safe; the in_close_cancel depth counter defers finalize
     * so on_close cannot fire mid-cancellation or twice. */
    if (c->abort) {
        c->in_close_cancel++;
        if (c->send) kl_dgram_send_discard_queued(c->send);
        if (c->send && kl_dgram_send_inflight(c->send) &&
            c->cancel_send && !c->send_cancel_requested) {
            c->send_cancel_requested = 1;
            c->cancel_send(c->cancel_ctx);
        }
        if (c->recv && kl_dgram_recv_inflight(c->recv) &&
            c->cancel_recv && !c->recv_cancel_requested) {
            c->recv_cancel_requested = 1;
            c->cancel_recv(c->cancel_ctx);
        }
        c->in_close_cancel--;
    }

    close_finalize(c);   /* detach now if already fully retired (and not deferred) */
    return 0;
}

int kl_dgram_close_begin(KlDgramClose *c)  { return close_common(c, /*abort=*/0); }
int kl_dgram_close_cancel(KlDgramClose *c) { return close_common(c, /*abort=*/1); }

KlDgramCloseState kl_dgram_close_state(const KlDgramClose *c) {
    return c ? (KlDgramCloseState)c->state : KL_DGRAM_CLOSE_CLOSED;
}
int kl_dgram_close_is_detached(const KlDgramClose *c) {
    return (c && c->notified) ? 1 : 0;
}

int kl_dgram_close_free(KlDgramClose *c) {
    if (!c)
        return 0;
    if (c->state != KL_DGRAM_CLOSE_CLOSED)
        return -1;                    /* refuse before confirmed detachment */
    memset(c, 0, sizeof(*c));         /* borrows no heap; send/recv are the caller's to free */
    return 0;
}
