# UDP source-address on wildcard binds (IP_PKTINFO): Design

Status: **done.**
Decisions taken (2026-07-18):
- Expose the datagram's **local (destination) address** by **extending the
  `KlUdpRecvFn` signature** (breaking, `KlUdp` is unreleased).
- `KlUdpServer` replies **automatically** from the captured local address.
- `KlUdpServer` **auto-enables** pktinfo when bound to a wildcard address.

## Problem

A UDP socket bound to `0.0.0.0` / `::` on a multi-homed host replies via
`sendto`, whose source address the kernel picks from the routing table, which
can differ from the address the client actually sent to, so the client drops the
reply. Correct behavior requires capturing the *local* address each datagram
arrived on (`IP_PKTINFO` / `IPV6_PKTINFO` control messages on `recvmsg`) and
sending the reply *from* that address (`sendmsg` with the matching cmsg). This is
also a hard requirement for a QUIC server.

## `KlUdp` changes

- **Config:** `KlUdpConfig.recv_pktinfo` (opt-in). On init, enable the receive
  option for the socket family: `IP_PKTINFO` (Linux) / `IP_RECVPKTINFO` (BSD/
  macOS) for v4, `IPV6_RECVPKTINFO` for v6. Guarded by `#ifdef`; a platform
  without support degrades gracefully (local address reported as absent).
- **Receive:** `recvmsg` already carries a control buffer; parse `CMSG` for the
  destination address into a per-socket `recv_local` scratch. The recv callback
  gains `local` / `local_len` params (NULL / 0 when pktinfo is off/unavailable):

  ```c
  typedef void (*KlUdpRecvFn)(KlUdp *udp, const void *data, size_t len,
                              const struct sockaddr *src, socklen_t src_len,
                              const struct sockaddr *local, socklen_t local_len,
                              void *user_data);
  ```

- **Send-from-source:** a new
  `kl_udp_send_to_from(udp, data, len, dest, dest_len, src, src_len)` sends via
  `sendmsg` with a source-address cmsg (`ipi_spec_dst` for v4, `ipi6_addr` for
  v6). `src == NULL` behaves exactly like `kl_udp_send_to`. The buffered
  send-queue node gains an optional source address so backpressured replies keep
  the correct source. `kl_udp_send_to` stays a thin wrapper (src = NULL).

## `KlUdpServer` changes

- **Auto-enable:** when `bind_addr` is a wildcard (`0.0.0.0` / `::` / NULL →
  `0.0.0.0`), set `recv_pktinfo` on the underlying `KlUdp`. Specific-address
  binds skip it (no overhead, source is unambiguous).
- **Automatic reply source:** the trampoline records each datagram's local
  address; `kl_udp_server_reply` uses `kl_udp_send_to_from` with it, so replies
  egress from the address the client hit. `KlUdpHandlerFn` is **unchanged**;
  the correctness is transparent to the handler.

## Portability

- v6 (`IPV6_RECVPKTINFO` / `IPV6_PKTINFO`) is RFC 3542: Linux + macOS.
- v4 enable-option differs (`IP_PKTINFO` on Linux, `IP_RECVPKTINFO` on BSD/
  macOS); the cmsg type is `IP_PKTINFO`. Both guarded by `#ifdef`; absence is a
  graceful no-op (local address absent, replies fall back to `sendto`).

## Tests

- **Linux** (the whole `127.0.0.0/8` is local): bind `KlUdp` to `0.0.0.0` with
  `recv_pktinfo`, send to `127.0.0.2`, assert the callback's `local` ==
  `127.0.0.2`. `KlUdpServer` echo bound to `0.0.0.0`: a client sending to
  `127.0.0.2` receives the reply *from* `127.0.0.2`.
- **macOS** (only `127.0.0.1` local by default): skip the multi-address cases;
  still assert enabling doesn't break normal single-address round-trips.
- pktinfo **off** → callback `local` is NULL.

## Non-goals

- GSO/GRO, `recvmmsg` batching, ECN/TOS: separate roadmap items (the recv
  callback can gain those later without another signature break by carrying them
  in the same call).
