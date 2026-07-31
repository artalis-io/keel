/*
 * udp_io_win.c — the Winsock sibling of udp_io_posix.c: the platform datagram-I/O
 * TU behind the neutral udp_io.h seam, compiled only on Windows (Makefile OS=
 * windows branch, UDP_IO_SRC). One provider per TU, so it uses Winsock directly
 * with no internal platform #ifdef.
 *
 * This mirrors udp_io_posix.c function-by-function, but per-datagram only:
 * Winsock has no recvmmsg/sendmmsg batching, no UDP GSO (UDP_SEGMENT), and no UDP
 * GRO — so the batch/GSO/GRO paths collapse to no-ops (tx_batch/rx_batch stay
 * NULL, kl_udp_io_send_gso returns unsupported so udp.c falls back to per-segment
 * sends, and delivered gro_seg is always 0).
 *
 * The extended-function pointers WSARecvMsg / WSASendMsg are not exported symbols;
 * they are fetched once per process via WSAIoctl(SIO_GET_EXTENSION_FUNCTION_
 * POINTER) and cached in file-scope statics (lazily, on first use).
 *
 * errno note: udp.c (portable) inspects `errno == EAGAIN || errno == EWOULDBLOCK`
 * after a send/recv to choose queue-vs-error. On Winsock the would-block error is
 * WSAGetLastError() == WSAEWOULDBLOCK, which is NOT the CRT EWOULDBLOCK udp.c
 * tests. So on every would-block return path we set `errno = EWOULDBLOCK` before
 * returning -1 — this keeps udp.c's errno checks working unchanged (udp.c stays
 * #ifdef-free and is not modified).
 */

#include "udp_internal.h"
#include "udp_io.h"

#include "socket.h"       /* sockcompat.h -> winsock2.h / ws2tcpip.h, KlSocketHandle */

#include <windows.h>
#include <mswsock.h>      /* WSAID_WSARECVMSG / WSAID_WSASENDMSG, LPFN_WSARECVMSG/SENDMSG */
#include <ws2tcpip.h>     /* IN_PKTINFO / IN6_PKTINFO / WSA_CMSG_* */
#include "udp_cmsg_win.h" /* shared WSARecvMsg fetch + pktinfo parse (reused by event_iocp.c) */

#include <errno.h>
#include <stdint.h>
#include <string.h>

/* RX control buffer: pktinfo (local address) + TOS cmsgs (no GRO on Windows). */
#define UDP_RX_CMSG_SPACE KL_UDP_WIN_RX_CMSG_SPACE
/* TX control buffer: a source-pin pktinfo cmsg + a TOS/Traffic-Class cmsg. */
#define UDP_TX_CMSG_SPACE (WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) + WSA_CMSG_SPACE(sizeof(int)))

/* ── Extended-function-pointer fetch (WSARecvMsg / WSASendMsg) ──────────── */

/* Fetched lazily via WSAIoctl on any bound socket; the pointers are process-wide
 * (identical across sockets), so caching them once is safe. NULL until fetched;
 * a fetch that fails leaves them NULL and the caller reports EWOULDBLOCK-style
 * failure (there is no plausible fallback for pktinfo/TOS control messages). */
static LPFN_WSARECVMSG udp_fn_recvmsg = NULL;
static LPFN_WSASENDMSG udp_fn_sendmsg = NULL;

LPFN_WSARECVMSG kl_udp_win_get_recvmsg(SOCKET s) {
    if (udp_fn_recvmsg)
        return udp_fn_recvmsg;
    GUID guid = WSAID_WSARECVMSG;
    LPFN_WSARECVMSG fn = NULL;
    DWORD nbytes = 0;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                 &fn, sizeof(fn), &nbytes, NULL, NULL) == 0)
        udp_fn_recvmsg = fn;
    return udp_fn_recvmsg;
}

static LPFN_WSASENDMSG udp_get_sendmsg(SOCKET s) {
    if (udp_fn_sendmsg)
        return udp_fn_sendmsg;
    GUID guid = WSAID_WSASENDMSG;
    LPFN_WSASENDMSG fn = NULL;
    DWORD nbytes = 0;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                 &fn, sizeof(fn), &nbytes, NULL, NULL) == 0)
        udp_fn_sendmsg = fn;
    return udp_fn_sendmsg;
}

/* Translate the last Winsock error into the CRT errno udp.c's send/recv-result
 * handling reads: WSAEWOULDBLOCK -> EWOULDBLOCK so the queue-on-would-block path
 * fires; anything else -> a non-would-block errno so udp.c takes the error path. */
static void udp_set_errno_from_wsa(void) {
    int e = WSAGetLastError();
    errno = (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS) ? EWOULDBLOCK : EIO;
}

/* ── cmsg build (source-pin pktinfo + TOS) ─────────────────────────────── */

/* Build per-datagram control messages into a WSAMSG's control buffer: a source-
 * pinning pktinfo cmsg (when src is non-NULL) and an IP_TOS / IPV6_TCLASS mark
 * cmsg (when tos >= 0). `family` selects the IP level for the TOS cmsg. Returns
 * the control length to use (0 if none). Mirrors udp_build_control in the POSIX
 * TU, but over WSAMSG/WSACMSGHDR. */
static ULONG udp_build_control(unsigned char *buf, size_t bufsz,
                               const struct sockaddr *src, int tos, int family) {
    WSAMSG tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.Control.buf = (CHAR *)buf;
    tmp.Control.len = (ULONG)bufsz;
    WSACMSGHDR *cm = WSA_CMSG_FIRSTHDR(&tmp);
    ULONG used = 0;

    if (src && cm) {
        if (src->sa_family == AF_INET) {
            cm->cmsg_level = IPPROTO_IP;
            cm->cmsg_type = IP_PKTINFO;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
            IN_PKTINFO pi;
            memset(&pi, 0, sizeof(pi));
            pi.ipi_addr = ((const struct sockaddr_in *)src)->sin_addr;
            memcpy(WSA_CMSG_DATA(cm), &pi, sizeof(pi));
            used += WSA_CMSG_SPACE(sizeof(IN_PKTINFO));
            cm = WSA_CMSG_NXTHDR(&tmp, cm);
        } else if (src->sa_family == AF_INET6) {
            cm->cmsg_level = IPPROTO_IPV6;
            cm->cmsg_type = IPV6_PKTINFO;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(IN6_PKTINFO));
            IN6_PKTINFO pi;
            memset(&pi, 0, sizeof(pi));
            pi.ipi6_addr = ((const struct sockaddr_in6 *)src)->sin6_addr;
            memcpy(WSA_CMSG_DATA(cm), &pi, sizeof(pi));
            used += WSA_CMSG_SPACE(sizeof(IN6_PKTINFO));
            cm = WSA_CMSG_NXTHDR(&tmp, cm);
        }
    }

    if (tos >= 0 && cm) {
        if (family == AF_INET) {
#if defined(IP_TOS)
            int v = tos & 0xff;
            cm->cmsg_level = IPPROTO_IP;
            cm->cmsg_type = IP_TOS;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(int));
            memcpy(WSA_CMSG_DATA(cm), &v, sizeof(v));
            used += WSA_CMSG_SPACE(sizeof(int));
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
            int v = tos & 0xff;
            cm->cmsg_level = IPPROTO_IPV6;
            cm->cmsg_type = IPV6_TCLASS;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(int));
            memcpy(WSA_CMSG_DATA(cm), &v, sizeof(v));
            used += WSA_CMSG_SPACE(sizeof(int));
#endif
        }
    }
    return used;
}

/* ── Send path ────────────────────────────────────────────────────────── */

/* WSASendMsg with control messages: source-pin pktinfo (when src) and/or a TOS
 * mark (when tos >= 0). Returns bytes sent, or -1 with errno set. */
static ssize_t udp_send_msg(KlUdp *udp, const void *data, size_t len,
                            const struct sockaddr *dest, socklen_t dest_len,
                            const struct sockaddr *src, int tos) {
    SOCKET s = (SOCKET)udp->fd;
    LPFN_WSASENDMSG fn = udp_get_sendmsg(s);
    if (!fn) { errno = EIO; return -1; }

    unsigned char control[UDP_TX_CMSG_SPACE];
    memset(control, 0, sizeof(control));
    int family = dest_len ? dest->sa_family : udp->family;
    ULONG clen = udp_build_control(control, sizeof(control), src, tos, family);

    WSABUF buf;
    buf.buf = (CHAR *)data;
    buf.len = (ULONG)len;
    WSAMSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.name = (LPSOCKADDR)dest;
    msg.namelen = (INT)dest_len;
    msg.lpBuffers = &buf;
    msg.dwBufferCount = 1;
    if (clen) {
        msg.Control.buf = (CHAR *)control;
        msg.Control.len = clen;
    }

    DWORD sent = 0;
    if (fn(s, &msg, 0, &sent, NULL, NULL) == SOCKET_ERROR) {
        udp_set_errno_from_wsa();
        return -1;
    }
    return (ssize_t)sent;
}

ssize_t kl_udp_io_raw_send(KlUdp *udp, const void *data, size_t len,
                           const struct sockaddr *dest, socklen_t dest_len,
                           const struct sockaddr *src, socklen_t src_len, int tos) {
    if ((src && src_len) || tos >= 0)
        return udp_send_msg(udp, data, len, dest, dest_len,
                            (src && src_len) ? src : NULL, tos);
    SOCKET s = (SOCKET)udp->fd;
    int n;
    if (dest_len == 0)
        n = send(s, (const char *)data, (int)len, 0);
    else
        n = sendto(s, (const char *)data, (int)len, 0, dest, (int)dest_len);
    if (n == SOCKET_ERROR) {
        udp_set_errno_from_wsa();
        return -1;
    }
    return n;
}

void kl_udp_io_flush_queue(KlUdp *udp) {
    int had_data = (udp->q_head != NULL);
    while (udp->q_head) {
        KlUdpDatagram *node = udp->q_head;
        ssize_t n = kl_udp_io_raw_send(udp, node->data, node->len,
                                       node->dest_len ? (struct sockaddr *)&node->dest : NULL,
                                       node->dest_len,
                                       node->src_len ? (struct sockaddr *)&node->src : NULL,
                                       node->src_len, node->tos);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;                 /* socket buffer still full — retry later */
            /* Hard error (e.g. WSAECONNREFUSED on a connected socket): drop it. */
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

/* Extract the datagram's local (destination) address from pktinfo control
 * messages into udp->recv_local. Returns its length, or 0 if none present. */
/* Shared with the IOCP completion backend via udp_cmsg_win.h — writes the pktinfo local
 * (destination) address into `*out`, so the readiness and completion Windows recv paths
 * parse it identically (no drift). */
socklen_t kl_udp_win_parse_local(WSAMSG *msg, struct sockaddr_storage *out) {
    for (WSACMSGHDR *cm = WSA_CMSG_FIRSTHDR(msg); cm; cm = WSA_CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO &&
            cm->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
            IN_PKTINFO pi;
            memcpy(&pi, WSA_CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in *s4 = (struct sockaddr_in *)out;
            memset(s4, 0, sizeof(*s4));
            s4->sin_family = AF_INET;
            s4->sin_addr = pi.ipi_addr;
            return sizeof(*s4);
        }
        if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_PKTINFO &&
            cm->cmsg_len >= WSA_CMSG_LEN(sizeof(IN6_PKTINFO))) {
            IN6_PKTINFO pi;
            memcpy(&pi, WSA_CMSG_DATA(cm), sizeof(pi));
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
            memset(s6, 0, sizeof(*s6));
            s6->sin6_family = AF_INET6;
            s6->sin6_addr = pi.ipi6_addr;
            return sizeof(*s6);
        }
    }
    return 0;
}

/* Read the received TOS / Traffic-Class byte from control messages (IP_TOS /
 * IPV6_TCLASS), or -1 if none. The cmsg payload is 1 byte or an int per platform. */
static int udp_parse_tos(WSAMSG *msg) {
#if defined(IP_TOS) || defined(IPV6_TCLASS)
    for (WSACMSGHDR *cm = WSA_CMSG_FIRSTHDR(msg); cm; cm = WSA_CMSG_NXTHDR(msg, cm)) {
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
            if (cm->cmsg_len >= WSA_CMSG_LEN(sizeof(int))) {
                int v;
                memcpy(&v, WSA_CMSG_DATA(cm), sizeof(v));
                return v & 0xff;
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

void kl_udp_io_recv_drain(KlUdp *udp) {
    SOCKET s = (SOCKET)udp->fd;
    LPFN_WSARECVMSG fn = kl_udp_win_get_recvmsg(s);

    union {
        WSACMSGHDR    align;
        unsigned char buf[UDP_RX_CMSG_SPACE];
    } control;

    for (;;) {
        int want_control = (udp->pktinfo || udp->recv_tos);
        /* Without WSARecvMsg we cannot capture pktinfo/TOS control data — fall
         * back to a plain recvfrom for the payload + source address. */
        if (want_control && !fn)
            want_control = 0;

        WSABUF iov;
        iov.buf = (CHAR *)udp->recv_buf;
        iov.len = (ULONG)udp->recv_buf_size;

        ssize_t n = 0;
        WSAMSG msg;
        socklen_t src_len = 0;
        int have_control = 0;   /* cmsgs only valid on a full (non-truncated) recv */

        if (want_control) {
            memset(&msg, 0, sizeof(msg));
            msg.name = (LPSOCKADDR)&udp->recv_src;
            msg.namelen = (INT)sizeof(udp->recv_src);
            msg.lpBuffers = &iov;
            msg.dwBufferCount = 1;
            msg.Control.buf = (CHAR *)control.buf;
            msg.Control.len = (ULONG)sizeof(control.buf);

            DWORD got = 0;
            if (fn(s, &msg, &got, NULL, NULL) == SOCKET_ERROR) {
                int e = WSAGetLastError();
                if (e == WSAEMSGSIZE) {   /* datagram truncated to the buffer */
                    udp->truncated++;
                    n = (ssize_t)udp->recv_buf_size;
                    src_len = (socklen_t)msg.namelen;
                    /* On WSAEMSGSIZE, WSARecvMsg does not reliably populate
                     * Control.buf/len — leave have_control 0 so the parse below
                     * skips the (indeterminate) control buffer for this datagram. */
                } else {
                    if (e != WSAEWOULDBLOCK)
                        udp->last_error = KL_ERR_IO;
                    break;   /* drained (or error) */
                }
            } else {
                n = (ssize_t)got;
                src_len = (socklen_t)msg.namelen;
                have_control = 1;   /* Control.buf/len valid — safe to parse cmsgs */
                if (msg.dwFlags & MSG_TRUNC)
                    udp->truncated++;
            }
        } else {
            int fromlen = (int)sizeof(udp->recv_src);
            int r = recvfrom(s, (char *)udp->recv_buf, (int)udp->recv_buf_size, 0,
                             (struct sockaddr *)&udp->recv_src, &fromlen);
            if (r == SOCKET_ERROR) {
                int e = WSAGetLastError();
                if (e == WSAEMSGSIZE) {   /* datagram truncated to the buffer */
                    udp->truncated++;
                    n = (ssize_t)udp->recv_buf_size;
                    src_len = (socklen_t)fromlen;
                } else {
                    if (e != WSAEWOULDBLOCK)
                        udp->last_error = KL_ERR_IO;
                    break;   /* drained (or error) */
                }
            } else {
                n = r;
                src_len = (socklen_t)fromlen;
            }
        }

        socklen_t local_len = (have_control && udp->pktinfo) ? kl_udp_win_parse_local(&msg, &udp->recv_local) : 0;
        udp->recv_tos_val = (have_control && udp->recv_tos) ? udp_parse_tos(&msg) : -1;

        /* Windows has no UDP GRO → gro_seg is always 0. */
        kl_udp_deliver(udp, udp->recv_buf, (size_t)n, 0,
                       (struct sockaddr *)&udp->recv_src, src_len,
                       local_len ? (struct sockaddr *)&udp->recv_local : NULL,
                       local_len);

        /* The callback may have called recv_stop() or free() — re-check. */
        if (!udp->recv_active || !kl_handle_valid(udp->fd))
            break;
    }
}

/* ── UDP GSO (unavailable on Windows) ─────────────────────────────────── */

ssize_t kl_udp_io_send_gso(KlUdp *udp, const void *data, size_t len,
                           uint16_t seg, const struct sockaddr *dest,
                           socklen_t dest_len) {
    (void)udp; (void)data; (void)len; (void)seg; (void)dest; (void)dest_len;
    /* No UDP GSO on Winsock. Return -1 with a non-would-block errno so udp.c
     * rules GSO unsupported (gso_disabled) and falls back to per-segment sends. */
    errno = EIO;
    return -1;
}

/* ── recv socket-option setup + batch lifecycle ───────────────────────── */

void kl_udp_io_setup_recv_opts(KlUdp *udp, const KlUdpConfig *cfg) {
    int family = udp->family;
    /* cppcheck-suppress unreadVariable ; s is used only inside the IP_PKTINFO/
     * IP_RECVTOS #if blocks below — invisible to the config cppcheck analyzes on
     * a host without the Winsock headers that define those macros. */
    SOCKET s = (SOCKET)udp->fd;

    /* Enable per-datagram local-address capture (best-effort). */
    if (cfg->recv_pktinfo) {
        /* cppcheck-suppress unreadVariable ; opt is consumed only inside the
         * IP_PKTINFO/IPV6_PKTINFO #if blocks (macro-blind on the cppcheck host). */
        DWORD opt = 1;
        if (family == AF_INET) {
#if defined(IP_PKTINFO)
            if (setsockopt(s, IPPROTO_IP, IP_PKTINFO, (char *)&opt, sizeof(opt)) == 0)
                udp->pktinfo = 1;
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_PKTINFO)
            if (setsockopt(s, IPPROTO_IPV6, IPV6_PKTINFO, (char *)&opt, sizeof(opt)) == 0)
                udp->pktinfo = 1;
#endif
        }
    }

    /* No UDP_GRO on Windows → leave udp->recv_gro clear. */

    /* Deliver each datagram's TOS/Traffic-Class byte (best-effort; IP_RECVTOS may
     * be absent on some SDKs → skip, leaving the flag clear). */
    if (cfg->recv_tos) {
        if (family == AF_INET) {
#if defined(IP_RECVTOS)
            DWORD on = 1;
            if (setsockopt(s, IPPROTO_IP, IP_RECVTOS, (char *)&on, sizeof(on)) == 0)
                udp->recv_tos = 1;
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_RECVTCLASS)
            DWORD on = 1;
            if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVTCLASS, (char *)&on, sizeof(on)) == 0)
                udp->recv_tos = 1;
#endif
        }
    }
}

/* No recvmmsg/sendmmsg batching on Windows — the batch inits/free are no-ops
 * (tx_batch/rx_batch stay NULL, so udp.c uses the per-datagram paths above). */
void kl_udp_io_tx_batch_init(KlUdp *udp) { (void)udp; }
void kl_udp_io_rx_batch_init(KlUdp *udp) { (void)udp; }
void kl_udp_io_batch_free(KlUdp *udp)    { (void)udp; }

/* ── Address-reuse / socket-buffer / TOS options ──────────────────────── */

/* Apply SO_REUSEADDR + SO_RCVBUF/SO_SNDBUF (best-effort). Winsock has no
 * SO_REUSEPORT. Winsock setsockopt takes a (const char *) optval. */
void kl_udp_io_apply_socket_opts(int fd, const KlUdpConfig *cfg) {
    SOCKET s = (SOCKET)fd;
    if (cfg->reuse_addr) {
        int opt = 1;
        (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));
    }
    /* No SO_REUSEPORT on Windows (SO_REUSEADDR already permits multiple binds). */
    /* Kernel socket-buffer sizing (best-effort). */
    if (cfg->so_rcvbuf > 0) {
        int v = cfg->so_rcvbuf;
        (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&v, sizeof(v));
    }
    if (cfg->so_sndbuf > 0) {
        int v = cfg->so_sndbuf;
        (void)setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&v, sizeof(v));
    }
}

/* Set the outgoing TOS/DSCP/ECN mark. Returns 0 on success, -1 otherwise.
 * Note: IP_TOS is restricted on Windows and typically fails — that's fine, we
 * return -1 like POSIX does when the option is unavailable. */
int kl_udp_io_set_tos(int fd, int family, int tos) {
#if defined(IP_TOS) || defined(IPV6_TCLASS)
    SOCKET s = (SOCKET)fd;
    int rc = -1;
    if (family == AF_INET) {
#if defined(IP_TOS)
        int v = tos & 0xff;
        rc = setsockopt(s, IPPROTO_IP, IP_TOS, (char *)&v, sizeof(v));
#endif
    } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
        int v = tos & 0xff;
        rc = setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, (char *)&v, sizeof(v));
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
 * address is multicast and matches the socket family. Returns 0 / -1 (last_error).
 * Winsock spells the IPv6 ops IPV6_ADD_MEMBERSHIP / IPV6_DROP_MEMBERSHIP. */
int kl_udp_io_mcast_membership(KlUdp *udp, const char *group,
                               unsigned iface_index, int join) {
    SOCKET s = (SOCKET)udp->fd;
    if (udp->family == AF_INET) {
        struct in_addr maddr;
        if (inet_pton(AF_INET, group, &maddr) != 1 ||
            (ntohl(maddr.s_addr) >> 28) != 0xE) {   /* not in 224.0.0.0/4 */
            udp->last_error = KL_ERR_INVALID_ARG;
            return -1;
        }
        (void)iface_index;   /* IPv4 by-index unsupported here; default interface */
        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = maddr;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(s, IPPROTO_IP,
                       join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                       (char *)&mreq, sizeof(mreq)) != 0) {
            udp->last_error = KL_ERR_SOCKET;
            return -1;
        }
        return 0;
    } else if (udp->family == AF_INET6) {
        struct in6_addr maddr;
        if (inet_pton(AF_INET6, group, &maddr) != 1 || maddr.s6_addr[0] != 0xff) {
            udp->last_error = KL_ERR_INVALID_ARG;
            return -1;
        }
        struct ipv6_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.ipv6mr_multiaddr = maddr;
        mreq.ipv6mr_interface = iface_index;
        if (setsockopt(s, IPPROTO_IPV6,
                       join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP,
                       (char *)&mreq, sizeof(mreq)) != 0) {
            udp->last_error = KL_ERR_SOCKET;
            return -1;
        }
        return 0;
    }
    udp->last_error = KL_ERR_INVALID_ARG;
    return -1;
}

/* Apply multicast/broadcast transmit options (best-effort). Winsock multicast
 * TTL/LOOP take a DWORD/int; the IPv6 multicast IF takes the interface index as
 * a DWORD. */
void kl_udp_io_apply_tx_options(int fd, int family, const KlUdpConfig *cfg) {
    SOCKET s = (SOCKET)fd;
    if (cfg->broadcast) {
        int opt = 1;
        (void)setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&opt, sizeof(opt));
    }
    /* Default TOS/DSCP/ECN marking for outgoing datagrams. */
    if (cfg->tos) {
        if (family == AF_INET) {
#if defined(IP_TOS)
            int v = cfg->tos & 0xff;
            (void)setsockopt(s, IPPROTO_IP, IP_TOS, (char *)&v, sizeof(v));
#endif
        } else if (family == AF_INET6) {
#if defined(IPV6_TCLASS)
            int v = cfg->tos & 0xff;
            (void)setsockopt(s, IPPROTO_IPV6, IPV6_TCLASS, (char *)&v, sizeof(v));
#endif
        }
    }
    if (family == AF_INET) {
#if defined(IP_MULTICAST_TTL)
        if (cfg->multicast_ttl > 0) {
            DWORD ttl = (DWORD)cfg->multicast_ttl;
            (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (char *)&ttl, sizeof(ttl));
        }
#endif
#if defined(IP_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) {
            DWORD off = 0;
            (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (char *)&off, sizeof(off));
        }
#endif
        /* IPv4 IP_MULTICAST_IF by index is not portable on Winsock (it takes an
         * in_addr); leave the default interface — matches the non-Linux POSIX
         * path which also skips IPv4 by-index. */
    } else if (family == AF_INET6) {
#if defined(IPV6_MULTICAST_HOPS)
        if (cfg->multicast_ttl > 0) {
            DWORD hops = (DWORD)cfg->multicast_ttl;
            (void)setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (char *)&hops, sizeof(hops));
        }
#endif
#if defined(IPV6_MULTICAST_LOOP)
        if (cfg->multicast_disable_loop) {
            DWORD off = 0;
            (void)setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (char *)&off, sizeof(off));
        }
#endif
#if defined(IPV6_MULTICAST_IF)
        if (cfg->multicast_iface > 0) {
            DWORD idx = cfg->multicast_iface;
            (void)setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, (char *)&idx, sizeof(idx));
        }
#endif
    }
}
