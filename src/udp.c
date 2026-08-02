#include <keel/udp.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "socket.h"   /* sockaddr / getaddrinfo / inet_ntop / ntohs via sockcompat.h */
#include "sockaddr_native.h" /* KlSockAddr <-> sockaddr (bind/connect/get_local_addr) */
#include "udp_internal.h"
#include "udp_io.h"
#include "event_caps.h"   /* kl_event_caps — pick readiness vs completion recv */
#include "io_engine.h"    /* kl_comp_post_udp_recv (completion loop) */

/* Some platforms spell the IPv6 group ops the "membership" way. */
#if !defined(IPV6_JOIN_GROUP) && defined(IPV6_ADD_MEMBERSHIP)
#define IPV6_JOIN_GROUP  IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#endif

/* recv/sendmmsg batch size defaults (the batch engines live in udp_io_posix.c). */
#define UDP_MMSG_DEFAULT 16
#define UDP_MMSG_MAX      64

/* ── Forward decls ────────────────────────────────────────────────────── */

static void udp_on_ready(KlSocketHandle fd, KlEventMask ready, void *user_data);

/* Reconcile event-loop interest with current recv/send state. */
void kl_udp_update_interest(KlUdp *udp) {
    if (!kl_handle_valid(udp->fd))
        return;
    KlEventMask want = 0;
    if (udp->recv_active) want |= KL_EVENT_READ;
    if (udp->q_head)      want |= KL_EVENT_WRITE;

    if (want == udp->want_mask)
        return;

    if (want == 0) {
        kl_watcher_del(udp->ctx, udp->fd);
        udp->watcher_added = 0;
        udp->want_mask = 0;
    } else if (!udp->watcher_added) {
        if (kl_watcher_add(udp->ctx, udp->fd, want, udp_on_ready, udp) == 0) {
            udp->watcher_added = 1;
            udp->want_mask = want;
        } else {
            udp->last_error = KL_ERR_EVENT_ADD;
        }
    } else {
        if (kl_watcher_mod(udp->ctx, udp->fd, want) == 0)
            udp->want_mask = want;
        else
            udp->last_error = KL_ERR_EVENT_ADD;
    }
}

/* ── Send path ────────────────────────────────────────────────────────── */

static int udp_enqueue(KlUdp *udp, const void *data, size_t len,
                       const struct sockaddr *dest, socklen_t dest_len,
                       const struct sockaddr *src, socklen_t src_len, int tos) {
    /* Backpressure cap (avoids max_send_queue - len underflow). */
    if (len > udp->max_send_queue || udp->q_bytes > udp->max_send_queue - len) {
        udp->dropped++;
        udp->last_error = KL_ERR_QUEUE_FULL;
        return -1;
    }
    if (len > SIZE_MAX - sizeof(KlUdpDatagram)) {
        udp->dropped++;
        udp->last_error = KL_ERR_OVERFLOW;
        return -1;
    }

    size_t node_sz = sizeof(KlUdpDatagram) + len;
    KlUdpDatagram *node = kl_malloc(udp->alloc, node_sz);
    if (!node) {
        udp->last_error = KL_ERR_ALLOC;
        return -1;
    }
    node->next = NULL;
    node->len = len;
    if (dest && dest_len) {
        memcpy(&node->dest, dest, dest_len);
        node->dest_len = dest_len;
    } else {
        node->dest_len = 0;
    }
    if (src && src_len) {
        memcpy(&node->src, src, src_len);
        node->src_len = src_len;
    } else {
        node->src_len = 0;
    }
    node->tos = tos;
    if (len)
        memcpy(node->data, data, len);

    if (udp->q_tail) udp->q_tail->next = node;
    else             udp->q_head = node;
    udp->q_tail = node;
    udp->q_bytes += len;

    kl_udp_update_interest(udp);   /* arm WRITE */
    return 0;
}

static int udp_send_common(KlUdp *udp, const void *data, size_t len,
                           const struct sockaddr *dest, socklen_t dest_len,
                           const struct sockaddr *src, socklen_t src_len, int tos) {
    if (!kl_handle_valid(udp->fd)) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }

    /* Completion loop (IOCP): post an overlapped WSASendTo (8b-4d). Plain sends
     * only — source-pinned / TOS sends fall through to the synchronous seam path,
     * which works on a completion socket too. q_bytes tracks outstanding overlapped
     * bytes for backpressure (the readiness send queue is unused on this loop);
     * on_drain fires when the last in-flight send completes (kl_udp_comp_on_send). */
    if (src == NULL && tos < 0 &&
        (kl_event_caps(&udp->ctx->loop) & KL_EVENT_CAP_COMPLETION)) {
        if (len > udp->max_send_queue || udp->q_bytes > udp->max_send_queue - len) {
            udp->last_error = KL_ERR_IO;   /* backpressure cap — drop the datagram */
            return -1;
        }
        if (kl_comp_post_udp_send(udp, data, len, dest, (int)dest_len) < 0) {
            udp->last_error = KL_ERR_IO;
            return -1;
        }
        udp->q_bytes += len;
        return 0;
    }

    /* Preserve ordering: if anything is queued, queue behind it. */
    if (udp->q_head)
        return udp_enqueue(udp, data, len, dest, dest_len, src, src_len, tos);

    ssize_t n = kl_udp_io_raw_send(udp, data, len, dest, dest_len, src, src_len, tos);
    if (n >= 0)
        return 0;   /* UDP send is all-or-nothing — no short-send handling */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return udp_enqueue(udp, data, len, dest, dest_len, src, src_len, tos);

    udp->last_error = KL_ERR_IO;
    return -1;
}

/* ── Receive path ─────────────────────────────────────────────────────── */

/* Deliver one received buffer. A GRO-coalesced buffer (gro_seg > 0 and
 * len > gro_seg) goes whole to on_recv_segments when set, else is split into
 * per-segment on_recv calls; a plain datagram goes to on_recv. */
void kl_udp_deliver(KlUdp *udp, const void *data, size_t len, int gro_seg,
                    const struct sockaddr *src, socklen_t src_len,
                    const struct sockaddr *local, socklen_t local_len) {
    /* Marshal the platform recv addresses to the neutral KlSockAddr the public
     * callbacks speak (the io/completion layers below still work in sockaddr). */
    KlSockAddr ksrc, klocal;
    const KlSockAddr *ksrc_p = NULL, *klocal_p = NULL;
    if (src && src_len && kl_sockaddr_from_native(&ksrc, src, src_len) == 0)
        ksrc_p = &ksrc;
    if (local && local_len && kl_sockaddr_from_native(&klocal, local, local_len) == 0)
        klocal_p = &klocal;

    if (gro_seg > 0 && len > (size_t)gro_seg) {
        if (udp->on_recv_segments) {
            udp->on_recv_segments(udp, data, len, (size_t)gro_seg,
                                  ksrc_p, klocal_p, udp->recv_seg_ud);
            return;
        }
        size_t seg = (size_t)gro_seg, off = 0;
        while (off < len) {
            size_t chunk = (len - off < seg) ? (len - off) : seg;
            if (udp->on_recv)
                udp->on_recv(udp, (const char *)data + off, chunk,
                             ksrc_p, klocal_p, udp->recv_ud);
            if (!udp->recv_active || !kl_handle_valid(udp->fd))
                return;   /* callback stopped/freed mid-split */
            off += chunk;
        }
        return;
    }
    if (udp->on_recv)
        udp->on_recv(udp, data, len, ksrc_p, klocal_p, udp->recv_ud);
}

static void udp_on_ready(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)fd;
    KlUdp *udp = user_data;
    if (ready & KL_EVENT_WRITE)
        kl_udp_io_flush_queue(udp);
    if (kl_handle_valid(udp->fd) && udp->recv_active && (ready & KL_EVENT_READ))
        kl_udp_io_recv_drain(udp);
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

int kl_udp_init(KlUdp *udp, const KlUdpConfig *cfg) {
    if (!udp)
        return -1;
    memset(udp, 0, sizeof(*udp));
    udp->fd = KL_INVALID_SOCKET;

    if (!cfg || !cfg->ctx) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    udp->ctx = cfg->ctx;
    udp->alloc = cfg->alloc ? cfg->alloc : cfg->ctx->alloc;
    if (!udp->alloc) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }

    size_t rbs = cfg->recv_buf_size ? cfg->recv_buf_size : 2048;
    if (rbs > 65535) rbs = 65535;
    udp->recv_buf_size = rbs;
    udp->max_send_queue = cfg->max_send_queue ? cfg->max_send_queue : (256u * 1024u);

    /* recv/sendmmsg batch size: 0 = default, clamp to [1, UDP_MMSG_MAX]. */
    int batch = cfg->mmsg_batch > 0 ? cfg->mmsg_batch : UDP_MMSG_DEFAULT;
    if (batch > UDP_MMSG_MAX) batch = UDP_MMSG_MAX;
    if (batch < 1) batch = 1;
    udp->mmsg_batch = batch;

    int family = cfg->family;
    struct addrinfo *ai = NULL;
    if (cfg->bind_addr) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = (family == AF_INET || family == AF_INET6) ? family : AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)cfg->bind_port);
        int rc = getaddrinfo(cfg->bind_addr, port_str, &hints, &ai);
        if (rc != 0 || !ai) {
            udp->last_error = KL_ERR_DNS;
            return -1;
        }
        family = ai->ai_family;
    } else if (family != AF_INET && family != AF_INET6) {
        family = AF_INET;
    }

    udp->fd = kl_sock_socket(udp->ctx->sockets, family, SOCK_DGRAM, 0);
    if (!kl_handle_valid(udp->fd)) {
        udp->last_error = KL_ERR_SOCKET;
        goto fail;
    }
    udp->family = family;
    udp->recv_tos_val = -1;
    kl_sock_set_cloexec(udp->ctx->sockets, udp->fd);
    if (kl_sock_set_nonblocking(udp->ctx->sockets, udp->fd) < 0) {
        udp->last_error = KL_ERR_SOCKET;
        goto fail;
    }

    /* SO_REUSEADDR/REUSEPORT + kernel socket-buffer sizing (best-effort). */
    kl_udp_io_apply_socket_opts(udp->fd, cfg);

    /* Broadcast + multicast transmit options (best-effort). */
    kl_udp_io_apply_tx_options(udp->fd, family, cfg);

    /* Per-datagram local-address / GRO / TOS receive options (best-effort;
     * pktinfo/platform-specific — lives behind the datagram-I/O seam). */
    kl_udp_io_setup_recv_opts(udp, cfg);

    if (ai) {
        KlSockAddr bind_sa;
        kl_sockaddr_from_native(&bind_sa, ai->ai_addr, ai->ai_addrlen);
        if (kl_sock_bind(udp->ctx->sockets, udp->fd, &bind_sa) < 0) {
            udp->last_error = KL_ERR_BIND;
            goto fail;
        }
        freeaddrinfo(ai);
        ai = NULL;
    }

    /* Optional init-time multicast join (requires a bound socket). */
    if (cfg->multicast_group && cfg->multicast_group[0]) {
        int mrc = kl_udp_io_mcast_membership(udp, cfg->multicast_group,
                                       cfg->multicast_iface, 1);
        if (mrc != 0)
            goto fail;   /* last_error set by helper */
    }

    udp->recv_buf = kl_malloc(udp->alloc, udp->recv_buf_size);
    if (!udp->recv_buf) {
        udp->last_error = KL_ERR_ALLOC;
        goto fail;
    }

    /* Allocate the (small) sendmmsg batch upfront where mmsg batching applies;
     * the recvmmsg batch (which carries the large data block) is allocated
     * lazily in kl_udp_recv_start. Failure just disables batching (an
     * optimization) — a no-op where batching is unavailable. */
    kl_udp_io_tx_batch_init(udp);
    return 0;

fail:
    if (ai) freeaddrinfo(ai);
    if (kl_handle_valid(udp->fd)) {
        kl_sock_close(udp->ctx->sockets, udp->fd);
        udp->fd = KL_INVALID_SOCKET;
    }
    return -1;
}

void kl_udp_free(KlUdp *udp) {
    if (!udp)
        return;
    udp->recv_active = 0;
    if (udp->watcher_added && kl_handle_valid(udp->fd))
        kl_watcher_del(udp->ctx, udp->fd);
    udp->watcher_added = 0;
    udp->want_mask = 0;

    KlUdpDatagram *node = udp->q_head;
    while (node) {
        KlUdpDatagram *next = node->next;
        kl_free(udp->alloc, node, sizeof(KlUdpDatagram) + node->len);
        node = next;
    }
    udp->q_head = udp->q_tail = NULL;
    udp->q_bytes = 0;

    if (udp->recv_buf) {
        kl_free(udp->alloc, udp->recv_buf, udp->recv_buf_size);
        udp->recv_buf = NULL;
    }
    kl_udp_io_batch_free(udp);
    if (kl_handle_valid(udp->fd)) {
        kl_sock_close(udp->ctx->sockets, udp->fd);
        udp->fd = KL_INVALID_SOCKET;
    }
}

int kl_udp_connect(KlUdp *udp, const KlSockAddr *peer) {
    if (!udp || !kl_handle_valid(udp->fd) || !peer) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (kl_sock_connect(udp->ctx->sockets, udp->fd, peer) < 0) {
        udp->last_error = KL_ERR_CONNECT;
        return -1;
    }
    udp->connected = 1;
    return 0;
}

int kl_udp_recv_start(KlUdp *udp, KlUdpRecvFn on_recv, void *user_data) {
    if (!udp || !kl_handle_valid(udp->fd) || !on_recv) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    udp->on_recv = on_recv;
    udp->recv_ud = user_data;
    udp->recv_active = 1;
    /* Lazily allocate the recvmmsg batch (large: mmsg_batch × recv_buf_size)
     * where mmsg batching applies. Failure just leaves rx_batch NULL → the
     * per-datagram path is used; a no-op where batching is unavailable. */
    kl_udp_io_rx_batch_init(udp);

    /* Completion loop (IOCP): a readiness watcher never fires here — associate the
     * socket with the port and post an overlapped WSARecvFrom instead (8b-4c). The
     * completion driver re-posts after each datagram via kl_udp_comp_on_recv. */
    if (kl_event_caps(&udp->ctx->loop) & KL_EVENT_CAP_COMPLETION) {
        if (kl_event_add(&udp->ctx->loop, udp->fd, KL_EVENT_READ, udp) < 0 ||
            kl_comp_post_udp_recv(udp) < 0) {
            udp->recv_active = 0;
            udp->last_error = KL_ERR_IO;
            return -1;
        }
        return 0;
    }

    kl_udp_update_interest(udp);
    if (!(udp->want_mask & KL_EVENT_READ)) {   /* watcher registration failed */
        udp->recv_active = 0;
        return -1;
    }
    return 0;
}

/* Completion-loop datagram receive (8b-4c): deliver the finished WSARecvFrom via the
 * model-blind kl_udp_deliver (identical to the readiness recvmsg path), then re-post
 * the next receive. gro_seg 0 — GRO coalescing is a readiness/Linux offload. */
void kl_udp_comp_on_recv(KlUdp *udp, const void *buf, size_t len,
                         const struct sockaddr *src, socklen_t src_len,
                         const KlUdpRxMeta *meta) {
    if (!udp->recv_active || !kl_handle_valid(udp->fd))
        return;
    if (meta->truncated)
        udp->truncated++;
    kl_udp_deliver(udp, buf, len, meta->gro_seg, src, src_len, meta->local, meta->local_len);
    if (udp->recv_active && kl_handle_valid(udp->fd))
        (void)kl_comp_post_udp_recv(udp);   /* arm the next datagram */
}

/* An overlapped WSASendTo of `len` bytes finished (8b-4d): release its
 * outstanding-bytes reservation; fire on_drain when nothing is left in flight
 * (same public semantics as the readiness queue emptying). */
void kl_udp_comp_on_send(KlUdp *udp, size_t len) {
    udp->q_bytes = (udp->q_bytes >= len) ? udp->q_bytes - len : 0;
    if (udp->q_bytes == 0 && udp->on_drain)
        udp->on_drain(udp, udp->drain_ud);
}

void kl_udp_recv_stop(KlUdp *udp) {
    if (!udp)
        return;
    udp->recv_active = 0;
    kl_udp_update_interest(udp);
}

int kl_udp_send_to(KlUdp *udp, const void *data, size_t len,
                   const KlSockAddr *dest) {
    return kl_udp_send_to_from(udp, data, len, dest, NULL);
}

int kl_udp_send_to_from(KlUdp *udp, const void *data, size_t len,
                        const KlSockAddr *dest, const KlSockAddr *src) {
    if (!udp || (!data && len) || !dest) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    struct sockaddr_storage ds, ss;
    socklen_t dl = kl_sockaddr_to_native(dest, &ds);
    socklen_t sl = src ? kl_sockaddr_to_native(src, &ss) : 0;
    if (dl == 0 || (src && sl == 0)) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_send_common(udp, data, len, (struct sockaddr *)&ds, dl,
                           src ? (struct sockaddr *)&ss : NULL, sl, -1);
}

int kl_udp_send_to_tos(KlUdp *udp, const void *data, size_t len,
                       const KlSockAddr *dest, int tos) {
    if (!udp || (!data && len) || !dest || tos < 0 || tos > 255) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    struct sockaddr_storage ds;
    socklen_t dl = kl_sockaddr_to_native(dest, &ds);
    if (dl == 0) { udp->last_error = KL_ERR_INVALID_ARG; return -1; }
    return udp_send_common(udp, data, len, (struct sockaddr *)&ds, dl, NULL, 0, tos);
}

int kl_udp_set_tos(KlUdp *udp, int tos) {
    if (!udp || !kl_handle_valid(udp->fd) || tos < 0 || tos > 255) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (kl_udp_io_set_tos(udp->fd, udp->family, tos) == 0)
        return 0;
    udp->last_error = KL_ERR_SOCKET;
    return -1;
}

int kl_udp_recv_tos(const KlUdp *udp) {
    return udp ? udp->recv_tos_val : -1;
}

int kl_udp_send(KlUdp *udp, const void *data, size_t len) {
    if (!udp || (!data && len)) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (!udp->connected) {
        udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_send_common(udp, data, len, NULL, 0, NULL, 0, -1);
}

/* Portable GSO fallback: send each segment individually via the normal path
 * (immediate send, or queued under backpressure — order preserved). */
static int udp_send_segments(KlUdp *udp, const void *buf, size_t total_len,
                             size_t seg, const struct sockaddr *dest,
                             socklen_t dest_len) {
    size_t off = 0;
    while (off < total_len) {
        size_t chunk = (total_len - off < seg) ? (total_len - off) : seg;
        if (udp_send_common(udp, (const char *)buf + off, chunk,
                            dest, dest_len, NULL, 0, -1) != 0)
            return -1;   /* over-cap / hard error — remaining segments dropped */
        off += chunk;
    }
    return 0;
}

int kl_udp_send_gso(KlUdp *udp, const void *buf, size_t total_len,
                    size_t segment_size, const KlSockAddr *dest) {
    if (!udp || (!buf && total_len) || total_len == 0 || segment_size == 0 ||
        segment_size > total_len || total_len > 65507 || !dest) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    struct sockaddr_storage ds;
    socklen_t dest_len = kl_sockaddr_to_native(dest, &ds);
    if (dest_len == 0) { udp->last_error = KL_ERR_INVALID_ARG; return -1; }
    const struct sockaddr *dest_sa = (const struct sockaddr *)&ds;
    /* Single-syscall GSO when there is more than one segment, nothing is already
     * queued (preserve ordering), and GSO hasn't been ruled unsupported. The
     * seam returns -1/EOPNOTSUPP where GSO is unavailable at build time. */
    if (segment_size < total_len && !udp->q_head && !udp->gso_disabled) {
        ssize_t n = kl_udp_io_send_gso(udp, buf, total_len, (uint16_t)segment_size,
                                       dest_sa, dest_len);
        if (n >= 0)
            return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            udp->gso_disabled = 1;   /* GSO unsupported here — stop trying */
        /* EAGAIN or unsupported: fall through and send/queue per-segment. */
    }
    return udp_send_segments(udp, buf, total_len, segment_size, dest_sa, dest_len);
}

void kl_udp_recv_segments(KlUdp *udp, KlUdpRecvSegmentsFn cb, void *user_data) {
    if (!udp) return;
    udp->on_recv_segments = cb;
    udp->recv_seg_ud = user_data;
}

int kl_udp_multicast_join(KlUdp *udp, const char *group, unsigned iface_index) {
    if (!udp || !kl_handle_valid(udp->fd) || !group) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return kl_udp_io_mcast_membership(udp, group, iface_index, 1);
}

int kl_udp_multicast_leave(KlUdp *udp, const char *group, unsigned iface_index) {
    if (!udp || !kl_handle_valid(udp->fd) || !group) {
        if (udp) udp->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return kl_udp_io_mcast_membership(udp, group, iface_index, 0);
}

void kl_udp_on_drain(KlUdp *udp, KlUdpDrainFn cb, void *user_data) {
    if (!udp) return;
    udp->on_drain = cb;
    udp->drain_ud = user_data;
}

size_t   kl_udp_send_queued(const KlUdp *udp) { return udp ? udp->q_bytes : 0; }
uint64_t kl_udp_dropped(const KlUdp *udp)     { return udp ? udp->dropped : 0; }
uint64_t kl_udp_truncated(const KlUdp *udp)   { return udp ? udp->truncated : 0; }
KlSocketHandle kl_udp_fd(const KlUdp *udp)    { return udp ? udp->fd : KL_INVALID_SOCKET; }
KlError  kl_udp_last_error(const KlUdp *udp)  { return udp ? udp->last_error : KL_ERR_INVALID_ARG; }

uint16_t kl_udp_local_port(const KlUdp *udp) {
    if (!udp || !kl_handle_valid(udp->fd))
        return 0;
    KlSockAddr la;
    if (kl_sock_get_local_addr(udp->ctx->sockets, udp->fd, &la) != 0)
        return 0;
    return kl_sockaddr_port(&la);
}
