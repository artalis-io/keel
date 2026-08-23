/*
 * udp_cmsg.h — INTERNAL, POSIX-only. Shared UDP control-message (cmsg) helpers for
 * the POSIX datagram receive paths: the readiness recv (socket_dgram_posix.c) and the POSIX
 * completion backends (event_iouring.c, event_pollcomp.c). Reusing one parser keeps the
 * two event models byte-identical (no drift).
 *
 * This header is POSIX-only *by inclusion*, not by #ifdef: it is included only from
 * POSIX TUs, so it can name POSIX-native cmsg types (struct msghdr) freely. The Winsock
 * receive path (socket_dgram_win.c) parses WSAMSG with its own helpers and does not include
 * this — keeping platform conditionals out of the cross-platform headers.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */
#ifndef KEEL_SRC_UDP_CMSG_H
#define KEEL_SRC_UDP_CMSG_H

#include <sys/socket.h>      /* struct msghdr, struct sockaddr_storage, socklen_t */

/* Control-message buffer size for a UDP recvmsg — generously sized for the RX cmsgs the
 * kernel may attach (pktinfo local addr + GRO + TOS). Kept as a plain constant (not tied
 * to the pktinfo struct sizes, which are glibc-gated in socket_dgram_posix.c) so the completion
 * backends can carry a control buffer without pulling in those platform structs. */
#define KL_UDP_RX_CTRL_SIZE 256

/* Extract the datagram's local (destination) address from a received message's pktinfo
 * control data into `*out`. Returns the sockaddr length written, or 0 if no pktinfo cmsg
 * was present. Shared by the readiness recv and the POSIX completion backends. */
socklen_t kl_udp_parse_local(struct msghdr *msg, struct sockaddr_storage *out);

/* Read the UDP_GRO coalesced segment size from a received message's control data, or 0 if
 * none present (or GRO unsupported at build time). Shared by readiness + completion. */
int kl_udp_parse_gro(struct msghdr *msg);

/* Read the received TOS / Traffic-Class byte (IP_TOS / IPV6_TCLASS cmsg) from a received message's
 * control data, or -1 if none present (or the platform macros are absent). Shared by the POSIX
 * completion backends (event_iouring.c, event_pollcomp.c) — receive-TOS. */
int kl_udp_parse_tos(struct msghdr *msg);

/* Control-message buffer size for a UDP sendmsg — a source-pin pktinfo cmsg + a TOS cmsg. */
#define KL_UDP_TX_CTRL_SIZE 128

/* Build the per-datagram SEND control messages into `buf`: the source-pin pktinfo cmsg (when `src`,
 * keyed by src->sa_family) + the TOS/Traffic-Class cmsg (when tos >= 0, keyed by `family`). `family` is
 * the CALLER-resolved fd family (kl_udp_send_family) — the builder never guesses it. Every cmsg record
 * is capacity-checked against the remaining buffer before it is written.
 *
 * Returns 0 on success with *out_len set to the control length (0 when nothing was requested — src NULL
 * and tos < 0). Returns -1 when a REQUESTED cmsg cannot be built — it does not fit the buffer, its
 * platform macro is absent, or the family is unknown — so the caller MUST fail the send rather than
 * silently transmit without the requested source-pin / TOS. Shared by the POSIX provider send and the
 * POSIX completion backends (event_iouring.c, event_pollcomp.c) — one implementation, no drift. */
int kl_udp_build_control(unsigned char *buf, size_t bufsz,
                         const struct sockaddr *src, int tos, int family, size_t *out_len);

/* Resolve the family for a send's TOS cmsg level: `dest` family, else `src` family, else the fd's own
 * family via getsockname. NEVER defaults to AF_INET. Returns AF_INET / AF_INET6, or -1 if undeterminable. */
int kl_udp_send_family(int fd, const struct sockaddr *dest, const struct sockaddr *src);

#endif /* KEEL_SRC_UDP_CMSG_H */
