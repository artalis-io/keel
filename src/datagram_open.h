#ifndef KEEL_SRC_DATAGRAM_OPEN_H
#define KEEL_SRC_DATAGRAM_OPEN_H

#include <keel/handle.h>        /* KlSocketHandle, KL_INVALID_SOCKET */
#include <keel/error.h>         /* KlError */
#include <keel/socket_dgram.h>  /* KL_DGRAM_RX_* — the capture-option bits rx_caps carries */

#include <stdint.h>

struct KlSocketProvider;   /* src/socket.h — the socket axis provider */
struct KlUdpConfig;        /* keel/udp.h — the datagram socket-option config (borrowed) */

/*
 * Result of kl_datagram_open — the prepared fd plus the capture options the provider's configure()
 * ACTUALLY enabled. configure() folds reuse/bufs/broadcast/TOS/multicast setup AND turns on per-datagram
 * pktinfo/GRO/TOS capture, reporting (as a KL_DGRAM_RX_* bitmask) which of the capture options the kernel
 * accepted. That mask cannot be reconstructed after the fact by inspecting the vtable or re-running
 * configure(), so it is surfaced here for the KlDatagram consumer / M2 capability derivation / M4
 * (which needs pktinfo for source-pinned replies) to consume.
 */
typedef struct {
    KlSocketHandle fd;       /* prepared (+bound iff cfg->bind_addr) fd on success; KL_INVALID_SOCKET on failure */
    uint32_t       rx_caps;  /* bitmask of KL_DGRAM_RX_* (PKTINFO/GRO/TOS) configure() enabled; 0 on failure */
    KlError        err;      /* KL_ERR_NONE on success; the failing step's KlError otherwise */
} KlDatagramPrep;

/*
 * M0 — provider-neutral datagram socket-preparation helper
 * (docs/datagram_consolidation_design.md §2 D-DNS-2).
 *
 * Mirrors kl_udp_init's prep EXACTLY, minus the KlUdp machine:
 *
 *     kl_sock_socket(SOCK_DGRAM) -> set cloexec + nonblocking
 *       -> provider->configure(...) -> kl_sock_bind (only if cfg->bind_addr)
 *
 * and STOPS at bind. The completion-loop association kl_udp_init performs at init is instead done
 * INSIDE kl_datagram_init (7B-7, as KlDgramCore's pre-adoption hook), so a bind-prepared fd is exactly
 * what kl_datagram_init expects — the helper deliberately does not touch the event loop.
 *
 * Returns 0 on success (out->fd valid, out->rx_caps = the accepted capture mask, out->err KL_ERR_NONE),
 * or -1 on failure (out->fd KL_INVALID_SOCKET, out->rx_caps 0, out->err the failing step's KlError). `out`
 * is REQUIRED (the fd is returned through it); a NULL `out` returns -1 and does nothing.
 *
 * OWNERSHIP HANDOFF. On success out->fd is a prepared (and, if a bind address was given, bound) fd the
 * CALLER OWNS: hand it to kl_datagram_init (which adopts it only on its OWN success, and on failure
 * leaves it with the caller) or close it on your own error path. On ANY step failure the helper CLOSES
 * the fd it created — the caller reclaims nothing (no fd leak, no double-close).
 *
 * `sockets` NULL selects the built-in default provider (mirrors kl_udp_init's NULL-sockets handling: the
 * kl_sockdef_* seam + default datagram ops). A non-NULL provider is rejected with KL_ERR_INVALID_ARG,
 * BEFORE any fd is created, if it has no datagram data-plane (no .dgram) OR a partial one that lacks the
 * required configure() op. `cfg` is required (it carries family/bind + the provider->configure
 * socket-option knobs); NULL is rejected with KL_ERR_INVALID_ARG.
 *
 * No consumer is migrated here (M0 is additive); M3 (DNS) and M4 (KlUdpServer) call this in later,
 * separately-reviewed increments.
 */
int kl_datagram_open(const struct KlSocketProvider *sockets,
                     const struct KlUdpConfig *cfg,
                     KlDatagramPrep *out);

#endif /* KEEL_SRC_DATAGRAM_OPEN_H */
