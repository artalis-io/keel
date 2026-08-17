#include <keel/server.h>
#include <keel/async.h>
#include <keel/timer.h>
#include <keel/tls.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include "internal.h"
#include "socket.h"       /* seam: kl_sock_* + KlSockAddr (no direct sockaddr) */
#include "event_caps.h"   /* PAL Phase 7: event↔socket capability negotiation */
#include "io_engine.h"    /* PAL Phase 8: completion-loop tick dispatch (IOCP) */
#include "server_plat.h"  /* AF_UNIX bind, peer creds, signals — per-platform, no #ifdef here */
#include "platform.h"     /* KlPlatWakeup — self-pipe so kl_server_stop wakes the run loop */
/* No websocket_server.h / h2_server.h / h2_internal.h: the readiness data plane now
 * dispatches ws/h2 (and PROXY) through proto_hooks.h — the same seam the completion
 * driver uses — so this TU names no optional-protocol type or symbol (Finding 1). */
#include "proto_hooks.h"  /* ws/h2/proxy readiness + upgrade seam */

#define KL_LISTEN_BACKLOG  128
#define KL_EVENTS_PER_TICK 64
#define KL_POLL_TIMEOUT_MS 1000


void kl_log(KlServer *s, int level, const char *fmt, ...) {
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

void kl_log_errno(KlServer *s, int level, const char *msg) {
    kl_log(s, level, "%s: %s", msg, strerror(errno));
}

static int kl_server_bind_tcp(KlServer *s) {
    /* Parse the numeric bind address (IPv4/IPv6 literal) — no DNS. */
    KlSockAddr bind_sa;
    if (kl_sockaddr_parse(&bind_sa, s->config.bind_addr, (uint16_t)s->config.port) != 0) {
        kl_log(s, KL_LOG_ERROR, "invalid bind address '%s'", s->config.bind_addr);
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    int family = (kl_sockaddr_family(&bind_sa) == KL_AF_INET6) ? AF_INET6 : AF_INET;

    s->listen_fd = kl_sock_socket(s->ev.sockets, family, SOCK_STREAM, 0);
    if (!kl_handle_valid(s->listen_fd)) {
        kl_log_errno(s, KL_LOG_ERROR, "socket");
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
        kl_log_errno(s, KL_LOG_ERROR, "bind");
        s->last_error = KL_ERR_BIND;
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
        return -1;
    }

    /* Retrieve OS-assigned port (useful when config.port == 0) */
    {
        KlSockAddr la;
        if (kl_sock_get_local_addr(s->ev.sockets, s->listen_fd, &la) == 0)
            s->bound_port = kl_sockaddr_port(&la);
    }

    return 0;
}

/*
 * Adopt a pre-bound, already-listening fd (socket activation). The fd's
 * family determines the transport, so TCP_NODELAY and unlink policy stay
 * correct. KEEL never owns (never unlinks) an adopted UNIX socket.
 */
static int kl_server_adopt_fd(KlServer *s) {
    s->listen_fd = s->config.listen_fd;

    KlSockAddr la;
    if (kl_sock_get_local_addr(s->ev.sockets, s->listen_fd, &la) != 0) {
        kl_log_errno(s, KL_LOG_ERROR, "getsockname on adopted fd");
        s->last_error = KL_ERR_SOCKET;
        s->listen_fd = KL_INVALID_SOCKET;
        return -1;
    }

    if (kl_sockaddr_family(&la) == KL_AF_UNIX) {
        s->config.transport = KL_TRANSPORT_UNIX;
        s->bound_port = 0;
    } else {
        s->config.transport = KL_TRANSPORT_TCP;
        s->bound_port = kl_sockaddr_port(&la);
    }
    /* Adopted fd is never unlinked — the supervisor owns the socket path. */
    s->unix_socket_owned = 0;
    /* CLOEXEC so the inherited listener doesn't re-leak to our own children. */
    kl_sock_set_cloexec(s->ev.sockets, s->listen_fd);
    return 0;
}

static int kl_server_bind_listener(KlServer *s) {
    if (s->config.listen_fd > 0)
        return kl_server_adopt_fd(s);
    if (s->config.transport == KL_TRANSPORT_UNIX)
        return kl_server_plat_bind_unix(s);
    return kl_server_bind_tcp(s);
}

int kl_request_peer_cred(const KlRequest *req, KlPeerCred *out) {
    if (!req)
        return -1;
    const KlConn *conn = kl_request_conn(req);
    if (!conn)
        return -1;
    return kl_peer_cred_fd(conn->stream.fd, out);
}

int kl_request_peer_label(const KlRequest *req, char *buf, size_t buflen) {
    if (!req || !buf || buflen == 0)
        return -1;
    const KlConn *conn = kl_request_conn(req);
    if (!conn || !kl_handle_valid(conn->stream.fd))
        return -1;

    return kl_server_plat_peer_label_fd(conn->stream.fd, buf, buflen);
}

const KlSockAddr *kl_request_peer_sockaddr(const KlRequest *req) {
    if (!req)
        return NULL;
    const KlConn *conn = kl_request_conn(req);
    if (!conn || kl_sockaddr_family(&conn->stream.peer_addr) == KL_AF_UNSPEC)
        return NULL;
    return &conn->stream.peer_addr;
}

int kl_request_peer_addr(const KlRequest *req, char *ip, size_t iplen,
                         uint16_t *port) {
    if (!req || !ip || iplen == 0)
        return -1;
    const KlConn *conn = kl_request_conn(req);
    if (!conn)
        return -1;

    const KlSockAddr *a = &conn->stream.peer_addr;
    KlAddrFamily fam = kl_sockaddr_family(a);
    if (fam != KL_AF_INET && fam != KL_AF_INET6)
        return -1;  /* UNSPEC / AF_UNIX — no IP address (use peer credentials) */
    if (kl_sockaddr_format_ip(a, ip, iplen) < 0)
        return -1;
    if (port)
        *port = kl_sockaddr_port(a);
    return 0;
}

int kl_request_peer_cert(const KlRequest *req, KlPeerCert *out) {
    if (!req || !out)
        return -1;
    const KlConn *conn = kl_request_conn(req);
    if (!conn || !conn->tls || !conn->tls->peer_cert)
        return -1;   /* plaintext connection, or backend lacks mTLS support */
    memset(out, 0, sizeof(*out));
    return conn->tls->peer_cert(conn->tls, out);
}

/* Socket-activation (systemd LISTEN_* fd inheritance) moved to server_activation.c
 * (Finding 6): a self-contained responsibility, separate from the readiness loop,
 * TCP listener construction, and peer accessors. */

/* Non-static so the freestanding kl_server_free (server_core.c) can call it on the
 * hosted path (it needs the AF_UNIX unlink); declared in internal.h. */
void kl_server_close_listener(KlServer *s) {
    if (kl_handle_valid(s->listen_fd)) {
        kl_sock_close(s->ev.sockets, s->listen_fd);
        s->listen_fd = KL_INVALID_SOCKET;
    }
    kl_server_plat_unlink_owned_unix(s);   /* POSIX: lstat+S_ISSOCK+unlink; Win: DeleteFile */
}

/* kl_server_conn_release moved to the freestanding-safe server core (server_core.c)
 * — the completion sweeps there call it, so the archive needs it in-core.
 *
 * kl_server_init (+ its stop-wakeup self-pipe helpers) also moved to server_core.c in
 * the Phase 10 UEFI server carve (S-4): a freestanding EFI server constructs its
 * KlServer from the archive alone. The hosted-only pieces (ws/h2/proxy hook installers,
 * PROXY CIDR allowlist, async file I/O, self-pipe) are #ifndef KEEL_FREESTANDING there. */


/* Route + middleware registration (the kl_server_route / kl_server_use family) and
 * the read-side body flow control (kl_request_pause_body / resume_body) plus
 * kl_server_stats moved to the freestanding-safe server core (server_core.c) in the
 * S-1 bisection. kl_server_ws moved to server_ws.c (Finding 1): the WebSocket type +
 * registration belong to the ws module, so this readiness TU owns no protocol type. */


/* ── Readiness accept adapter (step 6B-1): drive KlListener over the listen fd + pool credits ──
 * The listener owns the accept lifecycle (reserve-before-accept backpressure, pause/resume,
 * confirmed retirement); these hooks bridge it to the event loop and the split-credit pool. Only
 * installed for a readiness loop (!completion_loop); the completion path keeps its own accept
 * priming until 6B-3 part 2b-ii adopts the listener. */
static int server_accept_reserve(void *ctx) {
    KlServer *s = ctx;
    return kl_conn_pool_reserve(&s->pool);
}
static void server_accept_release(void *ctx) {
    KlServer *s = ctx;
    kl_conn_pool_return_credit(&s->pool);               /* free_credits++ ... */
    kl_listener_notify_slot_free(&s->accept_listener);  /* ...then resume the accept if paused */
}
static int server_accept_arm(void *ctx) {
    KlServer *s = ctx;
    if (s->listen_registered) return 0;                 /* already armed (level-triggered) */
    if (kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL) < 0) return -1;
    s->listen_registered = 1;
    return 0;
}
static void server_accept_disarm(void *ctx) {
    KlServer *s = ctx;
    if (s->listen_registered) {
        kl_event_del(&s->ev.loop, s->listen_fd);
        s->listen_registered = 0;
    }
}
static void server_accept_dispose(void *ctx, KlSocketHandle fd) {
    kl_sock_close(((KlServer *)ctx)->ev.sockets, fd);   /* no local var: read-only, keep ctx void* */
}
static void server_accept_on_accept(void *ctx, KlSocketHandle fd, KlSlotLease lease) {
    KlServer *s = ctx;
    KlConn *nc = kl_conn_acquire(&s->pool, fd);         /* commit — credit already reserved */
    if (!nc) {
        /* Invariant failure (credit-backed accept but no free KlConn): dispose the descriptor and
         * consume the lease to return the credit. free_credits+reserved+active==capacity guarantees
         * this cannot happen, but never leak an fd or a credit if it somehow does. */
        kl_sock_close(s->ev.sockets, fd);
        kl_slot_lease_release(&lease);
        return;
    }
    nc->slot_lease = lease;                             /* consumed once on close (after conn return) */
    nc->peer_source = KL_PEER_SOCKET;
    nc->stream.peer_addr = s->accept_pending_peer;
    nc->res.alloc = &s->alloc_storage;
    if (s->config.tls) {                               /* TLS: enter handshake instead of reading */
        nc->state = KL_CONN_TLS_HANDSHAKE;
        nc->tls_want = KL_EVENT_READ;
    }
    /* PROXY protocol from a trusted source: read the header first (before TLS/HTTP). */
    const KlProxyHooks *ph = kl_proxy_hooks();
    if (s->proxy_cidr_count > 0 && ph && ph->cidr_match &&
        kl_sockaddr_family(&s->accept_pending_peer) != KL_AF_UNSPEC &&
        ph->cidr_match(s->proxy_cidrs, s->proxy_cidr_count, &s->accept_pending_peer)) {
        nc->state = KL_CONN_PROXY_HEADER;
    }
    if (kl_event_add(&s->ev.loop, fd, KL_EVENT_READ, &nc->stream) < 0)  /* identity = raw stream */
        kl_server_conn_release(s, nc);                 /* releases the conn AND consumes the lease */
}

static int server_accept_listener_start(KlServer *s) {
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

int kl_server_run(KlServer *s) {
    KlAllocator *alloc = &s->alloc_storage;
    (void)alloc;   /* the readiness accept path now configures conns via the listener adapter */

    if (kl_server_bind_listener(s) < 0)
        return -1;

    /* An adopted fd (socket activation) is already listening — don't re-listen. */
    if (s->config.listen_fd <= 0 && kl_sock_listen(s->ev.sockets, s->listen_fd, KL_LISTEN_BACKLOG) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "listen");
        s->last_error = KL_ERR_LISTEN;
        kl_server_close_listener(s);
        return -1;
    }

    if (kl_sock_set_nonblocking(s->ev.sockets, s->listen_fd) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "fcntl");
        s->last_error = KL_ERR_SOCKET;
        kl_server_close_listener(s);
        return -1;
    }

    /* Register listen socket for read events */
    if (kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "event_add listen");
        s->last_error = KL_ERR_EVENT_ADD;
        kl_server_close_listener(s);
        return -1;
    }
    s->listen_registered = 1;   /* the readiness listener's arm hook treats this as already armed */

    if (s->config.transport == KL_TRANSPORT_UNIX) {
        /* An adopted fd (socket activation) has no path we own. */
        kl_log(s, KL_LOG_INFO, "listening on unix:%s",
               s->config.unix_socket_path ? s->config.unix_socket_path
                                          : "(inherited fd)");
    } else if (s->config.listen_fd > 0) {
        kl_log(s, KL_LOG_INFO, "listening on inherited fd %d (port %d)",
               (int)s->listen_fd, s->bound_port);
    } else {
        kl_log(s, KL_LOG_INFO, "listening on %s:%d",
               s->config.bind_addr, s->bound_port);
    }

    /* Ignore SIGPIPE + install SIGTERM/SIGINT graceful-stop handlers (POSIX) or
     * a console Ctrl handler (Windows). Done here — past the setup early-returns
     * — so a failed bind never leaves handlers installed without a restore. */
    kl_server_plat_signals_install(s);

    atomic_store(&s->running, 1);
    atomic_store(&s->draining, 0);
    KlEvent events[KL_EVENTS_PER_TICK];

    /* A completion loop (IOCP) is driven by the completion tick, not the readiness
     * wait/dispatch below. Detected once from the backend's advertised event model
     * (PAL Phase 8 io_engine seam) — the completion concept never enters the
     * readiness path. Never true on readiness backends, so that path is unchanged. */
    const int completion_loop =
        (kl_event_caps(&s->ev.loop) & KL_EVENT_CAP_COMPLETION) != 0;

    /* Readiness loops drive the accept path through the embedded KlListener (step 6B-1); the
     * completion path keeps its own accept priming until 6B-3 part 2b-ii. */
    if (!completion_loop) {
        if (server_accept_listener_start(s) < 0) {
            s->last_error = KL_ERR_EVENT_ADD;
            kl_server_close_listener(s);
            return -1;
        }
        s->accept_via_listener = 1;
    }

    while (atomic_load(&s->running)) {
        if (completion_loop) {
            /* One completion-loop tick — factored into the freestanding-safe server
             * core (server_core.c) so a freestanding EFI server shares it verbatim. */
            if (kl_server_run_completion_loop(s) < 0)
                break;
            continue;
        }
        /* Compute dynamic timeout based on nearest async op deadline */
        uint64_t now = kl_monotonic_ms();
        int wait_timeout = KL_POLL_TIMEOUT_MS;
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

        wait_timeout = kl_timer_next_timeout(&s->ev, wait_timeout);

        int n = kl_event_wait(&s->ev.loop, events, KL_EVENTS_PER_TICK,
                              wait_timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            kl_log_errno(s, KL_LOG_ERROR, "event_wait");
            break;
        }

        /* Dispatch async file I/O completions */
        if (s->file_io) {
            KlFileIOResult fio_results[KL_EVENTS_PER_TICK];
            int nf = s->file_io->tick(s->file_io, fio_results,
                                       KL_EVENTS_PER_TICK);
            for (int fi = 0; fi < nf; fi++) {
                KlConn *fc = fio_results[fi].udata;
                KlConnState fstate = kl_conn_on_file_complete(
                    fc, fio_results[fi].result, fio_results[fi].zero_copy);
                if (fstate == KL_CONN_SENDING) {
                    if (fc->file_io_phase == 1) {
                        /* FILE_IO_READING — async read pending, no WRITE reg */
                    } else {
                        kl_event_mod(&s->ev.loop, fc->stream.fd,
                                     KL_EVENT_WRITE, &fc->stream);
                    }
                } else if (fstate == KL_CONN_READING) {
                    kl_event_mod(&s->ev.loop, fc->stream.fd, KL_EVENT_READ, &fc->stream);
                } else if (fstate == KL_CONN_CLOSED) {
                    kl_event_del(&s->ev.loop, fc->stream.fd);
                    kl_server_conn_release(s, fc);
                }
            }
        }

        /* R3b-W: bracket the batch dispatch so a watcher deleted mid-batch (e.g. via async
         * completion during a connection callback) is not freed until every event in this batch
         * has been dispatched — closing the pointer-reuse ABA hole. If begin fails (max nesting
         * depth — an unbalanced-bracket bug), do not dispatch; fail-stop the run loop. */
        if (kl_event_ctx_dispatch_begin(&s->ev) < 0) {
            kl_log(s, KL_LOG_ERROR, "dispatch nesting overflow");
            break;
        }
        for (int i = 0; i < n; i++) {
            /* Watcher dispatch (tagged pointer, LSB=1) */
            if (kl_event_dispatch(&s->ev, &events[i]))
                continue;

            /* Readiness event identity is the raw KlStream (step 6B-2); recover the owning KlConn
             * only here, at the HTTP adapter boundary. NULL udata = the listen socket. */
            KlStream *evs = (KlStream *)events[i].udata;
            KlConn *c = evs ? kl_conn_from_stream(evs) : NULL;

            if (c == NULL) {
                /* Listen socket — accept new connections through the readiness KlListener
                 * (step 6B-1). It holds one reserved pool credit while LISTENING; each accepted
                 * fd is committed + wired by server_accept_on_accept; when the pool credit is
                 * exhausted it transitions to PAUSED (disarming the listen fd, so the kernel TCP
                 * backlog queues the rest) and resumes when a slot frees. */
                if (atomic_load(&s->draining)) goto rearm_listen;
                while (kl_listener_state(&s->accept_listener) == KL_LISTENER_STATE_LISTENING) {
                    KlSockAddr peer;
                    KlSocketHandle client_fd = kl_sock_accept(s->ev.sockets, s->listen_fd,
                                                              &peer);
                    if (!kl_handle_valid(client_fd)) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        kl_log_errno(s, KL_LOG_ERROR, "accept");
                        break;
                    }

                    /* The default provider's accept (kl_sockdef_accept) already returns a
                     * non-blocking, close-on-exec socket (accept4 on Linux/BSD) — skip the two
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
                    if (s->config.transport == KL_TRANSPORT_TCP)
                        (void)kl_sock_set_tcp_nodelay(s->ev.sockets, client_fd, 1);

                    /* Hand the accepted fd to the listener: it commits the reserved credit to a
                     * KlConn (server_accept_on_accept), then reserves + arms the next accept — or
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

            /* Client connection event */
            KlConnState new_state = c->state;

            /* PROXY protocol header — read before TLS/HTTP */
            if (c->state == KL_CONN_PROXY_HEADER) {
                int pr = kl_conn_read_proxy_header(c);
                if (pr < 0) {
                    new_state = KL_CONN_CLOSED;
                    goto transition;
                }
                if (pr == 0) {
                    new_state = KL_CONN_PROXY_HEADER;   /* need more bytes */
                    goto transition;
                }
                /* Done — advance to the real initial state. */
                if (s->config.tls) {
                    c->tls_want = KL_EVENT_READ;
                    c->state = KL_CONN_TLS_HANDSHAKE;
                } else {
                    c->state = KL_CONN_READING;
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

            /* TLS handshake — handle before normal read/write */
            if (c->state == KL_CONN_TLS_HANDSHAKE) {
                new_state = kl_conn_on_handshake(c);
                goto transition;
            }

            /* WebSocket — handle read/write events through the ws seam (symmetric with
             * the completion path's KlWsCompHooks; the conn only reached this state via a
             * ws upgrade, so the hooks are installed). */
            if (c->state == KL_CONN_WEBSOCKET) {
                const KlWsServerHooks *wsh = kl_ws_server_hooks();
                if (!wsh) { new_state = KL_CONN_CLOSED; goto transition; }
                if (events[i].ready & KL_EVENT_READ)
                    new_state = (KlConnState)wsh->on_readable(c);
                if (new_state == KL_CONN_WEBSOCKET &&
                    (events[i].ready & KL_EVENT_WRITE))
                    new_state = (KlConnState)wsh->on_writable(c);
                goto transition;
            }

            /* HTTP/2 — handle read/write events through the h2 seam. */
            if (c->state == KL_CONN_HTTP2) {
                const KlH2ServerHooks *h2h = kl_h2_server_hooks();
                if (!h2h) { new_state = KL_CONN_CLOSED; goto transition; }
                if (events[i].ready & KL_EVENT_READ)
                    new_state = (KlConnState)h2h->on_readable(c);
                if (new_state == KL_CONN_HTTP2 &&
                    (events[i].ready & KL_EVENT_WRITE))
                    new_state = (KlConnState)h2h->on_writable(c);
                goto transition;
            }

            if (events[i].ready & KL_EVENT_READ) {
                new_state = kl_conn_on_readable(c, &s->router);
            }

            if (events[i].ready & KL_EVENT_WRITE) {
                if (new_state == KL_CONN_SENDING || c->state == KL_CONN_SENDING)
                    new_state = kl_conn_on_writable(c);
            }

            /* Try immediate send after read→parse→handle */
            if (new_state == KL_CONN_SENDING) {
                new_state = kl_conn_on_writable(c);
            }

transition:
            /* Transition */
            if (new_state == KL_CONN_TLS_HANDSHAKE) {
                if (kl_event_mod(&s->ev.loop, c->stream.fd,
                                 (KlEventMask)c->tls_want, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_SENDING) {
                if (c->file_io_phase == 1) {
                    /* FILE_IO_READING — async read pending, no WRITE event */
                } else if (kl_event_mod(&s->ev.loop, c->stream.fd,
                                 KL_EVENT_WRITE, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_WEBSOCKET) {
                KlEventMask ws_mask = KL_EVENT_READ;
                const KlWsServerHooks *wsh = kl_ws_server_hooks();
                if (wsh && wsh->drain_pending && wsh->drain_pending(c))
                    ws_mask = (KlEventMask)(KL_EVENT_READ | KL_EVENT_WRITE);
                if (kl_event_mod(&s->ev.loop, c->stream.fd, ws_mask, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_HTTP2) {
                KlEventMask mask = KL_EVENT_READ;
                const KlH2ServerHooks *h2h = kl_h2_server_hooks();
                if (h2h && h2h->want_write && h2h->want_write(c))
                    mask = (KlEventMask)(KL_EVENT_READ | KL_EVENT_WRITE);
                if (kl_event_mod(&s->ev.loop, c->stream.fd, mask, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_READING ||
                       new_state == KL_CONN_READING_BODY ||
                       new_state == KL_CONN_PROXY_HEADER) {
                /* Read-side flow control: a paused body consumer keeps READ interest OFF (0)
                 * so the level-triggered loop stops delivering body bytes; kl_request_resume_body
                 * re-arms READ. Only READING_BODY pauses. */
                KlEventMask rm = (new_state == KL_CONN_READING_BODY && c->stream.read_paused)
                                     ? 0 : KL_EVENT_READ;
                if (kl_event_mod(&s->ev.loop, c->stream.fd, rm, &c->stream) < 0) {
                    kl_event_del(&s->ev.loop, c->stream.fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_SUSPENDED) {
                /* Handler suspended for async I/O — FD already removed
                 * from event loop by kl_async_suspend. */
            } else if (new_state == KL_CONN_CLOSED) {
                kl_event_del(&s->ev.loop, c->stream.fd);
                kl_server_conn_release(s,c);
            }
        }
        kl_event_ctx_dispatch_end(&s->ev);   /* R3b-W: reclaim watchers deferred during this batch */

        /* Sweep for timed-out connections.
         * Single-threaded: no TOCTOU risk — all event processing above is
         * complete, so connection states are stable.  Newly acquired slots
         * have fresh last_active_ms and won't be timed out. */
        now = kl_monotonic_ms();
        kl_server_sweep_conn_timeouts(s, now, 0);

        /* Sweep async op deadlines */
        {
            KlAsyncOp *aop = s->async_ops;
            while (aop) {
                KlAsyncOp *next_aop = aop->next;
                if (!aop->_terminal && aop->deadline_ms > 0 && now >= aop->deadline_ms) {
                    aop->deadline_ms = 0;   /* fire on_deadline at most once */
                    if (aop->on_deadline)
                        aop->on_deadline(aop, aop->user_data);
                }
                aop = next_aop;
            }
        }

        /* Fire expired timers */
        kl_timer_fire(&s->ev);

        /* Graceful drain: stop when all connections are idle or the deadline hits. */
        kl_server_drain_progress(s, now);
    }

    kl_server_plat_signals_restore(s);

    return 0;
}

void kl_server_stop(KlServer *s) {
    if (s->config.drain_timeout_ms > 0 && !atomic_load(&s->draining)) {
        /* Enter drain mode: stop accepting, let in-flight finish */
        atomic_store(&s->draining, 1);
        s->drain_deadline_ms = kl_monotonic_ms() +
                               (uint64_t)s->config.drain_timeout_ms;
    } else {
        atomic_store(&s->running, 0);
    }
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

/* kl_request_pause_body / kl_request_resume_body / kl_server_stats moved to the
 * freestanding-safe server core (server_core.c) in the S-1 bisection. */

/* kl_server_free moved to the freestanding-safe server core (server_core.c) in the
 * Phase 10 UEFI server S-7 teardown carve — a freestanding EFI server tears itself down
 * (close listener + accepted children, free pool/router, destroy TLS ctx) from the
 * archive alone, then kl_uefi_shutdown() releases the EFI providers before
 * ExitBootServices. The hosted-only bits (async-op cancel, AF_UNIX unlink) are
 * #ifndef KEEL_FREESTANDING there. */
