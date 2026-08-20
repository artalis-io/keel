/*
 * datagram_open.c — M0 provider-neutral datagram socket-preparation helper.
 *
 * A standalone function that reproduces kl_udp_init's create/configure/bind prep (src/udp.c:501-529)
 * with the same per-step error cleanup, but WITHOUT the KlUdp machine and WITHOUT the completion-loop
 * association (kl_datagram_init owns that). It is transport INFRASTRUCTURE — it legitimately creates and
 * binds a platform datagram socket through the socket seam, exactly like listener.c / connect_op.c /
 * udp.c — so it lives below the Tier-1 facade (datagram.c) and is on the TIER1_INFRA allowlist. See
 * src/datagram_open.h for the ownership-handoff contract and docs/datagram_consolidation_design.md §2.
 */

#include "datagram_open.h"

#include <keel/datagram.h>   /* KlDatagramSocketConfig + KlDatagramOps — the provider datagram data-plane (configure) */
#include <keel/sockaddr.h>   /* kl_sockaddr_parse / kl_sockaddr_family / KL_AF_INET6 */

#include "socket.h"          /* seam: kl_sock_socket/_bind/_close/_set_*, KlSocketProvider, kl_sockdef_dgram
                              *       (transitively supplies SOCK_DGRAM / AF_INET / AF_INET6 via sockcompat.h) */

int kl_datagram_open(const struct KlSocketProvider *sockets,
                     const struct KlDatagramSocketConfig *cfg,
                     KlDatagramPrep *out) {
    const KlDatagramOps *dg;
    void *sp_ctx;
    int family;
    KlSockAddr bind_sa;
    int have_bind = 0;

    if (!out)
        return -1;   /* the fd is returned through out — nothing to do without it */
    out->fd = KL_INVALID_SOCKET;
    out->rx_caps = 0;
    out->err = KL_ERR_INVALID_ARG;   /* default: the early-reject (bad-arg) reason */

    if (!cfg)
        return -1;   /* nothing created — the caller reclaims nothing */

    /* The datagram data-plane the provider supplies. NULL sockets = the built-in default provider (its
     * default datagram ops), mirroring kl_udp_init's NULL handling. Reject BEFORE creating an fd a
     * provider that either has no datagram vtable OR a partial one missing the required configure() op —
     * a partial vtable would otherwise crash at the configure() call. */
    dg = sockets ? sockets->dgram : kl_sockdef_dgram();
    sp_ctx = sockets ? sockets->context : NULL;
    if (!dg || !dg->configure)
        return -1;   /* KL_ERR_INVALID_ARG — no/partial datagram data-plane */

    /* Resolve family + optional numeric bind address — identical to kl_udp_init (src/udp.c:486-499).
     * A bind address is numeric (no DNS), so this stays pure and works on lwIP too; its family wins. */
    family = cfg->family;
    if (cfg->bind_addr) {
        if (kl_sockaddr_parse(&bind_sa, cfg->bind_addr, cfg->bind_port) != 0)
            return -1;   /* KL_ERR_INVALID_ARG — bad bind address; nothing created */
        family = (kl_sockaddr_family(&bind_sa) == KL_AF_INET6) ? AF_INET6 : AF_INET;
        have_bind = 1;
    } else if (family != AF_INET && family != AF_INET6) {
        family = AF_INET;
    }

    /* 1. create */
    out->fd = kl_sock_socket(sockets, family, SOCK_DGRAM, 0);
    if (!kl_handle_valid(out->fd)) {
        out->fd = KL_INVALID_SOCKET;   /* nothing to close */
        out->err = KL_ERR_SOCKET;
        return -1;
    }

    /* 2. cloexec (best-effort) + nonblocking (required) */
    kl_sock_set_cloexec(sockets, out->fd);
    if (kl_sock_set_nonblocking(sockets, out->fd) < 0) {
        out->err = KL_ERR_SOCKET;
        goto fail_close;
    }

    /* 3. provider datagram socket-option setup (reuse/bufs, broadcast/TOS/multicast, per-datagram
     *    capture). Best-effort and folded into one call before bind — like kl_udp_init, which never
     *    fails here. Its return value is the bitmask of KL_DGRAM_RX_* capture options the kernel
     *    accepted; surface it (it cannot be reconstructed later). */
    out->rx_caps = dg->configure(sp_ctx, out->fd, family, cfg);

    /* 4. bind (only when a bind address was supplied) */
    if (have_bind && kl_sock_bind(sockets, out->fd, &bind_sa) < 0) {
        out->err = KL_ERR_BIND;
        goto fail_close;
    }

    out->err = KL_ERR_NONE;
    return 0;   /* out->fd prepared + (optionally) bound — ownership transfers to the caller */

fail_close:
    /* A post-create step failed: close the fd we own so the caller reclaims nothing. */
    kl_sock_close(sockets, out->fd);
    out->fd = KL_INVALID_SOCKET;
    out->rx_caps = 0;
    return -1;
}
