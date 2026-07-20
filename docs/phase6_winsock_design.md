# Phase 5 + 6 — Portable handles + first Winsock provider — Design

Status: **designed (2026-07-20), pending implementation.** Follow-on to PAL
Phases 1–3 + server adoption. This is the largest PAL step: the first non-POSIX
socket provider, which is what finally *validates* the `KlSocketProvider` vtable
(and thus unblocks the public Phase 4 API, `docs/phase4_public_provider_design.md`).

**Decisions (2026-07-20):**
- **Portable handle first (Phase 5).** Introduce a `KlSocketHandle` type across
  the socket-facing API *before* Winsock, so the vtable is correct on Win64 from
  the start (a `SOCKET` is pointer-width, not `int`).
- **MinGW-w64 + a native Windows CI runner.** Keep the gmake Makefile with
  `_WIN32` guards + `ws2_32`; add a `windows-latest` (MSYS2/MinGW) CI job.
- **Fuller port.** Aim for most of the suite green on Windows, staged.

Context: Windows is greenfield (no `_WIN32` anywhere today). Cosmopolitan already
*runs* Keel on Windows via its POSIX-over-Winsock layer — but that uses Cosmo's
fds, so it does **not** exercise a `KlSocketProvider`. A native Winsock provider
is what proves the abstraction.

---

## Part A — Phase 5: portable socket handle

### A.1 The type — pointer-width on every platform
```c
/* keel/handle.h (new, installed) */
#include <stdint.h>
typedef intptr_t KlSocketHandle;               /* holds any provider's handle */
#define KL_INVALID_SOCKET ((KlSocketHandle)-1)
static inline int kl_handle_valid(KlSocketHandle h) { return h != KL_INVALID_SOCKET; }
```
`KlSocketHandle` is **`intptr_t` (pointer-width) on all platforms**, not `int` on
POSIX. Pointer-width because a provider's native handle may be wider than `int`
*or* a pointer:

| Provider | Native handle | Fits `intptr_t` |
|---|---|---|
| POSIX sockets | `int` fd | yes |
| Winsock | `SOCKET` (`UINT_PTR`) | yes |
| lwIP socket API | `int` fd | yes |
| lwIP raw API | `struct tcp_pcb *` | yes (needs pointer width) |
| UEFI | `EFI_*_PROTOCOL *` | yes (needs pointer width) |

Doing the (one-time, API-breaking) handle change now at pointer width avoids a
*second* break when a pointer-handle stack lands. `INVALID_SOCKET` is `(SOCKET)(~0)`
= `-1` reinterpreted, so `KL_INVALID_SOCKET == -1` is the shared invalid sentinel
for every **descriptor**-based provider (POSIX, Winsock, lwIP socket API). Because
`SOCKET` is unsigned, the ubiquitous POSIX idiom `if (fd < 0)` is unreliable on
Windows — so the core Phase 5 migration is mechanical but wide: replace
`fd < 0` / `fd == -1` socket checks with `!kl_handle_valid(fd)`.

### A.2 What changes to `KlSocketHandle`
Socket descriptors only: `KlConn.fd`, `KlUdp.fd`, `KlWatcher.fd`,
`KlResponse.conn_fd`, `KlConfig.listen_fd`, `KlServer.listen_fd`,
`KlClientPoolConn.fd`, the `event.h` API (`kl_event_add/mod/del`), `KlWatcherFn`,
the `KlTls` vtable (`handshake/read/write/shutdown`), `KlFileIO.sock_fd`,
`kl_udp_fd`/`kl_udp_server_fd`, `kl_peer_cred_fd`, `kl_systemd_listen_fd*`, and
the `KlSocketOps` signatures.

**Stays `int`:** *file* descriptors — `KlResponse.file_fd`, `KlFileIO.file_fd`
(a CRT fd on Windows via `_open`; the Winsock `sendfile` op converts it with
`_get_osfhandle` for `TransmitFile`). `KlEventLoop.fd` (backend-internal; -1 on
WSAPoll like poll). File and socket handles are genuinely different types on
Windows and must not be conflated.

### A.3 Source/ABI impact
On POSIX, `int` → `intptr_t` is **source-compatible** (an `int` fd stored in a
signed pointer-width integer; `-1` still invalid; arithmetic/comparison
unchanged), but **ABI-widening** — socket-handle struct fields grow 4→8 bytes on
LP64. That is fine under Keel's static-linking model (a version bump is a
recompile anyway) and has no runtime-behavior effect. So Phase 5 is *behaviorally*
a no-op on POSIX — the only real edits are the `fd < 0` → `kl_handle_valid` sweep
and the typedef — verified byte-for-byte by the existing full gauntlet (incl.
musl) + a flat bench. Ship it as its own CI-green POSIX-only commit before any
Winsock code. (Note: this loses the "pure no-op / no ABI change" property of an
`int` typedef; the trade is deliberate — one pointer-width break now vs two
breaks later.)

---

### A.4 lwIP compatibility — socket API vs raw API
lwIP is a future provider target; the handle decision above is what makes it fit
(or not), and the two lwIP APIs behave very differently:

**lwIP socket API** (`lwip_socket`/`lwip_send`/`lwip_recv`, BSD-compatible) —
**directly compatible.** Its descriptors are small `int` indices into lwIP's
socket table, and it returns `-1` on error like POSIX. `KlSocketHandle` (as
`intptr_t`) holds them, `KL_INVALID_SOCKET = -1` is correct, and its
`poll`/`select` shim maps onto the existing readiness `event_poll` backend. This
is the **low-risk lwIP integration** (a `KlSocketProvider` over `lwip_*` + a
poll-style backend) — mostly a provider + backend selection, no model change.

**lwIP raw API** (`struct tcp_pcb *` + callbacks) — the handle is a **pointer**,
which is exactly why `KlSocketHandle` is pointer-width: a raw-lwIP provider stores
the `tcp_pcb *` in the handle. **But the handle is the easy part.** The raw API is
not descriptor/readiness-based — it is callback-driven around
`sys_check_timeouts()`, structurally like IOCP (completion, not readiness), and
its invalid sentinel is `NULL` (`0`), not `-1`. So a raw-lwIP provider needs
**event-axis** work — a callback-driven progress model, not fd polling — which is
Phase 9, independent of this handle change. `intptr_t` makes the raw handle
*storable* today; it does not make raw lwIP *drivable* until Phase 9.

**Takeaway:** pointer-width `KlSocketHandle` keeps all three *descriptor*-based
stacks (POSIX, Winsock, lwIP socket API) on one readiness model with a shared
`-1` sentinel; the raw APIs (lwIP raw, IOCP) are a separate event-axis concern
regardless of handle type.

## Part B — Phase 6: the Winsock provider

### B.1 Ops mapping (`src/socket_winsock.c`, built only on `_WIN32`)
| KlSocketOps | POSIX | Winsock |
|---|---|---|
| `socket` | `socket()` | `WSASocketW(..., WSA_FLAG_NO_HANDLE_INHERIT)` |
| `connect`/`bind`/`listen`/`accept` | same | Winsock `connect`/`bind`/`listen`/`accept` (readiness — **not** AcceptEx/ConnectEx; those are IOCP/Phase 8) |
| `close` | `close()` | `closesocket()` |
| `send`/`recv` | `send(MSG_NOSIGNAL)`/`recv` | `send`/`recv` (no MSG_NOSIGNAL — Windows has no SIGPIPE) |
| `writev` (new op) | `writev()` | `WSASend()` with a `WSABUF[]` |
| `sendfile` (new op) | `sendfile()` | `TransmitFile()` (file `HANDLE` via `_get_osfhandle`) |
| `set_nonblocking` | `fcntl(O_NONBLOCK)` | `ioctlsocket(FIONBIO)` |
| `set_cloexec` | `fcntl(FD_CLOEXEC)` | `WSA_FLAG_NO_HANDLE_INHERIT` at create (no-op here) |
| `set_nosigpipe` | `SO_NOSIGPIPE` | no-op (no SIGPIPE) |
| `destroy` | none | `WSACleanup()` |
| caps | NATIVE_FD·WRITEV·SENDFILE | NATIVE_FD·WRITEV·SENDFILE |

**writev/sendfile become real vtable ops** here (server adoption left them as a
capability-gated hook precisely for this). POSIX gains `writev`/`sendfile` ops
that call the syscalls; the capability-gate stays for minimal providers.

`kl_socket_provider_winsock()` factory ensures `WSAStartup(MAKEWORD(2,2))`
(refcounted; `WSACleanup` on `destroy`). A `SOCKET` is native-pollable, so
NATIVE_FD holds and the server/event loop accept it.

### B.2 WSAPoll event backend
`WSAPOLLFD` is layout- and flag-compatible with `struct pollfd` (`POLLIN`/
`POLLOUT` exist in Winsock). Reuse `src/event_poll.c` with `#ifdef _WIN32`:
`poll` → `WSAPoll`, `<poll.h>` → `<winsock2.h>`. **Known WSAPoll defect:** a
failed non-blocking `connect` is not reported via `POLLOUT`. Keel already
verifies connect completion with `getsockopt(SO_ERROR)` after the writable event;
the mitigation is a bounded connect deadline so a never-signalled failure still
times out (the client already has the Happy-Eyeballs deadline; the server does
not `connect`). Document + test explicitly.

### B.3 Platform services for a fuller port (narrow interfaces, per the PAL doc)
Not everything is socket-shaped. A fuller Windows port needs small platform shims
(prefer several narrow files over one `KlPlatformOps`):
- **Monotonic clock** (`kl_monotonic_ms`): `QueryPerformanceCounter` /
  `GetTickCount64` on Windows.
- **Secure random** (DNS 0x20/cookies, WS mask): `BCryptGenRandom` / `rand_s`
  instead of `/dev/urandom`.
- **Thread-pool wakeup**: `thread_pool.c` uses `pipe()`. MinGW winpthreads gives
  pthreads; Windows has no `pipe()` on sockets and no `socketpair()` — use a
  self-connected loopback TCP pair for the wakeup fd (a small helper), so the
  pipe watcher stays a pollable socket.
- **Signals**: `SIGTERM`/`SIGINT` handling differs; the existing `KL_NO_SIGNAL`
  guard covers the minimal case; a `SetConsoleCtrlHandler` shim is optional.
- **AF_UNIX**: Windows 10+ supports `AF_UNIX` — unix-socket paths may work
  as-is; gate the tests behind a runtime probe.
- **File I/O**: `file_io_iouring` is Linux-only (already backend-gated); Windows
  uses the sync/buffered file path + `TransmitFile`.
- **TLS**: the mbedtls backend is portable C — build it, or mark TLS tests
  skipped on Windows for the prototype (SChannel is a later, separate backend).

### B.4 Build system (MinGW-w64)
- Makefile: detect Windows (`uname -s` → `MINGW*`/`MSYS*`, or an explicit
  `OS=windows`). On Windows: `EVENT_SRC = event_poll.c` (WSAPoll via `#ifdef`),
  add `src/socket_winsock.c`, link `-lws2_32 -lbcrypt`, test binaries get `.exe`.
- Cross-compile from Linux for a fast inner loop:
  `make CC=x86_64-w64-mingw32-gcc OS=windows`.
- Provider selection is compile-time on Windows: `kl_socket_provider_winsock()`
  is the sensible default there (wire it as the default when the public Phase 4
  config lands; until then the transports use it internally on `_WIN32`).

### B.5 Windows CI job
Add to `.github/workflows/ci.yml`:
```yaml
  windows:
    name: Windows (MinGW/WSAPoll)
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@...
      - uses: msys2/setup-msys2@...       # mingw-w64-x86_64-gcc, make
      - run: make OS=windows              # (shell: msys2)
      - run: make test OS=windows
```
Fuzzers/valgrind stay Linux-only. Start with the core test subset green, expand.

---

## Part C — Staging (each a CI-green commit)

1. **Phase 5 groundwork (done).** `keel/handle.h` (`KlSocketHandle = intptr_t` +
   `KL_INVALID_SOCKET` + `kl_handle_valid`) added to the umbrella; `writev` +
   `sendfile` promoted to real `KlSocketOps` ops (POSIX impls in `socket.c`;
   `kl_posix_sendfile` moved out of `response.c`); `response.c` dispatches through
   `kl_sock_writev`/`kl_sock_sendfile` (op present → op; else serialize/pread-send
   fallback). POSIX-validatable + bench flat. The **132-site `int fd` →
   `KlSocketHandle` field/check sweep is deferred to 6a**, where the MinGW build
   validates the unsigned-`SOCKET` / `!kl_handle_valid` correctness that the POSIX
   gauntlet structurally cannot.
   **Local validation loop (established):** a `debian:bookworm` +
   `gcc-mingw-w64-x86-64` container cross-compiles/links a Winsock program with
   `-Werror` — used as the pre-CI Windows check until a `windows-latest` runner
   exists. A `_Static_assert(sizeof(KlSocketHandle) >= sizeof(SOCKET))` +
   `INVALID_SOCKET`→`kl_handle_valid` compile-check confirms the handle type is
   lossless for a Winsock `SOCKET`.
1a. **6a-1 — retype the provider vtable to `KlSocketHandle` (done, POSIX-green +
   MinGW-validated).** `KlSocketOps` fd params + `socket`/`accept` returns +
   `kl_sock_*` wrappers + the POSIX ops adapters + the test mocks now use
   `KlSocketHandle` (POSIX fallbacks cast `(int)fd`; `in_fd` for `sendfile` stays
   `int` — a *file* handle). Behavior-preserving on POSIX (the callers' existing
   `int fd = kl_sock_socket(...)` narrows silently — those storage sites are the
   6a-2 field sweep). Validated on MinGW: the vtable is now non-lossy for a
   `SOCKET`.
2. **6a — Winsock provider + WSAPoll + build.** The `int fd`→`KlSocketHandle`
   field/check sweep (6a-2), `socket_winsock.c`, WSAPoll `#ifdef`, monotonic
   clock + entropy shims, MinGW Makefile, the loopback-pair wakeup. Green: build +
   the socket-provider tests + a plaintext server/client roundtrip on the Windows
   runner.
3. **6b — breadth.** UDP over Winsock, thread pool, more of the server/client
   suite, AF_UNIX probe, TLS (mbedtls build or skip). Expand the Windows test
   subset toward parity; document what's skipped and why.
4. **Feed back to the vtable.** Confirm `KlSocketHandle` + `writev`/`sendfile`
   ops are sufficient → **unblocks Phase 4** (publish the now-proven vtable).

## Part D — Risks / stop conditions
- **WSAPoll connect-failure defect** (B.2) → bounded deadline + `SO_ERROR`;
  stop & reconsider a `select()`-based backend if it can't be made reliable.
- **Handle truncation** — if any code still assumes `int`-width sockets after the
  sweep, catch it via `-Wconversion` on the Windows build for socket types.
- **pthread/pipe on Windows** — if winpthreads or the loopback-pair wakeup is
  flaky, that gates 6b (thread pool), not 6a.
- **Scope blowup** — "fuller port" is genuinely large; keep each 6b feature its
  own commit, and it is fine to ship 6a (core, vtable validated) and pause: the
  vtable validation — the real objective — is achieved at 6a.
- **Don't drift toward IOCP** — this is readiness-mode Winsock. IOCP is Phase 8
  (completion model, event-axis), explicitly out of scope.

## Part E — What this validates (the objective)
By the end of 6a: a real non-POSIX provider drives Keel's client and server over
WSAPoll, proving (1) the `KlSocketProvider` ops table, (2) `KlSocketHandle` as the
portable handle, and (3) `writev`/`sendfile` as vtable ops. That is exactly the
"proven by a real alternative provider" bar Phase 4 was waiting on — so Phase 4
(public API) executes next, on a shape validated in production, not on paper.
