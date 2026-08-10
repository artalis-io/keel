/*
 * keel/datagram_detail.h — Layout of KlDatagram (opt-in detail).
 *
 * INTERNAL / UNSTABLE. Include this ONLY to embed or stack-allocate a KlDatagram (e.g. KlUdp
 * embeds it); the fields are NOT part of the API and may change without notice. The
 * behavior/ownership contract is in <keel/datagram_contract.md>; the data-plane provider vtable is
 * in <keel/datagram.h>. External code must use the kl_udp_* accessors, not `dg.*`.
 *
 * This carries RAW datagram transport state only (Phase A carve): the socket handle, the
 * whole-datagram send queue, the receive buffer, event-loop interest, and the counters. The
 * borrowed event loop + allocator and the user-facing receive/drain callbacks stay on KlUdp (the
 * UDP configuration/control wrapper).
 */
#ifndef KEEL_DATAGRAM_DETAIL_H
#define KEEL_DATAGRAM_DETAIL_H

#include <keel/datagram.h>  /* typedef struct KlDatagram KlDatagram (the opaque handle) */
#include <keel/handle.h>    /* KlSocketHandle */
#include <keel/error.h>     /* KlError */
#include <keel/event.h>     /* KlEventMask */

#include <stddef.h>
#include <stdint.h>

struct KlUdpDatagram;      /* whole-datagram FIFO node (src/udp_internal.h) — pointer-only here */

/* Raw datagram transport state. Behaviour is unchanged by the Phase-A carve — this is a
 * structural relocation of the fields that previously lived directly on KlUdp. */
struct KlDatagram {
    KlSocketHandle fd;
    int            family;           /**< AF_INET / AF_INET6 (resolved at init). */
    int            connected;        /**< 1 after kl_udp_connect. */
    /* Receive */
    int            recv_gro;         /**< 1 = UDP_GRO enabled on this socket. */
    int            gso_disabled;     /**< 1 = UDP GSO ruled unsupported; use fallback. */
    int            recv_tos;         /**< 1 = deliver per-datagram TOS byte. */
    int            recv_tos_val;     /**< Current datagram's TOS byte, or -1. */
    int            recv_active;
    unsigned char *recv_buf;
    size_t         recv_buf_size;
    /* The per-datagram source + local (dest) address are stack scratch inside the datagram
     * provider's recv op (marshalled to KlSockAddr before delivery), so no host sockaddr lives
     * on this struct — KlDatagram is layout-neutral. */
    int            pktinfo;          /**< 1 = local-address capture enabled. */
    uint64_t       truncated;        /**< Count of oversized (truncated) datagrams. */
    /* Send queue (whole-datagram FIFO) */
    struct KlUdpDatagram *q_head;
    struct KlUdpDatagram *q_tail;
    size_t         q_bytes;          /**< Payload bytes currently queued. */
    size_t         max_send_queue;
    uint64_t       dropped;          /**< Datagrams dropped over the cap. */
    /* recv/sendmmsg batching (Linux). Batch blocks are opaque (defined in src/udp.c) to keep
     * this header free of the Linux-only struct mmsghdr. */
    int            mmsg_batch;       /**< Effective batch (1 = per-datagram). */
    void          *rx_batch;         /**< Opaque recvmmsg batch, or NULL. */
    void          *tx_batch;         /**< Opaque sendmmsg batch, or NULL. */
    /* Event-loop interest tracking */
    int            watcher_added;
    KlEventMask    want_mask;
    KlError        last_error;
};

#endif /* KEEL_DATAGRAM_DETAIL_H */
