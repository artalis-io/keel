/* glibc gates struct in{,6}_pktinfo + IP_PKTINFO / IPV6_PKTINFO behind
 * _GNU_SOURCE; macOS hides the v6 ones behind RFC 3542. Opt into both before
 * any system header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

#include "udp_internal.h"
#include "udp_cmsg.h"        /* kl_udp_parse_local — shared with the POSIX completion backends */
#include "udp_io.h"
#include "sockaddr_native.h" /* KlSockAddr <-> host sockaddr at the seam boundary */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* UDP GSO/GRO (Linux). The macros live in <linux/udp.h>, which conflicts with
 * <netinet/udp.h> on glibc; define them ourselves when absent to avoid that. */
#if defined(__linux__)
#include <netinet/udp.h>
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
#endif

/* RX control buffer: pktinfo (local address) + UDP_GRO + TOS cmsgs. */
#define UDP_RX_CMSG_SPACE (CMSG_SPACE(sizeof(struct in6_pktinfo)) + \
                           CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(int)))
/* TX control buffer: a source-pin pktinfo cmsg + a TOS/Traffic-Class cmsg. */
#define UDP_TX_CMSG_SPACE (CMSG_SPACE(sizeof(struct in6_pktinfo)) + CMSG_SPACE(sizeof(int)))

/* ── recv/sendmmsg batching (Linux) ───────────────────────────────────── */

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
    struct mmsghdr          *msgs;
    struct iovec            *iov;
    struct sockaddr_storage *dst;      /* per-msg dest (msg_name must persist to sendmmsg) */
    unsigned char           *ctrl;     /* n * ctrl_sz */
    size_t                   ctrl_sz;
    int                      n;
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
    b->ctrl_sz = UDP_RX_CMSG_SPACE;   /* pktinfo + UDP_GRO */
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
    kl_free(a, b->dst,  (size_t)b->n * sizeof(struct sockaddr_storage));
    kl_free(a, b->ctrl, (size_t)b->n * b->ctrl_sz);
    kl_free(a, b, sizeof(*b));
}

static UdpTxBatch *udp_tx_batch_new(KlAllocator *a, int n) {
    UdpTxBatch *b = kl_malloc(a, sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->n = n;
    b->ctrl_sz = UDP_TX_CMSG_SPACE;   /* pktinfo + TOS */
    b->msgs = kl_malloc(a, (size_t)n * sizeof(struct mmsghdr));
    b->iov  = kl_malloc(a, (size_t)n * sizeof(struct iovec));
    b->dst  = kl_malloc(a, (size_t)n * sizeof(struct sockaddr_storage));
    b->ctrl = kl_malloc(a, (size_t)n * b->ctrl_sz);
    if (!b->msgs || !b->iov || !b->dst || !b->ctrl) {
        udp_tx_batch_free(a, b);
        return NULL;
    }
    return b;
}

#endif /* __linux__ */

/* Build per-datagram control messages: a source-pinning pktinfo cmsg (when src
 * is non-NULL) and an IP_TOS / IPV6_TCLASS mark cmsg (when tos >= 0). `family`
 * selects the IP level for the TOS cmsg. Returns the msg_controllen to use (0 if
 * none). Portable — used by the single send/flush path and the batched flush. */
static size_t udp_build_control(unsigned char *buf, size_t bufsz,
                                const struct sockaddr *src, int tos, int family) {
    struct msghdr tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.msg_control = buf;
    tmp.msg_controllen = bufsz;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&tmp);
    size_t used = 0;

    if (src && cm) {
#if defined(IP_PKTINFO)
        if (src->sa_family == AF_INET) {
            cm->cmsg_level = IPPROTO_IP;
            cm->cmsg_type = IP_PKTINFO;
            cm->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
            struct in_pktinfo pi;
            memset(&pi, 0, sizeof(pi));
            pi.ipi_spec_dst = ((const struct sockaddr_in *)src)->sin_addr;
            memcpy(CMSG_DATA(cm), &pi, sizeof(pi));
            used += CMSG_SPACE(sizeof(struct in_pktinfo));
            cm = CMSG_NXTHDR(&tmp, cm);
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
            used += CMSG_SPACE(sizeof(struct in6_pktinfo));
            cm = CMSG_NXTHDR(&tmp, cm);
        }
    }

    if (tos >= 0 && cm) {
        if (family == AF_INET) {
#if defined(IP_TOS)
            int v = tos & 0xff;
            cm->cmsg_level = IPPROTO_IP;
            cm->cmsg_type = IP_TOS;
            cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &v, sizeof(v));
            used += CMSG_SPACE(sizeof(int));
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
            int v = tos & 0xff;
            cm->cmsg_level = IPPROTO_IPV6;
            cm->cmsg_type = IPV6_TCLASS;
            cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &v, sizeof(v));
            used += CMSG_SPACE(sizeof(int));
#endif
        }
    }
    return used;
}

/* ── Send path ────────────────────────────────────────────────────────── */

/* sendmsg with control messages: source-pin pktinfo (when src) and/or a TOS
 * mark (when tos >= 0). */
static ssize_t udp_send_msg(KlUdp *udp, const void *data, size_t len,
                            const struct sockaddr *dest, socklen_t dest_len,
                            const struct sockaddr *src, int tos) {
    unsigned char control[UDP_TX_CMSG_SPACE];
    memset(control, 0, sizeof(control));
    int family = dest_len ? dest->sa_family : udp->family;
    size_t clen = udp_build_control(control, sizeof(control), src, tos, family);

    struct iovec iov = { .iov_base = (void *)data, .iov_len = len };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (void *)dest;
    msg.msg_namelen = dest_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (clen) {
        msg.msg_control = control;
        msg.msg_controllen = clen;
    }

    ssize_t n;
    do { n = sendmsg(udp->fd, &msg, 0); } while (n < 0 && errno == EINTR);
    return n;
}

ssize_t kl_udp_io_raw_send(KlUdp *udp, const void *data, size_t len,
                           const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    /* Marshal the neutral addresses to host sockaddr at the seam boundary; the
     * internal send path (udp_send_msg / build_control) stays in host sockaddr. */
    struct sockaddr_storage ds, ss;
    socklen_t dest_len = (dest && kl_sockaddr_family(dest) != KL_AF_UNSPEC)
                         ? kl_sockaddr_to_native(dest, &ds) : 0;
    socklen_t src_len  = (src && kl_sockaddr_family(src) != KL_AF_UNSPEC)
                         ? kl_sockaddr_to_native(src, &ss) : 0;
    const struct sockaddr *dsa = dest_len ? (const struct sockaddr *)&ds : NULL;
    const struct sockaddr *ssa = src_len  ? (const struct sockaddr *)&ss : NULL;

    if (ssa || tos >= 0)
        return udp_send_msg(udp, data, len, dsa, dest_len, ssa, tos);
    ssize_t n;
    do {
        if (dest_len == 0)
            n = send(udp->fd, data, len, 0);
        else
            n = sendto(udp->fd, data, len, 0, dsa, dest_len);
    } while (n < 0 && errno == EINTR);
    return n;
}

#if defined(__linux__)
/* Dequeue+free the first `k` queued datagrams from the front (sent or dropped).
 * Reads only udp->q_head and self-bounds on it — never over-runs the queue. */
static void udp_drop_front(KlUdp *udp, int k) {
    for (int i = 0; i < k && udp->q_head; i++) {
        KlUdpDatagram *node = udp->q_head;
        udp->q_head = node->next;
        if (!udp->q_head) udp->q_tail = NULL;
        udp->q_bytes -= node->len;
        kl_free(udp->alloc, node, sizeof(KlUdpDatagram) + node->len);
    }
}

/* Drain the send queue in sendmmsg batches. The batch mirrors the queue front
 * in order, so after N are sent we simply drop the front N — no need to retain
 * the node pointers. */
static void udp_flush_queue_batched(KlUdp *udp) {
    UdpTxBatch *b = udp->tx_batch;
    int had_data = (udp->q_head != NULL);

    while (udp->q_head) {
        int cnt = 0;
        for (KlUdpDatagram *node = udp->q_head; node && cnt < b->n;
             node = node->next, cnt++) {
            b->iov[cnt].iov_base = node->data;
            b->iov[cnt].iov_len  = node->len;
            struct msghdr *m = &b->msgs[cnt].msg_hdr;
            memset(m, 0, sizeof(*m));
            m->msg_iov = &b->iov[cnt];
            m->msg_iovlen = 1;
            /* Marshal the neutral dest into the batch's per-msg scratch (its
             * lifetime must span the sendmmsg call). */
            socklen_t dest_len = (kl_sockaddr_family(&node->dest) != KL_AF_UNSPEC)
                                 ? kl_sockaddr_to_native(&node->dest, &b->dst[cnt]) : 0;
            if (dest_len) {
                m->msg_name = &b->dst[cnt];
                m->msg_namelen = dest_len;
            }
            int have_src = (kl_sockaddr_family(&node->src) != KL_AF_UNSPEC);
            if (have_src || node->tos >= 0) {
                struct sockaddr_storage src_ss;
                socklen_t src_len = have_src ? kl_sockaddr_to_native(&node->src, &src_ss) : 0;
                unsigned char *ctrl = b->ctrl + (size_t)cnt * b->ctrl_sz;
                int fam = dest_len ? (int)b->dst[cnt].ss_family : udp->family;
                size_t clen = udp_build_control(
                    ctrl, b->ctrl_sz,
                    src_len ? (struct sockaddr *)&src_ss : NULL,
                    node->tos, fam);
                if (clen) { m->msg_control = ctrl; m->msg_controllen = clen; }
            }
            b->msgs[cnt].msg_len = 0;
        }
        if (cnt == 0)
            break;                   /* nothing to send */

        int sent;
        do { sent = sendmmsg(udp->fd, b->msgs, (unsigned)cnt, 0); }
        while (sent < 0 && errno == EINTR);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;               /* socket buffer full — retry later */
            /* Hard error on the head datagram: drop it and continue. */
            udp->last_error = KL_ERR_IO;
            udp->dropped++;
            udp_drop_front(udp, 1);
            continue;
        }
        udp_drop_front(udp, sent);   /* drop the front `sent` we just sent */
        if (sent < cnt)
            break;                   /* next message would block — retry later */
    }

    kl_udp_update_interest(udp);
    if (had_data && udp->q_head == NULL && udp->on_drain)
        udp->on_drain(udp, udp->drain_ud);
}
#endif /* __linux__ */

void kl_udp_io_flush_queue(KlUdp *udp) {
#if defined(__linux__)
    if (udp->tx_batch) { udp_flush_queue_batched(udp); return; }
#endif
    int had_data = (udp->q_head != NULL);
    while (udp->q_head) {
        KlUdpDatagram *node = udp->q_head;
        ssize_t n = kl_udp_io_raw_send(udp, node->data, node->len,
                                       &node->dest, &node->src, node->tos);
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
    kl_udp_update_interest(udp);   /* drop WRITE once drained */
    if (had_data && udp->q_head == NULL && udp->on_drain)
        udp->on_drain(udp, udp->drain_ud);
}

/* ── Receive path ─────────────────────────────────────────────────────── */

/* Extract the datagram's local (destination) address from pktinfo control messages
 * into `*out`. Returns its length, or 0 if none present. Shared with the completion
 * backends (declared in udp_cmsg.h) so both event models parse pktinfo identically. */
socklen_t kl_udp_parse_local(struct msghdr *msg, struct sockaddr_storage *out) {
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
#if defined(IP_PKTINFO)
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
            struct in_pktinfo pi;
            memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in *s4 = (struct sockaddr_in *)out;
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
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
            memset(s6, 0, sizeof(*s6));
            s6->sin6_family = AF_INET6;
            s6->sin6_addr = pi.ipi6_addr;
            return sizeof(*s6);
        }
    }
    return 0;
}

/* Read the UDP_GRO segment size from a coalesced datagram's control messages, or 0 if
 * none present (or GRO unsupported at build time). Shared with the POSIX completion
 * backends via udp_cmsg.h so both event models parse GRO identically. */
int kl_udp_parse_gro(struct msghdr *msg) {
#if defined(__linux__) && defined(UDP_GRO)
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == IPPROTO_UDP && cm->cmsg_type == UDP_GRO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int seg;
            memcpy(&seg, CMSG_DATA(cm), sizeof(seg));
            return seg;
        }
    }
#else
    (void)msg;
#endif
    return 0;
}

/* Read the received TOS / Traffic-Class byte from control messages (IP_TOS /
 * IPV6_TCLASS), or -1 if none. The cmsg payload is 1 byte or an int per platform. */
static int udp_parse_tos(struct msghdr *msg) {
#if defined(IP_TOS) || defined(IPV6_TCLASS)
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
        int is_tos = 0;
#if defined(IP_TOS)
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_TOS)
            is_tos = 1;
#endif
#if defined(IPV6_TCLASS)
        if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_TCLASS)
            is_tos = 1;
#endif
        if (is_tos) {
            /* Payload is an int or a single byte per platform; compare cmsg_len
             * additively (never subtract — avoids size_t underflow on a runt). */
            if (cm->cmsg_len >= CMSG_LEN(sizeof(int))) {
                int v;
                memcpy(&v, CMSG_DATA(cm), sizeof(v));
                return v & 0xff;
            }
            if (cm->cmsg_len >= CMSG_LEN(1))
                return CMSG_DATA(cm)[0];
        }
    }
#else
    (void)msg;
#endif
    return -1;
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
            if (udp->pktinfo || udp->recv_gro || udp->recv_tos) {
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
            socklen_t local_len = udp->pktinfo ? kl_udp_parse_local(m, &udp->recv_local) : 0;
            int gro = udp->recv_gro ? kl_udp_parse_gro(m) : 0;
            udp->recv_tos_val = udp->recv_tos ? udp_parse_tos(m) : -1;
            /* Marshal host sockaddr -> neutral KlSockAddr at the seam boundary. */
            KlSockAddr ksrc, klocal;
            kl_sockaddr_from_native(&ksrc, (struct sockaddr *)&b->src[i], m->msg_namelen);
            if (local_len)
                kl_sockaddr_from_native(&klocal, (struct sockaddr *)&udp->recv_local, local_len);
            kl_udp_deliver(udp, b->iov[i].iov_base, (size_t)b->msgs[i].msg_len, gro,
                           &ksrc, local_len ? &klocal : NULL);
            /* The callback may have called recv_stop() or free() — re-check. */
            if (!udp->recv_active || udp->fd < 0)
                return;
        }
        if (cnt < b->n)
            break;   /* fewer than a full batch → socket drained */
    }
}
#endif /* __linux__ */

void kl_udp_io_recv_drain(KlUdp *udp) {
#if defined(__linux__)
    if (udp->rx_batch) { udp_recv_drain_batched(udp); return; }
#endif
    union {
        struct cmsghdr align;
        unsigned char  buf[UDP_RX_CMSG_SPACE];
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
        if (udp->pktinfo || udp->recv_gro || udp->recv_tos) {
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

        socklen_t local_len = udp->pktinfo ? kl_udp_parse_local(&msg, &udp->recv_local) : 0;
        int gro = udp->recv_gro ? kl_udp_parse_gro(&msg) : 0;
        udp->recv_tos_val = udp->recv_tos ? udp_parse_tos(&msg) : -1;

        /* Marshal host sockaddr -> neutral KlSockAddr at the seam boundary. */
        KlSockAddr ksrc, klocal;
        kl_sockaddr_from_native(&ksrc, (struct sockaddr *)&udp->recv_src, msg.msg_namelen);
        if (local_len)
            kl_sockaddr_from_native(&klocal, (struct sockaddr *)&udp->recv_local, local_len);
        kl_udp_deliver(udp, udp->recv_buf, (size_t)n, gro,
                       &ksrc, local_len ? &klocal : NULL);

        /* The callback may have called recv_stop() or free() — re-check. */
        if (!udp->recv_active || udp->fd < 0)
            break;
    }
}

/* ── UDP GSO (Linux) ──────────────────────────────────────────────────── */

ssize_t kl_udp_io_send_gso(KlUdp *udp, const void *data, size_t len,
                           uint16_t seg, const KlSockAddr *dest) {
#if defined(__linux__) && defined(UDP_SEGMENT)
    struct sockaddr_storage ds;
    socklen_t dest_len = (dest && kl_sockaddr_family(dest) != KL_AF_UNSPEC)
                         ? kl_sockaddr_to_native(dest, &ds) : 0;
    union {
        struct cmsghdr align;
        unsigned char  buf[CMSG_SPACE(sizeof(uint16_t))];
    } control;
    memset(&control, 0, sizeof(control));
    struct iovec iov = { .iov_base = (void *)data, .iov_len = len };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = dest_len ? (void *)&ds : NULL;
    msg.msg_namelen = dest_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);

    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm) { errno = EINVAL; return -1; }
    cm->cmsg_level = IPPROTO_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    memcpy(CMSG_DATA(cm), &seg, sizeof(seg));
    msg.msg_controllen = CMSG_SPACE(sizeof(uint16_t));

    ssize_t n;
    do { n = sendmsg(udp->fd, &msg, 0); } while (n < 0 && errno == EINTR);
    return n;
#else
    (void)udp; (void)data; (void)len; (void)seg; (void)dest;
    errno = EOPNOTSUPP;
    return -1;   /* GSO unavailable at build time — caller uses the fallback */
#endif
}

/* ── recv socket-option setup + batch lifecycle ───────────────────────── */

/* cppcheck-suppress constParameterPointer ; udp->pktinfo/recv_gro/recv_tos are
   written below, but only under the platform #ifdefs — invisible to the config
   cppcheck analyzes with those options undefined. */
void kl_udp_io_setup_recv_opts(KlUdp *udp, const KlUdpConfig *cfg) {
    int family = udp->family;

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

    /* Enable UDP_GRO receive coalescing (best-effort; Linux ≥5.0). */
#if defined(__linux__) && defined(UDP_GRO)
    if (cfg->recv_gro) {
        int on = 1;
        if (setsockopt(udp->fd, IPPROTO_UDP, UDP_GRO, &on, sizeof(on)) == 0)
            udp->recv_gro = 1;
    }
#endif

    /* Deliver each datagram's TOS/Traffic-Class byte (best-effort). */
    if (cfg->recv_tos) {
        if (family == AF_INET) {
#if defined(IP_RECVTOS)
            int on = 1;
            if (setsockopt(udp->fd, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on)) == 0)
                udp->recv_tos = 1;
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_RECVTCLASS)
            int on = 1;
            if (setsockopt(udp->fd, IPPROTO_IPV6, IPV6_RECVTCLASS, &on, sizeof(on)) == 0)
                udp->recv_tos = 1;
#endif
        }
    }
}

void kl_udp_io_tx_batch_init(KlUdp *udp) {
#if defined(__linux__)
    /* Allocate the (small) sendmmsg batch upfront; the recvmmsg batch (which
     * carries the large data block) is allocated lazily in kl_udp_recv_start.
     * Allocation failure just disables batching — it is an optimization. */
    if (udp->mmsg_batch > 1 && !udp->tx_batch)
        udp->tx_batch = udp_tx_batch_new(udp->alloc, udp->mmsg_batch);
#else
    (void)udp;
#endif
}

void kl_udp_io_rx_batch_init(KlUdp *udp) {
#if defined(__linux__)
    /* Lazily allocate the recvmmsg batch (large: mmsg_batch × recv_buf_size).
     * Failure just leaves rx_batch NULL → the per-datagram path is used. */
    if (udp->mmsg_batch > 1 && !udp->rx_batch)
        udp->rx_batch = udp_rx_batch_new(udp->alloc, udp->mmsg_batch,
                                         udp->recv_buf_size);
#else
    (void)udp;
#endif
}

void kl_udp_io_batch_free(KlUdp *udp) {
#if defined(__linux__)
    if (udp->rx_batch) { udp_rx_batch_free(udp->alloc, udp->rx_batch); udp->rx_batch = NULL; }
    if (udp->tx_batch) { udp_tx_batch_free(udp->alloc, udp->tx_batch); udp->tx_batch = NULL; }
#else
    (void)udp;
#endif
}

/* ── Address-reuse / socket-buffer / TOS options ──────────────────────── */

/* Apply SO_REUSEADDR/SO_REUSEPORT + SO_RCVBUF/SO_SNDBUF (best-effort). */
void kl_udp_io_apply_socket_opts(int fd, const KlUdpConfig *cfg) {
    if (cfg->reuse_addr) {
        int opt = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }
#ifdef SO_REUSEPORT
    if (cfg->reuse_port) {
        int opt = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }
#endif
    /* Kernel socket-buffer sizing (best-effort; the kernel clamps to its
     * rmem_max/wmem_max and may round/double the value). */
    if (cfg->so_rcvbuf > 0) {
        int v = cfg->so_rcvbuf;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
    }
    if (cfg->so_sndbuf > 0) {
        int v = cfg->so_sndbuf;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
    }
}

/* Set the outgoing TOS/DSCP/ECN mark. Returns 0 on success, -1 otherwise. */
int kl_udp_io_set_tos(int fd, int family, int tos) {
#if defined(IP_TOS) || defined(IPV6_TCLASS)
    int rc = -1;
    if (family == AF_INET) {
#if defined(IP_TOS)
        int v = tos & 0xff;
        rc = setsockopt(fd, IPPROTO_IP, IP_TOS, &v, sizeof(v));
#endif
    } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
        int v = tos & 0xff;
        rc = setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &v, sizeof(v));
#endif
    }
    return rc == 0 ? 0 : -1;
#else
    (void)fd; (void)family; (void)tos;
    return -1;
#endif
}

/* ── Multicast group membership ───────────────────────────────────────── */

/* Join (join=1) or leave (join=0) an any-source multicast group. Validates the
 * address is multicast and matches the socket family. Returns 0 / -1 (last_error). */
int kl_udp_io_mcast_membership(KlUdp *udp, const char *group,
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
void kl_udp_io_apply_tx_options(int fd, int family, const KlUdpConfig *cfg) {
    if (cfg->broadcast) {
        int opt = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    }
    /* Default TOS/DSCP/ECN marking for outgoing datagrams. */
    if (cfg->tos) {
        if (family == AF_INET) {
#if defined(IP_TOS)
            int v = cfg->tos & 0xff;
            (void)setsockopt(fd, IPPROTO_IP, IP_TOS, &v, sizeof(v));
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
            int v = cfg->tos & 0xff;
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &v, sizeof(v));
#endif
        }
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
