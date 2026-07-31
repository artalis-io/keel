#ifndef KEEL_SRC_UDP_INTERNAL_H
#define KEEL_SRC_UDP_INTERNAL_H

/*
 * udp_internal.h — shared internals between udp.c and the platform datagram-I/O
 * TU (udp_io_posix.c / a future udp_io_win.c).
 *
 * udp.c owns the portable state machine (send queue, event-loop interest,
 * lifecycle, public API); the platform I/O TU owns the raw datagram syscalls
 * (recvmsg/sendmsg + cmsg build/parse, recvmmsg/sendmmsg batching, GSO, and the
 * pktinfo/GRO/TOS recv socket-option setup) behind the neutral udp_io.h seam.
 * This header carries the pieces both sides touch: the queued-datagram node and
 * the two udp.c helpers the moved I/O code calls back into.
 *
 * INTERNAL header — not installed, no ABI commitment.
 */

#include <keel/udp.h>

#include "socket.h"          /* sockaddr types via sockcompat.h */

/* Whole-datagram FIFO node: header + inline payload (single allocation). */
struct KlUdpDatagram {
    struct KlUdpDatagram   *next;
    struct sockaddr_storage dest;      /* destination for sendto */
    socklen_t               dest_len;  /* 0 = connected send (use send()) */
    struct sockaddr_storage src;       /* pinned source address, or unset */
    socklen_t               src_len;   /* 0 = no source cmsg */
    int                     tos;       /* per-packet TOS byte, or -1 */
    size_t                  len;       /* payload length */
    unsigned char           data[];    /* payload */
};

/* ── udp.c helpers the platform I/O TU calls back into ────────────────────
 * (non-static so the moved recv-drain / flush engines can reach them). */

/* Reconcile event-loop interest with current recv/send state. */
void kl_udp_update_interest(KlUdp *udp);

/* Deliver one received buffer to the on_recv / on_recv_segments callbacks,
 * splitting a GRO-coalesced buffer per segment when needed. */
void kl_udp_deliver(KlUdp *udp, const void *data, size_t len, int gro_seg,
                    struct sockaddr *src, socklen_t src_len,
                    struct sockaddr *local, socklen_t local_len);

/* Control-message buffer size for a completion-loop UDP recvmsg — generously sized
 * for the RX cmsgs the kernel may attach (pktinfo local addr + GRO + TOS). Kept as a
 * plain constant here (not tied to the pktinfo struct sizes, which are glibc-gated in
 * udp_io_posix.c) so the completion backends can carry a control buffer without
 * pulling in those platform structs. */
#define KL_UDP_RX_CTRL_SIZE 256

/* Extract the datagram's local (destination) address from a received message's
 * pktinfo control data into `*out`. Returns the sockaddr length written, or 0 if no
 * pktinfo cmsg was present. Shared by the readiness recv (udp_io_posix.c) and the
 * POSIX completion backends (io_uring/pollcomp) so the two event models parse it
 * identically (no drift). POSIX-only: `struct msghdr` has no Winsock equivalent (the
 * Windows recv path uses WSAMSG in udp_io_win.c), and no Windows TU calls this. */
#ifndef _WIN32
socklen_t kl_udp_parse_local(struct msghdr *msg, struct sockaddr_storage *out);
#endif

/* Completion-loop datagram receive (PAL 8b-4c): a recv finished with `len` bytes in
 * udp->recv_buf from `src`, arriving on local address `local` (or NULL/0 when pktinfo
 * is disabled/unavailable, e.g. a backend without cmsg support). Deliver it
 * (kl_udp_deliver) then re-post the next receive. The completion driver calls this for
 * a KL_COMP_UDP_RECV event; the model-blind delivery matches the readiness recvmsg path. */
void kl_udp_comp_on_recv(KlUdp *udp, const void *buf, size_t len,
                         struct sockaddr *src, socklen_t src_len,
                         struct sockaddr *local, socklen_t local_len);

/* Completion-loop datagram send done (PAL 8b-4d): an overlapped WSASendTo of `len`
 * bytes finished — release its outstanding-bytes reservation and fire on_drain when
 * nothing is left in flight. */
void kl_udp_comp_on_send(KlUdp *udp, size_t len);

#endif /* KEEL_SRC_UDP_INTERNAL_H */
