#include <keel/udp_server.h>
#include <keel/datagram_batch.h>   /* M5.4: recv batch/GRO opt-in (kl_datagram_batch_create/attach) */

#include "datagram_open.h"   /* kl_datagram_open / KlDatagramPrep / kl_datagram_teardown */
#include "socket.h"          /* kl_sock_close — M0 fd cleanup on a pre-adoption init failure */
#include "event_caps.h"      /* kl_event_caps — completion detection for the GRO-capture gate (M5.4) */

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
    /* M5.4: on a completion loop the recv rides the single-flight core (no batch, per D-M5-3), so GRO
     * capture MUST stay off there — a coalesced buffer would be delivered whole (unsplit). It is enabled
     * on a readiness loop, where the batch's borrowed-view seam splits it per-datagram. */
    int completion = (kl_event_caps(&ctx->loop) & KL_EVENT_CAP_COMPLETION) != 0;

    /* Step 1: prepare the fd provider-neutrally (M0). recv_pktinfo on a wildcard bind (§4);
     * multicast_group is joined post-init via M2 (§6), not at configure. M5.4: recv_gro is enabled on a
     * readiness loop when requested (the recv batch splits it); mmsg_batch stays 0 here — the datagram's
     * recvmmsg batching is a separate RECV KlDatagramBatch attached below, not KlUdp's own path. */
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
        .mmsg_batch     = 0,               /* KlUdp's own batching stays off; the datagram batch is separate */
        .recv_gro       = (!completion && cfg->recv_gro) ? 1 : 0,   /* GRO capture: readiness only (M5.4) */
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
        .accepted_rx_caps = prep.rx_caps,   /* M5.4 §6.2: the socket's enabled RX mask → the GRO gate half */
    };
    if (kl_datagram_init_ex(&s->dg, &dc, eff_budget) != 0) {
        s->last_error = kl_datagram_last_error(&s->dg);   /* copy BEFORE closing */
        kl_sock_close(ctx->sockets, prep.fd);             /* init_ex failed before adoption → caller closes (§4/§8) */
        return -1;
    }
    /* fd is now adopted by s->dg; every failure below tears the datagram down (which closes the fd). */

    /* M5.4: opt into readiness recv batching / GRO from the existing knobs (§8). A RECV KlDatagramBatch
     * is attached BEFORE recv_start. mmsg_batch is normalized EXACTLY as KlUdp (udp.c): 0 → default 16,
     * capped to [1, 64], 1 disabling batching — so the legacy default fast path is preserved and a huge
     * input can't request an enormous allocation. */
    int mmsg = cfg->mmsg_batch > 0 ? cfg->mmsg_batch : 16;   /* 0 = default */
    if (mmsg > 64) mmsg = 64;                                 /* cap (KlUdp parity) */
    if (mmsg < 1)  mmsg = 1;                                  /* 1 = batching disabled */

    /* GRO capture was enabled on the socket iff the ACCEPTED RX mask carries it — and once it is on, the
     * splitting extension is CORRECTNESS-CRITICAL: the single-flight receiver cannot split a coalesced
     * buffer, so multiple logical datagrams would reach the handler as one. So a GRO-enabled socket makes
     * the RECV batch MANDATORY (keyed to the accepted bit, not merely provider support). */
    int gro_enabled = (prep.rx_caps & KL_DGRAM_RX_GRO) != 0;
    unsigned pcaps = kl_datagram_provider_caps(&s->dg);
    int want_batch = (mmsg > 1) && (pcaps & KL_DGRAM_CAP_RX_BATCH);
    int attached = 0;
    if (want_batch || gro_enabled) {
        int    n     = (mmsg > 1) ? mmsg : 1;   /* GRO-only (batching disabled) → 1 slot: single-recv + split */
        size_t bufsz = recv_cap;
        if (gro_enabled && bufsz < 65535) bufsz = 65535;   /* a GRO-coalesced buffer holds many segments */
        KlDatagramBatch *rb = kl_datagram_batch_create(&s->dg, KL_DGRAM_BATCH_RECV, n, bufsz);
        if (rb) {
            if (kl_datagram_recv_attach_batch(&s->dg, rb) == 0) attached = 1;
            else kl_datagram_batch_free(rb);
        }
    }
    /* Batching alone is a throughput optimization — its failure falls back to the correct single-flight
     * recv. But a GRO-enabled socket without ACTIVE splitting is a boundary-violation hazard. Splitting is
     * active only under the §6.2 TWO-PART gate — the attached batch AND provider CAP_GRO (gro_enabled
     * already carries the accepted-RX half). A batch that attaches with the provider half missing does NOT
     * split, so an attached-but-inactive batch is just as hazardous as no batch: fail loud (teardown
     * closes the fd) rather than deliver coalesced-unsplit datagrams. */
    int gro_split_active = attached && (pcaps & KL_DGRAM_CAP_GRO);
    if (gro_enabled && !gro_split_active) {
        s->last_error = KL_ERR_UNSUPPORTED;
        kl_datagram_teardown(&s->dg, NULL, NULL);
        return -1;
    }

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
