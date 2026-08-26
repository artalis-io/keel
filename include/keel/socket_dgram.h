#ifndef KEEL_SOCKET_DGRAM_H
#define KEEL_SOCKET_DGRAM_H

#include <keel/allocator.h>
#include <keel/handle.h>   /* KlSocketHandle, kl_ssize_t */
#include <keel/sockaddr.h>
#include <stddef.h>
#include <stdint.h>

/*
 * socket_dgram.h: the datagram data-plane a KlSocketProvider optionally provides.
 *
 * The socket provider's stream vtable (keel/socket.h) is a small, uniform surface
 * (flat send/recv + lifecycle). The datagram data-plane is larger and more
 * platform-divergent (sendmsg/recvmsg + pktinfo/TOS control messages, GSO, mmsg
 * batching, multicast), so it lives here as a separate optional ops table hung off
 * the provider (KlSocketProvider.dgram; present iff caps & KL_SOCK_CAP_DATAGRAM).
 *
 * Folding it onto the provider means ONE runtime object owns all of a stack's
 * socket I/O, stream and datagram, so a foreign stack (lwIP, …) supplies a single
 * provider with no separately-linked datagram artifact to keep in sync (axis-audit
 * A2). Every op takes the provider ctx + a KlSocketHandle and speaks KlSockAddr,
 * never the datagram core, so the provider holds no datagram machine state; the send-queue walk,
 * delivery, backpressure, and interest tracking all stay in the datagram core.
 *
 * This is the PROVIDER data-plane axis (KlSocketProvider.dgram); it is separate from the public
 * fixed-slot KlDatagram API in <keel/datagram.h>, which re-exports this provider surface. The
 * COMPLETION-axis op descriptors (KlDgramSendOp/KlDgramRecvOp) are a separate,
 * internal concern in src/completion_io.h, not here.
 */

struct KlDatagramSocketConfig;   /* keel/datagram.h: datagram socket-option config (borrowed) */

/* Per-datagram receive-capture options a provider enabled during configure(),
 * returned so the datagram core stores the matching state (kernel may reject any of them). */
#define KL_DGRAM_RX_PKTINFO  (1u << 0)  /* local (dest) address via pktinfo */
#define KL_DGRAM_RX_GRO      (1u << 1)  /* UDP_GRO receive coalescing */
#define KL_DGRAM_RX_TOS      (1u << 2)  /* per-datagram TOS/Traffic-Class */

/* Per-datagram receive metadata a provider fills from control messages. A field a
 * provider can't obtain is left at its "absent" value (has_local 0 / gro_seg 0 /
 * tos -1), which delivery treats as "not present": the same graceful degradation
 * as the option being off. */
typedef struct {
    KlSockAddr local;      /**< pktinfo local (dest) address; valid iff has_local */
    int        has_local;  /**< 1 = local filled */
    int        gro_seg;    /**< GRO coalesced segment size, 0 = none */
    int        tos;        /**< received TOS/Traffic-Class byte, or -1 */
    int        truncated;  /**< 1 = datagram was truncated to the buffer */
} KlDgramRxMeta;

/* One received datagram in a recv batch. `data` points into the rx-batch's own
 * payload storage (valid until the next recv_batch on that batch). */
typedef struct {
    const void   *data;
    size_t        len;
    KlSockAddr    src;
    KlDgramRxMeta meta;
} KlDgramRxSlot;

/* One datagram to place in a send batch (built by the datagram core from the send queue).
 * dest UNSPEC = connected send; src UNSPEC = no source-pin; tos -1 = no mark. */
typedef struct {
    const void *data;
    size_t      len;
    KlSockAddr  dest;
    KlSockAddr  src;
    int         tos;
} KlDgramTxDesc;

typedef struct KlDatagramOps {
    /* One datagram out. Returns bytes sent, or -1 (errno set): the datagram core decides
     * queue-on-EAGAIN vs drop. */
    kl_ssize_t (*send)(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                       const KlSockAddr *dest, const KlSockAddr *src, int tos);

    /* One datagram in, into buf; fills *src and *meta. Returns bytes (>=0), or -1
     * (errno set; EAGAIN/EWOULDBLOCK = drained). */
    kl_ssize_t (*recv)(void *ctx, KlSocketHandle fd, void *buf, size_t buflen,
                       KlSockAddr *src, KlDgramRxMeta *meta);

    /* One-syscall UDP GSO. Optional (NULL); or return -1/EOPNOTSUPP where GSO is
     * unavailable, and the datagram core falls back to per-segment send(). */
    kl_ssize_t (*send_gso)(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                           uint16_t seg, const KlSockAddr *dest);

    /* Init-time datagram socket-option setup, all best-effort, done in one call
     * before bind: address-reuse + kernel socket buffers, broadcast + default TOS +
     * multicast TTL/LOOP/IF, and enabling per-datagram pktinfo/GRO/TOS capture.
     * Returns the bitmask of KL_DGRAM_RX_* capture options the kernel accepted
     * (the datagram core stores the corresponding flags). */
    uint32_t (*configure)(void *ctx, KlSocketHandle fd, int family,
                          const struct KlDatagramSocketConfig *cfg);
    /* Set the outgoing TOS/DSCP/ECN mark (dynamic, post-init). 0 / -1. */
    int (*set_tos)(void *ctx, KlSocketHandle fd, int family, int tos);
    /* Join (join=1) / leave (join=0) an any-source multicast group. 0 / -1. */
    int (*mcast_membership)(void *ctx, KlSocketHandle fd, int family,
                            const char *group, unsigned iface_index, int join);

    /* KL_DGRAM_CAP_* the provider can honor on THIS fd, accounting for the fd's actual address
     * family. Optional (NULL ⇒ the facade grants no optional caps). Family-AWARE but takes no `family`
     * param: the provider inspects the fd (e.g. getsockname) or reports the cross-family intersection;
     * it MUST NOT report a capability the fd's family cannot use (e.g. BROADCAST on an IPv6 fd). */
    unsigned (*caps)(void *ctx, KlSocketHandle fd);

    /* ── Optional mmsg batching (Linux): data-oriented, no callbacks ───────── */
    /* All NULL on a provider without recvmmsg/sendmmsg → the datagram core uses send/recv in a
     * loop. The batch block is provider-allocated but core-owned (opaque here). */
    void *(*rx_batch_new)(KlAllocator *a, int n, size_t bufsz);
    void *(*tx_batch_new)(KlAllocator *a, int n);
    void  (*rx_batch_free)(KlAllocator *a, void *rx_batch);
    void  (*tx_batch_free)(KlAllocator *a, void *tx_batch);
    /* Drain one recvmmsg into rx_batch and fill up to `max` slots (each pointing at
     * the batch's payload storage). Returns datagrams received (>=0), or -1 (errno;
     * EAGAIN = drained). the datagram core iterates the slots and delivers. */
    int   (*recv_batch)(void *ctx, KlSocketHandle fd, void *rx_batch,
                        KlDgramRxSlot *slots, int max);
    /* Send `n` datagrams from `descs` in one sendmmsg (staged through tx_batch).
     * Returns datagrams sent (>=0), or -1 (errno; EAGAIN = would block). the datagram core
     * drops that many from the queue front. */
    int   (*send_batch)(void *ctx, KlSocketHandle fd, void *tx_batch,
                        const KlDgramTxDesc *descs, int n);
} KlDatagramOps;

#endif /* KEEL_SOCKET_DGRAM_H */
