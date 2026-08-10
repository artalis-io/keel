/*
 * client_async.c — HTTP/1.1 client, async (event-driven) API
 *
 * Freestanding step B2b: the non-blocking state machine driven by KlEventCtx
 * watchers lives here — Happy Eyeballs (RFC 8305) racing connect, the
 * completion-connect path (LC-0), async DNS, the SENDING/RECEIVING/TLS/proxy
 * states, and the public kl_client_start[_s] + kl_client_start_pooled entry
 * points. The async path uses kl_sock_send/kl_sock_recv through the provider +
 * watchers, so this TU links without the blocking sync path (client_sync.c).
 *
 * All allocation through KlAllocator. No Hull dependencies.
 */

#include <keel/client.h>
#include <keel/client_pool.h>
#include <keel/decompress.h>
#include <keel/dns_resolver.h>
#include <keel/parser.h>
#include <keel/timer.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>

#include "socket.h"     /* seam: kl_sock_* + KlSockAddr (no direct sockaddr) */
#include "resolve_sync.h" /* kl_resolve_sync — blocking name resolution -> KlSockAddr */
#include "event_caps.h" /* PAL Phase 7: event↔socket capability negotiation */
#include "io_engine.h"  /* kl_comp_post_connect / kl_comp_cancel — completion connect (LC-0) */
#include "watcher_internal.h" /* kl_watcher_add_detached — completion connect (LC-0) */
#include "client_internal.h"
#include "client_proxy.h" /* shared CONNECT serialization + status (no sync/async drift) */
#include "kl_cstr.h"    /* locale-free append builders + bounded find (no snprintf) */

/* Forward declarations */
static void async_on_event(KlSocketHandle fd, KlEventMask ready, void *user_data);
static void async_complete_success(KlClient *c);
static void async_complete_error(KlClient *c);
static void async_handle_tls_handshake(KlClient *c);
static void async_handle_sending_stream(KlClient *c);
static void async_handle_proxy_connecting(KlClient *c);
static void async_handle_proxy_handshake(KlClient *c);
static int  start_connect(KlClient *c, const KlSockAddr *addr);
/* Happy Eyeballs via KlConnectOp (6C) — hooks + entry-point wrappers defined below start_connect */
static void he_proceed_after_connect(KlClient *c);
static void he_win(KlClient *c, KlSocketHandle fd);
static void he_on_writable(KlClient *c, KlSocketHandle fd);
static void he_on_connect_result(KlClient *c, KlSocketHandle fd, KlEventMask ready);
static void he_cancel_timers(KlClient *c);
static void he_on_deadline(void *user_data);
static void he_on_delay(void *user_data);
static void dns_resolved(KlResolveReq *req, const KlResolveResult *result,
                          int error, void *user_data);

/* ── Build CONNECT request into heap buffer ──────────────────────── */

static int build_connect_request(KlClient *c, const char *host,
                                   uint16_t port, const char *auth)
{
    char buf[KL_PROXY_RESPONSE_MAX];
    size_t n = 0;
    /* Shared serialization (client_proxy.c) — one definition for sync + async. */
    if (kl_proxy_build_connect(buf, sizeof(buf), &n, host, port, auth) != 0)
        return -1;

    c->connect_buf = kl_malloc(c->alloc, (size_t)n);
    if (!c->connect_buf)
        return -1;
    memcpy(c->connect_buf, buf, (size_t)n);
    c->connect_len = (size_t)n;
    c->connect_sent = 0;
    return 0;
}

/* ── Post-DNS: create socket, connect, register watcher ──────────── */

/* True when the client's loop is a completion loop (IOCP / io_uring / pollcomp): connect is
 * driven over the completion axis (kl_comp_post_connect) instead of a readiness WRITE watcher.
 * Readiness loops (epoll/kqueue/poll) return 0 → the existing watcher path is used unchanged. */
static int client_loop_is_completion(const KlClient *c)
{
    return (kl_event_caps(&c->ev_ctx->loop) & KL_EVENT_CAP_COMPLETION) != 0;
}

/* Arm one nonblocking connect on a completion loop (LC-0): register a DETACHED watcher (so the
 * connect completion's tagged pointer resolves to async_on_event, without a parallel readiness
 * watch on the connecting fd), then post the connect. On completion the backend fires
 * KL_COMP_CONNECT → the driver → async_on_event (CONNECTING), which reads the win/fail result
 * the backend encoded in the delivered mask (KL_EVENT_WRITE = connected). Returns 0 if posted,
 * -1 on a hard local failure. */
static int client_comp_connect(KlClient *c, KlSocketHandle fd, const KlSockAddr *addr)
{
    void *tag = kl_watcher_add_detached(c->ev_ctx, fd, async_on_event, c);
    if (!tag)
        return -1;
    if (kl_comp_post_connect(c->ev_ctx, fd, addr, tag) != 0) {
        kl_watcher_del(c->ev_ctx, fd);
        return -1;
    }
    return 0;
}

/* Tear down one racing connect fd (loser/failure/deadline). On a completion loop a pending
 * connect op still references the fd — drop it (kl_comp_cancel frees the PC_CONNECT op) before
 * removing the (detached) watcher + closing the fd, so no stale completion dispatches against
 * the freed watcher. On a readiness loop this is just watcher_del + close. */
static void client_drop_connect_fd(KlClient *c, KlSocketHandle fd)
{
    if (client_loop_is_completion(c))
        kl_comp_cancel(c->ev_ctx, fd);
    kl_watcher_del(c->ev_ctx, fd);
    kl_sock_close(c->ev_ctx->sockets, fd);
}

static int start_connect(KlClient *c, const KlSockAddr *addr)
{
    int family = (kl_sockaddr_family(addr) == KL_AF_INET6) ? AF_INET6 :
                 (kl_sockaddr_family(addr) == KL_AF_UNIX)  ? AF_UNIX : AF_INET;
    KlSocketHandle fd = kl_sock_socket(c->ev_ctx->sockets, family, SOCK_STREAM, 0);
    if (!kl_handle_valid(fd))
        return -1;

    kl_sock_set_nosigpipe(c->ev_ctx->sockets, fd);
    if (kl_sock_set_nonblocking(c->ev_ctx->sockets, fd) < 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    /* Completion loop: drive connect over the completion axis (no readiness WRITE watcher). */
    if (client_loop_is_completion(c)) {
        c->fd = fd;
        c->state = KL_CLIENT_CONNECTING;   /* the connect completion advances us */
        if (client_comp_connect(c, fd, addr) != 0) {
            kl_sock_close(c->ev_ctx->sockets, fd);
            c->fd = KL_INVALID_SOCKET;
            return -1;
        }
        return 0;
    }

    int rc = kl_sock_connect(c->ev_ctx->sockets, fd, addr);
    if (rc < 0 && kl_sock_io_status(c->ev_ctx->sockets) != KL_IO_PENDING) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    c->fd = fd;
    c->state = (rc == 0) ? KL_CLIENT_SENDING : KL_CLIENT_CONNECTING;

    if (kl_watcher_add(c->ev_ctx, fd, KL_EVENT_WRITE, async_on_event, c) != 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        c->fd = KL_INVALID_SOCKET;
        return -1;
    }

    return 0;
}

/* ── Happy Eyeballs via KlConnectOp — racing connect over the resolved list (RFC 8305) ── 6C ──
 * KlConnectOp (connect_op.c) owns the resolve + racing cursor + attempt-delay stagger + winner
 * selection + loser cancellation + terminal-once + confirmed detachment. The client provides the
 * adapter hooks below and drives the on_* entry points from the resolver callback, the connect-
 * writable event, and the delay timer. The overall request DEADLINE stays CLIENT-owned (it must
 * outlive the connect terminal to bound the post-connect TLS/send/recv), so it is not a KlConnectOp
 * timer; only the Connection Attempt Delay is. The post-connect path is entered via he_win. */

/* Cancel the (client-owned) overall deadline timer. The Connection Attempt Delay is a KlConnectOp
 * timer disarmed via cli_co_cancel_delay at the connect terminal; dropped defensively here too. */
static void he_cancel_timers(KlClient *c)
{
    if (c->conn_delay_timer >= 0) {
        kl_timer_cancel(c->ev_ctx, c->conn_delay_timer);
        c->conn_delay_timer = -1;
    }
    if (c->deadline_timer >= 0) {
        kl_timer_cancel(c->ev_ctx, c->deadline_timer);
        c->deadline_timer = -1;
    }
}

/* Map a racing fd back to its address index (the client owns the idx->fd map). -1 if not found. */
static int he_idx_of_fd(const KlClient *c, KlSocketHandle fd)
{
    for (int i = 0; i < c->conn_addrs.naddrs; i++)
        if (c->conn_attempts[i].active && c->conn_attempts[i].fd == fd)
            return i;
    return -1;
}

/* ── KlConnectOp adapter hooks (ctx = the KlClient) ───────────────────────────────────────────── */

/* Begin name resolution: kick the (already-selected) resolver. dns_resolved drives on_resolved/
 * on_resolve_failed — possibly INLINE for a sync-completion-capable resolver, in which case the op
 * has already advanced past RESOLVING and the returned req is moot. If resolve() could NOT start
 * (returns NULL, no inline completion), flag it so cli_co_on_done suppresses the request callback
 * for the resulting terminal FAILED — the setup then frees the client and returns NULL, preserving
 * the "no callback on a start failure" contract while the op still retires cleanly (terminal +
 * detach). */
static int cli_co_start_resolve(void *ctx)
{
    KlClient *c = ctx;
    KlResolveReq *rq = c->resolver->resolve(c->resolver, c->ev_ctx,
                                            c->resolve_host, c->resolve_port,
                                            dns_resolved, c);
    if (kl_connect_op_state(&c->connect_op) != KL_CONNECT_OP_STATE_RESOLVING)
        return 0;                     /* dns_resolved ran inline — op already advanced */
    c->resolve_req = rq;
    if (!rq) { c->connect_start_failed = 1; return -1; }   /* → terminal FAILED, callback suppressed */
    return 0;
}

static void cli_co_cancel_resolve(void *ctx)
{
    KlClient *c = ctx;
    if (c->resolve_req && c->resolver && c->resolver->cancel)
        c->resolver->cancel(c->resolve_req);
    c->resolve_req = NULL;
    /* KEEL resolvers cancel by freeing the request and do NOT then invoke done_fn — so the machine
     * would never see the resolve retire. Report it synchronously now (the op is already terminal
     * when this hook runs, so this just clears resolve_inflight; reentrancy is guarded). */
    kl_connect_op_on_resolve_failed(&c->connect_op, KL_ERR_DNS);
}

/* Start one nonblocking connect to conn_addrs.addrs[idx]. 0 = in flight (or decided inline),
 * -1 = hard local failure (*out_err set; advance to the next address). */
static int cli_co_start_attempt(void *ctx, int idx, int *out_err)
{
    KlClient *c = ctx;
    const KlSockAddr *sa = &c->conn_addrs.addrs[idx];
    int fam = (kl_sockaddr_family(sa) == KL_AF_INET6) ? AF_INET6 : AF_INET;

    KlSocketHandle fd = kl_sock_socket(c->ev_ctx->sockets, fam,
                                       c->conn_addrs.ai_socktype, c->conn_addrs.ai_protocol);
    if (!kl_handle_valid(fd)) { *out_err = KL_ERR_CONNECT; return -1; }
    kl_sock_set_nosigpipe(c->ev_ctx->sockets, fd);
    if (kl_sock_set_nonblocking(c->ev_ctx->sockets, fd) < 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        *out_err = KL_ERR_CONNECT;
        return -1;
    }

    /* Completion loop: post the connect over the completion axis (several may race natively). The
     * completion always arrives on a later drain — no inline-success shortcut. */
    if (client_loop_is_completion(c)) {
        if (client_comp_connect(c, fd, sa) != 0) {
            kl_sock_close(c->ev_ctx->sockets, fd);
            *out_err = KL_ERR_CONNECT;
            return -1;
        }
        c->conn_attempts[idx].fd = fd;
        c->conn_attempts[idx].active = 1;
        return 0;
    }

    int rc = kl_sock_connect(c->ev_ctx->sockets, fd, sa);
    if (rc < 0 && kl_sock_io_status(c->ev_ctx->sockets) != KL_IO_PENDING) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        *out_err = KL_ERR_CONNECT;
        return -1;
    }
    if (kl_watcher_add(c->ev_ctx, fd, KL_EVENT_WRITE, async_on_event, c) != 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        *out_err = KL_ERR_CONNECT;
        return -1;
    }
    c->conn_attempts[idx].fd = fd;
    c->conn_attempts[idx].active = 1;
    if (rc == 0)   /* connected immediately (e.g. loopback) — decide this attempt inline */
        kl_connect_op_on_attempt_connected(&c->connect_op, idx, fd);
    return 0;
}

/* Abort an in-flight (not-yet-connected) racing attempt: drop its socket, then REPORT the retirement
 * to the machine. The provider op + fd are removed synchronously here; kl_connect_op_on_attempt_
 * failed clears attempt_active[idx] + pending so the op can reach confirmed detachment (the machine
 * never observes the drop otherwise). Reentrancy is guarded — this hook runs inside the machine's
 * terminal/cancel dispatch. */
static void cli_co_cancel_attempt(void *ctx, int idx)
{
    KlClient *c = ctx;
    if (c->conn_attempts[idx].active) {
        client_drop_connect_fd(c, c->conn_attempts[idx].fd);
        c->conn_attempts[idx].active = 0;
        c->conn_attempts[idx].fd = KL_INVALID_SOCKET;
    }
    kl_connect_op_on_attempt_failed(&c->connect_op, idx, KL_ERR_CONNECT);
}

/* Dispose a connected non-winner descriptor (straggler / out-of-range / duplicate). */
static void cli_co_dispose_fd(void *ctx, KlSocketHandle fd)
{
    KlClient *c = ctx;
    int idx = he_idx_of_fd(c, fd);
    if (idx >= 0) { c->conn_attempts[idx].active = 0; c->conn_attempts[idx].fd = KL_INVALID_SOCKET; }
    client_drop_connect_fd(c, fd);
}

/* Arm / disarm the Connection Attempt Delay (RFC 8305 §5 stagger). arm returns -1 (no timer) when
 * the delay is 0, so the machine fast-starts the next address without staggering. */
static int cli_co_arm_delay(void *ctx)
{
    KlClient *c = ctx;
    if (c->connect_delay_ms <= 0) return -1;
    c->conn_delay_timer = kl_timer_add(c->ev_ctx, (uint64_t)c->connect_delay_ms, he_on_delay, c);
    return (c->conn_delay_timer >= 0) ? 0 : -1;
}
static void cli_co_cancel_delay(void *ctx)
{
    KlClient *c = ctx;
    if (c->conn_delay_timer >= 0) {
        kl_timer_cancel(c->ev_ctx, c->conn_delay_timer);
        c->conn_delay_timer = -1;
    }
}

/* A winner has connected: adopt its fd + enter the shared post-connect path. The losers are
 * cancelled by the machine (co_request_cancels → cli_co_cancel_attempt) right after this returns;
 * the overall deadline stays armed (client-owned) to bound the remaining TLS/send/recv. */
static void he_win(KlClient *c, KlSocketHandle fd)
{
    int idx = he_idx_of_fd(c, fd);
    if (idx >= 0) { c->conn_attempts[idx].active = 0; c->conn_attempts[idx].fd = KL_INVALID_SOCKET; }
    c->fd = fd;
    he_proceed_after_connect(c);
}

/* Terminal: SUCCESS adopts the winner via he_win; FAILED completes the request with the error.
 * CANCELLED is the client's own doing (deadline / teardown) and completes separately. */
static void cli_co_on_done(void *ctx, KlConnectResult result, KlSocketHandle fd, int error)
{
    KlClient *c = ctx;
    if (result == KL_CONNECT_SUCCESS) {
        he_win(c, fd);
    } else if (result == KL_CONNECT_FAILED) {
        /* A resolver-start failure (resolve()==NULL) retires the op cleanly but must NOT fire the
         * request callback — the setup detects connect_start_failed and returns NULL instead. */
        if (c->connect_start_failed)
            return;
        c->error = error ? (KlError)error : KL_ERR_CONNECT;
        async_complete_error(c);
    }
    /* CANCELLED: the client itself requested the cancel (deadline / teardown) and completes
     * separately — nothing to do here. */
}

static void cli_co_on_detach(void *ctx)
{
    KlClient *c = ctx;
    c->conn_racing = 0;   /* the connect op is fully retired — reusable */
}

static const KlConnectOpHooks CLI_CONNECT_HOOKS = {
    .start_resolve  = cli_co_start_resolve,
    .cancel_resolve = cli_co_cancel_resolve,
    .start_attempt  = cli_co_start_attempt,
    .cancel_attempt = cli_co_cancel_attempt,
    .dispose_fd     = cli_co_dispose_fd,
    .arm_delay      = cli_co_arm_delay,
    .cancel_delay   = cli_co_cancel_delay,
    .on_done        = cli_co_on_done,
    .on_detach      = cli_co_on_detach,
};

/* A racing socket became writable: SO_ERROR==0 wins the race, else it failed. Map the fd to its
 * attempt index and drive the connect op; the client owns the failed socket, so drop it before
 * reporting the failure (the machine does not dispose a not-yet-connected attempt). */
static void he_on_writable(KlClient *c, KlSocketHandle fd)
{
    int idx = he_idx_of_fd(c, fd);
    if (idx < 0) return;
    int err = 0;
    kl_sock_get_so_error(c->ev_ctx->sockets, fd, &err);
    if (err == 0) {
        kl_connect_op_on_attempt_connected(&c->connect_op, idx, fd);
    } else {
        c->conn_attempts[idx].active = 0; c->conn_attempts[idx].fd = KL_INVALID_SOCKET;
        client_drop_connect_fd(c, fd);
        kl_connect_op_on_attempt_failed(&c->connect_op, idx, KL_ERR_CONNECT);
    }
}

/* Completion-loop connect result (LC-0): the backend delivered win/fail in the watcher mask
 * (KL_EVENT_WRITE = connected) rather than SO_ERROR (io_uring does not preserve it after a failed
 * IORING_OP_CONNECT). Mirrors he_on_writable's win/fail branch. */
static void he_on_connect_result(KlClient *c, KlSocketHandle fd, KlEventMask ready)
{
    int idx = he_idx_of_fd(c, fd);
    if (idx < 0) return;
    if (ready & KL_EVENT_WRITE) {
        kl_connect_op_on_attempt_connected(&c->connect_op, idx, fd);
    } else {
        c->conn_attempts[idx].active = 0; c->conn_attempts[idx].fd = KL_INVALID_SOCKET;
        client_drop_connect_fd(c, fd);
        kl_connect_op_on_attempt_failed(&c->connect_op, idx, KL_ERR_CONNECT);
    }
}

/* Connection Attempt Delay fired: advance to the next address (the KEEL one-shot timer retired). */
static void he_on_delay(void *user_data)
{
    KlClient *c = user_data;
    c->conn_delay_timer = -1;
    kl_connect_op_on_delay(&c->connect_op);
}

/* Overall request deadline fired: fail the whole request. Cancel the connect op (aborts any racing
 * attempts once + detaches it) then complete with TIMEOUT. Covers connect racing and the otherwise-
 * untimed post-connect TLS handshake / send / recv. */
static void he_on_deadline(void *user_data)
{
    KlClient *c = user_data;
    c->deadline_timer = -1;
    if (c->state == KL_CLIENT_DONE)
        return;
    kl_connect_op_cancel(&c->connect_op);   /* drop any racing sockets (idempotent if terminal) */
    c->error = KL_ERR_TIMEOUT;
    async_complete_error(c);
}

/* ── Async DNS resolve callback ──────────────────────────────────── */

/* Arm the overall request deadline (0 = none). */
static void he_arm_deadline(KlClient *c)
{
    if (c->timeout_ms > 0)
        c->deadline_timer = kl_timer_add(c->ev_ctx, (uint64_t)c->timeout_ms,
                                         he_on_deadline, c);
}

static void dns_resolved(KlResolveReq *req, const KlResolveResult *result,
                          int error, void *user_data)
{
    KlClient *c = user_data;
    (void)req;
    c->resolve_req = NULL;

    if (error || !result || result->naddrs < 1) {
        kl_connect_op_on_resolve_failed(&c->connect_op, KL_ERR_DNS);   /* → terminal FAILED */
        return;
    }

    /* Copy the whole (family-interleaved, preferred-first) list; KlConnectOp races it by index. */
    c->conn_addrs = *result;
    if (c->conn_addrs.naddrs > KL_RESOLVE_MAX_ADDRS)
        c->conn_addrs.naddrs = KL_RESOLVE_MAX_ADDRS;
    for (int i = 0; i < KL_RESOLVE_MAX_ADDRS; i++) {
        c->conn_attempts[i].active = 0;
        c->conn_attempts[i].fd = KL_INVALID_SOCKET;
    }
    c->conn_racing = 1;
    c->state = KL_CLIENT_CONNECTING;

    /* Arm the whole-request deadline (client-owned; bounds racing + post-connect), then hand the
     * address count to the connect op, which drives the racing via the adapter hooks. */
    he_arm_deadline(c);
    kl_connect_op_on_resolved(&c->connect_op, c->conn_addrs.naddrs);
}

/* Select the resolver for an async request. Precedence: explicit cfg->resolver
 * (borrowed) → cfg->system_dns (NULL → blocking sync name resolution) → auto-created
 * built-in async resolver (owned; *owned = 1). Returns NULL to fall back to
 * sync name resolution (also on auto-create failure — better than failing the request). */
static KlResolver *client_pick_resolver(const KlClientConfig *cfg,
                                        KlEventCtx *ev_ctx, int *owned) {
    *owned = 0;
    if (cfg && cfg->resolver)
        return cfg->resolver;
    if (cfg && cfg->system_dns)
        return NULL;
#ifdef KEEL_FREESTANDING
    /* Freestanding (F0 / UEFI): the built-in DNS-over-UDP resolver pulls the UDP +
     * dns_resolver stack, which is out of the minimal completion-client archive
     * (docs/phase10_uefi_feasibility_design.md §8 — IPv4/numeric first, DNS is U-5).
     * A freestanding consumer supplies cfg->resolver or a numeric address; here we
     * fall back to sync name resolution (kl_resolve_sync), same as cfg->system_dns. */
    (void)ev_ctx;
    return NULL;
#else
    KlResolver *r = kl_dns_resolver_create(ev_ctx, NULL);
    if (r)
        *owned = 1;
    return r;
#endif
}

/* ── State: CONNECTING ───────────────────────────────────────────── */

/* Single-fd connect completion (UNIX socket + sync sync name resolution paths). The
 * Happy Eyeballs path uses he_on_writable instead. */
static void async_handle_connecting(KlClient *c)
{
    int err = 0;
    kl_sock_get_so_error(c->ev_ctx->sockets, c->fd, &err);
    if (err != 0) {
        c->error = KL_ERR_CONNECT;
        async_complete_error(c);
        return;
    }
    he_proceed_after_connect(c);
}

/* Shared post-connect path: c->fd is a connected socket (Happy Eyeballs winner,
 * or the single-fd UNIX / sync-sync name resolution connect). Advances to the proxy /
 * TLS / sending state. */
static void he_proceed_after_connect(KlClient *c)
{
    /* Proxy: HTTPS target needs CONNECT tunnel first */
    if (c->is_proxied && c->is_tunnel) {
        if (build_connect_request(c, c->host_buf, c->target_port,
                                    c->proxy_auth) != 0) {
            c->error = KL_ERR_ALLOC;
            async_complete_error(c);
            return;
        }
        c->state = KL_CLIENT_PROXY_CONNECTING;
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
        return;
    }

    /* Proxy: HTTP target — request already has absolute-form URL, send directly */
    if (c->is_proxied) {
        c->state = KL_CLIENT_SENDING;
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
        return;
    }

    if (c->tls_cfg && c->tls_cfg->factory) {
        c->tls = c->tls_cfg->factory(c->tls_cfg->ctx, c->alloc);
        if (!c->tls) {
            c->error = KL_ERR_TLS_INIT;
            async_complete_error(c);
            return;
        }

        /* Route the TLS socket-BIO through the client's socket provider (e.g. lwIP). */
        if (c->tls->set_socket_provider)
            c->tls->set_socket_provider(c->tls, c->ev_ctx->sockets);
        /* FAIL CLOSED on set_hostname failure: without hostname verification a
         * cert for the wrong host would verify against the CA chain alone.
         * async_complete_error owns c->tls teardown (incl. the pool-discard path). */
        if (c->tls->set_hostname && c->host_buf[0] &&
            c->tls->set_hostname(c->tls, c->host_buf) != 0) {
            c->error = KL_ERR_TLS_INIT;
            async_complete_error(c);
            return;
        }

        c->state = KL_CLIENT_TLS_HANDSHAKE;
        async_handle_tls_handshake(c);
    } else {
        c->state = KL_CLIENT_SENDING;
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
    }
}

/* ── State: PROXY_CONNECTING (send CONNECT request) ──────────────── */

static void async_handle_proxy_connecting(KlClient *c)
{
    while (c->connect_sent < c->connect_len) {
        /* Send the CONNECT request over the provider (not raw write) so the proxy
         * path also works over a non-OS-fd provider; classify -1 via io_status. */
        kl_ssize_t w = kl_sock_send(c->ev_ctx->sockets, c->fd,
                                    c->connect_buf + c->connect_sent,
                                    c->connect_len - c->connect_sent);
        if (w < 0) {
            KlIoStatus st = kl_sock_io_status(c->ev_ctx->sockets);
            if (st == KL_IO_WOULD_BLOCK) {
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
            if (st == KL_IO_INTERRUPTED)
                continue;
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        if (w == 0) {
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        c->connect_sent += (size_t)w;
    }

    /* CONNECT request fully sent — switch to reading proxy response */
    kl_free(c->alloc, c->connect_buf, c->connect_len);
    c->connect_buf = NULL;

    /* Allocate proxy response buffer */
    c->proxy_recv = kl_malloc(c->alloc, KL_PROXY_RESPONSE_MAX);
    if (!c->proxy_recv) {
        c->error = KL_ERR_ALLOC;
        async_complete_error(c);
        return;
    }
    c->proxy_recv_len = 0;

    c->state = KL_CLIENT_PROXY_HANDSHAKE;
    kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_READ);
}

/* ── State: PROXY_HANDSHAKE (read proxy 200 response) ────────────── */

static void async_handle_proxy_handshake(KlClient *c)
{
    for (;;) {
        if (c->proxy_recv_len >= KL_PROXY_RESPONSE_MAX - 1) {
            c->error = KL_ERR_PROXY;
            async_complete_error(c);
            return;
        }

        /* Read the proxy response over the provider (not raw read); classify -1. */
        kl_ssize_t r = kl_sock_recv(c->ev_ctx->sockets, c->fd,
                                    c->proxy_recv + c->proxy_recv_len,
                                    KL_PROXY_RESPONSE_MAX - 1 - c->proxy_recv_len);
        if (r < 0) {
            KlIoStatus st = kl_sock_io_status(c->ev_ctx->sockets);
            if (st == KL_IO_WOULD_BLOCK) {
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
            if (st == KL_IO_INTERRUPTED)
                continue;
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        if (r == 0) {
            c->error = KL_ERR_PROXY;
            async_complete_error(c);
            return;
        }

        c->proxy_recv_len += (size_t)r;
        c->proxy_recv[c->proxy_recv_len] = '\0';

        /* Shared status check (client_proxy.c): 0 = need more, 1 = tunnel up, -1 = error. */
        int st = kl_proxy_connect_status(c->proxy_recv, c->proxy_recv_len);
        if (st == 0)
            continue;
        if (st < 0) {
            c->error = KL_ERR_PROXY;
            async_complete_error(c);
            return;
        }

        /* CONNECT tunnel established — free buffer, start TLS */
        kl_free(c->alloc, c->proxy_recv, KL_PROXY_RESPONSE_MAX);
        c->proxy_recv = NULL;

        /* Create TLS session over the tunnel */
        if (!c->tls_cfg || !c->tls_cfg->factory) {
            c->error = KL_ERR_TLS_INIT;
            async_complete_error(c);
            return;
        }

        c->tls = c->tls_cfg->factory(c->tls_cfg->ctx, c->alloc);
        if (!c->tls) {
            c->error = KL_ERR_TLS_INIT;
            async_complete_error(c);
            return;
        }

        /* Route the TLS socket-BIO through the client's socket provider (e.g. lwIP). */
        if (c->tls->set_socket_provider)
            c->tls->set_socket_provider(c->tls, c->ev_ctx->sockets);
        /* FAIL CLOSED on set_hostname failure (see the direct-connect path);
         * async_complete_error owns c->tls teardown. */
        if (c->tls->set_hostname && c->host_buf[0] &&
            c->tls->set_hostname(c->tls, c->host_buf) != 0) {
            c->error = KL_ERR_TLS_INIT;
            async_complete_error(c);
            return;
        }

        c->state = KL_CLIENT_TLS_HANDSHAKE;
        async_handle_tls_handshake(c);
        return;
    }
}

/* ── State: TLS_HANDSHAKE ────────────────────────────────────────── */

static void async_handle_tls_handshake(KlClient *c)
{
    KlTlsResult r = c->tls->handshake(c->tls, c->fd);
    if (r == KL_TLS_OK) {
        c->state = KL_CLIENT_SENDING;
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
        return;
    }
    if (r == KL_TLS_WANT_READ) {
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_READ);
        return;
    }
    if (r == KL_TLS_WANT_WRITE) {
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
        return;
    }
    c->error = KL_ERR_TLS_HANDSHAKE;
    async_complete_error(c);
}

/* ── State: SENDING ──────────────────────────────────────────────── */

static void async_handle_sending(KlClient *c)
{
    while (c->request_sent < c->request_len) {
        kl_ssize_t w = kl_client_io_write(c->ev_ctx->sockets, c->fd, c->tls,
                              c->request_buf + c->request_sent,
                              c->request_len - c->request_sent);
        if (w < 0) {
            if (kl_sock_io_status(c->ev_ctx->sockets) == KL_IO_WOULD_BLOCK) {
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        if (w == 0) {
            c->error = KL_ERR_IO;
            async_complete_error(c);
            return;
        }
        c->request_sent += (size_t)w;
    }

    /* If streaming body, transition to chunk-send state */
    if (c->body_read) {
        c->state = KL_CLIENT_SENDING_STREAM;
        c->chunk_phase = 0;
        async_handle_sending_stream(c);
        return;
    }

    c->state = KL_CLIENT_RECEIVING;
    kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_READ);
}

/* ── State: SENDING_STREAM (chunked body from body_read) ─────────── */

static void async_handle_sending_stream(KlClient *c)
{
    for (;;) {
        switch (c->chunk_phase) {
        case 0: {
            /* Read next chunk from body_read */
            ssize_t nr = c->body_read(c->chunk_buf, sizeof(c->chunk_buf),
                                       c->stream_user_data);
            if (nr < 0) {
                async_complete_error(c);
                return;
            }
            if (nr == 0) {
                /* EOF — send final chunk */
                memcpy(c->chunk_hdr, "0\r\n\r\n", KL_CLIENT_FINAL_CHUNK_LEN);
                c->chunk_hdr_len = KL_CLIENT_FINAL_CHUNK_LEN;
                c->chunk_hdr_sent = 0;
                c->chunk_phase = 4;
                continue;
            }
            c->chunk_len = (size_t)nr;
            c->chunk_sent = 0;
            /* Format chunk header: lowercase-hex size + CRLF (was "%zx\r\n") */
            size_t hoff = 0;
            if (kl_buf_append_hex(c->chunk_hdr, sizeof(c->chunk_hdr), &hoff,
                                  (uint64_t)nr) != 0 ||
                kl_buf_append_n(c->chunk_hdr, sizeof(c->chunk_hdr), &hoff,
                                "\r\n", 2) != 0) {
                async_complete_error(c);
                return;
            }
            c->chunk_hdr_len = hoff;
            c->chunk_hdr_sent = 0;
            c->chunk_phase = 1;
        }
            /* fall through */

        case 1:
            /* Send chunk header */
            while (c->chunk_hdr_sent < c->chunk_hdr_len) {
                kl_ssize_t w = kl_client_io_write(c->ev_ctx->sockets, c->fd, c->tls,
                                      c->chunk_hdr + c->chunk_hdr_sent,
                                      c->chunk_hdr_len - c->chunk_hdr_sent);
                if (w < 0) {
                    if (kl_sock_io_status(c->ev_ctx->sockets) == KL_IO_WOULD_BLOCK) {
                        kl_watcher_rearm(c->ev_ctx, c->fd);
                        return;
                    }
                    async_complete_error(c);
                    return;
                }
                if (w == 0) { async_complete_error(c); return; }
                c->chunk_hdr_sent += (size_t)w;
            }
            c->chunk_phase = 2;
            /* fall through */

        case 2:
            /* Send chunk data */
            while (c->chunk_sent < c->chunk_len) {
                kl_ssize_t w = kl_client_io_write(c->ev_ctx->sockets, c->fd, c->tls,
                                      c->chunk_buf + c->chunk_sent,
                                      c->chunk_len - c->chunk_sent);
                if (w < 0) {
                    if (kl_sock_io_status(c->ev_ctx->sockets) == KL_IO_WOULD_BLOCK) {
                        kl_watcher_rearm(c->ev_ctx, c->fd);
                        return;
                    }
                    async_complete_error(c);
                    return;
                }
                if (w == 0) { async_complete_error(c); return; }
                c->chunk_sent += (size_t)w;
            }
            /* Reuse chunk_hdr for trailing \r\n */
            c->chunk_hdr[0] = '\r';
            c->chunk_hdr[1] = '\n';
            c->chunk_hdr_len = 2;
            c->chunk_hdr_sent = 0;
            c->chunk_phase = 3;
            /* fall through */

        case 3:
            /* Send trailing \r\n */
            while (c->chunk_hdr_sent < c->chunk_hdr_len) {
                kl_ssize_t w = kl_client_io_write(c->ev_ctx->sockets, c->fd, c->tls,
                                      c->chunk_hdr + c->chunk_hdr_sent,
                                      c->chunk_hdr_len - c->chunk_hdr_sent);
                if (w < 0) {
                    if (kl_sock_io_status(c->ev_ctx->sockets) == KL_IO_WOULD_BLOCK) {
                        kl_watcher_rearm(c->ev_ctx, c->fd);
                        return;
                    }
                    async_complete_error(c);
                    return;
                }
                if (w == 0) { async_complete_error(c); return; }
                c->chunk_hdr_sent += (size_t)w;
            }
            /* Next chunk */
            c->chunk_phase = 0;
            continue;

        case 4:
            /* Send final chunk (0\r\n\r\n) */
            while (c->chunk_hdr_sent < c->chunk_hdr_len) {
                kl_ssize_t w = kl_client_io_write(c->ev_ctx->sockets, c->fd, c->tls,
                                      c->chunk_hdr + c->chunk_hdr_sent,
                                      c->chunk_hdr_len - c->chunk_hdr_sent);
                if (w < 0) {
                    if (kl_sock_io_status(c->ev_ctx->sockets) == KL_IO_WOULD_BLOCK) {
                        kl_watcher_rearm(c->ev_ctx, c->fd);
                        return;
                    }
                    async_complete_error(c);
                    return;
                }
                if (w == 0) { async_complete_error(c); return; }
                c->chunk_hdr_sent += (size_t)w;
            }
            /* Done sending — switch to receiving */
            c->state = KL_CLIENT_RECEIVING;
            kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_READ);
            return;

        default:
            async_complete_error(c);
            return;
        }
    }
}

/* ── State: RECEIVING ────────────────────────────────────────────── */

static void async_handle_receiving(KlClient *c)
{
    char buf[KL_CLIENT_RECV_BUF_SIZE];

    for (;;) {
        kl_ssize_t nread = kl_client_io_read(c->ev_ctx->sockets, c->fd, c->tls, buf, sizeof(buf));
        if (nread < 0) {
            if (kl_sock_io_status(c->ev_ctx->sockets) == KL_IO_WOULD_BLOCK) {
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
            /* A clean TLS shutdown surfaces as read()==-1 (the vtable read contract
             * has no distinct EOF code). Treat it like a socket EOF so a close-
             * delimited response (HTTP/1.0 / Connection: close, no Content-Length)
             * is finalized rather than reported as KL_ERR_IO. This is the REAL TLS
             * EOF signal — finalize here directly (do NOT fall through to the
             * nread==0 rearm below, which is for the WANT_READ 0 only). */
            if (c->tls && c->tls->at_eof && c->tls->at_eof(c->tls))
                goto eof;
            async_complete_error(c);
            return;
        }
        if (nread == 0) {
            if (c->tls) {
                /* Over TLS, read()==0 means the vtable returned WANT_READ (mbedTLS
                 * WANT_READ maps to 0), NOT end-of-stream — e.g. a non-blocking recv
                 * returned would-block mid-record. Re-arm and wait for more ciphertext.
                 * A real TLS EOF arrives via read()==-1 + at_eof() (handled above),
                 * never via 0. (Latent bug pre-U-6: masked only because the old EFI
                 * sync pump never returned WANT_READ.) */
                kl_watcher_rearm(c->ev_ctx, c->fd);
                return;
            }
        eof:
            if (c->resp.status > 0)
                async_complete_success(c);
            else {
                c->error = KL_ERR_PARSE;
                async_complete_error(c);
            }
            return;
        }

        size_t consumed;
        KlParseResult pr = c->parser->parse(c->parser, &c->resp,
                                              buf, (size_t)nread, &consumed);
        if (pr == KL_PARSE_OK) {
            async_complete_success(c);
            return;
        }
        if (pr == KL_PARSE_ERROR) {
            c->error = KL_ERR_PARSE;
            async_complete_error(c);
            return;
        }
        /* KL_PARSE_INCOMPLETE — try to read more (non-blocking) */
    }
}

/* ── KlWatcher callback ─────────────────────────────────────────── */

static void async_on_event(KlSocketHandle fd, KlEventMask ready, void *user_data)
{
    KlClient *c = user_data;

    switch (c->state) {
    case KL_CLIENT_RESOLVING:
        break;  /* DNS resolution handled by resolver callback, not watcher */
    case KL_CLIENT_CONNECTING:
        /* Happy Eyeballs races several fds — dispatch by the fd that fired.
         * The single-fd UNIX / sync-sync name resolution path uses c->fd directly.
         * On a completion loop the connect result is carried in `ready` (KL_EVENT_WRITE =
         * connected) — the backend already resolved win/fail (SO_ERROR isn't preserved after a
         * failed io_uring connect), so trust the delivered result instead of re-reading it. */
        if (client_loop_is_completion(c)) {
            if (c->conn_racing)
                he_on_connect_result(c, fd, ready);
            else if (ready & KL_EVENT_WRITE)
                he_proceed_after_connect(c);
            else { c->error = KL_ERR_CONNECT; async_complete_error(c); }
        } else if (c->conn_racing) {
            he_on_writable(c, fd);
        } else {
            async_handle_connecting(c);
        }
        break;
    case KL_CLIENT_PROXY_CONNECTING:
        async_handle_proxy_connecting(c);
        break;
    case KL_CLIENT_PROXY_HANDSHAKE:
        async_handle_proxy_handshake(c);
        break;
    case KL_CLIENT_TLS_HANDSHAKE:
        async_handle_tls_handshake(c);
        break;
    case KL_CLIENT_SENDING:
        async_handle_sending(c);
        break;
    case KL_CLIENT_SENDING_STREAM:
        async_handle_sending_stream(c);
        break;
    case KL_CLIENT_RECEIVING:
        async_handle_receiving(c);
        break;
    case KL_CLIENT_DONE:
        break;
    }
}

/* ── Completion helpers ──────────────────────────────────────────── */

static void async_complete_success(KlClient *c)
{
    he_cancel_timers(c);
    kl_watcher_del(c->ev_ctx, c->fd);

    if (c->pool) {
        /* Pool-aware: release or discard based on Connection header */
        c->pool_conn.fd = c->fd;
        c->pool_conn.tls = c->tls;
        if (kl_client_server_wants_close(&c->resp)) {
            kl_cpool_discard(c->pool, &c->pool_conn);
        } else {
            kl_cpool_release(c->pool, &c->pool_conn,
                              c->host_buf, c->pool_port, c->pool_is_tls,
                              NULL, 0);
        }
        c->tls = NULL;
        c->fd = KL_INVALID_SOCKET;
    } else {
        if (c->tls) {
            c->tls->shutdown(c->tls, c->fd);
            c->tls->destroy(c->tls);
            c->tls = NULL;
        }
        kl_sock_close(c->ev_ctx->sockets, c->fd);
        c->fd = KL_INVALID_SOCKET;
    }

    if (c->request_buf) {
        kl_free(c->alloc, c->request_buf, c->request_len);
        c->request_buf = NULL;
    }
    if (c->parser) {
        c->parser->destroy(c->parser);
        c->parser = NULL;
    }

    /* Decompress buffered response body if applicable */
    if (!c->decomp_wrap) {
        kl_client_decompress_response_body(&c->resp, c->decompress_cfg);
    }

    c->state = KL_CLIENT_DONE;
    c->error = KL_ERR_NONE;
    if (c->on_done)
        c->on_done(c, c->user_data);
}

static void async_complete_error(KlClient *c)
{
    he_cancel_timers(c);
    if (kl_handle_valid(c->fd))
        kl_watcher_del(c->ev_ctx, c->fd);

    if (c->pool) {
        /* Pool-aware: discard the connection on error */
        c->pool_conn.fd = c->fd;
        c->pool_conn.tls = c->tls;
        kl_cpool_discard(c->pool, &c->pool_conn);
        c->tls = NULL;
        c->fd = KL_INVALID_SOCKET;
    } else {
        if (c->tls) {
            if (kl_handle_valid(c->fd))
                c->tls->shutdown(c->tls, c->fd);
            c->tls->destroy(c->tls);
            c->tls = NULL;
        }

        if (kl_handle_valid(c->fd)) {
            kl_sock_close(c->ev_ctx->sockets, c->fd);
            c->fd = KL_INVALID_SOCKET;
        }
    }

    if (c->request_buf) {
        kl_free(c->alloc, c->request_buf, c->request_len);
        c->request_buf = NULL;
    }
    if (c->parser) {
        c->parser->destroy(c->parser);
        c->parser = NULL;
    }

    /* Free proxy buffers */
    if (c->connect_buf) {
        kl_free(c->alloc, c->connect_buf, c->connect_len);
        c->connect_buf = NULL;
    }
    if (c->proxy_recv) {
        kl_free(c->alloc, c->proxy_recv, KL_PROXY_RESPONSE_MAX);
        c->proxy_recv = NULL;
    }

    c->state = KL_CLIENT_DONE;
    /* error already set by caller — fallback if not set */
    if (c->error == KL_ERR_NONE)
        c->error = KL_ERR_IO;
    if (c->on_done)
        c->on_done(c, c->user_data);
}

/* ── Async public API ────────────────────────────────────────────── */

KlClient *kl_client_start_s(KlEventCtx *ev_ctx, KlAllocator *alloc,
                              const KlClientConfig *cfg,
                              const char *method, const char *url_str,
                              const KlClientHeader *headers, int num_headers,
                              const char *body, size_t body_len,
                              const KlClientStreamCfg *stream,
                              KlClientDoneFn on_done, void *user_data)
{
    if (!ev_ctx || !alloc || !method || !url_str)
        return NULL;
    if (num_headers < 0 || num_headers > KL_CLIENT_MAX_REQ_HEADERS)
        return NULL;
    if (num_headers > 0 && !headers)
        return NULL;

    KlUrl parsed;
    if (kl_url_parse(url_str, &parsed) != 0)
        return NULL;
    /* UNIX socket target: no host/port. Normalize the Host header to
     * "localhost" so all request building works unchanged; the connect
     * (below) goes to parsed.unix_path, bypassing DNS. Proxy is incompatible. */
    if (parsed.is_unix) {
        if (cfg && cfg->proxy && cfg->proxy->host)
            return NULL;
        parsed.host = "localhost";
        parsed.host_len = 9;
    }

    KlTlsConfig *tls_cfg = cfg ? cfg->tls : NULL;
    if (parsed.is_https && !tls_cfg)
        return NULL;
    if (!parsed.is_https)
        tls_cfg = NULL;

    size_t max_resp = (cfg && cfg->max_response_size > 0) ? cfg->max_response_size
                                                            : (size_t)KL_CLIENT_DEFAULT_MAX_RESP;

    /* Proxy routing */
    const KlProxyConfig *proxy = cfg ? cfg->proxy : NULL;
    int is_proxied = (proxy && proxy->host);
    int is_tunnel = is_proxied && parsed.is_https;

    /* For HTTPS through proxy: tls_cfg stays set (used after CONNECT),
     * but we don't do TLS on initial connect to proxy */

    /* Build absolute-form URL for HTTP forwarding through proxy */
    char abs_url_buf[KL_CLIENT_REQ_BUF_SIZE];
    const char *absolute_url = NULL;
    if (is_proxied && !parsed.is_https) {
        char host_z[KL_CLIENT_HOSTNAME_MAX];
        if (parsed.host_len >= sizeof(host_z))
            return NULL;
        memcpy(host_z, parsed.host, parsed.host_len);
        host_z[parsed.host_len] = '\0';

        const char *path = (parsed.path_len > 0) ? parsed.path : "/";
        int path_len = (parsed.path_len > 0) ? (int)parsed.path_len : 1;

        /* "http://<host>[:<port>]<path>" — byte-identical to the former
         * snprintf, built with bounded, locale-free append helpers. */
        size_t n = 0;
        if (kl_buf_append(abs_url_buf, sizeof(abs_url_buf), &n, "http://") != 0 ||
            kl_buf_append(abs_url_buf, sizeof(abs_url_buf), &n, host_z) != 0)
            return NULL;
        if (parsed.port != 80) {
            if (kl_buf_append_n(abs_url_buf, sizeof(abs_url_buf), &n, ":", 1) != 0 ||
                kl_buf_append_u64(abs_url_buf, sizeof(abs_url_buf), &n,
                                  (uint64_t)parsed.port) != 0)
                return NULL;
        }
        if (kl_buf_append_n(abs_url_buf, sizeof(abs_url_buf), &n, path,
                            (size_t)path_len) != 0)
            return NULL;
        absolute_url = abs_url_buf;
    }

    /* Build request buffer — headers-only for streaming, full for buffered */
    size_t req_len = 0;
    char *req_buf;
    if (stream && stream->body_read) {
        req_buf = kl_client_build_request_headers_only(alloc, method, &parsed,
                                               headers, num_headers, &req_len, 0,
                                               absolute_url);
    } else {
        req_buf = kl_client_build_request(alloc, method, &parsed,
                                 headers, num_headers, body, body_len,
                                 &req_len, 0, absolute_url);
    }
    if (!req_buf)
        return NULL;

    /* Copy hostname for SNI (always the target host, not the proxy) */
    char host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (parsed.host_len >= sizeof(host_buf)) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memcpy(host_buf, parsed.host, parsed.host_len);
    host_buf[parsed.host_len] = '\0';

    /* Allocate client */
    KlClient *c = kl_malloc(alloc, sizeof(KlClient));
    if (!c) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    c->fd = KL_INVALID_SOCKET;
    c->ev_ctx = ev_ctx;
    if (cfg && cfg->sockets) c->ev_ctx->sockets = cfg->sockets;  /* provider selection */
    /* PAL 8f-5a: on a completion loop, adopt the backend's native overlapped provider when
     * the configured one is incompatible — so a completion backend is a drop-in for the
     * client too (the event axis stays masked). */
    if (!kl_event_ctx_sockets_compatible(c->ev_ctx)) {
        const struct KlSocketProvider *np = kl_event_native_provider(&c->ev_ctx->loop);
        if (np) c->ev_ctx->sockets = np;
    }
    /* PAL Phase 7: the async client's readiness loop must be able to watch the
     * provider's handles (native fds). Reject an incoherent pairing up front. */
    if (!kl_event_ctx_sockets_compatible(c->ev_ctx)) {
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }
    c->alloc = alloc;
    c->tls_cfg = is_tunnel ? tls_cfg : NULL;  /* only for CONNECT tunnels */
    c->request_buf = req_buf;
    c->request_len = req_len;
    c->request_sent = 0;
    c->on_done = on_done;
    c->user_data = user_data;
    memcpy(c->host_buf, host_buf, parsed.host_len + 1);

    /* Happy Eyeballs / deadline timer state (timer ids: -1 = unset). */
    c->conn_delay_timer = -1;
    c->deadline_timer = -1;
    c->timeout_ms = (cfg && cfg->timeout_ms > 0) ? cfg->timeout_ms
                                                 : KL_CLIENT_DEFAULT_TIMEOUT_MS;
    c->connect_delay_ms = (cfg && cfg->connect_attempt_delay_ms > 0)
                              ? cfg->connect_attempt_delay_ms
                              : KL_CLIENT_CONNECT_ATTEMPT_DELAY_MS;

    /* Proxy state */
    c->is_proxied = is_proxied;
    c->is_tunnel = is_tunnel;
    c->target_port = (uint16_t)parsed.port;
    c->proxy_auth = (proxy && proxy->auth) ? proxy->auth : NULL;

    /* Non-tunnel direct connections still need tls_cfg */
    if (!is_proxied)
        c->tls_cfg = tls_cfg;

    /* Response decompression config */
    KlDecompressConfig *dcfg = cfg ? cfg->decompress : NULL;
    c->decompress_cfg = dcfg;

    /* Request streaming */
    if (stream && stream->body_read) {
        c->body_read = stream->body_read;
        c->stream_user_data = stream->user_data;
    }

    /* Create response parser (streaming or buffered) */
    if (stream && stream->on_body) {
        /* Wrap streaming callbacks with decompression if configured */
        if (dcfg && dcfg->factory) {
            DecompStreamWrap *w = kl_malloc(alloc, sizeof(DecompStreamWrap));
            if (!w) {
                kl_free(alloc, req_buf, req_len);
                kl_free(alloc, c, sizeof(KlClient));
                return NULL;
            }
            memset(w, 0, sizeof(*w));
            w->user_on_body = stream->on_body;
            w->user_on_headers = stream->on_headers;
            w->user_on_complete = stream->on_complete;
            w->user_data = stream->user_data;
            w->dcfg = dcfg;
            w->ds.alloc = alloc;
            c->decomp_wrap = w;

            c->parser = kl_response_parser_llhttp_s(max_resp, alloc,
                                                      kl_client_decomp_on_body,
                                                      kl_client_decomp_on_headers,
                                                      kl_client_decomp_on_complete,
                                                      w);
        } else {
            c->parser = kl_response_parser_llhttp_s(max_resp, alloc,
                                                      stream->on_body,
                                                      stream->on_headers,
                                                      stream->on_complete,
                                                      stream->user_data);
        }
    } else {
        c->parser = kl_response_parser_llhttp(max_resp, alloc);
    }
    if (!c->parser) {
        if (c->decomp_wrap) {
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        }
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    /* UNIX socket: connect directly, bypassing DNS resolution entirely. */
    if (parsed.is_unix) {
        KlSockAddr un;
        if (kl_sockaddr_from_unix(&un, parsed.unix_path) != 0 ||
            start_connect(c, &un) < 0) {
            c->parser->destroy(c->parser);
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        he_arm_deadline(c);   /* bound the connect/send/recv (single-fd path) */
        return c;
    }

    /* DNS resolution — resolve proxy host when proxied, target host otherwise. The host itself is
     * taken from the persistent c->host_buf / borrowed proxy->host at the connect-op wiring below. */
    int resolve_port = is_proxied ? proxy->port : parsed.port;

    /* Async DNS resolver path (explicit, built-in default, or sync name resolution). */
    int res_owned = 0;
    KlResolver *resolver = client_pick_resolver(cfg, ev_ctx, &res_owned);
    if (resolver) {
        c->resolver = resolver;
        c->owns_resolver = res_owned;
        /* Persistent host for start_resolve: c->host_buf (already copied above) for the target, or
         * the borrowed proxy host (valid through the request) when proxied — no dangling local. */
        c->resolve_host = is_proxied ? proxy->host : c->host_buf;
        c->resolve_port = resolve_port;
        c->state = KL_CLIENT_RESOLVING;
        /* Drive resolve + Happy Eyeballs via KlConnectOp (6C). kl_connect_op_start runs start_resolve
         * synchronously (which kicks resolver->resolve); a sync-completion-capable resolver may drive
         * the whole request to terminal inline. init/start are infallible (valid static hook table). */
        kl_connect_op_init(&c->connect_op, &CLI_CONNECT_HOOKS, c);
        kl_connect_op_start(&c->connect_op);
        if (c->connect_start_failed) {
            /* resolver->resolve() could not start: the op retired terminal + detached with no user
             * callback. Free the client and report the start failure as a NULL return (contract). */
            if (res_owned)
                resolver->destroy(resolver);
            c->resolver = NULL;
            c->owns_resolver = 0;
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            c->parser->destroy(c->parser);
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        /* Otherwise: still RESOLVING (deferred; resolve_req stored by start_resolve) or the resolver
         * completed inline (op advanced/terminal, possibly firing on_done + taking ownership of c).
         * Both cases return c — the request is live or already completed via on_done. */
        return c;
    }

    /* Sync DNS fallback (blocking sync name resolution) */
    char dns_host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (is_proxied) {
        size_t phl = strlen(proxy->host);
        if (phl >= sizeof(dns_host_buf)) {
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            c->parser->destroy(c->parser);
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        memcpy(dns_host_buf, proxy->host, phl + 1);
    } else {
        memcpy(dns_host_buf, host_buf, parsed.host_len + 1);
    }

    KlSockAddr addrs[KL_RESOLVE_MAX_ADDRS];
    int naddr = 0;
    if (kl_resolve_sync(dns_host_buf, (uint16_t)resolve_port, SOCK_STREAM,
                        addrs, KL_RESOLVE_MAX_ADDRS, &naddr) != 0) {
        if (c->decomp_wrap)
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        c->parser->destroy(c->parser);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    if (start_connect(c, &addrs[0]) < 0) {
        if (c->decomp_wrap)
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        c->parser->destroy(c->parser);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    he_arm_deadline(c);   /* bound the connect/send/recv (single-fd path) */
    return c;
}

KlClient *kl_client_start(KlEventCtx *ev_ctx, KlAllocator *alloc,
                           const KlClientConfig *cfg,
                           const char *method, const char *url_str,
                           const KlClientHeader *headers, int num_headers,
                           const char *body, size_t body_len,
                           KlClientDoneFn on_done, void *user_data)
{
    return kl_client_start_s(ev_ctx, alloc, cfg, method, url_str,
                              headers, num_headers, body, body_len,
                              NULL, on_done, user_data);
}

const KlClientResponse *kl_client_response(const KlClient *client)
{
    if (!client || client->error != KL_ERR_NONE)
        return NULL;
    return &client->resp;
}

int kl_client_error(const KlClient *client)
{
    if (!client)
        return -1;
    return client->error != KL_ERR_NONE ? -1 : 0;
}

KlError kl_client_last_error(const KlClient *client)
{
    if (!client)
        return KL_ERR_INVALID_ARG;
    return client->error;
}

void kl_client_cancel(KlClient *client)
{
    if (!client)
        return;

    /* Abort the connect op (6C): cancels an in-flight resolve, drops every racing attempt socket
     * (cli_co_cancel_attempt), and disarms the Connection Attempt Delay — all exactly once. */
    kl_connect_op_cancel(&client->connect_op);
    /* The overall request deadline is client-owned (not a connect-op timer) — cancel it here. */
    he_cancel_timers(client);

    if (kl_handle_valid(client->fd)) {
        /* Single-fd completion connect still in flight: drop its pending connect op before the
         * watcher/fd go away (HE attempts were already dropped by he_close_attempts above). */
        if (client->state == KL_CLIENT_CONNECTING && client_loop_is_completion(client))
            kl_comp_cancel(client->ev_ctx, client->fd);
        kl_watcher_del(client->ev_ctx, client->fd);

        if (client->pool) {
            client->pool_conn.fd = client->fd;
            client->pool_conn.tls = client->tls;
            kl_cpool_discard(client->pool, &client->pool_conn);
            client->tls = NULL;
            client->fd = KL_INVALID_SOCKET;
        } else {
            if (client->tls) {
                client->tls->shutdown(client->tls, client->fd);
                client->tls->destroy(client->tls);
                client->tls = NULL;
            }

            kl_sock_close(client->ev_ctx->sockets, client->fd);
            client->fd = KL_INVALID_SOCKET;
        }
    }

    client->state = KL_CLIENT_DONE;
    if (client->error == KL_ERR_NONE)
        client->error = KL_ERR_IO;
}

void kl_client_free(KlClient *client)
{
    if (!client)
        return;

    /* Cancel if still in-flight (cancels any pending resolve_req first). */
    if (client->state != KL_CLIENT_DONE)
        kl_client_cancel(client);

    /* Destroy the auto-created resolver (after any resolve_req was cancelled). */
    if (client->owns_resolver && client->resolver) {
        client->resolver->destroy(client->resolver);
        client->resolver = NULL;
        client->owns_resolver = 0;
    }

    if (client->request_buf) {
        kl_free(client->alloc, client->request_buf, client->request_len);
        client->request_buf = NULL;
    }

    if (client->parser) {
        client->parser->destroy(client->parser);
        client->parser = NULL;
    }

    kl_client_response_free(&client->resp);

    if (client->decomp_wrap) {
        if (client->decomp_wrap->active)
            kl_decompress_stream_free(&client->decomp_wrap->ds);
        kl_free(client->alloc, client->decomp_wrap, sizeof(DecompStreamWrap));
        client->decomp_wrap = NULL;
    }

    /* Free proxy buffers */
    if (client->connect_buf) {
        kl_free(client->alloc, client->connect_buf, client->connect_len);
        client->connect_buf = NULL;
    }
    if (client->proxy_recv) {
        kl_free(client->alloc, client->proxy_recv, KL_PROXY_RESPONSE_MAX);
        client->proxy_recv = NULL;
    }

    KlAllocator *alloc = client->alloc;
    kl_free(alloc, client, sizeof(KlClient));
}

/* ══════════════════════════════════════════════════════════════════════
 * Pooled async client API — connection pool integration
 * ══════════════════════════════════════════════════════════════════════ */

KlClient *kl_client_start_pooled(KlClientPool *pool,
                                   KlEventCtx *ev_ctx, KlAllocator *alloc,
                                   const KlClientConfig *cfg,
                                   const char *method, const char *url_str,
                                   const KlClientHeader *headers, int num_headers,
                                   const char *body, size_t body_len,
                                   KlClientDoneFn on_done, void *user_data)
{
    if (!pool || !ev_ctx || !alloc || !method || !url_str)
        return NULL;
    if (num_headers < 0 || num_headers > KL_CLIENT_MAX_REQ_HEADERS)
        return NULL;
    if (num_headers > 0 && !headers)
        return NULL;

    KlUrl parsed;
    if (kl_url_parse(url_str, &parsed) != 0)
        return NULL;
    /* UNIX sockets are not pooled (no host:port key); bypass the pool and
     * start a normal non-pooled async request. */
    if (parsed.is_unix) {
        return kl_client_start_s(ev_ctx, alloc, cfg, method, url_str,
                                 headers, num_headers, body, body_len,
                                 NULL, on_done, user_data);
    }

    KlTlsConfig *tls_cfg = cfg ? cfg->tls : NULL;
    int is_tls = parsed.is_https;
    if (is_tls && !tls_cfg)
        return NULL;
    if (!is_tls)
        tls_cfg = NULL;

    size_t max_resp = (cfg && cfg->max_response_size > 0) ? cfg->max_response_size
                                                            : (size_t)KL_CLIENT_DEFAULT_MAX_RESP;

    /* Build request buffer with keep-alive (pooled = no proxy in v1) */
    size_t req_len = 0;
    char *req_buf = kl_client_build_request(alloc, method, &parsed,
                                   headers, num_headers, body, body_len,
                                   &req_len, 1, NULL);
    if (!req_buf)
        return NULL;

    /* Copy hostname for SNI + pool key */
    char host_buf[KL_CLIENT_HOSTNAME_MAX];
    if (parsed.host_len >= sizeof(host_buf)) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memcpy(host_buf, parsed.host, parsed.host_len);
    host_buf[parsed.host_len] = '\0';

    /* Allocate client */
    KlClient *c = kl_malloc(alloc, sizeof(KlClient));
    if (!c) {
        kl_free(alloc, req_buf, req_len);
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    c->fd = KL_INVALID_SOCKET;
    c->ev_ctx = ev_ctx;
    if (cfg && cfg->sockets) c->ev_ctx->sockets = cfg->sockets;  /* provider selection */
    /* PAL 8f-5a: on a completion loop, adopt the backend's native overlapped provider when
     * the configured one is incompatible — so a completion backend is a drop-in for the
     * client too (the event axis stays masked). */
    if (!kl_event_ctx_sockets_compatible(c->ev_ctx)) {
        const struct KlSocketProvider *np = kl_event_native_provider(&c->ev_ctx->loop);
        if (np) c->ev_ctx->sockets = np;
    }
    /* PAL Phase 7: the async client's readiness loop must be able to watch the
     * provider's handles (native fds). Reject an incoherent pairing up front. */
    if (!kl_event_ctx_sockets_compatible(c->ev_ctx)) {
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }
    c->alloc = alloc;
    c->tls_cfg = tls_cfg;
    c->request_buf = req_buf;
    c->request_len = req_len;
    c->request_sent = 0;
    c->on_done = on_done;
    c->user_data = user_data;
    memcpy(c->host_buf, host_buf, parsed.host_len + 1);

    /* Pool integration */
    c->pool = pool;
    c->pool_port = parsed.port;
    c->pool_is_tls = is_tls;

    /* Response decompression config */
    c->decompress_cfg = cfg ? cfg->decompress : NULL;

    /* Create response parser */
    c->parser = kl_response_parser_llhttp(max_resp, alloc);
    if (!c->parser) {
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    /* Try pool acquire */
    KlClientPoolConn pconn;
    memset(&pconn, 0, sizeof(pconn));
    pconn.fd = -1;
    int acq = kl_cpool_acquire(pool, host_buf, parsed.port, is_tls,
                                NULL, 0, &pconn);

    if (acq == 0) {
        /* Pool hit — skip connect + TLS, go straight to sending */
        c->fd = pconn.fd;
        c->tls = pconn.tls;
        c->pool_conn = pconn;
        c->state = KL_CLIENT_SENDING;

        if (kl_watcher_add(ev_ctx, c->fd, KL_EVENT_WRITE, async_on_event, c) != 0) {
            c->fd = KL_INVALID_SOCKET;
            c->tls = NULL;
            kl_cpool_discard(pool, &pconn);
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            c->parser->destroy(c->parser);
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        return c;
    }

    /* Pool miss — normal connect flow */
    int res_owned = 0;
    KlResolver *resolver = client_pick_resolver(cfg, ev_ctx, &res_owned);
    if (resolver) {
        c->resolver = resolver;
        c->owns_resolver = res_owned;
        c->resolve_host = c->host_buf;   /* persistent copy (set above); no dangling local */
        c->resolve_port = parsed.port;
        c->state = KL_CLIENT_RESOLVING;
        /* Drive resolve + Happy Eyeballs via KlConnectOp (6C), same as the non-streaming path:
         * kl_connect_op_start runs start_resolve (kicks resolver->resolve) synchronously; a
         * start failure retires cleanly + returns NULL, an inline completion advances the op. */
        kl_connect_op_init(&c->connect_op, &CLI_CONNECT_HOOKS, c);
        kl_connect_op_start(&c->connect_op);
        if (c->connect_start_failed) {
            if (res_owned)
                resolver->destroy(resolver);
            c->resolver = NULL;
            c->owns_resolver = 0;
            if (c->decomp_wrap)
                kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
            c->parser->destroy(c->parser);
            kl_free(alloc, req_buf, req_len);
            kl_free(alloc, c, sizeof(KlClient));
            return NULL;
        }
        return c;                 /* deferred (RESOLVING) or inline-completed — both return c */
    }

    /* Sync DNS fallback (blocking name resolution → KlSockAddr) */
    KlSockAddr addrs[KL_RESOLVE_MAX_ADDRS];
    int naddr = 0;
    if (kl_resolve_sync(host_buf, (uint16_t)parsed.port, SOCK_STREAM,
                        addrs, KL_RESOLVE_MAX_ADDRS, &naddr) != 0) {
        if (c->decomp_wrap)
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        c->parser->destroy(c->parser);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    if (start_connect(c, &addrs[0]) < 0) {
        if (c->decomp_wrap)
            kl_free(alloc, c->decomp_wrap, sizeof(DecompStreamWrap));
        c->parser->destroy(c->parser);
        kl_free(alloc, req_buf, req_len);
        kl_free(alloc, c, sizeof(KlClient));
        return NULL;
    }

    return c;
}
