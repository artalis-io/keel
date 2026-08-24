# Phase 5 + 6, Portable handles + first Winsock provider, Design

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
*runs* Keel on Windows via its POSIX-over-Winsock layer, but that uses Cosmo's
fds, so it does **not** exercise a `KlSocketProvider`. A native Winsock provider
is what proves the abstraction.

---

## Part A - Phase 5: portable socket handle

### A.1 The type: pointer-width on every platform
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
Windows, so the core Phase 5 migration is mechanical but wide: replace
`fd < 0` / `fd == -1` socket checks with `!kl_handle_valid(fd)`.

### A.2 What changes to `KlSocketHandle`
Socket descriptors only: `KlConn.fd`, `KlUdp.fd`, `KlWatcher.fd`,
`KlResponse.conn_fd`, `KlConfig.listen_fd`, `KlServer.listen_fd`,
`KlClientPoolConn.fd`, the `event.h` API (`kl_event_add/mod/del`), `KlWatcherFn`,
the `KlTls` vtable (`handshake/read/write/shutdown`), `KlFileIO.sock_fd`,
`kl_udp_fd`/`kl_udp_server_fd`, `kl_peer_cred_fd`, `kl_systemd_listen_fd*`, and
the `KlSocketOps` signatures.

**Stays `int`:** *file* descriptors; `KlResponse.file_fd`, `KlFileIO.file_fd`
(a CRT fd on Windows via `_open`; the Winsock `sendfile` op converts it with
`_get_osfhandle` for `TransmitFile`). `KlEventLoop.fd` (backend-internal; -1 on
WSAPoll like poll). File and socket handles are genuinely different types on
Windows and must not be conflated.

### A.3 Source/ABI impact
On POSIX, `int` → `intptr_t` is **source-compatible** (an `int` fd stored in a
signed pointer-width integer; `-1` still invalid; arithmetic/comparison
unchanged), but **ABI-widening**: socket-handle struct fields grow 4→8 bytes on
LP64. That is fine under Keel's static-linking model (a version bump is a
recompile anyway) and has no runtime-behavior effect. So Phase 5 is *behaviorally*
a no-op on POSIX, the only real edits are the `fd < 0` → `kl_handle_valid` sweep
and the typedef: verified byte-for-byte by the existing full gauntlet (incl.
musl) + a flat bench. Ship it as its own CI-green POSIX-only commit before any
Winsock code. (Note: this loses the "pure no-op / no ABI change" property of an
`int` typedef; the trade is deliberate: one pointer-width break now vs two
breaks later.)

---

### A.4 lwIP compatibility: socket API vs raw API
lwIP is a future provider target; the handle decision above is what makes it fit
(or not), and the two lwIP APIs behave very differently:

**lwIP socket API** (`lwip_socket`/`lwip_send`/`lwip_recv`, BSD-compatible):
**directly compatible.** Its descriptors are small `int` indices into lwIP's
socket table, and it returns `-1` on error like POSIX. `KlSocketHandle` (as
`intptr_t`) holds them, `KL_INVALID_SOCKET = -1` is correct, and its
`poll`/`select` shim maps onto the existing readiness `event_poll` backend. This
is the **low-risk lwIP integration** (a `KlSocketProvider` over `lwip_*` + a
poll-style backend): mostly a provider + backend selection, no model change.

**lwIP raw API** (`struct tcp_pcb *` + callbacks), the handle is a **pointer**,
which is exactly why `KlSocketHandle` is pointer-width: a raw-lwIP provider stores
the `tcp_pcb *` in the handle. **But the handle is the easy part.** The raw API is
not descriptor/readiness-based: it is callback-driven around
`sys_check_timeouts()`, structurally like IOCP (completion, not readiness), and
its invalid sentinel is `NULL` (`0`), not `-1`. So a raw-lwIP provider needs
**event-axis** work, a callback-driven progress model, not fd polling, which is
Phase 9, independent of this handle change. `intptr_t` makes the raw handle
*storable* today; it does not make raw lwIP *drivable* until Phase 9.

**Takeaway:** pointer-width `KlSocketHandle` keeps all three *descriptor*-based
stacks (POSIX, Winsock, lwIP socket API) on one readiness model with a shared
`-1` sentinel; the raw APIs (lwIP raw, IOCP) are a separate event-axis concern
regardless of handle type.

## Part B - Phase 6: the Winsock provider

### B.1 Ops mapping (`src/socket_winsock.c`, built only on `_WIN32`)
| KlSocketOps | POSIX | Winsock |
|---|---|---|
| `socket` | `socket()` | `WSASocketW(..., WSA_FLAG_NO_HANDLE_INHERIT)` |
| `connect`/`bind`/`listen`/`accept` | same | Winsock `connect`/`bind`/`listen`/`accept` (readiness, **not** AcceptEx/ConnectEx; those are IOCP/Phase 8) |
| `close` | `close()` | `closesocket()` |
| `send`/`recv` | `send(MSG_NOSIGNAL)`/`recv` | `send`/`recv` (no MSG_NOSIGNAL, Windows has no SIGPIPE) |
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

### B.0 Platform-isolation principle (architecture)
**One platform family per translation unit, selected by the Makefile; never
`#ifdef _WIN32` inside a POSIX TU.** This is not new: Keel already isolates event
backends this way: `event_epoll.c`, `event_kqueue.c`, `event_poll.c`,
`event_iouring.c` are fully independent TUs that share no code, each implementing
`kl_event_*` for its platform, one selected per build. The Windows port extends
that existing pattern rather than threading `#ifdef` through shared files:
- **Event backend** → a new independent `src/event_wsapoll.c` TU (sibling to the
  others). `event_poll.c` is *not touched*.
- **Socket provider** → `socket_posix.c` (the POSIX provider) stays POSIX-family (its `__linux__`/`__APPLE__`
  `sendfile` variants are all POSIX); Winsock lives entirely in a separate
  `src/socket_winsock.c`. Consumers only call the `kl_sock_*` seam, so the
  remaining raw syscalls (`setsockopt`/`getsockname`/`close`/…) are handled by
  **routing them through new seam ops**, not by `#ifdef` in `server.c`/`udp.c`.
- **Platform services** (clock, entropy, thread wakeup) → a narrow interface with
  per-OS TUs (`platform_posix.c` / `platform_win.c`), replacing the scattered
  `#ifdef __APPLE__`/`/dev/urandom`/`clock_gettime` sites, not new `#ifdef _WIN32`
  blocks in `connection.c`/`dns_resolver.c`/`websocket_client.c`.

The Makefile already branches `EVENT_SRC`/`FILE_IO_SRC` by platform+backend; the
Windows branch adds `event_wsapoll.c` + `socket_winsock.c` + `platform_win.c` and
links `ws2_32`/`bcrypt`. Zero cross-platform `#ifdef` enters an existing TU.

### B.2 WSAPoll event backend (independent TU)
`src/event_wsapoll.c`, a self-contained backend TU implementing `kl_event_*`
over `WSAPoll(WSAPOLLFD*, ULONG, INT)`. It does **not** reuse `event_poll.c`'s
`fd_to_idx[fd]` direct-index map: a `SOCKET` is a large sparse kernel handle, not
a small dense fd, so the direct-index array is wrong for Windows. WSAPoll uses a
`SOCKET`-safe fd→slot lookup instead (linear scan over the active `WSAPOLLFD`
array: O(n) per add/mod/del, consistent with Keel's O(n) router / O(n) timeout
sweep; upgrade to a small hash only if profiling warrants). Uses `POLLRDNORM`/
`POLLWRNORM` for the `events` field (WSAPoll's documented input flags) and reads
`POLLRDNORM|POLLHUP|POLLERR` / `POLLWRNORM` from `revents`. **Known WSAPoll
defect:** a failed non-blocking `connect` is not reported via a writable event.
Keel already verifies connect completion with `getsockopt(SO_ERROR)` after the
writable event; the mitigation is a bounded connect deadline so a never-signalled
failure still times out (the client already has the Happy-Eyeballs deadline; the
server does not `connect`). Document + test explicitly.

### B.3 Platform services for a fuller port (narrow interfaces, per the PAL doc)
Not everything is socket-shaped. A fuller Windows port needs small platform shims
(prefer several narrow files over one `KlPlatformOps`):
- **Monotonic clock** (`kl_monotonic_ms`): `QueryPerformanceCounter` /
  `GetTickCount64` on Windows.
- **Secure random** (DNS 0x20/cookies, WS mask): `BCryptGenRandom` / `rand_s`
  instead of `/dev/urandom`.
- **Thread-pool wakeup**: `thread_pool.c` uses `pipe()`. MinGW winpthreads gives
  pthreads; Windows has no `pipe()` on sockets and no `socketpair()`: use a
  self-connected loopback TCP pair for the wakeup fd (a small helper), so the
  pipe watcher stays a pollable socket.
- **Signals**: `SIGTERM`/`SIGINT` handling differs; the existing `KL_NO_SIGNAL`
  guard covers the minimal case; a `SetConsoleCtrlHandler` shim is optional.
- **AF_UNIX**: Windows 10+ supports `AF_UNIX`; unix-socket paths may work
  as-is; gate the tests behind a runtime probe.
- **File I/O**: `file_io_iouring` is Linux-only (already backend-gated); Windows
  uses the sync/buffered file path + `TransmitFile`.
- **TLS**: the mbedtls backend is portable C; build it, or mark TLS tests
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

## Part C: Staging (each a CI-green commit)

1. **Phase 5 groundwork (done).** `keel/handle.h` (`KlSocketHandle = intptr_t` +
   `KL_INVALID_SOCKET` + `kl_handle_valid`) added to the umbrella; `writev` +
   `sendfile` promoted to real `KlSocketOps` ops (POSIX impls in `socket_posix.c`;
   `kl_posix_sendfile` moved out of `response.c`); `response.c` dispatches through
   `kl_sock_writev`/`kl_sock_sendfile` (op present → op; else serialize/pread-send
   fallback). POSIX-validatable + bench flat. The **132-site `int fd` →
   `KlSocketHandle` field/check sweep is deferred to 6a**, where the MinGW build
   validates the unsigned-`SOCKET` / `!kl_handle_valid` correctness that the POSIX
   gauntlet structurally cannot.
   **Local validation loop (established):** a `debian:bookworm` +
   `gcc-mingw-w64-x86-64` container cross-compiles/links a Winsock program with
   `-Werror`: used as the pre-CI Windows check until a `windows-latest` runner
   exists. A `_Static_assert(sizeof(KlSocketHandle) >= sizeof(SOCKET))` +
   `INVALID_SOCKET`→`kl_handle_valid` compile-check confirms the handle type is
   lossless for a Winsock `SOCKET`.
1a. **6a-1: retype the provider vtable to `KlSocketHandle` (done, POSIX-green +
   MinGW-validated).** `KlSocketOps` fd params + `socket`/`accept` returns +
   `kl_sock_*` wrappers + the POSIX ops adapters + the test mocks now use
   `KlSocketHandle` (POSIX fallbacks cast `(int)fd`; `in_fd` for `sendfile` stays
   `int`, a *file* handle). Behavior-preserving on POSIX (the callers' existing
   `int fd = kl_sock_socket(...)` narrows silently, those storage sites are the
   6a-2 field sweep). Validated on MinGW: the vtable is now non-lossy for a
   `SOCKET`.
1b. **6a-2: retype the public socket-handle API surface to `KlSocketHandle`
   (done, POSIX-green + MinGW-header-validated).** The struct fields + function
   signatures that carry a socket descriptor: `KlConn.fd`, `KlUdp.fd`,
   `KlWatcher.fd` + `KlWatcherFn`, `KlResponse.conn_fd`, `KlServer/KlConfig
   .listen_fd`, `KlClientPoolConn.fd`; the `event.h`/`event_ctx.h` APIs
   (`kl_event_*`, `kl_watcher_*`), `kl_conn_acquire`, `kl_response_file`,
   `kl_udp_fd`, `kl_udp_server_fd`, `kl_peer_cred_fd`; and the `KlTls` +
   `KlFileIO` (`sock_fd`) vtables. `KlEventLoop.fd` and `file_fd`/`in_fd` stay
   `int` (backend-internal / *file* handles). Every consumer forced by `-Werror`
   (watcher callbacks, event backends, TLS/file_io impls in src + tests +
   examples) retyped. Validated: full POSIX gauntlet (epoll/kqueue/io_uring/poll +
   musl) + a MinGW compile of the retyped public headers under `winsock2.h` (the
   handle-bearing surface is SOCKET-compatible). **Deferred to 6a-3:** the local
   `int fd` *variables* in consumers that store a socket handle (they narrow
   silently on POSIX; `-Wconversion` on the MinGW build validates their
   conversion) and the `fd < 0` → `kl_handle_valid` check style (behavior-
   equivalent for the intptr_t rep, INVALID_SOCKET→-1, so not a correctness
   change on POSIX, done where the `.c` cross-compiles).
1c. **6a-3a: socket-handle storage sweep (done, POSIX-green + `-Wconversion`
   validated).** Retyped the *internal* socket-handle storage that 6a-2 (public
   headers only) didn't reach: the transport structs' fd fields (`KlClient.fd`,
   `KlConnAttempt.fd`, `KlH2ClientConn.fd`, `KlWsClientConn.fd`, `KlDnsTcp.fd`)
   and the `int fd = kl_sock_socket(...)` / `kl_sock_accept(...)` locals across
   client.c/h2_client.c/websocket_client.c/dns_resolver.c/server.c → all
   `KlSocketHandle`. This kills the *truncate-at-birth* bug: a Winsock `SOCKET`
   is no longer narrowed to `int` at the point it's created/accepted. Validated
   the POSIX way: **`gcc -Wconversion` as a per-file audit** (it flags every
   `KlSocketHandle`→`int` narrowing, even on Linux): confirmed **zero** remaining
   narrowings involve `kl_sock_socket`/`kl_sock_accept` storage; the residue is
   exclusively terminal raw-POSIX-syscall args. Normal `-Werror` build unchanged
   (no `-Wconversion` in the Makefile, it's an audit tool, not a build gate).
1d. **6a-3b-i: WSAPoll backend TU + Makefile Windows branch (done, scaffolding).**
   `src/event_wsapoll.c`, an independent backend TU (sibling to event_epoll/
   kqueue/poll, no shared `#ifdef`), WSAPoll with a `SOCKET`-safe linear-scan
   fd→slot lookup (not the fd-indexed map poll uses). Makefile gains a top-level
   `WINDOWS` branch (`OS=windows` or MinGW/MSYS uname) selecting
   `event_wsapoll.c` + `-lws2_32 -lbcrypt`, isolated from the ELF hardening.
   Validated: POSIX builds unchanged (still select epoll/kqueue; `event_wsapoll.c`
   never compiled there), and `event_wsapoll.c` cross-compiles clean under
   MinGW-w64 with `-Werror`. Full `make OS=windows` does not link yet; it needs
   the remaining Windows TUs below.
1f. **6a-3b-ii/a: socket.h made platform-neutral (done, POSIX-green +
   MinGW-`socket.h`-compiles).** The seam header had raw POSIX syscalls in its
   inline NULL-provider fast path (`send(MSG_NOSIGNAL)`, `writev`, `close`): they
   would not compile under MinGW. Moved those bodies out of the header into the
   per-platform provider TU as a `kl_sockdef_*` default family (declared in
   socket.h, defined in socket_posix.c; socket_winsock.c will define the Windows
   versions). socket.h now dispatches inline through a provider's ops, else calls
   `kl_sockdef_*`: zero syscall in the header. The one unavoidable platform-
   include divergence (winsock2/ws2tcpip vs sys/socket, `struct iovec` shim,
   `off_t`/`ssize_t`) is isolated in a new `src/sockcompat.h`. `response.c` is
   untouched, the `struct iovec` shim keeps the seam's `writev` signature intact
   (no `KlIoVec` churn). Behaviorally a no-op on POSIX; the NULL-provider default
   is now one direct call (bench flat, it wraps a syscall). Validated: full POSIX
   gauntlet (epoll/kqueue/io_uring/poll + musl) + a MinGW compile of `socket.h`.
2. **6a-3b-ii/b: Winsock provider + platform TUs + raw-syscall routing.**
   **6a-3b-ii/b-1: Winsock provider (done, MinGW cross-compile validated).**
   `src/socket_winsock.c`: the Windows `kl_sockdef_*` (ioctlsocket FIONBIO,
   closesocket, WSASend for writev via `struct iovec`→`WSABUF`, a `_read`+`send`
   sendfile: TransmitFile is a 6b optimization, `SetHandleInformation` cloexec,
   no-op nosigpipe) + the Winsock `KlSocketOps` + `kl_socket_provider_winsock()`
   (`WSAStartup`/`WSACleanup` on destroy) + a WSA-code `kl_sock_errno_to_error`.
   Makefile gains a `SOCKET_SRC` selector (POSIX `socket_posix.c` default; Windows
   `socket_winsock.c`) + `-lmswsock`; `socket.h` declares the winsock factory.
   POSIX unaffected; `socket_winsock.c` cross-compiles clean under MinGW-w64.
   **Still pending (6a-3b-ii/b-2):** `src/platform_win.c`
   (monotonic clock via `QueryPerformanceCounter`, entropy via `BCryptGenRandom`,
   loopback-pair thread wakeup) behind a narrow `platform.h` with a
   `platform_posix.c` sibling; the raw-syscall residue routed through **new seam
   ops** (`setsockopt`/`getsockname`/`shutdown`/…) so `server.c`/`udp.c` stay
   POSIX-`#ifdef`-free, plus `fd<0`→`kl_handle_valid`; the Windows `CORE_SRC`
   swapping in `platform_win.c` and dropping POSIX-only TUs so `make OS=windows`
   *links*. Green: build + socket-provider tests + a plaintext server/client
   roundtrip on the Windows runner.
3. **6b: breadth.** UDP over Winsock, thread pool, more of the server/client
   suite, AF_UNIX probe, TLS (mbedtls build or skip). Expand the Windows test
   subset toward parity; document what's skipped and why.
   **6b-1: Winsock UDP + built-in DNS (done).** `udp_io_win.c` (WSARecvMsg/
   WSASendMsg + cmsg) and `dns_sys_win.c` (iphlpapi config discovery), each gated
   by a loopback smoke test on the runner (`smoke-udp`, `smoke-dns`).
   **6b-2: staged `utest` suite on the runner (done, Tier 1).** The Windows CI
   ran only link+roundtrip smoke tests; it now also runs a curated `make test-win`
   subset of the real `utest` suites. Unblocking fix: `utest.h` self-declares
   `QueryPerformanceCounter` on MinGW unless `<windows.h>` precedes it, clashing
   once a Keel header pulls in `<winsock2.h>`; a force-included `tests/win_prelude.h`
   (winsock2→ws2tcpip→windows, the sockcompat ordering) resolves it for every test
   without touching vendored `utest.h` or the 55 test files. `WIN_TEST_SUITES`
   (Makefile) started as **Tier 1**: 12 platform-neutral suites (allocator,
   body_reader, chunked, cors, decompress, drain, multipart_stream, overflow,
   parser, response_parser, router, url): pure in-memory logic. **Tier 2** then
   added the 9 build-clean socket/thread *runtime* suites (client, client_stream,
   connection, h2_client, redirect, server_stats, thread_pool, timer,
   websocket_client), validated green on the runner: they exercise the same
   WSAPoll/Winsock/winpthreads machinery the smoke tests prove. This also **clears
   the Part-D thread-pool risk**: `thread_pool` (winpthreads + the loopback-pair
   wakeup) passes on Windows. That brought CI to **21 utest suites** on Windows.
   **6b-3: Tier 3 (done).** The remaining portable suites use raw POSIX socket
   idioms in the *test harness* (`<sys/socket.h>` et al. plus socketpair/pipe/close/
   read/write/fcntl/poll/SO_RCVTIMEO). These are abstracted behind
   **`tests/net_compat.h`**: deliberately shaped like the library's own socket seam
   (`socket.h` + `socket_posix.c`/`socket_winsock.c`): the header carries no logic
   `#ifdef`, only the one include-selection boundary (the same boundary
   `src/sockcompat.h` owns), and the platform logic lives in two sibling TUs
   **`net_compat_posix.c`** / **`net_compat_win.c`** selected by the Makefile and
   linked into each test. The helpers are `kl_test_socketpair`/`closesock`/`sockread`/
   `sockwrite`/`set_nonblock`/`poll1`/`set_rcvtimeo` (on Windows `socketpair`/`pipe`
   become a self-connected loopback TCP pair: Winsock has neither). The 26 suites
   were mechanically ported to call the helpers (POSIX behavior unchanged, thin
   wrappers). Windows CI now runs **44 of 55** utest suites. The few genuinely
   POSIX-specific tests inside otherwise-portable suites are guarded
   `#if !defined(_WIN32)` (`kl_socket_provider_posix()`, POSIX-only TU; `kill()`/
   `SIGTERM`; the POSIX-errno taxonomy; `he.all_fail`, the WSAPoll connect-failure
   defect of §B.2; `sendfile_fallback`: hardcoded `/tmp` + CRT file I/O). One real
   backend gap surfaced and was fixed: `event_wsapoll.c`'s `kl_event_add` now rejects
   an invalid socket handle (`!kl_handle_valid`), matching the epoll/kqueue/poll
   backends. **11 suites excluded.** 9 are genuinely POSIX/Linux-only: `tls`,
   `tls_integration`, `peer_cert` (no TLS backend built on Windows, SChannel/mbedtls
   is a later, separate task); `udp_batching` (`recvmmsg`/`sendmmsg`) and
   `udp_offload` (UDP GSO): Linux-only offload; `udp_tos` (Windows restricts
   `IP_TOS`/DSCP `setsockopt`); `unix_socket` (`SO_PEERCRED` is Linux-only; Win10
   AF_UNIX lacks it); `file_io` + `file_io_iouring` (io_uring / POSIX file-path
   assumptions). The other 2 build clean but have runtime failures needing
   Windows-native iteration, so are deferred (not hidden): `dns_resolver` (its
   mock-UDP-nameserver + hosts/resolv.conf harness) and `proxy` (CONNECT-tunnel
   timing): both paths remain covered on Windows by `smoke-dns` and the POSIX
   suites. Local validation: all 44 build to PE32+ via `make OS=windows` (MinGW-w64)
   and the full POSIX gauntlet stays green; the runner executes them for real.
   **6b-4: mbedTLS backend on Windows + the mock-TLS mis-exclusion (done).** The
   mbedTLS `KlTls` backend (`src/tls_mbedtls.c`) is now platform-neutral: its BIO
   callbacks route through the socket seam (`kl_sockdef_send`/`kl_sockdef_recv`),
   which already own SIGPIPE suppression, EINTR retry, and errno translation
   (incl. Winsock `kl_wsa_set_errno`): instead of raw `send(MSG_NOSIGNAL)`/`read`/
   `write` + POSIX socket includes. So the same TU builds on POSIX and Windows with
   **zero platform `#ifdef`**, mirroring how the rest of PAL was done. A latent PAL
   regression was fixed along the way (the vtable moved to `KlSocketHandle` in 6a-2
   but `tls_mbedtls.c` still used `int fd`; uncaught because it's in no CI). The
   Makefile `KEEL_TLS=mbedtls` branch now also links (`-lmbedtls -lmbedx509
   -lmbedcrypto`) and accepts either a source-tree or system-prefix `MBEDTLS_DIR`
   (or default search paths). A real handshake is validated by `make KEEL_TLS=mbedtls
   smoke-tls` (`tests/smoke_tls.c`: loopback mbedTLS server+client, embedded
   self-signed cert via the new `kl_tls_mbedtls_ctx_create_from_buf`). **mbedTLS is
   bring-your-own and stays out of CI** (matching the miniz precedent), the backend
   is validated locally (macOS real handshake + MinGW compile-gate) and documented,
   not gated. Separately, the 3 "TLS" utest suites (`tls`, `tls_integration`,
   `peer_cert`) were found to use an **in-test mock KlTls, not mbedTLS**: they were
   mis-excluded in 6b-3. Ported via `net_compat.h` (a few POSIX-only tests guarded)
   and added to `WIN_TEST_SUITES`, bringing the Windows runner to **47 of 55**
   suites; the excluded set drops to 8 (6 POSIX/Linux-only + 2 runtime-deferred).
4. **Feed back to the vtable.** Confirm `KlSocketHandle` + `writev`/`sendfile`
   ops are sufficient → **unblocks Phase 4** (publish the now-proven vtable).

## Part D: Risks / stop conditions
- **WSAPoll connect-failure defect** (B.2) → bounded deadline + `SO_ERROR`;
  stop & reconsider a `select()`-based backend if it can't be made reliable.
- **Handle truncation**: if any code still assumes `int`-width sockets after the
  sweep, catch it via `-Wconversion` on the Windows build for socket types.
- **pthread/pipe on Windows**: if winpthreads or the loopback-pair wakeup is
  flaky, that gates 6b (thread pool), not 6a.
- **Scope blowup**: "fuller port" is genuinely large; keep each 6b feature its
  own commit, and it is fine to ship 6a (core, vtable validated) and pause: the
  vtable validation, the real objective, is achieved at 6a.
- **Don't drift toward IOCP**, this is readiness-mode Winsock. IOCP is Phase 8
  (completion model, event-axis), explicitly out of scope.

## Part E: What this validates (the objective)
By the end of 6a: a real non-POSIX provider drives Keel's client and server over
WSAPoll, proving (1) the `KlSocketProvider` ops table, (2) `KlSocketHandle` as the
portable handle, and (3) `writev`/`sendfile` as vtable ops. That is exactly the
"proven by a real alternative provider" bar Phase 4 was waiting on, so Phase 4
(public API) executes next, on a shape validated in production, not on paper.
