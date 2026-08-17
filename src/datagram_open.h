#ifndef KEEL_SRC_DATAGRAM_OPEN_H
#define KEEL_SRC_DATAGRAM_OPEN_H

#include <keel/handle.h>   /* KlSocketHandle, KL_INVALID_SOCKET */
#include <keel/error.h>    /* KlError */

struct KlSocketProvider;   /* src/socket.h — the socket axis provider */
struct KlUdpConfig;        /* keel/udp.h — the datagram socket-option config (borrowed) */

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
 * OWNERSHIP HANDOFF. On success it returns a prepared (and, if a bind address was given, bound) fd the
 * CALLER OWNS: hand it to kl_datagram_init (which adopts it only on its OWN success, and on failure
 * leaves it with the caller) or close it on your own error path. On ANY step failure the helper CLOSES
 * the fd it created and returns KL_INVALID_SOCKET — the caller reclaims nothing (no fd leak, no
 * double-close). `out_err` (optional; may be NULL) receives KL_ERR_NONE on success, or the failing
 * step's KlError (KL_ERR_INVALID_ARG / KL_ERR_SOCKET / KL_ERR_BIND).
 *
 * `sockets` NULL selects the built-in default provider (mirrors kl_udp_init's NULL-sockets handling: the
 * kl_sockdef_* seam + default datagram ops). A non-NULL provider WITHOUT a datagram data-plane (no
 * .dgram, i.e. no KL_SOCK_CAP_DATAGRAM) is rejected with KL_ERR_INVALID_ARG before any fd is created.
 * `cfg` is required (it carries family/bind + the provider->configure socket-option knobs); NULL is
 * rejected with KL_ERR_INVALID_ARG.
 *
 * No consumer is migrated here (M0 is additive); M3 (DNS) and M4 (KlUdpServer) call this in later,
 * separately-reviewed increments.
 */
KlSocketHandle kl_datagram_open(const struct KlSocketProvider *sockets,
                                const struct KlUdpConfig *cfg,
                                KlError *out_err);

#endif /* KEEL_SRC_DATAGRAM_OPEN_H */
