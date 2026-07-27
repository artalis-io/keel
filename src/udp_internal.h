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

/* Completion-loop datagram receive (PAL 8b-4c): a WSARecvFrom finished with `len`
 * bytes in udp->recv_buf from `src` — deliver it (kl_udp_deliver) then re-post the
 * next receive. The completion driver calls this for a KL_COMP_UDP_RECV event; the
 * model-blind delivery is identical to the readiness recvmsg path. */
void kl_udp_comp_on_recv(KlUdp *udp, const void *buf, size_t len,
                         struct sockaddr *src, socklen_t src_len);

#endif /* KEEL_SRC_UDP_INTERNAL_H */
