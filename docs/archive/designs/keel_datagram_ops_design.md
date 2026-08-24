# Datagram I/O as a socket-provider vtable (`KlDatagramOps`)

**Status:** in progress (staged). Resolves axis-audit finding A2, the `udp_io`
datagram data-plane was a **compile/link** seam (`Makefile UDP_IO_SRC` +
link-override) while the socket lifecycle was a **runtime** `KlSocketProvider`
vtable. The two had to be paired consistently (a foreign stack's socket provider
*and* its `udp_io_*.o`) with nothing enforcing it: set `sockets = socket_lwip` but
forget to link `udp_io_lwip` → an lwIP fd reaches `udp_io_posix` → `sendmsg` on an
lwIP descriptor. Folding the datagram data-plane into `KlSocketProvider` makes the
pairing impossible by construction: one runtime provider owns both stream and
datagram I/O.

## What moves, what stays

`udp.c` today calls `kl_udp_io_*` (link symbols) that **mix two concerns**:

- **Platform primitives**, the raw syscalls + platform types: one-datagram
  send (sendmsg + optional pktinfo/TOS cmsg), one-datagram recv (recvmsg + cmsg
  parse → src/local/gro/tos), GSO send, the mmsg batch engines, and the
  datagram socket-option setup (pktinfo/GRO/TOS/reuse/bufs/broadcast/multicast).
- **Machine logic**: walking `udp->q_head`, backpressure/drop accounting,
  `kl_udp_deliver`, `kl_udp_update_interest`, `on_drain`, recv-active re-checks.

Only the **primitives** belong on the socket provider. So:

- **Primitives → `KlDatagramOps`** (a new optional sub-vtable on `KlSocketProvider`).
- **Machine logic → `udp.c`** (was in `udp_io_*.c`): `kl_udp_io_flush_queue` /
  `kl_udp_io_recv_drain` become portable loops in `udp.c` that call the vtable
  primitives; the platform TUs keep only the primitives.

## The vtable

```c
/* Per-datagram receive metadata a provider fills from control messages. */
typedef struct {
    KlSockAddr local;     /* pktinfo dest addr; valid iff has_local */
    int        has_local;
    int        gro_seg;   /* GRO coalesced segment size, 0 = none */
    int        tos;       /* received TOS/TCLASS byte, or -1 */
    int        truncated; /* datagram was truncated to the buffer */
} KlDgramRxMeta;

/* KlDatagramOps: authoritative definition in include/keel/datagram.h. Every op
 * takes (ctx, fd, …) and speaks KlSockAddr, never KlUdp, so the provider owns no
 * machine state. Present iff caps & KL_SOCK_CAP_DATAGRAM. */
typedef struct {
    /* Per-datagram data-plane. */
    kl_ssize_t (*send)(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                       const KlSockAddr *dest, const KlSockAddr *src, int tos);
    kl_ssize_t (*recv)(void *ctx, KlSocketHandle fd, void *buf, size_t buflen,
                       KlSockAddr *src, KlDgramRxMeta *meta);
    kl_ssize_t (*send_gso)(void *ctx, KlSocketHandle fd, const void *data, size_t len,
                           uint16_t seg, const KlSockAddr *dest);   /* optional */

    /* Socket options, the three init-time setups folded into one pre-bind call
     * that returns the KL_DGRAM_RX_* bitmask the kernel accepted (design-review
     * change); set_tos + mcast stay separate (dynamic, post-init). */
    uint32_t (*configure)(void *ctx, KlSocketHandle fd, int family,
                          const struct KlUdpConfig *cfg);
    int (*set_tos)(void *ctx, KlSocketHandle fd, int family, int tos);
    int (*mcast_membership)(void *ctx, KlSocketHandle fd, int family,
                            const char *group, unsigned iface_index, int join);

    /* Optional mmsg batching: DATA-oriented, no callbacks (design-review change).
     * recv_batch drains one recvmmsg into the (KlUdp-owned) batch + fills a
     * caller-owned KlDgramRxSlot[]; send_batch takes a udp.c-built KlDgramTxDesc[].
     * All NULL → udp.c uses the per-datagram loop. */
    void *(*rx_batch_new)(KlAllocator *a, int n, size_t bufsz);
    void *(*tx_batch_new)(KlAllocator *a, int n);
    void  (*rx_batch_free)(KlAllocator *a, void *rx_batch);
    void  (*tx_batch_free)(KlAllocator *a, void *tx_batch);
    int   (*recv_batch)(void *ctx, KlSocketHandle fd, void *rx_batch,
                        KlDgramRxSlot *slots, int max);
    int   (*send_batch)(void *ctx, KlSocketHandle fd, void *tx_batch,
                        const KlDgramTxDesc *descs, int n);
} KlDatagramOps;
```

`KlSocketProvider` gains `const struct KlDatagramOps *dgram;` and a
`KL_SOCK_CAP_DATAGRAM` flag. `socket_posix`/`socket_winsock`/`socket_lwip` set it;
the overlapped completion providers leave it NULL (they never run the readiness
datagram path). `udp.c` dispatches through `kl_sock_dgram_*` inline helpers.

**Design-review decisions (why the shape above):** batching is kept but expressed
as *data* (slot/descriptor arrays) rather than `deliver`/`next` callback thunks,
the `recvmmsg`/`sendmmsg` syscalls stay in the provider, the queue-walk + delivery
stay in `udp.c`, and no function pointer crosses the vtable per datagram. The three
init-time socket-option setups collapse into one `configure()` returning the
accepted-capture bitmask.

## Migration (staged; each stage builds+tests green on every platform)

A temporary fallback keeps the tree green while providers migrate one platform at a
time: `udp.c` uses `provider->dgram` when present, else the existing `kl_udp_io_*`
link seam.

1. **Vtable + POSIX**: add `KlDatagramOps`, move the machine loops into `udp.c`,
   implement the POSIX primitives on `socket_posix`, keep the seam fallback.
   (kqueue + epoll/io_uring green.)
2. **Winsock**: `socket_winsock` datagram primitives.
3. **lwIP**: ✅ **done.** The datagram ops are folded onto `socket_lwip.c` (public
   headers only, the `(ctx, fd)` primitives need no internal Keel types) and
   `udp_io_lwip.c` is deleted. lwIP UDP now rides the same provider it already sets,
   so the A2 pairing is dissolved: a foreign stack supplies one provider with both
   stream + datagram, no separate link artifact. Proven by the container loopback
   (UDP echo + HTTPS) with no `udp_io_lwip`.
4. **Remove the seam**: ✅ **done.** Deleted the `kl_udp_io_*` fallback,
   `udp_io_posix.c`, `udp_io_win.c`, `udp_io.h`, and `Makefile UDP_IO_SRC`. The
   shared cmsg parsers the *completion* backends still need moved to standalone
   `udp_cmsg.c` (POSIX) / `udp_cmsg_win.c` (Winsock). `udp.c` now requires the
   provider to carry `.dgram` (`kl_udp_init` fails otherwise); `udp_dg()` resolves a
   NULL `KlEventCtx.sockets` to the built-in default via `kl_sockdef_dgram()`
   (mirroring the `kl_sockdef_*` stream fallback), and the overlapped completion
   providers (io_uring/IOCP/pollcomp) inherit the underlying provider's `.dgram` so
   UDP config/opts work on a completion loop (the data-plane there stays on the
   `kl_comp_post_udp_*` path).

**Refactor complete.** One runtime provider now owns all of a stack's socket I/O
(stream + datagram) on POSIX, Winsock, and lwIP; there is no separate `udp_io`
build/link artifact, and the A2 provider↔`udp_io` pairing is gone.

## Why this is the right end state (not false symmetry)

Not a leak fix (nothing bled upward) but a **mechanism-unification**: after this,
"the socket provider is the one runtime object that owns all socket I/O for a
stack" holds for stream **and** datagram. A foreign stack supplies one provider;
there is no second link-time artifact to keep in sync. It is justified now (not
speculative) because it removes a real, unguarded consistency requirement, the
exact coupling the axis audit (A2) flagged.
