/*
 * udp_cmsg_win.c — the shared Winsock UDP control-message helpers (udp_cmsg_win.h).
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
         * (WSA_CMSG_NXTHDR steps by ALIGN(cmsg_len), so 0 loops forever) — stop.
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
