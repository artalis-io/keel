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
#include <keel/parser.h>       /* kl_parser_llhttp (default request parser) */
#include <keel/event_ctx.h>    /* kl_event_ctx_init_ex / kl_watcher_add */
#ifndef KEEL_FREESTANDING
#include <keel/proxy_protocol.h> /* KlCidr / kl_cidr_parse_list (PROXY allowlist) */
#include <keel/file_io.h>        /* kl_file_io_create (async file I/O backend) */
#endif
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

/* Same tick bound server.c's readiness loop uses; a local copy keeps this TU
 * independent of server.c (both are plain compile-time constants). */
#define KL_POLL_TIMEOUT_MS 1000

/* ── Stop-wakeup self-pipe (hosted only) ──────────────────────────────────────
 * kl_server_stop() writes a byte to wake the run loop promptly. A freestanding EFI
 * server has no self-pipe (no OS pipe/socketpair, no kl_server_stop caller) — it
 * runs the completion loop directly — so both the helpers and their init call are
 * compiled out under KEEL_FREESTANDING. */
#ifndef KEEL_FREESTANDING
static void kl_server_on_wakeup(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)ready; (void)user_data;
    kl_plat_wakeup_drain(fd);
}

/* Open the stop-wakeup self-pipe and register its read end as a run-loop watcher. Best-
 * effort: on failure the loop simply falls back to tick-timeout stop latency. Called at the
 * end of kl_server_init, after the event ctx + provider are wired (so the watcher registers
 * correctly on both readiness and completion loops). */
static void kl_server_wakeup_init(KlServer *s) {
    s->stop_wake_rd = s->stop_wake_wr = KL_INVALID_SOCKET;
    KlPlatWakeup w;
    if (kl_plat_wakeup_open(&w) < 0) return;
    if (kl_watcher_add(&s->ev, w.rd, KL_EVENT_READ, kl_server_on_wakeup, s) < 0) {
        kl_plat_wakeup_close(&w);
        return;
    }
    s->stop_wake_rd = w.rd;
    s->stop_wake_wr = w.wr;
}
#endif /* !KEEL_FREESTANDING */

/* ── Server construction (model-blind: pool + router + event ctx + provider) ───
 * Moved from server.c in the Phase 10 UEFI server carve so the freestanding EFI
 * server can construct a KlServer from the archive alone. The hosted-only pieces —
 * the ws/h2/proxy upgrade-hook installers, the PROXY-protocol CIDR allowlist, the
 * async file-I/O backend, and the stop self-pipe — are compiled out under
 * KEEL_FREESTANDING (a freestanding server is pure HTTP/1.1, no PROXY, no async
 * file I/O, no self-pipe). Everything else is platform-neutral. */
int kl_server_init(KlServer *s, const KlConfig *config) {
    if (!s || !config) {
        if (s) s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->listen_fd = KL_INVALID_SOCKET;
    s->stop_wake_rd = s->stop_wake_wr = KL_INVALID_SOCKET;

#ifndef KEEL_FREESTANDING
    /* Register the WebSocket / HTTP-2 upgrade tables so connection.c's dispatch is
     * wired (idempotent; also pulls server_ws.o / server_h2.o out of the static
     * archive). A freestanding HTTP/1.1 server never links these — the seam stays
     * NULL and the core runs pure HTTP/1.1. */
    kl_ws_server_hooks_install();
    kl_h2_server_hooks_install();
    kl_proxy_hooks_install();   /* PROXY parser + CIDR match (proxy_protocol.c) */
#endif

    /* Apply defaults */
    s->config = *config;
    if (s->config.transport != KL_TRANSPORT_TCP &&
        s->config.transport != KL_TRANSPORT_UNIX) {
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (s->config.unix_socket_path &&
        s->config.transport == KL_TRANSPORT_TCP) {
        s->config.transport = KL_TRANSPORT_UNIX;
    }
    /* listen_fd: 0 = disabled, > 0 = adopt (fds 0-2 are stdio, not listeners).
     * Reject a negative value rather than silently ignoring it. */
    if (s->config.listen_fd < 0) {
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (s->config.bind_addr == NULL)
        s->config.bind_addr = "0.0.0.0";
    if (s->config.max_connections <= 0)
        s->config.max_connections = KL_DEFAULT_MAX_CONNS;
    if (s->config.read_timeout_ms <= 0)
        s->config.read_timeout_ms = KL_DEFAULT_READ_TIMEOUT;
    if (s->config.parser == NULL)
        s->config.parser = kl_parser_llhttp;
    if (s->config.max_body_size == 0)
        s->config.max_body_size = KL_DEFAULT_MAX_BODY_SIZE;
    if (s->config.max_header_size == 0)
        s->config.max_header_size = KL_READ_BUF_SIZE;
    if (s->config.max_header_size > SIZE_MAX / 2) {
        s->last_error = KL_ERR_OVERFLOW;
        return -1;
    }

    /* Set up allocator. A freestanding server (like the freestanding client) has no
     * stdlib default allocator in the archive — the caller MUST supply one (the EFI
     * pool allocator on UEFI); falling back to kl_allocator_default() would drag
     * malloc/free/stdio into the freestanding closure. */
    if (s->config.alloc) {
        s->alloc_storage = *s->config.alloc;
    } else {
#ifndef KEEL_FREESTANDING
        s->alloc_storage = kl_allocator_default();
#else
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
#endif
    }
    KlAllocator *alloc = &s->alloc_storage;

    /* Init subsystems */
    if (kl_router_init(&s->router, alloc) < 0) {
        s->last_error = KL_ERR_ALLOC;
        return -1;
    }
    if (kl_conn_pool_init(&s->pool, s->config.max_connections, alloc) < 0) {
        s->last_error = KL_ERR_ALLOC;
        kl_router_free(&s->router);
        return -1;
    }

    /* Copy TLS config if provided */
    if (s->config.tls) {
        s->tls_storage = *s->config.tls;
        s->config.tls = &s->tls_storage;
    }

    /* Copy HTTP/2 config if provided */
    if (s->config.h2) {
        s->h2_storage = *s->config.h2;
        s->config.h2 = &s->h2_storage;
    }

    /* Copy compress config if provided */
    if (s->config.compress) {
        s->compress_storage = *s->config.compress;
        s->config.compress = &s->compress_storage;
    }

    /* Parse the PROXY protocol trusted-source CIDR allowlist (if any). Freestanding
     * has no PROXY support — proxy_protocol.c is not in the archive. */
    s->proxy_cidrs = NULL;
    s->proxy_cidr_count = 0;
#ifndef KEEL_FREESTANDING
    if (s->config.proxy_trusted_cidrs) {
        KlCidr tmp[64];
        int cn = kl_cidr_parse_list(s->config.proxy_trusted_cidrs, tmp,
                                    (int)(sizeof(tmp) / sizeof(tmp[0])));
        if (cn < 0) {
            s->last_error = KL_ERR_INVALID_ARG;
            kl_conn_pool_free(&s->pool);
            kl_router_free(&s->router);
            return -1;
        }
        if (cn > 0) {
            s->proxy_cidrs = kl_malloc(alloc, (size_t)cn * sizeof(KlCidr));
            if (!s->proxy_cidrs) {
                s->last_error = KL_ERR_ALLOC;
                kl_conn_pool_free(&s->pool);
                kl_router_free(&s->router);
                return -1;
            }
            memcpy(s->proxy_cidrs, tmp, (size_t)cn * sizeof(KlCidr));
            s->proxy_cidr_count = cn;
        }
    }
#endif

    /* Create parsers and propagate config for each connection slot */
    for (int i = 0; i < s->pool.capacity; i++) {
        s->pool.conns[i].parser = s->config.parser(alloc);
        if (!s->pool.conns[i].parser) {
            s->last_error = KL_ERR_ALLOC;
            kl_conn_pool_free(&s->pool);
            kl_router_free(&s->router);
            return -1;
        }
        s->pool.conns[i].access_log = s->config.access_log;
        s->pool.conns[i].access_log_data = s->config.access_log_data;
        s->pool.conns[i].h2_config = s->config.h2;  /* NULL if disabled */
        s->pool.conns[i].router = &s->router;
        s->pool.conns[i].stream.ctx = &s->ev;   /* for the socket provider (ctx->sockets) */
        s->pool.conns[i].max_body_size = s->config.max_body_size;
        s->pool.conns[i].max_header_size = s->config.max_header_size;
    }

    /* Pre-allocate TLS sessions (one per connection slot) */
    if (s->config.tls) {
        /* A NULL factory would crash on the first call below — reject it up front (no
         * sessions allocated yet, so pool_free touches no tls). */
        if (!s->config.tls->factory) {
            s->last_error = KL_ERR_TLS_VTABLE;
            kl_conn_pool_free(&s->pool);
            kl_router_free(&s->router);
            return -1;
        }
        for (int i = 0; i < s->pool.capacity; i++) {
            s->pool.conns[i].tls = s->config.tls->factory(
                s->config.tls->ctx, alloc);
            if (!s->pool.conns[i].tls) {
                s->last_error = KL_ERR_TLS_INIT;
                kl_conn_pool_free(&s->pool);
                kl_router_free(&s->router);
                return -1;
            }
            /* Validate the vtable (all 7 required ops; alpn_protocol etc. optional). A
             * session missing a required op — notably `destroy` — must NOT reach
             * kl_conn_pool_free, which would call the NULL op. Free it here via its own
             * destroy only when present, clear the slot, THEN run pool cleanup. */
            if (!kl_tls_vtable_valid(s->pool.conns[i].tls)) {
                s->last_error = KL_ERR_TLS_VTABLE;
                if (s->pool.conns[i].tls->destroy)
                    s->pool.conns[i].tls->destroy(s->pool.conns[i].tls);
                s->pool.conns[i].tls = NULL;
                kl_conn_pool_free(&s->pool);
                kl_router_free(&s->router);
                return -1;
            }
        }
    }

    /* Init event context — must happen before thread pool / watcher registration.
     * A configured event_provider (e.g. lwIP / EFI) installs its own backend. */
    if (kl_event_ctx_init_ex(&s->ev, alloc, s->config.event_provider) < 0) {
        s->last_error = KL_ERR_EVENT_INIT;
        kl_conn_pool_free(&s->pool);
        kl_router_free(&s->router);
        return -1;
    }
    /* Route all socket ops (listen socket + accepted conns) through the selected
     * provider; NULL = built-in default. */
    s->ev.sockets = s->config.sockets;

    /* A completion loop needs an overlapped provider, which the default (or a
     * readiness) provider is not. If the configured provider is incompatible with the
     * loop, adopt the backend's own native provider (kl_event_native_provider — NULL on
     * readiness backends, the overlapped provider on completion backends). This makes a
     * completion backend a source-compatible drop-in: a server written for epoll works
     * unchanged. An explicitly-configured compatible provider is kept; a still-
     * incompatible pairing is rejected below. */
    if (!kl_event_ctx_sockets_compatible(&s->ev)) {
        const struct KlSocketProvider *np = kl_event_native_provider(&s->ev.loop);
        if (np) s->ev.sockets = np;
    }

    /* Negotiate the event loop against the socket provider now that both are wired
     * onto the ctx. Reject an incoherent pairing here rather than failing obscurely at
     * add-to-loop. */
    if (!kl_event_ctx_sockets_compatible(&s->ev)) {
        s->last_error = KL_ERR_SOCKET;
        kl_event_ctx_free(&s->ev);
        kl_conn_pool_free(&s->pool);
        kl_router_free(&s->router);
        return -1;
    }

    /* Preallocate the completion-mode TLS ciphertext scratch (one stable buffer per slot) at
     * init — NEVER in the event-loop hot path (Step-2 review). Only for TLS + a completion event
     * model: the HTTP completion adapter reads ciphertext into it, and readiness TLS decrypts
     * straight from the socket so needs none. The event model is only known now (post event-ctx
     * init). Unwinds like the negotiation failure above; pool_free reclaims any already set. */
    if (s->config.tls && (kl_event_caps(&s->ev.loop) & KL_EVENT_CAP_COMPLETION)) {
        for (int i = 0; i < s->pool.capacity; i++) {
            s->pool.conns[i].comp_cipher = kl_malloc(alloc, KL_COMP_CIPHER_SIZE);
            if (!s->pool.conns[i].comp_cipher) {
                s->last_error = KL_ERR_ALLOC;
                kl_event_ctx_free(&s->ev);
                kl_conn_pool_free(&s->pool);
                kl_router_free(&s->router);
                return -1;
            }
            s->pool.conns[i].comp_cipher_cap = KL_COMP_CIPHER_SIZE;
        }
    }

    /* Create async file I/O backend (NULL if backend doesn't support it). Freestanding
     * has no async file I/O — file_io.c is not in the archive; s->file_io stays NULL. */
#ifndef KEEL_FREESTANDING
    s->file_io = kl_file_io_create(&s->ev.loop, alloc);
#endif
    for (int i = 0; i < s->pool.capacity; i++)
        s->pool.conns[i].file_io = s->file_io;

    /* Self-pipe wakeup so kl_server_stop() wakes the run loop promptly (both axes). */
#ifndef KEEL_FREESTANDING
    kl_server_wakeup_init(s);
#endif

    return 0;
}

/* ── Server teardown (model-blind: close sockets, free pool/router, destroy ctx) ──
 * Moved from server.c in the S-7 teardown carve so a freestanding EFI server can tear
 * itself down from the archive alone (then kl_uefi_shutdown() releases the EFI providers
 * before ExitBootServices). kl_conn_pool_free closes every accepted child's socket
 * (draining its EFI tokens), so after this the socket provider's live count is 0 — the
 * precondition for a clean ExitBootServices. Hosted-only pieces (async-op cancel, the
 * self-pipe, the AF_UNIX unlink) are compiled out under KEEL_FREESTANDING. */
void kl_server_free(KlServer *s) {
#ifndef KEEL_FREESTANDING
    /* Cancel all active async ops (idempotent terminal: fires on_cancel once, removes
     * each op from the list). Freestanding has no async suspend (no thread pool). */
    while (s->async_ops)
        kl_async_cancel(s, s->async_ops);
#endif

    if (s->file_io) {                 /* NULL on freestanding (kl_file_io_create guarded) */
        s->file_io->destroy(s->file_io);
        s->file_io = NULL;
    }

    /* Close the listen socket. Hosted also unlinks an owned AF_UNIX path; a freestanding
     * EFI server has only the TCP listener, so close it directly. */
#ifndef KEEL_FREESTANDING
    kl_server_close_listener(s);
#else
    if (kl_handle_valid(s->listen_fd)) {
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
    }
#endif

    /* Tear down the stop-wakeup self-pipe (never opened on freestanding — stop_wake_rd
     * stays KL_INVALID_SOCKET, so this is skipped). */
    if (kl_handle_valid(s->stop_wake_rd)) {
        kl_watcher_del(&s->ev, s->stop_wake_rd);
        KlPlatWakeup w = { s->stop_wake_rd, s->stop_wake_wr };
        kl_plat_wakeup_close(&w);
        s->stop_wake_rd = s->stop_wake_wr = KL_INVALID_SOCKET;
    }
    kl_event_ctx_free(&s->ev);
    if (s->proxy_cidrs) {             /* NULL on freestanding (PROXY block guarded in init) */
        kl_free(&s->alloc_storage, s->proxy_cidrs,
                (size_t)s->proxy_cidr_count * sizeof(KlCidr));
        s->proxy_cidrs = NULL;
        s->proxy_cidr_count = 0;
    }
    kl_conn_pool_free(&s->pool);      /* closes every live accepted-child socket */
    kl_router_free(&s->router);
    if (s->config.tls && s->config.tls->ctx_destroy) {
        s->config.tls->ctx_destroy(s->config.tls->ctx);
    }
    if (s->config.compress && s->config.compress->ctx_destroy) {
        s->config.compress->ctx_destroy(s->config.compress->ctx);
    }
}

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

/* Release a connection and resume the listen socket if it was paused due to pool
 * exhaustion. Called from the event loop, timeout sweep, and async completion. */
void kl_server_conn_release(KlServer *s, KlConn *c) {
    kl_conn_release(&s->pool, c);
    if (s->listen_paused && s->pool.free_list) {
        kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL);
        s->listen_paused = 0;
    }
}

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
                        kl_comp_cancel(&s->ev, tc->stream.fd);
                    } else {
                        kl_event_del(&s->ev.loop, tc->stream.fd);
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
                tc->file_io->cancel(tc->file_io, tc->stream.fd);
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
                kl_comp_cancel(&s->ev, tc->stream.fd);   /* abort op → release via its completion */
            } else {
                kl_event_del(&s->ev.loop, tc->stream.fd);
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
    if (!c || c->stream.read_paused) return;                 /* idempotent */
    c->stream.read_paused = 1;
    if (!(kl_event_caps(&c->stream.ctx->loop) & KL_EVENT_CAP_COMPLETION))
        (void)kl_event_mod(&c->stream.ctx->loop, c->stream.fd, 0, c);   /* readiness: stop READ now */
    /* completion: comp_start_body_read skips the next recv; the in-flight recv may
     * deliver <=1 more chunk before the pause takes hold (bounded). */
}

void kl_request_resume_body(const KlRequest *req) {
    KlConn *c = req ? kl_request_conn(req) : NULL;
    if (!c || !c->stream.read_paused) return;                /* idempotent */
    c->stream.read_paused = 0;
    if (kl_event_caps(&c->stream.ctx->loop) & KL_EVENT_CAP_COMPLETION)
        kl_io_engine_post_read(c);                    /* completion: re-post the body recv */
    else
        (void)kl_event_mod(&c->stream.ctx->loop, c->stream.fd, KL_EVENT_READ, c);   /* readiness: re-arm READ */
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
