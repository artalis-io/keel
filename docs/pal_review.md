# PAL Architecture Review — POSIX/Winsock

**Date:** 2026-07-26
**Scope:** the Platform Abstraction Layer as it stands after PAL Phases 1–6 (the
socket seam, portable handle, per-OS platform services, event backends, and the
Winsock port) plus the mbedTLS backend.
**Goal of the review:** confirm loose coupling / tight internal coherence —
platform specifics confined to a thin PAL, no Windows/POSIX concepts (FDs,
syscalls, headers) percolating into the abstract Keel layers or vice-versa — and
assess readiness for future providers (e.g. lwIP).

## Verdict

The PAL holds to the intended architecture. Platform code is quarantined in thin
per-OS translation units behind narrow seams; the abstract/core layers are
`#ifdef`-free and handle-agnostic. It is positioned for additional descriptor-
based providers today (lwIP socket API), with one deliberately-deferred extension
(a completion-model event axis) needed for callback stacks (lwIP raw, IOCP).

## Evidence (measured)

| Check | Result |
|---|---|
| TUs carrying `#ifdef _WIN32/__linux__/__APPLE__` | **5**, all PAL TUs (`*_posix.c` siblings + `sockcompat.h`). **Zero** in any core/abstract TU. |
| Raw socket syscalls outside PAL TUs | **None** — all I/O flows through the `kl_sock_*`/`kl_sockdef_*` seam. |
| Public headers pulling platform network headers | **One** — `include/keel/net.h`, the documented single boundary. |
| FD-value assumptions (`fd < 0`) in abstract layers | Eliminated (F1) — all validity via `kl_handle_valid()`. |

## The thin PAL (seam inventory)

Each seam is an abstract header + Makefile-selected per-OS implementation TU:

- `handle.h` — `KlSocketHandle` (`intptr_t`, pointer-width) + `KL_INVALID_SOCKET`
  + `kl_handle_valid()`. Pointer-width so one type holds a POSIX `int`, a Winsock
  `SOCKET`, or a future pointer handle (lwIP raw `tcp_pcb *`, UEFI protocol ptr).
- `socket.h` + `socket_posix.c` / `socket_winsock.c` — the `KlSocketProvider`
  vtable (immutable `KlSocketOps` + `void *ctx` + capability bits) and the
  `kl_sockdef_*` NULL-provider defaults. Ops are `(ctx, KlSocketHandle, …)`; the
  interface carries no FD-value semantics.
- `platform.h` + `platform_posix.c` / `platform_win.c` — non-socket services
  (monotonic clock, entropy, thread-pool wakeup, `poll1`, file `pread`).
- `event.h` + `event_{epoll,kqueue,poll,iouring,wsapoll}.c` — readiness event
  loop, one backend per build.
- `server_plat.h` + `server_plat_{posix,win}.c`, `dns_sys.h` +
  `dns_sys_{posix,win}.c`, `udp_io.h` + `udp_io_{posix,win}.c` — server-bind /
  AF_UNIX, DNS config discovery, datagram cmsg I/O.
- `net.h` (public) / `sockcompat.h` (internal) — the only two places platform
  network headers are resolved (winsock2-before-windows ordering owned here).

Core TUs (`server.c`, `connection.c`, `client.c`, `udp.c`, `dns_resolver.c`,
`response.c`, `router.c`, `h2*`, `websocket*`) consume only these seams.

## Strengths

1. **Capability-gated vtable** (`KL_SOCK_CAP_NATIVE_FD/WRITEV/SENDFILE`) — a
   minimal provider advertises what it lacks; the framework falls back
   (serialize-vs-writev, pread-send-vs-sendfile). The mechanism a constrained
   lwIP/UEFI provider needs.
2. **Pointer-width handle done once** — the API-breaking `int`→`intptr_t` change
   already landed, so a pointer-handle provider won't force a second break.
3. **Single boundary headers** — no scattered `<sys/socket.h>`; one public
   (`net.h`) + one internal (`sockcompat.h`).
4. **No `#ifdef` in shared TUs** — a new platform adds a sibling TU + a Makefile
   arm, never conditionals threaded through core code.

## Findings & dispositions

- **F1 — `async.c` used `fd < 0` on a `KlSocketHandle`. FIXED.** The
  `KlWatcher`/`KlEventCtx` layer now validates with `!kl_handle_valid(fd)` — the
  Windows-safe idiom (a Winsock `SOCKET` is unsigned; `< 0` is unreliable).
  Behavior-preserving on POSIX.
- **F2 — POSIX-only calls in the public `server.h` now sit behind a capability
  query. FIXED.** Added `KlPlatformCap` (`PEER_CRED`, `PEER_CRED_PID`,
  `SYSTEMD_ACTIVATION`) + `kl_platform_caps()`, resolved in the platform slice
  (`server_plat_posix.c`: Linux full, macOS peer-cred+pid, other BSD peer-cred
  only; `server_plat_win.c`: none). Applications gate the peer-cred / systemd
  helpers on the relevant bit instead of inferring support from a `-1` return —
  making the portability contract explicit rather than platform assumptions
  leaking into application logic.
- **F3 — keep the event axis and the socket seam decoupled. PRESERVED (invariant).**
  `event.h` depends only on `handle.h` (`KlSocketHandle`), never on `socket.h`;
  `socket.h` never includes `event.h`. This decoupling is deliberate and must be
  maintained: it lets a **completion-model event axis** (lwIP raw callbacks around
  `sys_check_timeouts()`, or Windows IOCP) be introduced as an *alternative* event
  backend without disturbing the readiness path or the socket provider. Today's
  readiness seam already accepts a descriptor-based provider (lwIP **socket** API +
  its `poll` shim) with no core change; the completion model is the one scoped
  future extension (see `pal_transformation_design.md`, "event-axis").
- **F4 — `struct sockaddr` is baked into `KlSocketOps`. DEFERRED (revisit later).**
  Fine for POSIX/Winsock/lwIP-socket; a non-BSD address world (UEFI, some raw
  stacks) would want an address abstraction. Consistent with "add address types
  only when a real provider needs them." No action now.
- **F5 — the POSIX datagram TU carries within-POSIX-family conditionals. ACCEPTED.**
  Now `socket_dgram_posix.c` (the datagram data-plane folded onto `KlSocketProvider`;
  the old `udp_io_posix.c` seam was deleted in the A2 refactor). All are
  `__linux__`/`__APPLE__`/feature-macro (`IP_PKTINFO`/`UDP_GRO`/`UDP_SEGMENT`), i.e.
  the POSIX TU owning its own dialects — not cross-platform leakage. POSIX dialects
  living in the POSIX PAL TU is by design.

## lwIP readiness (summary)

- **Handle:** ready — `KlSocketHandle` stores a `tcp_pcb *` (pointer-width).
- **Socket provider:** ready — the capability-gated `KlSocketProvider` accepts a
  minimal `lwip_*` provider; readiness backend via lwIP's `poll`/`select` shim.
- **Raw API / completion model:** deferred — needs the F3 alternative event axis,
  kept unblocked by the event/socket decoupling. This is the one true extensibility
  boundary and it is known, not accidental.

## Bottom line

Loose external coupling, tight internal coherence: platform code is confined to
thin per-OS TUs behind narrow seams, the abstract layers are `#ifdef`-free and
FD-agnostic, and the handle + capability-vtable design already anticipates further
providers. F1/F2 are fixed; F3 is a preserved invariant; F4/F5 are understood and
accepted.
