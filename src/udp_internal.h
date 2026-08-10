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

/* Whole-datagram FIFO node: header + inline payload (single allocation).
 * Addresses are the Keel-neutral KlSockAddr; each platform udp_io TU marshals to
 * its own sockaddr layout at the seam (family KL_AF_UNSPEC = "unset"). */
struct KlUdpDatagram {
    struct KlUdpDatagram   *next;
    KlSockAddr              dest;      /* destination; UNSPEC = connected send() */
    KlSockAddr              src;       /* pinned source, or UNSPEC = none */
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
                    const KlSockAddr *src, const KlSockAddr *local);

/* The POSIX cmsg parser (kl_udp_parse_local) + its RX control-buffer size live in the
 * POSIX-only udp_cmsg.h, included by the POSIX recv TUs — kept out of this cross-platform
 * header so no platform #ifdef leaks in (struct msghdr has no Winsock equivalent). */

/* Per-datagram receive metadata a completion backend extracts from the recv (control
 * messages + flags), handed to kl_udp_comp_on_recv alongside the payload. All fields are
 * cross-platform (no msghdr): a backend that can't obtain a value leaves it zeroed, which
 * the delivery treats as "not present" — the same graceful degradation as the option being
 * off. Extensible (e.g. a future recv-TOS) without churning the function signature. */
typedef struct {
    const KlSockAddr *local;     /* local (dest) addr via pktinfo, or NULL */
    int              gro_seg;    /* GRO coalesced segment size, 0 = none */
    int              truncated;  /* 1 if the datagram was truncated (MSG_TRUNC) */
} KlUdpRxMeta;

/* Completion-loop datagram receive (PAL 8b-4c): a recv finished with `len` bytes in
 * udp->recv_buf from `src`, with per-datagram `meta` (local addr / GRO / truncation).
 * Deliver it (kl_udp_deliver) then re-post the next receive. The completion driver calls
 * this for a KL_COMP_DGRAM_RECV event; the model-blind delivery matches the readiness
 * recvmsg path. */
void kl_udp_comp_on_recv(KlUdp *udp, const void *buf, size_t len,
                         const KlSockAddr *src, const KlUdpRxMeta *meta);

/* Completion-loop datagram send done (PAL 8b-4d): an overlapped WSASendTo of `len`
 * bytes finished — release its outstanding-bytes reservation and fire on_drain when
 * nothing is left in flight. */
void kl_udp_comp_on_send(KlUdp *udp, size_t len);

/* The comp_udp_dispatch hook (completion_core.c → here): route a datagram completion
 * (KL_COMP_DGRAM_RECV / KL_COMP_DGRAM_SEND) to kl_udp_comp_on_recv / kl_udp_comp_on_send.
 * Registered on the ctx by kl_udp_init, so the generic tick reaches the UDP stack
 * without a static reference — a client-only build links neither udp.c nor these.
 * `evp` is a const KlCompletionEvent* (opaque const void* at the hook seam). */
struct KlEventCtx;
void kl_udp_comp_dispatch(struct KlEventCtx *ctx, const void *evp);

#endif /* KEEL_SRC_UDP_INTERNAL_H */
