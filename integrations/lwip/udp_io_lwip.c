/*
 * udp_io_lwip.c — the lwIP sibling of src/udp_io_posix.c / udp_io_win.c: the
 * platform datagram-I/O TU behind Keel's internal udp_io.h seam, implemented over
 * the lwIP BSD socket API. Linked AHEAD of a stock libkeel.a so its kl_udp_io_*
 * symbols override the archive's udp_io_posix.o at link time — the same
 * link-override pattern as socket_lwip.c / event_lwip.c / resolve_sync_lwip.c.
 *
 * BYO / opt-in: lwIP is NOT vendored — build against your own lwIP (LWIP_DIR) with
 * your lwipopts.h. Unlike the runtime-injected socket/event providers, udp_io is a
 * build/link seam, so this TU includes Keel's INTERNAL headers (udp_internal.h /
 * udp_io.h, via -I<keel>/src) to reach the KlUdp state udp.c drives. That is sound
 * because KlUdp is layout-neutral (no host sockaddr on the struct), so this TU —
 * compiled against lwIP headers — sees the identical struct layout as the stock,
 * host-built udp.c it links against.
 *
 * Per-datagram only: lwIP has no recvmmsg/sendmmsg batching, no UDP GSO
 * (UDP_SEGMENT) and no UDP GRO, so the batch/GSO paths collapse to no-ops (as on
 * Windows). Source-pinned sends and per-packet TOS cmsgs are likewise not offered
 * by lwIP's socket layer, so kl_udp_io_raw_send sends best-effort to the
 * destination and ignores a requested source pin / TOS. See
 * docs/lwip_platform_design.md.
 */

#define KEEL_PLATFORM_LWIP 1   /* socket.h/sockcompat.h -> lwip/sockets.h, not host */

#include "udp_internal.h"      /* KlUdp / KlUdpDatagram + kl_udp_deliver (internal seam) */
#include "udp_io.h"

#include "lwip/sockets.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

/* KlSockAddr <-> lwIP sockaddr — the lwIP-local marshalling, mirroring
 * socket_lwip.c's lw_to_native/lw_from_native (src/sockaddr_native.h resolves the
 * host layout via sockcompat, so the built-in helper is not reused here). This is
 * the only place lwIP's address layout touches the datagram I/O. */
static socklen_t lw_to_native(const KlSockAddr *a, struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof *ss);
    if (a->family == KL_AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)ss;
        in->sin_family = AF_INET;
        in->sin_port = lwip_htons(a->port);
        memcpy(&in->sin_addr, a->u.ip, 4);
        return (socklen_t)sizeof(*in);
    }
#if LWIP_IPV6
    if (a->family == KL_AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = lwip_htons(a->port);
        in6->sin6_scope_id = a->scope_id;
        memcpy(&in6->sin6_addr, a->u.ip, 16);
        return (socklen_t)sizeof(*in6);
    }
#endif
    return 0;
}

static int lw_from_native(KlSockAddr *out, const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        uint8_t ip[4]; memcpy(ip, &in->sin_addr, 4);
        return kl_sockaddr_from_ipv4(out, ip, lwip_ntohs(in->sin_port));
    }
#if LWIP_IPV6
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        uint8_t ip[16]; memcpy(ip, &in6->sin6_addr, 16);
        return kl_sockaddr_from_ipv6(out, ip, lwip_ntohs(in6->sin6_port),
                                     in6->sin6_scope_id);
    }
#endif
    return -1;
}

/* ── Send path ────────────────────────────────────────────────────────── */

ssize_t kl_udp_io_raw_send(KlUdp *udp, const void *data, size_t len,
                           const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    /* lwIP's socket layer offers neither a source-pin pktinfo cmsg nor a per-
     * packet TOS cmsg — send best-effort to the destination and ignore them. */
    (void)src; (void)tos;
    int s = (int)udp->fd;
    if (!dest || kl_sockaddr_family(dest) == KL_AF_UNSPEC)
        return lwip_send(s, data, len, 0);   /* connected socket */

    struct sockaddr_storage ss;
    socklen_t l = lw_to_native(dest, &ss);
    if (l == 0) { errno = EAFNOSUPPORT; return -1; }
    return lwip_sendto(s, data, len, 0, (struct sockaddr *)&ss, l);
}

ssize_t kl_udp_io_send_gso(KlUdp *udp, const void *data, size_t len,
                           uint16_t seg, const KlSockAddr *dest) {
    (void)udp; (void)data; (void)len; (void)seg; (void)dest;
    /* No UDP GSO on lwIP. Return -1 with a non-would-block errno so udp.c rules
     * GSO unsupported (gso_disabled) and falls back to per-segment sends. */
    errno = EIO;
    return -1;
}

void kl_udp_io_flush_queue(KlUdp *udp) {
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

void kl_udp_io_recv_drain(KlUdp *udp) {
    int s = (int)udp->fd;
    for (;;) {
        struct sockaddr_storage src;
        socklen_t sl = (socklen_t)sizeof(src);
        ssize_t n = lwip_recvfrom(s, udp->recv_buf, udp->recv_buf_size, 0,
                                  (struct sockaddr *)&src, &sl);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                udp->last_error = KL_ERR_IO;
            break;   /* drained (or error) */
        }

        /* Marshal the lwIP source address to the neutral KlSockAddr; lwIP offers
         * no pktinfo local (dest) address or GRO coalescing. */
        KlSockAddr ksrc;
        const KlSockAddr *ksrc_p =
            (lw_from_native(&ksrc, (struct sockaddr *)&src) == 0) ? &ksrc : NULL;
        udp->recv_tos_val = -1;
        kl_udp_deliver(udp, udp->recv_buf, (size_t)n, 0, ksrc_p, NULL);

        /* The callback may have called recv_stop() or free() — re-check. */
        if (!udp->recv_active || !kl_handle_valid(udp->fd))
            break;
    }
}

/* ── recv socket-option setup + batch lifecycle ───────────────────────── */

/* lwIP's socket layer does not expose per-datagram pktinfo / GRO / TOS receive
 * capture via setsockopt, so leave udp->pktinfo/recv_gro/recv_tos clear (their
 * default) — the recv path above delivers source-only, gro_seg 0, tos -1. */
void kl_udp_io_setup_recv_opts(KlUdp *udp, const KlUdpConfig *cfg) {
    (void)udp; (void)cfg;
}

/* No recvmmsg/sendmmsg batching on lwIP — the batch inits/free are no-ops
 * (tx_batch/rx_batch stay NULL, so udp.c uses the per-datagram paths above). */
void kl_udp_io_tx_batch_init(KlUdp *udp) { (void)udp; }
void kl_udp_io_rx_batch_init(KlUdp *udp) { (void)udp; }
void kl_udp_io_batch_free(KlUdp *udp)    { (void)udp; }

/* ── Address-reuse / socket-buffer / TOS options ──────────────────────── */

void kl_udp_io_apply_socket_opts(int fd, const KlUdpConfig *cfg) {
    int s = fd;
    if (cfg->reuse_addr) {
        int opt = 1;
        (void)lwip_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }
    /* lwIP usually lacks SO_REUSEPORT; skip. Kernel socket-buffer sizing is
     * best-effort where the option exists. */
#if defined(SO_RCVBUF)
    if (cfg->so_rcvbuf > 0) {
        int v = cfg->so_rcvbuf;
        (void)lwip_setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
    }
#endif
#if defined(SO_SNDBUF)
    if (cfg->so_sndbuf > 0) {
        int v = cfg->so_sndbuf;
        (void)lwip_setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
    }
#endif
}

int kl_udp_io_set_tos(int fd, int family, int tos) {
#if defined(IP_TOS)
    if (family == AF_INET) {
        int v = tos & 0xff;
        return lwip_setsockopt(fd, IPPROTO_IP, IP_TOS, &v, sizeof(v)) == 0 ? 0 : -1;
    }
#endif
#if LWIP_IPV6 && defined(IPV6_TCLASS)
    if (family == AF_INET6) {
        int v = tos & 0xff;
        return lwip_setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &v, sizeof(v)) == 0 ? 0 : -1;
    }
#endif
    (void)fd; (void)family; (void)tos;
    return -1;
}

void kl_udp_io_apply_tx_options(int fd, int family, const KlUdpConfig *cfg) {
    int s = fd;
    if (cfg->broadcast) {
        int opt = 1;
        (void)lwip_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    }
    if (cfg->tos)
        (void)kl_udp_io_set_tos(fd, family, cfg->tos);
    if (family == AF_INET) {
#if defined(IP_MULTICAST_TTL)
        if (cfg->multicast_ttl > 0) {
            uint8_t ttl = (uint8_t)cfg->multicast_ttl;
            (void)lwip_setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        }
#endif
#if defined(IP_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) {
            uint8_t off = 0;
            (void)lwip_setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &off, sizeof(off));
        }
#endif
    }
#if LWIP_IPV6
    else if (family == AF_INET6) {
#if defined(IPV6_MULTICAST_HOPS)
        if (cfg->multicast_ttl > 0) {
            int hops = cfg->multicast_ttl;
            (void)lwip_setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
        }
#endif
#if defined(IPV6_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) {
            unsigned int off = 0;
            (void)lwip_setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &off, sizeof(off));
        }
#endif
    }
#endif
}

/* ── Multicast group membership ───────────────────────────────────────── */

/* Join (join=1) / leave (join=0) an any-source multicast group (best-effort;
 * requires LWIP_IGMP for IPv4 / LWIP_IPV6_MLD for IPv6). Validates the address is
 * multicast and matches the socket family; returns 0 / -1 (last_error set). */
int kl_udp_io_mcast_membership(KlUdp *udp, const char *group,
                               unsigned iface_index, int join) {
    int s = (int)udp->fd;
    (void)s; (void)group; (void)iface_index; (void)join;
    if (udp->family == AF_INET) {
#if LWIP_IGMP
        struct in_addr maddr;
        if (lwip_inet_pton(AF_INET, group, &maddr) != 1 ||
            (lwip_ntohl(maddr.s_addr) >> 28) != 0xE) {   /* not in 224.0.0.0/4 */
            udp->last_error = KL_ERR_INVALID_ARG;
            return -1;
        }
        struct ip_mreq mreq;   /* IPv4 by-index not portable → default interface */
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = maddr;
        mreq.imr_interface.s_addr = lwip_htonl(INADDR_ANY);
        if (lwip_setsockopt(s, IPPROTO_IP,
                            join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                            &mreq, sizeof(mreq)) != 0) {
            udp->last_error = KL_ERR_SOCKET;
            return -1;
        }
        return 0;
#else
        udp->last_error = KL_ERR_SOCKET;   /* lwIP built without LWIP_IGMP */
        return -1;
#endif
    }
#if LWIP_IPV6 && LWIP_IPV6_MLD
    if (udp->family == AF_INET6) {
        struct in6_addr maddr;
        if (lwip_inet_pton(AF_INET6, group, &maddr) != 1 || maddr.s6_addr[0] != 0xff) {
            udp->last_error = KL_ERR_INVALID_ARG;
            return -1;
        }
        struct ipv6_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.ipv6mr_multiaddr = maddr;
        mreq.ipv6mr_interface = iface_index;
        if (lwip_setsockopt(s, IPPROTO_IPV6,
                            join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                            &mreq, sizeof(mreq)) != 0) {
            udp->last_error = KL_ERR_SOCKET;
            return -1;
        }
        return 0;
    }
#endif
    udp->last_error = KL_ERR_INVALID_ARG;
    return -1;
}
