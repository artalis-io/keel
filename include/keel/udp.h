#ifndef KEEL_UDP_H
#define KEEL_UDP_H

#include <keel/allocator.h>
#include <keel/handle.h>
#include <keel/error.h>
#include <keel/event_ctx.h>

#include <stddef.h>
#include <stdint.h>
#include <keel/sockaddr.h>   /* KlSockAddr — the public address currency */

/**
 * udp.h — Non-blocking UDP datagram socket over KlEventCtx.
 *
 * A UDP socket registered on the event loop: asynchronous receive (one
 * callback per datagram, with the sender's address) and a send path that
 * buffers whole datagrams under backpressure with a byte cap.
 *
 * Caller-owned struct (stack-allocatable). Single-threaded — all callbacks
 * fire on the event-loop thread. Not thread-safe.
 */

typedef struct KlUdp KlUdp;

/* DSCP class selectors + Expedited Forwarding (RFC 2474 / 4594). Shift into the
 * TOS / Traffic-Class byte with KL_TOS(). */
#define KL_DSCP_CS0   0
#define KL_DSCP_CS1   8
#define KL_DSCP_CS2  16
#define KL_DSCP_CS3  24
#define KL_DSCP_CS4  32
#define KL_DSCP_CS5  40
#define KL_DSCP_CS6  48
#define KL_DSCP_CS7  56
#define KL_DSCP_AF11 10
#define KL_DSCP_AF21 18
#define KL_DSCP_AF31 26
#define KL_DSCP_AF41 34
#define KL_DSCP_EF   46

/* ECN codepoints — the low 2 bits of the TOS byte (RFC 3168). */
#define KL_ECN_NOT_ECT 0
#define KL_ECN_ECT1    1
#define KL_ECN_ECT0    2
#define KL_ECN_CE      3

/** @brief Compose a TOS/Traffic-Class byte from a DSCP class and ECN codepoint. */
#define KL_TOS(dscp, ecn) ((((dscp) & 0x3f) << 2) | ((ecn) & 0x3))

/** @brief Internal queued-datagram node (defined in src/udp.c). */
typedef struct KlUdpDatagram KlUdpDatagram;

/**
 * @brief Called once per received datagram, on the event-loop thread.
 *
 * @param udp       Socket the datagram arrived on.
 * @param data      Datagram payload (borrowed — valid only for this call).
 * @param len       Payload length in bytes.
 * @param src       Sender address (borrowed — copy to retain).
 * @param local     Local (destination) address the datagram arrived on, or NULL
 *                  when pktinfo is disabled/unavailable. Borrowed. Useful on a
 *                  wildcard-bound multi-homed socket to reply from the right IP.
 * @param user_data Opaque pointer from kl_udp_recv_start.
 */
typedef void (*KlUdpRecvFn)(KlUdp *udp, const void *data, size_t len,
                            const KlSockAddr *src, const KlSockAddr *local,
                            void *user_data);

/**
 * @brief Called when the send queue transitions from non-empty back to empty.
 */
typedef void (*KlUdpDrainFn)(KlUdp *udp, void *user_data);

/**
 * @brief Called with a whole GRO-coalesced receive buffer (opt-in).
 *
 * When registered via kl_udp_recv_segments (and recv_gro is enabled), a
 * kernel-coalesced buffer is delivered here in one call: @p data holds K
 * back-to-back datagrams, each @p segment_size bytes except possibly the last.
 * The consumer splits it. Non-coalesced datagrams still go to KlUdpRecvFn.
 */
typedef void (*KlUdpRecvSegmentsFn)(KlUdp *udp, const void *data, size_t len,
                                    size_t segment_size,
                                    const KlSockAddr *src, const KlSockAddr *local,
                                    void *user_data);

/**
 * @brief UDP socket configuration.
 */
typedef struct KlUdpConfig {      /* named tag: the datagram provider ops (keel/datagram.h) reference it by struct tag */
    KlEventCtx  *ctx;             /**< Event loop (borrowed — must outlive udp). */
    int          family;         /**< AF_INET / AF_INET6 / AF_UNSPEC (auto from bind_addr). */
    const char  *bind_addr;      /**< Numeric bind address, or NULL for an unbound socket. */
    uint16_t     bind_port;      /**< Bind port, 0 for ephemeral (ignored if bind_addr is NULL). */
    size_t       recv_buf_size;  /**< Per-datagram recv buffer; 0 = 2048, capped at 65535. */
    size_t       max_send_queue; /**< Backpressure cap in bytes; 0 = 256 KiB. */
    int          reuse_addr;     /**< Set SO_REUSEADDR. */
    int          reuse_port;     /**< Set SO_REUSEPORT (fan-out across workers). */
    int          recv_pktinfo;   /**< Capture each datagram's local address (IP_PKTINFO / IPV6_RECVPKTINFO). */
    int          so_rcvbuf;      /**< SO_RCVBUF kernel receive buffer in bytes (0 = OS default). Bounds kernel-side drops under load. */
    int          so_sndbuf;      /**< SO_SNDBUF kernel send buffer in bytes (0 = OS default). */
    int          mmsg_batch;     /**< recvmmsg/sendmmsg batch size (Linux); 0 = default (16), 1 = per-datagram, capped at 64. Higher amortizes syscalls at the cost of RX memory (batch × recv_buf_size). */
    int          recv_gro;       /**< Enable UDP_GRO receive coalescing (Linux ≥5.0; opt-in). Coalesced buffers are split per-segment into on_recv unless a segments callback is set (kl_udp_recv_segments). */
    int          tos;            /**< IP_TOS / IPV6_TCLASS byte stamped on every send (DSCP<<2 | ECN); 0 = OS default. Compose with KL_TOS(). */
    int          recv_tos;       /**< Deliver each datagram's TOS/Traffic-Class byte (IP_RECVTOS / IPV6_RECVTCLASS); read it during on_recv via kl_udp_recv_tos. */
    int          broadcast;      /**< SO_BROADCAST — allow sending to broadcast addresses (IPv4). */
    int          multicast_ttl;  /**< IP_MULTICAST_TTL / IPV6_MULTICAST_HOPS for multicast sends; 0 = kernel default (1 = link-local). */
    int          multicast_disable_loop; /**< 1 = disable local loopback of sent multicast (default: enabled). */
    unsigned     multicast_iface;/**< Egress interface index for multicast sends; 0 = kernel default. IPv4 by-index honored on Linux only. */
    const char  *multicast_group;/**< Optional numeric multicast address to join at init (after bind); NULL = none. */
    KlAllocator *alloc;          /**< NULL = default allocator. */
} KlUdpConfig;

/**
 * @brief Non-blocking UDP socket. Caller-owned; initialize with kl_udp_init.
 */
struct KlUdp {
    KlEventCtx    *ctx;
    KlAllocator   *alloc;
    KlSocketHandle fd;
    int            family;           /**< AF_INET / AF_INET6 (resolved at init). */
    int            connected;        /**< 1 after kl_udp_connect. */
    /* Receive */
    KlUdpRecvFn    on_recv;
    void          *recv_ud;
    KlUdpRecvSegmentsFn on_recv_segments; /**< Coalesced-GRO callback, or NULL. */
    void          *recv_seg_ud;
    int            recv_gro;         /**< 1 = UDP_GRO enabled on this socket. */
    int            gso_disabled;     /**< 1 = UDP GSO ruled unsupported; use fallback. */
    int            recv_tos;         /**< 1 = deliver per-datagram TOS byte. */
    int            recv_tos_val;     /**< Current datagram's TOS byte, or -1. */
    int            recv_active;
    unsigned char *recv_buf;
    size_t         recv_buf_size;
    /* The per-datagram source + local (dest) address are stack scratch inside the
     * platform udp_io recv path (marshalled to KlSockAddr before delivery), so no
     * host sockaddr lives on this struct — KlUdp is layout-neutral (an lwIP udp_io
     * TU can share its ABI without host-sockaddr size skew). */
    int            pktinfo;          /**< 1 = local-address capture enabled. */
    uint64_t       truncated;        /**< Count of oversized (truncated) datagrams. */
    /* Send queue (whole-datagram FIFO) */
    KlUdpDatagram *q_head;
    KlUdpDatagram *q_tail;
    size_t         q_bytes;          /**< Payload bytes currently queued. */
    size_t         max_send_queue;
    uint64_t       dropped;          /**< Datagrams dropped over the cap. */
    KlUdpDrainFn   on_drain;
    void          *drain_ud;
    /* recv/sendmmsg batching (Linux). Batch blocks are opaque (defined in
     * src/udp.c) to keep this header free of the Linux-only struct mmsghdr. */
    int            mmsg_batch;       /**< Effective batch (1 = per-datagram). */
    void          *rx_batch;         /**< Opaque recvmmsg batch, or NULL. */
    void          *tx_batch;         /**< Opaque sendmmsg batch, or NULL. */
    /* Event-loop interest tracking */
    int            watcher_added;
    KlEventMask    want_mask;
    KlError        last_error;
};

/**
 * @brief Create and (optionally) bind a non-blocking UDP socket.
 * @return 0 on success, -1 on failure (last_error set).
 */
int kl_udp_init(KlUdp *udp, const KlUdpConfig *cfg);

/**
 * @brief Close the socket, deregister from the loop, and free all buffers.
 *
 * Does not free the KlUdp struct itself (caller-owned). Safe to call twice.
 */
void kl_udp_free(KlUdp *udp);

/**
 * @brief Fix the peer address. Enables kl_udp_send() and filters inbound
 *        datagrams to that peer.
 * @return 0 on success, -1 on failure (last_error set).
 */
int kl_udp_connect(KlUdp *udp, const KlSockAddr *peer);

/**
 * @brief Begin receiving. Registers READ interest; on_recv fires per datagram.
 * @return 0 on success, -1 on failure (last_error set).
 */
int  kl_udp_recv_start(KlUdp *udp, KlUdpRecvFn on_recv, void *user_data);

/**
 * @brief Stop receiving (drops READ interest; a draining send queue keeps WRITE).
 */
void kl_udp_recv_stop(KlUdp *udp);

/**
 * @brief Send a datagram to an explicit destination (unconnected socket).
 * @return 0 if sent or queued, -1 on error or over-cap (last_error set).
 */
int kl_udp_send_to(KlUdp *udp, const void *data, size_t len,
                   const KlSockAddr *dest);

/**
 * @brief Send a datagram to @p dest FROM a specific local source address.
 *
 * Like kl_udp_send_to but pins the reply's source address via a pktinfo control
 * message — needed on a wildcard-bound multi-homed socket so the reply egresses
 * from the address the client contacted. @p src NULL behaves like kl_udp_send_to.
 * @return 0 if sent or queued, -1 on error or over-cap (last_error set).
 */
int kl_udp_send_to_from(KlUdp *udp, const void *data, size_t len,
                        const KlSockAddr *dest, const KlSockAddr *src);

/**
 * @brief Send a datagram to the connected peer (requires kl_udp_connect).
 * @return 0 if sent or queued, -1 on error or over-cap (last_error set).
 */
int kl_udp_send(KlUdp *udp, const void *data, size_t len);

/**
 * @brief Join an any-source multicast group.
 *
 * @param group        Numeric multicast address matching the socket family
 *                     (IPv4 224.0.0.0/4, IPv6 ff00::/8).
 * @param iface_index  Interface index to join on, 0 = kernel default. IPv6 uses
 *                     the index on all platforms; IPv4 by-index is honored on
 *                     Linux only — elsewhere a non-zero index falls back to the
 *                     default interface. Membership is auto-dropped on
 *                     kl_udp_free (socket close).
 * @return 0 on success, -1 on failure — KL_ERR_INVALID_ARG (not a multicast
 *         address / family mismatch) or KL_ERR_SOCKET (setsockopt failed).
 */
int kl_udp_multicast_join(KlUdp *udp, const char *group, unsigned iface_index);

/** @brief Leave a multicast group previously joined (symmetric with join). */
int kl_udp_multicast_leave(KlUdp *udp, const char *group, unsigned iface_index);

/**
 * @brief Send @p buf as a run of UDP datagrams of @p segment_size bytes each
 *        (the last may be shorter), using UDP GSO where available.
 *
 * On Linux this is one sendmsg with a UDP_SEGMENT control message (the kernel
 * splits it on the wire); elsewhere — or if the kernel rejects GSO, or the
 * socket is under backpressure — it sends each segment individually through the
 * normal send/queue path. The on-wire result is identical; only the syscall
 * count differs.
 *
 * @param segment_size in (0, total_len]; @p total_len must be > 0 and ≤ 65507.
 * @return 0 if all segments were sent or queued, -1 on invalid args / over-cap.
 */
int kl_udp_send_gso(KlUdp *udp, const void *buf, size_t total_len,
                    size_t segment_size, const KlSockAddr *dest);

/**
 * @brief Register a coalesced-GRO receive callback (NULL to clear).
 *
 * When set and recv_gro is enabled, kernel-coalesced buffers are delivered whole
 * to @p cb (with the segment size); otherwise they are split per-segment into
 * the KlUdpRecvFn handler.
 */
void kl_udp_recv_segments(KlUdp *udp, KlUdpRecvSegmentsFn cb, void *user_data);

/**
 * @brief Set the socket's default TOS/Traffic-Class byte for outgoing datagrams
 *        (IP_TOS / IPV6_TCLASS). Compose with KL_TOS(); 0 clears to OS default.
 * @return 0 on success, -1 on failure (last_error set).
 */
int kl_udp_set_tos(KlUdp *udp, int tos);

/**
 * @brief Send one datagram with a per-packet TOS/ECN mark, overriding the
 *        socket default via an IP_TOS / IPV6_TCLASS control message.
 *
 * For congestion controllers that vary the ECN codepoint per packet (e.g. QUIC).
 * @param tos the TOS/Traffic-Class byte (0–255); compose with KL_TOS().
 * @return 0 if sent or queued, -1 on error / over-cap (last_error set).
 */
int kl_udp_send_to_tos(KlUdp *udp, const void *data, size_t len,
                       const KlSockAddr *dest, int tos);

/**
 * @brief The TOS/Traffic-Class byte of the datagram currently being delivered,
 *        or -1 if unavailable (recv_tos disabled, or none present).
 *
 * Only meaningful while inside the KlUdpRecvFn / KlUdpRecvSegmentsFn callback.
 */
int kl_udp_recv_tos(const KlUdp *udp);

/** @brief Register the send-queue-drained callback (NULL to clear). */
void kl_udp_on_drain(KlUdp *udp, KlUdpDrainFn cb, void *user_data);

/** @brief Bytes currently waiting in the send queue. */
size_t kl_udp_send_queued(const KlUdp *udp);

/** @brief Count of datagrams dropped because the send queue was at its cap. */
uint64_t kl_udp_dropped(const KlUdp *udp);

/** @brief Count of inbound datagrams truncated because they exceeded recv_buf_size. */
uint64_t kl_udp_truncated(const KlUdp *udp);

/** @brief The underlying socket fd (for getsockname etc.), or -1. */
KlSocketHandle kl_udp_fd(const KlUdp *udp);

/** @brief The local (bound) port in host byte order, or 0 if unbound/unknown. */
uint16_t kl_udp_local_port(const KlUdp *udp);

/** @brief Last error recorded on a -1 return. */
KlError kl_udp_last_error(const KlUdp *udp);

#endif /* KEEL_UDP_H */
