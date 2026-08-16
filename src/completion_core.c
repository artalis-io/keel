/*
 * completion_core.c — the generic, model-blind completion tick (freestanding B2a).
 *
 * The platform-independent core of the completion driver: drain the ctx's completion
 * loop and route each finished op. The GENERIC kinds are handled here directly —
 * KL_COMP_WATCHER / KL_COMP_CONNECT relay to kl_event_dispatch (client + generic
 * watcher path), and due timers fire via kl_timer_fire. The NON-generic kinds
 * (ACCEPT/READ/WRITE → the server; UDP_RECV/UDP_SEND → the udp stack) are routed
 * through two opaque KlEventCtx hooks (comp_conn_dispatch / comp_udp_dispatch), set by
 * the server / kl_udp_init when those features are used and NULL otherwise.
 *
 * The point of the split (from the old completion_driver.c): this TU references NEITHER
 * comp_on_accept/read/write NOR kl_udp_comp_on_recv/send, so a client-only completion
 * build — which uses only CONNECT/WATCHER + timers — links neither the server nor the
 * UDP stack. The hook implementations live in completion_server.c / udp.c.
 * See docs/keel_axis_audit.md ("no hidden global event-loop state" — the hooks live on
 * the per-loop KlEventCtx, not a file-scope global).
 */
#include <keel/event_ctx.h>      /* KlEventCtx + kl_event_dispatch */
#include <keel/timer.h>          /* kl_timer_fire — due timers on the completion tick */
#include "completion.h"          /* the abstract completion axis (KlCompletionEvent) */
#include "datagram_life.h"       /* KlDgramLife dispatch — type-safe datagram completion routing (7B-2a) */
#include "io_engine.h"           /* kl_comp_run (the seam this TU defines) */

#define KL_COMP_MAX_EVENTS 64

/* Generic completion tick — platform-independent. Drains the ctx's completion loop
 * and routes each finished op to its consumer. Shared by the server run loop and the
 * standalone kl_event_ctx_run. */
int kl_comp_run(struct KlEventCtx *ctx, int max, int timeout_ms) {
    if (max > KL_COMP_MAX_EVENTS) max = KL_COMP_MAX_EVENTS;
    KlCompletionEvent ev[KL_COMP_MAX_EVENTS];
    int n = kl_comp_drain(ctx, ev, max, timeout_ms);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        switch (ev[i].kind) {
        /* TCP conn completions → the server (via the ctx hook, set when a server runs
         * on this loop). NULL on a client-only build, where these never occur. */
        case KL_COMP_ACCEPT:
        case KL_COMP_READ:
        case KL_COMP_WRITE:
            if (ctx->comp_conn_dispatch)
                ctx->comp_conn_dispatch(ctx, &ev[i]);
            break;
        /* Datagram completions → the owner named by the token (7B-2a): type-safe routing via the
         * token's own dispatch handler (KlUdp or, from 7B-3, KlDatagram), NOT a ctx-global hook or an
         * untyped downcast of kl_dgram_life_target(). A dead token yields a NULL target the handler
         * drops; a token with no handler (or ev->life NULL) still has its transferred ref released. */
        case KL_COMP_DGRAM_RECV:
        case KL_COMP_DGRAM_SEND: {
            KlDgramLife *life = ev[i].life;
            KlDgramDispatchFn d = life ? kl_dgram_life_dispatch(life) : (KlDgramDispatchFn)0;
            if (d)
                d(kl_dgram_life_target(life), &ev[i]);   /* handler releases life after dispatch (iff !retain_life) */
            else if (life && !ev[i].retain_life)
                kl_dgram_life_release(life);              /* no handler → drop the transferred ref. 7B-9: a
                                                          * borrowed quarantine ref (retain_life=1) is NOT
                                                          * released here — same rule as the owner handlers. */
            break;
        }
        case KL_COMP_CONNECT:
            /* An outbound connect finished (LC-0). The consumer is the async KlClient, not
             * the server driver — so route it exactly like KL_COMP_WATCHER: the backend has
             * done the native connect and left the socket so getsockopt(SO_ERROR) reports
             * the truth, then relayed the result against the client's tagged connect watcher.
             * Dispatch it to that watcher (he_on_writable), which does the SO_ERROR win/fail
             * check unchanged. target = the tagged KlWatcher; bytes = KL_EVENT_WRITE. */
        case KL_COMP_WATCHER: {
            /* A readiness FD watch fired (thread-pool wakeup, timer, generic watcher).
             * Route it through the SAME watcher-dispatch the readiness loop uses — the
             * backend relayed it as a completion, but the driver stays platform-agnostic
             * (8e-2). target = the tagged KlWatcher udata; bytes = the ready mask. */
            KlEvent we = { ev[i].target, (KlEventMask)ev[i].bytes };
            kl_event_dispatch(ctx, &we);
            break;
        }
        }
    }

    /* Fire expired timers — the completion tick's counterpart to the readiness
     * kl_event_ctx_run's kl_timer_fire(). Without this, timer-driven async work stalls
     * over a completion loop: the client's Happy-Eyeballs attempt-delay and request
     * deadline, the DNS resolver's per-query timeout / retransmit and deferred
     * (shortcut) completion, redirect chains, etc. The caller clamps the drain timeout
     * to the next timer deadline, so the wait already wakes for due timers — this fires
     * them. (Server loops fire timers in their own run loop; this covers standalone
     * consumers driven via kl_event_ctx_run → kl_comp_run.) */
    kl_timer_fire(ctx);
    return n;
}
