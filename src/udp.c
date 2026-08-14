#include <keel/udp.h>

#include <stddef.h>   /* offsetof — recover KlUdp from its embedded KlDatagram */
#include <stdint.h>
#include <string.h>   /* memcpy / memset — freestanding intrinsics */

#include "socket.h"   /* seam: kl_sock_* + KlSockAddr + KlSocketProvider.dgram */
#include <keel/datagram.h>   /* KlDatagramOps — the provider datagram data-plane */
#include "udp_internal.h"
#include "datagram_slots.h"  /* the dedicated inbound slot (recv storage) */
#include "datagram_recv.h"   /* KlDgramRecv — Tier-1 serial receive machine */
#include "datagram_life.h"   /* KlDgramLife — stable-liveness token for datagram completion ops */
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
/* The socket provider, for kl_sock_io_status — the neutral would-block classifier (the errno mapping
 * is confined to kl_sockdef_io_status, so this TU stays errno-free / freestanding-clean). NULL = the
 * default provider; a freestanding provider reports the I/O category itself. */
static inline const KlSocketProvider *udp_sp(const KlUdp *u) {
    return u->dg.ctx ? u->dg.ctx->sockets : NULL;
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
    if (kl_sock_io_status(udp_sp(udp)) == KL_IO_WOULD_BLOCK)
        return udp_enqueue(udp, data, len, dest, src, tos);

    udp->dg.last_error = KL_ERR_IO;
    return -1;
}

/* ── Receive path ─────────────────────────────────────────────────────── */

/* Deliver one received buffer. A GRO-coalesced buffer (gro_seg > 0 and
 * len > gro_seg) goes whole to on_recv_segments when set, else is split into
 * per-segment on_recv calls; a plain datagram goes to on_recv. */
void kl_udp_deliver(KlUdp *udp, const KlDgramLife *life, const void *data, size_t len, int gro_seg,
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
            /* Between segments inspect ONLY the stable token's liveness (the callback may have freed
             * the owner) — never the possibly-dead wrapper's fields. */
            if (!kl_dgram_life_target(life))
                return;   /* callback freed the owner mid-split */
            off += chunk;
        }
        return;
    }
    if (udp->on_recv)
        udp->on_recv(udp, data, len, src, local, udp->recv_ud);
}

/* ── Receive machine (step 6.1) ────────────────────────────────────────────
 * The Tier-1 per-datagram receive rides the shared KlDgramRecv over a dedicated inbound slot
 * (KlDgramInbound — its own allocation, life-token-owned, 7A-1) — the same uniform captured-prefix
 * truncation, mandatory-peer, serial pause/resume
 * normalization the public KlDatagram path will use. GRO (a single coalesced recv) rides through it
 * via per-datagram scratch (gro_seg/tos); only true recvmmsg BATCHING (rx_batch) bypasses it as a
 * legacy Tier-2 path (see udp_recv_dgram). The byte-budget SEND queue and close remain LEGACY
 * compatibility paths, deliberately outside the Tier-1 fixed-slot send facet. */
typedef struct KlUdpRx {
    KlDgramInbound inbound;  /* the dedicated inbound slot backs udp->dg.recv_buf (its own allocation) */
    KlDgramRecv  recv;
    KlUdp       *udp;        /* owner — also the stable token's target while live */
    KlAllocator *alloc;      /* the event-ctx allocator (outlives KlUdp); frees inbound + this at final */
    KlDgramLife *life;       /* stable-liveness token owning this storage (see datagram_life.h) */
    int          completion; /* 1 = completion (post/on_complete); 0 = readiness (arm/pull) */
    int          gro_seg;    /* scratch: the in-flight datagram's GRO segment size (0 = none) */
    int          tos;        /* scratch: the in-flight datagram's TOS byte, or -1 */
} KlUdpRx;
static inline KlUdpRx *udp_rx(const KlUdp *u) { return (KlUdpRx *)u->rx; }

/* Token on_final: the owner ref AND every posted-op ref have been released, so the inbound storage is
 * no longer pinned by any op — free the slot + this holder. The receive machine borrows no heap. */
static void udp_rx_final(void *ctx) {
    KlUdpRx *rx = ctx;
    kl_dgram_inbound_free(&rx->inbound);
    kl_free(rx->alloc, rx, sizeof(*rx));
}

/* Machine delivery: recover the owner through the STABLE TOKEN (never the possibly-freed wrapper). A
 * dead token (kl_udp_free ran) drops the datagram silently. Otherwise fan out via the GRO-splitting
 * delivery, folding in the truncation counter + per-datagram TOS (scratch stashed by the adapter).
 * The GRO split re-checks token liveness between segments — a callback may free the owner. */
/* cppcheck-suppress constParameterCallback ; signature must match KlDgramRecvDeliverFn typedef */
static void udp_rx_deliver(void *ctx, const void *data, size_t len,
                           const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    const KlUdpRx *rx = ctx;
    KlUdp *udp = kl_dgram_life_target(rx->life);
    if (!udp) return;                                    /* owner freed — token dead */
    if (flags & KL_DGRAM_TRUNCATED) udp->dg.truncated++;
    udp->dg.recv_tos_val = rx->tos;
    kl_udp_deliver(udp, rx->life, data, len, rx->gro_seg, peer, local);
}

/* Readiness arm/disarm: re-sync the combined (READ recv + WRITE send-queue) interest. recv_active is
 * the READ-intent flag KlUdp already toggles in recv_start/recv_stop, so the hooks only reconcile.
 * ARM must report failure: if the watcher add/mod could not arm READ interest, return -1 so the
 * receive machine enters its error state instead of believing a receive is armed (recv_inflight=1)
 * when no READ interest exists. update_interest leaves want_mask without READ on failure. */
static int  udp_rx_arm(void *ctx) {
    KlUdp *udp = ((KlUdpRx *)ctx)->udp;
    kl_udp_update_interest(udp);
    return (udp->dg.want_mask & KL_EVENT_READ) ? 0 : -1;
}
static void udp_rx_disarm(void *ctx) { kl_udp_update_interest(((KlUdpRx *)ctx)->udp); }

/* Readiness pull: one datagram via the provider recv op into the inbound slot; marshal the source +
 * control metadata for the machine to finalize (peer mandatory; local + HAS_LOCAL; TRUNCATED). */
static int udp_rx_pull(void *ctx, size_t *out_len) {
    KlUdpRx *rx = ctx;
    KlUdp *udp = rx->udp;
    KlDgramSlot *in = kl_dgram_inbound_slot(&rx->inbound);
    KlSockAddr src;
    KlDgramRxMeta meta;
    kl_ssize_t n = udp_dg(udp)->recv(udp_sp_ctx(udp), udp->dg.fd, in->data, rx->inbound.slot.cap, &src, &meta);
    if (n < 0) {
        if (kl_sock_io_status(udp_sp(udp)) == KL_IO_WOULD_BLOCK) return 0;   /* drained (would-block) */
        udp->dg.last_error = KL_ERR_IO;
        return -1;                                                /* fatal receive error */
    }
    in->peer  = src;
    in->flags = 0;
    if (meta.has_local) { in->local = meta.local; in->flags |= KL_DGRAM_HAS_LOCAL; }
    if (meta.truncated) in->flags |= KL_DGRAM_TRUNCATED;
    rx->gro_seg = meta.gro_seg;
    rx->tos     = meta.tos;
    *out_len = (size_t)n;
    return 1;
}

/* Completion arm: post one overlapped recv into the inbound slot (dg->recv_buf points at it). */
static int udp_rx_post(void *ctx) {
    KlUdpRx *rx = ctx;
    return kl_comp_post_dgram_recv(&rx->udp->dg) < 0 ? -1 : 0;
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
                if (kl_sock_io_status(udp_sp(udp)) == KL_IO_WOULD_BLOCK) break;
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
                if (kl_sock_io_status(udp_sp(udp)) == KL_IO_WOULD_BLOCK) break;
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
    kl_udp_deliver(udp, udp_rx(udp)->life, data, len, meta->gro_seg,
                   (src && kl_sockaddr_family(src) != KL_AF_UNSPEC) ? src : NULL,
                   meta->has_local ? &meta->local : NULL);
}

/* Drain the socket on a READ-ready event. TIER-2: recvmmsg batches through the legacy loop when an
 * rx batch + recv_batch op are present (outside the Tier-1 machine). TIER-1: otherwise the shared
 * KlDgramRecv machine performs the serial per-datagram drain (uniform truncation/metadata/pause). */
static void udp_recv_dgram(KlUdp *udp, const KlDatagramOps *dg) {
    void *ctx = udp_sp_ctx(udp);

    if (udp->dg.rx_batch && dg->recv_batch) {
        KlDgramRxSlot slots[UDP_MMSG_MAX];
        int max = udp->dg.mmsg_batch < UDP_MMSG_MAX ? udp->dg.mmsg_batch : UDP_MMSG_MAX;
        for (;;) {
            int cnt = dg->recv_batch(ctx, udp->dg.fd, udp->dg.rx_batch, slots, max);
            if (cnt < 0) {
                if (kl_sock_io_status(udp_sp(udp)) != KL_IO_WOULD_BLOCK) udp->dg.last_error = KL_ERR_IO;
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

    (void)kl_dgram_recv_on_readable(&udp_rx(udp)->recv);
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
    /* Pin the receive storage across the whole dispatch: a callback (on_drain / on_recv) may free the
     * owner via kl_udp_free(), which drops the owner token ref. On readiness there is no in-flight
     * completion op holding a ref, so without this pin the machine would be freed while this drive is
     * still on the stack (UAF). The final free (udp_rx_final) is deferred until the release below. */
    KlDgramLife *life = udp_rx(udp)->life;
    kl_dgram_life_retain(life);
    if (ready & KL_EVENT_WRITE)
        udp_flush_dgram(udp, dg);
    /* Re-check liveness via the TOKEN (a WRITE-side on_drain may have freed the owner) before the
     * receive drive touches udp/dg again. */
    if (kl_dgram_life_target(life) && udp->dg.recv_active && kl_handle_valid(udp->dg.fd) &&
        (ready & KL_EVENT_READ))
        udp_recv_dgram(udp, dg);
    kl_dgram_life_release(life);   /* runs udp_rx_final if the owner was freed in a callback */
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

    /* Receive machine (step 6.1): allocate the KlUdpRx holder once; its dedicated INBOUND SLOT is
     * the recv buffer (replaces the standalone recv_buf malloc — still exactly one init-time alloc
     * for receive storage). A minimal 1×1 outbound slot is unused (the SEND queue stays legacy). */
    {
        /* The receive storage + its stable-liveness token are allocated from the EVENT-CONTEXT
         * allocator (not cfg->alloc) so they outlive the KlUdp wrapper: a completion op that pins
         * this storage may be reaped after kl_udp_free(), and the token's final release frees it. */
        KlAllocator *la = udp->dg.ctx->alloc ? udp->dg.ctx->alloc : udp->dg.alloc;
        KlUdpRx *rx = kl_malloc(la, sizeof(*rx));
        if (!rx) { udp->dg.last_error = KL_ERR_ALLOC; goto fail; }
        memset(rx, 0, sizeof(*rx));
        rx->udp   = udp;
        rx->alloc = la;
        rx->tos   = -1;
        if (kl_dgram_inbound_init(&rx->inbound, la, udp->dg.recv_buf_size) != 0) {
            kl_free(la, rx, sizeof(*rx));
            udp->dg.last_error = KL_ERR_ALLOC;
            goto fail;
        }
        rx->completion = (kl_event_caps(&udp->dg.ctx->loop) & KL_EVENT_CAP_COMPLETION) ? 1 : 0;
        int rinit = rx->completion
            ? kl_dgram_recv_init(&rx->recv, &rx->inbound, /*completion*/1, udp_rx_deliver, rx,
                                 udp_rx_post, NULL, NULL, rx)
            : kl_dgram_recv_init(&rx->recv, &rx->inbound, /*completion*/0, udp_rx_deliver, rx,
                                 udp_rx_arm, udp_rx_disarm, udp_rx_pull, rx);
        if (rinit != 0) {                       /* unwind the inbound slot + holder on init failure */
            kl_dgram_inbound_free(&rx->inbound);
            kl_free(la, rx, sizeof(*rx));
            udp->dg.last_error = KL_ERR_INVALID_ARG;
            goto fail;
        }
        rx->life = kl_dgram_life_create(la, udp, udp_rx_final, rx);   /* owner ref (refs=1, live) */
        if (!rx->life) {
            kl_dgram_inbound_free(&rx->inbound);
            kl_free(la, rx, sizeof(*rx));
            udp->dg.last_error = KL_ERR_ALLOC;
            goto fail;
        }
        udp->rx = rx;
        udp->dg.rx_life  = rx->life;   /* backends read this at post time to pin the op */
        udp->dg.recv_buf = kl_dgram_inbound_slot(&rx->inbound)->data;   /* completion post + delivery */
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
    /* Stop receiving (readiness disarms interest). A completion recv still physically posted is NOT
     * force-retired here — it stays PINNED by the stable token (each posted op holds a token ref) and
     * releases its ref when it is genuinely reaped/dequeued. The token's final release then frees the
     * inbound storage, so the buffer + machine outlive every op. See the token release at the tail. */
    if (udp->rx)
        kl_dgram_recv_stop(&udp_rx(udp)->recv);
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

    /* Confirmed detachment of the receive storage: mark the token dead (later completions recover a
     * NULL owner and drop) and drop the OWNER reference. The FINAL release (owner + every posted op)
     * runs udp_rx_final() to free the inbound slot + machine — immediately here when no completion op
     * is outstanding (readiness / idle completion), or deferred until the last op is reaped. */
    if (udp->rx) {
        KlDgramLife *life = udp_rx(udp)->life;
        udp->rx = NULL;
        udp->dg.rx_life = NULL;
        udp->dg.recv_buf = NULL;   /* the inbound slot is now owned solely by the token */
        kl_dgram_life_mark_dead(life);
        kl_dgram_life_release(life);
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
    KlUdpRx *rx = udp_rx(udp);

    /* Completion loop (IOCP/io_uring): a readiness watcher never fires — associate the socket with
     * the port, then let the machine post the first overlapped recv (its re-arm posts each next one).
     * The recvmmsg batch is not used on a completion loop. */
    if (rx->completion) {
        if (kl_event_add(&udp->dg.ctx->loop, udp->dg.fd, KL_EVENT_READ, udp) < 0 ||
            kl_dgram_recv_start(&rx->recv) < 0) {
            udp->dg.recv_active = 0;
            udp->dg.last_error = KL_ERR_IO;
            return -1;
        }
        return 0;
    }

    /* Readiness: lazily allocate the recvmmsg batch (large: mmsg_batch × recv_buf_size) where mmsg
     * batching applies — the TIER-2 legacy path that bypasses the machine. Failure leaves rx_batch
     * NULL → the Tier-1 machine drains per-datagram; a no-op where batching is unavailable. */
    {
        const KlDatagramOps *dg = udp_dg(udp);
        if (udp->dg.mmsg_batch > 1 && dg->rx_batch_new && !udp->dg.rx_batch)
            udp->dg.rx_batch = dg->rx_batch_new(udp->dg.alloc, udp->dg.mmsg_batch, udp->dg.recv_buf_size);
    }

    if (udp->dg.rx_batch) {
        /* TIER-2 legacy batch: interest is driven directly by recv_active (not the machine). */
        kl_udp_update_interest(udp);
    } else {
        /* TIER-1: the machine arms READ interest (its arm hook reconciles kl_udp_update_interest). */
        if (kl_dgram_recv_start(&rx->recv) < 0) {
            udp->dg.recv_active = 0;
            return -1;
        }
    }
    if (!(udp->dg.want_mask & KL_EVENT_READ)) {   /* watcher registration failed */
        udp->dg.recv_active = 0;
        kl_dgram_recv_stop(&rx->recv);
        return -1;
    }
    return 0;
}

/* Completion-loop datagram receive (step 6.1): a successful overlapped recv landed `len` bytes in
 * the inbound slot (== udp->dg.recv_buf). Marshal its source + control metadata into the slot and
 * drive the shared KlDgramRecv machine, which delivers (udp_rx_deliver) and RE-ARMS (posts the next
 * recv via udp_rx_post) — no manual re-post here. `src == NULL` (a connected socket without a
 * name) leaves the slot peer UNSPEC, which the machine rejects as a contract violation. */
void kl_udp_comp_on_recv(const KlUdp *udp, const void *buf, size_t len,
                         const KlSockAddr *src, const KlUdpRxMeta *meta) {
    KlUdpRx *rx = udp_rx(udp);   /* mutation flows through the (non-const) machine, not `udp` */
    KlDgramSlot *in = kl_dgram_inbound_slot(&rx->inbound);
    /* The machine delivers from the inbound slot. On the posted-recv backends (pollcomp/IOCP) the
     * provider already wrote there (ev->buf == dg->recv_buf == in->data) so this is skipped; a
     * backend that delivers from its OWN buffer (e.g. the lwIP raw copy-ring) passes a different
     * ev->buf, so copy it into the slot (bounded by capacity). */
    if (buf && buf != in->data && len) {
        size_t n = len <= rx->inbound.slot.cap ? len : rx->inbound.slot.cap;
        memcpy(in->data, buf, n);
    }
    in->flags = 0;
    if (src && kl_sockaddr_family(src) != KL_AF_UNSPEC) in->peer = *src;
    else memset(&in->peer, 0, sizeof(in->peer));
    if (meta->local) { in->local = *meta->local; in->flags |= KL_DGRAM_HAS_LOCAL; }
    if (meta->truncated) in->flags |= KL_DGRAM_TRUNCATED;
    rx->gro_seg = meta->gro_seg;
    rx->tos     = -1;   /* the completion recv carries no per-datagram TOS today */
    (void)kl_dgram_recv_on_complete(&rx->recv, len, 1);
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
    /* Recover the owner through the STABLE TOKEN (NULL once dead). ALL completion backends are on the
     * token path (Phase B.6: pollcomp/io_uring/IOCP/lwIP-raw), so every datagram completion carries
     * ev->life and the ref was transferred from the posted op; the legacy ev->target (KlUdp-deref)
     * recovery is gone. A dgram event without a token (ev->life == NULL) yields a NULL owner and is
     * safely dropped/retired below — never a dereference of a possibly-freed KlDatagram. */
    KlDgramLife *life = ev->life;
    KlUdp *udp = life ? (KlUdp *)kl_dgram_life_target(life) : NULL;
    switch (ev->kind) {
    case KL_COMP_DGRAM_RECV: {
        KlUdpRx *rx = udp ? udp_rx(udp) : NULL;
        if (!rx || !udp->dg.recv_active || !kl_handle_valid(udp->dg.fd)) {
            /* Owner dead (token)/freed (legacy), receiver stopped, or socket closed — retire the
             * machine's posted op only while it is still live; never touch a dead owner. */
            if (rx && kl_dgram_recv_inflight(&rx->recv))
                (void)kl_dgram_recv_on_complete(&rx->recv, 0, 0);
        } else if (!ev->ok) {
            /* A failed/cancelled recv (classified at the backend seam) retires with no delivery. */
            (void)kl_dgram_recv_on_complete(&rx->recv, 0, 0);
        } else {
            /* The backend converted src/local to neutral KlSockAddr at its seam. A recv without a
             * source name (KL_AF_UNSPEC) or pktinfo passes NULL, not a zeroed address. */
            int have_src   = kl_sockaddr_family(&ev->peer)  != KL_AF_UNSPEC;
            int have_local = kl_sockaddr_family(&ev->local) != KL_AF_UNSPEC;
            KlUdpRxMeta meta = {
                .local     = have_local ? &ev->local : NULL,
                .gro_seg   = ev->gro_seg,
                .truncated = ev->truncated,
            };
            kl_udp_comp_on_recv(udp, ev->buf, ev->bytes, have_src ? &ev->peer : NULL, &meta);
        }
        break;
    }
    case KL_COMP_DGRAM_SEND:
        if (udp) kl_udp_comp_on_send(udp, ev->bytes);   /* a late send on a dead owner is dropped */
        break;
    default: break;   /* core routes only UDP kinds here */
    }
    /* The token reference transferred backend-op → event; release it after dispatch (may trigger the
     * final release + free of the inbound storage once the owner ref + all op refs are gone). */
    if (life)
        kl_dgram_life_release(life);
}

void kl_udp_recv_stop(KlUdp *udp) {
    if (!udp)
        return;
    udp->dg.recv_active = 0;
    if (udp->rx)
        kl_dgram_recv_stop(&udp_rx(udp)->recv);   /* readiness disarms; completion drops on completion */
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
     * missing send_gso op (or a non-would-block failure from it) disables GSO for good. */
    if (segment_size < total_len && !udp->dg.q_head && !udp->dg.gso_disabled) {
        const KlDatagramOps *dg = udp_dg(udp);
        if (dg->send_gso) {
            ssize_t n = dg->send_gso(udp_sp_ctx(udp), udp->dg.fd, buf, total_len,
                                     (uint16_t)segment_size, dest);
            if (n >= 0)
                return 0;
            if (kl_sock_io_status(udp_sp(udp)) != KL_IO_WOULD_BLOCK)
                udp->dg.gso_disabled = 1;   /* GSO unsupported here — stop trying */
            /* would-block or unsupported: fall through and send/queue per-segment. */
        } else {
            udp->dg.gso_disabled = 1;       /* no GSO op — never attempt again */
        }
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
