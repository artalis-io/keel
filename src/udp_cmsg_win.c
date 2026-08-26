/*
 * udp_cmsg_win.c: the shared Winsock UDP control-message helpers (udp_cmsg_win.h).
 *
 * kl_udp_win_get_recvmsg (WSARecvMsg extension fetch) + kl_udp_win_parse_local
 * (pktinfo local-address parse) are used by the Winsock datagram provider
 * (socket_dgram_win.c) and the IOCP completion backend (event_iocp.c). They used to
 * live in udp_io_win.c; with the datagram data-plane folded onto the socket
 * provider, these shared helpers live here (always linked on Windows) instead.
 */

#include "udp_cmsg_win.h"

#include <windows.h>
#include <mswsock.h>       /* WSAID_WSARECVMSG, LPFN_WSARECVMSG */
#include <ws2tcpip.h>
#include <string.h>
#include <limits.h>        /* ULONG_MAX: bound the control-length size_t -> ULONG conversion */

/* Fetched lazily via WSAIoctl on any socket; process-wide, so cached once. */
static LPFN_WSARECVMSG udp_fn_recvmsg = NULL;

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

socklen_t kl_udp_win_parse_local(WSAMSG *msg, struct sockaddr_storage *out) {
    for (WSACMSGHDR *cm = WSA_CMSG_FIRSTHDR(msg); cm; cm = WSA_CMSG_NXTHDR(msg, cm)) {
        /* A runt/zeroed cmsg (cmsg_len < the header) can't advance the walk
         * (WSA_CMSG_NXTHDR steps by ALIGN(cmsg_len), so 0 loops forever); stop.
         * Happens on a control buffer never filled by a real recv (e.g. a cancelled
         * overlapped WSARecvMsg completing at teardown). */
        if (cm->cmsg_len < sizeof(WSACMSGHDR)) break;
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

/* Fetched lazily via WSAIoctl on any socket; process-wide, so cached once. */
static LPFN_WSASENDMSG udp_fn_sendmsg = NULL;

LPFN_WSASENDMSG kl_udp_win_get_sendmsg(SOCKET s) {
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

/* Overflow-safe capacity check: does a `space`-byte cmsg record fit after `used` bytes in `bufsz`?
 * `used` is a parameter so the subtraction is guarded without a `0 > bufsz` comparison. */
static int ctrl_fits(size_t used, size_t space, size_t bufsz) {
    return used <= bufsz && space <= bufsz - used;
}

int kl_udp_win_build_control(unsigned char *buf, size_t bufsz,
                             const struct sockaddr *src, int tos, int family, ULONG *out_len) {
    *out_len = 0;
    if (bufsz > (size_t)ULONG_MAX) return -1;            /* bound the size_t -> ULONG control length */
    size_t used = 0;   /* bytes written so far; the next cmsg starts at buf + used (WSA_CMSG_SPACE-aligned) */

    if (src) {                                           /* source-pin REQUESTED (family from src) */
        int ok = 0;
        if (src->sa_family == AF_INET) {
            size_t space = WSA_CMSG_SPACE(sizeof(IN_PKTINFO));
            if (!ctrl_fits(used, space, bufsz)) return -1;   /* overflow-safe capacity check */
            WSACMSGHDR *cm = (WSACMSGHDR *)(buf + used);
            cm->cmsg_level = IPPROTO_IP; cm->cmsg_type = IP_PKTINFO;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
            IN_PKTINFO pi; memset(&pi, 0, sizeof(pi));
            pi.ipi_addr = ((const struct sockaddr_in *)src)->sin_addr;
            memcpy(WSA_CMSG_DATA(cm), &pi, sizeof(pi));
            used += space; ok = 1;
        } else if (src->sa_family == AF_INET6) {
            size_t space = WSA_CMSG_SPACE(sizeof(IN6_PKTINFO));
            if (!ctrl_fits(used, space, bufsz)) return -1;   /* overflow-safe capacity check */
            WSACMSGHDR *cm = (WSACMSGHDR *)(buf + used);
            cm->cmsg_level = IPPROTO_IPV6; cm->cmsg_type = IPV6_PKTINFO;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(IN6_PKTINFO));
            IN6_PKTINFO pi; memset(&pi, 0, sizeof(pi));
            pi.ipi6_addr = ((const struct sockaddr_in6 *)src)->sin6_addr;
            memcpy(WSA_CMSG_DATA(cm), &pi, sizeof(pi));
            used += space; ok = 1;
        }
        if (!ok) return -1;                              /* requested source-pin but unknown family */
    }
    if (tos >= 0) {                                      /* TOS REQUESTED (family from the CALLER) */
        int v = tos & 0xff, ok = 0;
        size_t space = WSA_CMSG_SPACE(sizeof(int));
        if (family == AF_INET) {
            if (!ctrl_fits(used, space, bufsz)) return -1;   /* overflow-safe capacity check */
            WSACMSGHDR *cm = (WSACMSGHDR *)(buf + used);
            cm->cmsg_level = IPPROTO_IP; cm->cmsg_type = IP_TOS;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(int));
            memcpy(WSA_CMSG_DATA(cm), &v, sizeof(v));
            used += space; ok = 1;
        } else if (family == AF_INET6) {
            if (!ctrl_fits(used, space, bufsz)) return -1;   /* overflow-safe capacity check */
            WSACMSGHDR *cm = (WSACMSGHDR *)(buf + used);
            cm->cmsg_level = IPPROTO_IPV6; cm->cmsg_type = IPV6_TCLASS;
            cm->cmsg_len = WSA_CMSG_LEN(sizeof(int));
            memcpy(WSA_CMSG_DATA(cm), &v, sizeof(v));
            used += space; ok = 1;
        }
        if (!ok) return -1;                              /* requested TOS but unknown family */
    }
    *out_len = (ULONG)used;
    return 0;
}
int kl_udp_win_send_family(SOCKET s, const struct sockaddr *dest, const struct sockaddr *src) {
    if (dest && (dest->sa_family == AF_INET || dest->sa_family == AF_INET6))
        return dest->sa_family;
    if (src && (src->sa_family == AF_INET || src->sa_family == AF_INET6))
        return src->sa_family;
    struct sockaddr_storage ss;
    int sl = (int)sizeof(ss);
    if (getsockname(s, (struct sockaddr *)&ss, &sl) == 0 &&
        (ss.ss_family == AF_INET || ss.ss_family == AF_INET6))
        return (int)ss.ss_family;
    return -1;
}
