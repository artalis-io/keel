#include <keel/server.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include "internal.h"

#define KL_LISTEN_BACKLOG  128
#define KL_EVENTS_PER_TICK 64
#define KL_POLL_TIMEOUT_MS 1000

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

int kl_server_init(KlServer *s, KlConfig *config) {
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
    }

    return 0;
}

int kl_server_route(KlServer *s, const char *method, const char *pattern,
                    KlHandler handler, void *user_data,
                    KlBodyReaderFactory body_reader) {
    return kl_router_add(&s->router, method, pattern, handler, user_data,
                         body_reader);
}

int kl_server_run(KlServer *s) {
    KlAllocator *alloc = &s->alloc_storage;

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Create listen socket */
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        perror("setsockopt SO_REUSEADDR");
#ifdef SO_REUSEPORT
    (void)setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)s->config.port),
    };
    if (inet_pton(AF_INET, s->config.bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "keel: invalid bind address '%s'\n",
                s->config.bind_addr);
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    if (listen(s->listen_fd, KL_LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    if (set_nonblocking(s->listen_fd) < 0) {
        perror("fcntl");
        close(s->listen_fd);
        s->listen_fd = -1;
        return -1;
    }

    /* Init event loop */
    s->loop.alloc = alloc;
    if (kl_event_init(&s->loop) < 0) {
        perror("event_init");
        close(s->listen_fd);
        return -1;
    }

    /* Register listen socket for read events */
    if (kl_event_add(&s->loop, s->listen_fd, KL_EVENT_READ, NULL) < 0) {
        perror("event_add listen");
        close(s->listen_fd);
        kl_event_close(&s->loop);
        return -1;
    }

    fprintf(stderr, "keel: listening on %s:%d\n",
            s->config.bind_addr, s->config.port);

    s->running = 1;
    KlEvent events[KL_EVENTS_PER_TICK];

    while (s->running) {
        int n = kl_event_wait(&s->loop, events, KL_EVENTS_PER_TICK,
                              KL_POLL_TIMEOUT_MS);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("event_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            KlConn *c = events[i].udata;

            if (c == NULL) {
                /* Listen socket — accept new connections */
                while (1) {
                    int client_fd = accept(s->listen_fd, NULL, NULL);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
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
                        continue;
                    }

                    /* Set allocator on connection's response.
                     * This is set once on accept; response reuses it across keep-alive. */
                    nc->res.alloc = alloc;

                    if (kl_event_add(&s->loop, client_fd,
                                     KL_EVENT_READ, nc) < 0) {
                        kl_conn_release(&s->pool, nc);
                        continue;
                    }
                }
                continue;
            }

            /* Client connection event */
            KlConnState new_state = c->state;

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

            /* Transition */
            if (new_state == KL_CONN_SENDING) {
                if (kl_event_mod(&s->loop, c->fd,
                                 KL_EVENT_WRITE, c) < 0) {
                    kl_event_del(&s->loop, c->fd);
                    kl_conn_release(&s->pool, c);
                }
            } else if (new_state == KL_CONN_READING ||
                       new_state == KL_CONN_READING_BODY) {
                if (kl_event_mod(&s->loop, c->fd,
                                 KL_EVENT_READ, c) < 0) {
                    kl_event_del(&s->loop, c->fd);
                    kl_conn_release(&s->pool, c);
                }
            } else if (new_state == KL_CONN_CLOSED) {
                kl_event_del(&s->loop, c->fd);
                kl_conn_release(&s->pool, c);
            }
        }

        /* Sweep for timed-out connections.
         * Single-threaded: no TOCTOU risk — all event processing above is
         * complete, so connection states are stable.  Newly acquired slots
         * have fresh last_active_ms and won't be timed out. */
        uint64_t now = kl_monotonic_ms();
        uint64_t timeout = (uint64_t)s->config.read_timeout_ms;
        for (int i = 0; i < s->pool.capacity; i++) {
            KlConn *tc = &s->pool.conns[i];
            if (tc->state == KL_CONN_CLOSED || tc->state == KL_CONN_PROCESSING)
                continue;
            if (now - tc->last_active_ms > timeout) {
                /* Best-effort 408: small write to non-blocking socket
                 * will almost always succeed in one call. */
                if (tc->state == KL_CONN_READING ||
                    tc->state == KL_CONN_READING_BODY)
                    best_effort_write(tc->fd, kl_408_response,
                                      sizeof(kl_408_response) - 1);
                kl_event_del(&s->loop, tc->fd);
                kl_conn_release(&s->pool, tc);
            }
        }
    }

    return 0;
}

void kl_server_stop(KlServer *s) {
    s->running = 0;
}

void kl_server_free(KlServer *s) {
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
    kl_event_close(&s->loop);
    kl_conn_pool_free(&s->pool);
    kl_router_free(&s->router);
}
