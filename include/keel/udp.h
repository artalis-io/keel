#ifndef KEEL_UDP_H
#define KEEL_UDP_H

#include <keel/allocator.h>
#include <keel/error.h>
#include <keel/event_ctx.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

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

/** @brief Internal queued-datagram node (defined in src/udp.c). */
typedef struct KlUdpDatagram KlUdpDatagram;

/**
 * @brief Called once per received datagram, on the event-loop thread.
 *
 * @param udp       Socket the datagram arrived on.
 * @param data      Datagram payload (borrowed — valid only for this call).
 * @param len       Payload length in bytes.
 * @param src       Sender address (borrowed — copy to retain).
 * @param src_len   Length of @p src.
 * @param user_data Opaque pointer from kl_udp_recv_start.
 */
typedef void (*KlUdpRecvFn)(KlUdp *udp, const void *data, size_t len,
                            const struct sockaddr *src, socklen_t src_len,
                            void *user_data);

/**
 * @brief Called when the send queue transitions from non-empty back to empty.
 */
typedef void (*KlUdpDrainFn)(KlUdp *udp, void *user_data);

/**
 * @brief UDP socket configuration.
 */
typedef struct {
    KlEventCtx  *ctx;             /**< Event loop (borrowed — must outlive udp). */
    int          family;         /**< AF_INET / AF_INET6 / AF_UNSPEC (auto from bind_addr). */
    const char  *bind_addr;      /**< Numeric bind address, or NULL for an unbound socket. */
    uint16_t     bind_port;      /**< Bind port, 0 for ephemeral (ignored if bind_addr is NULL). */
    size_t       recv_buf_size;  /**< Per-datagram recv buffer; 0 = 2048, capped at 65535. */
    size_t       max_send_queue; /**< Backpressure cap in bytes; 0 = 256 KiB. */
    int          reuse_addr;     /**< Set SO_REUSEADDR. */
    int          reuse_port;     /**< Set SO_REUSEPORT (fan-out across workers). */
    KlAllocator *alloc;          /**< NULL = default allocator. */
} KlUdpConfig;

/**
 * @brief Non-blocking UDP socket. Caller-owned; initialize with kl_udp_init.
 */
struct KlUdp {
    KlEventCtx    *ctx;
    KlAllocator   *alloc;
    int            fd;
    int            connected;        /**< 1 after kl_udp_connect. */
    /* Receive */
    KlUdpRecvFn    on_recv;
    void          *recv_ud;
    int            recv_active;
    unsigned char *recv_buf;
    size_t         recv_buf_size;
    struct sockaddr_storage recv_src; /**< Scratch for the current datagram's source. */
    uint64_t       truncated;        /**< Count of oversized (truncated) datagrams. */
    /* Send queue (whole-datagram FIFO) */
    KlUdpDatagram *q_head;
    KlUdpDatagram *q_tail;
    size_t         q_bytes;          /**< Payload bytes currently queued. */
    size_t         max_send_queue;
    uint64_t       dropped;          /**< Datagrams dropped over the cap. */
    KlUdpDrainFn   on_drain;
    void          *drain_ud;
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
int kl_udp_connect(KlUdp *udp, const struct sockaddr *peer, socklen_t peer_len);

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
                   const struct sockaddr *dest, socklen_t dest_len);

/**
 * @brief Send a datagram to the connected peer (requires kl_udp_connect).
 * @return 0 if sent or queued, -1 on error or over-cap (last_error set).
 */
int kl_udp_send(KlUdp *udp, const void *data, size_t len);

/** @brief Register the send-queue-drained callback (NULL to clear). */
void kl_udp_on_drain(KlUdp *udp, KlUdpDrainFn cb, void *user_data);

/** @brief Bytes currently waiting in the send queue. */
size_t kl_udp_send_queued(const KlUdp *udp);

/** @brief Count of datagrams dropped because the send queue was at its cap. */
uint64_t kl_udp_dropped(const KlUdp *udp);

/** @brief Count of inbound datagrams truncated because they exceeded recv_buf_size. */
uint64_t kl_udp_truncated(const KlUdp *udp);

/** @brief The underlying socket fd (for getsockname etc.), or -1. */
int kl_udp_fd(const KlUdp *udp);

/** @brief The local (bound) port in host byte order, or 0 if unbound/unknown. */
uint16_t kl_udp_local_port(const KlUdp *udp);

/** @brief Last error recorded on a -1 return. */
KlError kl_udp_last_error(const KlUdp *udp);

#endif /* KEEL_UDP_H */
