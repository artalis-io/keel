#include <keel/server.h>
#include <keel/async.h>
#include <keel/tls.h>
#include <keel/websocket_server.h>
#include <keel/h2_server.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include "internal.h"

#define KL_LISTEN_BACKLOG  128
#define KL_EVENTS_PER_TICK 64
#define KL_POLL_TIMEOUT_MS 1000

/* ── Signal handling ─────────────────────────────────────────────── */

static _Atomic(KlServer *) kl_signal_server = NULL;

static void kl_signal_handler(int sig) {
    (void)sig;
    KlServer *s = atomic_load(&kl_signal_server);
    if (s) kl_server_stop(s);
}

static const char kl_408_response[] =
    "HTTP/1.1 408 Request Timeout\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

__attribute__((format(printf, 3, 4)))
static void kl_log(KlServer *s, int level, const char *fmt, ...) {
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

static void kl_log_errno(KlServer *s, int level, const char *msg) {
    kl_log(s, level, "%s: %s", msg, strerror(errno));
}

/*
 * Release a connection and resume the listen socket if it was paused
 * due to pool exhaustion.  Called from the event loop, timeout sweep,
 * and async completion.
 */
void kl_server_conn_release(KlServer *s, KlConn *c) {
    kl_conn_release(&s->pool, c);
    if (s->listen_paused && s->pool.free_list) {
        kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL);
        s->listen_paused = 0;
    }
}

int kl_server_init(KlServer *s, const KlConfig *config) {
    if (!s || !config) return -1;
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;

    /* Apply defaults */
    s->config = *config;
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

    /* Set up allocator */
    if (s->config.alloc) {
        s->alloc_storage = *s->config.alloc;
    } else {
        s->alloc_storage = kl_allocator_default();
    }
    KlAllocator *alloc = &s->alloc_storage;

    /* Init subsystems */
    if (kl_router_init(&s->router, alloc) < 0) return -1;
    if (kl_conn_pool_init(&s->pool, s->config.max_connections, alloc) < 0) {
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

    /* Create parsers and propagate config for each connection slot */
    for (int i = 0; i < s->pool.capacity; i++) {
        s->pool.conns[i].parser = s->config.parser(alloc);
        if (!s->pool.conns[i].parser) {
            kl_conn_pool_free(&s->pool);
            kl_router_free(&s->router);
            return -1;
        }
        s->pool.conns[i].access_log = s->config.access_log;
        s->pool.conns[i].access_log_data = s->config.access_log_data;
        s->pool.conns[i].h2_config = s->config.h2;  /* NULL if disabled */
        s->pool.conns[i].router = &s->router;
        s->pool.conns[i].max_body_size = s->config.max_body_size;
    }

    /* Pre-allocate TLS sessions (one per connection slot) */
    if (s->config.tls) {
        for (int i = 0; i < s->pool.capacity; i++) {
            s->pool.conns[i].tls = s->config.tls->factory(
                s->config.tls->ctx, alloc);
            if (!s->pool.conns[i].tls) {
                kl_conn_pool_free(&s->pool);
                kl_router_free(&s->router);
                return -1;
            }
            /* Validate vtable — all 7 required pointers must be set
             * (alpn_protocol is optional, NULL if not supported) */
            const KlTls *t = s->pool.conns[i].tls;
            if (!t->handshake || !t->read || !t->write ||
                !t->shutdown || !t->pending || !t->reset || !t->destroy) {
                kl_conn_pool_free(&s->pool);
                kl_router_free(&s->router);
                return -1;
            }
        }
    }

    /* Init event context — must happen before thread pool / watcher registration */
    if (kl_event_ctx_init(&s->ev, alloc) < 0) {
        kl_conn_pool_free(&s->pool);
        kl_router_free(&s->router);
        return -1;
    }

    return 0;
}

int kl_server_route(KlServer *s, const char *method, const char *pattern,
                    KlHandler handler, void *user_data,
                    KlBodyReaderFactory body_reader) {
    return kl_router_add(&s->router, method, pattern, handler, user_data,
                         body_reader);
}

int kl_server_use(KlServer *s, const char *method, const char *pattern,
                  KlMiddleware fn, void *user_data) {
    return kl_router_use(&s->router, method, pattern, fn, user_data);
}

int kl_server_use_post(KlServer *s, const char *method, const char *pattern,
                       KlMiddleware fn, void *user_data) {
    return kl_router_use_post(&s->router, method, pattern, fn, user_data);
}

int kl_server_ws(KlServer *s, const char *pattern, KlWsServerConfig *config) {
    /* Register as a GET route with no handler — ws_config triggers upgrade */
    if (kl_router_add(&s->router, "GET", pattern, NULL, NULL, NULL) < 0)
        return -1;
    s->router.routes[s->router.count - 1].ws_config = config;
    return 0;
}

int kl_server_run(KlServer *s) {
    KlAllocator *alloc = &s->alloc_storage;

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Create listen socket */
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        kl_log_errno(s, KL_LOG_WARN, "setsockopt SO_REUSEADDR");
#ifdef SO_REUSEPORT
    (void)setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)s->config.port),
    };
    if (inet_pton(AF_INET, s->config.bind_addr, &addr.sin_addr) != 1) {
        kl_log(s, KL_LOG_ERROR, "invalid bind address '%s'",
               s->config.bind_addr);
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "bind");
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    if (listen(s->listen_fd, KL_LISTEN_BACKLOG) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "listen");
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    if (set_nonblocking(s->listen_fd) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "fcntl");
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    /* Register listen socket for read events */
    if (kl_event_add(&s->ev.loop, s->listen_fd, KL_EVENT_READ, NULL) < 0) {
        kl_log_errno(s, KL_LOG_ERROR, "event_add listen");
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    kl_log(s, KL_LOG_INFO, "listening on %s:%d",
           s->config.bind_addr, s->config.port);

    /* Install signal handlers if requested */
    struct sigaction old_term, old_int;
    if (s->config.install_signal_handlers) {
        atomic_store(&kl_signal_server, s);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = kl_signal_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, &old_term);
        sigaction(SIGINT, &sa, &old_int);
    }

    atomic_store(&s->running, 1);
    atomic_store(&s->draining, 0);
    KlEvent events[KL_EVENTS_PER_TICK];

    while (atomic_load(&s->running)) {
        /* Compute dynamic timeout based on nearest async op deadline */
        uint64_t now = kl_monotonic_ms();
        int wait_timeout = KL_POLL_TIMEOUT_MS;
        for (KlAsyncOp *aop = s->async_ops; aop; aop = aop->next) {
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

        int n = kl_event_wait(&s->ev.loop, events, KL_EVENTS_PER_TICK,
                              wait_timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            kl_log_errno(s, KL_LOG_ERROR, "event_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            /* Tagged pointer dispatch: bit 0 set = watcher, clear = connection */
            uintptr_t tag = (uintptr_t)events[i].udata;
            if (tag & 1) {
                KlWatcher *w = (KlWatcher *)(tag & ~(uintptr_t)1);
                int wfd = w->fd;
                w->on_ready(wfd, events[i].ready, w->user_data);
                /* Re-arm for one-shot backends (io_uring POLL_ADD).
                 * Safe no-op if callback removed the watcher. */
                kl_watcher_rearm(&s->ev, wfd);
                continue;
            }

            KlConn *c = (KlConn *)events[i].udata;

            if (c == NULL) {
                /* Listen socket — accept new connections */
                if (atomic_load(&s->draining)) goto rearm_listen;
                while (1) {
                    int client_fd = accept(s->listen_fd, NULL, NULL);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        kl_log_errno(s, KL_LOG_ERROR, "accept");
                        break;
                    }

                    if (set_nonblocking(client_fd) < 0) {
                        close(client_fd);
                        continue;
                    }
                    int nodelay = 1;
                    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                                     &nodelay, sizeof(nodelay));

                    KlConn *nc = kl_conn_acquire(&s->pool, client_fd);
                    if (!nc) {
                        close(client_fd);
                        /* Pool full — stop accepting until a slot frees up.
                         * The kernel TCP backlog queues further connections. */
                        kl_event_del(&s->ev.loop, s->listen_fd);
                        s->listen_paused = 1;
                        break;
                    }

                    /* Set allocator on connection's response.
                     * This is set once on accept; response reuses it across keep-alive. */
                    nc->res.alloc = alloc;

                    /* TLS: enter handshake state instead of reading */
                    if (s->config.tls) {
                        nc->state = KL_CONN_TLS_HANDSHAKE;
                        nc->tls_want = KL_EVENT_READ;
                    }

                    if (kl_event_add(&s->ev.loop, client_fd,
                                     KL_EVENT_READ, nc) < 0) {
                        kl_server_conn_release(s,nc);
                        continue;
                    }
                }
                /* Re-arm listen socket (no-op for persistent backends;
                 * required for io_uring's one-shot POLL_ADD) */
rearm_listen:
                if (!s->listen_paused)
                    kl_event_mod(&s->ev.loop, s->listen_fd,
                                 KL_EVENT_READ, NULL);
                continue;
            }

            /* Client connection event */
            KlConnState new_state = c->state;

            /* TLS handshake — handle before normal read/write */
            if (c->state == KL_CONN_TLS_HANDSHAKE) {
                new_state = kl_conn_on_handshake(c);
                goto transition;
            }

            /* WebSocket — handle read events */
            if (c->state == KL_CONN_WEBSOCKET) {
                if (events[i].ready & KL_EVENT_READ)
                    new_state = (KlConnState)kl_ws_server_on_readable(c);
                goto transition;
            }

            /* HTTP/2 — handle read/write events */
            if (c->state == KL_CONN_HTTP2) {
                if (events[i].ready & KL_EVENT_READ)
                    new_state = (KlConnState)kl_h2_server_on_readable(c);
                if (new_state == KL_CONN_HTTP2 &&
                    (events[i].ready & KL_EVENT_WRITE))
                    new_state = (KlConnState)kl_h2_server_on_writable(c);
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
                if (kl_event_mod(&s->ev.loop, c->fd,
                                 (KlEventMask)c->tls_want, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_SENDING) {
                if (kl_event_mod(&s->ev.loop, c->fd,
                                 KL_EVENT_WRITE, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_WEBSOCKET) {
                if (kl_event_mod(&s->ev.loop, c->fd,
                                 KL_EVENT_READ, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_HTTP2) {
                KlEventMask mask = KL_EVENT_READ;
                if (c->h2 && c->h2->session &&
                    c->h2->session->want_write(c->h2->session))
                    mask = (KlEventMask)(KL_EVENT_READ | KL_EVENT_WRITE);
                if (kl_event_mod(&s->ev.loop, c->fd, mask, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_READING ||
                       new_state == KL_CONN_READING_BODY) {
                if (kl_event_mod(&s->ev.loop, c->fd,
                                 KL_EVENT_READ, c) < 0) {
                    kl_event_del(&s->ev.loop, c->fd);
                    kl_server_conn_release(s,c);
                }
            } else if (new_state == KL_CONN_SUSPENDED) {
                /* Handler suspended for async I/O — FD already removed
                 * from event loop by kl_async_suspend. */
            } else if (new_state == KL_CONN_CLOSED) {
                kl_event_del(&s->ev.loop, c->fd);
                kl_server_conn_release(s,c);
            }
        }

        /* Sweep for timed-out connections.
         * Single-threaded: no TOCTOU risk — all event processing above is
         * complete, so connection states are stable.  Newly acquired slots
         * have fresh last_active_ms and won't be timed out. */
        now = kl_monotonic_ms();
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
            /* WebSocket: exempt from HTTP idle timeout, check close deadline */
            if (tc->state == KL_CONN_WEBSOCKET) {
                if (kl_ws_server_check_close_timeout(tc, now)) {
                    kl_event_del(&s->ev.loop, tc->fd);
                    kl_server_conn_release(s,tc);
                }
                continue;
            }
            /* HTTP/2: PING keepalive is session's responsibility */
            if (tc->state == KL_CONN_HTTP2) {
                continue;
            }
            /* TLS handshake time counts against read timeout */
            int timed_out = (now - tc->last_active_ms > timeout);
            /* Body deadline: absolute time from body start, not resettable.
             * Catches slow-chunk attacks where 1 byte resets idle timer. */
            if (!timed_out && tc->state == KL_CONN_READING_BODY &&
                tc->body_start_ms > 0 &&
                now - tc->body_start_ms > body_timeout) {
                timed_out = 1;
            }
            if (timed_out) {
                /* Best-effort 408: small write to non-blocking socket
                 * will almost always succeed in one call.
                 * Skip for TLS handshake — no HTTP framing yet. */
                if (tc->state == KL_CONN_READING ||
                    tc->state == KL_CONN_READING_BODY)
                    best_effort_conn_write(tc, kl_408_response,
                                           sizeof(kl_408_response) - 1);
                kl_event_del(&s->ev.loop, tc->fd);
                kl_server_conn_release(s,tc);
            }
        }

        /* Sweep async op deadlines */
        {
            KlAsyncOp *aop = s->async_ops;
            while (aop) {
                KlAsyncOp *next_aop = aop->next;
                if (aop->deadline_ms > 0 && now >= aop->deadline_ms) {
                    if (aop->on_deadline)
                        aop->on_deadline(aop, aop->user_data);
                }
                aop = next_aop;
            }
        }

        /* Graceful drain: stop when all connections are idle or deadline hit */
        if (atomic_load(&s->draining)) {
            /* Send close 1001 to active WebSocket connections */
            for (int j = 0; j < s->pool.capacity; j++) {
                if (s->pool.conns[j].state == KL_CONN_WEBSOCKET)
                    kl_ws_server_drain_close(&s->pool.conns[j]);
                if (s->pool.conns[j].state == KL_CONN_HTTP2)
                    kl_h2_server_drain_shutdown(&s->pool.conns[j]);
            }
            int active = 0;
            for (int j = 0; j < s->pool.capacity; j++) {
                if (s->pool.conns[j].state != KL_CONN_CLOSED)
                    active++;
            }
            if (active == 0 || now >= s->drain_deadline_ms) {
                atomic_store(&s->running, 0);
            }
        }
    }

    /* Restore signal handlers */
    if (s->config.install_signal_handlers) {
        sigaction(SIGTERM, &old_term, NULL);
        sigaction(SIGINT, &old_int, NULL);
        atomic_store(&kl_signal_server, (KlServer *)NULL);
    }

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
}

void kl_server_free(KlServer *s) {
    /* Cancel all active async ops */
    while (s->async_ops) {
        KlAsyncOp *op = s->async_ops;
        s->async_ops = op->next;
        if (op->on_cancel)
            op->on_cancel(op, op->user_data);
        if (op->conn)
            op->conn->async_op = NULL;
    }

    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
    kl_event_ctx_free(&s->ev);
    kl_conn_pool_free(&s->pool);
    kl_router_free(&s->router);
    if (s->config.tls && s->config.tls->ctx_destroy) {
        s->config.tls->ctx_destroy(s->config.tls->ctx);
    }
}
