# Keel on lwIP — reference provider (opt-in, bring-your-own)

A **reference** that runs Keel's socket layer on the [lwIP](https://savannah.nongnu.org/projects/lwip/)
TCP/IP stack, via Keel's public provider API. It exists to **validate that the
public API is sufficient** to port Keel to a non-POSIX, non-Winsock platform — and
as a starting point if you want to deploy Keel on an lwIP target.

**lwIP is a platform Keel runs on top of (like POSIX or Windows/Winsock), not a
bring-your-own library.** As with those platforms, Keel does not vendor the stack:
lwIP comes from your target's SDK/firmware (or a checkout). Nothing here is built
by the default Keel build or CI. See `docs/archive/designs/lwip_platform_design.md` for the design.

## What's here

| File | Role |
|------|------|
| `socket_lwip.c` | `KlSocketProvider` over the lwIP socket API (`lwip_socket`/`lwip_send`/`lwip_writev`/…). `kl_socket_provider_lwip()`. |
| `event_lwip.c`  | `KlEventLoop` backend over `lwip_poll` (lwIP 2.1+). The readiness half — pair it with the provider so both agree what a pollable fd is. |
| `lwipopts.h`    | **Sample** host/loopback lwIP config (not production). Your target supplies its own. |
| `Makefile`      | Opt-in compile-gate against your `LWIP_DIR`. |

Both TUs use **only** the public headers `<keel/socket.h>` / `<keel/event.h>` and
Keel-owned types (`KlSocketHandle`, `KlIoVec`, `kl_ssize_t`) — no internal Keel or
host-POSIX types. `struct iovec`/`struct sockaddr` are lwIP's own (from
`lwip/sockets.h`, selected by `net.h`'s `KEEL_PLATFORM_LWIP` branch), translated
from `KlIoVec` inside `socket_lwip.c`.

## Validate (compile-gate)

```sh
make check LWIP_DIR=/path/to/lwip
```

A clean compile is the validation: the public provider/event API is sufficient to
author an lwIP platform. This is what the reference proves; it needs no hardware,
no running stack — just lwIP's headers.

## Running it (promotion to a platform build)

To actually serve requests over lwIP you build **libkeel itself** with these two
files as the platform's socket + event backend (replacing `socket_posix.c`/
`event_*.c`), then run under a full lwIP setup: `tcpip_init()`, a netif (a
loopback netif — `LWIP_NETIF_LOOPBACK` — lets `127.0.0.1` work on a dev host with
no hardware), and the sys/arch port (`contrib/ports/unix` on a host, or your
target's port). Server/client code is then identical to any other platform —
select the provider via `KlHttpServerConfig.sockets = kl_socket_provider_lwip()`. That build
integration is the platform-port **promotion path** (a `PLATFORM=lwip` Makefile
branch), deliberately out of scope for this opt-in reference — see
`docs/archive/designs/lwip_platform_design.md` §7.

## Notes / limitations

- **Readiness only** — this is the lwIP *socket* API. The lwIP *raw* callback API
  (completion model) is a separate event axis (roadmap Phase 9).
- **No `sendfile`/`cork`** — capability-gated off (Keel falls back to pread-send);
  no SIGPIPE on lwIP.
- **`NATIVE_FD` couples provider ↔ event backend** — both must agree "pollable"
  means "by lwip_poll." Here they're a matched set; generalizing that is roadmap
  Phase 7 (event/socket capability negotiation).
- **Config surface** — `lwipopts.h` is yours to own; the sample enables
  `LWIP_SOCKET`, `LWIP_SOCKET_POLL`, `LWIP_COMPAT_SOCKETS=0`, `LWIP_NETIF_LOOPBACK`.
