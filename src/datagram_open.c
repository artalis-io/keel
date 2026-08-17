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

#include <keel/udp.h>        /* KlUdpConfig — the datagram socket-option config */
#include <keel/datagram.h>   /* KlDatagramOps — the provider datagram data-plane (configure) */
#include <keel/sockaddr.h>   /* kl_sockaddr_parse / kl_sockaddr_family / KL_AF_INET6 */

#include "socket.h"          /* seam: kl_sock_socket/_bind/_close/_set_*, KlSocketProvider, kl_sockdef_dgram
                              *       (transitively supplies SOCK_DGRAM / AF_INET / AF_INET6 via sockcompat.h) */

KlSocketHandle kl_datagram_open(const struct KlSocketProvider *sockets,
                                const struct KlUdpConfig *cfg,
                                KlError *out_err) {
    KlError err = KL_ERR_INVALID_ARG;   /* default: the early-reject (bad-arg) reason */
    KlSocketHandle fd = KL_INVALID_SOCKET;
    const KlDatagramOps *dg;
    void *sp_ctx;
    int family;
    KlSockAddr bind_sa;
    int have_bind = 0;

    if (!cfg)
        goto done;   /* nothing created — the caller reclaims nothing */

    /* The datagram data-plane the provider supplies. NULL sockets = the built-in default provider (its
     * default datagram ops), mirroring kl_udp_init's NULL handling. A concrete provider without a
     * datagram vtable cannot back a datagram socket → reject before creating an fd. */
    dg = sockets ? sockets->dgram : kl_sockdef_dgram();
    sp_ctx = sockets ? sockets->context : NULL;
    if (!dg)
        goto done;   /* KL_ERR_INVALID_ARG — provider lacks KL_SOCK_CAP_DATAGRAM */

    /* Resolve family + optional numeric bind address — identical to kl_udp_init (src/udp.c:486-499).
     * A bind address is numeric (no DNS), so this stays pure and works on lwIP too; its family wins. */
    family = cfg->family;
    if (cfg->bind_addr) {
        if (kl_sockaddr_parse(&bind_sa, cfg->bind_addr, cfg->bind_port) != 0)
            goto done;   /* KL_ERR_INVALID_ARG — bad bind address; nothing created */
        family = (kl_sockaddr_family(&bind_sa) == KL_AF_INET6) ? AF_INET6 : AF_INET;
        have_bind = 1;
    } else if (family != AF_INET && family != AF_INET6) {
        family = AF_INET;
    }

    /* 1. create */
    fd = kl_sock_socket(sockets, family, SOCK_DGRAM, 0);
    if (!kl_handle_valid(fd)) {
        err = KL_ERR_SOCKET;
        fd = KL_INVALID_SOCKET;   /* nothing to close */
        goto done;
    }

    /* 2. cloexec (best-effort) + nonblocking (required) */
    kl_sock_set_cloexec(sockets, fd);
    if (kl_sock_set_nonblocking(sockets, fd) < 0) {
        err = KL_ERR_SOCKET;
        goto fail_close;
    }

    /* 3. provider datagram socket-option setup (reuse/bufs, broadcast/TOS/multicast, per-datagram
     *    capture). Best-effort and folded into one call before bind — like kl_udp_init, which never
     *    fails here; the accepted-capture bitmask is the KlDatagram consumer's concern after init, not
     *    this preparation helper's. */
    (void)dg->configure(sp_ctx, fd, family, cfg);

    /* 4. bind (only when a bind address was supplied) */
    if (have_bind && kl_sock_bind(sockets, fd, &bind_sa) < 0) {
        err = KL_ERR_BIND;
        goto fail_close;
    }

    if (out_err)
        *out_err = KL_ERR_NONE;
    return fd;   /* prepared + (optionally) bound — ownership transfers to the caller */

fail_close:
    /* A post-create step failed: close the fd we own so the caller reclaims nothing. */
    kl_sock_close(sockets, fd);
    fd = KL_INVALID_SOCKET;
done:
    if (out_err)
        *out_err = err;
    return KL_INVALID_SOCKET;
}
