#include <keel/udp.h>

#include <errno.h>
#include <stddef.h>   /* offsetof — recover KlUdp from its embedded KlDatagram */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "socket.h"   /* seam: kl_sock_* + KlSockAddr + KlSocketProvider.dgram */
#include <keel/datagram.h>   /* KlDatagramOps — the provider datagram data-plane */
#include "udp_internal.h"
#include "event_caps.h"   /* kl_event_caps — pick readiness vs completion recv */
#include "io_engine.h"    /* kl_comp_post_dgram_recv (completion loop) */
#include "completion.h"   /* KlCompletionEvent — the comp_udp_dispatch hook */
#include <keel/event_ctx.h>  /* KlEventCtx (comp_udp_dispatch hook + kl_sockaddr_family) */
#include <keel/sockaddr.h>   /* kl_sockaddr_family — neutral src/local from the event */

/* Some platforms spell the IPv6 group ops the "membership" way. */
#if !defined(IPV6_JOIN_GROUP) && defined(IPV6_ADD_MEMBERSHIP)
#define IPV6_JOIN_GROUP  IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#endif

/* recv/sendmmsg batch size defaults (the batch engines live in socket_dgram_posix.c). */
#define UDP_MMSG_DEFAULT 16
#define UDP_MMSG_MAX      64

/* ── Forward decls ────────────────────────────────────────────────────── */

static void udp_on_ready(KlSocketHandle fd, KlEventMask ready, void *user_data);

/* Recover the owning KlUdp from its embedded KlDatagram (the completion target). ONLY the UDP
 * adapter recovers KlUdp — the completion backends hold a KlDatagram* and never dereference KlUdp.
 * Mirrors kl_conn_of_stream (a completion backend never derefs a KlConn). */
static inline KlUdp *kl_udp_of_dg(KlDatagram *dg) {
    return (KlUdp *)((char *)dg - offsetof(KlUdp, dg));
}

/* The provider's datagram data-plane (KlSocketProvider.dgram). NULL sockets =
 * the built-in default provider → its default datagram ops (kl_sockdef_dgram). */
static inline const KlDatagramOps *udp_dg(const KlUdp *u) {
    const KlSocketProvider *sp = u->dg.ctx ? u->dg.ctx->sockets : NULL;
    /* NULL sockets = the built-in default provider (mirrors the kl_sockdef_* stream
     * fallback) → its default datagram ops. A concrete provider must supply .dgram. */
    return sp ? sp->dgram : kl_sockdef_dgram();
}
static inline void *udp_sp_ctx(const KlUdp *u) {
    const KlSocketProvider *sp = u->dg.ctx ? u->dg.ctx->sockets : NULL;
    return sp ? sp->context : NULL;
}

/* Reconcile event-loop interest with current recv/send state. */
void kl_udp_update_interest(KlUdp *udp) {
    if (!kl_handle_valid(udp->dg.fd))
        return;
    KlEventMask want = 0;
    if (udp->dg.recv_active) want |= KL_EVENT_READ;
    if (udp->dg.q_head)      want |= KL_EVENT_WRITE;

    if (want == udp->dg.want_mask)
        return;

    if (want == 0) {
        kl_watcher_del(udp->dg.ctx, udp->dg.fd);
        udp->dg.watcher_added = 0;
        udp->dg.want_mask = 0;
    } else if (!udp->dg.watcher_added) {
        if (kl_watcher_add(udp->dg.ctx, udp->dg.fd, want, udp_on_ready, udp) == 0) {
            udp->dg.watcher_added = 1;
            udp->dg.want_mask = want;
        } else {
            udp->dg.last_error = KL_ERR_EVENT_ADD;
        }
    } else {
        if (kl_watcher_mod(udp->dg.ctx, udp->dg.fd, want) == 0)
            udp->dg.want_mask = want;
        else
            udp->dg.last_error = KL_ERR_EVENT_ADD;
    }
}

/* ── Send path ────────────────────────────────────────────────────────── */

static int udp_enqueue(KlUdp *udp, const void *data, size_t len,
                       const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    /* Backpressure cap (avoids max_send_queue - len underflow). */
    if (len > udp->dg.max_send_queue || udp->dg.q_bytes > udp->dg.max_send_queue - len) {
        udp->dg.dropped++;
        udp->dg.last_error = KL_ERR_QUEUE_FULL;
        return -1;
    }
    if (len > SIZE_MAX - sizeof(KlUdpDatagram)) {
        udp->dg.dropped++;
        udp->dg.last_error = KL_ERR_OVERFLOW;
        return -1;
    }

    size_t node_sz = sizeof(KlUdpDatagram) + len;
    KlUdpDatagram *node = kl_malloc(udp->dg.alloc, node_sz);
    if (!node) {
        udp->dg.last_error = KL_ERR_ALLOC;
        return -1;
    }
    node->next = NULL;
    node->len = len;
    /* KL_AF_UNSPEC (zeroed) = "unset": connected send / no source pin. */
    if (dest) node->dest = *dest; else memset(&node->dest, 0, sizeof(node->dest));
    if (src)  node->src  = *src;  else memset(&node->src,  0, sizeof(node->src));
    node->tos = tos;
    if (len)
        memcpy(node->data, data, len);

    if (udp->dg.q_tail) udp->dg.q_tail->next = node;
    else             udp->dg.q_head = node;
    udp->dg.q_tail = node;
    udp->dg.q_bytes += len;

    kl_udp_update_interest(udp);   /* arm WRITE */
    return 0;
}

static int udp_send_common(KlUdp *udp, const void *data, size_t len,
                           const KlSockAddr *dest, const KlSockAddr *src, int tos) {
    if (!kl_handle_valid(udp->dg.fd)) {
        udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }

    /* Completion loop (IOCP): post an overlapped WSASendTo (8b-4d). Plain sends
     * only — source-pinned / TOS sends fall through to the synchronous seam path,
     * which works on a completion socket too. q_bytes tracks outstanding overlapped
     * bytes for backpressure (the readiness send queue is unused on this loop);
     * on_drain fires when the last in-flight send completes (kl_udp_comp_on_send). */
    if (src == NULL && tos < 0 &&
        (kl_event_caps(&udp->dg.ctx->loop) & KL_EVENT_CAP_COMPLETION)) {
        if (len > udp->dg.max_send_queue || udp->dg.q_bytes > udp->dg.max_send_queue - len) {
            udp->dg.last_error = KL_ERR_IO;   /* backpressure cap — drop the datagram */
            return -1;
        }
        if (kl_comp_post_dgram_send(&udp->dg, data, len, dest) < 0) {
            udp->dg.last_error = KL_ERR_IO;
            return -1;
        }
        udp->dg.q_bytes += len;
        return 0;
    }

    /* Preserve ordering: if anything is queued, queue behind it. */
    if (udp->dg.q_head)
        return udp_enqueue(udp, data, len, dest, src, tos);

    ssize_t n = udp_dg(udp)->send(udp_sp_ctx(udp), udp->dg.fd, data, len, dest, src, tos);
    if (n >= 0)
        return 0;   /* UDP send is all-or-nothing — no short-send handling */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return udp_enqueue(udp, data, len, dest, src, tos);

    udp->dg.last_error = KL_ERR_IO;
    return -1;
}

/* ── Receive path ─────────────────────────────────────────────────────── */

/* Deliver one received buffer. A GRO-coalesced buffer (gro_seg > 0 and
 * len > gro_seg) goes whole to on_recv_segments when set, else is split into
 * per-segment on_recv calls; a plain datagram goes to on_recv. */
void kl_udp_deliver(KlUdp *udp, const void *data, size_t len, int gro_seg,
                    const KlSockAddr *src, const KlSockAddr *local) {
    /* Addresses arrive already marshalled to KlSockAddr by the datagram provider
     * (KlDatagramOps, which owns its sockaddr layout). Here we just fan out. */
    if (gro_seg > 0 && len > (size_t)gro_seg) {
        if (udp->on_recv_segments) {
            udp->on_recv_segments(udp, data, len, (size_t)gro_seg,
                                  src, local, udp->recv_seg_ud);
            return;
        }
        size_t seg = (size_t)gro_seg, off = 0;
        while (off < len) {
            size_t chunk = (len - off < seg) ? (len - off) : seg;
            if (udp->on_recv)
                udp->on_recv(udp, (const char *)data + off, chunk,
                             src, local, udp->recv_ud);
            if (!udp->dg.recv_active || !kl_handle_valid(udp->dg.fd))
                return;   /* callback stopped/freed mid-split */
            off += chunk;
        }
        return;
    }
    if (udp->on_recv)
        udp->on_recv(udp, data, len, src, local, udp->recv_ud);
}

/* ── Datagram-provider dispatch ───────────────────────────────────────────
 * The datagram data-plane is a provider vtable (KlSocketProvider.dgram) folded
 * onto the socket provider, so one runtime provider owns stream + datagram I/O.
 * The provider supplies primitives (send/recv/configure/batch); the send-queue
 * walk, delivery, and interest tracking (the machine) live here. A provider
 * without dgram ops cannot do datagrams — kl_udp_init rejects it, so udp_dg() is
 * non-NULL on every live KlUdp.
 * (udp_dg / udp_sp_ctx are declared near the top — used by the send path too.) */

/* Dequeue+free the first `k` queued datagrams from the front (sent or dropped). */
static void udp_drop_front(KlUdp *udp, int k) {
    for (int i = 0; i < k && udp->dg.q_head; i++) {
        KlUdpDatagram *node = udp->dg.q_head;
        udp->dg.q_head = node->next;
        if (!udp->dg.q_head) udp->dg.q_tail = NULL;
        udp->dg.q_bytes -= node->len;
        kl_free(udp->dg.alloc, node, sizeof(KlUdpDatagram) + node->len);
    }
}

/* Drain the send queue through the provider datagram ops: sendmmsg batches when a
 * tx batch + send_batch op are present, else one send() per node. */
static void udp_flush_dgram(KlUdp *udp, const KlDatagramOps *dg) {
    void *ctx = udp_sp_ctx(udp);
    int had_data = (udp->dg.q_head != NULL);

    if (udp->dg.tx_batch && dg->send_batch) {
        KlDgramTxDesc descs[UDP_MMSG_MAX];
        while (udp->dg.q_head) {
            int cnt = 0;
            for (const KlUdpDatagram *node = udp->dg.q_head;
                 node && cnt < udp->dg.mmsg_batch && cnt < UDP_MMSG_MAX;
                 node = node->next, cnt++) {
                descs[cnt].data = node->data; descs[cnt].len = node->len;
                descs[cnt].dest = node->dest; descs[cnt].src = node->src;
                descs[cnt].tos  = node->tos;
            }
            if (cnt == 0) break;
            int sent = dg->send_batch(ctx, udp->dg.fd, udp->dg.tx_batch, descs, cnt);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                udp->dg.last_error = KL_ERR_IO; udp->dg.dropped++;
                udp_drop_front(udp, 1); continue;   /* drop the head, retry */
            }
            udp_drop_front(udp, sent);
            if (sent < cnt) break;
        }
    } else {
        while (udp->dg.q_head) {
            KlUdpDatagram *node = udp->dg.q_head;
            kl_ssize_t n = dg->send(ctx, udp->dg.fd, node->data, node->len,
                                    &node->dest, &node->src, node->tos);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                udp->dg.last_error = KL_ERR_IO; udp->dg.dropped++;
            }
            udp_drop_front(udp, 1);
        }
    }
    kl_udp_update_interest(udp);
    if (had_data && udp->dg.q_head == NULL && udp->on_drain)
        udp->on_drain(udp, udp->drain_ud);
}

/* Deliver one KlDgramRxSlot/recv result to the machine. */
static void udp_deliver_slot(KlUdp *udp, const void *data, size_t len,
                             const KlSockAddr *src, const KlDgramRxMeta *meta) {
    if (meta->truncated) udp->dg.truncated++;
    udp->dg.recv_tos_val = meta->tos;
    kl_udp_deliver(udp, data, len, meta->gro_seg,
                   (src && kl_sockaddr_family(src) != KL_AF_UNSPEC) ? src : NULL,
                   meta->has_local ? &meta->local : NULL);
}

/* Drain the socket through the provider datagram ops: recvmmsg batches when an rx
 * batch + recv_batch op are present, else one recv() per datagram. */
static void udp_recv_dgram(KlUdp *udp, const KlDatagramOps *dg) {
    void *ctx = udp_sp_ctx(udp);

    if (udp->dg.rx_batch && dg->recv_batch) {
        KlDgramRxSlot slots[UDP_MMSG_MAX];
        int max = udp->dg.mmsg_batch < UDP_MMSG_MAX ? udp->dg.mmsg_batch : UDP_MMSG_MAX;
        for (;;) {
            int cnt = dg->recv_batch(ctx, udp->dg.fd, udp->dg.rx_batch, slots, max);
            if (cnt < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) udp->dg.last_error = KL_ERR_IO;
                break;
            }
            if (cnt == 0) break;
            for (int i = 0; i < cnt; i++) {
                udp_deliver_slot(udp, slots[i].data, slots[i].len, &slots[i].src, &slots[i].meta);
                if (!udp->dg.recv_active || !kl_handle_valid(udp->dg.fd)) return;
            }
            if (cnt < max) break;   /* fewer than a full batch → socket drained */
        }
        return;
    }

    for (;;) {
        KlSockAddr src;
        KlDgramRxMeta meta;
        kl_ssize_t n = dg->recv(ctx, udp->dg.fd, udp->dg.recv_buf, udp->dg.recv_buf_size, &src, &meta);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) udp->dg.last_error = KL_ERR_IO;
            break;
        }
        udp_deliver_slot(udp, udp->dg.recv_buf, (size_t)n, &src, &meta);
        if (!udp->dg.recv_active || !kl_handle_valid(udp->dg.fd)) break;
    }
}

/* Is `group` a numeric multicast address of `family` (IPv4 224.0.0.0/4, IPv6
 * ff00::/8)? Validated here (platform-neutral) so the public API reports
 * KL_ERR_INVALID_ARG for a bad group vs KL_ERR_SOCKET for a setsockopt failure. */
static int udp_group_ok(int family, const char *group) {
    KlSockAddr a;
    if (kl_sockaddr_parse(&a, group, 0) != 0) return 0;
    if (family == AF_INET  && kl_sockaddr_family(&a) == KL_AF_INET)
        return (a.u.ip[0] & 0xF0) == 0xE0;
    if (family == AF_INET6 && kl_sockaddr_family(&a) == KL_AF_INET6)
        return a.u.ip[0] == 0xff;
    return 0;
}

/* Join/leave a multicast group through the provider datagram ops. */
static int udp_mcast_dispatch(KlUdp *udp, const char *group, unsigned iface, int join) {
    if (!udp_group_ok(udp->dg.family, group)) {
        udp->dg.last_error = KL_ERR_INVALID_ARG;   /* not a multicast addr / family mismatch */
        return -1;
    }
    int rc = udp_dg(udp)->mcast_membership(udp_sp_ctx(udp), udp->dg.fd, udp->dg.family,
                                           group, iface, join);
    if (rc != 0) udp->dg.last_error = KL_ERR_SOCKET;
    return rc;
}

static void udp_on_ready(KlSocketHandle fd, KlEventMask ready, void *user_data) {
    (void)fd;
    KlUdp *udp = user_data;
    const KlDatagramOps *dg = udp_dg(udp);
    if (ready & KL_EVENT_WRITE)
        udp_flush_dgram(udp, dg);
    if (kl_handle_valid(udp->dg.fd) && udp->dg.recv_active && (ready & KL_EVENT_READ))
        udp_recv_dgram(udp, dg);
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

int kl_udp_init(KlUdp *udp, const KlUdpConfig *cfg) {
    if (!udp)
        return -1;
    memset(udp, 0, sizeof(*udp));
    udp->dg.fd = KL_INVALID_SOCKET;

    if (!cfg || !cfg->ctx) {
        udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    udp->dg.ctx = cfg->ctx;
    /* Route datagram completions on this ctx to the UDP stack (set once; harmless if
     * set repeatedly by multiple UDP sockets sharing a ctx). On a readiness loop this
     * hook is simply never consulted. */
    cfg->ctx->comp_udp_dispatch = kl_udp_comp_dispatch;
    udp->dg.alloc = cfg->alloc ? cfg->alloc : cfg->ctx->alloc;
    if (!udp->dg.alloc) {
        udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    /* Datagram I/O is the socket provider's datagram vtable — a provider without it
     * (no KL_SOCK_CAP_DATAGRAM) cannot back a KlUdp. */
    if (!udp_dg(udp)) {
        udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }

    size_t rbs = cfg->recv_buf_size ? cfg->recv_buf_size : 2048;
    if (rbs > 65535) rbs = 65535;
    udp->dg.recv_buf_size = rbs;
    udp->dg.max_send_queue = cfg->max_send_queue ? cfg->max_send_queue : (256u * 1024u);

    /* recv/sendmmsg batch size: 0 = default, clamp to [1, UDP_MMSG_MAX]. */
    int batch = cfg->mmsg_batch > 0 ? cfg->mmsg_batch : UDP_MMSG_DEFAULT;
    if (batch > UDP_MMSG_MAX) batch = UDP_MMSG_MAX;
    if (batch < 1) batch = 1;
    udp->dg.mmsg_batch = batch;

    int family = cfg->family;
    KlSockAddr bind_sa;
    int have_bind = 0;
    if (cfg->bind_addr) {
        /* Numeric bind address (no DNS) — pure, works on lwIP too. */
        if (kl_sockaddr_parse(&bind_sa, cfg->bind_addr, cfg->bind_port) != 0) {
            udp->dg.last_error = KL_ERR_INVALID_ARG;
            return -1;
        }
        family = (kl_sockaddr_family(&bind_sa) == KL_AF_INET6) ? AF_INET6 : AF_INET;
        have_bind = 1;
    } else if (family != AF_INET && family != AF_INET6) {
        family = AF_INET;
    }

    udp->dg.fd = kl_sock_socket(udp->dg.ctx->sockets, family, SOCK_DGRAM, 0);
    if (!kl_handle_valid(udp->dg.fd)) {
        udp->dg.last_error = KL_ERR_SOCKET;
        goto fail;
    }
    udp->dg.family = family;
    udp->dg.recv_tos_val = -1;
    kl_sock_set_cloexec(udp->dg.ctx->sockets, udp->dg.fd);
    if (kl_sock_set_nonblocking(udp->dg.ctx->sockets, udp->dg.fd) < 0) {
        udp->dg.last_error = KL_ERR_SOCKET;
        goto fail;
    }

    /* Datagram socket-option setup (reuse/bufs, broadcast/TOS/multicast, and
     * per-datagram pktinfo/GRO/TOS capture): the provider's configure() folds all
     * three and reports the capture options the kernel accepted. */
    {
        uint32_t caps = udp_dg(udp)->configure(udp_sp_ctx(udp), udp->dg.fd, family, cfg);
        udp->dg.pktinfo  = (caps & KL_DGRAM_RX_PKTINFO) ? 1 : 0;
        udp->dg.recv_gro = (caps & KL_DGRAM_RX_GRO) ? 1 : 0;
        udp->dg.recv_tos = (caps & KL_DGRAM_RX_TOS) ? 1 : 0;
    }

    if (have_bind) {
        if (kl_sock_bind(udp->dg.ctx->sockets, udp->dg.fd, &bind_sa) < 0) {
            udp->dg.last_error = KL_ERR_BIND;
            goto fail;
        }
    }

    /* Optional init-time multicast join (requires a bound socket). */
    if (cfg->multicast_group && cfg->multicast_group[0]) {
        int mrc = udp_mcast_dispatch(udp, cfg->multicast_group, cfg->multicast_iface, 1);
        if (mrc != 0)
            goto fail;   /* last_error set by helper */
    }

    udp->dg.recv_buf = kl_malloc(udp->dg.alloc, udp->dg.recv_buf_size);
    if (!udp->dg.recv_buf) {
        udp->dg.last_error = KL_ERR_ALLOC;
        goto fail;
    }

    /* Allocate the (small) sendmmsg batch upfront where mmsg batching applies;
     * the recvmmsg batch (which carries the large data block) is allocated
     * lazily in kl_udp_recv_start. Failure just disables batching (an
     * optimization) — a no-op where batching is unavailable. */
    {
        const KlDatagramOps *dg = udp_dg(udp);
        if (udp->dg.mmsg_batch > 1 && dg->tx_batch_new && !udp->dg.tx_batch)
            udp->dg.tx_batch = dg->tx_batch_new(udp->dg.alloc, udp->dg.mmsg_batch);
    }
    return 0;

fail:
    if (kl_handle_valid(udp->dg.fd)) {
        kl_sock_close(udp->dg.ctx->sockets, udp->dg.fd);
        udp->dg.fd = KL_INVALID_SOCKET;
    }
    return -1;
}

void kl_udp_free(KlUdp *udp) {
    if (!udp)
        return;
    udp->dg.recv_active = 0;
    if (udp->dg.watcher_added && kl_handle_valid(udp->dg.fd))
        kl_watcher_del(udp->dg.ctx, udp->dg.fd);
    udp->dg.watcher_added = 0;
    udp->dg.want_mask = 0;

    KlUdpDatagram *node = udp->dg.q_head;
    while (node) {
        KlUdpDatagram *next = node->next;
        kl_free(udp->dg.alloc, node, sizeof(KlUdpDatagram) + node->len);
        node = next;
    }
    udp->dg.q_head = udp->dg.q_tail = NULL;
    udp->dg.q_bytes = 0;

    if (udp->dg.recv_buf) {
        kl_free(udp->dg.alloc, udp->dg.recv_buf, udp->dg.recv_buf_size);
        udp->dg.recv_buf = NULL;
    }
    {
        /* dg may be NULL here if init failed before the provider check (free is
         * called on the half-built udp) — guard it. */
        const KlDatagramOps *dg = udp_dg(udp);
        if (dg) {
            if (udp->dg.rx_batch && dg->rx_batch_free) dg->rx_batch_free(udp->dg.alloc, udp->dg.rx_batch);
            if (udp->dg.tx_batch && dg->tx_batch_free) dg->tx_batch_free(udp->dg.alloc, udp->dg.tx_batch);
        }
        udp->dg.rx_batch = udp->dg.tx_batch = NULL;
    }
    if (kl_handle_valid(udp->dg.fd)) {
        kl_sock_close(udp->dg.ctx->sockets, udp->dg.fd);
        udp->dg.fd = KL_INVALID_SOCKET;
    }
}

int kl_udp_connect(KlUdp *udp, const KlSockAddr *peer) {
    if (!udp || !kl_handle_valid(udp->dg.fd) || !peer) {
        if (udp) udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (kl_sock_connect(udp->dg.ctx->sockets, udp->dg.fd, peer) < 0) {
        udp->dg.last_error = KL_ERR_CONNECT;
        return -1;
    }
    udp->dg.connected = 1;
    return 0;
}

int kl_udp_recv_start(KlUdp *udp, KlUdpRecvFn on_recv, void *user_data) {
    if (!udp || !kl_handle_valid(udp->dg.fd) || !on_recv) {
        if (udp) udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    udp->on_recv = on_recv;
    udp->recv_ud = user_data;
    udp->dg.recv_active = 1;

    /* Completion loop (IOCP): a readiness watcher never fires here — associate the
     * socket with the port and post an overlapped WSARecvFrom instead (8b-4c). The
     * completion driver re-posts after each datagram via kl_udp_comp_on_recv. The
     * recvmmsg batch below is not allocated here: the completion path never runs the
     * readiness recv loop that consumes it. */
    if (kl_event_caps(&udp->dg.ctx->loop) & KL_EVENT_CAP_COMPLETION) {
        if (kl_event_add(&udp->dg.ctx->loop, udp->dg.fd, KL_EVENT_READ, udp) < 0 ||
            kl_comp_post_dgram_recv(&udp->dg) < 0) {
            udp->dg.recv_active = 0;
            udp->dg.last_error = KL_ERR_IO;
            return -1;
        }
        return 0;
    }

    /* Readiness path only: lazily allocate the recvmmsg batch (large: mmsg_batch ×
     * recv_buf_size) where mmsg batching applies. Failure just leaves rx_batch NULL
     * → the per-datagram path is used; a no-op where batching is unavailable. */
    {
        const KlDatagramOps *dg = udp_dg(udp);
        if (udp->dg.mmsg_batch > 1 && dg->rx_batch_new && !udp->dg.rx_batch)
            udp->dg.rx_batch = dg->rx_batch_new(udp->dg.alloc, udp->dg.mmsg_batch, udp->dg.recv_buf_size);
    }

    kl_udp_update_interest(udp);
    if (!(udp->dg.want_mask & KL_EVENT_READ)) {   /* watcher registration failed */
        udp->dg.recv_active = 0;
        return -1;
    }
    return 0;
}

/* Completion-loop datagram receive (8b-4c): deliver the finished WSARecvFrom via the
 * model-blind kl_udp_deliver (identical to the readiness recvmsg path), then re-post
 * the next receive. gro_seg 0 — GRO coalescing is a readiness/Linux offload. */
void kl_udp_comp_on_recv(KlUdp *udp, const void *buf, size_t len,
                         const KlSockAddr *src, const KlUdpRxMeta *meta) {
    if (!udp->dg.recv_active || !kl_handle_valid(udp->dg.fd))
        return;
    if (meta->truncated)
        udp->dg.truncated++;
    kl_udp_deliver(udp, buf, len, meta->gro_seg, src, meta->local);
    if (udp->dg.recv_active && kl_handle_valid(udp->dg.fd))
        (void)kl_comp_post_dgram_recv(&udp->dg);   /* arm the next datagram */
}

/* An overlapped WSASendTo of `len` bytes finished (8b-4d): release its
 * outstanding-bytes reservation; fire on_drain when nothing is left in flight
 * (same public semantics as the readiness queue emptying). */
void kl_udp_comp_on_send(KlUdp *udp, size_t len) {
    udp->dg.q_bytes = (udp->dg.q_bytes >= len) ? udp->dg.q_bytes - len : 0;
    if (udp->dg.q_bytes == 0 && udp->on_drain)
        udp->on_drain(udp, udp->drain_ud);
}

/* The comp_udp_dispatch hook (completion_core.c → here): route a datagram completion to
 * kl_udp_comp_on_recv / kl_udp_comp_on_send. Registered on the ctx by kl_udp_init so the
 * generic tick reaches the UDP stack without a static reference (a client-only build
 * links neither). The src/local KlSockAddr + GRO/truncation meta logic moved here from
 * the old kl_comp_run UDP branch. */
void kl_udp_comp_dispatch(struct KlEventCtx *ctx, const void *evp) {
    (void)ctx;
    const KlCompletionEvent *ev = evp;
    switch (ev->kind) {
    case KL_COMP_DGRAM_RECV: {  /* datagram — the target is a KlDatagram*, no server */
        /* The backend already converted src/local to the neutral KlSockAddr once at
         * its seam. A recv without a source name (family KL_AF_UNSPEC, e.g. a
         * connected socket) or without pktinfo passes NULL, not a zeroed address. */
        int have_src   = kl_sockaddr_family(&ev->peer)  != KL_AF_UNSPEC;
        int have_local = kl_sockaddr_family(&ev->local) != KL_AF_UNSPEC;
        KlUdpRxMeta meta = {
            .local     = have_local ? &ev->local : NULL,
            .gro_seg   = ev->gro_seg,
            .truncated = ev->truncated,
        };
        kl_udp_comp_on_recv(kl_udp_of_dg((KlDatagram *)ev->target), ev->buf, ev->bytes,
                            have_src ? &ev->peer : NULL, &meta);
        break;
    }
    case KL_COMP_DGRAM_SEND:
        kl_udp_comp_on_send(kl_udp_of_dg((KlDatagram *)ev->target), ev->bytes);
        break;
    default: break;   /* core routes only UDP kinds here */
    }
}

void kl_udp_recv_stop(KlUdp *udp) {
    if (!udp)
        return;
    udp->dg.recv_active = 0;
    kl_udp_update_interest(udp);
}

int kl_udp_send_to(KlUdp *udp, const void *data, size_t len,
                   const KlSockAddr *dest) {
    return kl_udp_send_to_from(udp, data, len, dest, NULL);
}

int kl_udp_send_to_from(KlUdp *udp, const void *data, size_t len,
                        const KlSockAddr *dest, const KlSockAddr *src) {
    if (!udp || (!data && len) || !dest ||
        kl_sockaddr_family(dest) == KL_AF_UNSPEC) {
        if (udp) udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_send_common(udp, data, len, dest, src, -1);
}

int kl_udp_send_to_tos(KlUdp *udp, const void *data, size_t len,
                       const KlSockAddr *dest, int tos) {
    if (!udp || (!data && len) || !dest ||
        kl_sockaddr_family(dest) == KL_AF_UNSPEC || tos < 0 || tos > 255) {
        if (udp) udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_send_common(udp, data, len, dest, NULL, tos);
}

int kl_udp_set_tos(KlUdp *udp, int tos) {
    if (!udp || !kl_handle_valid(udp->dg.fd) || tos < 0 || tos > 255) {
        if (udp) udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    const KlDatagramOps *dg = udp_dg(udp);
    int rc = dg->set_tos ? dg->set_tos(udp_sp_ctx(udp), udp->dg.fd, udp->dg.family, tos) : -1;
    if (rc == 0)
        return 0;
    udp->dg.last_error = KL_ERR_SOCKET;
    return -1;
}

int kl_udp_recv_tos(const KlUdp *udp) {
    return udp ? udp->dg.recv_tos_val : -1;
}

int kl_udp_send(KlUdp *udp, const void *data, size_t len) {
    if (!udp || (!data && len)) {
        if (udp) udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    if (!udp->dg.connected) {
        udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_send_common(udp, data, len, NULL, NULL, -1);
}

/* Portable GSO fallback: send each segment individually via the normal path
 * (immediate send, or queued under backpressure — order preserved). */
static int udp_send_segments(KlUdp *udp, const void *buf, size_t total_len,
                             size_t seg, const KlSockAddr *dest) {
    size_t off = 0;
    while (off < total_len) {
        size_t chunk = (total_len - off < seg) ? (total_len - off) : seg;
        if (udp_send_common(udp, (const char *)buf + off, chunk,
                            dest, NULL, -1) != 0)
            return -1;   /* over-cap / hard error — remaining segments dropped */
        off += chunk;
    }
    return 0;
}

int kl_udp_send_gso(KlUdp *udp, const void *buf, size_t total_len,
                    size_t segment_size, const KlSockAddr *dest) {
    if (!udp || (!buf && total_len) || total_len == 0 || segment_size == 0 ||
        segment_size > total_len || total_len > 65507 || !dest ||
        kl_sockaddr_family(dest) == KL_AF_UNSPEC) {
        if (udp) udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    /* Single-syscall GSO when there is more than one segment, nothing is already
     * queued (preserve ordering), and GSO hasn't been ruled unsupported. The
     * seam returns -1/EOPNOTSUPP where GSO is unavailable at build time. */
    if (segment_size < total_len && !udp->dg.q_head && !udp->dg.gso_disabled) {
        const KlDatagramOps *dg = udp_dg(udp);
        ssize_t n;
        if (dg->send_gso) {
            n = dg->send_gso(udp_sp_ctx(udp), udp->dg.fd, buf, total_len, (uint16_t)segment_size, dest);
        } else {
            n = -1; errno = EOPNOTSUPP;   /* no GSO op — fall through to per-segment */
        }
        if (n >= 0)
            return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            udp->dg.gso_disabled = 1;   /* GSO unsupported here — stop trying */
        /* EAGAIN or unsupported: fall through and send/queue per-segment. */
    }
    return udp_send_segments(udp, buf, total_len, segment_size, dest);
}

void kl_udp_recv_segments(KlUdp *udp, KlUdpRecvSegmentsFn cb, void *user_data) {
    if (!udp) return;
    udp->on_recv_segments = cb;
    udp->recv_seg_ud = user_data;
}

int kl_udp_multicast_join(KlUdp *udp, const char *group, unsigned iface_index) {
    if (!udp || !kl_handle_valid(udp->dg.fd) || !group) {
        if (udp) udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_mcast_dispatch(udp, group, iface_index, 1);
}

int kl_udp_multicast_leave(KlUdp *udp, const char *group, unsigned iface_index) {
    if (!udp || !kl_handle_valid(udp->dg.fd) || !group) {
        if (udp) udp->dg.last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    return udp_mcast_dispatch(udp, group, iface_index, 0);
}

void kl_udp_on_drain(KlUdp *udp, KlUdpDrainFn cb, void *user_data) {
    if (!udp) return;
    udp->on_drain = cb;
    udp->drain_ud = user_data;
}

size_t   kl_udp_send_queued(const KlUdp *udp) { return udp ? udp->dg.q_bytes : 0; }
uint64_t kl_udp_dropped(const KlUdp *udp)     { return udp ? udp->dg.dropped : 0; }
uint64_t kl_udp_truncated(const KlUdp *udp)   { return udp ? udp->dg.truncated : 0; }
KlSocketHandle kl_udp_fd(const KlUdp *udp)    { return udp ? udp->dg.fd : KL_INVALID_SOCKET; }
KlError  kl_udp_last_error(const KlUdp *udp)  { return udp ? udp->dg.last_error : KL_ERR_INVALID_ARG; }

uint16_t kl_udp_local_port(const KlUdp *udp) {
    if (!udp || !kl_handle_valid(udp->dg.fd))
        return 0;
    KlSockAddr la;
    if (kl_sock_get_local_addr(udp->dg.ctx->sockets, udp->dg.fd, &la) != 0)
        return 0;
    return kl_sockaddr_port(&la);
}
