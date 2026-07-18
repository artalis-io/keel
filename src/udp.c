/* macOS hides IPV6_PKTINFO / IPV6_RECVPKTINFO behind RFC 3542; opt in before
 * <netinet/in.h>. No-op on Linux. */
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

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
    struct sockaddr_storage src;       /* pinned source address, or unset */
    socklen_t               src_len;   /* 0 = no source cmsg */
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

/* Send via sendmsg with a pktinfo control message pinning the source address.
 * Returns -1/EINVAL if the family isn't supported for source pinning. */
static ssize_t udp_send_from(KlUdp *udp, const void *data, size_t len,
                             const struct sockaddr *dest, socklen_t dest_len,
                             const struct sockaddr *src) {
    union {
        struct cmsghdr align;
        unsigned char  buf[CMSG_SPACE(sizeof(struct in6_pktinfo))];
    } control;
    memset(&control, 0, sizeof(control));

    struct iovec iov = { .iov_base = (void *)data, .iov_len = len };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (void *)dest;
    msg.msg_namelen = dest_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;

    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
#if defined(IP_PKTINFO)
    if (src->sa_family == AF_INET) {
        msg.msg_controllen = CMSG_SPACE(sizeof(struct in_pktinfo));
        cm->cmsg_level = IPPROTO_IP;
        cm->cmsg_type = IP_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
        struct in_pktinfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.ipi_spec_dst = ((const struct sockaddr_in *)src)->sin_addr;
        memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
    } else
#endif
    if (src->sa_family == AF_INET6) {
        msg.msg_controllen = CMSG_SPACE(sizeof(struct in6_pktinfo));
        cm->cmsg_level = IPPROTO_IPV6;
        cm->cmsg_type = IPV6_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
        struct in6_pktinfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.ipi6_addr = ((const struct sockaddr_in6 *)src)->sin6_addr;
        memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
    } else {
        errno = EINVAL;
        return -1;
    }

    ssize_t n;
    do { n = sendmsg(udp->fd, &msg, 0); } while (n < 0 && errno == EINTR);
    return n;
}

static ssize_t udp_raw_send(KlUdp *udp, const void *data, size_t len,
                            const struct sockaddr *dest, socklen_t dest_len,
                            const struct sockaddr *src, socklen_t src_len) {
    if (src && src_len)
        return udp_send_from(udp, data, len, dest, dest_len, src);
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
                       const struct sockaddr *dest, socklen_t dest_len,
                       const struct sockaddr *src, socklen_t src_len) {
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
    if (src && src_len) {
        memcpy(&node->src, src, src_len);
        node->src_len = src_len;
    } else {
        node->src_len = 0;
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
                           const struct sockaddr *dest, socklen_t dest_len,
                           const struct sockaddr *src, socklen_t src_len) {
    if (udp->fd < 0) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    /* Preserve ordering: if anything is queued, queue behind it. */
    if (udp->q_head)
        return udp_enqueue(udp, data, len, dest, dest_len, src, src_len);

    ssize_t n = udp_raw_send(udp, data, len, dest, dest_len, src, src_len);
    if (n >= 0)
        return 0;   /* UDP send is all-or-nothing — no short-send handling */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return udp_enqueue(udp, data, len, dest, dest_len, src, src_len);

    udp->last_error = KL_ERR_IO;
    return -1;
}

static void udp_flush_queue(KlUdp *udp) {
    int had_data = (udp->q_head != NULL);
    while (udp->q_head) {
        KlUdpDatagram *node = udp->q_head;
        ssize_t n = udp_raw_send(udp, node->data, node->len,
                                 node->dest_len ? (struct sockaddr *)&node->dest : NULL,
                                 node->dest_len,
                                 node->src_len ? (struct sockaddr *)&node->src : NULL,
                                 node->src_len);
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

/* Extract the datagram's local (destination) address from pktinfo control
 * messages into udp->recv_local. Returns its length, or 0 if none present. */
static socklen_t udp_parse_local(KlUdp *udp, struct msghdr *msg) {
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
#if defined(IP_PKTINFO)
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo pi;
            memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in *s4 = (struct sockaddr_in *)&udp->recv_local;
            memset(s4, 0, sizeof(*s4));
            s4->sin_family = AF_INET;
            s4->sin_addr = pi.ipi_addr;
            return sizeof(*s4);
        }
#endif
        if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_PKTINFO) {
            struct in6_pktinfo pi;
            memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&udp->recv_local;
            memset(s6, 0, sizeof(*s6));
            s6->sin6_family = AF_INET6;
            s6->sin6_addr = pi.ipi6_addr;
            return sizeof(*s6);
        }
    }
    return 0;
}

static void udp_recv_drain(KlUdp *udp) {
    union {
        struct cmsghdr align;
        unsigned char  buf[CMSG_SPACE(sizeof(struct in6_pktinfo))];
    } control;

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
        if (udp->pktinfo) {
            msg.msg_control = control.buf;
            msg.msg_controllen = sizeof(control.buf);
        }

        ssize_t n;
        do { n = recvmsg(udp->fd, &msg, 0); } while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                udp->last_error = KL_ERR_IO;
            break;   /* drained (or error) */
        }
        if (msg.msg_flags & MSG_TRUNC)
            udp->truncated++;

        socklen_t local_len = udp->pktinfo ? udp_parse_local(udp, &msg) : 0;

        if (udp->on_recv)
            udp->on_recv(udp, udp->recv_buf, (size_t)n,
                         (struct sockaddr *)&udp->recv_src, msg.msg_namelen,
                         local_len ? (struct sockaddr *)&udp->recv_local : NULL,
                         local_len, udp->recv_ud);

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

    /* Enable per-datagram local-address capture (best-effort per platform). */
    if (cfg->recv_pktinfo) {
        int opt = 1;
        (void)opt;   /* unused on platforms lacking every pktinfo option */
        if (family == AF_INET) {
#if defined(IP_PKTINFO)
            if (setsockopt(udp->fd, IPPROTO_IP, IP_PKTINFO, &opt, sizeof(opt)) == 0)
                udp->pktinfo = 1;
#elif defined(IP_RECVPKTINFO)
            if (setsockopt(udp->fd, IPPROTO_IP, IP_RECVPKTINFO, &opt, sizeof(opt)) == 0)
                udp->pktinfo = 1;
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_RECVPKTINFO)
            if (setsockopt(udp->fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &opt, sizeof(opt)) == 0)
                udp->pktinfo = 1;
#endif
        }
    }

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
    return kl_udp_send_to_from(udp, data, len, dest, dest_len, NULL, 0);
}

int kl_udp_send_to_from(KlUdp *udp, const void *data, size_t len,
                        const struct sockaddr *dest, socklen_t dest_len,
                        const struct sockaddr *src, socklen_t src_len) {
    if (!udp || (!data && len) || !dest || dest_len == 0 ||
        dest_len > sizeof(struct sockaddr_storage) ||
        (src && src_len > sizeof(struct sockaddr_storage))) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_send_common(udp, data, len, dest, dest_len, src, src_len);
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
    return udp_send_common(udp, data, len, NULL, 0, NULL, 0);
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
