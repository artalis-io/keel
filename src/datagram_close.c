/*
 * datagram_close.c — INTERNAL close/cancel + confirmed-detachment lifecycle (Phase B, step 4).
 * See datagram_close.h. Coordinates the borrowed send + recv machines.
 *
 * BUSY HANDSHAKE. The coordinator counts active frames: each send/recv public op brackets itself
 * with on_activity(+1/-1) (installed at init) and every coordinator entry brackets its own body.
 * Detachment (the destructive on_close) runs ONLY when `busy` returns to 0 — i.e. once the OUTERMOST
 * send/recv provider/callback or coordinator frame has unwound — so a close/cancel initiated from
 * any callback (delivery, on_writable, on_drain, an inline arm/submit) is deferred, and no machine
 * or coordinator state is touched after on_close.
 *
 * ONE TERMINAL PREDICATE. Detach requires recv in-flight 0, send in-flight 0, AND the outbound queue
 * empty. The queue reaches empty by graceful drain OR abortive discard; a send error during a
 * graceful close ESCALATES to abortive cleanup (discard + cancel), so a failure never wedges the
 * close and slot ownership is never left implicit.
 */
#include "datagram_close.h"

#include <string.h>

/* Fire detachment exactly once. State is CLOSED before the callback so a stray later frame is a
 * no-op; the callback may free/tear down everything (no self-access after it returns). */
static void close_detach(KlDgramClose *c) {
    c->state = KL_DGRAM_CLOSE_CLOSED;
    if (!c->notified) {
        c->notified = 1;
        if (c->on_close) c->on_close(c->close_ctx);   /* DESTRUCTIVE TAIL — no c access after */
    }
}

/* recv retired + send retired + outbound queue empty. */
static int close_fully_retired(const KlDgramClose *c) {
    if (c->recv && kl_dgram_recv_inflight(c->recv)) return 0;
    if (c->send && kl_dgram_send_inflight(c->send)) return 0;
    if (c->send && kl_dgram_send_queued(c->send) > 0) return 0;
    return 1;
}

/* Abortive cleanup: discard queued-but-unsubmitted output and request cancellation of any
 * physically-outstanding op, each AT MOST ONCE. Used by kl_dgram_close_cancel and by the graceful
 * send-error escalation. Nested machine ops keep busy > 0, so they cannot detach mid-cleanup. */
static void close_abort_cleanup(KlDgramClose *c) {
    c->abort = 1;
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
}

/* The terminal logic — runs ONLY when busy returns to 0 (outermost frame unwound). A `detaching`
 * guard blocks reentry from a machine op that a cleanup cancel retires synchronously. */
static void close_run_terminal(KlDgramClose *c) {
    if (c->state != KL_DGRAM_CLOSE_CLOSING) return;
    if (c->detaching) return;
    c->detaching = 1;
    /* A send error during a graceful close makes the queue undrainable → escalate to abortive
     * cleanup (discard + cancel) so the close never waits forever and slots are reclaimed. */
    if (!c->abort && c->send && kl_dgram_send_error(c->send) &&
        (kl_dgram_send_inflight(c->send) || kl_dgram_send_queued(c->send) > 0)) {
        close_abort_cleanup(c);
    }
    int done = close_fully_retired(c);
    c->detaching = 0;
    if (done) close_detach(c);   /* destructive tail */
}

/* Enter/leave a frame. Detach is attempted only as the last frame unwinds (busy 1 → 0). */
static void close_enter(KlDgramClose *c) { c->busy++; }
static void close_leave(KlDgramClose *c) { if (--c->busy == 0) close_run_terminal(c); }

/* Activity notification installed on the send + recv machines. */
static void close_activity(void *ctx, int delta) {
    KlDgramClose *c = ctx;
    if (delta > 0) close_enter(c);
    else           close_leave(c);
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
    if (send) kl_dgram_send_set_activity_cb(send, close_activity, c);
    if (recv) kl_dgram_recv_set_activity_cb(recv, close_activity, c);
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

    close_enter(c);   /* our own frame — machine ops we call below cannot detach mid-body */
    if (abort) c->abort = 1;                            /* graceful → abortive escalation allowed */
    if (c->state == KL_DGRAM_CLOSE_OPEN) {
        c->state = KL_DGRAM_CLOSE_CLOSING;
        if (c->send) kl_dgram_send_set_closing(c->send, 1);   /* refuse new sends */
        if (c->recv) kl_dgram_recv_stop(c->recv);             /* stop receiving (readiness disarms) */
    }
    if (c->abort)
        close_abort_cleanup(c);
    close_leave(c);   /* attempt detach now if this is the outermost frame and fully retired */
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
