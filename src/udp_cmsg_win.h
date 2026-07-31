/*
 * udp_cmsg_win.h — INTERNAL, Windows-only. Shared Winsock control-message (WSAMSG) helpers
 * for the Windows datagram receive paths: the readiness recv (udp_io_win.c) and the IOCP
 * completion backend (event_iocp.c). Reusing one WSARecvMsg fetch + pktinfo parser keeps the
 * two paths byte-identical (no drift) — the Winsock analogue of the POSIX udp_cmsg.h.
 *
 * Windows-only *by inclusion*, not by #ifdef: included only from Windows TUs, so it names
 * Winsock-native types (WSAMSG, LPFN_WSARECVMSG, SOCKET) freely. The POSIX recv path
 * (udp_io_posix.c / event_iouring.c / event_pollcomp.c) parses struct msghdr via udp_cmsg.h
 * and does not include this — keeping platform conditionals out of the cross-platform headers.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */
#ifndef KEEL_SRC_UDP_CMSG_WIN_H
#define KEEL_SRC_UDP_CMSG_WIN_H

#include "socket.h"       /* sockcompat.h -> winsock2.h (ordered before windows.h), SOCKET */
#include <mswsock.h>      /* WSAID_WSARECVMSG, LPFN_WSARECVMSG */
#include <ws2tcpip.h>     /* IN_PKTINFO / IN6_PKTINFO / WSA_CMSG_* / WSAMSG */

/* RX control-message buffer size: pktinfo (local address) + a TOS/traffic-class cmsg. */
#define KL_UDP_WIN_RX_CMSG_SPACE (WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) + WSA_CMSG_SPACE(sizeof(int)))

/* Fetch the WSARecvMsg extension function pointer (WSAID_WSARECVMSG) for socket `s`. Not an
 * exported symbol; fetched once via WSAIoctl and cached process-wide. Returns NULL if the
 * extension is unavailable (caller falls back to WSARecvFrom, without a local address). */
LPFN_WSARECVMSG kl_udp_win_get_recvmsg(SOCKET s);

/* Extract the datagram's local (destination) address from a received WSAMSG's pktinfo
 * control data into `*out`. Returns the sockaddr length written, or 0 if no pktinfo cmsg was
 * present. Shared by the readiness recv and the IOCP completion backend. */
socklen_t kl_udp_win_parse_local(WSAMSG *msg, struct sockaddr_storage *out);

#endif /* KEEL_SRC_UDP_CMSG_WIN_H */
