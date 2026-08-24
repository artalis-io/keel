# UDP Batching + Segmentation Offload: Design

Status: **Complete; Phase A (recvmmsg/sendmmsg batching) + Phase B (GSO/GRO)
implemented 2026-07-19.**
Decisions: GSO via a **new per-call send API** (`kl_udp_send_gso`, with a portable
per-segment fallback); GRO delivered **both ways**; transparent per-segment split
by default, plus an opt-in coalesced-buffer callback; `recvmmsg`/`sendmmsg`
batching **always-on** where the syscall exists (batch-size knob), GSO/GRO
**opt-in**.

Amortizes syscall cost on the existing single-loop UDP substrate for high-PPS
services and lays groundwork for a future QUIC/HTTP-3 data path. All four
mechanisms are **Linux-only**; every other platform (macOS, BSD, Cosmopolitan)
transparently keeps today's per-datagram `recvmsg`/`sendto` loop.

---

## Background: the four mechanisms

Today `udp_recv_drain` loops `recvmsg` one datagram at a time into a single
`recv_buf` until EAGAIN (one syscall/datagram); `udp_flush_queue` drains a FIFO of
individually-allocated `KlUdpDatagram` nodes via `sendto`/`send` (one
syscall/datagram), each optionally carrying a source-pinning `IP_PKTINFO` cmsg.

| Mechanism | Dir | Kernel API | Effect |
|-----------|-----|-----------|--------|
| `recvmmsg` | RX | `recvmmsg(2)` | Up to N datagrams per syscall. Transparent. |
| `sendmmsg` | TX | `sendmmsg(2)` | Up to N queued datagrams per syscall. Transparent. |
| **GRO** | RX | `UDP_GRO` sockopt + cmsg | Kernel coalesces same-flow datagrams into one buffer, reports segment size via a control message. |
| **GSO** | TX | `UDP_SEGMENT` cmsg | One `sendmsg` of a multi-segment buffer; kernel splits into MTU-sized datagrams on the wire. |

## Surface

### `KlUdpConfig` additions

```c
int  mmsg_batch;   /* recvmmsg/sendmmsg batch size; 0 = default (16), 1 = disable
                    * (per-datagram). Capped at UDP_MMSG_MAX (64). RX pre-allocates
                    * mmsg_batch × recv_buf_size + per-slot sockaddr/control. */
int  recv_gro;     /* 1 = enable UDP_GRO receive coalescing (opt-in; Linux >=5.0). */
```

GSO needs **no socket flag**; it is a per-message `UDP_SEGMENT` cmsg, so calling
`kl_udp_send_gso` is itself the opt-in.

### New send API: GSO

```c
/* Send `buf` (total_len bytes) as a run of UDP datagrams of `segment_size` each
 * (the final segment may be shorter) to `dest`. On Linux this is ONE sendmsg
 * with a UDP_SEGMENT cmsg (kernel/NIC splits on the wire); elsewhere (or if the
 * kernel rejects GSO) it falls back to sending each segment individually; the
 * on-wire result is identical, only the syscall count differs. Backpressure:
 * queued whole (one node, gso_size = segment_size) on EAGAIN.
 * Requires 0 < segment_size <= total_len; total_len <= 65507. */
int kl_udp_send_gso(KlUdp *udp, const void *buf, size_t total_len,
                    size_t segment_size, const struct sockaddr *dest,
                    socklen_t dest_len);
```

### GRO delivery: transparent split (default) or coalesced (opt-in)

With `recv_gro` on, the kernel may hand back a coalesced buffer of K segments.
Default: KEEL splits it by the reported segment size and fires the existing
`on_recv` once per logical datagram; **callback semantics unchanged**. A consumer
that wants the whole buffer (zero per-segment dispatch) registers:

```c
typedef void (*KlUdpRecvSegmentsFn)(KlUdp *udp, const void *data, size_t len,
                                    size_t segment_size,
                                    const struct sockaddr *src, socklen_t src_len,
                                    const struct sockaddr *local, socklen_t local_len,
                                    void *user_data);

/* When set (and recv_gro enabled), coalesced buffers are delivered WHOLE to this
 * callback with the segment size (each segment is segment_size bytes except
 * possibly the last); the consumer splits. When NULL (default), coalesced buffers
 * are split internally and delivered one segment at a time to on_recv. A
 * non-coalesced datagram always goes to on_recv. */
void kl_udp_recv_segments(KlUdp *udp, KlUdpRecvSegmentsFn cb, void *user_data);
```

### `KlUdpServer`

Passthrough config (`mmsg_batch`, `recv_gro`), plus `kl_udp_server_send_gso` and
`kl_udp_server_recv_segments` mirroring the primitives.

## Implementation notes

- **`KlUdpDatagram`** gains `size_t gso_size;` (0 = normal, >0 = GSO segment size),
  so a queued GSO buffer replays with its `UDP_SEGMENT` cmsg on flush.
- **RX batch state** (lazily allocated in `kl_udp_recv_start` when `mmsg_batch > 1`
  and `recvmmsg` exists): `struct mmsghdr[N]`, `struct iovec[N]`, a
  `N × recv_buf_size` data block, `struct sockaddr_storage[N]`, and an
  `N × CMSG_SPACE(...)` control block (sized for pktinfo **and** the `UDP_GRO`
  cmsg). Freed in `recv_stop`/`free`. The existing single `recv_buf` remains the
  fallback path.
- **`udp_recv_drain`** rework: batched path calls `recvmmsg(N)`; per returned
  message it parses the local address (pktinfo) and the `UDP_GRO` cmsg for the
  segment size, then dispatches (single → `on_recv`; coalesced → `on_recv_segments`
  if set, else internal split). Loops until `recvmmsg` returns `< N` (drained) or
  the callback stops receiving. GRO is parsed in the non-batched path too (a plain
  `recvmsg` can also return a coalesced buffer).
- **`udp_flush_queue`** rework: batched path builds up to N `mmsghdr`s from the
  queue head; each with its `iovec`, `msg_name` (dest), and a per-slot control
  block carrying the source `IP_PKTINFO` and/or `UDP_SEGMENT` cmsg; then
  `sendmmsg`. It returns the count sent; dequeue+free those, keep the rest, stop on
  EAGAIN. Per-message hard errors drop that datagram (`dropped++`), matching
  today's semantics. The `on_drain` callback still fires on the non-empty→empty
  transition.
- **Feature detection:** `recvmmsg`/`sendmmsg` gated on `__linux__` (glibc) /
  availability; a runtime EAGAIN/ENOSYS on the batched call degrades to the
  per-datagram loop for that tick. `UDP_GRO`/`UDP_SEGMENT` gated on the macros
  being defined (`<linux/udp.h>`); `setsockopt(UDP_GRO)` failure → GRO silently
  off; a `UDP_SEGMENT` send that errptrs `EIO`/`ENOTSUP` → per-segment fallback.
- **Header hygiene:** `_GNU_SOURCE` is already set for `recvmmsg`/`struct mmsghdr`;
  `UDP_SEGMENT`/`UDP_GRO` come from `<linux/udp.h>` behind `#ifdef` guards so the
  build stays clean on non-Linux and under `-Werror`.
- **Bounds:** `mmsg_batch` clamped to `[1, 64]`; GSO `total_len <= 65507`,
  `segment_size` in `(0, total_len]`, segment count bounded.

## Phasing

- **Phase A: transparent batching *(DONE 2026-07-19)*:** `recvmmsg` + `sendmmsg`
  with the `mmsg_batch` knob (always-on where available). No API change; the
  syscall-amortization win. Both paths verified; batched on a Linux container
  (compile + tests + ASan/UBSan), fallback on macOS.
- **Phase B: offload *(DONE 2026-07-19)*:** GRO (`recv_gro` + internal split +
  `kl_udp_recv_segments` coalesced callback) and GSO (`kl_udp_send_gso`).
  GSO backpressure degrades to per-segment queued sends through the existing
  queue (no special GSO queue node; simpler than the original sketch and
  equivalent on the wire). `KlUdpServer` passes `recv_gro` through (transparent
  split); the GSO/coalesced-callback advanced surface lives on `KlUdp`. Both
  paths verified; offload on a Linux container (compile + tests + ASan/UBSan +
  the exact CI scan-build), fallback on macOS.

## Non-goals (v1)

`SO_TXTIME`/pacing, hardware timestamping, zerocopy `MSG_ZEROCOPY`, and io_uring
UDP multishot (the io_uring file backend is separate). *(ECN/TOS/DSCP marking,
originally listed here as a separate roadmap item; was implemented 2026-07-19:
`KlUdpConfig.tos`, `kl_udp_set_tos`, per-packet `kl_udp_send_to_tos`, and
`recv_tos` + `kl_udp_recv_tos`.)*

## Tests (`tests/test_udp_batching.c`)

Batching is transparent (falls back to per-datagram), so most tests assert
delivery regardless of platform; offload tests probe and `UTEST_SKIP` where the
kernel lacks support (like the multicast/HE timing tests):

- **recvmmsg drain**: send M datagrams, receive all M (count, order, content)
  with `mmsg_batch = 16`. Passes via fallback on non-Linux.
- **sendmmsg flush**: queue M under forced backpressure (tiny `so_sndbuf`), flush,
  all M delivered; `on_drain` fires once.
- **GSO round-trip**: `kl_udp_send_gso` a 3-segment buffer; the receiver gets 3
  datagrams with the right sizes/content. Works via the per-segment fallback where
  GSO is unavailable, so it asserts on all platforms.
- **GSO validation**: `segment_size == 0`, `> total_len`, `total_len > 65507` →
  `KL_ERR_INVALID_ARG`.
- **GRO split** *(Linux, probe+skip)*; `recv_gro` on; blast same-flow datagrams;
  assert the split reassembles the originals (transparent path) and that the
  coalesced callback, when registered, receives the whole buffer + segment size.
- **batch=1 parity**: `mmsg_batch = 1` behaves exactly like today's loop.

## Delivery

Two commits (Phase A, then Phase B). Full gauntlet each; `make test`, poll
backend, ASan+UBSan, scan-build, cppcheck (run the full `make cppcheck` target
given the added `#ifdef` configs), gcc-14: and CI-green.
