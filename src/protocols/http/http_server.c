#include <keel/clock.h>
#include <keel/http_server.h>
#include <keel/async.h>
#include <keel/timer.h>
#include <keel/tls.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include "http_internal.h"
#include "kl_atomic.h"    /* lock-free int atomics for the running/draining flags */
#include "socket.h"       /* seam: kl_sock_* + KlSockAddr (no direct sockaddr) */
#include "event_caps.h"   /* event↔socket capability negotiation */
#include "completion_io.h"    /* completion-loop tick dispatch (IOCP) */
#include "completion_http.h"  /* kl_http_comp_accept_setup: a completion loop's accept path */
#include "http_server_plat.h"  /* AF_UNIX bind, peer creds, signals: per-platform, no #ifdef here */
#include "platform.h"     /* KlPlatWakeup: self-pipe so kl_http_server_stop wakes the run loop */
/* No websocket_server.h / http2_server.h / http2_internal.h: the readiness data plane now
 * dispatches ws/h2 (and PROXY) through http_proto_hooks.h (the same seam the completion
 * driver uses) so this TU names no optional-protocol type or symbol. */
#include "http_proto_hooks.h"  /* ws/h2/proxy readiness + upgrade seam */

#define KL_LISTEN_BACKLOG  128
#define KL_EVENTS_PER_TICK 64
#define KL_POLL_TIMEOUT_MS 1000


void kl_http_server_log(KlHttpServer *s, int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s->config.log_fn) {
        s->config.log_fn(level, fmt, ap, s->config.log_user_data);
    } else {
        fprintf(stderr, "keel: ");
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

void kl_http_server_log_errno(KlHttpServer *s, int level, const char *msg) {
    kl_http_server_log(s, level, "%s: %s", msg, strerror(errno));
}

static int kl_http_server_bind_tcp(KlHttpServer *s, int *port) {
    /* Parse the numeric bind address (IPv4/IPv6 literal); no DNS. */
    KlSockAddr bind_sa;
    if (kl_sockaddr_parse(&bind_sa, s->config.bind_addr, (uint16_t)s->config.port) != 0) {
        kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "invalid bind address '%s'", s->config.bind_addr);
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    int family = (kl_sockaddr_family(&bind_sa) == KL_AF_INET6) ? AF_INET6 : AF_INET;

    s->listen_fd = kl_sock_socket(s->ev.sockets, family, SOCK_STREAM, 0);
    if (!kl_handle_valid(s->listen_fd)) {
        kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "socket");
        s->last_error = KL_ERR_SOCKET;
        return -1;
    }
    kl_sock_set_cloexec(s->ev.sockets, s->listen_fd);

    (void)kl_sock_set_reuseaddr(s->ev.sockets, s->listen_fd, 1);
    (void)kl_sock_set_reuseport(s->ev.sockets, s->listen_fd, 1);   /* best-effort */

    /* IPv6 dual-stack: accept both IPv4 and IPv6 on :: */
    if (family == AF_INET6)
        (void)kl_sock_set_ipv6only(s->ev.sockets, s->listen_fd, 0);

    if (kl_sock_bind(s->ev.sockets, s->listen_fd, &bind_sa) < 0) {
        kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "bind");
        s->last_error = KL_ERR_BIND;
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
        return -1;
    }

    /* Retrieve OS-assigned port (useful when config.port == 0) */
    {
        KlSockAddr la;
        if (kl_sock_get_local_addr(s->ev.sockets, s->listen_fd, &la) == 0)
            *port = kl_sockaddr_port(&la);
    }

    return 0;
}

/*
 * Adopt a pre-bound, already-listening fd (socket activation). The fd's
 * family determines the transport, so TCP_NODELAY and unlink policy stay
 * correct. KEEL never owns (never unlinks) an adopted UNIX socket.
 */
static int kl_http_server_adopt_fd(KlHttpServer *s, int *port) {
    s->listen_fd = s->config.listen_fd;

    KlSockAddr la;
    if (kl_sock_get_local_addr(s->ev.sockets, s->listen_fd, &la) != 0) {
        kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "getsockname on adopted fd");
        s->last_error = KL_ERR_SOCKET;
        s->listen_fd = KL_INVALID_SOCKET;
        return -1;
    }

    if (kl_sockaddr_family(&la) == KL_AF_UNIX) {
        s->config.transport = KL_HTTP_SERVER_TRANSPORT_UNIX;
        *port = 0;
    } else {
        s->config.transport = KL_HTTP_SERVER_TRANSPORT_TCP;
        *port = kl_sockaddr_port(&la);
    }
    /* Adopted fd is never unlinked; the supervisor owns the socket path. */
    s->unix_socket_owned = 0;
    /* CLOEXEC so the inherited listener doesn't re-leak to our own children. */
    kl_sock_set_cloexec(s->ev.sockets, s->listen_fd);
    return 0;
}

/* Binds (or adopts) the listener and reports its port in *port (0 for a UNIX socket). The port is
 * not stored on the server here: kl_http_server_run publishes it once the socket is listening. */
static int kl_http_server_bind_listener(KlHttpServer *s, int *port) {
    *port = 0;
    if (s->config.listen_fd > 0)
        return kl_http_server_adopt_fd(s, port);
    if (s->config.transport == KL_HTTP_SERVER_TRANSPORT_UNIX)
        return kl_http_server_plat_bind_unix(s);
    return kl_http_server_bind_tcp(s, port);
}

/* Fail closed off the built-in provider: the handle of an lwIP, UEFI or injected transport is not a
 * host descriptor, and querying it as one could report an unrelated host socket's identity. */
int kl_http_conn_peer_cred(const KlHttpConn *c, KlPeerCred *out) {
    if (!c || !kl_http_server_plat_peer_cred_readable(conn_provider(c)))
        return -1;
    return kl_peer_cred_fd(c->stream.fd, out);
}

int kl_http_request_peer_cred(const KlHttpRequest *req, KlPeerCred *out) {
    if (!req)
        return -1;
    return kl_http_conn_peer_cred(kl_http_request_conn(req), out);
}

int kl_http_request_peer_label(const KlHttpRequest *req, char *buf, size_t buflen) {
    if (!req || !buf || buflen == 0)
        return -1;
    const KlHttpConn *conn = kl_http_request_conn(req);
    if (!conn || !kl_handle_valid(conn->stream.fd) ||
        !kl_http_server_plat_peer_cred_readable(conn_provider(conn)))
        return -1;

    return kl_http_server_plat_peer_label_fd(conn->stream.fd, buf, buflen);
}

const KlSockAddr *kl_http_request_peer_sockaddr(const KlHttpRequest *req) {
    if (!req)
        return NULL;
    const KlHttpConn *conn = kl_http_request_conn(req);
    if (!conn || kl_sockaddr_family(&conn->stream.peer_addr) == KL_AF_UNSPEC)
        return NULL;
    return &conn->stream.peer_addr;
}

int kl_http_request_peer_addr(const KlHttpRequest *req, char *ip, size_t iplen,
                         uint16_t *port) {
    if (!req || !ip || iplen == 0)
        return -1;
    const KlHttpConn *conn = kl_http_request_conn(req);
    if (!conn)
        return -1;

    const KlSockAddr *a = &conn->stream.peer_addr;
    KlAddrFamily fam = kl_sockaddr_family(a);
    if (fam != KL_AF_INET && fam != KL_AF_INET6)
        return -1;  /* UNSPEC / AF_UNIX: no IP address (use peer credentials) */
    if (kl_sockaddr_format_ip(a, ip, iplen) < 0)
        return -1;
    if (port)
        *port = kl_sockaddr_port(a);
    return 0;
}

int kl_http_request_peer_cert(const KlHttpRequest *req, KlPeerCert *out) {
    if (!req || !out)
        return -1;
    const KlHttpConn *conn = kl_http_request_conn(req);
    if (!conn || !conn->tls || !conn->tls->peer_cert)
        return -1;   /* plaintext connection, or backend lacks mTLS support */
    memset(out, 0, sizeof(*out));
    return conn->tls->peer_cert(conn->tls, out);
}

/* Socket-activation (systemd LISTEN_* fd inheritance) lives in http_server_activation.c:
 * a self-contained responsibility, separate from the readiness loop,
 * TCP listener construction, and peer accessors. */

/* Non-static so the freestanding kl_http_server_free (http_server_core.c) can call it on the
 * hosted path (it needs the AF_UNIX unlink); declared in internal.h. */
void kl_http_server_close_listener(KlHttpServer *s) {
    if (kl_handle_valid(s->listen_fd)) {
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
    }
    kl_http_server_plat_unlink_owned_unix(s);   /* POSIX: lstat+S_ISSOCK+unlink; Win: DeleteFile */
}

/* kl_http_server_conn_release lives in the freestanding-safe server core (http_server_core.c):
 * the completion sweeps there call it, so the archive needs it in-core.
 *
 * kl_http_server_init (+ its stop-wakeup self-pipe helpers) also live in http_server_core.c, so
 * a freestanding EFI server constructs its
 * KlHttpServer from the archive alone. The hosted-only pieces (ws/h2/proxy hook installers,
 * PROXY CIDR allowlist, async file I/O, self-pipe) are #ifndef KEEL_FREESTANDING there. */


/* Route + middleware registration (the kl_http_server_route / kl_http_server_use family) and
 * the read-side body flow control (kl_http_request_pause_body / resume_body) plus
 * kl_http_server_stats live in the freestanding-safe server core (http_server_core.c).
 * kl_http_server_ws_upgrade lives in http_server_ws.c: the WebSocket type +
 * registration belong to the ws module, so this readiness TU owns no protocol type. */


/* ── Readiness accept adapter: drive KlListener over the listen fd + pool credits ──
 * The listener owns the accept lifecycle (reserve-before-accept backpressure, pause/resume,
 * confirmed retirement); these hooks bridge it to the event loop and the split-credit pool. Only
 * installed for a readiness loop (!completion_loop); the completion path drives its own accept
 * adapter over the same listener. */
static int server_accept_reserve(void *ctx) {
    KlHttpServer *s = ctx;
    return kl_http_conn_pool_reserve(&s->pool);
}
static void server_accept_release(void *ctx) {
    KlHttpServer *s = ctx;
    kl_http_conn_pool_return_credit(&s->pool);               /* free_credits++ ... */
    kl_listener_notify_slot_free(&s->accept_listener);  /* ...then resume the accept if paused */
}
static int server_accept_arm(void *ctx) {
    KlHttpServer *s = ctx;
    if (s->listen_registered) return 0;                 /* already armed (level-triggered) */
    if (kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL) < 0) return -1;
    s->listen_registered = 1;
    return 0;
}
static void server_accept_disarm(void *ctx) {
    KlHttpServer *s = ctx;
    if (s->listen_registered) {
        kl_event_del(&s->ev.loop, s->listen_fd);
        s->listen_registered = 0;
    }
}
/* The accept back-off ended: restore the listen interest the exhaustion path dropped, unless the
 * listener has since paused (it re-arms on its own when a slot frees) or closed. */
static void server_accept_backoff_check(KlHttpServer *s) {
    if (!s->accept_backoff_until || kl_monotonic_ms() < s->accept_backoff_until) return;
    s->accept_backoff_until = 0;
    if (kl_listener_state(&s->accept_listener) != KL_LISTENER_STATE_LISTENING) return;
    if (server_accept_arm(s) < 0)
        kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "accept: cannot re-arm the listen socket");
}
static void server_accept_dispose(void *ctx, KlSocketHandle fd) {
    kl_sock_close(((KlHttpServer *)ctx)->ev.sockets, fd);   /* no local var: read-only, keep ctx void* */
}
static void server_accept_on_accept(void *ctx, KlSocketHandle fd, KlSlotLease lease) {
    KlHttpServer *s = ctx;
    KlHttpConn *nc = kl_http_conn_acquire(&s->pool, fd);         /* commit; credit already reserved */
    if (!nc) {
        /* Invariant failure (credit-backed accept but no free KlHttpConn): dispose the descriptor and
         * consume the lease to return the credit. free_credits+reserved+active==capacity guarantees
         * this cannot happen, but never leak an fd or a credit if it somehow does. */
        kl_sock_close(s->ev.sockets, fd);
        kl_slot_lease_release(&lease);
        return;
    }
    nc->slot_lease = lease;                             /* consumed once on close (after conn return) */
    nc->peer_source = KL_HTTP_PEER_SOCKET;
    nc->stream.peer_addr = s->accept_pending_peer;
    nc->res.alloc = &s->alloc_storage;
    if (s->config.tls) {                               /* TLS: enter handshake instead of reading */
        nc->state = KL_HTTP_CONN_TLS_HANDSHAKE;
        nc->tls_want = KL_EVENT_READ;
    }
    /* PROXY protocol from a trusted source: read the header first (before TLS/HTTP). */
    const KlProxyHooks *ph = kl_proxy_hooks();
    if (s->proxy_cidr_count > 0 && ph && ph->cidr_match &&
        kl_sockaddr_family(&s->accept_pending_peer) != KL_AF_UNSPEC &&
        ph->cidr_match(s->proxy_cidrs, s->proxy_cidr_count, &s->accept_pending_peer)) {
        nc->state = KL_HTTP_CONN_PROXY_HEADER;
    }
    if (kl_event_add(&s->ev.loop, fd, KL_EVENT_READ, &nc->stream) < 0)  /* identity = raw stream */
        kl_http_server_conn_release(s, nc);                 /* releases the conn AND consumes the lease */
}

#ifdef KEEL_HTTP_SERVER_TEST_HOOKS
/* Test-only (a -DKEEL_HTTP_SERVER_TEST_HOOKS copy linked ahead of libkeel.a): nonzero makes the
 * readiness accept path fail to start, the one start-up failure after the signal handlers are
 * installed. */
int kl_http_server_test_fail_listener_start = 0;
#endif

static int server_accept_listener_start(KlHttpServer *s) {
#ifdef KEEL_HTTP_SERVER_TEST_HOOKS
    if (kl_http_server_test_fail_listener_start) return -1;
#endif
    s->accept_alive = 1;                               /* liveness token live before any lease issues */
    KlListenerHooks h = {
        .reserve = server_accept_reserve, .release = server_accept_release, .credit_ctx = s,
        .liveness = &s->accept_alive,
        .arm_accept = server_accept_arm, .disarm_accept = server_accept_disarm,
        .on_accept = server_accept_on_accept, .dispose_fd = server_accept_dispose,
    };
    if (kl_listener_init(&s->accept_listener, /*completion_mode=*/0, &h, s) < 0) return -1;
    return kl_listener_start(&s->accept_listener);
}

/* A start-up that fails clears running again: it was set at entry (below), and a server that never
 * started must not read as running. */
static int server_start_failed(KlHttpServer *s) {
    kl_atomic_store_int(&s->running, 0);
    return -1;
}

int kl_http_server_run(KlHttpServer *s) {
    KlAllocator *alloc = &s->alloc_storage;
    (void)alloc;   /* the readiness accept path now configures conns via the listener adapter */

    /* Run once per init. A drain closed the listener (an adopted listen_fd too), so re-adopting
     * config.listen_fd could listen on, register and later close an unrelated socket now holding that
     * number; a rerun after a plain stop would leak the first listener. Refused before any state or
     * descriptor is touched, so a running first run is undisturbed. */
    if (s->run_started) {
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    s->run_started = 1;

    /* Mark the server running BEFORE anything another thread can observe (bound_port, the
     * "listening" log). A kl_http_server_stop issued once a caller has seen the server start (from
     * another thread, or the SIGTERM handler) then clears it, and the loop below sees the stop.
     * Setting it after the bind, as this did, overwrote such a stop and the server ran on. */
    kl_atomic_store_int(&s->running, 1);
    kl_atomic_store_int(&s->draining, 0);
    s->drain_listener_closed = 0;

    /* A stop that came before the store above (a signal during startup) would be overwritten by it:
     * honour it here, before anything listens. A stop after the store clears `running` or starts a
     * drain as usual. Both are seq-cst, so one of the two always sees the stop. */
    if (kl_atomic_load_int(&s->stop_requested)) {
        kl_atomic_store_int(&s->running, 0);
        kl_atomic_store_int(&s->draining, 0);
        return 0;
    }

    int port = 0;
    if (kl_http_server_bind_listener(s, &port) < 0)
        return server_start_failed(s);

    /* An adopted fd (socket activation) is already listening; don't re-listen. */
    if (s->config.listen_fd <= 0 && kl_sock_listen(s->ev.sockets, s->listen_fd, KL_LISTEN_BACKLOG) < 0) {
        kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "listen");
        s->last_error = KL_ERR_LISTEN;
        kl_http_server_close_listener(s);
        return server_start_failed(s);
    }

    if (kl_sock_set_nonblocking(s->ev.sockets, s->listen_fd) < 0) {
        kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "fcntl");
        s->last_error = KL_ERR_SOCKET;
        kl_http_server_close_listener(s);
        return server_start_failed(s);
    }

    /* Register listen socket for read events */
    if (kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL) < 0) {
        kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "event_add listen");
        s->last_error = KL_ERR_EVENT_ADD;
        kl_http_server_close_listener(s);
        return server_start_failed(s);
    }
    s->listen_registered = 1;   /* the readiness listener's arm hook treats this as already armed */

    if (s->config.transport == KL_HTTP_SERVER_TRANSPORT_UNIX) {
        /* An adopted fd (socket activation) has no path we own. */
        kl_http_server_log(s, KL_HTTP_SERVER_LOG_INFO, "listening on unix:%s",
               s->config.unix_socket_path ? s->config.unix_socket_path
                                          : "(inherited fd)");
    } else if (s->config.listen_fd > 0) {
        kl_http_server_log(s, KL_HTTP_SERVER_LOG_INFO, "listening on inherited fd %d (port %d)",
               (int)s->listen_fd, port);
    } else {
        kl_http_server_log(s, KL_HTTP_SERVER_LOG_INFO, "listening on %s:%d",
               s->config.bind_addr, port);
    }

    /* Ignore SIGPIPE + install SIGTERM/SIGINT graceful-stop handlers (POSIX) or
     * a console Ctrl handler (Windows). Done here (past the setup early-returns)
     * so a failed bind never leaves handlers installed without a restore. */
    /* This is where the signal-safe path is ENABLED, so it is also where the invariant has to hold.
     * kl_http_server_init() already refused any platform that fails the check; re-stating it at the
     * point the handler goes live keeps the ordering visible to a reader, and costs one constant-folded
     * comparison on every toolchain that promises lock-freedom at compile time. */
    if (!kl_atomic_int_is_lock_free(&s->running)) {
        s->last_error = KL_ERR_UNSUPPORTED;
        kl_http_server_close_listener(s);
        return server_start_failed(s);
    }
    kl_http_server_plat_signals_install(s);

    /* running / draining were set at entry, before the server could be seen to start. */
    KlEvent events[KL_EVENTS_PER_TICK];
    /* The release generation (comp_gen) of each connection event's KlHttpConn, taken when the batch
     * was captured. One batch can carry several events for one connection (kqueue reports READ and
     * WRITE as two), and handling one may release the connection, after which the slot can be
     * acquired again by an accept in the same batch. An event whose connection's generation moved
     * belongs to a connection that no longer exists, so it is dropped instead of acting on the slot. */
    unsigned event_gen[KL_EVENTS_PER_TICK];

    /* A completion loop (IOCP) is driven by the completion tick, not the readiness
     * wait/dispatch below. Detected once from the backend's advertised event model
     * (completion seam); the completion concept never enters the
     * readiness path. Never true on readiness backends, so that path is unchanged. */
    const int completion_loop =
        (kl_event_caps(&s->ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;

    /* Readiness loops drive the accept path through the embedded KlListener; the
     * completion path drives its own accept adapter over the same listener. */
    if (!completion_loop) {
        if (server_accept_listener_start(s) < 0) {
            s->last_error = KL_ERR_EVENT_ADD;
            kl_http_server_close_listener(s);
            /* The handlers installed above point at this server: a run that never started must not
             * leave them behind, or a later SIGTERM stops a server the caller may have freed. */
            kl_http_server_plat_signals_restore(s);
            return server_start_failed(s);
        }
        s->accept_via_listener = 1;
    }

    int run_failed = 0;                 /* the loop stopped on an error, not on a stop */

    /* A completion loop sets up its accept path (prime the accepts, start the accept listener) here,
     * before the port is published, rather than in its first tick. A failure takes the same exit a
     * failed first tick did. */
    if (completion_loop && kl_http_comp_accept_setup(s) < 0)
        run_failed = 1;

    /* Startup has succeeded: the socket listens and is registered with the loop, its accept path is
     * set up, and no setup step is left that can fail. Publish the port only now, with an atomic
     * store, so a nonzero port is never seen for a run that fails to start; another thread may poll
     * kl_http_server_bound_port (an atomic load) until it is nonzero. */
    if (!run_failed)
        kl_atomic_store_int(&s->bound_port, port);

    while (!run_failed && kl_atomic_load_int(&s->running)) {
        if (completion_loop) {
            /* One completion-loop tick: factored into the freestanding-safe server
             * core (http_server_core.c) so a freestanding EFI server shares it verbatim. */
            if (kl_http_server_run_completion_loop(s) < 0) {
                run_failed = 1;
                break;
            }
            continue;
        }
        /* Last thing before the wait: everything that can write ran in the previous tick (dispatch,
         * sweep, deadlines, timers), so a suspended stream whose output is waiting is now watched for
         * writability, and one whose output went is not. HTTP/2 output written outside its
         * connection's events is flushed first, since a producer it notifies may write. */
        kl_http_server_settle_h2(s);
        kl_async_sync_stream_interest(s);
        const int h2_due = kl_http_server_settle_h2_due(s);

        /* Compute dynamic timeout based on nearest async op deadline */
        server_accept_backoff_check(s);
        uint64_t now = kl_monotonic_ms();
        int wait_timeout = KL_POLL_TIMEOUT_MS;
        if (s->accept_backoff_until > now && s->accept_backoff_until - now < (uint64_t)wait_timeout)
            wait_timeout = (int)(s->accept_backoff_until - now);   /* wake to restore accepting */
        for (const KlAsyncOp *aop = s->async_ops; aop; aop = aop->next) {
            if (aop->deadline_ms > 0) {
                if (now >= aop->deadline_ms) {
                    wait_timeout = 0;
                    break;
                }
                uint64_t rem = aop->deadline_ms - now;
                if (rem < (uint64_t)wait_timeout)
                    wait_timeout = (int)rem;
            }
        }

        if (h2_due) wait_timeout = 0;
        wait_timeout = kl_timer_next_timeout(&s->ev, wait_timeout);
        wait_timeout = kl_http_server_drain_wait(s, now, wait_timeout);

        /* Open the batch bracket BEFORE kl_event_wait and hold it through the file-I/O
         * completion callbacks AND watcher/connection dispatch. Those file-I/O callbacks run while a
         * watcher-event batch is already captured and can delete/reallocate watchers, so the bracket
         * must already be open (else the ABA window reopens before dispatch). begin → wait →
         * file-I/O → dispatch → end on EVERY exit (timeout, error, EINTR, normal iteration). If begin
         * fails (max nesting, an unbalanced-bracket bug), fail-stop the run loop. */
        if (kl_event_ctx_dispatch_begin(&s->ev) < 0) {
            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR, "dispatch nesting overflow");
            run_failed = 1;
            break;
        }

        int n = kl_event_wait(&s->ev.loop, events, KL_EVENTS_PER_TICK,
                              wait_timeout);
        if (n < 0) {
            kl_event_ctx_dispatch_end(&s->ev);
            if (errno == EINTR) continue;
            kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "event_wait");
            run_failed = 1;
            break;
        }

        for (int i = 0; i < n; i++) {
            uintptr_t u = (uintptr_t)events[i].udata;
            event_gen[i] = (u && !(u & 1))   /* a connection's stream: not the listener, not a watcher */
                               ? kl_http_conn_from_stream((KlStream *)events[i].udata)->comp_gen : 0;
        }

        /* Dispatch async file I/O completions */
        if (s->file_io) {
            KlFileIOResult fio_results[KL_EVENTS_PER_TICK];
            int nf = s->file_io->tick(s->file_io, fio_results,
                                       KL_EVENTS_PER_TICK);
            for (int fi = 0; fi < nf; fi++) {
                KlHttpConn *fc = fio_results[fi].udata;
                KlHttpConnState fstate = kl_http_conn_on_file_complete(
                    fc, fio_results[fi].result, fio_results[fi].zero_copy);
                if (fstate == KL_HTTP_CONN_SENDING) {
                    if (fc->file_io_phase == 1) {
                        /* FILE_IO_READING: async read pending, no WRITE reg */
                    } else {
                        kl_event_mod(&s->ev.loop, fc->stream.fd,
                                     KL_EVENT_WRITE, &fc->stream);
                    }
                } else if (fstate == KL_HTTP_CONN_READING) {
                    kl_event_mod(&s->ev.loop, fc->stream.fd, KL_EVENT_READ, &fc->stream);
                } else if (fstate == KL_HTTP_CONN_DRAINING) {
                    kl_event_mod(&s->ev.loop, fc->stream.fd, KL_EVENT_READ, &fc->stream);
                } else if (fstate == KL_HTTP_CONN_CLOSED) {
                    kl_event_del(&s->ev.loop, fc->stream.fd);
                    kl_http_server_conn_release(s, fc);
                }
            }
        }

        /* Batch dispatch runs inside the bracket opened before kl_event_wait above. */
        for (int i = 0; i < n; i++) {
            /* Watcher dispatch (tagged pointer, LSB=1) */
            if (kl_event_dispatch(&s->ev, &events[i]))
                continue;

            /* Readiness event identity is the raw KlStream; recover the owning KlHttpConn
             * only here, at the HTTP adapter boundary. NULL udata = the listen socket. */
            KlStream *evs = (KlStream *)events[i].udata;
            KlHttpConn *c = evs ? kl_http_conn_from_stream(evs) : NULL;

            if (c == NULL) {
                /* Listen socket: accept new connections through the readiness KlListener.
                 * It holds one reserved pool credit while LISTENING; each accepted
                 * fd is committed + wired by server_accept_on_accept; when the pool credit is
                 * exhausted it transitions to PAUSED (disarming the listen fd, so the kernel TCP
                 * backlog queues the rest) and resumes when a slot frees. */
                if (kl_atomic_load_int(&s->draining)) goto rearm_listen;
                while (kl_listener_state(&s->accept_listener) == KL_LISTENER_STATE_LISTENING) {
                    KlSockAddr peer;
                    KlSocketHandle client_fd = kl_sock_accept(s->ev.sockets, s->listen_fd,
                                                              &peer);
                    if (!kl_handle_valid(client_fd)) {
                        /* Classified by the socket provider, not a hosted errno: a provider that
                         * reports by status alone is read correctly. */
                        KlIoStatus ast = kl_sock_io_status(s->ev.sockets);
                        if (ast == KL_IO_WOULD_BLOCK) break;
                        /* Out of descriptors or memory: the level-triggered listen socket would
                         * wake every tick and fail again (a busy loop). Drop its interest and
                         * restore it once a short delay has passed (server_accept_backoff_check). */
                        if (ast == KL_IO_RESOURCE_EXHAUSTED) {
                            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                                               "accept: out of descriptors or memory, backing off");
                            s->accept_backoff_until = kl_monotonic_ms() + KL_HTTP_ACCEPT_RETRY_MS;
                            server_accept_disarm(s);
                        } else if (!s->ev.sockets) {
                            /* The default hosted provider reports through errno too. */
                            kl_http_server_log_errno(s, KL_HTTP_SERVER_LOG_ERROR, "accept");
                        } else {
                            /* A custom provider may report through its status alone: errno
                             * would be stale. */
                            kl_http_server_log(s, KL_HTTP_SERVER_LOG_ERROR,
                                               "accept: failed (I/O status %d)", (int)ast);
                        }
                        break;
                    }

                    /* The default provider's accept (kl_sockdef_accept) already returns a
                     * non-blocking, close-on-exec socket (accept4 on Linux/BSD); skip the two
                     * redundant per-connection syscalls on the accept hot path. A custom
                     * provider's accept op may not fold them, so apply them for a non-default
                     * (non-NULL) provider. */
                    if (s->ev.sockets) {
                        if (kl_sock_set_nonblocking(s->ev.sockets, client_fd) < 0) {
                            kl_sock_close(s->ev.sockets, client_fd);
                            continue;
                        }
                        /* Don't leak client connections into child processes. */
                        kl_sock_set_cloexec(s->ev.sockets, client_fd);
                    }
                    if (s->config.transport == KL_HTTP_SERVER_TRANSPORT_TCP)
                        (void)kl_sock_set_tcp_nodelay(s->ev.sockets, client_fd, 1);

                    /* Hand the accepted fd to the listener: it commits the reserved credit to a
                     * KlHttpConn (server_accept_on_accept), then reserves + arms the next accept, or
                     * PAUSEs if the pool is full, ending this drain. The peer is stashed for the
                     * hook (single-threaded, consumed immediately). */
                    s->accept_pending_peer = peer;
                    kl_listener_on_accepted(&s->accept_listener, client_fd);
                }
                /* Re-arm listen socket (no-op for persistent backends; required for io_uring's
                 * one-shot POLL_ADD) unless the listener paused (which already disarmed it). */
rearm_listen:
                if (s->listen_registered &&
                    kl_listener_state(&s->accept_listener) != KL_LISTENER_STATE_PAUSED)
                    kl_event_mod(&s->ev.loop, s->listen_fd,
                                 KL_EVENT_READ, NULL);
                continue;
            }

            /* Client connection event, unless an earlier event in this batch released its
             * connection (the slot may since hold a new one). */
            if (c->comp_gen != event_gen[i]) continue;
            KlHttpConnState new_state = c->state;

            /* PROXY protocol header: read before TLS/HTTP */
            if (c->state == KL_HTTP_CONN_PROXY_HEADER) {
                int pr = kl_http_conn_read_proxy_header(c);
                if (pr < 0) {
                    new_state = KL_HTTP_CONN_CLOSED;
                    goto transition;
                }
                if (pr == 0) {
                    new_state = KL_HTTP_CONN_PROXY_HEADER;   /* need more bytes */
                    goto transition;
                }
                /* Done: advance to the real initial state. */
                if (s->config.tls) {
                    c->tls_want = KL_EVENT_READ;
                    c->state = KL_HTTP_CONN_TLS_HANDSHAKE;
                } else {
                    c->state = KL_HTTP_CONN_READING;
                }
                new_state = c->state;
                /* Process any bytes already buffered after the header this
                 * tick; if none are pending, wait for the next event (edge-
                 * triggered backends don't re-deliver the consumed readiness,
                 * but newly-arriving bytes always trigger). */
                uint8_t probe;
                if (kl_sock_recv_peek(s->ev.sockets, c->stream.fd, &probe, 1) <= 0)
                    goto transition;
            }

            /* TLS handshake: handle before normal read/write */
            if (c->state == KL_HTTP_CONN_TLS_HANDSHAKE) {
                new_state = kl_http_conn_on_handshake(c);
                goto transition;
            }

            /* WebSocket: handle read/write events through the ws seam (symmetric with
             * the completion path's KlWsCompHooks; the conn only reached this state via a
             * ws upgrade, so the hooks are installed). */
            if (c->state == KL_HTTP_CONN_WEBSOCKET) {
                const KlWsServerHooks *wsh = kl_ws_server_hooks();
                if (!wsh) { new_state = KL_HTTP_CONN_CLOSED; goto transition; }
                if (events[i].ready & KL_EVENT_READ)
                    new_state = (KlHttpConnState)wsh->on_readable(c);
                if (new_state == KL_HTTP_CONN_WEBSOCKET &&
                    (events[i].ready & KL_EVENT_WRITE))
                    new_state = (KlHttpConnState)wsh->on_writable(c);
                goto transition;
            }

            /* Post-rejection drain (#278): the final response is already out and SEND half-closed;
             * every readable tick discards one bounded chunk until the body is consumed, the peer
             * EOFs, a bound is hit or the socket errors. Handled before the generic READ dispatch so
             * drained bytes are never parsed as a new request. */
            if (c->state == KL_HTTP_CONN_DRAINING) {
                new_state = c->answer_linger_stopped ? kl_http_conn_drain_hangup(c, events[i].ready)
                                                     : kl_http_conn_drain_step(c, kl_monotonic_ms());
                goto transition;
            }

            /* HTTP/2: handle read/write events through the h2 seam. */
            if (c->state == KL_HTTP_CONN_HTTP2) {
                const KlHttp2ServerHooks *h2h = kl_http2_server_hooks();
                if (!h2h) { new_state = KL_HTTP_CONN_CLOSED; goto transition; }
                if (events[i].ready & KL_EVENT_READ)
                    new_state = (KlHttpConnState)h2h->on_readable(c);
                if (new_state == KL_HTTP_CONN_HTTP2 &&
                    (events[i].ready & KL_EVENT_WRITE))
                    new_state = (KlHttpConnState)h2h->on_writable(c);
                goto transition;
            }

            /* Sending the streamed answer to a rejected request: the readable side is the rest of
             * its body, read and discarded within the drain's bounds, never parsed. */
            if (c->state == KL_HTTP_CONN_SENDING && c->reject_discard) {
                if (events[i].ready & (KL_EVENT_READ | KL_EVENT_HANGUP | KL_EVENT_ERROR))
                    new_state = kl_http_conn_discard_step(c, kl_monotonic_ms());
                if (new_state == KL_HTTP_CONN_CLOSED) goto transition;
            } else if (events[i].ready & KL_EVENT_READ) {
                new_state = kl_http_conn_on_readable(c, &s->router);
            }

            if (events[i].ready & KL_EVENT_WRITE) {
                if (new_state == KL_HTTP_CONN_SENDING || c->state == KL_HTTP_CONN_SENDING)
                    new_state = kl_http_conn_on_writable(c);
            }

            /* Try immediate send after read→parse→handle */
            if (new_state == KL_HTTP_CONN_SENDING) {
                new_state = kl_http_conn_on_writable(c);
            }

transition:
            /* A kept-alive TLS connection whose engine already holds its next request. */
            if (new_state == KL_HTTP_CONN_READING)
                new_state = kl_http_conn_redrive_tls(c, &s->router, new_state);
            /* Transition */
            switch (new_state) {
            case KL_HTTP_CONN_TLS_HANDSHAKE: {
                if (kl_event_mod(&s->ev.loop, c->stream.fd,
                                 (KlEventMask)c->tls_want, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_http_server_conn_release(s,c);
                }
                break;
            }
            case KL_HTTP_CONN_SENDING: {
                if (c->file_io_phase == 1) {
                    /* FILE_IO_READING: async read pending, no WRITE event */
                } else if (kl_event_mod(&s->ev.loop, c->stream.fd,
                                 (KlEventMask)(KL_EVENT_WRITE |
                                     ((c->reject_discard && !c->reject_input_done) ? KL_EVENT_READ : 0)),
                                 &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_http_server_conn_release(s,c);
                }
                break;
            }
            case KL_HTTP_CONN_WEBSOCKET: {
                KlEventMask ws_mask = KL_EVENT_READ;
                const KlWsServerHooks *wsh = kl_ws_server_hooks();
                if (wsh && wsh->drain_pending && wsh->drain_pending(c))
                    ws_mask = (KlEventMask)(KL_EVENT_READ | KL_EVENT_WRITE);
                if (kl_event_mod(&s->ev.loop, c->stream.fd, ws_mask, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_http_server_conn_release(s,c);
                }
                break;
            }
            case KL_HTTP_CONN_HTTP2: {
                KlEventMask mask = KL_EVENT_READ;
                const KlHttp2ServerHooks *h2h = kl_http2_server_hooks();
                if (h2h && h2h->want_write && h2h->want_write(c))
                    mask = (KlEventMask)(KL_EVENT_READ | KL_EVENT_WRITE);
                if (kl_event_mod(&s->ev.loop, c->stream.fd, mask, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_http_server_conn_release(s,c);
                }
                break;
            }
            case KL_HTTP_CONN_READING:
            case KL_HTTP_CONN_READING_BODY:
            case KL_HTTP_CONN_PROXY_HEADER: {
                /* Read-side flow control: a paused body consumer keeps READ interest OFF (0)
                 * so the level-triggered loop stops delivering body bytes; kl_http_request_resume_body
                 * re-arms READ. Only READING_BODY pauses. */
                KlEventMask rm = (new_state == KL_HTTP_CONN_READING_BODY && c->stream.read_paused)
                                     ? 0 : KL_EVENT_READ;
                if (kl_event_mod(&s->ev.loop, c->stream.fd, rm, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_http_server_conn_release(s,c);
                }
                break;
            }
            case KL_HTTP_CONN_DRAINING: {
                /* Post-rejection drain (#278): the response is flushed and SEND is half-closed; keep
                 * READ armed so each readable tick discards one bounded chunk. The sweep enforces the
                 * deadline, so a client that simply stops sending still gets closed. */
                /* A lingering drain past its cap reads no more: only the hang-up is watched (where
                 * the loop can), else no interest at all until the deadline. */
                KlEventMask dm = KL_EVENT_READ;
                if (c->answer_linger_stopped)
                    dm = (kl_event_caps(&s->ev.loop) & KL_EVENT_CAP_HANGUP) ? KL_EVENT_HANGUP
                                                                            : (KlEventMask)0;
                if (kl_event_mod(&s->ev.loop, c->stream.fd, dm, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_http_server_conn_release(s, c);
                }
                break;
            }
            case KL_HTTP_CONN_SUSPENDED: {
                /* Handler suspended for async I/O; FD already removed
                 * from event loop by kl_async_suspend. */
                break;
            }
            case KL_HTTP_CONN_CLOSED: {
                kl_event_del(&s->ev.loop, c->stream.fd);
                kl_http_server_conn_release(s,c);
                break;
            }
            case KL_HTTP_CONN_PROCESSING:
                /* Transient inside dispatch, and normalised away before any drive path sees it:
                 * conn_process() and kl_async_complete() both convert a left-over PROCESSING into
                 * SENDING or CLOSED precisely because no drive path acts on it. Arriving here would
                 * be a bug upstream, not something to recover from, and doing nothing is exactly
                 * what the previous if-chain did. The arm exists so the state is named rather than
                 * matched by nothing. */
                break;
            }   /* no default: a new KlHttpConnState must fail to compile here */
        }
        kl_event_ctx_dispatch_end(&s->ev);   /* reclaim watchers deferred during this batch */

        /* Sweep for timed-out connections.
         * Single-threaded: no TOCTOU risk; all event processing above is
         * complete, so connection states are stable.  Newly acquired slots
         * have fresh last_active_ms and won't be timed out. */
        now = kl_monotonic_ms();
        kl_http_server_sweep_conn_timeouts(s, now, 0);

        /* Sweep async op deadlines */
        kl_async_sweep_deadlines(s, now);

        /* Fire expired timers */
        kl_timer_fire(&s->ev);

        /* Graceful drain: stop when all connections are idle or the deadline hits. */
        kl_http_server_drain_progress(s, now);
    }

    kl_http_server_plat_signals_restore(s);

    /* Stopped by an error: a server that is no longer serving does not read as running, and the
     * caller hears of the failure (the header's contract: -1 on fatal error). */
    if (run_failed) {
        kl_atomic_store_int(&s->running, 0);
        return -1;
    }
    return 0;
}

/* The drain is entered by exactly one caller, and the state each later caller observes decides what it
 * does (stop_requested, KL_HTTP_STOP_*):
 *   NONE         nothing has stopped the server; the caller claims the drain (compare-and-swap);
 *   CLAIMED      a claimer is publishing the drain's deadline; the caller leaves the drain alone;
 *   ESTABLISHED  the drain (or a plain stop) is in effect; the caller forces an immediate stop.
 * So overlapping calls cannot rewrite the deadline (the claimer is its only writer) or escalate a drain
 * that has not begun, while a second stop issued once the drain is established still stops at once. */
int kl_http_server_stop_claim(KlHttpServer *s) {
    return kl_atomic_cas_int(&s->stop_requested, KL_HTTP_STOP_NONE, KL_HTTP_STOP_CLAIMED);
}

void kl_http_server_stop_publish(KlHttpServer *s) {
    /* The deadline is written before the flag is published (seq-cst stores): a loop tick that sees
     * draining also sees its deadline, never the 0 that would end the drain at once. A caller that
     * sees ESTABLISHED sees draining set. */
    s->drain_deadline_ms = kl_monotonic_ms() + (uint64_t)s->config.drain_timeout_ms;
    kl_atomic_store_int(&s->draining, 1);
    kl_atomic_store_int(&s->stop_requested, KL_HTTP_STOP_ESTABLISHED);
}

void kl_http_server_stop(KlHttpServer *s) {
    /* Recorded before `running` is touched, for a run that has not published `running` yet (it checks
     * stop_requested right after, so a stop is never lost to that run's own `running = 1`). */
    if (s->config.drain_timeout_ms <= 0) {
        kl_atomic_store_int(&s->stop_requested, KL_HTTP_STOP_ESTABLISHED);
        kl_atomic_store_int(&s->running, 0);
    } else if (kl_http_server_stop_claim(s)) {
        kl_http_server_stop_publish(s);      /* enter drain mode: stop accepting, let in-flight finish */
    } else if (kl_atomic_load_int(&s->stop_requested) == KL_HTTP_STOP_ESTABLISHED) {
        kl_atomic_store_int(&s->running, 0); /* a second stop of an established drain ends it now */
    }
    /* else CLAIMED: another caller is entering the drain; this call only wakes the loop. */

    /* Wake the run loop so it re-checks running/drain now instead of after the current
     * wait/drain tick. write() is async-signal-safe + thread-safe, so this is valid both
     * from the SIGTERM/SIGINT handler and from another thread (the common test pattern:
     * server on a worker thread, stop from main). Falls back to tick-timeout latency if the
     * wakeup pair couldn't be opened. */
    if (kl_handle_valid(s->stop_wake_wr)) {
        KlPlatWakeup w = { s->stop_wake_rd, s->stop_wake_wr };
        kl_plat_wakeup_signal(&w);
    }
}

/* kl_http_request_pause_body / kl_http_request_resume_body / kl_http_server_stats live in the
 * freestanding-safe server core (http_server_core.c). */

/* kl_http_server_free lives in the freestanding-safe server core (http_server_core.c): a
 * freestanding EFI server tears itself down
 * (close listener + accepted children, free pool/router, destroy TLS ctx) from the
 * archive alone, then kl_uefi_shutdown() releases the EFI providers before
 * ExitBootServices. The hosted-only bits (async-op cancel, AF_UNIX unlink) are
 * #ifndef KEEL_FREESTANDING there. */
