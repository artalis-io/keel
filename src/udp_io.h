#ifndef KEEL_SRC_UDP_IO_H
#define KEEL_SRC_UDP_IO_H

/*
 * udp_io.h — the platform-neutral datagram-I/O seam udp.c calls through.
 *
 * Everything below the raw-syscall line lives here: recvmsg/sendmsg + cmsg
 * build/parse, the recvmmsg/sendmmsg batch engines and their batch blocks, UDP
 * GSO (UDP_SEGMENT), and the pktinfo/GRO/TOS recv socket-option setup. The POSIX
 * implementation is udp_io_posix.c; a Winsock sibling (udp_io_win.c) can slot in
 * later. udp.c itself references none of msghdr/cmsg/recvmmsg/pktinfo — it drives
 * the send queue and event-loop interest and calls these functions.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include "udp_internal.h"

/* Apply the per-datagram local-address / GRO / TOS receive socket options that
 * are pktinfo/platform-specific (IP_PKTINFO, IPV6_RECVPKTINFO, UDP_GRO,
 * IP_RECVTOS, IPV6_RECVTCLASS). Reads cfg->recv_pktinfo / recv_gro / recv_tos and
 * sets udp->pktinfo / recv_gro / recv_tos when the kernel accepts each option.
 * Best-effort — a rejected option simply leaves the corresponding flag clear. */
void kl_udp_io_setup_recv_opts(KlUdp *udp, const KlUdpConfig *cfg);

/* Allocate the (small) sendmmsg batch upfront when batching is enabled
 * (udp->mmsg_batch > 1). Best-effort — failure just leaves udp->tx_batch NULL and
 * the per-datagram flush path is used. No-op where mmsg batching is unavailable. */
void kl_udp_io_tx_batch_init(KlUdp *udp);

/* Lazily allocate the (large) recvmmsg batch when batching is enabled and it is
 * not yet allocated. Best-effort — failure leaves udp->rx_batch NULL and the
 * per-datagram recv path is used. No-op where mmsg batching is unavailable. */
void kl_udp_io_rx_batch_init(KlUdp *udp);

/* Free the recv/send mmsg batch blocks (if any) and clear the pointers. */
void kl_udp_io_batch_free(KlUdp *udp);

/* Immediate single-datagram send: send/sendto for the plain case, or sendmsg
 * with a source-pin pktinfo cmsg (when src) and/or a TOS mark cmsg (when
 * tos >= 0). Returns the raw syscall result (bytes sent, or -1 with errno set) —
 * udp.c decides queue-on-EAGAIN vs error from it. */
ssize_t kl_udp_io_raw_send(KlUdp *udp, const void *data, size_t len,
                           const struct sockaddr *dest, socklen_t dest_len,
                           const struct sockaddr *src, socklen_t src_len, int tos);

/* One-syscall UDP GSO send: a single sendmsg carrying a UDP_SEGMENT cmsg so the
 * kernel splits `data` into `seg`-byte datagrams on the wire. Returns the sendmsg
 * result, or -1 with errno == EOPNOTSUPP where GSO is unavailable at build time
 * (udp.c then falls back to per-segment sends). */
ssize_t kl_udp_io_send_gso(KlUdp *udp, const void *data, size_t len,
                           uint16_t seg, const struct sockaddr *dest,
                           socklen_t dest_len);

/* Drain the send queue: sendmmsg batches when a tx batch is present, else one
 * sendmsg/sendto per node. Drops nodes as they are sent (or on a hard error),
 * reconciles event-loop interest, and fires on_drain when the queue empties. */
void kl_udp_io_flush_queue(KlUdp *udp);

/* Drain the socket: recvmmsg batches when an rx batch is present, else one
 * recvmsg per datagram. Parses pktinfo/GRO/TOS control messages and calls
 * kl_udp_deliver per datagram; stops early if a callback stopped/freed the recv. */
void kl_udp_io_recv_drain(KlUdp *udp);

/* Join (join=1) or leave (join=0) an any-source multicast group. Validates that
 * `group` is a multicast address matching the socket family, then issues the
 * IP_ADD/DROP_MEMBERSHIP (IPv4) or IPV6_JOIN/LEAVE_GROUP (IPv6) setsockopt. On
 * failure sets udp->last_error and returns -1; returns 0 on success. */
int kl_udp_io_mcast_membership(KlUdp *udp, const char *group,
                               unsigned iface_index, int join);

/* Apply the address-reuse and kernel socket-buffer options (best-effort):
 * SO_REUSEADDR (cfg->reuse_addr), SO_REUSEPORT (cfg->reuse_port, where the
 * platform provides it), and SO_RCVBUF/SO_SNDBUF (cfg->so_rcvbuf/so_sndbuf).
 * A rejected or unavailable option is silently ignored. */
void kl_udp_io_apply_socket_opts(int fd, const KlUdpConfig *cfg);

/* Set the outgoing TOS/DSCP/ECN mark on an already-bound socket: IP_TOS (IPv4)
 * or IPV6_TCLASS (IPv6). Returns 0 on success, -1 if the option is unavailable
 * at build time or the setsockopt fails. */
int kl_udp_io_set_tos(int fd, int family, int tos);

/* Apply the multicast/broadcast transmit socket options (best-effort):
 * SO_BROADCAST, default TOS/DSCP/ECN marking, and IP/IPV6 MULTICAST_TTL/LOOP/IF.
 * A rejected or unavailable option is silently ignored. */
void kl_udp_io_apply_tx_options(int fd, int family, const KlUdpConfig *cfg);

#endif /* KEEL_SRC_UDP_IO_H */
