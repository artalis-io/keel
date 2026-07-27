# Keel Platform Abstraction — Incremental Transformation Plan

Status: **design / roadmap (2026-07-19).** No code changes in this document.
Scope: a staged plan to let Keel select an **event engine** and a **socket /
network-stack provider** independently, without a big-bang PAL rewrite. Each
phase is a small, tested, reviewable change that leaves Keel buildable, keeps
public behavior, and opens a clean seam for the next phase.

**This is not a PAL rewrite.** The abstractions below are extracted from real
existing code (cited by `file:line`) and near-term platform needs, not from a
theoretical universal platform layer. The recommended first implementation step
is Phase 1 (an internal socket-ops seam) — and only if it demonstrably improves
the code.

---

## 1. Current-state architecture review

Keel is a single-threaded, event-loop C11 transport substrate. Two axes already
exist in the codebase but are entangled with POSIX assumptions:

### 1.1 Event engine — already a compile-time seam (good)

`include/keel/event.h` defines one narrow, readiness-based interface:
`kl_event_init / add / mod / del / wait / close`, over `KlEventLoop` and a
`KlEvent { void *udata; KlEventMask ready; }`. Four backends implement it, one
selected at build time via `BACKEND=` in the Makefile:

| Backend | File | Model |
|---|---|---|
| epoll (Linux default) | `src/event_epoll.c` (1.7 KB) | readiness / level |
| kqueue (macOS default) | `src/event_kqueue.c` (2.9 KB) | readiness |
| io_uring (opt-in) | `src/event_iouring.c` (7.9 KB) | completion, **adapted** to the readiness interface (`loop.fd = -1`) |
| poll (universal fallback) | `src/event_poll.c` (6.6 KB) | readiness / level |

**Assessment:** this is already a clean, cache-friendly, compile-time-specialized
abstraction with no per-op dispatch. It is *readiness-shaped*, and io_uring is
already shoe-horned into it. This is the axis that a future **IOCP** (completion)
backend will stress — see Phase 7/8. **Do not redesign it until a concrete
completion backend exposes a real limitation.**

`KlEventCtx` (`event_ctx.h`) composes a loop + allocator + watcher list and adds
the generic-FD `KlWatcher` layer (tagged-pointer dispatch) used by all clients,
timers, thread-pool wakeups, UDP, and DNS. This is the real "event surface" the
rest of the code sits on.

### 1.2 Socket operations — scattered raw POSIX calls (the actual work)

There is **no socket seam today.** Raw syscalls are spread across modules:

| File | socket-family syscalls | notes |
|---|---|---|
| `src/udp.c` | 34 | largest surface: `sendmsg/recvmsg/recvmmsg/sendmmsg`, `IP_PKTINFO`, GSO/GRO, TOS |
| `src/client.c` | 23 (+41 I/O) | `socket/connect` + nonblock/NOSIGPIPE setup **duplicated 4×** |
| `src/server.c` | 17 | `socket/bind/listen/accept`, TCP + `AF_UNIX`, `SO_REUSEPORT`, peer creds |
| `src/h2_client.c` | 9 | own connect + nonblock/NOSIGPIPE (dup 2×) |
| `src/websocket_client.c` | 9 | own connect + nonblock/NOSIGPIPE |
| `src/connection.c` | 4 | accept-side fd handling |
| `src/dns_resolver.c` | 3 | TCP fallback connect (added this cycle) |
| `src/client_pool.c`, `h2.c`, `response.c` | 2–4 | misc |

**The single strongest finding:** the *nonblocking + close-on-exec +
`SO_NOSIGPIPE`/`MSG_NOSIGNAL` + connect* idiom is copy-pasted at least **8
times**: `client.c:106/198/1370/1472`, `h2_client.c:496/533`,
`websocket_client.c:967`, `dns_resolver.c:720`, plus the server variants
`server.c:59` (`set_nonblocking`) and `server.c:66` (`set_cloexec`). Same
`#ifdef SO_NOSIGPIPE / #else MSG_NOSIGNAL` branch appears in every client
transport. This duplication *is* the abstraction leak, and consolidating it is
Phase 1.

### 1.3 Transport I/O — partial seams already exist (good models)

- `src/internal.h`: `conn_read / conn_write / conn_write_all` — TLS-aware
  (`if (c->tls) c->tls->read(...) else read(fd)`). Server-side reads/writes all
  go through these.
- `src/client.c:48/61`: `io_write / io_read` — the same `(fd, KlTls*)` shape for
  the client side. The DNS TCP fallback just added `dns_tcp_write/read` in the
  same shape (`dns_resolver.c`).

These prove the pattern the socket provider should follow: **a thin
branch-or-dispatch keyed on a provider pointer, inlined on the hot path.** They
are currently duplicated per module rather than centralized.

### 1.4 Already-pluggable provider vtables (mirror these conventions)

Keel already ships bring-your-own vtables with immutable ops + user context:
`KlTls` (`tls.h`), `KlResolver` (`resolver.h`), `KlRequestParser/KlResponseParser`
(`parser.h`), `KlCompress/KlDecompress`, `KlFileIO` (`file_io.h`), `KlAllocator`.
The socket provider (Phase 3+) should look and feel exactly like these:
`const KlXxxOps *ops; void *ctx;` with explicit lifetime and no global state.

### 1.5 Platform `#ifdef` inventory

~46 platform-conditional sites. Categories:
- **NOSIGPIPE dialect**: `SO_NOSIGPIPE` (BSD/macOS) vs `MSG_NOSIGNAL` (Linux) —
  every client transport.
- **Linux-only UDP offload**: `IP_PKTINFO`, `UDP_SEGMENT`/GSO, GRO, `recvmmsg`,
  `SO_REUSEPORT` in `udp.c` (with documented per-datagram fallbacks).
- **CLOEXEC portability**: macOS lacks `SOCK_CLOEXEC` on `socket()` →
  `server.c:66` uses `fcntl` (already commented).
- **`AF_UNIX`** local transport in `server.c` (bind node, mode, peer creds).

No `_WIN32` branches exist yet — Winsock is greenfield.

### 1.6 TLS / DNS boundaries

TLS is fully behind `KlTls` and always wraps transport via the `(fd, KlTls*)`
helpers — a socket provider slots *underneath* TLS cleanly. DNS is behind
`KlResolver`; the built-in resolver owns its own UDP/TCP sockets (would consume
the socket provider like any other module).

---

## 2. Platform / socket assumption map — 5-way classification

Per the task's required classification:

### (1) Already appropriately abstracted — leave alone
- Event engine (`event.h`, 4 backends, compile-time selected).
- TLS transport (`KlTls`), DNS (`KlResolver`), parser, compress, file_io,
  allocator — all vtable-pluggable.
- `conn_read/conn_write` server-side transport I/O seam.

### (2) Abstraction leaking but safe to defer
- Duplicated `io_read/io_write`, `dns_tcp_read/write`, per-client `send/recv`
  wrappers — same shape, not yet shared. Consolidatable but non-blocking.
- `SO_NOSIGPIPE`/`MSG_NOSIGNAL` `#ifdef` repeated per module.

### (3) Blocking future Winsock / lwIP / IOCP / UEFI — the real seam work
- **Scattered raw `socket/bind/listen/accept/connect/setsockopt/fcntl`** (§1.2).
  No central place to swap a stack. **This is Phase 1's target.**
- Nonblocking/CLOEXEC/NOSIGPIPE setup assumes POSIX fcntl + BSD sockets.
- `getaddrinfo` used directly (blocking) in server bind + as sync client path.

### (4) Likely to require public API evolution (defer past Phase 4)
- Public `int fd` everywhere: `KlConn.fd` (`connection.h:42`), `KlEvent.udata`,
  `KlResponse.conn_fd/file_fd`, `KlConfig.listen_fd`/`KlServer.listen_fd`
  (`server.h`), `KlUdp.fd` + `kl_udp_fd()`, `KlClientPoolConn.fd`,
  `KlWatcher.fd` + `kl_watcher_*`, `KlTls` vtable takes `int fd`, `KlFileIO`
  takes `int file_fd/sock_fd`, `kl_peer_cred_fd`, `kl_systemd_listen_fd*`.
  Winsock `SOCKET` is a pointer-width handle (fits `int` poorly on Win64); UEFI
  has no fd. These need portable-handle evolution **but only when a concrete
  non-POSIX provider lands** (Phase 5).

### (5) Performance-critical — do not generalize prematurely
- `udp.c` batch paths (`recvmmsg/sendmmsg`, GSO/GRO), server `writev`/`sendfile`
  response path (`response.c`), the `conn_read/conn_write` inline hot loop, and
  the io_uring submission path. Any seam here must stay inlineable / zero-alloc.

---

## 3. Staged roadmap (dependencies + risk)

| Phase | Goal | Depends on | Risk | Public API change? |
|---|---|---|---|---|
| **0** | Baseline & characterization | — | none | no |
| **1** | Internal socket-ops seam (POSIX only) | 0 | low | no |
| **2** | Mock provider + failure-injection tests | 1 | low | no |
| **3** | Explicit internal `KlSocketProvider` object | 1,2 | med | no (internal) |
| **4** | Public provider selection in `KlConfig` | 3 | med | additive |
| **5** | Portable addresses/handles (only what next provider needs) | 4 | high | evolutionary |
| **6** | First non-POSIX provider (Winsock **or** lwIP sockets) | 4,5 | high | additive |
| **7** | Event/socket capability negotiation | 6 | med | additive |
| **8** | IOCP completion backend | 6(Winsock),7 | very high | additive |
| **9** | lwIP raw provider | 6(lwIP),7 | very high | additive |
| **10** | UEFI feasibility + optional prototype | 5,7 | very high | additive/subset |

**Status:** Phase 0 done (`f29ed15`, baseline in Appendix A). **Phase 1 + 1b done**
— `src/socket.{h,c}` landed; client transports, DNS resolver, server, and udp
switched over (Appendix C). **Phase 2 done** (Appendix D + E) — the seam became a
`KlSocketProvider` vtable threaded through the client transports via
`KlEventCtx.sockets` (internal, opaque — NOT the public Phase 4 selection API),
with a fault-injection mock + conformance tests, and the ops table extended to
the full socket lifecycle. **Phase 3 done** (Appendix F) — provider semantics:
`destroy` lifecycle hook, capability query + native-fd escape hatch, and a
`kl_sock_errno_to_error` taxonomy (errno → stable `KlError`, native errno
preserved). Still internal — no public API. **Server adoption done**
(`docs/server_provider_adoption_design.md`, commits `29d8e33`+`aa27663`) — the
server's lifecycle, `conn_read`/`conn_write`, and the `writev`/`sendfile`
response fast paths now flow through the seam (capability-gated; POSIX
byte-identical, bench flat).

**Phase 4 (public selection) resequenced to run AFTER Phase 6 (Winsock
prototype)** — designed in `docs/phase4_public_provider_design.md`, implementation
gated. Rationale: the `int fd` vtable isn't yet proven by a real non-POSIX
provider, so publishing it now risks a public breaking change (Winsock `SOCKET`
handle width, `writev`/`sendfile` ops, accept/connect shape). Build Winsock
first, freeze the public vtable once. Finer public error codes: **deferred** —
keep the coarse `KlError` mapping.

**Phase 5 + 6 (portable handles + Winsock) designed** in
`docs/phase6_winsock_design.md`. Decisions: **Phase 5 first** — a `KlSocketHandle`
type, **`intptr_t` (pointer-width) on every platform**, across the socket-facing
API, so the vtable is correct on Win64 before any Winsock code and future
pointer-handle stacks (lwIP raw `tcp_pcb *`, UEFI protocol pointers) fit without a
second break. Behaviorally a no-op on POSIX (source-compatible; ABI widens
4→8 bytes, fine under static linking). Descriptor-based providers — POSIX,
Winsock, **lwIP socket API** — share the `-1` invalid sentinel and the readiness
model; raw APIs (lwIP raw, IOCP) are a separate event-axis concern.
`writev`/`sendfile` become real vtable ops (POSIX + Winsock `WSASend`/
`TransmitFile`). **MinGW-w64 + a `windows-latest` CI runner** (WSAPoll backend via
`#ifdef` in `event_poll.c`; link `ws2_32`/`bcrypt`). **Fuller port**, staged:
5 (POSIX no-op) → 6a (provider + WSAPoll + build + clock/entropy shims, core
tests green) → 6b (UDP, thread pool, breadth). 6a is the point where the vtable is
validated and Phase 4 unblocks.

**Phase 4 done** — public `KlConfig.sockets`/`KlClientConfig.sockets` selection
landed (`include/keel/socket.h`), no internal/POSIX types leaked, validated by the
**lwIP BYO reference** (`examples/lwip/`, `docs/lwip_platform_design.md`) which
proves the public provider/event API is sufficient for a third platform. mbedTLS
backend builds on POSIX + Windows (seam-routed, BYO/out-of-CI).

**Phase 7 (event/socket capability negotiation) done** — designed in
`docs/phase7_capability_negotiation_design.md`, implemented as specified.
**Formalize the contract only:** the event backend gets an internal capability
surface (`src/event_caps.h`, `KL_EVENT_CAP_READINESS | _NATIVE_FD`, `COMPLETION`
reserved/unimplemented; each of the 5 backends returns a one-line const set), and
the server's implicit native-fd assumption became a two-sided event↔socket
negotiation (`kl_event_ctx_sockets_compatible()` in async.c, at the `KlEventCtx`
wire-up — F3-safe: `event_caps.h` pulls only `<keel/event.h>`, never the socket
seam). The server guard moved from a pre-alloc provider-only check to the truthful
two-sided check after loop+provider are wired; the async client rejects an
incoherent pairing at both start paths. **Internal-first** — no public
`keel/event.h` change; the public event-capability API is frozen only when a real
consumer (Phase 8 IOCP / Phase 9 lwIP-raw) exercises it. Behaviorally a no-op today
(readiness+native-fd is one point in the new space); the guard is now truthful
rather than assumed. `test_event_caps` proves it rejects a non-native provider
(POSIX + Windows CI).

**Phase 8 (IOCP completion backend) designed** in `docs/phase8_iocp_design.md`.
Decisions: a **true completion axis** (IOCP owns overlapped buffers; the provider
posts `WSARecv`/`WSASend`; completions carry the data — no `\Device\Afd` readiness
flattening), staged as a **foundational subset (8a)**: the IOCP backend + selection
+ the plaintext TCP HTTP path (accept/read/write/close/keep-alive), with TLS/UDP/
streaming/file-I/O completion deferred to 8b. The overriding constraint is
**orthogonality**: the completion model is contained to `event_iocp.c` + an internal
I/O-engine seam (`io_engine.h`) + a completion connection driver, and must not
percolate into orthogonal concepts (the model-blind protocol core is shared
verbatim), into other platforms (IOCP objects live only in the Windows/`BACKEND=iocp`
Makefile branch — no `#ifdef` in shared code), or into **Keel's public API** (zero
`include/keel/*.h` change; `KL_EVENT_CAP_COMPLETION`/`KL_SOCK_CAP_OVERLAPPED` are
internal). The Phase 7 negotiation gains its completion arm (COMPLETION ⋄
OVERLAPPED). Grep-assertable litmus tests enforce the containment; the crux/stop
condition is whether the transport/protocol-core split holds without the completion
concept leaking into the shared core.

**Phase 8a is landing incrementally** (IOCP is Windows-runtime-only — the completion
path is validated by a Windows CI oracle, not the dev host). *Increment 1 — the
completion-model negotiation — done:* an internal `KL_SOCK_CAP_OVERLAPPED` bit
(reserved from the public cap space, kept out of `<keel/socket.h>`), and the Phase 7
negotiation generalized into a pure `kl_caps_compatible(ev_caps, provider)` with a
completion arm (a `KL_EVENT_CAP_COMPLETION` loop requires an `OVERLAPPED` provider;
the readiness arm is unchanged). Unit-tested on POSIX across the full matrix
(`test_event_caps`) even without an IOCP backend. *Increment 2 — the IOCP event
backend + overlapped provider + build/CI wiring — done:* `src/event_iocp.c`
(Windows/`BACKEND=iocp` only) implements the `KlEventLoop` lifecycle over an IOCP
port and advertises `KL_EVENT_CAP_COMPLETION | _NATIVE_FD`; `kl_socket_provider_iocp()`
is the overlapped provider (Winsock control-plane defaults + the `OVERLAPPED` cap).
Selected in the Makefile (no `#ifdef` in shared code); a **Windows (IOCP)** CI job
link-gates the build and boots a real IOCP loop (`test_iocp_engine`, Windows-only).
The readiness `kl_event_wait` is a documented no-op on this loop. *Increment 3 —
the `io_engine` dispatch seam — done:* `src/io_engine.h` declares
`kl_io_engine_run_completion()`; the server run loop detects a completion loop once
(via `kl_event_caps`) and delegates the tick to it, leaving the readiness
wait/dispatch path byte-identical (the branch is never taken on readiness backends).
The symbol is defined per-backend with no `#ifdef` in shared code — a stub in
`src/io_engine.c` (linked on every non-IOCP build, never called) and the real tick
in `event_iocp.c` (a documented placeholder until the driver lands). *Increment 4a —
the model-blind protocol-core exposure — done:* `src/conn_internal.h` +
`kl_conn_dispatch_request` / `kl_conn_run_post_body` / `kl_conn_send_complete` —
thin non-static handles onto connection.c's existing static core, so the completion
driver reuses the exact parse→route→handle→lifecycle path after a completed
`WSARecv` without the readiness transport wrapper and without connection.c learning
the event model. `kl_conn_on_readable` is unchanged (byte-identical, POSIX-tested).
*Increment 4b — the completion connection driver — done:* `event_iocp.c` gains the
full driver — `AcceptEx` (socket pre-create + `GetAcceptExSockaddrs` +
`SO_UPDATE_ACCEPT_CONTEXT`) → `WSARecv` → the model-blind core
(`kl_conn_dispatch_request`) → response serialized via `kl_response_build_iovec`
(extracted from `kl_response_send`, shared) and posted with `WSASend` (partial-send
tracked) → keep-alive re-post / close — all off `GetQueuedCompletionStatusEx`, per-op
`OVERLAPPED` recovered by `CONTAINING_RECORD`. `tests/smoke_iocp.c` drives an
end-to-end HTTP-over-IOCP roundtrip (the server pinned to the completion loop +
overlapped provider, hit by the sync client) in the Windows-IOCP CI job — the first
runtime proof of the completion axis. **8a scope:** plaintext GET/HEAD (no request
body); request bodies, TLS, UDP, and streaming/file responses over IOCP remain 8b.
Orthogonality held throughout — no `include/keel/*.h` change; the completion model
lives only in `event_iocp.c` (Makefile-selected, no `#ifdef` in shared code); the
readiness path is byte-identical. **Phase 8a is complete** — the plaintext GET/HEAD
HTTP-over-IOCP foundational subset serves real requests on the Windows CI runner
(`smoke-iocp`).

**Phase 8b (completion-axis breadth) designed** in
`docs/phase8b_iocp_breadth_design.md`. Two governing constraints, both orthogonality
axes: (1) **non-invasive to surface public APIs** — request bodies, TLS, UDP, and
streaming/file responses get IOCP support with **no** change to `KlBodyReader` /
`KlTls` / `KlUdp` / `KlResponse` (a surface never learns the event model); and (2)
**completion is a platform-independent concept, not an IOCP detail** — completion is
an event *axis* (peer to readiness: abstract `event.h` + epoll/kqueue/… impls), so
the generic completion **driver logic** moves to a platform-independent
`completion.h` + `completion_driver.c`, and IOCP (`WSA*`/`OVERLAPPED`/`AcceptEx`/
`TransmitFile`/GQCS) becomes merely one *implementation* of that axis — reusable by
a future io_uring-completion / POSIX-AIO backend. Staged **8b-0** (extract the
platform-independent completion axis from 8a's driver — pure refactor) → **8b-1**
bodies → **8b-2** files (`TransmitFile`) → **8b-3** streaming → **8b-4** UDP →
**8b-5** TLS (buffered BIO; deepest, last). Grep-assertable litmus enforces both
axes; TLS is the crux/stop-condition.

Event-backend work (IOCP, UEFI events, RTOS loops) stays a **separate axis** from
socket providers and is not merged with it.

---

## 4. Recommended next three phases

**Phase 1 — internal socket-ops seam (implement first, POSIX-only).**
Centralize the duplicated socket setup/teardown behind a small internal
`src/socket.c` + `src/socket.h` (not public). One production provider: POSIX.
Zero behavior change; measured before/after. Details in §5.

**Phase 2 — mock provider + deterministic failure injection.**
Add a test-only provider that forces short reads/writes, `EWOULDBLOCK`, delayed
completion, `ECONNRESET`, UDP loss/reorder, and cancellation races. Use it to
harden server/client/UDP/cleanup paths. Value is independent of portability:
it directly improves robustness and pins down lifecycle/cancellation semantics.

**Phase 3 — explicit internal `KlSocketProvider`.**
Promote the seam to an immutable-ops + context object carried by the event ctx /
server / client config **internally**, with capability flags and an
`errno`-preserving error map. Still POSIX-only in production; still no public
provider API. This is the last phase before any public surface changes.

Stop and re-evaluate after each. Do **not** proceed to Phase 4 (public API) until
the mock provider (Phase 2) has proven the seam is real and complete.

---

## 5. Phase 1 concrete design (the smallest coherent subset)

**New internal files (not installed, not public):** `src/socket.h`, `src/socket.c`.

**Surface — exactly the operations Keel already performs**, named to mirror the
existing `kl_` style, all thin static wrappers over POSIX today:

```c
/* src/socket.h — INTERNAL. No ABI commitment. POSIX-backed in Phase 1. */
int     kl_sock_create(int family, int type, int proto);      /* + CLOEXEC */
int     kl_sock_set_nonblocking(int fd);
int     kl_sock_set_nosigpipe(int fd);                        /* SO_NOSIGPIPE / no-op */
int     kl_sock_bind(int fd, const struct sockaddr *, socklen_t);
int     kl_sock_listen(int fd, int backlog);
int     kl_sock_accept(int fd, struct sockaddr *, socklen_t *); /* + nonblock+cloexec on child */
int     kl_sock_connect(int fd, const struct sockaddr *, socklen_t);
ssize_t kl_sock_send(int fd, const void *, size_t);           /* MSG_NOSIGNAL where needed */
ssize_t kl_sock_recv(int fd, void *, size_t);
ssize_t kl_sock_sendto(int fd, const void *, size_t, const struct sockaddr *, socklen_t);
ssize_t kl_sock_recvfrom(int fd, void *, size_t, struct sockaddr *, socklen_t *);
int     kl_sock_shutdown(int fd, int how);
int     kl_sock_close(int fd);
int     kl_sock_setopt(int fd, int level, int optname, const void *, socklen_t);
int     kl_sock_getopt(int fd, int level, int optname, void *, socklen_t *);
```

Plus a single `kl_sock_connect_nonblocking(family, addr, len, int *out_fd)` that
absorbs the create+cloexec+nonblock+nosigpipe+connect idiom duplicated 8×.

**Constraints (Phase 1):**
- `static inline` in `socket.h` for the hot 1-liners (send/recv/close) so there
  is **zero call-overhead delta** vs today; only the multi-step setup helpers go
  in `socket.c`.
- Signatures still take/return `int fd` — no handle abstraction yet.
- No dispatch, no context object, no provider enum. One implementation.
- The `#ifdef SO_NOSIGPIPE/MSG_NOSIGNAL/SOCK_CLOEXEC` branches move **into
  `socket.c` once**, deleted from the 8 call sites.

**What Phase 1 explicitly does NOT touch:** the event backends, `conn_read/
conn_write` (they stay; internally they may later call `kl_sock_*`), `udp.c`'s
batch/offload paths (migrate opportunistically only where it's a clean 1:1),
public headers, `getaddrinfo`.

**Deliverables (when implemented):** `src/socket.{h,c}`; call sites in
`client.c`, `h2_client.c`, `websocket_client.c`, `dns_resolver.c`, `server.c`
switched over; all existing tests green on epoll/kqueue/io_uring/poll + macOS +
Linux ASan/UBSan + scan-build + cppcheck; `make bench` before/after with no
measurable regression.

**Stop after Phase 1 if** the consolidation does not clearly reduce duplication
or improve testability — the seam only earns its place if Phase 2's mock can
plug into it.

---

## 6. Cross-cutting notes (preserve, don't generalize prematurely)

**Event abstraction.** Keep readiness-shaped. When IOCP arrives (Phase 8), do not
flatten completion into readiness — introduce capability flags
(`readiness` vs `completion`) and let the high-level connection interface hide
the difference, contained to the backend rather than leaked into POSIX paths.

**UDP (already production).** Preserve connected/unconnected, v4/v6,
send-to/recv-from, event-loop integration, `IP_PKTINFO` dest addr, batch and
GSO/GRO paths, TOS/ECN. These are the QUIC groundwork; add per-provider
capability flags rather than forcing the least-capable stack. `udp.c` migrates to
`kl_sock_*` only where 1:1; the batch/offload syscalls stay Linux-specialized
behind capability checks.

**Unix-domain sockets (already production).** Keep first-class: path/abstract-
namespace semantics, peer credentials (`SO_PEERCRED`/`LOCAL_PEERPID`/`SO_PEERSEC`
in `server.c`), unlink-on-cleanup, local-vs-remote security distinction. Do **not**
dissolve into a generic socket type. A future local-transport abstraction (adding
Windows named pipes / AF_UNIX / in-memory channels) waits for a concrete second
local transport.

**Error model.** Phase 1 keeps `errno` internally. Portable `KlError`-style
categories (would-block, cancelled, timeout, conn-reset, unreachable, in-use,
unsupported, invalid-state, resource-limit, provider-failure) arrive with the
provider object (Phase 3), and **always** preserve the native code alongside.

**Broader PAL.** Do not build a monolithic `KlPlatformOps`. Introduce narrow
interfaces (monotonic clock, secure random, worker wakeup) only where the code is
already `#ifdef`-scattered and a second impl is genuinely needed. Prefer several
small interfaces over one giant one.

**HTTP/QUIC layering.** Preserve `HTTP/1.1 → llhttp → stream`, `HTTP/2 → session →
stream`, `HTTP/3 → QUIC → UDP`. llhttp stays HTTP/1.x-only. QUIC is **not** part
of this transformation; PAL choices only must not preclude a later QUIC provider
over the UDP seam.

**Security boundary.** Keel stays an event/transport substrate. Provider/endpoint
*facts* (provider identity, backend identity, endpoint type, local/remote, peer
address, TLS state, byte counts, lifecycle) may be exposed for Hull; capability
authorization, manifest enforcement, and audit policy stay in Hull.

---

## 7. Performance discipline (every phase)

Benchmark before/after (`make bench`, 4-endpoint wrk suite). No mandatory
per-operation heap allocation. Preserve `sendfile`/`writev`/vectored I/O and
backend fast paths. Keep vtables immutable and cache-friendly; specialize at init
time; permit compile-time-selected providers. Do not duplicate large
implementations solely to dodge one indirect call. Any measurable regression must
be explained or the abstraction deferred/removed.

## 8. Public API discipline

No abrupt replacement. Where APIs expose `int fd`, keep them as POSIX-convenience
APIs; add portable alternatives only when a non-POSIX provider needs them; mark
platform-specific APIs; deprecate only after replacements exist. The provider API
stays internal until proven by (1) POSIX provider, (2) mock provider, (3) one
real alternative provider or a detailed prototype.

## 9. Consciously deferred work

- Public provider selection API (`KlConfig.sockets`) — Phase 4, after mock proof.
- Portable handle/address types — Phase 5, only what the next real provider needs.
- Winsock, IOCP, lwIP (socket + raw), UEFI — Phases 6/8/9/10, each gated on a
  concrete target + test environment.
- Portable error taxonomy — Phase 3, additive.
- `getaddrinfo` abstraction / async name resolution unification — deferred; the
  `KlResolver` vtable already covers async DNS.
- Local-transport abstraction (named pipes etc.) — deferred to a real second
  local transport.
- QUIC/HTTP-3 — out of scope; only keep the UDP seam clean.

## 10. Stop conditions (report instead of continuing)

Stop and surface problem + options + tradeoffs + recommended smallest next step
when:
- a public API change appears necessary (before Phase 4);
- a performance regression can't be eliminated;
- the readiness event abstraction can't cleanly host a planned completion
  provider;
- the socket seam starts merely re-implementing the BSD socket API with no
  added value;
- a universal-handle design turns speculative;
- no second provider is available to validate an abstraction;
- tests expose unclear lifecycle/cancellation semantics;
- changes start spreading unrelated platform abstractions across the tree.

---

## Appendix A — Phase 0 baseline record (2026-07-19)

Established before any Phase 1 seam code, so a regression is measurable.

**Backends / tests.** `make test` (837 unit tests) green on all four event
backends and platforms via CI: Linux epoll, Linux io_uring, Linux poll-fallback,
macOS kqueue, musl/Alpine, Cosmopolitan (APE). `make debug-test` (ASan + UBSan)
clean — the one UBSan note (`redirect.c:77`, a `memcpy(NULL, NULL, 0)` on an
empty body) is pre-existing, non-fatal, and unrelated to PAL work.

**Benchmark baseline** (`make bench`, Apple M1 Max, macOS, **kqueue**, 4 threads /
100 connections / 10 s):

| Endpoint | Req/sec | Avg latency | p99 |
|---|---|---|---|
| `GET /hello` (baseline) | 101,159 | 0.98 ms | 1.21 ms |
| `GET /users/42` (route params) | 98,777 | 1.00 ms | 1.25 ms |
| `GET /mw/hello` (middleware chain) | 101,675 | — | — |
| `POST /echo` (body reading) | 97,158 | — | — |

Phase 1 must land within noise of these numbers (the seam is `static inline` on
the hot 1-liners, so no delta is expected). Re-run on Linux/epoll before/after
when Phase 1 is implemented.

**Frozen ABI surface — public `int fd` (do NOT change type/signature in Phases 1–4).**
Struct members: `KlConn.fd`, `KlUdp.fd`, `KlWatcher.fd`, `KlResponse.conn_fd` +
`.file_fd`, `KlConfig.listen_fd` + `KlServer.listen_fd`, `KlClientPoolConn.fd`
(+ pool-internal `fd`), `KlEventLoop.fd` (backend-internal). Functions:
`kl_conn_acquire`, `kl_watcher_add/mod/del/rearm`, `kl_event_add/mod/del`,
`kl_response_file`, `kl_peer_cred_fd`, `kl_systemd_listen_fd*`, `kl_udp_fd`,
`kl_udp_server_fd`, and the `KlWatcherFn` callback. Vtables: `KlTls`
handshake/read/write/shutdown, `KlFileIO` submit/cancel — all take `int fd`.
**Phase 1 touches none of these** (internal call-site consolidation only), so it
is ABI- and source-compatible by construction; portable-handle evolution is
deferred to Phase 5 and only for what a real non-POSIX provider needs.

**Test gaps to consider filling alongside Phase 1/2** (safety net around the seam):
UDP connected-mode, Unix-domain cleanup, watcher cancellation races, accept-path
error handling.

## Appendix B — Phase 0 checklist status

- [x] `make test` green on epoll, kqueue, io_uring, poll (+ macOS, musl, cosmo).
- [x] `make bench` baseline captured (Appendix A).
- [x] ASan/UBSan status confirmed (`make debug-test`, one pre-existing benign note).
- [x] ABI-sensitive `int fd` surface enumerated and frozen (Appendix A).
- [ ] Focused seam-safety tests (UDP connected-mode, UDS cleanup, watcher
      cancellation, accept-path errors) — land with Phase 1/2.

## Appendix C — Phase 1 record (2026-07-19)

`src/socket.{h,c}` introduced (internal, POSIX-only, one provider). Surface:
`kl_sock_set_nonblocking`, `kl_sock_set_cloexec`, `kl_sock_set_nosigpipe`
(out-of-line in socket.c), and inline `kl_sock_send`/`kl_sock_recv`
(MSG_NOSIGNAL / EINTR loop). Migrated: the duplicated nonblock+CLOEXEC+
SO_NOSIGPIPE+send/recv idioms in `client.c` (io_write/io_read + 2 async connects;
the 2 sync connect-with-timeout paths keep their own fcntl only because they
*restore* blocking mode), `h2_client.c`, `websocket_client.c`, `dns_resolver.c`
(TCP connect + TCP I/O), and `server.c` (`set_nonblocking`/`set_cloexec`
removed). Net: the `#ifdef SO_NOSIGPIPE` block (was 7×) and the `#ifdef
MSG_NOSIGNAL` send loop (was 3×) now live once.

No public API change; no ABI change (frozen `int fd` surface untouched). Verified
on macOS + Linux (epoll + poll) full suite, ASan/UBSan, smoke, scan-build,
cppcheck 2.13. Benchmark before → after (M1 Max / kqueue, req/s): /hello
101,159 → 108,299; /users/42 98,777 → 102,368; /mw/hello 101,675 → 105,544;
POST /echo 97,158 → 104,978 — within run-to-run noise (inline hot path).

**Phase 1b (2026-07-19):** `udp.c`'s own `udp_set_nonblocking` /
`udp_set_cloexec` (exact duplicates of the seam helpers) removed and pointed at
`kl_sock_set_nonblocking` / `kl_sock_set_cloexec`. The UDP `sendmsg`/`recvmsg`/
`recvmmsg` + multicast/GSO/TOS `setsockopt` surface stays specialized (not a
`kl_sock_*` concern). Also folded in: a `.gitignore` fix so all extensionless
example binaries are ignored (`examples/*` + `!examples/*.c` + `!examples/*.md`).

**Still consciously deferred:** `tls_mbedtls.c` (opt-in backend, has its own
MSG_NOSIGNAL write — a TLS-transport concern, not the socket seam);
`thread_pool.c` pipe nonblocking (a pipe, not a socket); `kl_sock_create` /
`kl_sock_connect_nonblocking` combined helpers (call sites' cleanup differs
enough that the granular helpers read better — revisit if a second provider wants
them).

## Appendix D — Phase 2 record (2026-07-20)

The seam gained a provider object (Phase 3's `KlSocketProvider` pulled forward so
a mock can be injected): an immutable `KlSocketOps` table + `void *context` +
capability flags, with a built-in static POSIX provider
(`kl_socket_provider_posix()`, cap `KL_SOCK_CAP_NATIVE_FD`, name "posix"). The
`kl_sock_*` wrappers now take `const KlSocketProvider *`; **a NULL provider keeps
the inline POSIX fast path with no indirect call**, so production hot paths are
unchanged (bench flat — Appendix C numbers held). Any individual op may be NULL
and falls back to POSIX.

**Injection point:** `KlEventCtx` gained an opaque `const struct KlSocketProvider
*sockets` field (forward-declared tag only — the vtable stays in `src/socket.h`,
internal). This is *not* the public provider-selection API (Phase 4); it is
internal plumbing a test or a future provider sets before creating a transport.
Threaded through the client transports (`client.c` io_write/io_read + async
connects; `h2_client.c`; `websocket_client.c`; `dns_resolver.c` TCP I/O +
connect; `udp.c` setup) by reading `ctx->sockets`. The sync blocking-client path
and the **server conn_read/conn_write hot path** pass NULL (stay POSIX) — server
adoption is deferred (it is the throughput-critical path and gains least from
injection).

**Tests** (`tests/test_socket_provider.c`, 10 cases): POSIX identity + caps;
NULL-provider and explicit-POSIX real I/O over socketpair; mock dispatch + setup
hooks; short-write, EWOULDBLOCK, ECONNRESET injection; per-op NULL fallback; a
decorator wrapping real socketpair I/O with a forced short write; and a KlUdp
created on a mock-carrying ctx (proves `ctx->sockets` reaches a real transport).

**Stop check:** adding the `sockets` field to the public `KlEventCtx` is an
additive, source-compatible, opaque-pointer change — it does not expose the
provider API or commit to Phase 4. No behavior change for existing callers.

**Deferred to later phases:** server hot-path adoption; connect/bind/listen/accept
in the ops table (Phase 1 seam only covered setup + send/recv); the public
`KlConfig.sockets` selection API + portable error taxonomy (Phase 4); a real
non-POSIX provider (Phase 6).

## Appendix E — Phase 2 ops-table extension (2026-07-20)

Follow-up to Appendix D: the `KlSocketOps` table grew the socket lifecycle —
`socket`, `connect`, `bind`, `listen`, `accept`, `close` — alongside the existing
setup + send/recv ops, so a provider can own the whole descriptor lifecycle
(a real non-POSIX provider and the public Phase 4 API both need this). Inline
`kl_sock_socket/connect/bind/listen/accept/close` wrappers keep the NULL-provider
/ NULL-op → raw-syscall fast path; the POSIX provider gained matching adapters
(ops table switched to designated initializers).

Adoption: every `socket()`/`connect()` in the client transports now routes
through the seam with its provider (`client.c` sync=NULL + async=ctx, `h2_client`,
`websocket_client`, `dns_resolver` TCP, `udp` socket+bind+connect); the server's
`socket`/`bind`/`listen`/`accept` route through the seam with a NULL provider
(passthrough — no behavior change, but the whole socket lifecycle now flows
through one seam, so future server adoption is a NULL→provider flip). Tests added:
a full socket→bind→listen→connect→accept→close loop over loopback via the
wrappers, mock socket-op dispatch with a NULL-op fallback, and deterministic
connect-failure (ECONNREFUSED) injection. Bench unchanged (lifecycle ops are
one-shot, not per-byte).

## Appendix F — Phase 3 semantics (2026-07-20)

Completes the provider object (the vtable shape landed in Phase 2). All internal;
no public API.

- **Lifecycle.** `KlSocketOps` gained an optional `void (*destroy)(void *ctx)`;
  `kl_socket_provider_destroy(p)` invokes it (NULL provider / NULL op → no-op).
  Contract: a provider is *borrowed* by transports and must outlive them; the
  owner tears it down after all its transports are freed. The POSIX provider is
  static with `destroy == NULL`.
- **Capabilities + native-fd escape hatch.**
  `kl_socket_provider_has_cap(p, cap)` (NULL == POSIX == native-fd);
  `kl_sock_native_fd(p, fd)` returns the OS descriptor as-is when the provider
  advertises `KL_SOCK_CAP_NATIVE_FD`, else -1 (the caller must not treat the
  handle as an OS fd). This is the documented seam for a future non-native
  provider (lwIP raw, UEFI) where the int is an index, not a descriptor.
- **Error taxonomy.** `kl_sock_errno_to_error(int)` maps socket errnos onto the
  existing coarse `KlError` network codes (TIMEOUT / CONNECT for
  refused+unreachable / BIND for in-use+denied / ALLOC for resource exhaustion /
  INVALID_ARG for unsupported+invalid / IO for reset+transient+generic). The raw
  errno is preserved for diagnostics (the function is pure). Finer *public*
  categories (distinct would-block / conn-reset / unreachable codes) are deferred
  to the Phase 4 public error-taxonomy work rather than churning the public
  `KlError` enum during an internal phase.

Tests (`tests/test_socket_provider.c`, now 17 cases): capability query,
native-fd escape hatch (POSIX vs a no-cap mock), destroy called-exactly-once +
no-op paths, and the errno→KlError mapping incl. errno-preservation.

**Not done (Phase 4+):** public `KlConfig.sockets` selection API + custom-provider
example; finer public error codes; server hot-path adoption; connect/bind ops
already exist but the server still passes NULL.
