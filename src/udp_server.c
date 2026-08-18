#include <keel/udp_server.h>

#include "datagram_open.h"   /* kl_datagram_open / KlDatagramPrep / kl_datagram_teardown */
#include "socket.h"          /* kl_sock_close — M0 fd cleanup on a pre-adoption init failure */

#include <string.h>

/* M4: KlUdpServer re-implemented on the KlDatagram core (M0 prep + M1 BOTH policy) + the M2 extended
 * layer (source-pin caps + multicast). See docs/datagram_m4_udpserver_design.md. */

#define KL_UDP_DGRAM_MAX          65507u          /* a full UDP payload — one queueable reply per slot */
#define KL_UDP_DEFAULT_SEND_QUEUE (256u * 1024u)  /* max_send_queue == 0 default */

/* recv trampoline: KlDatagram recv → user handler. Captures the local (destination) address (source-pin
 * reply) when the provider delivered it (KL_DGRAM_HAS_LOCAL). */
static void us_on_recv(void *ud, const void *data, size_t len,
                       const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    KlUdpServer *s = ud;
    if (local && (flags & KL_DGRAM_HAS_LOCAL)) { s->local = *local; s->have_local = 1; }
    else                                       { s->have_local = 0; }
    s->handler(s, data, len, peer, s->user_data);
}

/* A wildcard bind (INADDR_ANY / in6addr_any) is where the reply source is ambiguous on a multi-homed
 * host, so capture the local address (and require source-pin) there. */
static int udp_addr_is_wildcard(const char *bind_addr) {
    if (!bind_addr)
        return 1;                 /* default "0.0.0.0" */
    return strcmp(bind_addr, "0.0.0.0") == 0 || strcmp(bind_addr, "::") == 0;
}

int kl_udp_server_init(KlUdpServer *s, KlEventCtx *ctx,
                       const KlUdpServerConfig *cfg,
                       KlUdpHandlerFn handler, void *user_data) {
    if (!s)
        return -1;
    memset(s, 0, sizeof(*s));
    if (!ctx || !cfg || !handler) {
        s->last_error = KL_ERR_INVALID_ARG;
        return -1;
    }
    s->handler = handler;
    s->user_data = user_data;

    KlAllocator *alloc = cfg->alloc ? cfg->alloc : ctx->alloc;
    int wildcard = udp_addr_is_wildcard(cfg->bind_addr);

    /* Step 1: prepare the fd provider-neutrally (M0). recv_gro + mmsg_batch are FORCED OFF (§7 — the
     * single-flight core cannot split GRO-coalesced datagrams nor drive recvmmsg); recv_pktinfo on a
     * wildcard bind (§4); multicast_group is joined post-init via M2 (§6), not at configure. */
    KlUdpConfig uc = {
        .ctx            = ctx,
        .bind_addr      = cfg->bind_addr ? cfg->bind_addr : "0.0.0.0",
        .bind_port      = cfg->port,
        .recv_buf_size  = cfg->recv_buf_size,
        .max_send_queue = cfg->max_send_queue,
        .reuse_addr     = 1,               /* servers want quick rebind */
        .reuse_port     = cfg->reuse_port,
        .recv_pktinfo   = wildcard,
        .so_rcvbuf      = cfg->so_rcvbuf,
        .so_sndbuf      = cfg->so_sndbuf,
        .mmsg_batch     = 0,               /* inert on the single-flight core */
        .recv_gro       = 0,               /* forced off — correctness (§7) */
        .tos            = cfg->tos,
        .recv_tos       = cfg->recv_tos,
        .broadcast      = cfg->broadcast,
        .multicast_ttl  = cfg->multicast_ttl,
        .multicast_disable_loop = cfg->multicast_disable_loop,
        .multicast_iface = cfg->multicast_iface,
        .multicast_group = NULL,           /* joined post-init (§6) */
        .alloc          = alloc,
    };
    KlDatagramPrep prep;
    if (kl_datagram_open(ctx->sockets, &uc, &prep) != 0) {
        s->last_error = prep.err;          /* open closed its own fd on failure (M0) */
        return -1;
    }

    /* Step 1a: source-pin needs the RECEIVE capture accepted too — otherwise the server would reply via
     * the default route (§4). Checked on `prep` BEFORE adoption; failure closes the caller-held fd. */
    if (wildcard && !(prep.rx_caps & KL_DGRAM_RX_PKTINFO)) {
        kl_sock_close(ctx->sockets, prep.fd);
        s->last_error = KL_ERR_UNSUPPORTED;
        return -1;
    }

    /* Step 2: adopt the fd into a fixed-slot KlDatagram with the M1 BOTH byte-gate policy. Sizing (§3):
     * budget = max_send_queue (normalized), full-datagram slots, slot count derived from the budget. */
    size_t eff_budget = cfg->max_send_queue ? cfg->max_send_queue : KL_UDP_DEFAULT_SEND_QUEUE;
    size_t slots = eff_budget / KL_UDP_DGRAM_MAX;
    if (slots == 0) slots = 1;
    /* Preserve KlUdp's recv-buffer cap (a UDP datagram never exceeds 65535); an uncapped
     * recv_buf_size would otherwise allocate an arbitrarily large inbound slot. */
    size_t recv_cap = cfg->recv_buf_size ? cfg->recv_buf_size : 2048;
    if (recv_cap > 65535) recv_cap = 65535;
    KlDatagramConfig dc = {
        .ctx = ctx, .alloc = alloc, .sockets = ctx->sockets, .fd = prep.fd,
        .send_slots = slots, .send_slot_cap = KL_UDP_DGRAM_MAX,
        .recv_cap = recv_cap,
        .want_caps = wildcard ? KL_DGRAM_CAP_SOURCE_PIN : 0u,   /* fail-loud where source-pin is load-bearing (§4) */
    };
    if (kl_datagram_init_ex(&s->dg, &dc, eff_budget) != 0) {
        s->last_error = kl_datagram_last_error(&s->dg);   /* copy BEFORE closing */
        kl_sock_close(ctx->sockets, prep.fd);             /* init_ex failed before adoption → caller closes (§4/§8) */
        return -1;
    }
    /* fd is now adopted by s->dg; every failure below tears the datagram down (which closes the fd). */

    if (kl_datagram_recv_start(&s->dg, us_on_recv, s) != 0) {
        s->last_error = kl_datagram_last_error(&s->dg);
        kl_datagram_teardown(&s->dg, NULL, NULL);
        return -1;
    }

    if (cfg->multicast_group && cfg->multicast_group[0]) {   /* join-at-init (non-empty, KlUdp parity) via M2 */
        if (kl_datagram_multicast_join(&s->dg, cfg->multicast_group, cfg->multicast_iface) != 0) {
            s->last_error = kl_datagram_last_error(&s->dg);
            kl_datagram_teardown(&s->dg, NULL, NULL);
            return -1;
        }
    }
    return 0;
}

int kl_udp_server_reply(KlUdpServer *s, const void *data, size_t len,
                        const KlSockAddr *dest) {
    if (!s)
        return -1;
    /* Reply from the address the client hit (multi-homed correctness); socket-default TOS (tos = -1). */
    KlDatagramMessage m = {
        .data = data, .len = len, .peer = dest,
        .local = s->have_local ? &s->local : NULL, .tos = -1,
    };
    KlDatagramSendStatus st = kl_datagram_send(&s->dg, &m);
    if (st == KL_DATAGRAM_ACCEPTED)
        return 0;
    switch (st) {                                      /* map to KlUdp's int/last_error surface (§8) */
        case KL_DATAGRAM_WOULD_BLOCK:                  /* fallthrough — both are the KlUdp drop surface */
        case KL_DATAGRAM_TOO_LARGE:   s->last_error = KL_ERR_QUEUE_FULL;  break;
        case KL_DATAGRAM_CLOSED:      s->last_error = KL_ERR_INVALID_ARG; break;
        case KL_DATAGRAM_UNSUPPORTED: s->last_error = KL_ERR_UNSUPPORTED; break;
        default:                      s->last_error = KL_ERR_IO;          break;
    }
    return -1;
}

int kl_udp_server_multicast_join(KlUdpServer *s, const char *group,
                                 unsigned iface_index) {
    if (!s) return -1;
    int rc = kl_datagram_multicast_join(&s->dg, group, iface_index);
    if (rc != 0)
        s->last_error = kl_datagram_last_error(&s->dg);
    return rc;
}

int kl_udp_server_multicast_leave(KlUdpServer *s, const char *group,
                                  unsigned iface_index) {
    if (!s) return -1;
    int rc = kl_datagram_multicast_leave(&s->dg, group, iface_index);
    if (rc != 0)
        s->last_error = kl_datagram_last_error(&s->dg);
    return rc;
}

void kl_udp_server_free(KlUdpServer *s) {
    if (!s)
        return;
    /* Synchronous outside a handler; deferred to the current tick's end from within one (§9). Idempotent
     * (a second call finds s->dg.core NULL → no-op). Does NOT free the caller-owned KlUdpServer struct. */
    kl_datagram_teardown(&s->dg, NULL, NULL);
    s->handler = NULL;
}

uint16_t kl_udp_server_local_port(const KlUdpServer *s) {
    return s ? kl_datagram_local_port(&s->dg) : 0;
}

KlSocketHandle kl_udp_server_fd(const KlUdpServer *s) {
    return s ? kl_datagram_fd(&s->dg) : KL_INVALID_SOCKET;
}

KlError kl_udp_server_last_error(const KlUdpServer *s) {
    return s ? s->last_error : KL_ERR_INVALID_ARG;
}
