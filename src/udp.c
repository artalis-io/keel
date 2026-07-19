/* glibc gates struct in{,6}_pktinfo + IP_PKTINFO / IPV6_PKTINFO behind
 * _GNU_SOURCE; macOS hides the v6 ones behind RFC 3542. Opt into both before
 * any system header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

/* Some platforms spell the IPv6 group ops the "membership" way. */
#if !defined(IPV6_JOIN_GROUP) && defined(IPV6_ADD_MEMBERSHIP)
#define IPV6_JOIN_GROUP  IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#endif

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

/* ── recv/sendmmsg batching (Linux) ───────────────────────────────────── */

#define UDP_MMSG_DEFAULT 16
#define UDP_MMSG_MAX      64

#if defined(__linux__)
/* RX batch: N slots for one recvmmsg call. Data is one contiguous block sliced
 * per slot; each slot has its own source addr + control buffer (pktinfo). */
typedef struct {
    struct mmsghdr          *msgs;
    struct iovec            *iov;
    struct sockaddr_storage *src;
    unsigned char           *data;     /* n * bufsz */
    unsigned char           *ctrl;     /* n * ctrl_sz */
    size_t                   bufsz;
    size_t                   ctrl_sz;
    int                      n;
} UdpRxBatch;

/* TX batch: N msghdrs for one sendmmsg call; payloads live in the queue nodes,
 * each slot carries its own control buffer for a source-pinning pktinfo cmsg. */
typedef struct {
    struct mmsghdr *msgs;
    struct iovec   *iov;
    unsigned char  *ctrl;     /* n * ctrl_sz */
    size_t          ctrl_sz;
    int             n;
} UdpTxBatch;

static void udp_rx_batch_free(KlAllocator *a, UdpRxBatch *b) {
    if (!b) return;
    kl_free(a, b->msgs, (size_t)b->n * sizeof(struct mmsghdr));
    kl_free(a, b->iov,  (size_t)b->n * sizeof(struct iovec));
    kl_free(a, b->src,  (size_t)b->n * sizeof(struct sockaddr_storage));
    kl_free(a, b->data, (size_t)b->n * b->bufsz);
    kl_free(a, b->ctrl, (size_t)b->n * b->ctrl_sz);
    kl_free(a, b, sizeof(*b));
}

static UdpRxBatch *udp_rx_batch_new(KlAllocator *a, int n, size_t bufsz) {
    UdpRxBatch *b = kl_malloc(a, sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->n = n;
    b->bufsz = bufsz;
    b->ctrl_sz = CMSG_SPACE(sizeof(struct in6_pktinfo));
    b->msgs = kl_malloc(a, (size_t)n * sizeof(struct mmsghdr));
    b->iov  = kl_malloc(a, (size_t)n * sizeof(struct iovec));
    b->src  = kl_malloc(a, (size_t)n * sizeof(struct sockaddr_storage));
    b->data = kl_malloc(a, (size_t)n * bufsz);
    b->ctrl = kl_malloc(a, (size_t)n * b->ctrl_sz);
    if (!b->msgs || !b->iov || !b->src || !b->data || !b->ctrl) {
        udp_rx_batch_free(a, b);
        return NULL;
    }
    return b;
}

static void udp_tx_batch_free(KlAllocator *a, UdpTxBatch *b) {
    if (!b) return;
    kl_free(a, b->msgs, (size_t)b->n * sizeof(struct mmsghdr));
    kl_free(a, b->iov,  (size_t)b->n * sizeof(struct iovec));
    kl_free(a, b->ctrl, (size_t)b->n * b->ctrl_sz);
    kl_free(a, b, sizeof(*b));
}

static UdpTxBatch *udp_tx_batch_new(KlAllocator *a, int n) {
    UdpTxBatch *b = kl_malloc(a, sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->n = n;
    b->ctrl_sz = CMSG_SPACE(sizeof(struct in6_pktinfo));
    b->msgs = kl_malloc(a, (size_t)n * sizeof(struct mmsghdr));
    b->iov  = kl_malloc(a, (size_t)n * sizeof(struct iovec));
    b->ctrl = kl_malloc(a, (size_t)n * b->ctrl_sz);
    if (!b->msgs || !b->iov || !b->ctrl) {
        udp_tx_batch_free(a, b);
        return NULL;
    }
    return b;
}

/* Write a source-pinning pktinfo cmsg for `src` into `buf`; returns the
 * msg_controllen to use, or 0 on an unsupported family. */
static size_t udp_build_pktinfo(unsigned char *buf, size_t bufsz,
                                const struct sockaddr *src) {
    struct msghdr tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.msg_control = buf;
    tmp.msg_controllen = bufsz;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&tmp);
    if (!cm) return 0;
#if defined(IP_PKTINFO)
    if (src->sa_family == AF_INET) {
        cm->cmsg_level = IPPROTO_IP;
        cm->cmsg_type = IP_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
        struct in_pktinfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.ipi_spec_dst = ((const struct sockaddr_in *)src)->sin_addr;
        memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
        return CMSG_SPACE(sizeof(struct in_pktinfo));
    }
#endif
    if (src->sa_family == AF_INET6) {
        cm->cmsg_level = IPPROTO_IPV6;
        cm->cmsg_type = IPV6_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
        struct in6_pktinfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.ipi6_addr = ((const struct sockaddr_in6 *)src)->sin6_addr;
        memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
        return CMSG_SPACE(sizeof(struct in6_pktinfo));
    }
    return 0;
}
#endif /* __linux__ */

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
    msg.msg_controllen = sizeof(control.buf);   /* must be set before CMSG_FIRSTHDR */

    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm) {
        errno = EINVAL;
        return -1;
    }
#if defined(IP_PKTINFO)
    if (src->sa_family == AF_INET) {
        cm->cmsg_level = IPPROTO_IP;
        cm->cmsg_type = IP_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
        struct in_pktinfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.ipi_spec_dst = ((const struct sockaddr_in *)src)->sin_addr;
        memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
        msg.msg_controllen = CMSG_SPACE(sizeof(struct in_pktinfo));
    } else
#endif
    if (src->sa_family == AF_INET6) {
        cm->cmsg_level = IPPROTO_IPV6;
        cm->cmsg_type = IPV6_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
        struct in6_pktinfo pi;
        memset(&pi, 0, sizeof(pi));
        pi.ipi6_addr = ((const struct sockaddr_in6 *)src)->sin6_addr;
        memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
        msg.msg_controllen = CMSG_SPACE(sizeof(struct in6_pktinfo));
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

#if defined(__linux__)
/* Dequeue+free the first `k` queued datagrams (already sent or dropped). */
static void udp_dequeue_front(KlUdp *udp, KlUdpDatagram **nodes, int k) {
    for (int i = 0; i < k; i++) {
        KlUdpDatagram *node = nodes[i];
        udp->q_head = node->next;
        if (!udp->q_head) udp->q_tail = NULL;
        udp->q_bytes -= node->len;
        kl_free(udp->alloc, node, sizeof(KlUdpDatagram) + node->len);
    }
}

/* Drain the send queue in sendmmsg batches. */
static void udp_flush_queue_batched(KlUdp *udp) {
    UdpTxBatch *b = udp->tx_batch;
    int had_data = (udp->q_head != NULL);

    while (udp->q_head) {
        KlUdpDatagram *nodes[UDP_MMSG_MAX];
        int cnt = 0;
        for (KlUdpDatagram *node = udp->q_head; node && cnt < b->n;
             node = node->next, cnt++) {
            nodes[cnt] = node;
            b->iov[cnt].iov_base = node->data;
            b->iov[cnt].iov_len  = node->len;
            struct msghdr *m = &b->msgs[cnt].msg_hdr;
            memset(m, 0, sizeof(*m));
            m->msg_iov = &b->iov[cnt];
            m->msg_iovlen = 1;
            if (node->dest_len) {
                m->msg_name = &node->dest;
                m->msg_namelen = node->dest_len;
            }
            if (node->src_len) {
                unsigned char *ctrl = b->ctrl + (size_t)cnt * b->ctrl_sz;
                size_t clen = udp_build_pktinfo(ctrl, b->ctrl_sz,
                                                (struct sockaddr *)&node->src);
                if (clen) { m->msg_control = ctrl; m->msg_controllen = clen; }
            }
            b->msgs[cnt].msg_len = 0;
        }

        int sent;
        do { sent = sendmmsg(udp->fd, b->msgs, (unsigned)cnt, 0); }
        while (sent < 0 && errno == EINTR);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;               /* socket buffer full — retry later */
            /* Hard error on the head datagram: drop it and continue. */
            udp->last_error = KL_ERR_IO;
            udp->dropped++;
            udp_dequeue_front(udp, nodes, 1);
            continue;
        }
        udp_dequeue_front(udp, nodes, sent);   /* sent >= 1 here */
        if (sent < cnt)
            break;                   /* next message would block — retry later */
    }

    udp_update_interest(udp);
    if (had_data && udp->q_head == NULL && udp->on_drain)
        udp->on_drain(udp, udp->drain_ud);
}
#endif /* __linux__ */

static void udp_flush_queue(KlUdp *udp) {
#if defined(__linux__)
    if (udp->tx_batch) { udp_flush_queue_batched(udp); return; }
#endif
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
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
            struct in_pktinfo pi;
            memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in *s4 = (struct sockaddr_in *)&udp->recv_local;
            memset(s4, 0, sizeof(*s4));
            s4->sin_family = AF_INET;
            s4->sin_addr = pi.ipi_addr;
            return sizeof(*s4);
        }
#endif
        if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_PKTINFO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
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

#if defined(__linux__)
/* Drain the socket in recvmmsg batches, firing on_recv per datagram. */
static void udp_recv_drain_batched(KlUdp *udp) {
    UdpRxBatch *b = udp->rx_batch;
    for (;;) {
        /* (Re)init the msghdrs each round — the kernel mutates
         * msg_len/flags/namelen/controllen. */
        for (int i = 0; i < b->n; i++) {
            b->iov[i].iov_base = b->data + (size_t)i * b->bufsz;
            b->iov[i].iov_len  = b->bufsz;
            struct msghdr *m = &b->msgs[i].msg_hdr;
            memset(m, 0, sizeof(*m));
            m->msg_iov = &b->iov[i];
            m->msg_iovlen = 1;
            m->msg_name = &b->src[i];
            m->msg_namelen = sizeof(struct sockaddr_storage);
            if (udp->pktinfo) {
                m->msg_control = b->ctrl + (size_t)i * b->ctrl_sz;
                m->msg_controllen = b->ctrl_sz;
            }
            b->msgs[i].msg_len = 0;
        }

        int cnt;
        do { cnt = recvmmsg(udp->fd, b->msgs, (unsigned)b->n, 0, NULL); }
        while (cnt < 0 && errno == EINTR);
        if (cnt < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                udp->last_error = KL_ERR_IO;
            break;
        }
        if (cnt == 0)
            break;

        for (int i = 0; i < cnt; i++) {
            struct msghdr *m = &b->msgs[i].msg_hdr;
            if (m->msg_flags & MSG_TRUNC)
                udp->truncated++;
            socklen_t local_len = udp->pktinfo ? udp_parse_local(udp, m) : 0;
            if (udp->on_recv)
                udp->on_recv(udp, b->iov[i].iov_base, (size_t)b->msgs[i].msg_len,
                             (struct sockaddr *)&b->src[i], m->msg_namelen,
                             local_len ? (struct sockaddr *)&udp->recv_local : NULL,
                             local_len, udp->recv_ud);
            /* The callback may have called recv_stop() or free() — re-check. */
            if (!udp->recv_active || udp->fd < 0)
                return;
        }
        if (cnt < b->n)
            break;   /* fewer than a full batch → socket drained */
    }
}
#endif /* __linux__ */

static void udp_recv_drain(KlUdp *udp) {
#if defined(__linux__)
    if (udp->rx_batch) { udp_recv_drain_batched(udp); return; }
#endif
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

/* ── Multicast group membership ───────────────────────────────────────── */

/* Join (join=1) or leave (join=0) an any-source multicast group. Validates the
 * address is multicast and matches the socket family. Returns 0 / -1 (last_error). */
static int udp_mcast_membership(KlUdp *udp, const char *group,
                                unsigned iface_index, int join) {
    if (udp->family == AF_INET) {
        struct in_addr maddr;
        if (inet_pton(AF_INET, group, &maddr) != 1 ||
            (ntohl(maddr.s_addr) >> 28) != 0xE) {   /* not in 224.0.0.0/4 */
            udp->last_error = KL_ERR_INVALID_ARG;
            return -1;
        }
#if defined(IP_ADD_MEMBERSHIP)
#if defined(__linux__)
        struct ip_mreqn mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = maddr;
        mreq.imr_address.s_addr = htonl(INADDR_ANY);
        mreq.imr_ifindex = (int)iface_index;
#else
        (void)iface_index;   /* IPv4 by-index unsupported here; default interface */
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = maddr;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
#endif
        if (setsockopt(udp->fd, IPPROTO_IP,
                       join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                       &mreq, sizeof(mreq)) != 0) {
            udp->last_error = KL_ERR_SOCKET;
            return -1;
        }
        return 0;
#else
        udp->last_error = KL_ERR_SOCKET;   /* multicast unsupported on this build */
        return -1;
#endif
    } else if (udp->family == AF_INET6) {
        struct in6_addr maddr;
        if (inet_pton(AF_INET6, group, &maddr) != 1 || maddr.s6_addr[0] != 0xff) {
            udp->last_error = KL_ERR_INVALID_ARG;
            return -1;
        }
#if defined(IPV6_JOIN_GROUP)
        struct ipv6_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.ipv6mr_multiaddr = maddr;
        mreq.ipv6mr_interface = iface_index;
        if (setsockopt(udp->fd, IPPROTO_IPV6,
                       join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                       &mreq, sizeof(mreq)) != 0) {
            udp->last_error = KL_ERR_SOCKET;
            return -1;
        }
        return 0;
#else
        udp->last_error = KL_ERR_SOCKET;
        return -1;
#endif
    }
    udp->last_error = KL_ERR_INVALID_ARG;
    return -1;
}

/* Apply multicast/broadcast transmit options (best-effort). */
static void udp_apply_tx_options(int fd, int family, const KlUdpConfig *cfg) {
    if (cfg->broadcast) {
        int opt = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    }
    if (family == AF_INET) {
#if defined(IP_MULTICAST_TTL)
        if (cfg->multicast_ttl > 0) {
#if defined(__linux__)
            int ttl = cfg->multicast_ttl;
#else
            unsigned char ttl = (unsigned char)cfg->multicast_ttl;
#endif
            (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        }
#endif
#if defined(IP_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) {
#if defined(__linux__)
            int off = 0;
#else
            unsigned char off = 0;
#endif
            (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &off, sizeof(off));
        }
#endif
#if defined(__linux__) && defined(IP_MULTICAST_IF)
        if (cfg->multicast_iface > 0) {
            struct ip_mreqn mr;
            memset(&mr, 0, sizeof(mr));
            mr.imr_ifindex = (int)cfg->multicast_iface;
            (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mr, sizeof(mr));
        }
#endif
    } else if (family == AF_INET6) {
#if defined(IPV6_MULTICAST_HOPS)
        if (cfg->multicast_ttl > 0) {
            int hops = cfg->multicast_ttl;
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
        }
#endif
#if defined(IPV6_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) {
            int off = 0;
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &off, sizeof(off));
        }
#endif
#if defined(IPV6_MULTICAST_IF)
        if (cfg->multicast_iface > 0) {
            unsigned idx = cfg->multicast_iface;
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof(idx));
        }
#endif
    }
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

    /* recv/sendmmsg batch size: 0 = default, clamp to [1, UDP_MMSG_MAX]. */
    int batch = cfg->mmsg_batch > 0 ? cfg->mmsg_batch : UDP_MMSG_DEFAULT;
    if (batch > UDP_MMSG_MAX) batch = UDP_MMSG_MAX;
    if (batch < 1) batch = 1;
    udp->mmsg_batch = batch;

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
    udp->family = family;
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

    /* Kernel socket-buffer sizing (best-effort; the kernel clamps to its
     * rmem_max/wmem_max and may round/double the value). */
    if (cfg->so_rcvbuf > 0) {
        int v = cfg->so_rcvbuf;
        (void)setsockopt(udp->fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
    }
    if (cfg->so_sndbuf > 0) {
        int v = cfg->so_sndbuf;
        (void)setsockopt(udp->fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
    }

    /* Broadcast + multicast transmit options (best-effort). */
    udp_apply_tx_options(udp->fd, family, cfg);

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

    /* Optional init-time multicast join (requires a bound socket). */
    if (cfg->multicast_group && cfg->multicast_group[0]) {
        int mrc = udp_mcast_membership(udp, cfg->multicast_group,
                                       cfg->multicast_iface, 1);
        /* cppcheck-suppress knownConditionTrueFalse ; false positive: the helper
         * returns 0 on real platforms (where IP_ADD_MEMBERSHIP is defined), and
         * only always -1 in cppcheck's synthetic no-multicast-macro config. */
        if (mrc != 0)
            goto fail;   /* last_error set by helper */
    }

    udp->recv_buf = kl_malloc(udp->alloc, udp->recv_buf_size);
    if (!udp->recv_buf) {
        udp->last_error = KL_ERR_ALLOC;
        goto fail;
    }

#if defined(__linux__)
    /* Allocate the (small) sendmmsg batch upfront; the recvmmsg batch (which
     * carries the large data block) is allocated lazily in kl_udp_recv_start.
     * Allocation failure just disables batching — it is an optimization. */
    if (batch > 1)
        udp->tx_batch = udp_tx_batch_new(udp->alloc, batch);
#endif
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
#if defined(__linux__)
    if (udp->rx_batch) { udp_rx_batch_free(udp->alloc, udp->rx_batch); udp->rx_batch = NULL; }
    if (udp->tx_batch) { udp_tx_batch_free(udp->alloc, udp->tx_batch); udp->tx_batch = NULL; }
#endif
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
#if defined(__linux__)
    /* Lazily allocate the recvmmsg batch (large: mmsg_batch × recv_buf_size).
     * Failure just leaves rx_batch NULL → the per-datagram path is used. */
    if (udp->mmsg_batch > 1 && !udp->rx_batch)
        udp->rx_batch = udp_rx_batch_new(udp->alloc, udp->mmsg_batch,
                                         udp->recv_buf_size);
#endif
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

int kl_udp_multicast_join(KlUdp *udp, const char *group, unsigned iface_index) {
    if (!udp || udp->fd < 0 || !group) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_mcast_membership(udp, group, iface_index, 1);
}

int kl_udp_multicast_leave(KlUdp *udp, const char *group, unsigned iface_index) {
    if (!udp || udp->fd < 0 || !group) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_mcast_membership(udp, group, iface_index, 0);
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
