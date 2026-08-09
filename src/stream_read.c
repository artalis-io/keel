/*
 * stream_read.c — Phase-B KlStream strict read pause/resume machinery (step 2B). See
 * stream_read.h. Model-agnostic: received bytes land in the stable read_buf; while paused a
 * completed receive (bytes or terminal) is held undelivered until resume, which delivers it
 * exactly once then re-arms — unless the deliver callback paused/closed the stream. No TLS
 * state here (the adapter's deliver hook decrypts).
 *
 * Async discipline (mirrors the write side): an arm hook may complete SYNCHRONOUSLY (EFI/lwIP/
 * future providers may call on_recv from inside read_arm). recv_inflight is set BEFORE arming
 * and the outer frame never clobbers state a synchronous completion/pause/close already changed.
 * A receive is delivered only when recv_inflight was set (duplicate/spurious completions are
 * dropped, never delivered). Logical close stops delivery but does NOT claim retirement of a
 * physically-outstanding completion receive — that clears only on the completion (or a future
 * cancel-retirement path).
 */
#include "stream_read.h"

int kl_stream_read_init(KlStream *s, int completion_mode,
                        KlStreamReadDeliverFn deliver, KlStreamReadArmFn arm,
                        KlStreamReadDisarmFn disarm, void *ctx) {
    if (!s || s->read_inited || !deliver || !arm) return -1;
    if (!s->read_buf || s->read_cap == 0) return -1;      /* need a stable receive buffer */
    if (!completion_mode && !disarm) return -1;           /* readiness pause MUST drop interest */
    s->read_deliver          = deliver;
    s->read_arm              = arm;
    s->read_disarm           = disarm;      /* optional (no-op) in completion mode */
    s->read_ctx              = ctx;
    s->read_completion_mode  = completion_mode ? 1 : 0;
    s->read_inited           = 1;
    s->read_paused           = 0;
    s->recv_inflight         = 0;
    s->recv_held             = 0;
    s->held_len              = 0;
    s->held_ok               = 0;
    s->read_closed           = 0;
    s->arming                = 0;
    s->rearm_pending         = 0;
    s->completed_inline      = 0;
    return 0;
}

/* Arm the next receive. ITERATIVE trampoline so a provider that completes synchronously inside
 * read_arm() (EFI/lwIP/future) cannot grow the C stack: a re-arm requested during a synchronous
 * delivery sets rearm_pending and returns immediately (no recursion), and this loop performs it.
 * Synchronous completion RETIRES that particular arm (on_recv sets completed_inline) — read_arm's
 * subsequent return can no longer fail it; only a hook that did NOT complete inline and returns
 * < 0 fails+closes. recv_inflight is set BEFORE the hook so a synchronous on_recv sees it.
 * Precondition: the caller decided arming is due. Returns 0, or -1 if a (non-inline) arm failed. */
static int stream_arm(KlStream *s) {
    if (s->arming) { s->rearm_pending = 1; return 0; }   /* defer nested re-arm to the trampoline */

    int result = 0;
    for (;;) {
        if (s->read_closed || s->read_paused || s->recv_inflight || s->recv_held) break;
        s->recv_inflight    = 1;
        s->recv_cancel_requested = 0;   /* a genuinely new recv op — not yet cancel-requested */
        s->completed_inline = 0;
        s->rearm_pending    = 0;
        s->arming           = 1;
        int rc = s->read_arm(s->read_ctx);   /* may sync-complete (on_recv), pause, close, defer a re-arm */
        s->arming = 0;
        if (s->completed_inline) {
            /* This arm retired synchronously; its return cannot retroactively fail it. If the
             * delivery requested another receive (and the stream is still open/active), loop —
             * iteratively, not recursively. Pause/close during the delivery leaves rearm_pending
             * clear, suppressing the re-arm. */
            if (s->rearm_pending) continue;
            break;
        }
        if (rc < 0) {                        /* genuine arm failure — no sync completion happened */
            if (s->recv_inflight) s->recv_inflight = 0;
            s->read_closed = 1;
            result = -1;
            break;
        }
        break;                               /* posted async — await the completion */
    }
    return result;
}

/* Deliver one completed receive, then re-arm the next — UNLESS the completion was terminal
 * (ok == 0), or the deliver callback synchronously paused/closed the stream. Terminal state is
 * set BEFORE the callback (so a terminal callback that calls start()/resume() cannot re-arm a
 * closed stream), and a terminal carries no data (len normalized to 0). State is re-checked
 * through the stream's stable fields AFTER the callback. Returns 0, or -1 if a re-arm failed. */
static int stream_deliver_and_continue(KlStream *s, size_t len, int ok) {
    if (!ok) { s->read_closed = 1; len = 0; }         /* terminal: closed before callback, no data */
    s->read_deliver(s->read_ctx, s->read_buf, len, ok);
    if (!ok) return 0;                                /* terminal delivered — never re-arm */
    if (s->read_closed || s->read_paused) return 0;   /* callback closed/paused — do not re-arm */
    if (s->recv_inflight) return 0;                   /* a synchronous completion already re-armed */
    return stream_arm(s);
}

int kl_stream_read_start(KlStream *s) {
    if (!s || !s->read_inited) return -1;
    if (s->read_paused || s->read_closed || s->recv_inflight || s->recv_held) return 0;
    return stream_arm(s);
}

int kl_stream_on_recv(KlStream *s, size_t len, int ok) {
    if (!s || !s->read_inited) return -1;
    if (!s->recv_inflight) return 0;                  /* duplicate/spurious — drop, never deliver */
    s->recv_inflight = 0;                             /* this receive op has retired */
    if (s->arming) s->completed_inline = 1;           /* synchronous completion of the current arm */
    if (s->read_closed) {                             /* retirement of a closed op — drop data */
        if (s->on_retire) s->on_retire(s);            /* recv physically retired — let close finalize */
        return 0;
    }

    if (ok && len > s->read_cap) {                    /* backend contract violation — fail closed */
        s->read_closed = 1;
        if (s->on_retire) s->on_retire(s);            /* recv retired (into error) — notify */
        return -1;                                    /* not delivered as data; caller tears down */
    }
    if (!ok) len = 0;                                 /* terminal carries no data */

    if (s->read_paused) {                             /* STRICT: hold, do not deliver */
        s->recv_held = 1;                             /* completed_inline (set above) still marks the */
        s->held_len  = len;                           /* arm retired; rearm_pending stays clear (no re-arm) */
        s->held_ok   = ok ? 1 : 0;
        if (s->on_retire) s->on_retire(s);            /* recv op physically retired (held); notify */
        return 0;
    }
    int r = stream_deliver_and_continue(s, len, ok ? 1 : 0);
    /* If the deliver callback re-armed, recv_inflight is set again and finalize sees the read side
     * busy; if it paused/closed instead, recv_inflight stays 0 and this notification detaches. */
    if (!s->recv_inflight && s->on_retire) s->on_retire(s);
    return r;
}

void kl_stream_pause(KlStream *s) {
    if (!s || !s->read_inited || s->read_paused) return;   /* idempotent */
    s->read_paused = 1;
    if (!s->read_completion_mode && s->recv_inflight) {
        /* Readiness: drop READ interest ONLY if it is actually armed (pause-before-start must not
         * disarm nonexistent interest); a pending readable is cancelled, so nothing completes. */
        s->read_disarm(s->read_ctx);
        s->recv_inflight = 0;
    }
    /* Completion: leave a posted recv physically in flight — it completes into read_buf and is
     * held; recv_inflight stays set until that completion (retirement). */
}

int kl_stream_resume(KlStream *s) {
    if (!s || !s->read_inited || !s->read_paused) return 0;   /* idempotent */
    s->read_paused = 0;
    if (s->read_closed) return 0;

    if (s->recv_held) {
        size_t len = s->held_len;
        int ok = s->held_ok;
        s->recv_held = 0;                             /* consume the held completion exactly once */
        s->held_len  = 0;
        s->held_ok   = 0;
        return stream_deliver_and_continue(s, len, ok);  /* deliver once, then re-arm per re-check */
    }
    /* No held data — re-arm to fetch more, unless a receive is already physically in flight
     * (completion mode: a recv posted before pause may still be outstanding). */
    if (s->recv_inflight) return 0;
    return stream_arm(s);
}

void kl_stream_read_close(KlStream *s) {
    if (!s || !s->read_inited || s->read_closed) return;   /* idempotent — no repeat disarm */
    s->read_closed = 1;                              /* LOGICAL close: no future delivery/re-arm */
    s->recv_held   = 0;                              /* discard held completion — not delivered */
    s->held_len    = 0;
    s->held_ok     = 0;
    if (!s->read_completion_mode) {
        /* Readiness: drop interest (only if armed); the pending readable is cancelled, so nothing
         * is physically outstanding after this. */
        if (s->recv_inflight && s->read_disarm) s->read_disarm(s->read_ctx);
        s->recv_inflight = 0;
    }
    /* Completion: a posted recv may still be PHYSICALLY outstanding (kernel/provider owns the
     * buffer + target). Do NOT clear recv_inflight here — retirement happens when its completion
     * reaches on_recv (dropped), or via the Step-3 cancel-retirement path. This logical/physical
     * split is what Step-3 detachment builds on. */
}

int kl_stream_read_held(const KlStream *s) {
    return (s && s->recv_held) ? 1 : 0;
}
