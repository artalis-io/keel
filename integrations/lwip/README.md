# Keel on lwIP (socket + event providers)

A bring-your-own **lwIP platform** for Keel: a matched pair of a `KlSocketProvider`
(over the lwIP BSD socket API) and a `KlEventProvider` (over `lwip_poll`), so Keel
runs on an lwIP TCP/IP stack with no kernel sockets. lwIP is **not vendored** —
build against your own lwIP (`LWIP_DIR`) + `lwipopts.h`.

- `socket_lwip.c` — `KlSocketProvider` mapping `KlSocketOps` → `lwip_*`.
- `event_lwip.c` — `KlEventProvider` over `lwip_poll` (the runtime event-backend
  seam; its `native_provider()` returns the lwIP socket provider, so installing
  `event_provider` auto-wires the matched `sockets`).
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

`lwip_loopback_test.c` is the proof — a real Keel HTTP server on the lwIP
providers answering `200 OK` to an lwIP client over loopback, linked against a
**stock** `libkeel`:

```sh
make -C ../..                       # stock libkeel.a (any backend)
make loopback LWIP_DIR=/path/to/lwip
# -> keel: listening on 127.0.0.1:8080
#    lwIP loopback: Keel server on lwIP replied 200 OK (correct)
```

## Tested versions

| lwIP | Status |
|------|--------|
| STABLE-2.2.0 | **Loopback runtime verified** (200 OK on stock libkeel) + build-gate |
| 2.1.x | Expected to work (same `lwip_poll` + BSD socket API) |

## Scope

lwIP is **readiness-only** (its sockets layer is `lwip_poll`); the lwIP *raw*
callback API (completion model) is out of scope. See
`docs/lwip_platform_design.md` for the full platform-port shape + promotion path.
