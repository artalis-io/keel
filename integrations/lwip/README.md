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

## The runtime boundary (important)

A *running* Keel server on lwIP needs more than these two runtime providers,
because part of the socket layer is **compile-time, not runtime-swappable**:

- **`struct sockaddr` layout is compile-time.** lwIP uses the BSD layout (with
  `sin_len`); Linux does not. A host-built `libkeel.a` fills a host-layout
  `sockaddr` and hands it to `lwip_bind`, which misreads it (`bind: EIO`). The
  address types are baked in at compile time via `keel/net.h`.
- **Address resolution is compile-time.** `server.c` calls `getaddrinfo`; on lwIP
  that must be `lwip_getaddrinfo`.
- **The stop/wakeup fd is a host pipe** that `lwip_poll` cannot watch; on lwIP it
  must be a loopback socket (`platform_lwip`).

So the **event backend is cleanly runtime-injectable** (`KlEventProvider`, done),
but the **socket/address ABI is not** — it requires compiling Keel with
`-DKEEL_PLATFORM_LWIP` (`keel/net.h` → `lwip/sockets.h`) across the library, i.e.
a `PLATFORM=lwip` build (see `docs/lwip_platform_design.md` §2/§7). lwIP is
therefore a **hybrid**: a runtime event provider + a compile-time socket ABI, not
a pure drop-in `.a` like the mbedTLS/nghttp2 vtable integrations.

`lwip_loopback_test.c` is the runtime proof (a real Keel server + client over
lwIP loopback); it runs only against a `KEEL_PLATFORM_LWIP` build of `libkeel`.
lwIP loopback itself is confirmed working headless (`tcpip_init` + `LWIP_HAVE_LOOPIF`).

## Tested versions

| lwIP | Status |
|------|--------|
| STABLE-2.2.0 | Build-gate verified (providers compile; loopback confirmed) |
| 2.1.x | Expected to work (same `lwip_poll` + BSD socket API) |

## Scope

lwIP is **readiness-only** (its sockets layer is `lwip_poll`); the lwIP *raw*
callback API (completion model) is out of scope. See
`docs/lwip_platform_design.md` for the full platform-port shape + promotion path.
