# lwIP-sockets platform port: Design

Status: **designed (2026-07-26).** Positioned as the second branch of PAL Phase 6
(the table's "first non-POSIX provider: Winsock **or** lwIP sockets"; Winsock
shipped, this is the other half). **Scope of the immediate deliverable: an opt-in,
non-vendored BYO reference** that validates the now-public Phase 4 provider API
against a genuinely different readiness stack, *not* a committed/default platform
port. Promotion to a first-class platform is a separate later decision (§7).

## 1. Positioning: platform axis, not the BYO-library axis

lwIP is a **platform you deploy Keel on top of**, same axis as POSIX and
Windows/Winsock, not a bring-your-own composable *library* like mbedTLS/miniz.
So the relevant question isn't "does this violate BYO?" (it doesn't; BYO governs
layered libraries; the PAL governs platform ports) but "how much platform does
Keel commit to?"

Two things follow:
- **lwIP is never vendored.** On an embedded target the stack comes from the
  device firmware/SDK; Keel links it via the cross-toolchain/sysroot, exactly as
  Windows provides `ws2_32` and POSIX provides libc. The reference build points at
  a user-supplied lwIP (`LWIP_DIR`) + the user's `lwipopts.h`.
- **The reference is opt-in and commits Keel to nothing**: no `src/` default TU,
  no CI, no maintenance guarantee. It is a faithful *subset* of the full port
  shape (§2), so it can be promoted later without rework.

The public `KlSocketProvider` vtable (`keel/socket.h`) + `KlEventLoop`
(`keel/event.h`) are the only surfaces the reference uses; that is precisely what
this exercise validates.

## 2. Full platform-port shape (the reference is a faithful subset)

A complete lwIP-sockets port is three per-platform TUs (mirroring
`socket_posix.c`/`event_poll.c`/`platform_posix.c`), selected by a Makefile
platform branch; never `#ifdef` in a shared TU.

### 2.1 Handle model: already covered
lwIP socket descriptors are small `int` indices (optionally shifted by
`LWIP_SOCKET_OFFSET` to avoid colliding with system fds); invalid is `-1`. They
fit `KlSocketHandle` (`intptr_t`) with `KL_INVALID_SOCKET == -1` and
`kl_handle_valid()`; no handle work needed (Phase 5 already made this
pointer-width for exactly such stacks).

### 2.2 `socket_lwip.c`: the `KlSocketProvider`
Maps `KlSocketOps` → the `lwip_*` BSD-compatible socket API (requires `LWIP_SOCKET`):

| KlSocketOps | lwIP |
|---|---|
| socket/bind/listen/accept/connect/close | `lwip_socket`/`lwip_bind`/`lwip_listen`/`lwip_accept`/`lwip_connect`/`lwip_close` |
| send/recv/recv_peek | `lwip_send`/`lwip_recv` (`recv_peek` = `lwip_recv(..., MSG_PEEK)`) |
| writev | `lwip_writev` (if `LWIP_SOCKET` + writev enabled): translate `KlIoVec` → `struct iovec` **inside this TU** (same confinement as POSIX/Winsock); else leave NULL (no `WRITEV` cap → Keel serializes) |
| set_nonblocking | `lwip_fcntl(fd, F_SETFL, O_NONBLOCK)` or `lwip_ioctl(fd, FIONBIO, …)` |
| set_reuseaddr/ipv6only/tcp_nodelay | `lwip_setsockopt` (each gated on the lwipopts that enable it; best-effort → -1 otherwise) |
| get_local_addr / get_so_error | `lwip_getsockname` / `lwip_getsockopt(SO_ERROR)` (async-connect completion) |
| sendfile | **omit**: lwIP has no sendfile; no `SENDFILE` cap → Keel pread-sends |
| set_cork / set_nosigpipe | no-op (lwIP has neither) |

Capabilities: `KL_SOCK_CAP_NATIVE_FD` (pollable by the paired lwIP event backend,
see §4) `| KL_SOCK_CAP_WRITEV` when `lwip_writev` is available. Offsets stay
`uint64_t`, sizes `kl_ssize_t`; the public API already forbids `ssize_t`/`off_t`.

### 2.3 `event_lwip.c`: the readiness backend
lwIP 2.1+ ships `lwip_poll` (BSD `poll` semantics, `LWIP_SOCKET_POLL`); so this
backend is **nearly a copy of `event_poll.c`** with `poll → lwip_poll` and the
`struct pollfd`/`fd_set` types from lwIP. On older lwIP, fall back to a
`lwip_select`-based backend (fd_set ↔ `KlEventLoop`). Implements the public
`kl_event_*` contract.

### 2.4 `platform_lwip.c`: platform services
- `kl_monotonic_ms` → `sys_now()` (lwIP's millisecond clock).
- entropy → **target-provided**: lwIP has no CSPRNG; the port takes it from the
  device (hardware RNG / SDK): a documented port requirement, not Keel's to ship.
- thread-pool wakeup → the loopback-socketpair trick over lwIP's loopback netif
  (like `platform_win.c`'s loopback pair), or omit the thread pool in a
  single-loop (`NO_SYS`) configuration.

### 2.5 Build model
A Makefile platform branch (`PLATFORM=lwip`, sibling to the `OS=windows` branch)
selects `socket_lwip.c` + `event_lwip.c` + `platform_lwip.c` and takes lwIP
headers/libs from `LWIP_DIR`/the sysroot; **not vendored**. Threading model:
Keel's single-threaded event loop pairs naturally with lwIP's sequential
(`NO_SYS=0`, `tcpip_thread`) sockets API; `NO_SYS=1` bare-metal is a later
variant.

## 3. Immediate deliverable: opt-in BYO reference (no vendoring)

- **Location:** `examples/lwip/` (self-contained; clearly not a shipped platform):
  `socket_lwip.c` (the reference provider), `event_lwip.c` (reference `lwip_poll`
  backend), a `demo.c` (server + client over lwIP loopback), a sample
  `lwipopts.h`, and a `README.md`.
- **Build:** opt-in only: `make -C examples/lwip LWIP_DIR=/path/to/lwip`. lwIP and
  `lwipopts.h` are user-supplied; nothing vendored; not wired into the default
  build, `make examples`, or CI.
- **Public-API-only:** the reference includes exactly `<keel/socket.h>`,
  `<keel/event.h>`, `<keel/server.h>`, `<keel/client.h>`: no internal headers. If
  it needs something internal to work, that's a Phase 4 API gap to fix (the point
  of the exercise).
- **Runtime test without hardware:** build lwIP with a **loopback netif**
  (`LWIP_HAVE_LOOPIF`) and run the demo's server + client over `127.0.0.1` in one
  process on a dev host; no tap/root/QEMU needed. QEMU with a networked target is
  an optional second rung for a real cross-build.
- **What it proves:** the frozen public `KlSocketProvider` + `KlEventLoop`
  contracts are sufficient to run Keel's server and client on a non-POSIX,
  non-Winsock readiness stack: the first *external* validation of the Phase 4 API.
- **What it is NOT:** not vendored, not a default/shipped platform, not in CI, no
  maintenance guarantee.

## 4. Key decisions / the one real seam

**`NATIVE_FD` couples the socket provider to the event backend.** The flag means
"pollable by *this build's* event loop." For POSIX/Winsock the loop polls OS
descriptors; for lwIP the loop is `event_lwip` (`lwip_poll`) and the "fd" is an
lwIP socket index. The reference sidesteps the mismatch by shipping a **matched
set** (socket_lwip + event_lwip built together). Generalizing this, letting the
socket provider and event backend *negotiate* what "native/pollable" means, is
exactly **Phase 7 (event/socket capability negotiation)**, and this port is the
concrete motivation for it. (The PAL-review F3 invariant, `event.h` and the
socket seam stay decoupled, is what keeps Phase 7 addable without disturbing
either.)

Other decisions: no `sendfile`/`cork`/SIGPIPE (capability-gated / no-op); nonblock
via `lwip_fcntl`/`FIONBIO`; async-connect completion via `getsockopt(SO_ERROR)`;
`lwipopts.h` is the port's config surface (owner enables `LWIP_SOCKET`,
`LWIP_SOCKET_POLL`, `SO_REUSE`, `LWIP_NETIF_LOOPBACK`, etc.).

## 5. Risks
- **lwIP config surface** (`lwipopts.h`) is large and per-target; the reference
  ships a *sample* opts, not a blessed one.
- **Readiness only**: this is the lwIP *socket* API (readiness). The lwIP *raw*
  callback API (completion model) is Phase 9, gated on the Phase 7 event axis;
  out of scope here.
- **No CSPRNG in lwIP**: entropy is a documented target requirement.
- **Coupling (§4)**: until Phase 7, provider+backend must be a matched pair.

## 6. Verification
- Reference compiles against a user lwIP under `-Werror` (build-gate: proves the
  `KlSocketOps`/`KlEventLoop` mapping is complete using only public headers).
- Demo runs a real server+client HTTP round-trip over lwIP's loopback netif on a
  dev host.
- Cross-check: no internal Keel header is needed → the public Phase 4 API is
  sufficient. Any gap found becomes an additive Phase 4 follow-up.

## 7. Out of scope / promotion path
Out of scope: vendoring lwIP; a default/CI platform build; the lwIP raw provider
(Phase 9); UEFI (Phase 10); Phase 7 itself. **Promotion path:** if lwIP proves a
target Keel wants to *officially support*, promote the reference TUs to
`src/socket_lwip.c` + `src/event_lwip.c` + `src/platform_lwip.c` under a
`PLATFORM=lwip` Makefile branch, add a host-lwIP CI lane, and do Phase 7 to make
the provider/backend pairing a negotiated capability rather than a matched-set
convention. Until then it stays an opt-in reference: validating the API, costing
no maintenance.
