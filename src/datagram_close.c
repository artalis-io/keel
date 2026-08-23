/*
 * datagram_close.c — INTERNAL close/cancel + confirmed-detachment lifecycle.
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

/* Fire detachment exactly once with its terminal classification. State is CLOSED + result recorded
 * before the callback so a stray later frame is a no-op; the callback may free/tear down everything
 * (no self-access after it returns). */
static void close_detach(KlDgramClose *c, KlDatagramCloseResult result) {
    c->state  = KL_DGRAM_CLOSE_CLOSED;
    if (!c->notified) {
        c->notified = 1;
        c->result   = result;
        if (c->on_close) c->on_close(c->close_ctx, result);   /* DESTRUCTIVE TAIL — no c access after */
    }
}

/* recv retired + send retired + outbound queue empty (the machine-level gate: no op physically
 * outstanding). Only once this holds does the backend classifier resolve RETIRED vs
 * QUARANTINED. */
static int close_fully_retired(const KlDgramClose *c) {
    if (c->recv && kl_dgram_recv_inflight(c->recv)) return 0;
    if (c->send && kl_dgram_send_inflight(c->send)) return 0;
    if (c->send && kl_dgram_send_queued(c->send) > 0) return 0;
    return 1;
}

/* The outbound side has quiesced — queue empty and no in-flight send. This (NOT full retirement) is
 * the frozen trigger for backend retirement + fd close: a datagram RECV has no natural EOF, so the fd
 * close is what finally retires it; for EFI closing the child is also what CLASSIFIES the ops. */
static int close_send_drained(const KlDgramClose *c) {
    if (c->send && kl_dgram_send_inflight(c->send)) return 0;
    if (c->send && kl_dgram_send_queued(c->send) > 0) return 0;
    return 1;
}

/* The §4.3 object-result join, computed only when fully retired. Without a backend classifier every
 * op is confirmed RETIRED → DETACHED. With one: any QUARANTINED op ⇒ QUARANTINED (retain its life ref
 * forever); else a transport error with ALL ops RETIRED ⇒ CLOSE_ERROR; else DETACHED. A classifier
 * that still reports PENDING ⇒ KL_DGRAM_CLOSE_NONE (stay CLOSING — re-run via a machine retirement OR
 * an explicit kl_dgram_close_retry() from the backend drain path).
 * CLOSE_ERROR is legal ONLY when every op is RETIRED, so a failed close never frees storage whose
 * ownership is unknown (that is what QUARANTINED is for). */
static KlDatagramCloseResult close_join_result(KlDgramClose *c) {
    if (!c->retire)
        return KL_DGRAM_DETACHED;
    int quarantined = 0, transport_err = 0;
    const KlDgramOpKind kinds[2] = { KL_DGRAM_OP_RECV, KL_DGRAM_OP_SEND };
    const int present[2] = { c->recv != NULL, c->send != NULL };
    for (int i = 0; i < 2; i++) {
        if (!present[i]) continue;
        int te = 0;
        KlDgramRetireResult r = c->retire(c->retire_ctx, kinds[i], &te);
        if (r == KL_DGRAM_RETIRE_PENDING)     return KL_DGRAM_CLOSE_NONE;   /* stay CLOSING */
        if (r == KL_DGRAM_RETIRE_QUARANTINED) quarantined = 1;
        if (te)                               transport_err = 1;
    }
    if (quarantined)   return KL_DGRAM_QUARANTINED;
    if (transport_err) return KL_DGRAM_CLOSE_ERROR;   /* all RETIRED + a transport error */
    return KL_DGRAM_DETACHED;
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
    /* Backend retirement + fd close — EXACTLY ONCE, the moment the outbound side has quiesced (frozen
     * §4.1). A datagram recv has no natural EOF, so this is where the still-outstanding recv is retired
     * (cancel_recv) and the fd is closed; for EFI closing the child is also what CLASSIFIES the ops.
     * The busy/detaching handshake makes a synchronous recv retirement inside these hooks re-enter
     * safely (deferred to this frame). */
    if (!c->backend_retired && (close_send_drained(c) || c->abandon)) {
        c->backend_retired = 1;
        if (c->recv && kl_dgram_recv_inflight(c->recv) &&
            c->cancel_recv && !c->recv_cancel_requested) {
            c->recv_cancel_requested = 1;
            c->cancel_recv(c->cancel_ctx);
        }
        if (c->backend_close) c->backend_close(c->backend_ctx);   /* close fd exactly once */
    }
    /* Machine-level gate (no op physically outstanding), THEN classify the outcome. A classifier
     * still reporting PENDING yields NONE → stay CLOSING; the next retirement re-runs this.
     * OWNER-DESTRUCTION (abandon, §4a): FORCE the terminal even with an in-flight op — the op is
     * harmless (recv buffer is life-token-owned + outlives; send payload is a backend-owned copy) and the
     * token is marked dead in the terminal so late completions drop. Honour the classifier for QUARANTINE
     * (the op's ref stays retained / pinned); a still-PENDING op is abandoned as DETACHED. */
    KlDatagramCloseResult res = KL_DGRAM_CLOSE_NONE;
    if (c->abandon) {
        res = close_join_result(c);
        if (res == KL_DGRAM_CLOSE_NONE) res = KL_DGRAM_DETACHED;
    } else if (close_fully_retired(c)) {
        res = close_join_result(c);
    }
    c->detaching = 0;
    if (res != KL_DGRAM_CLOSE_NONE) close_detach(c, res);   /* destructive tail */
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

/* Public frame bracket for a caller (the readiness dispatch) that runs send/recv ops AND then touches the
 * owning handle afterwards: holding the frame keeps `busy > 0` for the whole dispatch, so a teardown
 * requested from within a delivery callback defers its terminal to `kl_dgram_close_release` — the LAST
 * action — instead of firing inside the inner recv/send leave (which would free the handle mid-dispatch).
 * `release` may run the terminal (and, for an abandon, free the object), so it must be the caller's last
 * touch of any state reachable through this close machine. */
void kl_dgram_close_hold(KlDgramClose *c)    { if (c) close_enter(c); }
void kl_dgram_close_release(KlDgramClose *c) { if (c) close_leave(c); }

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

int kl_dgram_close_set_retire(KlDgramClose *c, KlDgramRetireFn retire, void *ctx) {
    if (!c) return -1;
    if (c->state != KL_DGRAM_CLOSE_OPEN) return -1;   /* frozen once closing begins */
    c->retire     = retire;
    c->retire_ctx = ctx;
    return 0;
}

int kl_dgram_close_set_backend_close(KlDgramClose *c, KlDgramBackendCloseFn fn, void *ctx) {
    if (!c) return -1;
    if (c->state != KL_DGRAM_CLOSE_OPEN) return -1;   /* frozen once closing begins */
    c->backend_close = fn;
    c->backend_ctx   = ctx;
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

/* Owner-destruction abandon (see datagram_close.h §4a). Arms the `abandon` flag, then requests the
 * terminal through close_common — reusing the busy handshake so the forced (silent) terminal fires
 * immediately if no frame is active, or is DEFERRED to the outermost leave if invoked from within a
 * delivery frame. No machine state is freed here (the terminal's destructive-tail reclaim does that at
 * the safe point). */
int kl_dgram_close_abandon(KlDgramClose *c) {
    if (!c) return -1;
    if (c->state == KL_DGRAM_CLOSE_CLOSED) return 0;   /* already terminal — idempotent */
    c->abandon = 1;
    return close_common(c, /*abort=*/1);   /* stop/cancel/discard, then close_leave → (deferred) terminal */
}

/* Backend-drain PROGRESS hook (§4.3): re-run the terminal logic when a PENDING op's retirement may
 * have resolved out-of-band. Uses the busy handshake so it composes with any in-flight frame (a
 * reentrant call defers to the outermost unwind). Idempotent; a no-op unless CLOSING. */
int kl_dgram_close_retry(KlDgramClose *c) {
    if (!c) return -1;
    if (c->state != KL_DGRAM_CLOSE_CLOSING) return 0;
    close_enter(c);
    close_leave(c);   /* re-runs close_run_terminal → re-queries the classifier on the outermost unwind */
    return 0;
}

KlDgramCloseState kl_dgram_close_state(const KlDgramClose *c) {
    return c ? (KlDgramCloseState)c->state : KL_DGRAM_CLOSE_CLOSED;
}
int kl_dgram_close_is_detached(const KlDgramClose *c) {
    return (c && c->notified) ? 1 : 0;
}
KlDatagramCloseResult kl_dgram_close_result(const KlDgramClose *c) {
    return c ? c->result : KL_DGRAM_CLOSE_NONE;
}

int kl_dgram_close_free(KlDgramClose *c) {
    if (!c)
        return 0;
    if (c->state != KL_DGRAM_CLOSE_CLOSED)
        return -1;                    /* refuse before a terminal classification (DETACHED/QUARANTINED) */
    memset(c, 0, sizeof(*c));         /* borrows no heap; send/recv are the caller's to free */
    return 0;
}
