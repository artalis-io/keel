# Keel on lwIP (socket + event providers)

A bring-your-own **lwIP platform** for Keel: a matched pair of a `KlSocketProvider`
(over the lwIP BSD socket API) and a `KlEventProvider` (over `lwip_poll`), so Keel
runs on an lwIP TCP/IP stack with no kernel sockets. lwIP is **not vendored** —
build against your own lwIP (`LWIP_DIR`) + `lwipopts.h`.

- `socket_lwip.c` — `KlSocketProvider` mapping `KlSocketOps` → `lwip_*`, **and** the
  datagram data-plane (`KlDatagramOps` — `lwip_sendto`/`lwip_recvfrom`) folded onto
  the same provider, so `KlUdp` (hence `udp_server` and the built-in async DNS
  resolver) runs on lwIP with no separate link artifact. One runtime provider owns
  both stream + datagram I/O (axis-audit A2), using **only public Keel headers**.
  Per-datagram only (lwIP has no recvmmsg/GSO/GRO/pktinfo).
- `event_lwip.c` — `KlEventProvider` over `lwip_poll` (the runtime event-backend
  seam; its `native_provider()` returns the lwIP socket provider, so installing
  `event_provider` auto-wires the matched `sockets`).
- `resolve_sync_lwip.c` — blocking name resolution over `lwip_getaddrinfo` (the
  client-axis seam; overrides the stock host `kl_resolve_sync` at link time).
- `platform_wakeup_lwip.c` — self-connected lwIP UDP wakeup (responsive
  `kl_server_stop`; overrides the generic `src/platform_wakeup_*` seam).
- `keel_lwip.h` — `kl_socket_provider_lwip()` + `kl_event_provider_lwip()`.
- `lwipopts.h` — a **production-oriented baseline** config (Keel provider
  requirements + security hardening + a documented sizing block to TUNE per
  deployment). It is what the loopback + HTTPS tests run against, so the
  provider-critical settings are CI-exercised. Copy + tune for your target, and
  replace the placeholder `LWIP_RAND` with a CSPRNG (see the header's warning).

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
  (the datagram ops on `socket_lwip.c`).

```sh
make -C ../..                       # stock libkeel.a (any backend)
make loopback LWIP_DIR=/path/to/lwip
# -> keel: listening on 127.0.0.1:8080
#    lwIP loopback: raw client -> Keel server replied 200 OK (correct)
#    lwIP loopback: Keel client on lwIP got 200 (correct)
#    lwIP loopback: Keel UDP echo on lwIP round-tripped (correct)
```

A third phase runs a Keel `KlUdp` echo on the lwIP providers (the datagram ops
folded onto `socket_lwip.c`), exercised by a raw lwIP UDP client — proving the
datagram axis end to end.

Run in CI by the **Integration (lwIP)** job (clones lwIP + lwip-contrib, stock
libkeel, `make loopback`), so the payoff is regression-protected.

**Responsive stop** is wired: `platform_wakeup_lwip.c` provides a self-connected
lwIP UDP wakeup (overriding the generic `src/platform_wakeup_*` seam at link time),
so `kl_server_stop` wakes `lwip_poll` immediately rather than on the next tick.

**TLS over lwIP** needs **no lwIP-specific TLS code**: the mbedTLS integration's
socket-BIO can be routed through a `KlSocketProvider`
(`kl_tls_mbedtls_ctx_set_socket_provider(ctx, kl_socket_provider_lwip())`), so a
genuine TLS handshake + HTTPS request runs over lwIP with the *existing*
`socket_lwip.c`. The loopback's optional Phase 4 proves it end to end:

```sh
make loopback-tls LWIP_DIR=/path/to/lwip MBEDTLS_DIR=/path/to/mbedtls
# -> lwIP loopback: Keel HTTPS (mbedTLS) on lwIP handshake + roundtrip OK (correct)
```

**Complete on lwIP:** server + client + UDP + TLS all run on a stock `libkeel.a`.
`lwipopts.h` is a production-oriented baseline (tune the sizing block + supply a
CSPRNG for `LWIP_RAND` per deployment).

## Tested versions

| lwIP | Status |
|------|--------|
| STABLE-2.2.0 | **Loopback verified: server + client + UDP + HTTPS** (200 OK + UDP echo + mbedTLS handshake on stock libkeel) + build-gate |
| 2.1.x | Expected to work (same `lwip_poll` + BSD socket API) |

## Scope

lwIP is **readiness-only** (its sockets layer is `lwip_poll`); the lwIP *raw*
callback API (completion model) is out of scope. See
`docs/lwip_platform_design.md` for the full platform-port shape + promotion path.
