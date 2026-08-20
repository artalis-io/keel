/*
 * socket_dgram_win.c — the Winsock datagram data-plane (KlDatagramOps) for the
 * built-in Winsock socket provider. The primitive-only form of udp_io_win.c: every
 * op takes (ctx, fd, …) and speaks KlSockAddr, with no KlUdp machine state — the
 * send-queue walk, delivery, and interest tracking stay in udp.c, which dispatches
 * through KlSocketProvider.dgram.
 *
 * Winsock has no recvmmsg/sendmmsg batching, no UDP GSO, and no UDP GRO, so the
 * batch ops are NULL (udp.c uses the per-datagram loop) and send_gso reports
 * unsupported. The WSARecvMsg/WSARecvFrom recv always attaches a control buffer
 * and parses opportunistically, so the primitive needs no per-socket capture flags.
 */

#include <keel/datagram.h>     /* KlDatagramSocketConfig (configure) + KlDatagramOps */
#include <keel/socket.h>
#include "sockaddr_native.h"   /* KlSockAddr <-> Winsock sockaddr at the boundary */
#include "udp_cmsg_win.h"      /* kl_udp_win_get_recvmsg / kl_udp_win_parse_local (shared w/ IOCP) */

#include <windows.h>
#include <mswsock.h>           /* WSAID_WSASENDMSG, LPFN_WSASENDMSG */
#include <ws2tcpip.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

/* TX control buffer: a source-pin pktinfo cmsg + a TOS/Traffic-Class cmsg. */
#define DGRAM_TX_CMSG_SPACE (WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) + WSA_CMSG_SPACE(sizeof(int)))

#if !defined(IPV6_JOIN_GROUP) && defined(IPV6_ADD_MEMBERSHIP)
#define IPV6_JOIN_GROUP  IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#endif

/* ── Extension fetch + errno + cmsg helpers (self-contained statics) ────── */

/* Shared with the IOCP backend (udp_cmsg_win.c) — one cached WSASendMsg fetch, no drift. */
static LPFN_WSASENDMSG dgram_get_sendmsg(SOCKET s) { return kl_udp_win_get_sendmsg(s); }

/* WSAEWOULDBLOCK -> EWOULDBLOCK (udp.c queues); anything else -> EIO (error path). */
static void dgram_set_errno_from_wsa(void) {
    int e = WSAGetLastError();
    errno = (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS) ? EWOULDBLOCK : EIO;
}

/* Delegates to the shared Winsock builder (udp_cmsg_win.c) — one implementation shared with the
 * IOCP backend (no drift). Returns 0 with *out set, or -1 if a requested cmsg could not be built. */
static int dgram_build_control(unsigned char *buf, size_t bufsz,
                               const struct sockaddr *src, int tos, int family, ULONG *out) {
    return kl_udp_win_build_control(buf, bufsz, src, tos, family, out);
}

/* Runt-cmsg-safe (see kl_udp_win_parse_local): stop the walk on a zero-len cmsg. */
static int dgram_parse_tos(WSAMSG *msg) {
#if defined(IP_TOS) || defined(IPV6_TCLASS)
    for (WSACMSGHDR *cm = WSA_CMSG_FIRSTHDR(msg); cm; cm = WSA_CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_len < sizeof(WSACMSGHDR)) break;
        int is_tos = 0;
#if defined(IP_TOS)
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_TOS) is_tos = 1;
#endif
#if defined(IPV6_TCLASS)
        if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_TCLASS) is_tos = 1;
#endif
        if (is_tos) {
            if (cm->cmsg_len >= WSA_CMSG_LEN(sizeof(int))) {
                int v; memcpy(&v, WSA_CMSG_DATA(cm), sizeof(v)); return v & 0xff;
            }
            if (cm->cmsg_len >= WSA_CMSG_LEN(1))
                return ((unsigned char *)WSA_CMSG_DATA(cm))[0];
        }
    }
#else
    (void)msg;
#endif
    return -1;
}

/* ── Per-datagram send/recv ───────────────────────────────────────────── */

static kl_ssize_t wdg_send(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                           const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    (void)ctx;
    SOCKET s = (SOCKET)fd;
    struct sockaddr_storage ds, ss;
    socklen_t dest_len = (dest && kl_sockaddr_family(dest) != KL_AF_UNSPEC)
                         ? kl_sockaddr_to_native(dest, &ds) : 0;
    socklen_t src_len  = (src && kl_sockaddr_family(src) != KL_AF_UNSPEC)
                         ? kl_sockaddr_to_native(src, &ss) : 0;

    if (src_len || tos >= 0) {
        LPFN_WSASENDMSG fn = dgram_get_sendmsg(s);
        if (!fn) { errno = EIO; return -1; }
        /* _Alignas: the builder makes typed WSACMSGHDR accesses into this buffer (WSACMSGHDR alignment
         * required); a plain unsigned char[] is only byte-aligned. */
        _Alignas(WSACMSGHDR) unsigned char control[DGRAM_TX_CMSG_SPACE];
        memset(control, 0, sizeof(control));
        /* Family for the TOS cmsg level: dest, else src, else getsockname — never defaulted to v4. */
        int family = kl_udp_win_send_family(s, dest_len ? (struct sockaddr *)&ds : NULL,
                                            src_len ? (struct sockaddr *)&ss : NULL);
        if (family < 0) { errno = EIO; return -1; }
        ULONG clen;
        if (dgram_build_control(control, sizeof(control),
                                src_len ? (struct sockaddr *)&ss : NULL, tos, family, &clen) != 0) {
            errno = EIO; return -1;   /* requested source-pin/TOS could not be built → fail the send */
        }
        WSABUF buf = { (ULONG)len, (CHAR *)data };
        WSAMSG msg;
        memset(&msg, 0, sizeof(msg));
        msg.name = dest_len ? (LPSOCKADDR)&ds : NULL;
        msg.namelen = (INT)dest_len;
        msg.lpBuffers = &buf;
        msg.dwBufferCount = 1;
        if (clen) { msg.Control.buf = (CHAR *)control; msg.Control.len = clen; }
        DWORD sent = 0;
        if (fn(s, &msg, 0, &sent, NULL, NULL) == SOCKET_ERROR) { dgram_set_errno_from_wsa(); return -1; }
        return (kl_ssize_t)sent;
    }

    int n;
    if (dest_len == 0) n = send(s, (const char *)data, (int)len, 0);
    else               n = sendto(s, (const char *)data, (int)len, 0, (struct sockaddr *)&ds, (int)dest_len);
    if (n == SOCKET_ERROR) { dgram_set_errno_from_wsa(); return -1; }
    return n;
}

static kl_ssize_t wdg_recv(void *ctx, KlSocketHandle fd, void *buf, size_t buflen,
                           KlSockAddr *src, KlDgramRxMeta *meta) {
    (void)ctx;
    SOCKET s = (SOCKET)fd;
    memset(meta, 0, sizeof(*meta));
    meta->tos = -1;
    memset(src, 0, sizeof(*src));
    struct sockaddr_storage from;
    memset(&from, 0, sizeof(from));   /* kernel fills it; zero-init keeps analyzers happy */
    LPFN_WSARECVMSG fn = kl_udp_win_get_recvmsg(s);

    if (fn) {
        union { WSACMSGHDR align; unsigned char b[KL_UDP_WIN_RX_CMSG_SPACE]; } ctrl;
        WSABUF iov = { (ULONG)buflen, (CHAR *)buf };
        WSAMSG msg;
        memset(&msg, 0, sizeof(msg));
        msg.name = (LPSOCKADDR)&from;
        msg.namelen = (INT)sizeof(from);
        msg.lpBuffers = &iov;
        msg.dwBufferCount = 1;
        msg.Control.buf = (CHAR *)ctrl.b;
        msg.Control.len = (ULONG)sizeof(ctrl.b);
        DWORD got = 0;
        if (fn(s, &msg, &got, NULL, NULL) == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEMSGSIZE) {   /* truncated; control indeterminate — skip parse */
                meta->truncated = 1;
                if (msg.namelen > 0)
                    (void)kl_sockaddr_from_native(src, (struct sockaddr *)&from, (socklen_t)msg.namelen);
                return (kl_ssize_t)buflen;
            }
            errno = (e == WSAEWOULDBLOCK) ? EWOULDBLOCK : EIO;
            return -1;
        }
        if (msg.namelen > 0)
            (void)kl_sockaddr_from_native(src, (struct sockaddr *)&from, (socklen_t)msg.namelen);
        struct sockaddr_storage local;
        socklen_t ll = kl_udp_win_parse_local(&msg, &local);
        if (ll && kl_sockaddr_from_native(&meta->local, (struct sockaddr *)&local, ll) == 0)
            meta->has_local = 1;
        meta->tos = dgram_parse_tos(&msg);
        if (msg.dwFlags & MSG_TRUNC) meta->truncated = 1;
        return (kl_ssize_t)got;
    }

    /* No WSARecvMsg extension — plain recvfrom, source only. */
    int fromlen = (int)sizeof(from);
    int r = recvfrom(s, (char *)buf, (int)buflen, 0, (struct sockaddr *)&from, &fromlen);
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEMSGSIZE) {
            meta->truncated = 1;
            if (fromlen > 0)
                (void)kl_sockaddr_from_native(src, (struct sockaddr *)&from, (socklen_t)fromlen);
            return (kl_ssize_t)buflen;
        }
        errno = (e == WSAEWOULDBLOCK) ? EWOULDBLOCK : EIO;
        return -1;
    }
    if (fromlen > 0)
        (void)kl_sockaddr_from_native(src, (struct sockaddr *)&from, (socklen_t)fromlen);
    return r;
}

static kl_ssize_t wdg_send_gso(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                               uint16_t seg, const KlSockAddr *dest) {
    (void)ctx; (void)fd; (void)data; (void)len; (void)seg; (void)dest;
    errno = EIO;   /* no UDP GSO on Winsock — udp.c falls back to per-segment sends */
    return -1;
}

/* ── Socket options (configure folds the three init-time setups) ──────── */

static uint32_t wdg_configure(void *ctx, KlSocketHandle fd, int family,
                              const struct KlDatagramSocketConfig *cfg) {
    (void)ctx;
    SOCKET s = (SOCKET)fd;
    uint32_t caps = 0;

    if (cfg->reuse_addr) { int o = 1; (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&o, sizeof(o)); }
    /* No SO_REUSEPORT on Winsock (SO_REUSEADDR already permits multiple binds). */
    if (cfg->so_rcvbuf > 0) { int v = cfg->so_rcvbuf; (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&v, sizeof(v)); }
    if (cfg->so_sndbuf > 0) { int v = cfg->so_sndbuf; (void)setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&v, sizeof(v)); }

    if (cfg->broadcast) { int o = 1; (void)setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&o, sizeof(o)); }
    if (cfg->tos) {
        if (family == AF_INET) {
#if defined(IP_TOS)
            int v = cfg->tos & 0xff; (void)setsockopt(s, IPPROTO_IP, IP_TOS, (char *)&v, sizeof(v));
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
            int v = cfg->tos & 0xff; (void)setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, (char *)&v, sizeof(v));
#endif
        }
    }
    if (family == AF_INET) {
#if defined(IP_MULTICAST_TTL)
        if (cfg->multicast_ttl > 0) { DWORD ttl = (DWORD)cfg->multicast_ttl; (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (char *)&ttl, sizeof(ttl)); }
#endif
#if defined(IP_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) { DWORD off = 0; (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (char *)&off, sizeof(off)); }
#endif
    } else if (family == AF_INET6) {
#if defined(IPV6_MULTICAST_HOPS)
        if (cfg->multicast_ttl > 0) { DWORD h = (DWORD)cfg->multicast_ttl; (void)setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (char *)&h, sizeof(h)); }
#endif
#if defined(IPV6_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) { DWORD off = 0; (void)setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (char *)&off, sizeof(off)); }
#endif
#if defined(IPV6_MULTICAST_IF)
        if (cfg->multicast_iface > 0) { DWORD idx = cfg->multicast_iface; (void)setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, (char *)&idx, sizeof(idx)); }
#endif
    }

    /* Per-datagram capture (no UDP GRO on Windows). */
    if (cfg->recv_pktinfo) {
        DWORD o = 1; (void)o;   /* used only inside the IP_PKTINFO/IPV6_PKTINFO blocks */
        if (family == AF_INET) {
#if defined(IP_PKTINFO)
            if (setsockopt(s, IPPROTO_IP, IP_PKTINFO, (char *)&o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_PKTINFO;
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_PKTINFO)
            if (setsockopt(s, IPPROTO_IPV6, IPV6_PKTINFO, (char *)&o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_PKTINFO;
#endif
        }
    }
    if (cfg->recv_tos) {
        if (family == AF_INET) {
#if defined(IP_RECVTOS)
            DWORD o = 1; if (setsockopt(s, IPPROTO_IP, IP_RECVTOS, (char *)&o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_TOS;
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_RECVTCLASS)
            DWORD o = 1; if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVTCLASS, (char *)&o, sizeof(o)) == 0) caps |= KL_DGRAM_RX_TOS;
#endif
        }
    }
    return caps;
}

static int wdg_set_tos(void *ctx, KlSocketHandle fd, int family, int tos) {
    (void)ctx;
#if defined(IP_TOS) || defined(IPV6_TCLASS)
    SOCKET s = (SOCKET)fd; int rc = -1;
    if (family == AF_INET) {
#if defined(IP_TOS)
        int v = tos & 0xff; rc = setsockopt(s, IPPROTO_IP, IP_TOS, (char *)&v, sizeof(v));
#endif
    } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
        int v = tos & 0xff; rc = setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, (char *)&v, sizeof(v));
#endif
    }
    return rc == 0 ? 0 : -1;
#else
    (void)fd; (void)family; (void)tos;
    return -1;
#endif
}

static int wdg_mcast(void *ctx, KlSocketHandle fd, int family,
                     const char *group, unsigned iface_index, int join) {
    (void)ctx;
    SOCKET s = (SOCKET)fd;
    if (family == AF_INET) {
        struct in_addr maddr;
        if (inet_pton(AF_INET, group, &maddr) != 1 || (ntohl(maddr.s_addr) >> 28) != 0xE)
            return -1;
        (void)iface_index;   /* IPv4 by-index not portable on Winsock; default interface */
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = maddr;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        return setsockopt(s, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                          (char *)&mreq, sizeof(mreq)) == 0 ? 0 : -1;
    } else if (family == AF_INET6) {
        struct in6_addr maddr;
        if (inet_pton(AF_INET6, group, &maddr) != 1 || maddr.s6_addr[0] != 0xff)
            return -1;
        struct ipv6_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.ipv6mr_multiaddr = maddr;
        mreq.ipv6mr_interface = iface_index;
        return setsockopt(s, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                          (char *)&mreq, sizeof(mreq)) == 0 ? 0 : -1;
    }
    return -1;
}

/* M2: report ONLY the caps usable on THIS fd. Source-pin AND per-packet TOS both ride WSASendMsg
 * (wdg_send returns EIO if the extension is absent), so both are gated on a successful runtime probe
 * of WSASendMsg for this socket AND the family's cmsg macro. Multicast/broadcast are gated on their
 * family-specific macros; broadcast is IPv4-only; connected-mode send is always available. If the
 * family cannot be determined, grant nothing (fail-loud). */
static unsigned wdg_caps(void *ctx, KlSocketHandle fd) {
    (void)ctx;
    SOCKET s = (SOCKET)fd;
    struct sockaddr_storage ss;
    int sl = (int)sizeof(ss);
    if (getsockname(s, (struct sockaddr *)&ss, &sl) != 0)
        return 0;
    unsigned caps = KL_DGRAM_CAP_CONNECTED;
#if defined(IP_PKTINFO) || defined(IP_TOS) || defined(IPV6_PKTINFO) || defined(IPV6_TCLASS)
    int have_sendmsg = (dgram_get_sendmsg(s) != NULL);   /* probe the extension for THIS socket */
#endif
    if (ss.ss_family == AF_INET) {
#if defined(IP_PKTINFO) || defined(IP_TOS)
        if (have_sendmsg) {   /* source-pin + per-packet TOS both ride WSASendMsg */
#if defined(IP_PKTINFO)
            caps |= KL_DGRAM_CAP_SOURCE_PIN;
#endif
#if defined(IP_TOS)
            caps |= KL_DGRAM_CAP_TOS;
#endif
        }
#endif
#if defined(IP_ADD_MEMBERSHIP)
        caps |= KL_DGRAM_CAP_MULTICAST;
#endif
#if defined(SO_BROADCAST)
        caps |= KL_DGRAM_CAP_BROADCAST;   /* IPv4-only */
#endif
    } else if (ss.ss_family == AF_INET6) {
#if defined(IPV6_PKTINFO) || defined(IPV6_TCLASS)
        if (have_sendmsg) {
#if defined(IPV6_PKTINFO)
            caps |= KL_DGRAM_CAP_SOURCE_PIN;
#endif
#if defined(IPV6_TCLASS)
            caps |= KL_DGRAM_CAP_TOS;
#endif
        }
#endif
#if defined(IPV6_JOIN_GROUP)
        caps |= KL_DGRAM_CAP_MULTICAST;
#endif
    }
    return caps;
}

/* ── The ops table (no mmsg batching on Winsock) ──────────────────────── */

const KlDatagramOps kl_socket_winsock_dgram_ops = {
    .send             = wdg_send,
    .recv             = wdg_recv,
    .send_gso         = wdg_send_gso,
    .configure        = wdg_configure,
    .set_tos          = wdg_set_tos,
    .mcast_membership = wdg_mcast,
    .caps             = wdg_caps,
    .rx_batch_new     = NULL,
    .tx_batch_new     = NULL,
    .rx_batch_free    = NULL,
    .tx_batch_free    = NULL,
    .recv_batch       = NULL,
    .send_batch       = NULL,
};
