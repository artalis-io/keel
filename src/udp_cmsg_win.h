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

/* TX control-message buffer size (pktinfo source-pin + TOS) — the RX size already covers both. */
#define KL_UDP_WIN_TX_CMSG_SPACE KL_UDP_WIN_RX_CMSG_SPACE

/* Fetch the WSASendMsg extension pointer (WSAID_WSASENDMSG) for `s`; cached process-wide. NULL if the
 * extension is unavailable (caller falls back to WSASendTo — no control message). */
LPFN_WSASENDMSG kl_udp_win_get_sendmsg(SOCKET s);

/* Build the per-datagram SEND control messages into `buf`: source-pin pktinfo (when `src`) + a TOS
 * cmsg (when tos >= 0, keyed by `family`, CALLER-resolved via kl_udp_win_send_family). Every record is
 * capacity-checked. Returns 0 with *out set to the control length (0 when nothing requested), or -1 if
 * a REQUESTED cmsg cannot be built (doesn't fit or unknown family) — the caller MUST fail the send.
 * Shared by the Winsock provider send + the IOCP backend. */
int kl_udp_win_build_control(unsigned char *buf, size_t bufsz,
                             const struct sockaddr *src, int tos, int family, ULONG *out_len);

/* Resolve the family for a send's TOS cmsg level: `dest`, else `src`, else getsockname(s). NEVER
 * defaults to AF_INET. Returns AF_INET / AF_INET6, or -1 if undeterminable. */
int kl_udp_win_send_family(SOCKET s, const struct sockaddr *dest, const struct sockaddr *src);

#endif /* KEEL_SRC_UDP_CMSG_WIN_H */
