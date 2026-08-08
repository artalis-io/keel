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
/* Happy Eyeballs helpers (defined below start_connect) */
static void he_proceed_after_connect(KlClient *c);
static void he_start_next(KlClient *c);
static void he_win(KlClient *c, KlSocketHandle fd);
static void he_on_writable(KlClient *c, KlSocketHandle fd);
static void he_close_attempts(KlClient *c, KlSocketHandle keep_fd);
static void he_cancel_timers(KlClient *c);
static void he_on_deadline(void *user_data);
static void he_on_delay(void *user_data);

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
        c->state = KL_HCLIENT_CONNECTING;   /* the connect completion advances us */
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
    c->state = (rc == 0) ? KL_HCLIENT_SENDING : KL_HCLIENT_CONNECTING;

    if (kl_watcher_add(c->ev_ctx, fd, KL_EVENT_WRITE, async_on_event, c) != 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        c->fd = KL_INVALID_SOCKET;
        return -1;
    }

    return 0;
}

/* ── Happy Eyeballs — racing connect over the resolved list (RFC 8305) ─── */

/* Cancel the Connection Attempt Delay + overall deadline timers (idempotent). */
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

/* Close every active racing attempt except keep_fd (the winner, or -1 for all). */
static void he_close_attempts(KlClient *c, KlSocketHandle keep_fd)
{
    for (int i = 0; i < c->conn_next; i++) {
        if (c->conn_attempts[i].active && c->conn_attempts[i].fd != keep_fd) {
            client_drop_connect_fd(c, c->conn_attempts[i].fd);
            c->conn_attempts[i].active = 0;
        }
    }
    c->conn_pending = 0;
}

/* A winner has connected: stop the stagger, drop the losers, adopt its fd, and
 * hand off to the shared post-connect path. The overall deadline stays armed to
 * bound the remaining TLS/send/recv. */
static void he_win(KlClient *c, KlSocketHandle fd)
{
    if (c->conn_delay_timer >= 0) {
        kl_timer_cancel(c->ev_ctx, c->conn_delay_timer);
        c->conn_delay_timer = -1;
    }
    /* Mark the winner inactive so he_close_attempts won't touch its fd. */
    for (int i = 0; i < c->conn_next; i++)
        if (c->conn_attempts[i].active && c->conn_attempts[i].fd == fd)
            c->conn_attempts[i].active = 0;
    he_close_attempts(c, fd);
    c->fd = fd;
    he_proceed_after_connect(c);
}

/* Arm the Connection Attempt Delay to stagger the next address, if any remain. */
static void he_arm_delay(KlClient *c)
{
    if (c->conn_delay_timer >= 0) {
        kl_timer_cancel(c->ev_ctx, c->conn_delay_timer);
        c->conn_delay_timer = -1;
    }
    if (c->state == KL_HCLIENT_CONNECTING && c->conn_next < c->conn_addrs.naddrs)
        c->conn_delay_timer = kl_timer_add(c->ev_ctx,
                                           (uint64_t)c->connect_delay_ms,
                                           he_on_delay, c);
}

/* Start a nonblocking connect to address index idx. Returns 0 if an attempt is
 * now in flight (or already won), -1 on a hard local failure (try the next). */
static int he_new_attempt(KlClient *c, int idx)
{
    const KlSockAddr *sa = &c->conn_addrs.addrs[idx];
    int fam = (kl_sockaddr_family(sa) == KL_AF_INET6) ? AF_INET6 : AF_INET;

    KlSocketHandle fd = kl_sock_socket(c->ev_ctx->sockets, fam, c->conn_addrs.ai_socktype, c->conn_addrs.ai_protocol);
    if (!kl_handle_valid(fd))
        return -1;

    kl_sock_set_nosigpipe(c->ev_ctx->sockets, fd);
    if (kl_sock_set_nonblocking(c->ev_ctx->sockets, fd) < 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    /* Completion loop: post the connect over the completion axis. Several client connect
     * ops can be outstanding at once (Happy Eyeballs races them natively); the first
     * KL_COMP_CONNECT that reports connected wins (he_on_connect_result → he_win), losers are
     * dropped by client_drop_connect_fd. No immediate-success shortcut — the completion always
     * arrives on a subsequent drain (loopback POLLOUT is ready at once). */
    if (client_loop_is_completion(c)) {
        if (client_comp_connect(c, fd, sa) != 0) {
            c->conn_last_err = KL_ERR_CONNECT;
            kl_sock_close(c->ev_ctx->sockets, fd);
            return -1;
        }
        c->conn_attempts[idx].fd = fd;
        c->conn_attempts[idx].active = 1;
        c->conn_pending++;
        return 0;
    }

    int rc = kl_sock_connect(c->ev_ctx->sockets, fd, sa);
    if (rc < 0 && kl_sock_io_status(c->ev_ctx->sockets) != KL_IO_PENDING) {
        c->conn_last_err = KL_ERR_CONNECT;
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    if (kl_watcher_add(c->ev_ctx, fd, KL_EVENT_WRITE, async_on_event, c) != 0) {
        kl_sock_close(c->ev_ctx->sockets, fd);
        return -1;
    }

    c->conn_attempts[idx].fd = fd;
    c->conn_attempts[idx].active = 1;
    c->conn_pending++;

    if (rc == 0)            /* connected immediately (e.g. loopback) */
        he_win(c, fd);
    return 0;
}

/* Advance the cursor and start the next viable address; skip hard local
 * failures without consuming the delay. Complete with an error when the list is
 * exhausted and nothing is pending. */
static void he_start_next(KlClient *c)
{
    while (c->state == KL_HCLIENT_CONNECTING &&
           c->conn_next < c->conn_addrs.naddrs) {
        int idx = c->conn_next++;
        if (he_new_attempt(c, idx) == 0) {
            he_arm_delay(c);   /* schedule the next staggered attempt */
            return;
        }
    }
    if (c->state == KL_HCLIENT_CONNECTING && c->conn_pending == 0) {
        c->error = (c->conn_last_err != KL_ERR_NONE) ? c->conn_last_err
                                                     : KL_ERR_CONNECT;
        async_complete_error(c);
    }
}

/* Connection Attempt Delay fired: start the next address if we're still racing. */
static void he_on_delay(void *user_data)
{
    KlClient *c = user_data;
    c->conn_delay_timer = -1;
    if (c->state == KL_HCLIENT_CONNECTING)
        he_start_next(c);
}

/* Overall request deadline fired: fail the whole request (covers connect racing
 * and the otherwise-untimed TLS handshake / send / recv). */
static void he_on_deadline(void *user_data)
{
    KlClient *c = user_data;
    c->deadline_timer = -1;
    if (c->state == KL_HCLIENT_DONE)
        return;
    he_close_attempts(c, KL_INVALID_SOCKET);    /* drop any racing sockets */
    c->error = KL_ERR_TIMEOUT;
    async_complete_error(c);
}

/* One racing attempt failed: drop it, then either fast-start the next address
 * (a failure must not wait out the delay, §5) or complete when all are gone. */
static void he_fail_attempt(KlClient *c, KlSocketHandle fd)
{
    for (int i = 0; i < c->conn_next; i++) {
        if (c->conn_attempts[i].active && c->conn_attempts[i].fd == fd) {
            client_drop_connect_fd(c, fd);
            c->conn_attempts[i].active = 0;
            c->conn_pending--;
            break;
        }
    }
    c->conn_last_err = KL_ERR_CONNECT;

    if (c->conn_next < c->conn_addrs.naddrs)
        he_start_next(c);
    else if (c->conn_pending == 0) {
        c->error = c->conn_last_err;
        async_complete_error(c);
    }
}

/* A racing socket became writable: SO_ERROR==0 wins the race, else it failed. */
static void he_on_writable(KlClient *c, KlSocketHandle fd)
{
    int err = 0;
    kl_sock_get_so_error(c->ev_ctx->sockets, fd, &err);
    if (err == 0)
        he_win(c, fd);
    else
        he_fail_attempt(c, fd);
}

/* Completion-loop connect result (LC-0). The backend delivered the connect outcome in the
 * watcher mask (KL_EVENT_WRITE = connected, else failed) rather than via SO_ERROR — io_uring
 * does not preserve SO_ERROR after a failed IORING_OP_CONNECT, so the client trusts the
 * delivered result. Mirrors he_on_writable's win/fail branch. */
static void he_on_connect_result(KlClient *c, KlSocketHandle fd, KlEventMask ready)
{
    if (ready & KL_EVENT_WRITE)
        he_win(c, fd);
    else
        he_fail_attempt(c, fd);
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
        c->error = KL_ERR_DNS;
        async_complete_error(c);
        return;
    }

    /* Copy the whole (family-interleaved, preferred-first) list and race it. */
    c->conn_addrs = *result;
    if (c->conn_addrs.naddrs > KL_RESOLVE_MAX_ADDRS)
        c->conn_addrs.naddrs = KL_RESOLVE_MAX_ADDRS;
    c->conn_next = 0;
    c->conn_pending = 0;
    c->conn_racing = 1;
    c->conn_last_err = KL_ERR_NONE;
    c->state = KL_HCLIENT_CONNECTING;

    he_arm_deadline(c);
    he_start_next(c);
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
        c->state = KL_HCLIENT_PROXY_CONNECTING;
        kl_watcher_mod(c->ev_ctx, c->fd, KL_EVENT_WRITE);
        return;
    }

    /* Proxy: HTTP target — request already has absolute-form URL, send directly */
    if (c->is_proxied) {
        c->state = KL_HCLIENT_SENDING;
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

        c->state = KL_HCLIENT_TLS_HANDSHAKE;
        async_handle_tls_handshake(c);
    } else {
        c->state = KL_HCLIENT_SENDING;
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

    c->state = KL_HCLIENT_PROXY_HANDSHAKE;
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

        c->state = KL_HCLIENT_TLS_HANDSHAKE;
        async_handle_tls_handshake(c);
        return;
    }
}

/* ── State: TLS_HANDSHAKE ────────────────────────────────────────── */

static void async_handle_tls_handshake(KlClient *c)
{
    KlTlsResult r = c->tls->handshake(c->tls, c->fd);
    if (r == KL_TLS_OK) {
        c->state = KL_HCLIENT_SENDING;
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
        c->state = KL_HCLIENT_SENDING_STREAM;
        c->chunk_phase = 0;
        async_handle_sending_stream(c);
        return;
    }

    c->state = KL_HCLIENT_RECEIVING;
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
            c->state = KL_HCLIENT_RECEIVING;
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
    case KL_HCLIENT_RESOLVING:
        break;  /* DNS resolution handled by resolver callback, not watcher */
    case KL_HCLIENT_CONNECTING:
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
    case KL_HCLIENT_PROXY_CONNECTING:
        async_handle_proxy_connecting(c);
        break;
    case KL_HCLIENT_PROXY_HANDSHAKE:
        async_handle_proxy_handshake(c);
        break;
    case KL_HCLIENT_TLS_HANDSHAKE:
        async_handle_tls_handshake(c);
        break;
    case KL_HCLIENT_SENDING:
        async_handle_sending(c);
        break;
    case KL_HCLIENT_SENDING_STREAM:
        async_handle_sending_stream(c);
        break;
    case KL_HCLIENT_RECEIVING:
        async_handle_receiving(c);
        break;
    case KL_HCLIENT_DONE:
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

    c->state = KL_HCLIENT_DONE;
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

    c->state = KL_HCLIENT_DONE;
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

    /* DNS resolution — resolve proxy host when proxied, target host otherwise */
    const char *resolve_host = is_proxied ? proxy->host : host_buf;
    int resolve_port = is_proxied ? proxy->port : parsed.port;

    /* Async DNS resolver path (explicit, built-in default, or sync name resolution). */
    int res_owned = 0;
    KlResolver *resolver = client_pick_resolver(cfg, ev_ctx, &res_owned);
    if (resolver) {
        c->resolver = resolver;
        c->owns_resolver = res_owned;
        c->state = KL_HCLIENT_RESOLVING;
        KlResolveReq *rq = resolver->resolve(resolver, ev_ctx,
                                             resolve_host, resolve_port,
                                             dns_resolved, c);
        /* The resolver contract permits synchronous completion: dns_resolved
         * may have already run inside resolve() — firing on_done (DONE) or
         * advancing the connect (CONNECTING/…) — and taken ownership of c. In
         * that case the caller owns c (and frees it via kl_client_free); a NULL
         * return is NOT a start failure and must not free c out from under
         * on_done. Only when we are still RESOLVING did resolve() truly defer. */
        if (c->state != KL_HCLIENT_RESOLVING)
            return c;                 /* rq discarded; resolve_req cleared by dns_resolved */
        c->resolve_req = rq;
        if (!c->resolve_req) {
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

    /* Cancel in-flight DNS resolution */
    if (client->resolve_req && client->resolver) {
        client->resolver->cancel(client->resolve_req);
        client->resolve_req = NULL;
    }

    /* Cancel Happy Eyeballs racing (timers + any in-flight attempt sockets). */
    he_cancel_timers(client);
    he_close_attempts(client, KL_INVALID_SOCKET);

    if (kl_handle_valid(client->fd)) {
        /* Single-fd completion connect still in flight: drop its pending connect op before the
         * watcher/fd go away (HE attempts were already dropped by he_close_attempts above). */
        if (client->state == KL_HCLIENT_CONNECTING && client_loop_is_completion(client))
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

    client->state = KL_HCLIENT_DONE;
    if (client->error == KL_ERR_NONE)
        client->error = KL_ERR_IO;
}

void kl_client_free(KlClient *client)
{
    if (!client)
        return;

    /* Cancel if still in-flight (cancels any pending resolve_req first). */
    if (client->state != KL_HCLIENT_DONE)
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
        c->state = KL_HCLIENT_SENDING;

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
        c->state = KL_HCLIENT_RESOLVING;
        c->resolve_req = resolver->resolve(resolver, ev_ctx,
                                            host_buf, parsed.port,
                                            dns_resolved, c);
        if (!c->resolve_req) {
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
        return c;
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
