#ifndef KEEL_UDP_SERVER_H
#define KEEL_UDP_SERVER_H

#include <keel/allocator.h>
#include <keel/error.h>
#include <keel/event_ctx.h>
#include <keel/udp.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/**
 * udp_server.h — Datagram dispatch surface, symmetric with KlServer.
 *
 * A bound UDP socket that invokes a handler once per inbound datagram (with
 * the sender's address), for line/datagram services: syslog, statsd, service
 * discovery, DTLS front-ends, game/telemetry protocols, or a mock DNS server.
 *
 * A thin wrapper over KlUdp. It shares a KlEventCtx — it does NOT own a thread
 * or event loop — so one process can serve TCP HTTP (KlServer) and a UDP
 * service on a single loop. Multi-core scaling is horizontal via SO_REUSEPORT,
 * the same as the TCP server.
 *
 * Caller-owned struct (stack-allocatable). Not thread-safe.
 */

typedef struct KlUdpServer KlUdpServer;

/**
 * @brief Called once per received datagram, on the event-loop thread.
 *
 * @param s         Server the datagram arrived on (use kl_udp_server_reply).
 * @param data      Datagram payload (borrowed — valid only for this call).
 * @param len       Payload length.
 * @param src       Sender address (borrowed — copy to retain).
 * @param src_len   Length of @p src.
 * @param user_data Opaque pointer from kl_udp_server_init.
 */
typedef void (*KlUdpHandlerFn)(KlUdpServer *s, const void *data, size_t len,
                               const struct sockaddr *src, socklen_t src_len,
                               void *user_data);

/**
 * @brief UDP server configuration.
 */
typedef struct {
    const char  *bind_addr;      /**< Bind address; NULL = "0.0.0.0". */
    uint16_t     port;           /**< Bind port; 0 = ephemeral. */
    size_t       recv_buf_size;  /**< Per-datagram recv buffer; 0 = 2048. */
    size_t       max_send_queue; /**< Reply backpressure cap; 0 = 256 KiB. */
    int          reuse_port;     /**< SO_REUSEPORT for worker fan-out. */
    int          so_rcvbuf;      /**< SO_RCVBUF kernel receive buffer in bytes (0 = OS default). */
    int          so_sndbuf;      /**< SO_SNDBUF kernel send buffer in bytes (0 = OS default). */
    int          mmsg_batch;     /**< recvmmsg/sendmmsg batch size (Linux); 0 = default (16). */
    int          recv_gro;       /**< Enable UDP_GRO receive coalescing (Linux); coalesced buffers are split per-segment into the handler. */
    int          tos;            /**< IP_TOS / IPV6_TCLASS byte on outgoing datagrams (DSCP<<2 | ECN); 0 = default. */
    int          recv_tos;       /**< Deliver each datagram's TOS byte (read via kl_udp_recv_tos on the server's socket). */
    int          broadcast;      /**< SO_BROADCAST — allow broadcast replies (IPv4). */
    int          multicast_ttl;  /**< IP_MULTICAST_TTL / IPV6_MULTICAST_HOPS for multicast sends; 0 = default. */
    int          multicast_disable_loop; /**< 1 = disable local loopback of sent multicast. */
    unsigned     multicast_iface;/**< Egress interface index for multicast sends; 0 = default. */
    const char  *multicast_group;/**< Optional multicast group to join at init (after bind); NULL = none. */
    KlAllocator *alloc;          /**< NULL = event-context allocator. */
} KlUdpServerConfig;

/**
 * @brief UDP dispatch server. Caller-owned; initialize with kl_udp_server_init.
 */
struct KlUdpServer {
    KlUdp          udp;
    KlUdpHandlerFn handler;
    void          *user_data;
    KlError        last_error;
    struct sockaddr_storage local;   /* current datagram's local (dest) addr, if pktinfo */
    socklen_t      local_len;        /* 0 = unknown (reply uses the socket default) */
};

/**
 * @brief Bind a UDP socket and begin dispatching datagrams to @p handler.
 * @return 0 on success, -1 on failure (last_error set).
 */
int kl_udp_server_init(KlUdpServer *s, KlEventCtx *ctx,
                       const KlUdpServerConfig *cfg,
                       KlUdpHandlerFn handler, void *user_data);

/**
 * @brief Send a reply datagram (typically to the sender, from the handler).
 * @return 0 if sent or queued, -1 on error / over-cap (last_error set).
 */
int kl_udp_server_reply(KlUdpServer *s, const void *data, size_t len,
                        const struct sockaddr *dest, socklen_t dest_len);

/**
 * @brief Join / leave an any-source multicast group on the server socket.
 *
 * For dynamic membership beyond the optional init-time @c multicast_group. See
 * kl_udp_multicast_join for the @p group / @p iface_index semantics.
 * @return 0 on success, -1 on failure (last_error set).
 */
int kl_udp_server_multicast_join(KlUdpServer *s, const char *group, unsigned iface_index);
int kl_udp_server_multicast_leave(KlUdpServer *s, const char *group, unsigned iface_index);

/**
 * @brief Stop dispatching, close the socket, and free buffers.
 *
 * Does not free the KlUdpServer struct itself (caller-owned). Safe to call twice.
 */
void kl_udp_server_free(KlUdpServer *s);

/** @brief The local (bound) port in host byte order, or 0. */
uint16_t kl_udp_server_local_port(const KlUdpServer *s);

/** @brief The underlying socket fd, or -1. */
int kl_udp_server_fd(const KlUdpServer *s);

/** @brief Last error recorded on a -1 return. */
KlError kl_udp_server_last_error(const KlUdpServer *s);

#endif /* KEEL_UDP_SERVER_H */
