# Keel on lwIP (socket + event providers)

A bring-your-own **lwIP platform** for Keel: a matched pair of a `KlSocketProvider`
(over the lwIP BSD socket API) and a `KlEventProvider` (over `lwip_poll`), so Keel
runs on an lwIP TCP/IP stack with no kernel sockets. lwIP is **not vendored** —
build against your own lwIP (`LWIP_DIR`) + `lwipopts.h`.

- `socket_lwip.c` — `KlSocketProvider` mapping `KlSocketOps` → `lwip_*`.
- `event_lwip.c` — `KlEventProvider` over `lwip_poll` (the runtime event-backend
  seam; its `native_provider()` returns the lwIP socket provider, so installing
  `event_provider` auto-wires the matched `sockets`).
- `resolve_sync_lwip.c` — blocking name resolution over `lwip_getaddrinfo` (the
  client-axis seam; overrides the stock host `kl_resolve_sync` at link time).
- `udp_io_lwip.c` — the datagram-I/O seam (`kl_udp_io_*`) over lwIP sockets
  (`lwip_sendto`/`lwip_recvfrom`), so `KlUdp` — hence `udp_server` and the built-in
  async DNS resolver — runs on lwIP. Unlike the runtime-injected socket/event
  providers, `udp_io` is a build/link seam, so this TU includes Keel's *internal*
  headers (`-I../../src`) and overrides the stock `udp_io_posix.o` at link time.
  Per-datagram only (lwIP has no recvmmsg/GSO/GRO/pktinfo).
- `platform_wakeup_lwip.c` — self-connected lwIP UDP wakeup (responsive
  `kl_server_stop`; overrides the generic `src/platform_wakeup_*` seam).
- `keel_lwip.h` — `kl_socket_provider_lwip()` + `kl_event_provider_lwip()`.
- `lwipopts.h` — a **sample** host/loopback config (not blessed for production).

## Build-gate (what CI runs)

```sh
make check LWIP_DIR=/path/to/lwip     # compile the providers against your lwIP
```

A clean compile **is** the validation: it proves Keel's public `KlSocketProvider`
+ `KlEventProvider` API is sufficient to author an lwIP platform using **only**
public Keel headers — no internal Keel or host-POSIX types.

## Pure runtime provider (finding dissolved)

lwIP now runs against a **stock `libkeel.a`** — no `-DKEEL_PLATFORM_LWIP` library
build. It is a pure drop-in like the mbedTLS/nghttp2 vtable integrations.

An earlier pass found a real boundary: `struct sockaddr`'s layout is compile-time
(lwIP has `sin_len`, Linux doesn't), so a host-built libkeel filled a host-layout
`sockaddr` that `lwip_bind` misread (`bind: EIO`), and `getaddrinfo` was baked in.
The **KlSockAddr address-ABI neutralization** (`docs/keel_sockaddr_design.md`)
dissolved it:

- Core speaks the Keel-owned, fixed-layout **`KlSockAddr`** everywhere; a platform
  `struct sockaddr` exists only inside socket providers. `socket_lwip.c` marshals
  `KlSockAddr` ↔ lwIP `sockaddr` at the boundary, so **no host-layout sockaddr
  ever reaches lwIP**.
- Server bind parses the numeric address with the pure `kl_sockaddr_parse` (no
  `getaddrinfo`); the clients' blocking name resolution is confined to a
  platform-swappable `resolve_sync` TU.

So **both axes are runtime-injectable**: the event backend (`KlEventProvider`) and
the socket/address provider (`KlSocketProvider` + `KlSockAddr`). No library
recompile.

`lwip_loopback_test.c` is the proof — the server, client, and datagram axes on
lwIP, linked against a **stock** `libkeel`:
- a raw lwIP client → the Keel **server** (`200 OK`),
- a Keel async **client** on the lwIP providers → the same server (`200`), with
  name resolution via `resolve_sync_lwip.c` (`lwip_getaddrinfo`, linked ahead of
  the stock lib so it overrides the host `kl_resolve_sync`), and
- a Keel **`KlUdp` echo** on the lwIP providers, bounced by a raw lwIP UDP client
  (`udp_io_lwip.c` overriding the stock `udp_io_posix.o`).

```sh
make -C ../..                       # stock libkeel.a (any backend)
make loopback LWIP_DIR=/path/to/lwip
# -> keel: listening on 127.0.0.1:8080
#    lwIP loopback: raw client -> Keel server replied 200 OK (correct)
#    lwIP loopback: Keel client on lwIP got 200 (correct)
#    lwIP loopback: Keel UDP echo on lwIP round-tripped (correct)
```

A third phase runs a Keel `KlUdp` echo on the lwIP providers (`udp_io_lwip.c`),
exercised by a raw lwIP UDP client — proving the datagram axis end to end.

Run in CI by the **Integration (lwIP)** job (clones lwIP + lwip-contrib, stock
libkeel, `make loopback`), so the payoff is regression-protected.

**Responsive stop** is wired: `platform_wakeup_lwip.c` provides a self-connected
lwIP UDP wakeup (overriding the generic `src/platform_wakeup_*` seam at link time),
so `kl_server_stop` wakes `lwip_poll` immediately rather than on the next tick.

**Not yet on lwIP:** TLS-over-lwIP; `lwipopts.h` is a sample, not
production-blessed. (udp is now on lwIP via `udp_io_lwip.c` — the base for
`udp_server` + the built-in async DNS resolver.)

## Tested versions

| lwIP | Status |
|------|--------|
| STABLE-2.2.0 | **Loopback verified: server + client + UDP** (200 OK + UDP echo on stock libkeel) + build-gate |
| 2.1.x | Expected to work (same `lwip_poll` + BSD socket API) |

## Scope

lwIP is **readiness-only** (its sockets layer is `lwip_poll`); the lwIP *raw*
callback API (completion model) is out of scope. See
`docs/lwip_platform_design.md` for the full platform-port shape + promotion path.
