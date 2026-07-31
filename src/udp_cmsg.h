/*
 * udp_cmsg.h — INTERNAL, POSIX-only. Shared UDP control-message (cmsg) helpers for
 * the POSIX datagram receive paths: the readiness recv (udp_io_posix.c) and the POSIX
 * completion backends (event_iouring.c, event_pollcomp.c). Reusing one parser keeps the
 * two event models byte-identical (no drift).
 *
 * This header is POSIX-only *by inclusion*, not by #ifdef: it is included only from
 * POSIX TUs, so it can name POSIX-native cmsg types (struct msghdr) freely. The Winsock
 * receive path (udp_io_win.c) parses WSAMSG with its own helpers and does not include
 * this — keeping platform conditionals out of the cross-platform headers.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */
#ifndef KEEL_SRC_UDP_CMSG_H
#define KEEL_SRC_UDP_CMSG_H

#include <sys/socket.h>      /* struct msghdr, struct sockaddr_storage, socklen_t */

/* Control-message buffer size for a UDP recvmsg — generously sized for the RX cmsgs the
 * kernel may attach (pktinfo local addr + GRO + TOS). Kept as a plain constant (not tied
 * to the pktinfo struct sizes, which are glibc-gated in udp_io_posix.c) so the completion
 * backends can carry a control buffer without pulling in those platform structs. */
#define KL_UDP_RX_CTRL_SIZE 256

/* Extract the datagram's local (destination) address from a received message's pktinfo
 * control data into `*out`. Returns the sockaddr length written, or 0 if no pktinfo cmsg
 * was present. Shared by the readiness recv and the POSIX completion backends. */
socklen_t kl_udp_parse_local(struct msghdr *msg, struct sockaddr_storage *out);

/* Read the UDP_GRO coalesced segment size from a received message's control data, or 0 if
 * none present (or GRO unsupported at build time). Shared by readiness + completion. */
int kl_udp_parse_gro(struct msghdr *msg);

#endif /* KEEL_SRC_UDP_CMSG_H */
