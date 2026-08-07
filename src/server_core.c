/*
 * server_core.c — the model-blind, freestanding-safe half of the KlServer.
 *
 * Split out of server.c for the Phase 10 UEFI *server* (docs/phase10_uefi_server_design.md,
 * S-1), mirroring the client's async.c -> event_ctx.c bisection. This TU holds the
 * server API + run machinery that touches ONLY the completion / connection-pool /
 * router / timer seams — no OS sockets, no signals, no systemd, no self-pipe, no
 * readiness event loop, no stdio. A freestanding completion server (EFI_TCP4 +
 * the EFI completion backend) links it WITHOUT dragging in server.c's hosted half.
 *
 * The hosted half stays byte-identical in server.c: kl_log/kl_log_errno (stdio),
 * TCP/AF_UNIX bind + listen, systemd socket activation, signal handling, the
 * stop-wakeup self-pipe, and the readiness kl_server_run wait/accept/dispatch loop
 * (which delegates its completion branch to kl_server_run_completion_loop here).
 */

#include <keel/server.h>
#include <keel/async.h>
#include <keel/timer.h>
#include <keel/request.h>
#include "internal.h"
#include "io_engine.h"    /* kl_io_engine_run_completion / kl_io_engine_post_read / kl_comp_cancel */
#include "event_caps.h"   /* kl_event_caps — completion vs readiness pause/resume */
#include "platform.h"     /* kl_monotonic_ms */
#include "proto_hooks.h"  /* ws/h2 upgrade seam — sweep/drain reach ws/h2 through it */
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

/* Same tick bound server.c's readiness loop uses; a local copy keeps this TU
 * independent of server.c (both are plain compile-time constants). */
#define KL_POLL_TIMEOUT_MS 1000

/* ── Completion run-loop tick (the freestanding server's whole loop body) ──────
 * One iteration of the completion event loop, factored out of kl_server_run so
 * BOTH the hosted completion backends (io_uring/IOCP/pollcomp) and a freestanding
 * EFI server share it verbatim. Uses only completion/timer/async seams. Returns 0
 * to continue the loop, -1 to break (the completion tick reported a fatal error).
 * The caller owns the `while (running)` guard + drain/running atomics. */
int kl_server_run_completion_loop(KlServer *s) {
    /* Bound the tick by the nearest async-op deadline / timer so they fire on time
     * — the completion loop is a full event loop (watchers relayed via
     * KL_COMP_WATCHER; timers + async deadlines serviced here, 8e-2). */
    uint64_t cnow = kl_monotonic_ms();
    int cwait = KL_POLL_TIMEOUT_MS;
    for (const KlAsyncOp *aop = s->async_ops; aop; aop = aop->next) {
        if (aop->deadline_ms > 0) {
            if (cnow >= aop->deadline_ms) { cwait = 0; break; }
            uint64_t rem = aop->deadline_ms - cnow;
            if (rem < (uint64_t)cwait) cwait = (int)rem;
        }
    }
    cwait = kl_timer_next_timeout(&s->ev, cwait);
    if (kl_io_engine_run_completion(s, cwait) < 0)
        return -1;
    cnow = kl_monotonic_ms();
    /* Idle-timeout sweep on the completion loop too (slowloris defense) — the
     * completion path never falls through to the readiness sweep in server.c. */
    kl_server_sweep_conn_timeouts(s, cnow, 1);
    for (KlAsyncOp *aop = s->async_ops; aop; ) {   /* async-op deadlines */
        KlAsyncOp *next_aop = aop->next;
        if (!aop->_terminal && aop->deadline_ms > 0 && cnow >= aop->deadline_ms) {
            aop->deadline_ms = 0;   /* fire on_deadline at most once */
            if (aop->on_deadline)
                aop->on_deadline(aop, aop->user_data);
        }
        aop = next_aop;
    }
    kl_timer_fire(&s->ev);                          /* due one-shot timers */
    kl_server_drain_progress(s, cnow);              /* graceful drain (completion loop) */
    return 0;
}

/* ── Idle-timeout sweep + graceful-drain (shared by both run-loop branches) ────
 * Moved here from server.c so the freestanding completion server has them
 * in-archive. WebSocket/HTTP-2 are reached only through the upgrade seam, so these
 * name no ws/h2 symbol; a freestanding HTTP/1.1 build leaves the hooks NULL. */

static const char kl_408_response[] =
    "HTTP/1.1 408 Request Timeout\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

void kl_server_sweep_conn_timeouts(KlServer *s, uint64_t now, int completion_loop) {
    uint64_t timeout = (uint64_t)s->config.read_timeout_ms;
    uint64_t body_timeout = s->config.body_timeout_ms > 0
                            ? (uint64_t)s->config.body_timeout_ms
                            : timeout;
    for (int i = 0; i < s->pool.capacity; i++) {
        KlConn *tc = &s->pool.conns[i];
        if (tc->state == KL_CONN_CLOSED || tc->state == KL_CONN_PROCESSING)
            continue;
        /* Suspended: exempt from idle timeout — has its own deadline */
        if (tc->state == KL_CONN_SUSPENDED)
            continue;
        /* WebSocket: exempt from HTTP idle timeout, check close deadline (seam). */
        if (tc->state == KL_CONN_WEBSOCKET) {
            const KlWsServerHooks *wsh = kl_ws_server_hooks();
            if (wsh) {
                if (wsh->auto_ping) wsh->auto_ping(tc, now);
                if (wsh->check_close_timeout && wsh->check_close_timeout(tc, now)) {
                    if (completion_loop) {
                        kl_comp_cancel(&s->ev, tc->fd);
                    } else {
                        kl_event_del(&s->ev.loop, tc->fd);
                        kl_server_conn_release(s, tc);
                    }
                }
            }
            continue;
        }
        /* HTTP/2: PING keepalive is session's responsibility */
        if (tc->state == KL_CONN_HTTP2)
            continue;
        /* TLS handshake time counts against read timeout */
        int timed_out = (now - tc->last_active_ms > timeout);
        /* Body deadline: absolute time from body start, not resettable. */
        if (!timed_out && tc->state == KL_CONN_READING_BODY &&
            tc->body_start_ms > 0 &&
            now - tc->body_start_ms > body_timeout) {
            timed_out = 1;
        }
        if (timed_out) {
            /* Cancel pending async file read before release (readiness io_uring path) */
            if (tc->file_io_phase == 1 && tc->file_io) {
                tc->file_io->cancel(tc->file_io, tc->fd);
                tc->file_io_phase = 3;  /* FILE_IO_CANCELLING */
                continue;  /* wait for cancel CQE in next tick */
            }
            if (tc->file_io_phase == 3)
                continue;  /* FILE_IO_CANCELLING — still waiting */
            /* Best-effort 408 (skip for TLS handshake — no HTTP framing yet). */
            if (tc->state == KL_CONN_READING ||
                tc->state == KL_CONN_READING_BODY)
                best_effort_conn_write(tc, kl_408_response,
                                       sizeof(kl_408_response) - 1);
            if (completion_loop) {
                kl_comp_cancel(&s->ev, tc->fd);   /* abort op → release via its completion */
            } else {
                kl_event_del(&s->ev.loop, tc->fd);
                kl_server_conn_release(s, tc);
            }
        }
    }
}

void kl_server_drain_progress(KlServer *s, uint64_t now) {
    if (!atomic_load(&s->draining)) return;
    const KlWsServerHooks *wsh = kl_ws_server_hooks();
    const KlH2ServerHooks *h2h = kl_h2_server_hooks();
    for (int j = 0; j < s->pool.capacity; j++) {
        if (wsh && wsh->drain_close && s->pool.conns[j].state == KL_CONN_WEBSOCKET)
            wsh->drain_close(&s->pool.conns[j]);
        if (h2h && h2h->drain_shutdown && s->pool.conns[j].state == KL_CONN_HTTP2)
            h2h->drain_shutdown(&s->pool.conns[j]);
    }
    int active = 0;
    for (int j = 0; j < s->pool.capacity; j++)
        if (s->pool.conns[j].state != KL_CONN_CLOSED) active++;
    if (active == 0 || now >= s->drain_deadline_ms)
        atomic_store(&s->running, 0);
}

/* ── Route + middleware registration (thin router wrappers, seam-only) ───────── */

int kl_server_route(KlServer *s, const char *method, const char *pattern,
                    KlHandler handler, void *user_data,
                    KlBodyReaderFactory body_reader) {
    return kl_router_add(&s->router, method, pattern, handler, user_data,
                         body_reader);
}

int kl_server_route_streaming(KlServer *s, const char *method, const char *pattern,
                               KlHandler handler, void *user_data,
                               KlBodyReaderFactory body_reader) {
    return kl_router_add_streaming(&s->router, method, pattern, handler,
                                    user_data, body_reader);
}

int kl_server_route_streaming_async(KlServer *s, const char *method,
                                       const char *pattern,
                                       KlHandler handler, void *user_data,
                                       KlBodyReaderFactory body_reader) {
    return kl_router_add_streaming_async(&s->router, method, pattern, handler,
                                            user_data, body_reader);
}

int kl_server_use(KlServer *s, const char *method, const char *pattern,
                  KlMiddleware fn, void *user_data) {
    return kl_router_use(&s->router, method, pattern, fn, user_data);
}

int kl_server_use_post(KlServer *s, const char *method, const char *pattern,
                       KlMiddleware fn, void *user_data) {
    return kl_router_use_post(&s->router, method, pattern, fn, user_data);
}

/* ── Read-side body flow control (event-axis-agnostic) ────────────────────────
 * Readiness drops/re-arms READ interest (kl_event_mod); completion stops posting /
 * re-posts the recv via the io_engine seam. Both idempotent; loop-thread only. */
void kl_request_pause_body(const KlRequest *req) {
    KlConn *c = req ? kl_request_conn(req) : NULL;
    if (!c || c->read_paused) return;                 /* idempotent */
    c->read_paused = 1;
    if (!(kl_event_caps(&c->ctx->loop) & KL_EVENT_CAP_COMPLETION))
        (void)kl_event_mod(&c->ctx->loop, c->fd, 0, c);   /* readiness: stop READ now */
    /* completion: comp_start_body_read skips the next recv; the in-flight recv may
     * deliver <=1 more chunk before the pause takes hold (bounded). */
}

void kl_request_resume_body(const KlRequest *req) {
    KlConn *c = req ? kl_request_conn(req) : NULL;
    if (!c || !c->read_paused) return;                /* idempotent */
    c->read_paused = 0;
    if (kl_event_caps(&c->ctx->loop) & KL_EVENT_CAP_COMPLETION)
        kl_io_engine_post_read(c);                    /* completion: re-post the body recv */
    else
        (void)kl_event_mod(&c->ctx->loop, c->fd, KL_EVENT_READ, c);   /* readiness: re-arm READ */
}

/* ── Read-only load snapshot ──────────────────────────────────────────────── */
void kl_server_stats(const KlServer *s, KlServerStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s) return;

    out->active_connections = s->pool.active_count;
    out->max_connections    = s->pool.capacity;
    out->listen_paused      = s->listen_paused;

    /* Count suspended connections by walking the async ops list */
    int suspended = 0;
    for (const KlAsyncOp *op = s->async_ops; op; op = op->next)
        suspended++;
    out->async_suspended = suspended;
}
