#ifndef KEEL_SRC_DATAGRAM_OPEN_H
#define KEEL_SRC_DATAGRAM_OPEN_H

#include <keel/handle.h>        /* KlSocketHandle, KL_INVALID_SOCKET */
#include <keel/error.h>         /* KlError */
#include <keel/socket_dgram.h>  /* KL_DGRAM_RX_* — the capture-option bits rx_caps carries */

#include <stdint.h>

struct KlSocketProvider;          /* src/socket.h — the socket axis provider */
struct KlDatagramSocketConfig;    /* keel/datagram.h — the datagram socket-option config (borrowed) */

/*
 * Result of kl_datagram_open — the prepared fd plus the capture options the provider's configure()
 * ACTUALLY enabled. configure() folds reuse/bufs/broadcast/TOS/multicast setup AND turns on per-datagram
 * pktinfo/GRO/TOS capture, reporting (as a KL_DGRAM_RX_* bitmask) which of the capture options the kernel
 * accepted. That mask cannot be reconstructed after the fact by inspecting the vtable or re-running
 * configure(), so it is surfaced here for the KlDatagram consumer / capability derivation / source-pinned
 * replies (which need pktinfo) to consume.
 */
typedef struct {
    KlSocketHandle fd;       /* prepared (+bound iff cfg->bind_addr) fd on success; KL_INVALID_SOCKET on failure */
    uint32_t       rx_caps;  /* bitmask of KL_DGRAM_RX_* (PKTINFO/GRO/TOS) configure() enabled; 0 on failure */
    KlError        err;      /* KL_ERR_NONE on success; the failing step's KlError otherwise */
} KlDatagramPrep;

/*
 * Provider-neutral datagram socket-preparation helper
 * (docs/archive/designs/datagram_consolidation_design.md §2).
 *
 * Does the datagram socket prep, minus the send/receive machine:
 *
 *     kl_sock_socket(SOCK_DGRAM) -> set cloexec + nonblocking
 *       -> provider->configure(...) -> kl_sock_bind (only if cfg->bind_addr)
 *
 * and STOPS at bind. The completion-loop association is instead done
 * INSIDE kl_datagram_init (as KlDgramCore's pre-adoption hook), so a bind-prepared fd is exactly
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
 * `sockets` NULL selects the built-in default provider (mirrors the datagram initializer's NULL-sockets handling: the
 * kl_sockdef_* seam + default datagram ops). A non-NULL provider is rejected with KL_ERR_INVALID_ARG,
 * BEFORE any fd is created, if it has no datagram data-plane (no .dgram) OR a partial one that lacks the
 * required configure() op. `cfg` is required (it carries family/bind + the provider->configure
 * socket-option knobs); NULL is rejected with KL_ERR_INVALID_ARG.
 *
 */
int kl_datagram_open(const struct KlSocketProvider *sockets,
                     const struct KlDatagramSocketConfig *cfg,
                     KlDatagramPrep *out);

struct KlDatagram;   /* keel/datagram.h (opaque layout in keel/datagram_detail.h) */

/* Reclaim callback: frees the object that EMBEDS the KlDatagram (e.g. the resolver). Invoked exactly
 * once by kl_datagram_teardown when the datagram has been reclaimed — synchronously if no busy frame is
 * active, or at the outermost busy-frame leave otherwise. NULL if the handle is caller-managed (e.g.
 * stack-allocated), in which case only the datagram's own heap state is reclaimed. */
typedef void (*KlDatagramReclaimFn)(void *ctx);

/*
 * SYNCHRONOUS owner-destruction teardown of a KlDatagram (internal) — for a consumer bound to a
 * synchronous free contract, i.e. that must fully reclaim the handle before returning (the DNS
 * resolver). See docs/archive/designs/datagram_sync_teardown_design.md §4a.
 *
 * The public lifecycle (kl_datagram_close_begin/cancel then kl_datagram_free) is confirmed-detachment: on
 * a completion backend the recv op's cancel terminal drains on a LATER loop tick, so `free` is refused
 * until then — the object cannot be reclaimed synchronously. kl_datagram_teardown instead ABANDONS any
 * in-flight op and reclaims immediately. It is safe because: a recv op's inbound buffer is
 * life-token-owned (freed on the token's final release when the op is finally reaped — or intentionally
 * pinned by an EFI-quarantined recv OR send); a send op's payload is a backend-owned copy, so
 * the send slots free safely; the token is marked dead so late completions drop; NO user close callback
 * is invoked and NO public terminal is reported.
 *
 * REENTRANCY: if called from within a send/recv delivery frame (on_recv / on_writable /
 * on_drain), the reclamation is DEFERRED to the outermost frame leave — still within the same top-level
 * dispatch, so no machine state is freed under an active frame and no caller pump is needed. `reclaim`
 * (if non-NULL) frees the embedding owner as the destructive tail of that reclamation, whether it runs
 * synchronously or deferred — so the caller MUST NOT free the owner itself.
 *
 * IDEMPOTENT: a NULL/already-reclaimed handle is a no-op success (returns 0, `reclaim` NOT invoked).
 * Returns 0, or -1 only for a NULL `dg`. The ordinary confirmed-detachment path is unchanged.
 */
int kl_datagram_teardown(struct KlDatagram *dg, KlDatagramReclaimFn reclaim, void *reclaim_ctx);

#endif /* KEEL_SRC_DATAGRAM_OPEN_H */
