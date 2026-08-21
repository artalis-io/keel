/*
 * socket_dgram_posix.c — the POSIX datagram data-plane (KlDatagramOps) for the
 * built-in POSIX socket provider. This is the primitive-only form of what used to
 * live in udp_io_posix.c: every op takes (ctx, fd, …) and speaks KlSockAddr, with
 * no datagram machine state — the send-queue walk, delivery, and interest tracking
 * stay in the datagram core, which dispatches through KlSocketProvider.dgram.
 *
 * The recvmsg path always attaches a control buffer and parses opportunistically:
 * the kernel only fills the cmsgs that configure() enabled, so the primitive needs
 * no per-socket capture flags. Provider ctx is unused (POSIX ops act on the fd).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

#include <keel/datagram.h>     /* KlDatagramSocketConfig (configure) + KlDatagramOps */
#include <keel/socket.h>
#include "sockaddr_native.h"   /* KlSockAddr <-> host sockaddr at the boundary */
#include "udp_cmsg.h"          /* kl_udp_build_control / kl_udp_send_family — shared send cmsg builder */
/* Self-contained: the pktinfo/GRO cmsg parsers are dup'd static below rather than
 * shared through a separate seam TU, so a foreign stack can link-override the whole
 * datagram data-plane by supplying its own KlSocketProvider.dgram. */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#if defined(__linux__)
#include <netinet/udp.h>
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#ifndef UDP_GRO
#define UDP_GRO 104
#endif
#endif

/* TX control buffer: a source-pin pktinfo cmsg + a TOS/Traffic-Class cmsg. */
#define DGRAM_TX_CMSG_SPACE (CMSG_SPACE(sizeof(struct in6_pktinfo)) + CMSG_SPACE(sizeof(int)))
/* RX control buffer: pktinfo (local address) + UDP_GRO + TOS cmsgs. */
#define DGRAM_RX_CMSG_SPACE (CMSG_SPACE(sizeof(struct in6_pktinfo)) + \
                             CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(int)))

#if !defined(IPV6_JOIN_GROUP) && defined(IPV6_ADD_MEMBERSHIP)
#define IPV6_JOIN_GROUP  IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#endif

/* ── cmsg build/parse (primitive helpers) ─────────────────────────────── */

/* Build per-datagram send control messages — delegates to the shared builder (udp_cmsg.c) so the
 * provider send and the POSIX completion backends share one implementation (no drift). Returns 0 with
 * *out set, or -1 if a REQUESTED cmsg could not be built (caller fails the send). */
static int dgram_build_control(unsigned char *buf, size_t bufsz,
                               const struct sockaddr *src, int tos, int family, size_t *out) {
    return kl_udp_build_control(buf, bufsz, src, tos, family, out);
}

/* Read the received TOS / Traffic-Class byte from control messages, or -1. */
static int dgram_parse_tos(struct msghdr *msg) {
#if defined(IP_TOS) || defined(IPV6_TCLASS)
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
        int is_tos = 0;
#if defined(IP_TOS)
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_TOS) is_tos = 1;
#endif
#if defined(IPV6_TCLASS)
        if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_TCLASS) is_tos = 1;
#endif
        if (is_tos) {
            if (cm->cmsg_len >= CMSG_LEN(sizeof(int))) {
                int v; memcpy(&v, CMSG_DATA(cm), sizeof(v)); return v & 0xff;
            }
            if (cm->cmsg_len >= CMSG_LEN(1)) return CMSG_DATA(cm)[0];
        }
    }
#else
    (void)msg;
#endif
    return -1;
}

/* Extract the datagram's local (dest) address from pktinfo cmsgs. 0 if none. */
static socklen_t dgram_parse_local(struct msghdr *msg, struct sockaddr_storage *out) {
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
#if defined(IP_PKTINFO)
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
            struct in_pktinfo pi; memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in *s4 = (struct sockaddr_in *)out;
            memset(s4, 0, sizeof(*s4));
            s4->sin_family = AF_INET; s4->sin_addr = pi.ipi_addr;
            return sizeof(*s4);
        }
#endif
        if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_PKTINFO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
            struct in6_pktinfo pi; memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
            memset(s6, 0, sizeof(*s6));
            s6->sin6_family = AF_INET6; s6->sin6_addr = pi.ipi6_addr;
            return sizeof(*s6);
        }
    }
    return 0;
}

/* Read the UDP_GRO coalesced segment size, or 0 if none / unavailable. */
static int dgram_parse_gro(struct msghdr *msg) {
#if defined(__linux__) && defined(UDP_GRO)
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == IPPROTO_UDP && cm->cmsg_type == UDP_GRO &&
            cm->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int seg; memcpy(&seg, CMSG_DATA(cm), sizeof(seg)); return seg;
        }
    }
#else
    (void)msg;
#endif
    return 0;
}

/* Fill a KlDgramRxMeta from a completed recvmsg's control + flags. */
static void dgram_fill_meta(struct msghdr *msg, KlDgramRxMeta *meta) {
    memset(meta, 0, sizeof(*meta));
    meta->tos = -1;
    struct sockaddr_storage local;
    socklen_t local_len = dgram_parse_local(msg, &local);
    if (local_len && kl_sockaddr_from_native(&meta->local, (struct sockaddr *)&local, local_len) == 0)
        meta->has_local = 1;
    meta->gro_seg = dgram_parse_gro(msg);
    meta->tos = dgram_parse_tos(msg);
    if (msg->msg_flags & MSG_TRUNC) meta->truncated = 1;
}

/* ── Per-datagram send/recv ───────────────────────────────────────────── */

static kl_ssize_t pdg_send(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                           const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    (void)ctx;
    int s = (int)fd;
    struct sockaddr_storage ds, ss;
    socklen_t dest_len = (dest && kl_sockaddr_family(dest) != KL_AF_UNSPEC)
                         ? kl_sockaddr_to_native(dest, &ds) : 0;
    socklen_t src_len  = (src && kl_sockaddr_family(src) != KL_AF_UNSPEC)
                         ? kl_sockaddr_to_native(src, &ss) : 0;
    const struct sockaddr *dsa = dest_len ? (const struct sockaddr *)&ds : NULL;

    if (src_len || tos >= 0) {
        /* _Alignas: the builder makes typed struct cmsghdr accesses into this buffer, which requires
         * cmsghdr alignment — a plain unsigned char[] is only byte-aligned. */
        _Alignas(struct cmsghdr) unsigned char control[DGRAM_TX_CMSG_SPACE];
        memset(control, 0, sizeof(control));
        /* Family for the TOS cmsg level: dest, else src, else getsockname — never defaulted to v4. */
        int family = kl_udp_send_family(s, dsa, src_len ? (struct sockaddr *)&ss : NULL);
        if (family < 0) { errno = EINVAL; return -1; }
        size_t clen;
        if (dgram_build_control(control, sizeof(control),
                                src_len ? (struct sockaddr *)&ss : NULL, tos, family, &clen) != 0) {
            errno = EINVAL; return -1;   /* requested source-pin/TOS could not be built → fail the send */
        }
        struct iovec iov = { .iov_base = (void *)data, .iov_len = len };
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = (void *)dsa;
        msg.msg_namelen = dest_len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (clen) { msg.msg_control = control; msg.msg_controllen = clen; }
        kl_ssize_t n;
        do { n = sendmsg(s, &msg, 0); } while (n < 0 && errno == EINTR);
        return n;
    }
    kl_ssize_t n;
    do {
        if (dest_len == 0) n = send(s, data, len, 0);
        else               n = sendto(s, data, len, 0, dsa, dest_len);
    } while (n < 0 && errno == EINTR);
    return n;
}

static kl_ssize_t pdg_recv(void *ctx, KlSocketHandle fd, void *buf, size_t buflen,
                           KlSockAddr *src, KlDgramRxMeta *meta) {
    (void)ctx;
    union { struct cmsghdr align; unsigned char buf[DGRAM_RX_CMSG_SPACE]; } control;
    struct sockaddr_storage from;
    struct iovec iov = { .iov_base = buf, .iov_len = buflen };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);

    kl_ssize_t n;
    do { n = recvmsg((int)fd, &msg, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) return n;

    memset(src, 0, sizeof(*src));
    if (msg.msg_namelen > 0)
        (void)kl_sockaddr_from_native(src, (struct sockaddr *)&from, msg.msg_namelen);
    dgram_fill_meta(&msg, meta);
    return n;
}

static kl_ssize_t pdg_send_gso(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                               uint16_t seg, const KlSockAddr *dest) {
    (void)ctx;
#if defined(__linux__) && defined(UDP_SEGMENT)
    struct sockaddr_storage ds;
    socklen_t dest_len = (dest && kl_sockaddr_family(dest) != KL_AF_UNSPEC)
                         ? kl_sockaddr_to_native(dest, &ds) : 0;
    union { struct cmsghdr align; unsigned char buf[CMSG_SPACE(sizeof(uint16_t))]; } control;
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
    kl_ssize_t n;
    do { n = sendmsg((int)fd, &msg, 0); } while (n < 0 && errno == EINTR);
    return n;
#else
    (void)fd; (void)data; (void)len; (void)seg; (void)dest;
    errno = EOPNOTSUPP;
    return -1;
#endif
}

/* ── Socket options (configure folds the three init-time setups) ──────── */

static uint32_t pdg_configure(void *ctx, KlSocketHandle fd, int family,
                              const struct KlDatagramSocketConfig *cfg) {
    (void)ctx;
    int s = (int)fd;
    uint32_t caps = 0;

    /* Address-reuse + kernel socket buffers. */
    if (cfg->reuse_addr) { int o = 1; (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &o, sizeof(o)); }
#ifdef SO_REUSEPORT
    if (cfg->reuse_port) { int o = 1; (void)setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &o, sizeof(o)); }
#endif
    if (cfg->so_rcvbuf > 0) { int v = cfg->so_rcvbuf; (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)); }
    if (cfg->so_sndbuf > 0) { int v = cfg->so_sndbuf; (void)setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)); }

    /* Broadcast + default TOS + multicast TTL/LOOP/IF. */
    if (cfg->broadcast) { int o = 1; (void)setsockopt(s, SOL_SOCKET, SO_BROADCAST, &o, sizeof(o)); }
    if (cfg->tos) {
        if (family == AF_INET) {
#if defined(IP_TOS)
            int v = cfg->tos & 0xff; (void)setsockopt(s, IPPROTO_IP, IP_TOS, &v, sizeof(v));
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
            int v = cfg->tos & 0xff; (void)setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, &v, sizeof(v));
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
            (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        }
#endif
#if defined(IP_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) {
#if defined(__linux__)
            int off = 0;
#else
            unsigned char off = 0;
#endif
            (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &off, sizeof(off));
        }
#endif
#if defined(__linux__) && defined(IP_MULTICAST_IF)
        if (cfg->multicast_iface > 0) {
            struct ip_mreqn mr; memset(&mr, 0, sizeof(mr));
            mr.imr_ifindex = (int)cfg->multicast_iface;
            (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &mr, sizeof(mr));
        }
#endif
    } else if (family == AF_INET6) {
#if defined(IPV6_MULTICAST_HOPS)
        if (cfg->multicast_ttl > 0) { int h = cfg->multicast_ttl; (void)setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &h, sizeof(h)); }
#endif
#if defined(IPV6_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) { int off = 0; (void)setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &off, sizeof(off)); }
#endif
#if defined(IPV6_MULTICAST_IF)
        if (cfg->multicast_iface > 0) { unsigned idx = cfg->multicast_iface; (void)setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof(idx)); }
#endif
    }

    /* Per-datagram receive capture (report which the kernel accepted). */
    if (cfg->recv_pktinfo) {
        int o = 1; (void)o;
        if (family == AF_INET) {
#if defined(IP_PKTINFO)
            if (setsockopt(s, IPPROTO_IP, IP_PKTINFO, &o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_PKTINFO;
#elif defined(IP_RECVPKTINFO)
            if (setsockopt(s, IPPROTO_IP, IP_RECVPKTINFO, &o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_PKTINFO;
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_RECVPKTINFO)
            if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVPKTINFO, &o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_PKTINFO;
#endif
        }
    }
#if defined(__linux__) && defined(UDP_GRO)
    if (cfg->recv_gro) { int o = 1; if (setsockopt(s, IPPROTO_UDP, UDP_GRO, &o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_GRO; }
#endif
    if (cfg->recv_tos) {
        if (family == AF_INET) {
#if defined(IP_RECVTOS)
            int o = 1; if (setsockopt(s, IPPROTO_IP, IP_RECVTOS, &o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_TOS;
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_RECVTCLASS)
            int o = 1; if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVTCLASS, &o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_TOS;
#endif
        }
    }
    return caps;
}

static int pdg_set_tos(void *ctx, KlSocketHandle fd, int family, int tos) {
    (void)ctx;
#if defined(IP_TOS) || defined(IPV6_TCLASS)
    int s = (int)fd, rc = -1;
    if (family == AF_INET) {
#if defined(IP_TOS)
        int v = tos & 0xff; rc = setsockopt(s, IPPROTO_IP, IP_TOS, &v, sizeof(v));
#endif
    } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
        int v = tos & 0xff; rc = setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, &v, sizeof(v));
#endif
    }
    return rc == 0 ? 0 : -1;
#else
    (void)fd; (void)family; (void)tos;
    return -1;
#endif
}

static int pdg_mcast(void *ctx, KlSocketHandle fd, int family,
                     const char *group, unsigned iface_index, int join) {
    (void)ctx;
    int s = (int)fd;
    (void)s;   /* used only inside the IP_ADD_MEMBERSHIP / IPV6_JOIN_GROUP blocks */
    if (family == AF_INET) {
        struct in_addr maddr;
        if (inet_pton(AF_INET, group, &maddr) != 1 || (ntohl(maddr.s_addr) >> 28) != 0xE)
            return -1;
#if defined(IP_ADD_MEMBERSHIP)
#if defined(__linux__)
        struct ip_mreqn mreq; memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = maddr;
        mreq.imr_address.s_addr = htonl(INADDR_ANY);
        mreq.imr_ifindex = (int)iface_index;
#else
        (void)iface_index;
        struct ip_mreq mreq; memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = maddr;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
#endif
        return setsockopt(s, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                          &mreq, sizeof(mreq)) == 0 ? 0 : -1;
#else
        return -1;
#endif
    } else if (family == AF_INET6) {
        struct in6_addr maddr;
        if (inet_pton(AF_INET6, group, &maddr) != 1 || maddr.s6_addr[0] != 0xff)
            return -1;
#if defined(IPV6_JOIN_GROUP)
        struct ipv6_mreq mreq; memset(&mreq, 0, sizeof(mreq));
        mreq.ipv6mr_multiaddr = maddr;
        mreq.ipv6mr_interface = iface_index;
        return setsockopt(s, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                          &mreq, sizeof(mreq)) == 0 ? 0 : -1;
#else
        return -1;
#endif
    }
    return -1;
}

/* ── mmsg batching (Linux; NULL ops elsewhere → the datagram core per-datagram loop) ── */

#if defined(__linux__)
typedef struct {
    struct mmsghdr          *msgs;
    struct iovec            *iov;
    struct sockaddr_storage *src;
    unsigned char           *data;
    unsigned char           *ctrl;
    size_t                   bufsz, ctrl_sz;
    int                      n;
} DgramRxBatch;

typedef struct {
    struct mmsghdr          *msgs;
    struct iovec            *iov;
    struct sockaddr_storage *dst;
    unsigned char           *ctrl;
    size_t                   ctrl_sz;
    int                      n;
} DgramTxBatch;

static void pdg_rx_batch_free(KlAllocator *a, void *p) {
    DgramRxBatch *b = p; if (!b) return;
    kl_free(a, b->msgs, (size_t)b->n * sizeof(struct mmsghdr));
    kl_free(a, b->iov,  (size_t)b->n * sizeof(struct iovec));
    kl_free(a, b->src,  (size_t)b->n * sizeof(struct sockaddr_storage));
    kl_free(a, b->data, (size_t)b->n * b->bufsz);
    kl_free(a, b->ctrl, (size_t)b->n * b->ctrl_sz);
    kl_free(a, b, sizeof(*b));
}

static void *pdg_rx_batch_new(KlAllocator *a, int n, size_t bufsz) {
    DgramRxBatch *b = kl_malloc(a, sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->n = n; b->bufsz = bufsz; b->ctrl_sz = DGRAM_RX_CMSG_SPACE;
    b->msgs = kl_malloc(a, (size_t)n * sizeof(struct mmsghdr));
    b->iov  = kl_malloc(a, (size_t)n * sizeof(struct iovec));
    b->src  = kl_malloc(a, (size_t)n * sizeof(struct sockaddr_storage));
    b->data = kl_malloc(a, (size_t)n * bufsz);
    b->ctrl = kl_malloc(a, (size_t)n * b->ctrl_sz);
    if (!b->msgs || !b->iov || !b->src || !b->data || !b->ctrl) { pdg_rx_batch_free(a, b); return NULL; }
    return b;
}

static void pdg_tx_batch_free(KlAllocator *a, void *p) {
    DgramTxBatch *b = p; if (!b) return;
    kl_free(a, b->msgs, (size_t)b->n * sizeof(struct mmsghdr));
    kl_free(a, b->iov,  (size_t)b->n * sizeof(struct iovec));
    kl_free(a, b->dst,  (size_t)b->n * sizeof(struct sockaddr_storage));
    kl_free(a, b->ctrl, (size_t)b->n * b->ctrl_sz);
    kl_free(a, b, sizeof(*b));
}

static void *pdg_tx_batch_new(KlAllocator *a, int n) {
    DgramTxBatch *b = kl_malloc(a, sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->n = n; b->ctrl_sz = DGRAM_TX_CMSG_SPACE;
    b->msgs = kl_malloc(a, (size_t)n * sizeof(struct mmsghdr));
    b->iov  = kl_malloc(a, (size_t)n * sizeof(struct iovec));
    b->dst  = kl_malloc(a, (size_t)n * sizeof(struct sockaddr_storage));
    b->ctrl = kl_malloc(a, (size_t)n * b->ctrl_sz);
    if (!b->msgs || !b->iov || !b->dst || !b->ctrl) { pdg_tx_batch_free(a, b); return NULL; }
    return b;
}

/* One recvmmsg into the batch; fill up to `max` slots pointing at the batch's
 * payload storage. Returns datagrams (>=0) or -1 (errno). */
static int pdg_recv_batch(void *ctx, KlSocketHandle fd, void *rx_batch,
                          KlDgramRxSlot *slots, int max) {
    (void)ctx;
    DgramRxBatch *b = rx_batch;
    int n = b->n < max ? b->n : max;
    for (int i = 0; i < n; i++) {
        b->iov[i].iov_base = b->data + (size_t)i * b->bufsz;
        b->iov[i].iov_len  = b->bufsz;
        struct msghdr *m = &b->msgs[i].msg_hdr;
        memset(m, 0, sizeof(*m));
        m->msg_iov = &b->iov[i];
        m->msg_iovlen = 1;
        m->msg_name = &b->src[i];
        m->msg_namelen = sizeof(struct sockaddr_storage);
        m->msg_control = b->ctrl + (size_t)i * b->ctrl_sz;
        m->msg_controllen = b->ctrl_sz;
        b->msgs[i].msg_len = 0;
    }
    int cnt;
    do { cnt = recvmmsg((int)fd, b->msgs, (unsigned)n, 0, NULL); }
    while (cnt < 0 && errno == EINTR);
    if (cnt <= 0) return cnt;

    for (int i = 0; i < cnt; i++) {
        struct msghdr *m = &b->msgs[i].msg_hdr;
        slots[i].data = b->iov[i].iov_base;
        slots[i].len  = (size_t)b->msgs[i].msg_len;
        memset(&slots[i].src, 0, sizeof(slots[i].src));
        if (m->msg_namelen > 0)
            (void)kl_sockaddr_from_native(&slots[i].src, (struct sockaddr *)&b->src[i], m->msg_namelen);
        dgram_fill_meta(m, &slots[i].meta);
    }
    return cnt;
}

/* Send `n` datagrams from `descs` in one sendmmsg. Returns datagrams sent (>=0)
 * or -1 (errno). */
static int pdg_send_batch(void *ctx, KlSocketHandle fd, void *tx_batch,
                          const KlDgramTxDesc *descs, int n) {
    (void)ctx;
    DgramTxBatch *b = tx_batch;
    if (n > b->n) n = b->n;
    for (int i = 0; i < n; i++) {
        b->iov[i].iov_base = (void *)descs[i].data;
        b->iov[i].iov_len  = descs[i].len;
        struct msghdr *m = &b->msgs[i].msg_hdr;
        memset(m, 0, sizeof(*m));
        m->msg_iov = &b->iov[i];
        m->msg_iovlen = 1;
        socklen_t dest_len = (kl_sockaddr_family(&descs[i].dest) != KL_AF_UNSPEC)
                             ? kl_sockaddr_to_native(&descs[i].dest, &b->dst[i]) : 0;
        if (dest_len) { m->msg_name = &b->dst[i]; m->msg_namelen = dest_len; }
        int have_src = (kl_sockaddr_family(&descs[i].src) != KL_AF_UNSPEC);
        if (have_src || descs[i].tos >= 0) {
            struct sockaddr_storage src_ss;
            socklen_t src_len = have_src ? kl_sockaddr_to_native(&descs[i].src, &src_ss) : 0;
            unsigned char *ctrl = b->ctrl + (size_t)i * b->ctrl_sz;
            int fam = kl_udp_send_family((int)fd, dest_len ? (struct sockaddr *)&b->dst[i] : NULL,
                                         src_len ? (struct sockaddr *)&src_ss : NULL);
            size_t clen = 0;
            /* A descriptor whose requested source-pin/TOS cannot be built (undeterminable family or the
             * cmsg doesn't fit) FAILS the batch — never transmit without the requested metadata. */
            if (fam < 0 ||
                dgram_build_control(ctrl, b->ctrl_sz, src_len ? (struct sockaddr *)&src_ss : NULL,
                                    descs[i].tos, fam, &clen) != 0) {
                errno = EINVAL; return -1;
            }
            if (clen) { m->msg_control = ctrl; m->msg_controllen = clen; }
        }
        b->msgs[i].msg_len = 0;
    }
    int sent;
    do { sent = sendmmsg((int)fd, b->msgs, (unsigned)n, 0); }
    while (sent < 0 && errno == EINTR);
    return sent;
}
#endif /* __linux__ */

/* ── The ops table ────────────────────────────────────────────────────── */

/* M2: report ONLY the caps whose implementation is actually compiled in FOR THIS fd's family. Each bit
 * is gated on the exact macro the corresponding op uses (build_control source-pin, IP_TOS/IPV6_TCLASS,
 * IP_ADD_MEMBERSHIP/IPV6_JOIN_GROUP, SO_BROADCAST) so a bit never over-reports a silently-absent op
 * (e.g. IPv4 without IP_PKTINFO emits no source-pin cmsg → SOURCE_PIN is NOT granted). Connected-mode
 * send is always available. If the family cannot be determined, grant nothing (fail-loud). */
static unsigned pdg_caps(void *ctx, KlSocketHandle fd) {
    (void)ctx;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t sl = sizeof(ss);
    if (getsockname((int)fd, (struct sockaddr *)&ss, &sl) != 0)
        return 0;
    unsigned caps = KL_DGRAM_CAP_CONNECTED;   /* connect()+send: no family-specific compile guard */
    if (ss.ss_family == AF_INET) {
#if defined(IP_PKTINFO)
        caps |= KL_DGRAM_CAP_SOURCE_PIN;
#endif
#if defined(IP_TOS)
        caps |= KL_DGRAM_CAP_TOS;
#endif
#if defined(IP_ADD_MEMBERSHIP)
        caps |= KL_DGRAM_CAP_MULTICAST;
#endif
#if defined(SO_BROADCAST)
        caps |= KL_DGRAM_CAP_BROADCAST;   /* IPv4-only */
#endif
    } else if (ss.ss_family == AF_INET6) {
#if defined(IPV6_PKTINFO)
        caps |= KL_DGRAM_CAP_SOURCE_PIN;
#endif
#if defined(IPV6_TCLASS)
        caps |= KL_DGRAM_CAP_TOS;
#endif
#if defined(IPV6_JOIN_GROUP)
        caps |= KL_DGRAM_CAP_MULTICAST;
#endif
        /* no broadcast on IPv6 */
    }
    /* M5 high-throughput SUPPORT. Family-INDEPENDENT in mechanism, but only meaningful for IP datagram
     * sockets — so gate on AF_INET/AF_INET6 to preserve M2's exact-fd truthfulness (a non-IP fd, e.g.
     * AF_UNIX SOCK_DGRAM, must NOT be told it has UDP batch/GSO/GRO). RX/TX batch = recvmmsg/sendmmsg
     * ops compiled in (Linux); GSO = the send_gso op can attempt UDP_SEGMENT (advisory — first use may
     * EOPNOTSUPP); GRO = the provider can capture UDP_GRO (per-socket activation also needs accepted_rx). */
    if (ss.ss_family == AF_INET || ss.ss_family == AF_INET6) {
#if defined(__linux__)
        caps |= KL_DGRAM_CAP_RX_BATCH | KL_DGRAM_CAP_TX_BATCH;
#endif
#if defined(UDP_SEGMENT)
        caps |= KL_DGRAM_CAP_GSO;
#endif
#if defined(UDP_GRO)
        caps |= KL_DGRAM_CAP_GRO;
#endif
    }
    return caps;
}

const KlDatagramOps kl_socket_posix_dgram_ops = {
    .send             = pdg_send,
    .recv             = pdg_recv,
    .send_gso         = pdg_send_gso,
    .configure        = pdg_configure,
    .set_tos          = pdg_set_tos,
    .mcast_membership = pdg_mcast,
    .caps             = pdg_caps,
#if defined(__linux__)
    .rx_batch_new     = pdg_rx_batch_new,
    .tx_batch_new     = pdg_tx_batch_new,
    .rx_batch_free    = pdg_rx_batch_free,
    .tx_batch_free    = pdg_tx_batch_free,
    .recv_batch       = pdg_recv_batch,
    .send_batch       = pdg_send_batch,
#else
    .rx_batch_new     = NULL,
    .tx_batch_new     = NULL,
    .rx_batch_free    = NULL,
    .tx_batch_free    = NULL,
    .recv_batch       = NULL,
    .send_batch       = NULL,
#endif
};
