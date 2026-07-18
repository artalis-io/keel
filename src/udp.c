#include <keel/udp.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* Whole-datagram FIFO node: header + inline payload (single allocation). */
struct KlUdpDatagram {
    struct KlUdpDatagram   *next;
    struct sockaddr_storage dest;      /* destination for sendto */
    socklen_t               dest_len;  /* 0 = connected send (use send()) */
    size_t                  len;       /* payload length */
    unsigned char           data[];    /* payload */
};

/* ── Socket flag helpers (portable; macOS lacks SOCK_CLOEXEC on socket()) ── */

static int udp_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void udp_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* ── Forward decls ────────────────────────────────────────────────────── */

static void udp_on_ready(int fd, KlEventMask ready, void *user_data);
static void udp_recv_drain(KlUdp *udp);
static void udp_flush_queue(KlUdp *udp);

/* Reconcile event-loop interest with current recv/send state. */
static void udp_update_interest(KlUdp *udp) {
    if (udp->fd < 0)
        return;
    KlEventMask want = 0;
    if (udp->recv_active) want |= KL_EVENT_READ;
    if (udp->q_head)      want |= KL_EVENT_WRITE;

    if (want == udp->want_mask)
        return;

    if (want == 0) {
        kl_watcher_del(udp->ctx, udp->fd);
        udp->watcher_added = 0;
        udp->want_mask = 0;
    } else if (!udp->watcher_added) {
        if (kl_watcher_add(udp->ctx, udp->fd, want, udp_on_ready, udp) == 0) {
            udp->watcher_added = 1;
            udp->want_mask = want;
        } else {
            udp->last_error = KL_ERR_EVENT_ADD;
        }
    } else {
        if (kl_watcher_mod(udp->ctx, udp->fd, want) == 0)
            udp->want_mask = want;
        else
            udp->last_error = KL_ERR_EVENT_ADD;
    }
}

/* ── Send path ────────────────────────────────────────────────────────── */

static ssize_t udp_raw_send(KlUdp *udp, const void *data, size_t len,
                            const struct sockaddr *dest, socklen_t dest_len) {
    ssize_t n;
    do {
        if (dest_len == 0)
            n = send(udp->fd, data, len, 0);
        else
            n = sendto(udp->fd, data, len, 0, dest, dest_len);
    } while (n < 0 && errno == EINTR);
    return n;
}

static int udp_enqueue(KlUdp *udp, const void *data, size_t len,
                       const struct sockaddr *dest, socklen_t dest_len) {
    /* Backpressure cap (avoids max_send_queue - len underflow). */
    if (len > udp->max_send_queue || udp->q_bytes > udp->max_send_queue - len) {
        udp->dropped++;
        udp->last_error = KL_ERR_QUEUE_FULL;
        return -1;
    }
    if (len > SIZE_MAX - sizeof(KlUdpDatagram)) {
        udp->dropped++;
        udp->last_error = KL_ERR_OVERFLOW;
        return -1;
    }

    size_t node_sz = sizeof(KlUdpDatagram) + len;
    KlUdpDatagram *node = kl_malloc(udp->alloc, node_sz);
    if (!node) {
        udp->last_error = KL_ERR_ALLOC;
        return -1;
    }
    node->next = NULL;
    node->len = len;
    if (dest && dest_len) {
        memcpy(&node->dest, dest, dest_len);
        node->dest_len = dest_len;
    } else {
        node->dest_len = 0;
    }
    if (len)
        memcpy(node->data, data, len);

    if (udp->q_tail) udp->q_tail->next = node;
    else             udp->q_head = node;
    udp->q_tail = node;
    udp->q_bytes += len;

    udp_update_interest(udp);   /* arm WRITE */
    return 0;
}

static int udp_send_common(KlUdp *udp, const void *data, size_t len,
                           const struct sockaddr *dest, socklen_t dest_len) {
    if (udp->fd < 0) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    /* Preserve ordering: if anything is queued, queue behind it. */
    if (udp->q_head)
        return udp_enqueue(udp, data, len, dest, dest_len);

    ssize_t n = udp_raw_send(udp, data, len, dest, dest_len);
    if (n >= 0)
        return 0;   /* UDP send is all-or-nothing — no short-send handling */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return udp_enqueue(udp, data, len, dest, dest_len);

    udp->last_error = KL_ERR_IO;
    return -1;
}

static void udp_flush_queue(KlUdp *udp) {
    int had_data = (udp->q_head != NULL);
    while (udp->q_head) {
        KlUdpDatagram *node = udp->q_head;
        ssize_t n = udp_raw_send(udp, node->data, node->len,
                                 node->dest_len ? (struct sockaddr *)&node->dest : NULL,
                                 node->dest_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;                 /* socket buffer still full — retry later */
            /* Hard error (e.g. ECONNREFUSED on a connected socket): drop it. */
            udp->last_error = KL_ERR_IO;
            udp->dropped++;
        }
        udp->q_head = node->next;
        if (!udp->q_head) udp->q_tail = NULL;
        udp->q_bytes -= node->len;
        kl_free(udp->alloc, node, sizeof(KlUdpDatagram) + node->len);
    }
    udp_update_interest(udp);   /* drop WRITE once drained */
    if (had_data && udp->q_head == NULL && udp->on_drain)
        udp->on_drain(udp, udp->drain_ud);
}

/* ── Receive path ─────────────────────────────────────────────────────── */

static void udp_recv_drain(KlUdp *udp) {
    for (;;) {
        struct msghdr msg;
        struct iovec  iov;
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = udp->recv_buf;
        iov.iov_len  = udp->recv_buf_size;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_name = &udp->recv_src;
        msg.msg_namelen = sizeof(udp->recv_src);

        ssize_t n;
        do { n = recvmsg(udp->fd, &msg, 0); } while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                udp->last_error = KL_ERR_IO;
            break;   /* drained (or error) */
        }
        if (msg.msg_flags & MSG_TRUNC)
            udp->truncated++;

        if (udp->on_recv)
            udp->on_recv(udp, udp->recv_buf, (size_t)n,
                         (struct sockaddr *)&udp->recv_src, msg.msg_namelen,
                         udp->recv_ud);

        /* The callback may have called recv_stop() or free() — re-check. */
        if (!udp->recv_active || udp->fd < 0)
            break;
    }
}

static void udp_on_ready(int fd, KlEventMask ready, void *user_data) {
    (void)fd;
    KlUdp *udp = user_data;
    if (ready & KL_EVENT_WRITE)
        udp_flush_queue(udp);
    if (udp->fd >= 0 && udp->recv_active && (ready & KL_EVENT_READ))
        udp_recv_drain(udp);
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

int kl_udp_init(KlUdp *udp, const KlUdpConfig *cfg) {
    if (!udp)
        return -1;
    memset(udp, 0, sizeof(*udp));
    udp->fd = -1;

    if (!cfg || !cfg->ctx) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    udp->ctx = cfg->ctx;
    udp->alloc = cfg->alloc ? cfg->alloc : cfg->ctx->alloc;
    if (!udp->alloc) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }

    size_t rbs = cfg->recv_buf_size ? cfg->recv_buf_size : 2048;
    if (rbs > 65535) rbs = 65535;
    udp->recv_buf_size = rbs;
    udp->max_send_queue = cfg->max_send_queue ? cfg->max_send_queue : (256u * 1024u);

    int family = cfg->family;
    struct addrinfo *ai = NULL;
    if (cfg->bind_addr) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = (family == AF_INET || family == AF_INET6) ? family : AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)cfg->bind_port);
        int rc = getaddrinfo(cfg->bind_addr, port_str, &hints, &ai);
        if (rc != 0 || !ai) {
            udp->last_error = KL_ERR_DNS;
            return -1;
        }
        family = ai->ai_family;
    } else if (family != AF_INET && family != AF_INET6) {
        family = AF_INET;
    }

    udp->fd = socket(family, SOCK_DGRAM, 0);
    if (udp->fd < 0) {
        udp->last_error = KL_ERR_SOCKET;
        goto fail;
    }
    udp_set_cloexec(udp->fd);
    if (udp_set_nonblocking(udp->fd) < 0) {
        udp->last_error = KL_ERR_SOCKET;
        goto fail;
    }

    if (cfg->reuse_addr) {
        int opt = 1;
        (void)setsockopt(udp->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }
#ifdef SO_REUSEPORT
    if (cfg->reuse_port) {
        int opt = 1;
        (void)setsockopt(udp->fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }
#endif

    if (ai) {
        if (bind(udp->fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            udp->last_error = KL_ERR_BIND;
            goto fail;
        }
        freeaddrinfo(ai);
        ai = NULL;
    }

    udp->recv_buf = kl_malloc(udp->alloc, udp->recv_buf_size);
    if (!udp->recv_buf) {
        udp->last_error = KL_ERR_ALLOC;
        goto fail;
    }
    return 0;

fail:
    if (ai) freeaddrinfo(ai);
    if (udp->fd >= 0) { close(udp->fd); udp->fd = -1; }
    return -1;
}

void kl_udp_free(KlUdp *udp) {
    if (!udp)
        return;
    udp->recv_active = 0;
    if (udp->watcher_added && udp->fd >= 0)
        kl_watcher_del(udp->ctx, udp->fd);
    udp->watcher_added = 0;
    udp->want_mask = 0;

    KlUdpDatagram *node = udp->q_head;
    while (node) {
        KlUdpDatagram *next = node->next;
        kl_free(udp->alloc, node, sizeof(KlUdpDatagram) + node->len);
        node = next;
    }
    udp->q_head = udp->q_tail = NULL;
    udp->q_bytes = 0;

    if (udp->recv_buf) {
        kl_free(udp->alloc, udp->recv_buf, udp->recv_buf_size);
        udp->recv_buf = NULL;
    }
    if (udp->fd >= 0) {
        close(udp->fd);
        udp->fd = -1;
    }
}

int kl_udp_connect(KlUdp *udp, const struct sockaddr *peer, socklen_t peer_len) {
    if (!udp || udp->fd < 0 || !peer || peer_len == 0) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (connect(udp->fd, peer, peer_len) < 0) {
        udp->last_error = KL_ERR_CONNECT;
        return -1;
    }
    udp->connected = 1;
    return 0;
}

int kl_udp_recv_start(KlUdp *udp, KlUdpRecvFn on_recv, void *user_data) {
    if (!udp || udp->fd < 0 || !on_recv) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    udp->on_recv = on_recv;
    udp->recv_ud = user_data;
    udp->recv_active = 1;
    udp_update_interest(udp);
    if (!(udp->want_mask & KL_EVENT_READ)) {   /* watcher registration failed */
        udp->recv_active = 0;
        return -1;
    }
    return 0;
}

void kl_udp_recv_stop(KlUdp *udp) {
    if (!udp)
        return;
    udp->recv_active = 0;
    udp_update_interest(udp);
}

int kl_udp_send_to(KlUdp *udp, const void *data, size_t len,
                   const struct sockaddr *dest, socklen_t dest_len) {
    if (!udp || (!data && len) || !dest || dest_len == 0 ||
        dest_len > sizeof(struct sockaddr_storage)) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_send_common(udp, data, len, dest, dest_len);
}

int kl_udp_send(KlUdp *udp, const void *data, size_t len) {
    if (!udp || (!data && len)) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (!udp->connected) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_send_common(udp, data, len, NULL, 0);
}

void kl_udp_on_drain(KlUdp *udp, KlUdpDrainFn cb, void *user_data) {
    if (!udp) return;
    udp->on_drain = cb;
    udp->drain_ud = user_data;
}

size_t   kl_udp_send_queued(const KlUdp *udp) { return udp ? udp->q_bytes : 0; }
uint64_t kl_udp_dropped(const KlUdp *udp)     { return udp ? udp->dropped : 0; }
uint64_t kl_udp_truncated(const KlUdp *udp)   { return udp ? udp->truncated : 0; }
int      kl_udp_fd(const KlUdp *udp)          { return udp ? udp->fd : -1; }
KlError  kl_udp_last_error(const KlUdp *udp)  { return udp ? udp->last_error : KL_ERR_INVALID_ARG; }

uint16_t kl_udp_local_port(const KlUdp *udp) {
    if (!udp || udp->fd < 0)
        return 0;
    struct sockaddr_storage sa;
    memset(&sa, 0, sizeof(sa));
    socklen_t len = sizeof(sa);
    if (getsockname(udp->fd, (struct sockaddr *)&sa, &len) != 0)
        return 0;
    if (sa.ss_family == AF_INET)
        return ntohs(((struct sockaddr_in *)&sa)->sin_port);
    if (sa.ss_family == AF_INET6)
        return ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);
    return 0;
}
