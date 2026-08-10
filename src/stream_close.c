/*
 * stream_close.c — Phase-B KlStream graceful-close / confirmed-detachment lifecycle (step 3).
 * See stream_close.h.
 *
 * The whole module exists to enforce one invariant: on_close (detachment) fires exactly once,
 * only after BOTH the receive and the send operation are PHYSICALLY retired. It reads the write
 * machinery's send_inflight (src/stream_write.c) and the read machinery's recv_inflight
 * (src/stream_read.c) directly, and drives finalization from the retirement points via the
 * generic on_retire hook it installs at init — so the read/write TUs carry no static dependency
 * on this module.
 *
 * Cancellation obeys the same synchronous-completion discipline as the step-2B arm trampoline:
 * a cancel hook may retire its op inline (calling on_recv/on_write_complete, which fire on_retire
 * → finalize). The in_close_cancel sentinel defers that finalize until the cancel calls unwind,
 * so on_close cannot fire re-entrantly or twice.
 */
#include "stream_close.h"
#include "stream_write.h"      /* kl_stream_write_pending */
#include "stream_read.h"       /* kl_stream_read_close */

/* Fire the detachment callback exactly once. State moves to CLOSED before the callback so any
 * stray later on_retire (a late completion) is a no-op. The callback may free/reuse the stream. */
static void stream_detach(KlStream *s) {
    s->close_state = KL_STREAM_STATE_CLOSED;
    if (!s->close_notified) {
        s->close_notified = 1;
        if (s->on_close) s->on_close(s->close_ctx);   /* reuse legal only after this returns */
    }
}

/* Are both operations physically retired? The receive is retired when no recv is armed/posted
 * (recv_inflight == 0); the send when no async send is in flight (send_inflight == 0). For a
 * GRACEFUL close we additionally require the write queue to have fully drained (nothing left to
 * send); an ABORTIVE close does not wait on the queue — queued-but-unsubmitted bytes are the
 * stream's own memory, freed by the owner at on_close. */
static int stream_fully_retired(const KlStream *s) {
    if (s->recv_inflight) return 0;                    /* a receive op is still outstanding */
    if (s->send_inflight) return 0;                    /* a send op is still outstanding */
    if (!s->close_abort && kl_stream_write_pending(s) > 0) return 0; /* graceful: drain first */
    return 1;
}

/* Re-check the close condition and detach if it holds. Deferred while ANY cancel frame is on the
 * stack (in_close_cancel depth > 0, incl. a reentrant kl_stream_cancel from a hook) — the
 * OUTERMOST cancel frame's unwinding re-attempts it. No-op unless CLOSING. */
static void stream_close_finalize(KlStream *s) {
    if (s->close_state != KL_STREAM_STATE_CLOSING) return;   /* not closing, or already detached */
    if (s->in_close_cancel) return;                    /* defer past the (possibly nested) cancel window */
    if (!stream_fully_retired(s)) return;              /* an op is still physically outstanding */
    stream_detach(s);
}

/* Generic retirement notification installed on the stream at init: the write machinery
 * (on_write_complete / flush) and the read machinery (on_recv) call this whenever a physical op
 * retires, driving finalization without any static dependency on this TU. */
static void stream_on_retire(KlStream *s) {
    stream_close_finalize(s);
}

int kl_stream_close_init(KlStream *s, KlStreamCloseFn on_close, void *ctx) {
    if (!s || s->close_inited || !on_close) return -1;
    s->close_state    = KL_STREAM_STATE_OPEN;
    s->close_abort    = 0;
    s->in_close_cancel = 0;
    s->close_notified = 0;
    s->wq_closing     = 0;
    s->recv_cancel_requested = 0;
    s->send_cancel_requested = 0;
    s->on_close       = on_close;
    s->close_ctx      = ctx;
    s->cancel_recv    = NULL;
    s->cancel_send    = NULL;
    s->on_retire      = stream_on_retire;   /* wire read/write → finalize */
    s->close_inited   = 1;
    return 0;
}

int kl_stream_set_cancel(KlStream *s, KlStreamCancelFn cancel_recv, KlStreamCancelFn cancel_send) {
    if (!s || !s->close_inited) return -1;
    if (s->close_state != KL_STREAM_STATE_OPEN) return -1;  /* frozen once closing begins: changing
                                                               provider hooks mid-cancel would weaken
                                                               the lifecycle contract */
    s->cancel_recv = cancel_recv;
    s->cancel_send = cancel_send;
    return 0;
}

/* Shared close body. `abort` selects graceful (0) vs abortive (1). Logically closes the read side,
 * refuses new writes, optionally issues cancels (guarded by in_close_cancel so a synchronous
 * cancel completion cannot detach mid-cancel), then finalizes once. */
static int stream_close_common(KlStream *s, int abort) {
    if (!s || !s->close_inited) return -1;
    if (s->close_state == KL_STREAM_STATE_CLOSED) return 0;  /* already detached — idempotent */

    if (abort) s->close_abort = 1;                     /* graceful → abortive escalation is allowed */
    if (s->close_state == KL_STREAM_STATE_OPEN) {
        s->close_state = KL_STREAM_STATE_CLOSING;
        s->wq_closing  = 1;                            /* refuse new writes (kl_stream_write → CLOSED) */
        if (s->read_inited)
            kl_stream_read_close(s);                   /* logical read close; readiness retires recv */
    }

    /* Abortive: request cancellation of any physically-outstanding op — AT MOST ONCE per op
     * (recv/send_cancel_requested). The requested flag is set BEFORE the hook so a synchronous
     * retirement is safe; a repeated kl_stream_cancel() while the op is still outstanding does NOT
     * re-invoke the hook. A hook may retire its op synchronously (→ on_retire → finalize) or even
     * re-enter kl_stream_cancel; both are DEFERRED by the in_close_cancel DEPTH counter and the
     * finalize is re-attempted when the outermost cancel frame unwinds — so on_close fires at most
     * once and never mid-cancellation. */
    if (s->close_abort) {
        s->in_close_cancel++;
        if (s->recv_inflight && s->cancel_recv && !s->recv_cancel_requested) {
            s->recv_cancel_requested = 1;
            s->cancel_recv(s->close_ctx);
        }
        if (s->send_inflight && s->cancel_send && !s->send_cancel_requested) {
            s->send_cancel_requested = 1;
            s->cancel_send(s->close_ctx);
        }
        s->in_close_cancel--;
    }

    stream_close_finalize(s);   /* detach now if both ops are already retired (and not deferred) */
    return 0;
}

int kl_stream_close_begin(KlStream *s) { return stream_close_common(s, /*abort=*/0); }
int kl_stream_cancel(KlStream *s)      { return stream_close_common(s, /*abort=*/1); }

KlStreamCloseState kl_stream_close_state(const KlStream *s) {
    return s ? (KlStreamCloseState)s->close_state : KL_STREAM_STATE_CLOSED;
}

int kl_stream_is_detached(const KlStream *s) {
    return (s && s->close_notified) ? 1 : 0;
}
