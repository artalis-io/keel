# KEEL Networking Architecture Axis Audit

> **Historical, append-only evidence log: newest pass first.** Each pass records what was verified
> on its date; verify any specific claim against current code before acting on it. Current-state
> docs: [architecture.md](../../architecture/overview.md), [architecture_invariants.md](../../architecture/invariants.md).
> Index: [audits/README.md](README.md).

## Seventeenth pass: separation holds; one backpressure gap above the axis (2026-10-06)

**Revision:** `main` `03cef23`. Read-only; no source changed.
**Scope:** the event, socket and protocol seams after the completion-queue and backpressure work
(#457 to #459) and the sixteenth pass's fixes (#461). The sixteenth pass's F1 to F4 were re-checked
and hold as described; its I1 (internal `src/socket.h` pulls `sockcompat.h`) is unchanged.

**Verdict:** the three-axis separation holds, and no protocol TU calls an event engine. **One High**
(F1, below), three Lows and three Informational notes. The High is a semantic gap above the axis,
not a coupling: readiness and completion are no longer observably equivalent for TLS WebSocket
backpressure.

### 1. Architecture map (verified)

- **Event axis:** `include/keel/event.h` (`KlEventOps`; a completion-capable provider must supply a
  completion table with `drain`, enforced in `event_dispatch.c:kl_event_init_provider`). Readiness:
  `event_epoll.c` (edge-triggered; every server transition re-arms with `kl_event_mod`),
  `event_kqueue.c`, `event_poll.c`, `event_wsapoll.c`. Completion: `event_iouring.c`, `event_iocp.c`,
  `event_pollcomp.c`. Seam: `src/completion.h` (`KlCompletionOps`, trailing `send_max`: 0 on the
  in-tree backends, 16 KiB on EFI), `completion_dispatch.c`, `completion_core.c:kl_comp_run`.
- **Socket axis:** public `include/keel/socket.h` (`KlSocketProvider`, `KlIoStatus`, optional
  `io_status`); `socket_posix.c`, `socket_winsock.c` (WSA errors mapped to errno); overlapped
  providers in their event TUs; negotiated by `event_ctx.c:kl_event_ctx_sockets_compatible`.
- **Protocol layer:** `http_internal.h` `conn_read`/`conn_write` is the only TLS and transport mux;
  `completion_http_server.c` is the HTTP completion adapter (op counting, the output queue with
  pinned regions, 256 KiB posts, the 1 MiB producer bound, the stream gate); WebSocket and h2
  completion drives come through the atomic hook tables in `http_proto_hooks.c`.

### 2. Traces

- **Readiness receive (epoll):** `kl_http_server_run` → `kl_event_wait` → `kl_event_dispatch` →
  `kl_http_conn_on_readable` → `conn_read` → `kl_stream_recv`; a plaintext -1 is classified by
  `kl_stream_io_status` (WOULD_BLOCK keeps state, INTERRUPTED retries); parse; the transition
  re-arms with `kl_event_mod`.
- **Completion receive (io_uring):** `kl_comp_post_recv` (`comp_ops++`, buffer by phase) →
  `iou_comp_post_recv` → `iou_comp_drain`/`iou_complete` → `kl_comp_run` →
  `comp_server_conn_dispatch` → `comp_on_read` (`comp_ops--`; TLS feeds the engine and drives) →
  the next post, explicit or deferred.
- **Accept:** readiness loops `kl_sock_accept` while the listener is LISTENING (EMFILE disarms with
  back-off) → `kl_listener_on_accepted`; completion primes `comp_accept_listener_start`, posts
  through `comp_accept_arm`, and `comp_on_accept` → `comp_setup_accepted`; a closing listener
  disposes accepted handles.
- **Send and backpressure:** readiness writes until would-block and re-arms WRITE; streams and
  WebSocket buffer in a bounded `KlDrain`. Completion queues (`kl_comp_queue_write`, one send in
  flight, at most min(256 KiB, `send_max`) per post, short writes finished inside the op), with the
  1 MiB producer bound for plaintext streams and WebSocket and the stream gate for TLS streams.
- **Close with outstanding work:** `kl_http_server_conn_release` cancels a suspended op, then (with
  ops posted) sets `comp_closing`, cancels the backend ops and lets the last completion release;
  `kl_http_conn_release` sets `comp_closing` first, runs the protocol cleanups, closes and resets.

### 3. Findings

**F1. High (C): TLS WebSocket output on completion loops is unbounded.** `http_internal.h`
`conn_write` (TLS branch), `completion_http_server.c` `kl_comp_tls_flush`/`comp_tlsq_absorb_ring`.
Principle: goal 8, equivalent backpressure above the axis. #457 made every TLS write move the
engine's ciphertext into the uncapped queue, so `tls->write` never refuses and the WebSocket drain
never buffers; the plaintext WebSocket bound and the TLS stream gate do not cover it. Readiness
refuses at the socket. Scenario: a wss client stops reading during a broadcast and server memory
grows per frame, on every completion backend. Fix: the WebSocket admission check before
`tls->write`, and TLS twins of the WebSocket bound tests. (Also C-audit N2.)

**F2. Low (C): protocol clients classify would-block through `errno`.** `websocket_client.c`,
`http2_client.c`, the DNS TCP fallback. `socket.h` says a consumer never reads a hosted errno; a
provider that implements only `io_status` would see would-block as failure. No in-tree provider is
affected. Fix: `kl_sock_io_status`, with an errno-free mock-provider test.

**F3. Low (C): `send_max` is honoured only by the output queue.** `comp_send_response` posts a
buffered plaintext response directly; on EFI one above 16 KiB fails and closes the connection,
though the same response over TLS succeeds. Fix: route it through the queue when `send_max` is set.

**F4. Low (C): the WebSocket core reads completion-adapter internals** (`http_server_ws.c` reads
`comp_tlsq_*` and chunks on `comp_driven && !tls`). Not engine leakage, but the coupling hid F1.
Fix: exported accessors in the completion adapter.

**Informational.** I1 unchanged. I2: `http_response.c` branches on `KL_SOCK_CAP_OVERLAPPED` to pick
the completion writer (deliberate since #441; a driver-installed writer would remove it). I3: dead
`file_io` writing path and unused `<fcntl.h>` includes in the h2 and WebSocket TUs.

### 4. Protocol independence (mechanical)

`grep -rnE '<sys/epoll\.h>|<sys/event\.h>|io_uring|WSA|OVERLAPPED|epoll_|kevent' src/protocols/`:
24 matching lines, **all in comments, 0 code hits**; none in `http2/`, `websocket/`, `dns/` or
`proxy_protocol/`. Native includes appear only in the platform-service TUs (`dns_sys_win.c`,
`dns_sys_posix.c`, `http_server_plat_posix.c`, `http_server_plat_win.c`). The recent changes leaked
no engine type or call upward; they added Keel-level flags (`comp_driven`, the overlapped capability,
queue fields read by WebSocket).

### 5. Compatibility matrix (CI lanes in `ci.yml` and `soak.yml`)

| Pairing | Implemented | Tested in CI | Status |
|---|---|---|---|
| Linux sockets + epoll | yes | full `make test`, ASan/UBSan, soak | supported default |
| Linux + poll | yes | full suite, soak | supported fallback |
| Linux + io_uring | yes (liburing) | curated `test-iouring`, smokes, ASan smokes, soak | supported; subset; F1 applies |
| Darwin + kqueue | yes | full `make test`, soak | supported |
| Winsock + WSAPoll | yes (MinGW, MSVC) | `test-win` subset, smokes, `test-msvc`, soak | supported; subset |
| Winsock + IOCP | yes (MinGW, MSVC) | `test-win-iocp` subset, smokes, soak | supported; subset; F1 applies |
| pollcomp double | yes | `test-pollcomp`, ASan smokes, soak | test double by design |
| lwIP raw | yes | `loopback-raw`, `loopback-raw-asan` | integration-tested; not production (C-audit N5, N6, N8) |
| UEFI EFI_TCP4/UDP4 | yes | compile and symbol gates only; no firmware run | incomplete as a runtime claim (C-audit N7, F3) |

### 6. Contract summary

1. **Ownership:** an accept or connect transfers exactly one handle; physical close waits for the
   last posted op.
2. **Affinity:** connections, ops and timers belong to one loop thread; hook tables are atomically
   published; cross-thread work goes through `KlWakeup` or the thread pool's `done_fn`.
3. **Readiness:** a notification permits an attempt; WOULD_BLOCK keeps state, INTERRUPTED retries;
   every transition re-arms.
4. **Completion:** one post yields exactly one terminal event; a failed post owns nothing; cancel
   yields a failed completion (the lwIP raw and EFI providers do not yet honour this: C-audit N6, N7).
5. **Lifetime:** application bytes are copied on acceptance; a posted queue region stays pinned
   until it retires.
6. **Cancellation:** a logical close cancels the async op first, then the backend ops.
7. **Timeouts:** timers and the sweep run on the loop thread, serialized with I/O.
8. **Errors:** transient flow uses `KlIoStatus` (F2 lists the stragglers).
9. **Close:** graceful close drains queued output; abortive close cancels.
10. **Backpressure:** writers see would-block past the admission bound under both models; F1 is the
    exception.

### 7. Roadmap

- **Immediate:** F1 (with C-audit N1 to N4).
- **Next:** F2 (`kl_sock_io_status` in the clients), F3 (`send_max` for direct posts), and the lwIP
  raw and EFI provider contract fixes (C-audit N5 to N8).
- **Cleanup:** F4 accessors, I2's driver-installed writer, I3's dead code; I1 stays deferred.
- **Deferred:** an EFI runtime lane; widening the io_uring and IOCP curated suites toward parity.

## Sixteenth pass: separation holds, but readiness error semantics and global installation need hardening (2026-10-06)

### Fix follow-up (2026-10-06, uncommitted)

The discovery record below is preserved as evidence at `da62043`. At the user's subsequent
request, F1-F4 were addressed without changing the architecture or public function signatures:

- **F1:** `http_connection.c` uses the existing provider status seam for negative plaintext header
  and body reads: WOULD_BLOCK retains state; INTERRUPTED retries; EOF/reset retain the terminal
  behavior. `test_http_connection.c` covers both phases, interrupted-then-would-block, no spurious
  body error callback, and terminal EOF/reset. TLS result handling remains unchanged.
- **F2:** `http_proto_hooks.c` publishes/reads all five hook registries atomically, preserving the
  first-table/idempotent/reset policy. `completion_http_server.c` drops the unsynchronized global
  sentinel and uses the now-safe idempotent installers. `test_http_proto_hooks.c` covers concurrent
  publication and reading of all five families. The original race reproducer and both hook tests
  passed under Clang TSan; GCC TSan encountered host address-layout initialization failures.
- **F3:** `event_dispatch.c` requires a completion table with a drain entry after provider init,
  once runtime caps are known, and unwinds through the provider's close hook exactly once on
  failure. Only drain is universal; other entries remain requirements of their consuming feature,
  not a mandatory HTTP/datagram superset. `test_event_provider_vtable.c` covers missing table,
  missing drain, exactly-once close, cleared provider routing, and a working drain-only loop.
  Late model-validation failure is reported as `KL_ERR_EVENT_INIT`; existing pre-init missing
  event-op failures remain `KL_ERR_INVALID_ARG`. The original ASan/UBSan crash probe now prints
  `init=-1` with no sanitizer error (its exit 1 represents the expected rejected configuration).
- **F4:** `streaming.md` distinguishes immediate application-byte copying from backend-owned
  copies or pinned borrowed transport regions. `event.h` removes the obsolete runtime-injection
  restriction and describes the completion install requirement; `completion.h` identifies the
  universal drain requirement. `CHANGELOG.md` records the behavior changes.
- **I1:** deferred informational header-firewall cleanup; no production defect was attributed to
  it and no header-dependency refactor was performed.

Verification: full native readiness `make test` passed; pollcomp ASan/UBSan focused suites
`http_connection`, `http_proto_hooks`, `event_provider_vtable`, `event_provider`,
`read_flow_control`, and `completion_output` passed. No-completion (`KEEL_NO_COMPLETION=1`)
provider-validation and hook suites passed. The freestanding self-contained server archive and
undefined-symbol gates passed for x86-64 and ARM64 using `FREESTANDING_NM=llvm-nm-18` (the host's
default GNU nm cannot read ARM64 COFF). Structural Tier-1/substrate/protocol/integration/pipe seams
and readiness identity gates passed. Windows and native io_uring were not rerun locally; static
analysis tool availability remains as recorded below. No commit or push was performed.

**Revision:** `da62043525e79095a95e05b2bedbe5931c176bde`, clean worktree at entry.
**Scope:** current event/socket/transport/protocol seams, representative HTTP/TLS/WebSocket
paths, native completion lifetime code, runtime provider installation, and existing axis tests.
This is an architecture audit, not an exhaustive security certification or a new production-readiness claim.

**Verdict:** the decomposition remains sound. Native event semantics are preserved and engines do
not parse protocols. However, it is not a clean architectural pass: **two Medium findings, two Low
findings, and one Informational boundary observation** remain. No Critical or High finding was
established in this pass. No production source or test source was changed.

### 1. Current architecture map

- Event interface: `include/keel/event.h`, dispatch in `src/event_dispatch.c`, negotiation in
  `src/event_ctx.c` (`kl_caps_compatible`, `kl_event_ctx_sockets_compatible`), internal queries in
  `src/event_caps.h`. The loop owns opaque backend state and an optional runtime `KlEventOps`.
- Readiness engines: `event_epoll.c`, `event_kqueue.c`, `event_poll.c`, `event_wsapoll.c`.
  Completion engines: `event_iouring.c`, `event_iocp.c`, `event_pollcomp.c` with
  `event_pollcomp_builtin.c` as the compile-time glue. `BACKEND=` selects the built-in engine;
  `KEEL_NO_COMPLETION` selects explicit absent-axis stubs, not a readiness emulation of completion.
- Completion seam: `completion.h` (`KlCompletionOps`, `KlCompletionEvent`),
  `completion_dispatch.c` (per-loop vtable routing), `completion_core.c` (`kl_comp_run`). Generic
  watchers/connects dispatch through the event context; HTTP accept/read/write use
  `ctx->comp_conn_dispatch`; datagram/pipe work uses the per-owner `KlCompLife` dispatch token.
- Socket seam: installed `include/keel/socket.h` (`KlSocketProvider`, `KlSocketOps`, `KlIoVec`,
  `KlIoStatus`), internal `src/socket.h` dispatchers, `socket_posix.c` / `socket_winsock.c` and
  datagram siblings. `KlSocketHandle` is pointer-width, not a universal POSIX `int`.
  Native address/vector translation stays below the protocol layer. Missing socket ops intentionally
  fall back to hosted defaults; a foreign-handle provider must supply the relevant ops.
- Wire-up: `http_server_core.c` and `http_client_async.c` adopt the engine's native overlapped
  provider when the configured pairing is incompatible, then reject any still-incompatible pair.
  This fallback is explicit in code; capability bits establish event-model suitability, not proof
  that arbitrary third-party handle domains agree. Provider authors must supply a coherent pair.
- Transport: `stream.c`, `stream_read.c`, `stream_write.c`, `stream_close.c`, `listener.c`,
  `connect_op.c`. Hooks keep readiness registration separate from completion submission. The write
  facet captures copying versus borrowing with each in-flight operation. Pipe streams are not sockets;
  `pipe_stream.c` reaches the pipe PAL and `completion_pipe.h`, not `KlSocketProvider`.
- Protocols: `src/protocols/{http,http2,websocket,dns,proxy_protocol}`. HTTP owns the TLS-aware
  `conn_read` / `conn_write` adapter and HTTP policy. `completion_http_server.c` selects raw receive
  buffers and accounts for pending operations; engines see `KlStream` and raw buffers, not TLS or HTTP.
  WebSocket and HTTP/2 completion adapters reuse the protocol cores through hook tables.
  Platform services (`http_server_plat_*`, `dns_sys_*`) are intentionally separate implementation TUs.
- Future stacks are not merely hypothetical: lwIP raw completion is standing-CI exercised and EFI
  providers have strict compile gates. No AF_XDP/DPDK/QUIC support is claimed or proposed here.

### 2. Execution-path traces

**Readiness accept and receive (epoll).** `kl_http_server_run` installs the readiness
`KlListener`; listen readiness has NULL event identity. `kl_event_wait_builtin` obtains epoll
notifications; `kl_sock_accept` drains accepts until would-block or pool-credit pause.
`kl_listener_on_accepted` hands ownership plus a `KlSlotLease` to `server_accept_on_accept`.
Connection registration uses `&conn->stream`. A READ notification reaches
`kl_http_conn_on_readable` -> `conn_read` -> `kl_stream_recv` -> provider `recv`.
`kl_http_conn_parse_headers` feeds only bytes after `hdr_parsed`, then shares
`conn_dispatch_request` / `kl_http_conn_ingest_body` with the completion adapter. A partial header
stays READING and the server re-arms READ. **A negative transient receive instead closes: F1.**

**Completion accept and receive (pollcomp; native io_uring corroboration by source).**
`kl_http_comp_run` primes the backend and installs `comp_accept_listener_start` with its returned
window. The listener reserves credit before `comp_accept_arm` posts one accept. Pollcomp tracks
a `PC_ACCEPT`; io_uring submits an ACCEPT SQE. Drain produces a real accepted socket, not a
readability claim. `kl_comp_run` -> `comp_server_conn_dispatch` -> `comp_on_accept` -> shared
listener -> `comp_setup_accepted`. `kl_comp_post_recv` increments `comp_ops`, selects a stable
plaintext window or `comp_cipher`, and rolls back the count if posting fails. Pollcomp keeps
EAGAIN/EINTR pending; io_uring returns a READ completion from its CQE. `comp_on_read` retires the
count, feeds ciphertext through `KlTls` if needed, and invokes shared parser/body functions.
Pause stops the next post; the generic stream facet can hold one already-posted receive undelivered.

**Send and backpressure.** On readiness, response `writev`/file output and WebSocket drain writers
retain partial progress and return pending, and WRITE interest drives the remainder.
`kl_stream_write` atomically reserves a bounded queue and opportunistically writes the prefix;
`kl_stream_flush` drives the rest. Completion HTTP `conn_write` queues plaintext through
`kl_comp_queue_write`, or absorbs TLS output, and `comp_tlsq_kick` posts at most 256 KiB (or the
backend's smaller `send_max`) with at most one send in flight. Pollcomp's `send_done`, io_uring's
`iou_prep_send_tail`, and IOCP's partial-send reissue finish the logical operation before reporting
its WRITE completion. The producer queue/drain then progresses from `comp_tls_on_write`.
The recent WebSocket fix chunks a large plaintext drain into 64 KiB writer calls, so an individual
writer attempt cannot exceed the 1 MiB admission allowance. The generic stream counts both queued
and copying-backend in-flight bytes and wakes a blocked producer only on physical progress.

**Close with outstanding work.** Readiness drops registration before pool release; watcher batches
are bracketed with `kl_event_ctx_dispatch_begin/end`, so deleted watcher nodes cannot be recycled
into captured events. For HTTP completion, `kl_http_server_conn_release` cancels a suspended async
op, marks `comp_closing` while `comp_ops > 0`, requests cancellation, and does not reuse the slot.
`comp_on_read` / `comp_on_write` retire those counts; the last one performs physical release.
The release returns the physical pool slot before releasing its admission lease. Generic stream
close waits for both receive and send retirement; `KlCompLife` pins pipe/datagram memory past
logical close. IOCP teardown cancels and dequeues kernel-owned `OVERLAPPED` records before freeing;
io_uring maintains owned operation records and handles cancel/timeout sentinel CQEs separately.
Timers run on the same loop thread, so timeout-versus-I/O ordering is serialized, not simultaneous.
Loop destruction is not user callback delivery; owners must detach before destroying their loop.

### 3. Findings

#### F1 - Medium: readiness HTTP treats transient receive errors as terminal

**Files/symbols:** `src/protocols/http/http_connection.c`, `kl_http_conn_on_readable`, header read
near line 1045 and body read near line 1115. **Principles:** goals 2, 7, and 9.

Both branches classify every plaintext `nr <= 0` as CLOSED, without consulting
`kl_stream_io_status`. Header reads thus close on EAGAIN/EWOULDBLOCK or EINTR; body reads also invoke
the reader's `on_error`. A readiness notification only permits an attempt, and can be stale/spurious;
a signal can interrupt the read. Neither outcome means the peer died. The provider classification
seam already exists, and completion pollcomp keeps these results pending, so observable lifecycle
semantics differ above the event axis. This is connection loss, not an established memory-corruption
or remote exploit claim.

**Proof:** a no-socket mock `KlSocketOps.recv` returning -1, with `io_status` returning
WOULD_BLOCK or INTERRUPTED, was passed through a real `KlHttpConn` to
`kl_http_conn_on_readable`. Four cases gave:

```text
phase=headers result=EAGAIN before=2 after=10 CLOSED=10
phase=headers result=EINTR  before=2 after=10 CLOSED=10
phase=body    result=EAGAIN before=3 after=10 CLOSED=10
phase=body    result=EINTR  before=3 after=10 CLOSED=10
```

Probe: `/tmp/keel-axis-spurious-read.c` (temporary evidence, not a committed test). It uses a
256-byte read buffer, an otherwise zeroed connection/router, a provider-only receive/status pair,
and both READING/READING_BODY states; no parser is reached on the negative result. Reproduce by
compiling with `-Iinclude -Isrc -Isrc/protocols/http -L. -lkeel -lpthread`.
**Smallest fix:** classify negative results at both sites; retain state for WOULD_BLOCK, retry
INTERRUPTED, and preserve existing terminal/EOF and TLS-WANT semantics. Add mock-provider tests
for both phases, including that a body reader gets no terminal callback for a transient result.
Do not change the public API or rewrite the parser.

#### F2 - Medium: protocol capability installation is not safe across independent loop threads

**Files/symbols:** `http_proto_hooks.c` getters/setters and `hooks_set_once`,
`http_server_core.c:155-157` initialization installers, `completion_http_server.c:988-992`
`comp_hooks_done` and completion installers. **Principles:** goals 1 and 11 (loop-local operation
versus process-wide mutable setup).

The registry's same-table guard enforces identity but does not synchronize access. Setters always
write plain global pointers, including an idempotent reinstall. Concurrent server initialization
therefore races; initializing another server can also write while an existing loop reads a hook.
Two completion loops accepting their first connections race on the plain process-wide
`comp_hooks_done`, then on the completion hook pointers. Single-thread affinity of each individual
loop does not synchronize different loops. Identical pointer values do not make concurrent C
read/write or write/write access defined.

**Proof:** the real `http_proto_hooks.c` was compiled under TSan with a pthread probe that starts
two threads behind an atomic barrier and has each install the SAME static `KlWsCompHooks` table.
The initial PIE run could not start TSan on this host; `-no-pie` outside the sandbox worked:

```text
WARNING: ThreadSanitizer: data race
Write of size 8 ... kl_ws_comp_hooks_set ... http_proto_hooks.c:40
Previous write of size 8 ... kl_ws_comp_hooks_set ... http_proto_hooks.c:40
Location is global 'g_ws_comp_hooks'
SUMMARY: ThreadSanitizer: data race ... kl_ws_comp_hooks_set
```

Probe: `/tmp/keel-axis-hooks-race.c`. Build: `cc -std=c11 -g -O1 -fsanitize=thread -no-pie
-Iinclude -Isrc -Isrc/protocols/http /tmp/keel-axis-hooks-race.c
src/protocols/http/http_proto_hooks.c -lpthread -o /tmp/keel-axis-hooks-race-nopie`.
This is a sanitizer-confirmed setter race plus a source-traced public call path, not an observed
two-server crash. Other hook families have the same implementation pattern.

**Smallest fix:** preserve the existing compiled-in capability registry, but synchronize publication
and reads (including same-table installs and resets), and remove or synchronize the completion
install sentinel. Immutable tables need no broader registry redesign. Test two independent servers
initializing/accepting concurrently under TSan; retain freestanding and static-link behavior.

#### F3 - Low: completion provider installation validates only the readiness-shaped ops

**Files/symbols:** `event_caps.h:kl_event_ops_required_valid`,
`event_dispatch.c:kl_event_init_provider`, `completion_dispatch.c:kl_comp_drain`.
**Principles:** goals 3 and 13.

A runtime provider can supply all seven validated `KlEventOps` entries, advertise
`KL_EVENT_CAP_COMPLETION`, and leave `completion == NULL`. Initialization returns success.
The first `kl_event_ctx_run` then dereferences the absent completion table. A non-NULL table with
a missing required `drain` slot has the analogous unchecked boundary. This is malformed trusted
provider configuration, not network-triggerable with the shipped tables; severity is therefore Low.

**Proof:** `/tmp/keel-axis-malformed-completion.c` installed a no-op event table with completion
caps and no completion table into `kl_event_ctx_init_ex`, then called `kl_event_ctx_run(ctx, 1, 0)`
against the ASan/UBSan pollcomp build:

```text
completion provider with NULL completion table: init=0
completion_dispatch.c:35: runtime error: member access within null pointer ... KlCompletionOps
AddressSanitizer: SEGV ... kl_comp_drain -> kl_comp_run -> kl_event_ctx_run
```

**Smallest fix:** after initialization reveals caps, reject a completion-advertising provider
without its required completion subset and unwind through its close hook. Define conditional
requirements carefully: datagram-only/client-only/autonomous providers must not be forced to
implement unrelated accept/file operations. No public ABI change is needed.

#### F4 - Low: authoritative streaming ownership text overpromises backend copies

**Files:** `docs/contracts/streaming.md`, ownership paragraph; `include/keel/event.h`, completion
capability comment. **Principles:** goals 6 and 14, authoring contract accuracy.

The streaming contract says all three completion backends copy every posted send and no operation
references `KlDrain` memory after return. The HTTP completion queue now explicitly supports a
borrowing provider: `comp_tlsq_pinned`, `comp_tlsq_reserve`, and `comp_tlsq_retire` preserve the old
posted region until its completion. Application bytes are still copied immediately, so the
application lifetime guarantee holds, but the universal backend-copy claim does not. A future
provider author could infer permission to retire queue storage too early. Separately, the event
capability comment says completion is not runtime-injectable, contradicted by the completion
vtable and the runtime-injection smoke.

**Smallest fix:** distinguish immediate application-buffer copying from backend copying/borrowing;
document that borrowed submitted storage is pinned until physical retirement. Correct the obsolete
runtime-injection comment. No behavior change is indicated by this finding.

#### I1 - Informational: protocol isolation is operationally strong, not a header-firewall proof

The direct networking/engine grep found no native event-engine calls in protocol cores. Actual
platform includes occur in `http_server_plat_win.c` and `dns_sys_win.c`: platform-services adapters,
not protocol parsing or transport data-plane implementations. Those are justified exceptions.
However, internal `src/socket.h` still includes `sockcompat.h`, which imports native networking
headers transitively into consumers. The public authoring header remains neutral, and the structural
gates prevent current direct violations, but a passing direct-include grep does not prove protocol
TUs compile without any native header dependency. Consider trimming that transitive include only
as a separately tested small cleanup; no speculative provider architecture is needed.

### 4. Goal coverage and compatibility matrix

| Goal | Assessment at this revision |
|---|---|
| 1. Independent axes | Per-loop vtable routing and separate providers preserved; F2 is unsafe global capability publication, not merged engines. |
| 2. Honest native semantics | Production engines preserve their native model; F1 violates transient readiness semantics above them. |
| 3. Public abstraction | Native engine types remain opaque; F3 weakens runtime completion install validation. |
| 4. Protocol isolation | Core direct-call scans and structural gates pass; justified platform-service TUs and I1 qualify the header claim. |
| 5. Neutral socket contract | Pointer-width handles, neutral addresses/vectors, Winsock error translation; optional native fallbacks are explicit. |
| 6. Ownership/lifetime | Counted HTTP ops, pinned borrowed queues, life tokens, physical-detach close verified by trace and focused tests; F4 is contract drift. |
| 7. Partial I/O | Backend tails/short writes and shared incremental parser checked; F1 is a concrete inconsistency. |
| 8. Backpressure | Generic bounded atomic stream queue, HTTP admission bound/drain, paused receive limits and progress tests exercised. |
| 9. Error normalization | Socket classification seam exists; F1 bypasses it. Completion bytes/ok intentionally coarsen terminal causes, so full native-detail equivalence is not claimed. |
| 10. Cancellation/timeouts | Logical versus physical close and terminal-once discipline preserved; serialized timers and accept-cancel success disposal traced. |
| 11. Thread affinity | Per-loop operations single-threaded; independent loops expose F2. Signal-handler server pointer remains an explicit platform exception. |
| 12. Registration/submission | Distinct watcher registrations and per-op completion records; no artificial unification. |
| 13. Feature/configuration | Backend/absent-axis gates and negotiated socket fallback preserved; F3 is the malformed-table gap. |
| 14. Future providers | lwIP/EFI and non-socket pipes show practical replaceability; exotic data planes still need their own coherent adapters and contracts. |

| Pairing | Implemented/buildable | Evidence | Production maturity |
|---|---|---|---|
| Linux sockets + epoll | Yes | Native focused tests in this pass; standing CI | Supported production backend; operational battle-testing not established here |
| Linux sockets + poll | Yes | Source trace and standing CI; no fresh local poll run in this pass | Supported fallback; same F1 applies |
| Linux sockets + io_uring | Yes with liburing and kernel support | Native op source review; standing CI on audited revision; no local liburing available | Supported production completion backend; fresh local execution not claimed |
| Darwin sockets + kqueue | Yes on macOS | Source/gate coverage and standing CI, not locally rerun on Linux | Supported; platform execution evidence comes from CI |
| Winsock + WSAPoll | Yes on Windows | Native handle/error seam review and standing MinGW/MSVC CI | Supported; F1 is shared above the provider |
| Winsock + IOCP | Yes on Windows | Operation registry/retirement source review and standing CI | Supported; fresh local Windows execution not claimed |
| POSIX sockets + pollcomp | Yes | Five ASan/UBSan smoke programs and focused sanitized tests in this pass | Test double only; not production-ready by design |
| lwIP raw + injected completion | BYO | Standing integration CI, neutral backend/provider seam | Supported integration; not a claim about every embedded deployment |
| EFI TCP/UDP + injected completion | BYO | Strict standing compile gates; prior local firmware execution | Compile-supported; no fresh firmware execution in this pass |
| Named pipes + IOCP | Yes on Windows | Separate transport path and standing CI | Supported non-socket transport; readiness pipes on Windows explicitly unsupported |

The preceding CI run for `da62043` passed all 24 CI jobs plus CodeQL, Benchmark, and Scorecard.
That is existing evidence, not a fresh local execution of foreign platforms. Neither green CI nor
code existence establishes long-running production reliability.

### 5. Contract definition (existing design, no new API)

1. A socket/stream/listener owner belongs to one loop thread. Cross-thread work posts through an
   explicit wakeup/worker handoff; it does not mutate the transport. Global capability publication
   must additionally be synchronized across independent loop threads (F2).
2. Successful accept/connect transfers exactly one handle ownership capability; failure transfers
   none. Admission leases return credits exactly once and only after the physical slot is reusable.
3. READ/WRITE readiness is permission to attempt I/O, not an operation result. Negative transient
   results retain/retry interest without delivering EOF or a terminal body callback (F1).
4. A successful completion submission owns one operation until exactly one terminal delivery or
   explicit teardown retirement. Failed submission owns nothing and emits no later result. Native
   partial progress is internal to the backend's logical op.
5. Caller write buffers may be reused immediately after acceptance; backend-borrowed transport
   storage remains pinned until retirement. Queued and in-flight bytes count toward backpressure.
6. Cancellation requests logical termination, not physical retirement. A success racing cancel is
   still accounted for; successful accepted handles are disposed if the listener is closing.
7. Timeout and completion callbacks serialize on the owner loop. State changes precede callbacks;
   late work cannot resume a recycled slot. No recycling before the final outstanding operation.
8. Read pause reduces readiness interest or stops completion posting, with at most one held posted
   receive. Graceful close drains deliverable output; terminal send failure abandons delivery but
   still waits for physical retirement. Abortive close requests cancellation.
9. Errors use the socket provider classifier for transient control flow. Stream `ok == 0` is a
   coarse terminal condition, not a promise of a detailed native error record. Preserve additional
   native detail where that API actually carries it.
10. Owners detach before loop teardown; freeing a loop is not terminal callback delivery. A runtime
    provider is validated for its advertised event model before the loop is exposed as usable (F3).

Authoritative contracts remain `docs/contracts/stream.md`, `streaming.md`, `async_lifecycle.md`,
`datagram.md`, and the platform-support document; this section is an audit summary, not a replacement.

### 6. Tests, evidence limits, and incremental roadmap

**Fresh passing checks:** `check-sockaddr-neutral`, `check-tier1-boundary`,
`check-substrate-purity`, `check-protocol-no-integration`, `check-integration-seam`,
`check-protocol-home`, `check-pipe-seam`, `check-readiness-identity`, `check-state-dispatch`,
`check-no-eventloop-fd`, `check-completion-lane-parity`, `check-pollcomp-suite-list`,
`check-winsock-init`. All five `make smoke-pollcomp-asan` roundtrips passed (plain HTTP,
TLS, WebSocket, async/thread pool, async client).

Focused pollcomp ASan/UBSan suites passed: `stream_read`, `stream_transport`, `stream_close`,
`stream_writable`, `stream_single_shot`, `listener`, `connect_op`, `read_flow_control`, and
`completion_output` (17 cases). Native epoll suites: `event_ctx` (9 cases), `socket_provider`
(26), `read_flow_control` (2), and `completion_output` (16 passed, one completion-only case skipped).

**Test-selection correction:** a manually selected pollcomp `event_ctx` suite uses readiness-only
`kl_event_wait`; its first assertion failed and early test exit leaked 112 fixture bytes. The
Makefile already excludes this suite from the completion lane for that reason. This was not counted
as a production leak or a passing completion test. Its native epoll execution passed. Socket tests
also initially hit sandbox EPERM; rerunning outside the sandbox passed. Sanitizer runs outside the
sandbox avoided LSan's ptrace restriction. The first TSan PIE probe could not initialize, but the
non-PIE unsandboxed probe confirmed F2.

**Unavailable locally:** `make analyze` could not run because `scan-build` is not installed;
`make cppcheck` could not run because `cppcheck` is not installed. `pkg-config liburing` also
failed, so no fresh local io_uring build/run is claimed. No Windows, macOS, or EFI execution was
performed locally. Existing standing CI covers those supported axes at the audited revision.

**Next incremental work:** fix F1 and F2 separately with narrowly targeted regression tests;
add the conditional completion-provider gate/test for F3; reconcile F4's ownership and injection
text. Prefer a concurrent two-loop TSan regression over another static-only hook review. Preserve
the current transport architecture and consumer-facing contracts. Header-firewall trimming (I1)
is deferred cleanup, not a prerequisite for consumer usage. No broad refactor is recommended.

### 7. Changes made

Only `docs/archive/audits/keel_axis_audit.md` was changed to prepend this evidence log. Temporary
reproducers were created under `/tmp`; generated build/test artifacts were rebuilt. No production
code, public API, committed test source, or build configuration was changed; nothing was committed
or pushed by this audit.

## Fifteenth pass: the transport layer and pipe streams sit above the axes without bending them (2026-09-30)

**Scope:** `d16956d..f0d9085` (18 commits, 95 files, +7991/-496) plus a whole-repo re-check. The
range adds a transport layer above the axes and a second, non-socket kind of stream: the `KlStream`
write/read/close facets (including the writable-again edge `kl_stream_on_writable` and the fix that
stops a graceful close hanging after a terminal write failure), the `KlListener` object handoff
family, Windows Named Pipes on IOCP, anonymous pipe pairs on Windows (completion) and POSIX
(readiness), close-on-exec on Keel's own descriptors, and gates `check-pipe-seam` (R1-R8),
`check-cloexec` and `check-no-dgram-life`.

**Verdict: architecturally sound; two protocol-layer partial-I/O defects and two IOCP op-lifetime
defects found.** The event, socket and protocol axes remain orthogonal and separately replaceable, and
the pipe transport stays off the socket axis in both I/O models. The same-day C audit (fifteenth pass
of `keel_audit.md`) found defects that fall under this audit's goals 6 and 7; they are summarised as
A1-A5 below because they are axis-relevant, not because the separation is violated. A1 is Critical:
HTTP/1 requests split across reads are rejected on **both** event models, identically, which is
"consistent across models" in the worst sense. No fourteenth-pass finding has regressed.

### 1. Architecture map (verified current)

- **Event axis (shape unchanged).** `include/keel/event.h` + `src/event_caps.h` +
  `src/completion.h` / `completion_core.c` / `completion_dispatch.c`. Readiness: `event_epoll.c`,
  `event_kqueue.c`, `event_poll.c`, `event_wsapoll.c`. Completion: `event_iouring.c`, `event_iocp.c`,
  `event_pollcomp.c`. `completion.h` gained `KL_COMP_PIPE_READ/_WRITE/_ACCEPT`, routed by life token
  like the datagram kinds (`completion_core.c:61-75`).
- **Socket axis (unchanged).** `src/socket.h` `KlSocketProvider` with `socket_posix.c` /
  `socket_winsock.c`; the readiness server's accept is close-on-exec (`accept4(SOCK_CLOEXEC)`,
  `kl_sockdef_set_cloexec`, `socket_posix.c:54,132-138`); see A5 for the creators that are not.
- **Transport layer (new, above both axes).** `KlStream` (`stream.c`, `stream_write.c`,
  `stream_read.c`, `stream_close.c`), `KlListener` (`listener.c`, fd or adapter-object handoff),
  `KlConnectOp` (`connect_op.c`). An adapter installs model-specific hooks: readiness =
  `kl_stream_set_writer` + `kl_stream_flush`, `kl_stream_read_init(completion=0, ..., disarm)`;
  completion = `kl_stream_set_submit` + `kl_stream_on_write_complete`, `kl_stream_read_init(completion=1)`.
  The stream machinery names no engine.
- **Pipe transport (new).** `src/pipe_stream.c` is generic substrate (no platform header or type) over
  two seams: `src/platform_pipe.h` (PAL create/open/close; POSIX readiness read/write;
  `platform_pipe_win.c`, `platform_pipe_posix.c`) and `src/completion_pipe.h` (overlapped pipe ops,
  implemented only in `event_iocp.c`; every other engine links `completion_pipe_absent.c` with
  `kl_comp_pipe_available() == 0`). `KlPipeHandle` is an incomplete struct pointer
  (`platform_pipe.h:41`), so it cannot convert implicitly to `KlSocketHandle` or `int`. Lifetime is
  `KlCompLife` (`completion_life.{h,c}`, pinned by `check-no-dgram-life`).
- **Protocol layer (unchanged).** `src/protocols/{http,http2,websocket,dns,proxy_protocol}/` reach I/O
  only through `conn_read`/`conn_write` (`http_internal.h:45`) onto `kl_stream_recv`/`kl_stream_send`,
  or through `KlTls`; `check-pipe-seam` R4 keeps pipe headers out of protocol TUs.

### 2. Execution-path traces

- **Readiness receive (epoll, HTTP).** Registration identity `&conn->stream`
  (`check-readiness-identity`) → `kl_http_conn_on_readable` (`http_connection.c:819`) → `conn_read` →
  `kl_stream_recv` (`:478`) → `parse(read_buf, read_len)` (`:886`) → on INCOMPLETE, state stays READING
  and interest is re-armed. **The re-armed read appends and the parser is fed from byte 0 again: A1.**
- **Completion receive (IOCP, named pipe).** `kl_stream_read_start` → `stream_arm` sets
  `recv_inflight` before the hook → `pipe_arm` (`pipe_stream.c:147`) → `pipe_post(READ)` takes one
  `kl_comp_life_retain` → `kl_comp_pipe_post` (`event_iocp.c:833`) allocates a `KlIocpOp` →
  `iocp_pipe_issue` → `ReadFile(..., &op->ov)`; `ERROR_IO_PENDING` transfers the ref to the op, and a
  synchronous failure is self-queued (`:765-769`), so each accepted post yields exactly one completion
  → drain → `iocp_pipe_complete` (`:775`; zero-byte success re-issued, `ERROR_BROKEN_PIPE` → `ok = 0`)
  → `completion_core.c:66-70` `kl_comp_life_dispatch` → `pipe_dispatch` (`pipe_stream.c:250`) →
  `kl_stream_on_recv` → `pipe_deliver` → `kl_comp_life_release`. The stream re-arms unless paused
  (then the completion is held).
- **Accept, readiness (epoll/WSAPoll).** Listen event with NULL udata → `kl_listener_state ==
  LISTENING` loop (`http_server.c:457`) → `kl_sock_accept` until EAGAIN → `kl_listener_on_accepted`
  → `server_accept_on_accept` commits the pool credit → `kl_event_mod(listen_fd, READ)` unless PAUSED
  (`:489-494`).
- **Accept, completion (IOCP).** `comp_accept_listener_start` (`completion_http_server.c:709`) →
  `comp_accept_arm` → `kl_comp_post_accept` → `comp_on_accept` (`:722`) →
  `kl_listener_on_accept_failed` (credit returned) or `kl_listener_on_accepted` →
  `comp_setup_accepted`. The named-pipe listener rides the same `KlListener` through the object family
  (`listener_arm` posts `KL_PIPE_OP_ACCEPT` on a fresh `KlPipeStream`, `pipe_stream.c:459`;
  `listener_accept_done` → `kl_listener_on_accepted_obj`).
- **Send + backpressure larger than one op.** Completion (pipe): `kl_stream_write` reserves atomically
  (`kl_drain_reserve_buffer`) → `stream_pump_completion` submits the queue (`stream_write.c:99`) →
  `pipe_submit` copies (queue consumed at submit; in-flight bytes still count in
  `kl_stream_write_pending`) → partial `WriteFile` re-issued in `iocp_pipe_complete` until
  `send_done == send_total` → `kl_stream_on_write_complete` pumps the next batch and
  `stream_writable_edge` wakes a producer that saw `WOULD_BLOCK`. Readiness:
  `kl_drain_reserve_write` direct-sends the prefix and queues the rest; the adapter's writer turns a
  short write into write interest; `kl_stream_flush` drains on writability and fires the edge.
- **POSIX readiness pipe write, EPIPE from HUP/ERR delivered as READ (epoll).** `rd_write`
  (`pipe_stream.c:209`) short-writes → `want_write` → `pipe_watch` → `kl_watcher_add(pfd, WRITE,
  pipe_ready)`; the producer gets `WOULD_BLOCK` (edge armed). The child closes its end; epoll reports
  `EPOLLERR`, mapped to `KL_EVENT_READ` (`event_epoll.c:70`) → `pipe_ready` ignores the mask
  (`:226`) and, with `want_write` set, calls `kl_stream_flush` → `kl_plat_pipe_write` → `write_quiet`
  (`platform_pipe_posix.c:124`) blocks SIGPIPE in this thread, gets `EPIPE`, consumes only a SIGPIPE
  it raised → `-1` → sticky `wq_err`, `stream_writable_edge` fires once, the retry returns
  `KL_STREAM_ERROR`, `on_retire` lets a graceful close detach (`stream_close.c:42`) → `pipe_watch`
  deletes the watcher. poll, WSAPoll, the io_uring relay (`event_iouring.c:1021`) and pollcomp
  (`event_pollcomp.c:734`) map HUP/ERR the same way; kqueue reports `EVFILT_WRITE`. Pinned by
  `test_anon_pipe.c` `blocked_writer_peer_gone_wakes_once_errors_and_closes`.
- **Close with outstanding work.** `kl_pipe_free` sets `freed` (silences the consumer) →
  `kl_stream_cancel` → `pipe_cancel_recv/_send` → `CancelIoEx`; each op still completes once → owner
  ref released; the handle closes and memory is freed only at the final release (`pipe_final`,
  `pipe_stream.c:109`). Loop teardown: `iocp_pipe_complete` under `st->quiescing` frees the op without
  an event and releases the ref; `pipe_final` clears `send_inflight` so the queue is not leaked.
  Readiness mode has no op in flight: `pipe_ready` holds a ref across callbacks, `kl_pipe_free` deletes
  the watcher before the fd closes, and `kl_watcher_del` defers the node free during a batch
  (`event_ctx.c:243-265`).

### 3. Findings

**Axis-relevant defects (from the same-day C audit; see `keel_audit.md` fifteenth pass for detail):**

| # | Severity | Files+symbols | Goal | Summary |
|---|---|---|---|---|
| A1 | Critical | `http_connection.c:886`; `completion_http_server.c:468`; `http1_parser_llhttp.c:161` | 7 (partial I/O) | A request whose request line or headers span two reads is re-fed from byte 0 to the stateful llhttp parser: rejected, or parsed as a different header set, or given an out-of-bounds path length. Reproduced on a real `KlHttpServer` over WSAPoll and IOCP. The two models are consistent with each other and both wrong: the partial-input contract was never pinned by a test that completes a split request. |
| A2 | High | `http_server_ws.c:505-513`; `websocket_client.c:690-693` | 7 (partial I/O) | A WebSocket data frame spanning reads is treated as a new message per chunk (server closes 1002; client keeps only the tail). |
| A3 | Medium | `event_iocp.c:259-279` `iocp_watch_reinterest` | 6 (op lifetime) | An interest change while a watcher op awaits re-arm (no kernel I/O) leaks that op until loop close; reached once per async client request. |
| A4 | Medium | `event_iocp.c:1060-1066` `iocp_watch_rearm_dispatched` | 6 (op lifetime) | A failed re-arm leaves an op that neither the delete path nor the quiesce pre-pass frees, so `kl_event_close` waits forever. |
| A5 | Medium | `socket_posix.c:102-104`; `event_iouring.c:662`; `event_pollcomp.c:497`; `event_iocp.c:481` | 13/cross-cutting (resource ownership) | Default-provider sockets and completion-engine accepted sockets are inheritable; #343 covered Keel's internal descriptors only. |

**Architecture findings (this pass):**

| # | Severity | Files+symbols | Principle | Why / failure scenario | Smallest fix |
|---|---|---|---|---|---|
| F1 | Low | `pipe_stream.c:386-390` `kl_anon_pipe_create`; `integrations/platform/lwip/event_lwip.c:184-187` `lwev_caps`; `event.h:79` | Goal 13: never silently select an incompatible combination | `kl_anon_pipe_create` reads `KL_EVENT_CAP_NATIVE_FD` as "watches OS descriptors". The runtime-installed lwIP BSD loop advertises `READINESS \| NATIVE_FD` but polls lwIP socket numbers via `lwip_poll` (its own wakeup comment: "The host self-pipe ... is invisible to lwip_poll"). On a hosted POSIX build with that loop, create returns `KL_PIPE_OK` for a pipe whose readiness is never reported, instead of `KL_PIPE_UNSUPPORTED`. The design doc lists only lwIP-raw and EFI as refused. Unusual combination, no memory-safety impact. | Refuse the readiness path on a runtime-installed loop (`ctx->loop.ops`), as `kl_comp_pipe_available` already does for IOCP; add a refusal test. Dropping `NATIVE_FD` from lwIP is not an option (`kl_event_ctx_sockets_compatible` needs it). |
| F2 | Low | `docs/contracts/stream.md:216`; `stream_write.c:99-114` `stream_pump_completion`, `:172-174` | Contract and implementation must agree | The read side is inline-safe (`stream_arm` sets `recv_inflight` before the hook and tracks `completed_inline`); the write side is not: `send_inflight` is set after `submit_fn` returns, so an inline `kl_stream_on_write_complete` is dropped as spurious and the queue stalls permanently. No shipped backend completes inline (`iocp_pipe_issue` self-queues even synchronous results); the contract is public, and the failure is silent. | Choose: correct the contract to exclude `submit` (and say so on `KlStreamSubmitFn`), or make the pump mirror `stream_arm`. Pin the choice with an inline-completing mock submit. |
| F3 | Informational | `listener.c` `kl_listener_on_accepted` / `_obj` wrong-family branches | Total descriptor ownership | Calling the wrong family's completion retires the accept as failed (credit correct) but neither hands over nor disposes the fd/object; only an adapter bug reaches it. | Document that in the refused case the value stays the caller's to dispose. |
| F4 | Informational | `completion_life.c:13,54`; `completion_life.h:26-28` | Stale-name hygiene | Two comments still describe the token as datagram-specific, and "Owners today" omits the POSIX readiness pipe. | Comment edits. |

**Judged sound (not findings):** the pipe transport stays off the socket axis (no `kl_sock_*` /
`kl_sockdef_*` / provider call in any pipe TU; the one `KlSocketHandle` conversion is
`kl_plat_pipe_pollable`, a watcher registration value; `KlStream.fd` stays `KL_INVALID_SOCKET`; only
`platform_pipe_posix.c`, which owns the fd+1 encoding, is exempt from the no-cast rule). Readiness is
honest (`pipe_ready` reports `kl_stream_on_recv` only after `read()` returned data, EOF or a terminal
error; EAGAIN stays armed). Registration vs submission stays distinct (one persistent watcher per POSIX
pipe stream, changed only when the derived mask changes; one submitted op per IOCP read/write/accept
with its own buffer ownership). `KlCompLife` is balanced on every post, failed post, dispatch and
quiesce. Support selection is decided before any OS call and observable as `KL_PIPE_UNSUPPORTED`
(apart from F1): IOCP when the compiled-in engine has no runtime `loop.ops`, otherwise POSIX plus
`NATIVE_FD`; Windows WSAPoll refused because `kl_plat_pipe_readiness()` is 0.

### 4. Compatibility matrix

| Combination | TCP / HTTP stack | Pipe transport | Evidence |
|---|---|---|---|
| Linux sockets + epoll | tested; **A1 affects production** | anon pair (readiness): tested | CI Linux epoll, musl, ASan+UBSan (PR #346 head `d8b7bb4`, tree identical to `f0d9085`) |
| Linux sockets + io_uring | tested; A1 affects it | anon pair via watcher relay: tested | CI io_uring unit suite + sanitized io_uring |
| Darwin sockets + kqueue | tested; A1 affects it | anon pair (readiness, `F_SETNOSIGPIPE`): tested | CI macOS kqueue, pollcomp on macOS |
| Winsock + WSAPoll | tested; A1 **reproduced** | unsupported by design | CI WSAPoll archive, MinGW, MSVC; local A1 repro |
| Winsock + IOCP | tested; A1 **reproduced**; A3/A4 | named pipe client + listener + anon pair: tested | CI Windows IOCP; local A1 repro |
| pollcomp double | test instrument; tested | anon pair via relay: tested | CI sanitized pollcomp, pollcomp ubuntu + macOS |
| poll fallback | tested | anon pair: tested | CI poll fallback, no-completion |
| Cosmopolitan | builds; smoke only | known defect on Windows hosts (full pipe reported writable), not exercised in CI | CI library + smoke; `test_anon_pipe.c` tracked case |
| runtime lwIP BSD loop | integration; tested | incorrectly accepted (F1) | static trace |

Given A1, no HTTP/1 server combination should be called production-ready until it is fixed and pinned
by a split-request test on both models.

### 5. Mechanical evidence

- **Protocol independence.** `src/protocols/**` grepped for `<sys/epoll.h>`, `<sys/event.h>`,
  `io_uring`, `WSA`, `OVERLAPPED`, `epoll_`, `kevent`, `<winsock2.h>`, `<sys/socket.h>`: 21 hits in 9
  files, 20 of them comments; the one include is `dns_sys_win.c:13 <winsock2.h>`, the sanctioned
  platform-sibling TU behind `dns_sys.h`. No protocol core TU includes a platform networking header or
  calls an engine.
- **Pipe TUs vs the socket axis.** All 9 pipe TUs grepped for `kl_sock_`, `kl_sockdef_`,
  `KlSocketProvider` calls, `sockets->`, `WSA`, `closesocket`: zero code hits.
- **Gates.** `check-pipe-seam` R1-R8 and `check-cloexec` are self-canaried and green in CI Static
  Analysis; `check-cloexec` does not cover socket creators (A5).

### 6. Limits of this pass

Read-only static tracing on a Windows host, plus the local A1 reproduction (parser probe and an
end-to-end server run on WSAPoll and IOCP). io_uring and the Apple `container` VM were not available;
Linux/macOS/io_uring/pollcomp execution evidence is CI's. The Cosmopolitan-on-Windows full-pipe defect
was not reproduced here. F1 is inferred from code and the integration's own comment; no test drives the
lwIP BSD provider with a pipe pair.

### 7. Recommended roadmap

1. **Immediate correctness:** A1 (parse only new bytes; completion grow path resets parser + request;
   split-request test at many split points on both models), then A2, A3, A4.
2. **Hygiene with security weight:** A5 (close-on-exec at every socket creator; extend `check-cloexec`).
3. **Contract clarity:** F2 (pick one rule, pin it), F1 (refuse runtime loops for readiness pairs),
   F3/F4 comment edits.
4. **Test-coverage lesson:** both A1 and A2 are partial-input defects that loopback tests never split.
   A small helper that delivers a byte stream in configurable fragments, used by the HTTP/1 and
   WebSocket suites on both event models, would have caught both.

### 8. Changes made

None (read-only pass).

## Fourteenth pass: post-MSVC-coverage-campaign whole-repo re-audit; three-axis separation holds (2026-09-15)

**Verdict: architecturally sound.** The event / socket / protocol axes remain genuinely orthogonal and
separately replaceable. Trigger: the MSVC coverage campaign (#313, #315, #317, #318, #319, #320), which
took the second Windows toolchain from 3 to 101 of 102 semantic suites and fixed the installed artifact.
That campaign touched the PAL, the test harness and packaging, so this pass asks whether any of it leaked
an event model or a platform assumption upward. It did not. **The thirteenth pass's F1 (oversized-header
divergence between the two event models) is now FIXED.** One new Low finding, on handle-validity idiom in
the protocol layer. All **ten** axis-boundary gates pass (the thirteenth pass cited six; four have been
added since).

### 1. Architecture map (verified current)

- **Event axis:** `include/keel/event.h` (readiness) + `src/event_caps.h` (negotiation) +
  `src/completion.h` + `src/completion_core.c` + `src/completion_dispatch.c` / `src/completion_io.h`.
  Readiness backends `event_epoll.c` / `event_kqueue.c` / `event_poll.c` / `event_wsapoll.c`; completion
  backends `event_iouring.c` / `event_iocp.c` / `event_pollcomp.c` (+ `event_pollcomp_builtin.c`). Two
  neutral fillers exist for builds that omit an axis: `completion_absent.c` and
  `completion_readiness_stub.c`. The retired `io_engine` TU is gone from the tracked tree, and unlike the
  thirteenth pass no stale object lingers (that pass's F2 is closed).
- **Socket axis:** `src/socket.h` (`KlSocketProvider` vtable + `KL_SOCK_CAP_*`), providers
  `socket_posix.c` / `socket_winsock.c`, datagram data-plane siblings `socket_dgram_posix.c` /
  `socket_dgram_win.c`; overlapped providers live in their event TUs. Handle type `KlSocketHandle`
  (`include/keel/handle.h`, pointer-width `intptr_t`, `KL_INVALID_SOCKET`, `kl_handle_valid()`).
- **Protocol layer:** `src/protocols/{http,http2,websocket,dns,proxy_protocol}/`, reaching I/O only
  through `conn_read` / `conn_write` (`http_internal.h`) onto `kl_stream_recv` / `kl_stream_send` or the
  `KlTls` vtable.

### 2. Execution-path traces

**Readiness receive (epoll / WSAPoll).** Registration uses `&conn->stream` as the udata identity
(`http_server_core.c:731,741,744`, enforced by `check-readiness-identity`). The engine reports readiness,
`kl_http_conn_on_readable` calls `conn_read` (`http_connection.c:849`), which resolves to
`c->tls->read(...)` or `kl_stream_recv(&c->stream, ...)`. Bytes land in the sliding `read_buf` window;
interest is re-armed with `kl_event_mod(..., KL_EVENT_READ, &c->stream)`. The protocol code names no
engine and no platform type anywhere on this path.

**Completion receive (io_uring / IOCP / pollcomp).** `kl_comp_post_recv`
(`completion_http_server.c:76`) chooses the destination buffer (the TLS cipher buffer when `c->tls`,
otherwise the `read_buf` headroom) and submits through `kl_comp_post_recv_raw(&c->stream, ...)`. On
completion the driver feeds the SAME model-blind core the readiness path uses:
`kl_http_conn_dispatch_request`, `kl_http_conn_ingest_body`, `kl_http_conn_send_complete`
(`http_conn_internal.h:186,194,216`), whose definitions are thin non-static handles onto the identical
statics (`http_connection.c:1100-1116`). This is the load-bearing property of the whole design: one
parse/route/handle/lifecycle core, two submission models, no event model in the protocol TU.

**Send with backpressure.** `conn_write_all` (`http_internal.h:56`) retries short writes with a bounded
spin (`KL_HTTP_CONN_WRITE_SPIN_MAX`) rather than assuming one write drains the buffer; the completion
side posts through `kl_comp_post_send(c, iov, iovcnt, total)` and retires on the completion. Both models
treat a partial transfer as normal rather than exceptional.

**Close with outstanding work.** `http_server_core.c:463` documents that teardown DEQUEUES every
completion before freeing, so no `OVERLAPPED` is released while the kernel still owns it. The
post-rejection drain (`http_connection.c:1125`) half-closes and performs a bounded, non-blocking drain so
that a committed final response is not destroyed by an RST caused by unread request bytes.

### 3. Findings

| # | Severity | Files+symbols | Principle | Why / failure scenario | Smallest fix |
|---|----------|---------------|-----------|------------------------|--------------|
| F1 | Low | `src/protocols/http/http_server_core.c:162` (`if (s->config.listen_fd < 0)`); field `KlHttpServerConfig.listen_fd` (`include/keel/http_server.h:147`, type `KlSocketHandle`) | Goal 5: the socket abstraction must not carry POSIX assumptions that make Winsock second class. `include/keel/handle.h:24-27` states the rule outright: because a Winsock `SOCKET` is UNSIGNED, the POSIX idiom `if (fd < 0)` is unreliable on Windows; test validity with `kl_handle_valid()`, never `< 0`. | Protocol-layer code applies the forbidden numeric idiom to a handle-typed field, and the in-code rationale is POSIX-specific ("fds 0-2 are stdio, not listeners") used to justify `0 = disabled` on a cross-platform field. No failure today: Windows kernel handles are small positive values, so only `KL_INVALID_SOCKET` trips the test, and the documented absent value is `0`. The wart is that a handle-typed field uses an integer sentinel whose justification does not hold on one supported platform, and a caller who reasonably passes `KL_INVALID_SOCKET` to mean "none" receives `KL_ERR_INVALID_ARG` instead of "disabled". | Either document on the field that `0`, not `KL_INVALID_SOCKET`, is the absent sentinel and that the check is input validation rather than handle validation, or accept `KL_INVALID_SOCKET` as equivalent to `0`. Prefer the former: it is a comment change. Consider extending `tests/test_fd_type_convention.c`, which already pins the file-descriptor vs socket-handle distinction, to pin this sentinel too. |
| F2 | Informational | `src/platform_thread_posix.c:28`, `src/platform_thread_win.c:35` | Allocator discipline (CLAUDE.md: no direct malloc/free; all allocation through `KlAllocator`). | The PAL thread trampoline heap-allocates its `{fn, arg}` carrier with raw `malloc`/`free`. The comment explains why it must be heap-allocated (it outlives the call) but not why it bypasses the allocator. `kl_plat_thread_create()` takes no allocator, so the bypass is structural and defensible; it is simply the only one outside `allocator_default_stdlib.c`, and it is unstated. | One sentence in `src/platform_thread.h` recording that the seam deliberately sits below the allocator. Also raised as L1 in the companion `/c-audit` pass of the same date. |

**Thirteenth-pass F1 is RESOLVED.** `comp_grow_headers_or_431()` (`completion_http_server.c:489`) now
grows the completion read buffer up to `max_header_size` and best-effort posts the same
`KL_HTTP_431_RESPONSE` before closing, mirroring readiness (`http_connection.c:828-840`). The residual
`if (space == 0) return -1;` at `completion_http_server.c:84` is a defensive guard below that helper, not
the divergence. Both models now honor `max_header_size`, and both answer 431 rather than a bare TCP
close.

### 4. Compatibility matrix

| Combination | Status | Evidence for this pass |
|---|---|---|
| Linux sockets + epoll | production; tested | CI `Linux (epoll)`, `Linux (poll fallback)`, `ASan + UBSan` |
| Linux sockets + io_uring | production; tested | CI `Completion (io_uring)`, `Completion (io_uring) unit suite`, `Sanitized completion (io_uring)`. **Not run in this pass** (Linux only; this host is Windows) |
| Darwin sockets + kqueue | production; tested | CI `macOS (kqueue)`, `Completion (poll) (macos-latest)` |
| Winsock + WSAPoll | production; tested | **Run in this pass: 102 suites green** (`make test-win`) |
| Winsock + IOCP | production; tested | **Run in this pass: 97 suites green** (`make BACKEND=iocp test-win-iocp`) |
| pollcomp double | test instrument; tested | CI `Sanitized completion (pollcomp)`, `Completion (poll)`. **Not run in this pass** (POSIX only) |
| Winsock + MSVC toolchain, both backends | production; tested | 101 of 102 (WSAPoll) and 96 of 97 (IOCP) semantic suites under `cl.exe`; one documented exclusion, an MSVC 19.44 C1001 compiler ICE |

### 5. Mechanical evidence

- **Protocol independence (Goal 4):** zero occurrences of `<sys/epoll.h>`, `<sys/event.h>`,
  `<liburing.h>`, `io_uring_*`, `epoll_*`, `kevent(`, `WSAPoll`, `OVERLAPPED`, `CreateIoCompletionPort`,
  `WSARecv` / `WSASend`, `<winsock2.h>`, `<windows.h>`, `<sys/socket.h>` or `<netinet/*>` in the protocol
  CORE TUs. Every apparent hit is either a comment or one of exactly four platform-sibling TUs
  (`http_server_plat_posix.c` / `http_server_plat_win.c`, `dns_sys_posix.c` / `dns_sys_win.c`) that
  implement a type-neutral seam header, the same one-platform-per-TU pattern as `event_*.c` and
  `socket_*.c`. Measured directly: `http_server.c`, `http_server_core.c`, `http_connection.c`,
  `http2_server.c`, `websocket.c` and `dns_resolver.c` contain **zero** platform includes and **zero**
  `#ifdef _WIN32` / `__linux__` / `__APPLE__`.
- **Native error access:** no `WSAGetLastError()` outside the platform TUs; `src/sockcompat.h`
  translates into the CRT `errno` space at the boundary.
- **Handle typing:** `tests/test_fd_type_convention.c` pins the distinction that a FILE descriptor is an
  `int` while a socket handle is a `KlSocketHandle`, which is why `kl_http_response_file(..., int fd,
  ...)` is correct rather than an oversight.
- **Axis gates:** all ten pass: `check-substrate-purity`, `check-protocol-no-integration`,
  `check-integration-seam`, `check-protocol-home`, `check-sockaddr-neutral`, `check-readiness-identity`
  (plus its selftest), `check-backend-isolation`, `check-no-eventloop-fd`, `check-tier1-boundary`.

### 6. Limits of this pass

Run on a Windows host, so the io_uring and pollcomp lanes were **not executed here**; the matrix cites CI
for those and says so rather than implying local verification. The skill's suggested Apple `container`
Linux VM was not available. `make cppcheck` and `make analyze` were likewise not run locally; they run in
CI's `Static Analysis` and `Analyze` jobs, both green on `main`.

### 7. Recommended roadmap

1. **Immediate correctness:** none. Neither finding is a correctness defect.
2. **Cheap clarity:** the F1 comment on `KlHttpServerConfig.listen_fd`; the F2 sentence in
   `src/platform_thread.h`.
3. **Test coverage:** extend `tests/test_fd_type_convention.c` to pin the `listen_fd` absent-sentinel
   convention, so the POSIX-flavoured idiom cannot spread by imitation.
4. **Deferred:** nothing architectural. The axes held under the heaviest recent pressure (a second
   toolchain, a PAL thread migration, and a packaging fix), which is the useful signal from this pass.

### 8. Changes made

None. This pass is read-only: no source file was modified.

## Thirteenth pass: post-AF_UNIX-node-cleanup (#251) whole-repo re-audit; three-axis separation holds (2026-08-26)

**Verdict: architecturally sound.** The event / socket / protocol axes remain genuinely orthogonal
and separately replaceable. Trigger: the AF_UNIX node-lifecycle hardening merged at `4a5ac7e` (PR
#251); a fresh whole-repo `/axis-audit`. One Low functional-divergence finding between the two event
models (oversized-header handling); no architectural coupling introduced. All six axis-boundary gates
pass (`check-substrate-purity`, `check-readiness-identity`, `check-sockaddr-neutral`,
`check-protocol-home`, `check-protocol-no-integration`, `check-integration-seam`).

### 1. Architecture map (verified current)

- **Event axis:** `include/keel/event.h` (readiness) + `src/event_caps.h` (negotiation) +
  `src/completion.h` + `src/completion_core.c` (platform-independent completion + generic driver) +
  `src/completion_dispatch.c` / `src/completion_io.h` (run-loop seam). The old `io_engine` is retired:
  no `src/io_engine.c`/`.h` is git-tracked (a stale `io_engine.o` build artifact remains until
  `make clean`; F2). Backends present and `BACKEND=`-selected: readiness `event_epoll/kqueue/poll/
  wsapoll.c`; completion `event_iouring.c` (SQE/CQE), `event_iocp.c`, `event_pollcomp.c` (poll double).
- **Socket axis:** `src/socket.h` (`KlSocketProvider` vtable + `KL_SOCK_CAP_*`), providers
  `socket_posix.c` / `socket_winsock.c`; overlapped providers live in their event TUs. `KlSocketHandle`
  (pointer-width, `KL_INVALID_SOCKET`, `kl_handle_valid`). Datagram data plane rides the provider's
  `dgram` op (`socket_dgram_{posix,win}.c`, `udp_cmsg{,_win}.c`), model-blind.
- **Protocol layer** (`src/protocols/`): `http_connection.c` (+ model-blind core
  `kl_http_conn_on_readable`/`_dispatch_request`/`_ingest_body`/`_send_complete` in
  `http_conn_internal.h`), `http2_server.c`, `websocket.c`, the `http_client_*`/`http2_client.c`/
  `websocket_client.c` TUs, `http_sse.c`, `http_response.c`, body readers, the `KlTls` vtable. These go
  through `conn_read`/`conn_write` + the socket seam and never call an event engine.
- **Negotiation:** `kl_event_ctx_sockets_compatible()` (`src/event_ctx.c`): a completion loop requires
  an overlapped provider; a readiness loop requires a native-fd provider.

### 2. Execution-path traces (real names)

- **Readiness receive** (epoll/kqueue): `kl_event_wait` reports READ against `&conn->stream` (the
  `check-readiness-identity` gate enforces registrations use `&conn->stream`) -> dispatch ->
  `kl_http_conn_on_readable` -> `conn_read` (socket seam `kl_sock_recv` or `KlTls->read`) into
  `stream.read_buf` -> llhttp -> `kl_http_conn_dispatch_request` / `_ingest_body` -> handler ->
  `kl_http_response_*` -> `conn_write`; EAGAIN re-arms interest via `kl_event_mod`.
- **Completion receive** (io_uring/pollcomp/IOCP): `kl_comp_post_recv(c)` computes
  `space = read_cap - read_len` and calls `kl_comp_post_recv_raw(&c->stream, buf, space)` (the backend
  sees only a `KlStream`, never an HTTP type) -> backend submits (`io_uring_prep_recv` / `WSARecv` /
  pollcomp poll, keyed on the stream token) -> drain (`kl_comp_run` -> `completion_dispatch.c`) maps
  the CQE/packet to a `KlCompletionEvent` -> `completion_http_server.c` recv handler ->
  `kl_http_conn_ingest`/parse -> `comp_send_response` -> `kl_comp_post_send_raw`; the next recv is
  posted, or parked when `stream.read_paused` (read-side flow control).
- **Accept:** readiness -> listener readable -> `kl_sock_accept` loop -> conn acquire -> register
  `&conn->stream`. completion -> `kl_comp_prime_accepts_raw`/`kl_comp_post_accept_raw`
  (io_uring ACCEPT / AcceptEx) -> completion delivers the new fd -> conn acquire -> `kl_comp_post_recv`.
- **Send + backpressure:** readiness -> `conn_write` partial -> EAGAIN -> `KlDrain` buffers the
  remainder + re-arms WRITE -> flush on writable. completion -> `kl_comp_post_send_raw` submits the
  whole iov; a partial send re-preps the tail (`send_done`/`send_total`); backpressure is the bounded
  send queue + not posting the next recv until drained.
- **Close with outstanding work:** readiness -> `kl_event_del` + `kl_sock_close`; stale post-close
  events guarded by slot generation. completion -> `kl_comp_close` -> conn release; outstanding ops
  handled by io_uring `queue_exit`-before-free (kernel drops in-flight), IOCP port-quiesce +
  deliberate leak of still-kernel-owned ops (avoids UAF), and the datagram close coordinator's
  retire/cancel with the backend-owned life token. Exactly-one-terminal per op.

### 3. Findings

| # | Severity | Files+symbols | Principle | Why / failure scenario | Smallest fix |
|---|----------|---------------|-----------|------------------------|--------------|
| F1 | Low | readiness `http_connection.c:796-806` (grow to `max_header_size`, emit `kl_431_response`) vs completion `completion_http_server.c:84` (`space==0 -> -1 -> kl_comp_close`) and `:503` (`off>=read_cap -> kl_comp_close`) | Goal 7/8: readiness and completion must not produce observably different protocol semantics for the same input. | Oversized request headers are handled differently by event model: the readiness path grows the header buffer up to the configured `max_header_size` and returns a proper `431 Request Header Fields Too Large`; the completion path uses the fixed `KL_HTTP_CONN_READ_BUF_SIZE` (8 KB) read buffer (no growth) and closes the connection with no 431. Two observable differences: (a) 431 vs bare TCP close; (b) a `max_header_size` configured above 8 KB is honored on readiness but silently capped at 8 KB on any completion backend. Both models still reject oversized input safely (no memory-safety impact); the divergence is functional/config. | Align the completion path: on header-buffer overflow, best-effort post the same `kl_431_response` before close (mirrors readiness), and either grow the completion read buffer to `max_header_size` or document that `max_header_size` > `KL_HTTP_CONN_READ_BUF_SIZE` is a readiness-only knob. Needs a small design decision (grow vs document) before coding. |
| F2 | Informational | stale `src/io_engine.o` (no git-tracked source) | Build hygiene, not architecture. | The `io_engine` translation unit is retired (no `.c`/`.h` tracked), but a stale object file lingers in a dirty tree and can confuse a grep/`ls`. | `make clean` (removes it); no source change. |

### 4. Compatibility matrix

| Combination | Implemented | Buildable | Tested | Production-ready | Notes |
|---|---|---|---|---|---|
| Linux sockets + epoll (readiness) | yes | yes | yes (`make`, full suite) | yes | default on Linux |
| Darwin sockets + kqueue (readiness) | yes | yes | yes (`make`, full suite) | yes | default on macOS |
| POSIX + poll (readiness fallback) | yes | yes | yes (`BACKEND=poll`, CI) | yes | universal fallback |
| Linux sockets + io_uring (completion) | yes | yes | yes (container `test-iouring`, ASan/LSan) | yes | completion-native SQE/CQE + splice |
| Winsock + WSAPoll (readiness) | yes | yes | yes (CI Windows) | yes | |
| Winsock + IOCP (completion) | yes | yes | yes (CI Windows IOCP + smoke) | yes | overlapped provider |
| pollcomp double (completion) | yes | yes | yes (`BACKEND=pollcomp`, ASan) | n/a (test/CI double) | portable completion driver under ASan |
| lwIP raw (completion provider) | yes | yes | yes (loopback-raw ASan/UBSan/LSan) | integration | runtime-injected provider |
| EFI_TCP4/UDP4 (UEFI completion) | yes | yes | yes (host-mock + QEMU/OVMF) | integration | freestanding provider |

### 5. Contract (unchanged from prior passes; re-affirmed)

Socket ownership: the provider owns the native handle; the protocol layer holds only a `KlStream` +
`KlSocketHandle`. Event-loop affinity: one loop, one thread; the thread pool bridges blocking work via
pipe + watcher. Readiness = register interest / wait / op / EAGAIN / re-arm; completion = submit /
track / drain / interpret / retire, with exactly-one-terminal and post-close stale events discarded by
generation (readiness) or queue-exit/quiesce (completion). Error normalization: platform errors ->
`kl_sock_io_status` categories through the seam. Backpressure: readiness reduces interest + `KlDrain`;
completion bounds the send queue + pauses recv posting. Cancellation/close: `kl_comp_cancel` marks
aborted + posts a cancel (completion) or `kl_event_del` (readiness); the datagram coordinator defers
the destructive terminal to the outermost frame.

### 6. Recommended roadmap

1. Immediate: resolve F1 (decide grow-vs-document for `max_header_size` on completion; mirror the 431
   on overflow). Small, contained, improves cross-model consistency.
2. Test coverage: add a deterministic oversized-header test asserting identical observable behavior
   (status + close) under readiness and the pollcomp double, to lock the F1 contract once chosen.
3. Hygiene: `make clean` to drop the stale `io_engine.o` (F2).
4. No architectural change warranted; the three-axis design is intact and the recent socket-axis
   additions (AF_UNIX node cleanup, datagram data plane) exemplify rather than erode it.

### 7. Changes made

None (report-only pass). F1 carries a design decision (grow vs document) that should be made before
coding; F2 is a `make clean`. The AF_UNIX node cleanup merged in #251 is pure socket-axis transport
code invoked through the neutral `unix_socket_node.h` contract by the `_plat_` adapters, so it adds no
protocol-layer coupling: `http_connection.c` and the other protocol TUs remain free of platform
networking headers and event-engine calls (verified by grep and the six axis gates).

## Twelfth pass: the public `KlDatagram` STABLE facade (datagram Phase B) preserves the three-axis separation (2026-08-17)

**Verdict: architecturally sound, the datagram transport consolidation (Phase B, Step 7B) added
the largest new axis surface since the eleventh pass and it EXEMPLIFIES the three-axis design
rather than eroding it.** Fresh `/axis-audit` over the merged branch (`186691f`, now in
`origin/main` as PR #240 + dependabot follow-ups). The new public `kl_datagram_*` API is a pure
seam-level adapter that runs, model-blind, over all five completion/readiness backends.

### Audit target

- Working tree = `transport/datagram-phase-b` @ `186691f`, confirmed an ancestor of `origin/main`
  (merged). This is the shipped tree.
- New since the eleventh pass: the whole Phase B datagram arc; `KlUdp` → `KlUdpServer` →
  built-in `dns_resolver` → the internal `KlDgramCore` → the **public `KlDatagram` fixed-slot
  facade** (`include/keel/datagram.h`, `src/datagram.c`) wired live to pollcomp, io_uring,
  readiness (epoll/kqueue/poll), IOCP, and lwIP-raw + EFI_UDP4.

### Mechanical independence (Goal 4): PASS

- **Protocol + datagram TUs name no platform networking header or event-engine symbol.** grep over
  `connection.c`, `h2.c`, `websocket.c`, `client.c`, `h2_client.c`, `websocket_client.c`, `sse.c`,
  `response.c`, `router.c`, `server.c`, `server_core.c`, `udp_server.c`, `dns_resolver.c`,
  `parsers/*` for `<sys/epoll.h>`/`<sys/event.h>`/`<liburing.h>`/`<winsock2.h>`/`io_uring_`/
  `epoll_*`/`kevent`/`WSA*`/`CreateIoCompletionPort`/`OVERLAPPED` → **empty**.
- **The new facade `src/datagram.c` is seam-only.** Its includes are exclusively Keel headers
  (`keel/datagram.h`, `keel/datagram_detail.h`, `keel/event_ctx.h`, `keel/event.h`,
  `datagram_core.h`, `completion.h`, `io_engine.h`, `socket.h`, `event_caps.h`, `datagram_life.h`,
  `<string.h>`). It selects completion-vs-readiness purely on `kl_event_caps(...) &
  KL_EVENT_CAP_COMPLETION`; the backend mechanisms (`CreateIoCompletionPort`, io_uring inert
  `kl_event_add`) appear only in *comments*, never as code.
- **`make check-sockaddr-neutral` → OK (16 protocol TUs are KlSockAddr-only).** The address ABI
  seam remains intact across the datagram additions.

### The datagram facade is a textbook axis adapter (Goals 1–3, 7, 8, 12)

`src/datagram.c` holds **two adapter tables selected by capability**, both feeding the *same*
model-blind `KlDgramCore` state machine (`kl_dgram_core_recv_on_complete` /
`kl_dgram_core_send_on_complete` / inbound-slot), i.e. semantic consistency ABOVE the axis,
native mechanism BELOW:

| Concern | Completion (`dg_comp_*`) | Readiness (`dg_rdy_*`) |
|---|---|---|
| Send | build by-value `KlDgramSendOp` → `kl_comp_post_dgram_send` (retain-transfer-on-success / release-on-failure) → `INFLIGHT` | `dg_ops->send` (all-or-nothing) → `DONE`, or `WOULD_BLOCK`→`WOULDBLOCK` |
| Recv arm | post `KlDgramRecvOp` (inbound slot buffer) → completion routed by the B.6 token to `kl_datagram_comp_dispatch` | `want_mask |= READ`; `dg_reconcile` one watcher; `dg_rdy_pull` a delivered slot |
| Backpressure | interest rides the posted op (no watcher) | `dg_reconcile_write` toggles the WRITE bit from send-queue depth |
| Registration vs submission | one op = one in-flight submission (Goal 12 preserved) | one persistent watcher spanning many ops |

Completion never dereferences a transport object, the neutral `KlDgramSendOp`/`KlDgramRecvOp`
descriptors (7B-2b) carry everything by value; the B.6 token routes each completion back to the
live `KlDgramCore` (NULL once dead) so a stale wrapper is never touched.

### Operation lifetime + ownership (Goal 6): the invariant that got amended this arc

- **`KlCompletionEvent.retain_life` is single-sourced across all three release sites**:
  `completion_core.c` (router no-handler fallback), `datagram.c` (`kl_datagram_comp_dispatch`),
  `udp.c` (`kl_udp_comp_dispatch`) each release the borrowed life ref *iff* `!retain_life`. This
  is a deliberate, documented amendment to the completion release invariant so the EFI QUARANTINE
  case (an abandoned firmware RxToken that may still write the inbound buffer) can keep its ref
  fail-closed while every other backend passes `retain_life=0` (memset default) unchanged.
- **Retain-transfer-on-success / release-on-failure** is applied uniformly in both `dg_comp_submit`
  and `dg_comp_arm`, the op takes the ref only when the post succeeds.
- **A real operation-lifetime bug was found and fixed this arc (IOCP):** a send-only `KlUdp`
  socket (`kl_udp_send_to` with no `kl_udp_recv_start`) was never associated with the completion
  port, so its `WSASendTo` completion never posted and teardown hung forever. Fixed by associating
  at `kl_udp_init()` (completion mode), covering send-only + recv, the general lesson (a
  completion socket must be port-associated at CREATE, not lazily at first-recv) is exactly the
  Goal-6 "close-while-outstanding / shutdown ordering" class this audit targets, and it is now
  proven on real Windows IOCP CI.

### Validation run this pass

- **Native completion-driver double**, `make smoke-pollcomp-asan`: GET/POST/file/stream/UDP/h2c/
  WebSocket/async/client/TLS over the `poll()` completion facade, **ASan+UBSan+LSan clean**.
- **Production completion backend**, in the Apple `container` Linux VM (kernel 6.18):
  `make smoke-iouring-asan` → **ASan/UBSan clean** (GET/POST/file/bigfile-splice/stream/astream/
  bigstream/UDP/h2c/h2-pk/idle/keepalive/resilience/large + async/thread-pool + KlClient). The
  curated `test-iouring` completion gate (56/56) was validated at this exact commit earlier in the
  branch's history; re-running it in the same tree hit only the documented ASan-archive
  contamination gotcha (plain build linking a leftover `-fsanitize` `libkeel.a`), not a regression.
- **Readiness axis**: native default (kqueue) `make smoke-pollcomp-asan` peer + the
  `test_datagram_live` loopback round-trip adapt to the build backend (pollcomp/io_uring/readiness)
  via `kl_event_ctx_init`, proving one facade over both models.

### Findings: 0 new critical / high / medium

| # | Severity | Area | Note |
|---|----------|------|------|
| - | informational | `src/socket_dgram_posix.c:500-504` | `pdg_rx_batch_new` computes `(size_t)n * sizeof(...)` for the `recvmmsg` batch without an explicit overflow guard on `n`. **This is a socket-layer robustness nit, not an axis issue**: `n` is the app-set `mmsg_batch` config knob (not network input) and is 64-bit-safe; batch-then-check + free-on-error is otherwise correct. Cross-referenced from the same-day `/c-audit` (L1). |

All prior-pass informational items (per-server event-loop state for the declined Finding 2, etc.)
remain as previously dispositioned. No axis regression introduced by the datagram arc.

### Compatibility matrix (datagram row added; ⚙ = firmware-only, no host CI e2e)

| Backend combo | Stream (HTTP) | `KlUdp` datagram | Public `KlDatagram` |
|---|---|---|---|
| Darwin sockets + kqueue (readiness) | ✅ tested | ✅ tested | ✅ live (`test_datagram_live`) |
| Linux sockets + epoll (readiness) | ✅ tested | ✅ tested | ✅ live (container) |
| POSIX + poll (readiness fallback) | ✅ tested | ✅ tested | ✅ live |
| Linux sockets + io_uring (completion) | ✅ tested | ✅ tested | ✅ live (container, ASan) |
| pollcomp double (completion) | ✅ tested | ✅ tested | ✅ live + public mock |
| Winsock + WSAPoll (readiness) | ✅ CI | ✅ CI smoke-udp | ✅ CI smoke-datagram |
| Winsock + IOCP (completion) | ✅ CI | ✅ CI (assoc-at-create fix) | ✅ CI smoke-datagram |
| lwIP-raw (completion provider) | ✅ container | ✅ container | ✅ container (raw_datagram_test) |
| EFI_UDP4 (completion provider) | ✅ QEMU/OVMF | ✅ QEMU (dns harness) | ⚙ host-mock only¹ |

¹ EFI public-`KlDatagram` is host-mock + design-note validated (7B-9); the one deferred item is a
QEMU/OVMF public-`KlDatagram` e2e (the firmware dgram-DNS harness exercises `KlUdp`, not the
public facade). All other cells are runtime-tested, not merely present.

### Contract (unchanged + one amendment)

The socket-ownership / loop-affinity / completion-delivery / cancellation / close / backpressure
contract from prior passes stands. **Amendment (this arc):** a completion terminal may set
`KlCompletionEvent.retain_life=1` to signal that the life ref is BORROWED (not transferred), the
dispatch handler retires the op's state machine but abandons the ref (fail-closed for
abandoned-token backends). Backends that don't need it leave it 0. The terminal-result rule
(exactly one terminal per op; late/duplicate events discarded via the B.6 generation token)
is unchanged.

### Roadmap

1. *Deferred (not blocking):* a QEMU/OVMF **public-`KlDatagram`** e2e to promote the EFI facade
   cell from ⚙ to firm ✅ (the transport is already proven via the `KlUdp`-based dgram-DNS harness).
2. *Optional hardening:* an explicit `mmsg_batch` cap or overflow guard at `pdg_rx_batch_new`
   (informational; 64-bit-safe today).

No code changes were made in this pass; it is a re-verification; the architecture is sound.

---

## Eleventh pass: orchestration-layer refactors re-verify the three-axis separation (2026-08-08)

**Verdict: architecturally sound, the client/server orchestration refactors (this session's
review rounds) STRENGTHENED axis separation; no regressions.** Fresh `/axis-audit` after the
Finding-1 dispatch unification, the `client_proxy.c` extraction, the `server_activation.c`
split, and the EFI server data-plane fixes.

### Mechanical independence (Goal 4): PASS

- **Protocol-layer TUs name no platform networking header or event-engine symbol.** grep over
  `connection.c`, `response.c`, `client_{common,sync,async,proxy}.c`, `h2_client.c`,
  `websocket{,_client}.c`, `server_{ws,h2}.c`, `sse.c`, `router.c`, `chunked.c`, `parsers/*` for
  `<sys/epoll.h>`/`<sys/event.h>`/`<sys/socket.h>`/`netinet`/`arpa`/`io_uring`/`epoll_*`/`kevent`/
  `WSA*`/`OVERLAPPED`/`CreateIoCompletionPort` → **clean**. The new `client_proxy.c` (shared proxy
  CONNECT) is a pure protocol-layer TU (bounded `kl_buf_append_*` + `memcmp`); it is in
  `AXIS_PROTO_TUS` so the gate keeps it clean.
- **The readiness server (`server.c`) now names NO optional-protocol symbol** (grep for
  `kl_ws_server_*`/`kl_h2_server_*`/`kl_cidr_match`/`c->h2->`/`c->ws->` → only the `_hooks()`
  getters). Finding 1 (this session) routed the readiness data plane through the same
  `proto_hooks.h` seam the completion driver already used, so **both event models now dispatch
  WebSocket/HTTP-2/PROXY identically**, the asymmetry the review flagged is closed. This is a
  positive axis result: `server.c` (readiness) and `completion_server.c` (completion) are now
  peers above the protocol seam, neither owning protocol internals.

### Axis-relevant refactors this session (all confirmed non-regressive)

- **Finding 1: readiness ws/h2/proxy via hooks.** Extended `KlWsServerHooks`/`KlH2ServerHooks`
  with the readiness data-plane entrypoints; `server.c` dispatches through them (NULL-guarded,
  symmetric with `completion_server.c`). `kl_server_ws` moved to `server_ws.c`. server.c dropped
  `websocket_server.h`/`h2_server.h`/`h2_internal.h`.
- **`client_proxy.c`**, the sync/async proxy CONNECT is now one transport-independent module;
  the two event models differ only in byte movement (blocking vs the async state machine), the
  same "protocol above the axis" principle, applied to the client.
- **`server_activation.c`**, the systemd socket-activation surface is its own TU. It reads
  `LISTEN_*` env + `getpid` (platform, hosted), that is the *activation* responsibility, not a
  protocol or event-model concern; it names no event engine.
- **EFI server data plane**: accept backpressure (capacity-gated arming), alloc-free send, and
  `el_close` teardown live entirely in the EFI socket/completion provider (`integrations/uefi/`),
  below the axis; the server core is unchanged. See the review-round subsection of the tenth pass.

### Protocol-hook registry: capability-global, enablement per-server (clarification)

The `proto_hooks.h` tables are **process-wide install-once registrations of compiled-in
capabilities** (which protocol *implementation* to dispatch to), guarded by `hooks_set_once`
(commit `61bddb1`: idempotent same-table install / NULL reset allowed; a different live
replacement rejected). This is NOT global *configuration*: protocol **enablement is per-server**
and orthogonal to the registry,
- WebSocket fires only when the matched route has `ws_config` (`connection.c:520`), set per
  server via `kl_server_ws`;
- HTTP/2 is gated by per-connection `h2_config` (`connection.c:347/527/799`), copied from that
  server's `cfg.h2` (`server_core.c:213`);
- PROXY by that server's `cfg.proxy_trusted_cidrs` → `s->proxy_cidr_count`.

So two `KlServer`s in one process **can** run different *enabled* protocol sets today (e.g. A =
ws+h2, B = plain HTTP/1.1: B never sets `ws_config`, so the global ws table is never consulted
for it). The only thing the global registry precludes is two *different implementations of the
same protocol* selected per-server: exotic and unneeded. This is why de-globalizing the tables
into per-`KlServer` state (the review's declined Finding 2) buys ~nothing for real Keel usage.

### Sanitizer / driver checks

- Completion-axis driver under ASan (`make smoke-pollcomp-asan`): async/thread-pool over-completion
  roundtrip + async `KlClient` connect+GET over the completion loop; **both OK**.
- Full suite under ASan+UBSan (`make debug-test`): **65/65, 0 leaks/UB** (see c-audit twelfth pass).
- mock-EFI failure-path harness (ASan/UBSan): PASS incl. the new backpressure test.

### Compatibility matrix (reaffirmed)

Unchanged from the tenth pass; `EFI_TCP4 server (plaintext + HTTPS)` remains **firmware-verified
(QEMU/OVMF, container): S-4 + S-6 + S-7**. All other rows unchanged.

**One documented boundary (unchanged, cross-backend):** on the completion server recv path every
backend (IOCP/pollcomp/EFI) peeks `c->tls` to route ciphertext to `feed_input` vs plaintext to
`read_buf`. This is the existing completion-mode TLS contract, not a UEFI-specific leak; a neutral
`post_recv` destination spec that removes protocol knowledge from all completion backends is
deferred cross-backend work (recorded in `event_efi.h` + the review triage).

---

## Tenth pass: the UEFI HTTP(S) **server** validates the model-blind server core (2026-08-08)

**Verdict: architecturally sound, the inbound direction is the mirror of the ninth pass and,
if anything, a stronger validation of the axis model.** A STOCK freestanding `KlServer` serves
`GET / → 200`, **plaintext (S-4) and HTTPS (S-6)**, and tears down cleanly (S-7) over EFI_TCP4
on bare UEFI firmware (QEMU/OVMF, verified in an Ubuntu 24.04 container), with **zero protocol
edits**. The whole S-1..S-7 effort added no platform coupling to `src/`.

### What this pass confirms about the axes

1. **Protocol + server core stayed above both axes (Goal 4): mechanical PASS.** The core server
   TUs (`src/server_core.c`, `src/server.c`) contain no platform-networking or event-engine code
   (`grep` for `sys/epoll`/`sys/event`/`io_uring_`/`epoll_`/`kevent(`/`WSA`/`EFI_TCP4_PROTOCOL`/
   `efi_sock_` finds only comment prose). No EFI/UEFI TU lives in `src/`; every EFI symbol is under
   `integrations/uefi/`. The server core serves over EFI_TCP4 byte-identically to epoll/io_uring.
2. **The `KlCompletionOps` vtable hosts a SERVER backend, not just a client (Goals 1, 12).** S-3/S-4
   added `prime_accepts`/`post_accept` + the completion-native `post_recv`/`post_send` to the EFI
   backend (`event_efi.c`), surfaced as `KL_COMP_ACCEPT`/`KL_COMP_READ`/`KL_COMP_WRITE` and consumed
   by the model-blind `completion_server.c`, the exact shape io_uring/IOCP/pollcomp use. The client
   rode the watcher relay and never needed these; adding them left the client path untouched.
3. **Completion-mode server TLS needed nothing below the axis (Goals 4, 9).** S-6 required only the
   two documented completion-TLS obligations in the backend; feed received *ciphertext* to
   `tls->feed_input` and a synchronous send for `kl_comp_tls_flush` (already `efi_sock_send`). The
   `KlTls` memory-BIO ops (`feed_input`/`drain_output`) served the server verbatim; **no
   TLS-vtable change**. The one fix was a client-only mbedTLS config missing `MBEDTLS_SSL_SRV_C`.
4. **Server operation-lifetime + teardown (Goals 6, 10): clean.** S-7 carved a freestanding
   `kl_server_free`; `kl_conn_pool_free` closes every accepted-child socket (draining its EFI
   tokens), so `kl_uefi_socket_provider_live_count() == 0` after teardown, the precondition for
   `kl_uefi_shutdown()` / ExitBootServices. Firmware-verified: served → clean teardown, 0 live
   sockets, providers released. The `post_recv`/`post_send` ops are generation-stale-guarded (an
   accepted child that closes mid-op is dropped, never delivered) and freed on completion/cancel.

The only core deltas were the `KEEL_FREESTANDING`-guarded `kl_server_init`/`kl_server_free` carve
into `server_core.c` (the freestanding server archive now constructs + tears down a `KlServer`
from itself), a build-axis guard, not a platform `#ifdef` in a dispatch path.

**Compatibility-matrix delta:** `EFI_TCP4 server (plaintext + HTTPS)` moves from *not-started* to
**firmware-verified (QEMU/OVMF, container): S-4 plaintext + S-6 HTTPS + S-7 clean teardown**.

### Review round (post-S-7): three seam fixes, one documented boundary

A follow-up server-side review found three concrete implementation bugs at the inbound seam
(all now fixed + verified; commit `71573dd`):
- **Accept backpressure (Goal 8) is now REAL, not just claimed.** The socket layer had auto-
  re-armed every consumed Accept token, so EFI kept a full armed pool and accepted into
  firmware children Keel couldn't service. Fixed by splitting the accept lifecycle: `listen`
  arms none; `accept` harvests without re-arming; `kl_uefi_socket_accept_arm(fd, want)` arms
  capacity-gated (`want` = free Keel slots), driven every tick by `el_prime_accepts`. New mock
  test `t_accept_backpressure` proves excess connections are not accepted.
- **Alloc-free send (op/buffer lifetime, Goal 6).** `post_send` had `kl_malloc`'d per response;
  it now copies into an inline per-op buffer. NB: the copy is *mandatory*; `comp_tls_post_encrypted`
  frees its ciphertext right after posting, so a "reference stable segments" optimization is a
  use-after-free on the TLS path (caught in the container as HTTPS-000; a good reminder the send
  contract is copy-required across all completion backends).
- **`el_close` teardown.** Now retires all connect/watch/server-I/O records + clears latched
  state before the ctx is freed (the inline send buffer also removes the former leak window).

**Documented boundary (not a regression):** on the server recv path the EFI backend peeks
`c->tls` to route ciphertext to `feed_input` vs plaintext to `read_buf`. This is the *existing*
completion-mode TLS contract for every backend (IOCP/pollcomp too), not a UEFI-specific leak;
removing protocol knowledge from all completion backends (a neutral post_recv destination spec)
is deferred cross-backend work, recorded here and in `event_efi.h`.

---

## Ninth pass: the UEFI completion provider validates the operation-lifetime contract (2026-08-06)

**Verdict: architecturally sound, and the F-8 hardening is a direct, positive stress-test of
the completion axis's *operation-lifetime* contract (Goals 6 & 10).** The EFI provider is a
completion-native backend of exactly the shape the axis model anticipates (`event_efi.c` posts
EFI tokens ≈ SQEs/OVERLAPPEDs; `socket_efi_tcp4.c` is the socket axis; the protocol layer above
is unchanged and platform-blind). No axis-separation regressions; the review found real *lifetime*
bugs in the new provider, precisely the class Goal 6 ("operation ownership and lifetime correct,
completion makes these dangerous: close-while-outstanding, cancellation, timeout races, stale
completion after handle reuse, UAF/double-free") and Goal 10 ("cancel racing completion → exactly-
one terminal result") name, and they are now fixed and host-test-covered.

### What this pass confirms about the axes

1. **Protocol layer stayed above both axes (Goal 4).** None of U1–U8 required a change above the
   socket/completion seam. The `KlClient` HTTP path, the parser, and the DNS `KlResolver` consumer
   are byte-identical to the POSIX build; the fixes live entirely in the EFI socket provider + its
   completion backend + the mbedTLS platform TU. A completion provider's token-lifetime bug did not
   leak upward, the abstraction held.

2. **The completion lifetime contract is now honored by a third backend (Goal 6/10).** io_uring and
   IOCP already encode "every submitted op reaches exactly one terminal state (completion or
   cancel→drain) before its buffers/state are freed." The EFI provider originally violated this on
   the *failed-cancel* edge (a token that refuses to drain was freed anyway). The **quarantine
   model**: stable provider-owned slot storage (`static KlUefiConn g_conns[]`) that is never
   reclaimed once a token cannot be confirmed retired; is the EFI-specific realization of the same
   contract, mirroring io_uring's cancel-sentinel discipline. Explicit per-token state
   (`conn_posted`/`tx_posted`/`rx_posted`/`close_posted`) is the registration-vs-submission
   distinction (Goal 12) made concrete: each flag is one in-flight submission, set on submit,
   cleared on the observed terminal.

3. **Close-with-outstanding-work is correct under the completion model (Goal 6, required trace).**
   `efi_sock_close` now cancels, drains **only** posted tokens, and, critically, if any drain
   fails, quarantines instead of tearing down. This is the EFI analogue of the io_uring
   close-with-pending-CQE path, and it fixed a real ~60 s stall rooted in `CheckEvent` consuming the
   signal (a completion-delivery honesty issue, Goal 2: readiness/completion semantics represented
   honestly, the code must not treat a consumed event as still-signalled).

4. **Error normalization + bounds at the seam (Goal 9, security).** The impossible-`DataLength`
   validation (U4) is the completion-axis instance of "platform error/length values treated as byte
   counts", a completion event's reported length is untrusted and now bounds-checked against
   `KL_EFI_RXBUF` before any copy.

5. **Backend selection / post-EBS fail-closed (Goal 13).** ExitBootServices is the EFI equivalent of
   "the event engine went away." The provider now fails closed on every data-path + heap entry after
   EBS (`kl_uefi_after_ebs()`), so no post-teardown firmware call is made, the selected backend's
   unavailability is observable and safe, never a silent partial.

**Method:** the same 18-scenario host mock-EFI harness (`mock_efi_test.c`) that drives the C-audit
pass exercises these axis paths directly: cancel-succeeds/fails/races, close-with-outstanding-
receive, consumed-connect-event-during-close, post-EBS: under ASan+UBSan. See the eleventh C-audit
pass for the finding table.

**Compatibility-matrix delta:** `EFI_TCP4 + EFI completion backend` moves from *buildable/
happy-path-tested* to *failure-path host-tested* (cancel/close/timeout/post-EBS) **plus full TLS
cert validation** (CA + hostname + validity-time over Runtime Services GetTime, fail-closed; proven
by a valid/expired QEMU pair: U-8). No change to the POSIX/io_uring/IOCP/pollcomp rows.

**U-8 postscript (2026-08-06): certificate validity-time (the last open item).** Enabling
`MBEDTLS_HAVE_TIME_DATE` bound mbedTLS's clock to Runtime Services `GetTime` via a small pure
conversion (`civil_time.c`) + glue (`time_uefi.c`). Axis note: this touched only the socket/platform
side, the *protocol* layer (KlClient, the TLS vtable contract) is unchanged (Goal 4 holds), and the
clock is fail-closed (Goal 9/13: an unavailable/implausible RTC degrades safely to "reject all
certs", never silently accepts). `GetTime` is a Runtime Service (valid across ExitBootServices), so
it does not reintroduce a boot-services dependency in the post-EBS window.

## Eighth pass: the freestanding portability phase strengthens all three axes (2026-08-05)

**Verdict: architecturally sound; every change in the freestanding phase (PRs #199–#211) either
preserved or *improved* the axis separation.** No new findings; three concrete improvements + one
future-provider validation. This pass reviews the phase's axis impact (the TU splits are movement,
nm-proven; the new mechanisms are what matter).

### Improvements to the axes

1. **The completion driver is now genuinely feature-agnostic (Goal 3/4).** `completion_driver.c`
   split into `completion_core.c` (the generic tick `kl_comp_run`: drain → WATCHER/CONNECT via
   `kl_event_dispatch`, ACCEPT/READ/WRITE + UDP routed out, then timers) / `completion_server.c`
   (KlConn/HTTP-1 + TLS memory-BIO) / `completion_h2.c` / `completion_ws.c`, decoupled via **two
   opaque `KlEventCtx` hooks**: `comp_conn_dispatch` (set by the server) and `comp_udp_dispatch`
   (set by `kl_udp_init`). The hooks take `const void *ev` so `KlCompletionEvent` (internal
   `src/completion.h`) never reaches the public header: no backend-internal type leaks upward
   (avoids the "generic event object exposing backend unions" smell). They're **per-loop fields,
   not file-scope globals** (avoids "hidden global event-loop state"). nm-proven: `completion_core.o`
   has no static ref to `comp_on_accept/read/write` or `kl_udp_comp_on_recv/send`, a client-only
   completion link (the freestanding archive) pulls neither the server nor UDP. This is the cleanest
   possible expression of "protocols/features sit above the generic completion axis."
2. **Error classification is now a first-class SEAM op, not an errno leak (Goal 9, the big win).**
   B1 added `KlIoStatus` + `kl_sock_io_status(p)` (`src/socket.h`): the socket provider classifies
   the last -1 (WOULD_BLOCK/INTERRUPTED/PENDING/CLOSED/RESET/FATAL); the errno mapping lives in
   ONE place (`kl_sockdef_io_status`, the POSIX/Winsock seam), reached via the op-or-sockdef inline
   exactly like `kl_sock_get_so_error`. The client (`client_async/sync/pool`, 16 sites) now reads
   `kl_sock_io_status`, **NOT `errno`** (client_async.c: 0 errno). Previously the protocol/client
   read `errno`, a platform detail, on every would-block/connect-pending, a real axis leak. Now a
   non-errno provider (EFI, a pure mock) classifies natively; hosted providers fall back to the
   errno map with zero behavior change. U-0 confirmed `EFI_STATUS → KlIoStatus` is a clean switch.
3. **The completion event carries only `KlSockAddr` (Goal 5).** A2 removed `<keel/net.h>` +
   `struct sockaddr_storage` from `src/completion.h`; backends convert native→Keel once at their
   seam. One fewer host-ABI seam in the core event; benefits lwIP + EFI alike.

### Mechanical independence (Goal 4): PASS
The new client TUs (`client_common/async/sync.c`) + `completion_core.c` + `event_ctx.c` include NO
platform networking/event header and call no `epoll_*`/`kevent`/`io_uring_*`/`WSA*`/`OVERLAPPED`
(the sole hit is `event_ctx.c`'s `kl_socket_provider_has_cap`, a capability query through the
seam). `event_lwip_raw.c` remains lwIP-free (7th pass). The H1 watcher-liveness guard (9th c-audit
/ #198) survived the `async.c`→`event_ctx.c` split intact; it lives in the `event_ctx.h` inline,
so both `kl_event_ctx_run` and the completion driver see it.

### Goal 14 (future providers): VALIDATED, not just assessed
U-0 (#211) ran a **raw EFI_TCP4 GET → 200 under QEMU/OVMF**, empirically confirming the completion
+ pointer-handle model maps onto EFI tokens. The 6 lifecycle findings (type-0 token events;
`Poll()`+`CheckEvent()` pump = "the loop IS the firmware pump"; opened-child-protocol; DHCP async;
`EFI_STATUS`→KlIoStatus clean switch; teardown ordering) show the existing `KlCompletionOps`
contract (`post_connect`/`cancel`/`drain`) needs no new abstraction for EFI_TCP4, a client-driving
backend uses only `post_connect` + `cancel` + a `drain` emitting `KL_COMP_CONNECT`/`WATCHER`. The
UEFI blocker is now the freestanding LIBC surface (delivered: mem*/strlen, self-contained archive),
not the axis model.

### Automated gate
cppcheck 0; scan-build "No bugs found"; 64 suites under ASan+UBSan (incl the split TUs + the new
`test_kl_cstr`/`test_kl_cstr_builtin`); the freestanding archive symbol-gated for x86_64 + aarch64;
the freestanding harness runs the client over a mock completion provider 57/57 (ASan+UBSan+LSan),
the "equivalent protocol behavior over a mock event/socket provider" this skill asks for; io_uring
(56) + loopback-raw gates green across the phase's PRs.

### Compatibility matrix (unchanged from 7th pass + one addition)
| Combination | Status |
|-------------|--------|
| Linux epoll / Darwin kqueue / Linux io_uring (default) | production |
| Winsock + WSAPoll / IOCP | buildable + MinGW-gated |
| pollcomp double | CI/ASan gate |
| lwIP-raw (server + client + UDP + DNS + HTTPS) | loopback-verified, ASan-clean |
| **Freestanding client archive (mock completion provider)** | **host harness 57/57 (ASan+UBSan+LSan); CRT-less PE/COFF link x86_64+aarch64; U-0 raw EFI_TCP4 GET in QEMU** |
| EFI_TCP4 provider (real KlClient over EFI) | designed + U-0-validated; U-1..U-3 pending |

No changes made in this pass (review-only; the one code touch this session: `KL_CLIENT_CHUNK_HDR_SIZE`
; is a cosmetic c-audit L1, not an axis issue).

---

## Seventh pass: the lwIP-raw CLIENT axis validates the third event model end to end (2026-08-05)

**Verdict: architecturally sound.** The completion-native lwIP-raw provider, the third event
model (Phase 9): now carries the full **client** axis (plaintext, Happy-Eyeballs, DNS, HTTPS)
in addition to the server, and it did so with **zero `src/` changes** for the UDP/DNS transport
(LC-3a/LC-3: `src/udp.c` and `src/dns_resolver.c` run verbatim over the raw completion loop) and
only **backend-conforming** fixes for TLS (LC-4: the raw backend was made to satisfy the existing
cross-backend completion-TLS contract, not the reverse). This is the strongest possible evidence
that the event axis, socket axis, and protocol layer are genuinely independent: KEEL's own
datagram machine, DNS resolver, HTTP client, and TLS driver were all reused unchanged over a
socketless, fd-less, `NO_SYS=1` callback stack.

### What the LC-0..LC-5 work added to the axes

1. **A new completion primitive, honestly modeled: `KL_COMP_CONNECT` + `KlCompletionOps.post_connect`**
   (`src/completion.h`). This is the **outbound counterpart of `KL_COMP_ACCEPT`**, construct a
   connect op on a client-owned nonblocking socket, submit, receive completion, interpret win/fail.
   It is a genuine completion concept (not synthetic readiness): the result is carried in the
   delivered event mask (`KL_EVENT_WRITE`=connected, `0`=failed), *not* re-derived from
   `getsockopt(SO_ERROR)` on the client side: deliberately, because io_uring drops `SO_ERROR`
   after a failed `IORING_OP_CONNECT`. Implemented on all three completion backends
   (`event_pollcomp.c` PC_CONNECT, `event_iouring.c` IOU_CONNECT, `event_iocp.c` ConnectEx, the
   latter also fixed the previously-broken IOCP async client). **Goal 2 (honest semantics): PASS.**
2. **The client's completion-vs-readiness connect branch** (`src/client.c`
   `client_comp_connect`/`he_proceed_after_connect`) + `kl_watcher_add_detached` (`src/async.c`):
   a ctx-owned watcher node with no readiness arm, so a completion loop can carry the connect
   result to the client without the client thinking in either event model. **Goal 3 (no model
   leak): PASS**, the client never sees SQEs/CQEs/OVERLAPPED/POLLOUT; it registers a tagged
   watcher and receives a Keel-level connect result.
3. **`KlUdp` over the raw loop (LC-3a)**, the raw socket provider gained `KlDatagramOps`
   (`SOCK_DGRAM → udp_new`; `post_udp_recv/send → udp_recv/udp_sendto`), so `src/udp.c`'s machine
   runs unchanged. **DNS (LC-3)** then rides that KlUdp via `kl_dns_resolver_create` on a ctx whose
   `sockets = kl_socket_provider_lwip_raw()`: one DNS path, no lwIP `dns_gethostbyname`.
4. **HTTPS (LC-4)**: client TLS is socket-BIO routed through the provider (`t->sp =
   ev_ctx->sockets` → `lwr_sock_send/recv`); server TLS is the generic memory-BIO
   completion-TLS leg. Both reuse existing legs; the backend only moves bytes.

### Mechanical independence checks (Goal 4): PASS

- **No protocol TU** (`connection.c`, `h2.c`, `websocket.c`, `client.c`, `h2_client.c`,
  `websocket_client.c`, `sse.c`, `response.c`, `router.c`, `redirect.c`, `client_pool.c`,
  `dns_resolver.c`) includes a platform networking/event header or calls `epoll_*`/`kevent`/
  `io_uring_*`/`WSA*`/`OVERLAPPED`/`GetQueuedCompletion*` directly (only `client.c` pulls
  `platform.h` for the abstracted `kl_plat_poll1` sync wait; the remaining hits are comments).
- **The raw completion backend `event_lwip_raw.c` is lwIP-free**: it includes only the neutral
  seam headers (`keel_lwip_raw.h`, `lwip_raw_glue.h`); **no `<lwip/*>`**. Every `tcp_*`/`udp_*`
  call is confined to `lwip_raw_glue.c`. No `<lwip/*>` appears anywhere in `src/` (the sole
  `sockcompat.h` `#include "lwip/sockets.h"` is the guarded non-raw lwIP-*socket* ABI seam,
  by-design and unrelated to the raw/completion path).
- Addresses cross the seam as `KlSockAddr` / raw IPv4 bytes + host-order port; handles as
  `KlSocketHandle` (`intptr_t`) carrying a `tcp_pcb*`/`udp_pcb*`, never an `int`.

### Execution-path traces (completion, lwIP-raw)

- **Connect:** `client_comp_connect` → `kl_watcher_add_detached(fd)` → `kl_comp_post_connect` →
  `lwr_comp_post_connect` → `kl_lwr_connect`/`tcp_connect` → `lwr_cli_connected` (connected_cb) →
  `KL_COMP_CONNECT` (mask-encoded) → `kl_comp_run` → `kl_event_dispatch` → the client's tagged
  connect watcher → `he_on_connect_result` → SENDING.
- **Receive:** `lwr_srv_recv` (tcp_recv, retained-rx pbuf on `ERR_MEM` backpressure) →
  `kl_lwr_take_staged` (issues `tcp_recved` as bytes are delivered) → `KL_COMP_READ` →
  `comp_on_read` → `kl_conn_ingest_body` (model-blind core).
- **Send + backpressure:** `kl_conn_send_complete` → `post_send` → `kl_lwr_send_begin`/
  `lwr_send_pump` (`tcp_write`+`tcp_output`, window-bounded `conn_cap × KL_LWR_TX_WIN`; `ERR_MEM`
  → EAGAIN → re-arm) → `lwr_srv_sent` (tcp_sent) → `KL_COMP_WRITE`.
- **Close-with-outstanding:** `kl_lwr_tcp_abort`/`lwr_srv_err` → mark slot `dead` + `->pcb=NULL`
  (prevents freed-pcb aliasing in `lwr_conn_find`) → terminal completion exactly once
  (`terminated`/`pend_terminal` cross-guard).

### Findings

- **HIGH (axis lifetime): same-batch Happy-Eyeballs connect UAF.** `kl_comp_run`
  (`src/completion_driver.c`) drains a *batch* of completions and dispatches them sequentially;
  `kl_event_dispatch` (`include/keel/event_ctx.h`) derefs the tagged `KlWatcher*` with **no
  liveness check**. Dispatching a winning `KL_COMP_CONNECT` frees the losing attempts' watcher
  nodes (`he_close_attempts → kl_watcher_del`), so a loser's still-batched `KL_COMP_CONNECT`
  then reads freed memory. This is the axis-relevant instance of Goal 6/10 (op lifetime + cancel
  racing completion within one drain). Architectural principle: **a completion driver that
  batch-dispatches must tolerate a target being retired earlier in the same batch.** Smallest
  fix: validate the tagged watcher is still linked in `ctx->watchers` before deref (backend-
  agnostic). Same latent shape exists for `KL_COMP_WATCHER`; the guard covers both. (Mirrors the
  c-audit H1; see `docs/keel_audit.md` ninth pass.)
- **MEDIUM, lwip-raw accept→post_recv dangling-pcb** (`lwip_raw_glue.c`): a stack abort in the
  pre-owner window is misrouted; the driver can adopt a freed pcb. Goal 6 lifetime. Fix:
  `tcp_arg(newpcb, slot)` at accept. (c-audit M1.)
- **INFORMATIONAL: stale `SO_ERROR` comments** in `completion_driver.c`/`event_pollcomp.c`/
  `event_iouring.c` describe a connect win/fail mechanism the client no longer uses (it trusts
  the mask). Comment-only; correct them. (c-audit L3.)

Everything else: honest readiness vs completion semantics, no model leak in the public API,
protocol platform-independence, error normalization at the seam, model-independent backpressure
(the raw retained-rx ERR_MEM flow-control is the completion analog of readiness interest
reduction), registration-vs-submission distinction: holds.

### Compatibility matrix (updated)

| Combination | Status |
|-------------|--------|
| Linux sockets + epoll | production (default) |
| Darwin sockets + kqueue | production (default) |
| Linux sockets + io_uring | production (default `BACKEND=iouring`) |
| Winsock + WSAPoll | buildable + MinGW-gated |
| Winsock + IOCP | buildable + MinGW-gated; async client fixed (LC-0 ConnectEx) |
| pollcomp (portable completion double) | CI/ASan gate |
| **lwIP-raw completion: server** | loopback-verified, ASan+UBSan+LSan, CI-gated |
| **lwIP-raw completion: client (plaintext + HE)** | loopback-verified (LC-1/LC-2), ASan-clean; **H1 fix pending** for racing dual-stack connect |
| **lwIP-raw completion: UDP + DNS** | loopback-verified (LC-3a/LC-3), ASan-clean, `src/` unchanged |
| **lwIP-raw completion: HTTPS (client + server)** | loopback-verified (LC-4), ASan-clean, BYO mbedTLS (local/hull gate) |

### Future-provider compatibility (Goal 14): UEFI

The lwip-raw client axis is a near-exact precedent for a UEFI provider (completion-native,
socketless, fd-less, single-loop, pointer-handle). The concrete blockers are now only the
freestanding toolchain + the EFI event/token lifecycle; assessed in the new
`docs/phase10_uefi_feasibility_design.md` (roadmap Phase 10). No new abstraction is needed; the
`KlCompletionOps` + `KlSocketProvider` seams map directly onto `EFI_TCP4` tokens/events.

### Automated gate

cppcheck 0, scan-build "No bugs found," 60 suites under ASan+UBSan (`make debug-test`), the
loopback-raw + raw-tls ASan runs, epoll + io_uring (56-suite) gates; all green (one UBSan
finding, `redirect.c:77` NULL-to-`strncasecmp`, is a protocol-layer nit tracked as c-audit L1,
not an axis issue).

---

## Sixth pass: datagram data-plane folded onto the socket provider (2026-08-03)

**Verdict: architecturally sound, this pass *removed the last link-time coupling on the socket
axis*.** The fifth pass (same day) reviewed the `udp_io` seam once it spoke `KlSockAddr`. Since
then the datagram data-plane has been **folded onto `KlSocketProvider`** (PRs #169–#172, the A2
refactor): the `kl_udp_io_*` seam and its per-platform TUs (`udp_io_posix.c`, `udp_io_win.c`,
`udp_io_lwip.c`, `udp_io.h`) are **deleted**, replaced by an optional `KlDatagramOps` vtable hung
off the provider (`KlSocketProvider.dgram`, present iff `capabilities & KL_SOCK_CAP_DATAGRAM`).
One runtime object now owns *all* of a stack's socket I/O; stream **and** datagram. This directly
dissolves the asymmetry that motivated the work: previously TCP was runtime-injected via the
provider while UDP was paired at *link time* with a matching `udp_io_*.o`. Both planes are now
selected by the same runtime `KlEventCtx.sockets` pointer.

### What changed in the axis structure (all improvements)

1. **The socket axis is now fully runtime-injectable for both I/O planes.** `include/keel/datagram.h`
   defines `KlDatagramOps`, a **primitive-only** vtable whose every op takes `(void *ctx,
   KlSocketHandle fd, …)` + `KlSockAddr` and **never `KlUdp`**. The provider therefore holds no UDP
   machine state. Ops: `send`/`recv`/`send_gso`/`configure` (folds all socket-option setup,
   reuse/bufs/broadcast/TOS/multicast/pktinfo/GRO, and returns the accepted `KL_DGRAM_RX_*`
   capability bitmask)/`set_tos`/`mcast_membership`, plus a **data-oriented** batch surface
   (`rx_batch_new`/`tx_batch_new`/`recv_batch` filling `KlDgramRxSlot[]` / `send_batch` taking
   `KlDgramTxDesc[]`): no thunks, so `recvmmsg`/`sendmmsg` batching survives the fold intact.

2. **All datagram machine logic stays in `udp.c`** (the model-blind layer): the send-queue walk +
   ordering (`udp_send_common`/`udp_enqueue`/`udp_flush_dgram`), delivery + GRO split
   (`kl_udp_deliver`), interest tracking, on-drain backpressure, drop accounting, and the
   multicast-group validation (`udp_group_ok`, preserving the public `KL_ERR_INVALID_ARG` contract).
   `udp.c` dispatches through `udp_dg(udp)` → `sp->dgram`. Mechanical check this pass: `udp.c`
   contains **zero** non-comment platform net/event symbols (`epoll`/`kqueue`/`io_uring`/`WSA`/
   `recvmsg`/`sendto`/…): every match is explanatory prose.

3. **Readiness vs completion is chosen honestly, via the event-caps abstraction, not platform
   mechanics.** `udp.c` branches on `kl_event_caps(&loop) & KL_EVENT_CAP_COMPLETION`: a completion
   loop posts an overlapped recv (`kl_comp_post_udp_recv`) and its readiness watcher never fires
   (verified at `udp.c:509–520`); a readiness loop drives `udp_recv_dgram` → `dg->recv`. **Both**
   funnel into the *same* model-blind `kl_udp_deliver` (`udp.c:156`). No synthetic readiness, no
   "readable ⇒ completed" pretense. Plain sends on a completion loop post overlapped
   (`kl_comp_post_udp_send`); source-pinned / TOS sends fall through to the synchronous `dg->send`
   seam (documented, works on a completion socket), an explicit, narrow, honest deviation.

4. **Shared cmsg parsers extracted to always-linked TUs.** The pktinfo/GRO parsers the *completion*
   backends still need (`event_iouring.c`, `event_pollcomp.c`, `event_iocp.c`) moved from the
   deleted seam into `src/udp_cmsg.c` (`kl_udp_parse_local`/`kl_udp_parse_gro`) and
   `src/udp_cmsg_win.c` (`kl_udp_win_get_recvmsg`/`kl_udp_win_parse_local`). The readiness datagram
   provider (`socket_dgram_posix.c`/`socket_dgram_win.c`) keeps its **own** static copies so a
   foreign stack can link-override the entire data-plane with no residual seam dependency.

5. **The default + completion-inheritance cases are wired symmetrically with the stream axis.**
   `kl_sockdef_dgram()` returns the built-in datagram ops when `KlEventCtx.sockets == NULL`
   (mirroring the `kl_sockdef_*` stream fallback); the overlapped completion providers inherit the
   base plane (`event_iouring.c`/`event_pollcomp.c`: `prov.dgram = posix->dgram`; `event_iocp.c`:
   `.dgram = &kl_socket_winsock_dgram_ops`) so `configure()`/socket-opts work identically while
   recv/send route through the completion post. lwIP supplies its own `lwip_dgram_ops` using only
   public headers (`integrations/lwip/socket_lwip.c`), no `udp_io_lwip.c`.

### Execution-path re-trace (new datagram structure)

- **Readiness UDP receive:** `udp_on_ready` → `udp_recv_dgram(udp, dg)` → `dg->recv(ctx, fd, buf,
  size, &src, &meta)` (the provider `recvmsg`s into a stack `sockaddr_storage`, marshals via
  `kl_sockaddr_from_native`, parses pktinfo/GRO/TOS cmsgs opportunistically into `KlDgramRxMeta`)
  → `kl_udp_deliver(src: KlSockAddr, …)` → `on_recv`. Host sockaddr never escapes the provider TU.
- **Completion UDP receive:** `kl_udp_recv_start` sees `KL_EVENT_CAP_COMPLETION` → `kl_event_add` +
  `kl_comp_post_udp_recv` (overlapped `WSARecvFrom` / io_uring recv) → backend fills host `peer` +
  cmsgs into the completion event → `completion_driver.c` `KL_COMP_UDP_RECV` marshals once via
  `kl_sockaddr_from_native` → `kl_udp_comp_on_recv` → **same** `kl_udp_deliver` → re-post.
- **Send + backpressure:** readiness; `dg->send`; on `EAGAIN` → `udp_enqueue`, `q_bytes` grows,
  interest gains `KL_EVENT_WRITE`, `udp_flush_dgram` drains on writability, `on_drain` at empty.
  Completion: `kl_comp_post_udp_send`, `q_bytes` tracks outstanding overlapped bytes against
  `max_send_queue`, `kl_udp_comp_on_send` releases + fires `on_drain`. Same Keel-level semantics.
- **Close with outstanding work:** `kl_udp_free` frees the send queue, rx/tx batch blocks (via
  `dg->rx_batch_free`/`tx_batch_free`), and unregisters interest; `recv_active`/`kl_handle_valid`
  guards in `kl_udp_deliver` + `kl_udp_comp_on_recv` stop re-arm after a mid-callback free.

### Findings

- **[Low: FIXED this pass, via /c-audit 8th pass] TOS control-message family guess.** In
  `pdg_send` (`socket_dgram_posix.c`) and `wdg_send` (`socket_dgram_win.c`) a source-pinned/TOS
  send on a *connected* socket (dest `UNSPEC`) hard-coded `AF_INET` when building the TOS cmsg,
  which would attach an `IP_TOS` cmsg to an IPv6 flow. Fixed to derive the family from dest, else
  the pinned source, else v4: `family = dest_len ? dest->family : (src_len ? src->family : AF_INET)`.

- **[Informational] Unused rx-batch allocation on a completion loop.** `kl_udp_recv_start`
  (`udp.c:503–507`) allocates the `recvmmsg` batch via the inherited `posix->dgram` whenever
  `mmsg_batch > 1`, but a completion loop consumes recv through `kl_comp_post_udp_recv`, never the
  batch. The block is config-gated (off by default) and correctly freed at teardown, so this is a
  small transient over-allocation, not a leak or correctness bug. Left as-is to avoid churn; a
  one-line `!(caps & KL_EVENT_CAP_COMPLETION)` guard would remove it if ever measured.

- **[Informational: FIXED this pass] Stale seam references in comments.** With `udp_io_*` deleted,
  several comments still named it (`udp.c:20/29/158`, `socket_dgram_posix.c`, `include/keel/udp.h`).
  Corrected to reference the datagram provider / `socket_dgram_posix.c`. A stray untracked build
  artifact (`src/udp_io_win.o`) was removed.

- **[Informational] No new axis leak introduced.** Re-ran the mechanical protocol-independence
  greps: `datagram.h` includes no platform networking headers (only `allocator`/`handle`/`sockaddr`
  + `stddef`/`stdint`/`sys/types` for `ssize_t`, matching `socket.h`); it references `KlUdp` only in
  prose and `struct KlUdpConfig` only as a borrowed forward-declared config pointer. The
  `check-sockaddr-neutral` invariant from the fifth pass is unaffected, the fold happens *inside*
  the provider boundary that was already permitted to see host sockaddrs.

### Compatibility matrix (datagram plane, updated)

| Socket × Event                | Stream | Datagram plane | Notes |
|-------------------------------|--------|----------------|-------|
| Linux sockets + epoll         | ✅ prod | ✅ `kl_socket_posix_dgram_ops` (mmsg/GSO/GRO/TOS) | readiness `dg->recv`/`send` |
| Linux sockets + io_uring      | ✅ prod | ✅ inherits `posix->dgram` for opts; recv/send via completion post | `configure` + overlapped I/O |
| Darwin sockets + kqueue       | ✅ prod | ✅ `posix` dgram (per-datagram; no mmsg/GSO) | readiness |
| Winsock + WSAPoll             | ✅ build | ✅ `kl_socket_winsock_dgram_ops` (WSARecvMsg pktinfo) | readiness; no mmsg/GSO/GRO |
| Winsock + IOCP                | ✅ build | ✅ `.dgram = winsock`; recv/send via overlapped post | cross-compiled (MinGW) |
| pollcomp double (POSIX)       | ✅ test | ✅ inherits `posix->dgram`; completion post path | ASan gate |
| lwIP (foreign stack)          | ✅ test | ✅ `lwip_dgram_ops` (public headers only) | loopback CI |

### Contract clarified this pass: the datagram data-plane

- **Ownership split:** the provider supplies **primitives** (`send`/`recv`/`configure`/`set_tos`/
  `mcast_membership`/`send_gso` + optional data-oriented batch), taking only `(ctx, fd, KlSockAddr,
  buffers, meta)`; `udp.c` owns **all** machine state (queue, ordering, interest, drop accounting,
  delivery, multicast validation). A provider is stateless per-socket beyond the fd.
- **Capability handshake:** a provider advertises `KL_SOCK_CAP_DATAGRAM` and sets `.dgram`;
  `kl_udp_init` rejects a provider without `.dgram`. `configure()` returns the *accepted*
  `KL_DGRAM_RX_*` bitmask so `udp.c` learns which cmsg captures the stack actually enabled.
- **recv contract:** the recv op always attaches a control buffer and parses cmsgs
  opportunistically (kernel fills only what `configure` enabled), so no per-socket capture flags
  cross the seam; truncation/local/GRO/TOS surface via `KlDgramRxMeta`.
- **Model-agnostic delivery:** both readiness (`dg->recv`) and completion (`kl_comp_post_udp_recv`)
  paths deliver through the single `kl_udp_deliver`; a completion event ≠ a whole coalesced buffer
  is split identically to the readiness GRO path.

### Recommended roadmap

- *Correctness:* none outstanding (L1 fixed).
- *Coverage:* add a mock `KlDatagramOps` provider test (mirroring `test_socket_provider.c`) that
  exercises `configure` cap-bitmask negotiation + a short/failing `send` → enqueue path without a
  real socket, to pin the machine/provider contract deterministically.
- *Cleanup (deferred, optional):* the completion-loop `rx_batch` guard above.

### Changes made this pass

- `src/udp.c`, `src/socket_dgram_posix.c`, `include/keel/udp.h`: corrected stale `udp_io`
  comments to reference the datagram provider (comment-only; no code change).
- `src/socket_dgram_posix.c`, `src/socket_dgram_win.c`: L1 TOS-family fix (from the /c-audit pass).
- Removed stray untracked `src/udp_io_win.o`.
- Verified: `make` (kqueue) clean; 61 test suites pass; `make smoke-pollcomp-asan` (completion
  axis) clean; `make cppcheck` clean.

---

## Fifth pass: address-ABI neutralization, event-provider seam, lwIP as a third stack (2026-08-03)

**Verdict: architecturally sound, the recent work *strengthened* the three-axis separation rather
than eroding it, and empirically validated it.** This pass reviews everything since the fourth pass
(PRs #150–#165): the runtime **event-provider** seam, the **KlSockAddr address-ABI
neutralization**, the `udp_io` seam flip to KlSockAddr, **TLS-over-`KlSocketProvider`** routing,
and the **lwIP platform** (server + client + UDP + TLS on a stock `libkeel.a`). The headline: the
"future provider compatibility" goal (Goal 14) that prior passes could only reason about
hypothetically is now **realized and CI-tested** by a real third stack that is neither POSIX nor
Winsock, the strongest possible evidence the socket/event axes are genuinely independent.

### What changed in the axis structure (all improvements)

1. **Socket/address axis fully neutralized.** Core + protocol layers now speak a Keel-owned,
   fixed-layout `KlSockAddr` (`include/keel/sockaddr.h`); a platform `struct sockaddr` exists
   **only** inside socket providers, marshalled at the single `src/sockaddr_native.h` boundary.
   This *dissolved* the compile-time-socket-ABI coupling a stricter reading of Goal 5 would have
   flagged before (a host-layout `sockaddr` baked into `KlConn`/`KlUdp`/resolver structs). It is
   **mechanically enforced**: `make check-sockaddr-neutral` confirms 12 protocol TUs are
   KlSockAddr-only, and `sockaddr_native.h` is `#include`d **only** by the marshalling boundary
   (socket providers, the `udp_io` TUs, the completion backends, the resolver seam, server bind):
   never by a protocol/core TU (verified this pass).

2. **Event axis is now runtime-injectable**, symmetric with the socket axis: `KlEventProvider`
   (#150/#151) installs a backend via `KlEventCtx`/`KlConfig`, and a backend's
   `native_provider()` auto-wires its matched socket provider. Previously the event backend was
   compile-time-only.

3. **The `udp_io` seam speaks KlSockAddr** (this session): the datagram-I/O boundary and the
   queued-datagram node carry `KlSockAddr`; each platform `udp_io` TU marshals its own host
   layout, exactly mirroring the socket vtable. `KlUdp` no longer embeds a host `sockaddr_storage`
   (recv scratch moved to stack locals), the struct is now layout-neutral, which is what lets a
   foreign stack share its ABI.

4. **TLS transport is socket-axis-agnostic.** The mbedTLS socket-BIO now routes ciphertext through
   `kl_sock_send`/`kl_sock_recv(t->sp, …)` (opt-in via `kl_tls_mbedtls_ctx_set_socket_provider`),
   so TLS composes with **any** socket provider. Crucially this is a clean axis split: `bio_send`/
   `bio_recv` check `comp_mode` **first** and return on the memory-BIO path, so the provider (`sp`)
   only ever affects the readiness/socket-BIO path, the completion (`feed_input`/`drain_output`)
   transport is untouched. The generic `KlTls` vtable is **unchanged** (the provider is
   integration-config, not a vtable field), so no event/socket model leaks into the protocol
   contract.

### Execution-path re-trace (address marshalling, both axes)

- **Readiness UDP receive:** `kl_udp_io_recv_drain` (udp_io_posix/win) `recvmsg`/`recvfrom` into a
  stack `sockaddr_storage` scratch → `kl_sockaddr_from_native` → `kl_udp_deliver(src: KlSockAddr)`
  → `on_recv`. Host sockaddr never escapes the TU.
- **Completion UDP receive:** backend (`event_iouring`/`iocp`/`pollcomp`) fills the host `peer`
  into `KlCompletionEvent` → `completion_driver.c` `KL_COMP_UDP_RECV` marshals **once** via
  `kl_sockaddr_from_native` → `kl_udp_comp_on_recv(src: KlSockAddr)` → same `kl_udp_deliver`. The
  neutral address is the single currency the model-blind delivery shares across axes.
- **TLS over a foreign socket provider:** `connection.c` `conn_read/write` → `KlTls.read/write`
  → mbedTLS `bio_recv/bio_send` → `kl_sock_recv/send(t->sp=lwIP)` → `lwip_recv/lwip_send`. No host
  fd assumption anywhere on the path.

### Findings

| # | Severity | Area | Finding | Status |
|---|----------|------|---------|--------|
| A1 | Medium | `sockaddr_native.h` marshalling boundary | The neutralization concentrates all untrusted host→neutral conversion into one seam, which makes that seam's robustness load-bearing. Two defects there (missing `len` lower-bound before the `sockaddr_in{,6}` cast → OOB read; uninitialised `KlSockAddr` delivered when `from_native` fails) were the highest-value issues of the companion C audit. | **Fixed** in the 7th C-audit pass (`docs/keel_audit.md`, commit this session). Architecturally, the fix reinforces the boundary contract: *the marshalling seam must reject/deflect any address it cannot represent, never emit garbage upward.* |
| A2 | Informational | `udp_io` injection is link-time, not a runtime vtable | **UDP fully works on lwIP** (`udp_io_lwip.c`, CI-tested loopback echo), this is **not** a functional gap. The only asymmetry: the socket + event axes are **runtime** vtables (one binary can hold both the posix and lwIP providers and choose per-`KlEventCtx`), whereas `udp_io` is **link-selected** (the linker resolves `kl_udp_io_*` once; a foreign stack link-overrides `udp_io_*.o`, as lwIP does). A runtime `KlUdpIoProvider` would only add *per-socket mixed-stack UDP within one process* (some datagrams on the kernel, some on lwIP simultaneously), not a real use case, since a process runs on one network stack. **No action needed.** | Accepted: justified no-action. |
| A3 | Low | TLS provider-routing should be a generic `KlTls` hook | Socket-provider routing was opt-in **per TLS backend** (`kl_tls_mbedtls_ctx_set_socket_provider`), so each future backend would reinvent it and the app had to manually match the TLS ctx's provider to the connection's `KlConfig.sockets`/`KlClientConfig.sockets`, a footgun: forget it and TLS silently falls back to **host** sockets on a foreign stack. | **Fixed**, added an optional `KlTls.set_socket_provider` vtable method that `connection.c` (`kl_conn_on_handshake`) and `client.c` (sync + async) **auto-wire** from the connection's own provider before the handshake; mbedTLS implements it. TLS-over-any-provider now works with zero per-app config; the generic contract stays neutral (backends may leave it NULL). Proven: the lwIP HTTPS loopback dropped its explicit ctx calls and still handshakes over lwIP. |
| A4 | Informational | completion UDP backpressure accounting | `udp.c` reserves `q_bytes` on `kl_comp_post_udp_send` and releases it in `kl_udp_comp_on_send`. This relies on every posted overlapped send surfacing exactly one completion (incl. cancellation-via-close). The backends honor that (a cancelled/failed `WSASendTo`/`sendmsg` still surfaces `KL_COMP_UDP_SEND` with the reserved `len`), so the reservation cannot leak, but the invariant is contract-enforced, not structurally guaranteed. | Accepted; documented in the contract (§5, "completion delivery"). |
| A5 | Informational | error normalization across providers | The Winsock `EAGAIN != EWOULDBLOCK` split (`kl_wsa_set_errno`) is safe only because every would-block test ORs both codes. Cross-provider error normalization otherwise routes through `kl_sock_errno_to_error` → `KlError`. | Accepted (see C-audit R1); keep the OR convention. |

No Critical/High. No new coupling violations: the mechanical protocol-independence grep is clean
(protocol TUs reference `WSARecv`/`WSASend` only in doc comments; no platform-net headers, no
event-engine symbols), and the address-neutrality grep-gate passes.

### Compatibility matrix (updated, lwIP added)

| Combination | Implemented | Buildable | Tested | Production-ready |
|---|---|---|---|---|
| Linux sockets + epoll | ✅ | ✅ | ✅ full suite | ✅ (default Linux) |
| Linux sockets + poll (fallback) | ✅ | ✅ | ✅ full suite | ✅ |
| Linux sockets + io_uring | ✅ | ✅ | ✅ gate + smokes + LSan | ✅ |
| Darwin sockets + kqueue | ✅ | ✅ | ✅ full suite (891) | ✅ (default macOS) |
| Winsock + WSAPoll | ✅ | ✅ | ✅ Windows CI subset | ✅ |
| Winsock + IOCP | ✅ | ✅ | ✅ lifecycle + smokes | ⚠️ plaintext prod; real-mbedTLS BYO/out-of-CI |
| pollcomp (portable completion double) | ✅ | ✅ | ✅ smoke + ASan/LSan | n/a; test double |
| **lwIP + lwip_poll (readiness), server + client + UDP + TLS** | ✅ | ✅ | ✅ **loopback + HTTPS runtime test in CI** (Integration (lwIP), stock libkeel) | ⚠️ reference/BYO, sample-tunable `lwipopts.h`, CSPRNG-for-`LWIP_RAND` required per deployment |

lwIP is **readiness-only** (its sockets layer is `lwip_poll`); the lwIP *raw* callback (completion)
API remains out of scope. Still not built (by design): Winsock+io_uring (N/A), IOCP+non-Winsock
(N/A), UEFI SNP (future).

### Roadmap delta

- **Immediate correctness:** none open (A1 fixed this session in the C-audit pass).
- **A3, done:** added the optional `KlTls.set_socket_provider` vtable method, auto-wired from the
  connection's provider in `connection.c`/`client.c` (mbedTLS implements it). The TLS transport is
  now socket-provider-agnostic *by the framework* rather than by per-app config, and the
  host-socket-fallback footgun on a foreign stack is gone.
- **A2, no action:** a runtime `KlUdpIoProvider` vtable is **not** pursued. UDP-on-lwIP already
  works via the link-override; the only thing a runtime vtable would add (mixed-stack UDP within one
  process) is not a real use case.
- **Everything else** from the prior passes' roadmaps stands unchanged.

---

## Fourth pass: PROXY / streaming / TransmitFile over completion; full parity (2026-08-01)

**Verdict: architecturally sound, the completion axis is now at full functional AND test parity
with readiness, with no new coupling violations.** This pass re-runs the orthogonality litmus and
assesses the code added since the third pass (PRs #130 streaming HOL, #133 TransmitFile chunking,
#136 PROXY-over-completion; #135 TLS suites over completion). All three axes remain separately
replaceable; protocols contain no platform/event logic; the new completion features reuse the
model-blind core rather than duplicating it.

**Mechanical litmus (all clean):** grep of the protocol TUs (`connection.c`, `h2.c`, `websocket.c`,
`response.c`, `sse.c`, `client.c`, `h2_client.c`, `websocket_client.c`, `router.c`, `cors.c`,
`redirect.c`, `chunked.c`, `body_reader_multipart.c`, `parsers/*.c`) finds **no** platform
networking / event headers (`sys/epoll`, `sys/event`, `liburing`, `winsock2`, `windows`,
`mswsock`, `io_uring.h`), **no** engine symbols (`epoll_*`, `kevent`, `io_uring_*`, `WSARecv/Send`,
`OVERLAPPED`, `GetQueuedCompletion*`), and **no** `completion.h` / `kl_comp_*` / `KL_COMP_*` leak.
Protocols stay above both axes.

**New-code orthogonality (assessed correct):**
- **PROXY-over-completion (#136).** The model-blind parse (`kl_proxy_parse`) is shared; only the
  I/O wrapper is axis-specific: readiness `kl_conn_read_proxy_header` (socket peek+consume) vs.
  completion `kl_conn_ingest_proxy` (parse from `read_buf`). This is the *correct* axis split (same
  shape as recv itself: shared parser, per-axis I/O), not duplication of a state machine. The
  accept gate (`comp_on_accept` → `kl_cidr_match`) and the header phase (`comp_drive_proxy`) live in
  `completion_driver.c`, the integration layer, mirroring the readiness gate in `server.c`; no
  protocol TU is touched.
- **Streaming HOL fix (#130).** `comp_stream_pump` drives the transport-neutral `KlDrain` via
  `kl_comp_post_send`; producers (SSE / chunked / response) write to the drain, never a socket or
  engine. Backpressure is the drain cap + `stream_inflight` (≤1 send), model-independent. The two
  new `KlDrain` accessors (`kl_drain_data`/`kl_drain_consume`) are generic buffer ops.
- **TransmitFile chunking (#133).** Entirely inside `event_iocp.c`; the offset-advancing re-post is
  internal and surfaces a single `KL_COMP_WRITE` to the driver, the completion abstraction (one
  op = one whole-transfer completion) is preserved. IOCP-only; io_uring (splice) / pollcomp
  (pread+send) have no DWORD limit.

**Finding (Informational, I1):** the completion backends' `kl_comp_post_recv`
(`event_pollcomp.c:224`, `event_iouring.c:421`, `event_iocp.c:255`) now branch on
`c->state != KL_CONN_PROXY_HEADER` to recv plaintext before TLS, extending the pre-existing
`c->tls` branch. This has the backend read a protocol/connection state enum. It is **consistent
with the established, deliberate coupling**, the completion backends are not pure socket providers;
they are the completion-axis connection drivers that already operate on `KlConn` (8–11 field refs
each: `c->tls`, `c->read_buf`, `c->read_len`, `c->read_cap`). So this is not a new axis violation.
An optional future cleanup would replace both conditions with one driver-owned boolean (e.g.
`c->tls_active`, false during the PROXY header) so the backend needn't name a protocol state; not
worth a change now. No fix applied.

**Changes made this pass:** none to the axis (report-only, orthogonality is sound). Separately,
the concurrent c-audit sixth pass fixed two Low allocator free-size mismatches on a zero-length
completion send (`pc_op_free`, `iocp_op_free`); see `docs/keel_audit.md`.

**Exercised:** `make debug-test` (55 suites, ASan+UBSan, 0 failures / 0 sanitizer hits);
`smoke-pollcomp` + `-asan` (GET/POST/file/stream/**bigstream**/UDP/h2/**proxy**); real io_uring in
the Apple container (`test-iouring` incl. `peer_addr` 6/6 + the four TLS suites; `smoke-iouring`);
`cppcheck` clean; MinGW compile-gate on the Windows TUs. IOCP is exercised by the Windows CI
`smoke-iocp` (GET/POST/file/**bigfile-chunked**/stream/**bigstream**/**proxy**/UDP/udp-local) +
`smoke-iocp-tls`.

### Compatibility matrix (updated)

| Combination | Implemented | Buildable | Tested (CI) | Production-ready |
|---|---|---|---|---|
| Linux sockets + epoll | ✅ | ✅ | ✅ full suite + smoke | ✅ (default Linux) |
| Linux sockets + poll (fallback) | ✅ | ✅ | ✅ full suite | ✅ |
| Linux sockets + io_uring (completion) | ✅ | ✅ | ✅ 49-suite gate (incl. TLS mock + PROXY) + smokes + **LSan** | ✅ (default io_uring, 8f-5) |
| Darwin sockets + kqueue | ✅ | ✅ | ✅ full suite (macOS CI) | ✅ (default macOS) |
| Winsock + WSAPoll | ✅ | ✅ | ✅ 47-suite subset (Windows CI) | ✅ |
| Winsock + IOCP (completion) | ✅ | ✅ | ✅ lifecycle + smokes: plaintext, **TLS-via-mock**, **PROXY**, **chunked TransmitFile**, UDP-local | ⚠️ prod-ready plaintext + PROXY; real-mbedTLS TLS is BYO/out-of-CI (F3) |
| pollcomp (portable completion double) | ✅ | ✅ | ✅ smoke + tls/ws/async + **proxy** + **bigstream**, ASan/LSan | n/a; **test double** |

Net vs. the third pass: the completion row's *Tested* column now covers TLS (mock), PROXY, and
chunked file bodies, the earlier "per-suite behavioural gaps" are closed. The only standing
limitation is real-mbedTLS-over-IOCP (F3, BYO/out-of-CI by policy).

---

## Third pass: completion test-coverage-gap triage (2026-08-01)

**Verdict: no hidden backend bug.** This pass triaged the low/informational test-coverage gaps
from §3 (F2/F3/F4), the default-provider unit suites *excluded* from the completion test set
(`IOURING_TEST_SUITES`): by running each over the completion backend (pollcomp, the shared
`completion_driver.c` double) and root-causing every failure. All but one are fixture limitations
or inherent axis-semantics differences, not backend defects; the one real functional gap (PROXY
protocol over a completion loop) is now made safe (fail-loud at init) rather than silently
mis-served.

**Per-suite triage (excluded default-provider suites, run over pollcomp):**

| Suite | Over completion | Root cause | Disposition |
|---|---|---|---|
| `tls_integration`, `peer_cert`, `cross_module`, `unix_socket` | TLS tests failed | Each defined its **own per-file passthrough mock TLS** that implemented only the *readiness* vtable (`read`/`write` on the socket), not the completion-mode `feed_input`/`drain_output`; `comp_on_accept` correctly rejects a TLS conn whose backend can't do memory-BIO mode. **Not a backend bug.** | **Now enabled (2026-08-01 follow-up):** ported all four to the shared completion-capable `tests/mock_tls.h` (the same mock `smoke-pollcomp-tls`/`smoke-iocp-tls` use; `peer_cert` installs its canned cert via the new `mock_tls_peer_cert_fn` hook). They now pass over readiness *and* completion (pollcomp + real io_uring) and are in `IOURING_TEST_SUITES` (44 → 48). Unit-level TLS-over-completion coverage, complementing the smokes. |
| `peer_addr` | 4/6 pass; `proxy_v1/v2_trusted` fail | **Real gap (now fixed):** PROXY-protocol header handling (`KL_CONN_PROXY_HEADER`) originally lived only in the readiness run loop; the completion driver had no PROXY-header phase, so a trusted-source PROXY header was misparsed as HTTP. | **Fully supported (2026-08-01 follow-up):** the completion driver grew a PROXY-header phase; `comp_on_accept` enters `KL_CONN_PROXY_HEADER` for a trusted peer, `comp_drive_proxy` parses the plaintext header from `read_buf` via the model-blind `kl_conn_ingest_proxy` and then enters the real initial state (TLS handshake feeding the buffered ClientHello, or HTTP read). The header recv is plaintext even for a TLS conn (`post_recv` skips the TLS branch while `state == KL_CONN_PROXY_HEADER`, all three backends). `peer_addr` now passes 6/6 over completion and is in `IOURING_TEST_SUITES`; a PROXY-v1 roundtrip is in `smoke-pollcomp` + `smoke-iocp`. The #134 fail-loud init guard was removed. |
| `udp_multicast` | 4/5 pass; `broadcast_flag_gates_send` fails | Inherent axis semantics: the test asserts a *synchronous* `EACCES` on a broadcast send without `SO_BROADCAST`, which only holds for readiness; completion sends are queued async, so the error surfaces on the send completion, not the post. **Not a backend bug.** | Excluded by design (already documented in the Makefile). |
| `async`, `event`, `event_ctx`, `event_caps`, `socket_provider` | n/a | Inherently readiness-axis: raw `kl_event_wait` drivers / readiness-cap + provider-negotiation assertions (a completion loop has no `kl_event_wait`, only `kl_comp_run`); `async` also hand-builds a `KlConn` with a NULL `ctx`. | Excluded by design (not applicable to a completion loop). |

**F3 (mbedTLS-over-IOCP, real TLS):** unchanged; BYO / out-of-CI. The IOCP TLS *code path* is
exercised by `smoke-iocp-tls` (the completion-mode `tests/mock_tls.h`); real mbedTLS-over-IOCP is
a bring-your-own concern consistent with the mbedTLS policy. Accepted, not a gap to close in CI.

**F4 (`test_async` over completion):** confirmed a test-harness artifact (a hand-built conn with
NULL `ctx` driven through the resume path; would equally hit any completion backend), not a
backend bug. The async-over-completion path itself *is* covered: `smoke-pollcomp-async` and the
io_uring `/astream` case (#127) drive `kl_async_suspend`/resume + timer over the completion loop.
`test_async` stays excluded (a completion-aware async fixture would be the enabling work).

**Net:** the completion backends are now at functional **and** test parity with readiness. The
TLS-touching suites were ported to the shared completion mock, and PROXY-over-completion was
implemented (`comp_drive_proxy` + `kl_conn_ingest_proxy`), so `IOURING_TEST_SUITES` grew 44 → 49
(the four TLS suites + `peer_addr`). The only remaining excludes are inherent axis-semantics or
readiness-axis tests: `udp_multicast`'s `broadcast_flag_gates_send` (asserts a synchronous EACCES;
completion sends are async) and the raw-`kl_event_wait` / provider-negotiation driver suites
(`async`, `event`, `event_ctx`, `event_caps`, `socket_provider`). The one accepted limitation is
BYO mbedTLS-over-IOCP (F3, out-of-CI by policy).

---

## Second pass: completion-axis parity + robustness follow-up (2026-07-31)

**Verdict: unchanged; architecturally sound.** No new architectural findings; the orthogonality
map, honest-model-representation, and grep litmus from the first pass all still hold. This pass
records two completion-axis items closed since 2026-07-30 and one Windows-IOCP robustness fix, and
re-confirms the remaining gaps are the same low/informational items (no criticals).

**Closed since the first pass:**

- **Async / long-lived streaming over the completion loop is proven, not assumed (#127, Phase
  8g-2).** `tests/smoke_iouring.c` gained an `/astream` handler that begins a chunked stream,
  writes a chunk, `kl_async_suspend`s on a `kl_timer_add` deadline, then resumes on the completion
  loop to write the tail + end. It exercises begin-stream → suspend → timer-fire → resume →
  end-stream entirely over io_uring, closing the open question from the 8g scoping (the outbound
  stream buffer was wired by default in #126/8g-0).

- **Streaming head-of-line blocking over the completion loop is fixed (#130, Phase 8g-1), the
  last *functional* gap in the completion backends is now closed.** `comp_send_stream` used to
  flush a streaming response by busy-spinning `kl_drain_flush`; on a slow client whose socket send
  buffer is full, the non-blocking send returned would-block while `stream_ended`, spinning the
  loop thread and starving every other connection. Streaming now rides the same overlapped drive as
  buffered/file responses: `comp_stream_pump` posts the outbound buffer as one overlapped send
  (`kl_comp_post_send` copies it; the backend surfaces one `KL_COMP_WRITE` when it is fully out) and
  `comp_on_write` re-pumps: posting freshly-produced bytes or completing once the stream ended and
  the buffer drained. At most one send is in flight (`KlResponse.stream_inflight`), memory stays
  bounded by the drain cap, and the loop is free between sends. Purely completion-side
  (`completion_driver.c` + two `KlDrain` peek/consume accessors); readiness, protocols, and the
  public streaming API are untouched. A `/bigstream` slow-reader-plus-concurrent-fast-client HOL
  test on all three completion smokes (pollcomp/iouring/iocp) guards it; validated on pollcomp+ASan,
  io_uring+LSan (container), and the Windows-IOCP CI job.

- **UDP datagram local (dest) address parity on the completion backends is complete (#128).** IOCP
  now captures the datagram's local address via an overlapped `WSARecvMsg` + `IP_PKTINFO` control
  message (`kl_udp_win_get_recvmsg` / `kl_udp_win_parse_local` in the Windows-only, include-scoped
  `src/udp_cmsg_win.h`, shared byte-for-byte with the readiness recv), matching io_uring and
  pollcomp (#118/#119). Previously `kl_comp_post_udp_recv` used `WSARecvFrom` (source only), so
  `kl_udp_send_to_from` reply-from delivered a NULL local over IOCP. The Keel-level `KlUdpRxMeta`
  contract (source + local + gro_seg + truncated) is now honoured identically across all three
  completion backends, a parity win *above* the axis, with the platform mechanics staying in the
  Windows TUs + the include-scoped header (no `#ifdef` in cross-platform code, no public-API change).

**Robustness fix made this pass (unlike the report-only first pass):** the overlapped `WSARecvMsg`
posted for UDP recv over IOCP leaves an outstanding op that `closesocket` cancels at teardown; it
completes with `bytes == 0` and an *unfilled* (zeroed) `WSAMSG` control buffer. The completion path
parsed that buffer unconditionally, and the cmsg walk spun forever (`WSA_CMSG_NXTHDR` advances by
`WSA_CMSG_ALIGN(cmsg_len)`, so `cmsg_len == 0` yields the same pointer), wedging the loop thread so
`kl_server_run` never returned. `WSARecvFrom` never read control data, so this only surfaced once
`WSARecvMsg` landed. Fixed two ways: `kl_udp_win_parse_local`/`udp_parse_tos` now stop the walk on
a runt cmsg (`cmsg_len < sizeof(WSACMSGHDR)`), a general loop-safety fix that also hardens the
readiness path, and the IOCP `KL_IOCP_UDP_RECV` handler only parses source + pktinfo metadata when
`bytes > 0` (a cancelled/failed op carries no valid name/control). This maps to Goal 6 (op
lifetime: close-while-outstanding) and Goal 7 (partial/zero-length completions handled honestly):
a completion event is not a received datagram. Validated by the Windows-IOCP `smoke-iocp` job,
which now prints `... + UDP + udp-local` after enabling `recv_pktinfo` and asserting the local
address is delivered.

**Matrix delta:** Winsock + IOCP (completion) UDP now has full datagram-metadata parity (source +
local/pktinfo + truncation), matching io_uring/pollcomp; the §4 row is otherwise unchanged
(plaintext prod-ready; real-mbedTLS TLS remains BYO/out-of-CI, F3).

**Also closed since the first pass:** IOCP file bodies larger than TransmitFile's ~2 GiB per-call
cap are now sent as several offset-advancing `TransmitFile` chunks re-posted from the
`KL_IOCP_SENDFILE` completion (`iocp_post_transmitfile_chunk`), transparent to the driver (one
`KL_COMP_WRITE` when the whole head+file is out; ≤1 op in flight). Previously such a body was
rejected. A `/bigfile` smoke case with a test-lowered per-call cap (`KEEL_IOCP_TF_CHUNK`) verifies
the multi-chunk path delivers every byte at the right offset without needing a >2 GiB fixture.

**Still open (all low/informational, see §3/§6):** F2 (per-suite triage so the default-provider
integration suites run over completion), F3 (a self-hosted mbedTLS Windows-IOCP TLS smoke, or
document the BYO gap), F4 (a completion-aware async test fixture). Non-functional, explicitly
deferred: io_uring multishot recv / registered buf-rings (benchmark showed no current need). (The
completion-streaming HOL and TransmitFile >2 GiB items are no longer open; closed by #130 / 8g-1
and the TransmitFile chunking above.)

---

## First pass: event / socket / protocol orthogonality (2026-07-30)

**Verdict: architecturally sound.** The event axis (readiness + completion), the socket/
platform axis, and the protocol layer are genuinely orthogonal. The mechanical litmus tests
pass, both event models are represented honestly, and every supported backend combination is
CI-tested (not merely present). No critical/high findings. The two real defects that a
lifetime/semantics audit would surface, an io_uring watch leak and a completion graceful-drain
hang: were already found + fixed earlier the same day (PRs #111, #113); they are recorded here
as resolved. Remaining items are test-coverage gaps (low) and one informational API note.

**Method:** repo map + mechanical grep litmus (protocol TUs vs platform headers/event symbols;
backend vs protocol symbols; public-header leak scan; Winsock-parity scan) + end-to-end path
traces over a readiness backend and a completion backend + review of ownership/lifetime,
partial-I/O, backpressure, cancellation, error-normalization, and CI backend coverage. Drew on
the just-completed 8d–8f completion migration + `/c-audit` fifth pass.

---

## 1. Current architecture map

**Event axis** (`BACKEND=` selects one at build time):
- Interface: `include/keel/event.h` (readiness: `kl_event_init/add/mod/del/wait/close` over
  `KlEventLoop` + `KlEvent{void *udata; KlEventMask ready;}`). Capability surface (internal):
  `src/event_caps.h` (`KL_EVENT_CAP_READINESS | _NATIVE_FD | _COMPLETION`, `kl_event_caps`,
  `kl_caps_compatible`, `kl_event_ctx_sockets_compatible`).
- Completion axis (peer to readiness): `src/completion.h` (abstract `KlCompletionEvent{target,
  kind, bytes, ok, accepted_fd, buf, peer}`, `kl_comp_post_recv/send/accept/sendfile/udp_*`,
  `kl_comp_drain`) + `src/completion_driver.c` (the platform-independent connection driver +
  `kl_io_engine_run_completion`) + `src/io_engine.h` (run-loop dispatch seam).
- Readiness backends: `event_epoll.c`, `event_kqueue.c`, `event_wsapoll.c`, `event_poll.c`.
- Completion backends: `event_iouring.c` (Linux, SQE/CQE), `event_iocp.c` (Windows), and
  `event_pollcomp.c` (portable `poll()` facade, the CI/ASan test double). Each implements
  `kl_event_caps` + `kl_event_native_provider` + the `completion.h` post/drain contract.

**Socket axis:** `src/socket.h`; `KlSocketProvider {const KlSocketOps *ops; void *context;
unsigned capabilities;}` (`KL_SOCK_CAP_NATIVE_FD | _OVERLAPPED`). Providers: `socket_posix.c`,
`socket_winsock.c`; overlapped providers (`kl_socket_provider_iouring/iocp/pollcomp`) live in
their event TUs. Native handle: `KlSocketHandle = intptr_t` (`include/keel/handle.h`),
`KL_INVALID_SOCKET = (KlSocketHandle)-1`, tested via `kl_handle_valid()` (never `< 0`). Selected
on `KlEventCtx.sockets` + public `KlConfig.sockets` / `KlClientConfig.sockets`. Error taxonomy:
`kl_sock_errno_to_error` (+ `kl_wsa_set_errno` on the Winsock seam).

**Protocol layer:** `connection.c` + `conn_internal.h` model-blind core (`kl_conn_dispatch_request`,
`kl_conn_ingest_body`, `kl_conn_send_complete`, `kl_conn_on_readable`), `h2.c`, `websocket.c`,
`response.c`, body readers, `chunked.c`, `cors.c`, `sse.c`, client transports (`client.c`,
`h2_client.c`, `websocket_client.c`), `KlTls` vtable. All server I/O flows through
`conn_read`/`conn_write` (`internal.h`) which branch TLS-or-plain and call the socket seam.

**How they connect:** the server run loop (`kl_server_run`) detects the axis once via
`kl_event_caps` → readiness branch (`kl_event_wait` + `kl_event_dispatch`) or completion branch
(`kl_io_engine_run_completion` → `kl_comp_run`). Both drive the *same* model-blind protocol core.
`kl_server_init` negotiates the loop against the provider (`kl_event_ctx_sockets_compatible`) and,
since 8f-5a, auto-adopts the backend's overlapped provider (`kl_event_native_provider`) when a
completion loop is paired with the default provider, so protocols/consumers never name the axis.

---

## 2. Execution-path traces

**Readiness receive (epoll/kqueue):** `kl_server_run` → `kl_event_wait` reports `KL_EVENT_READ`
→ `kl_event_dispatch` (untagged udata = conn) → `kl_conn_on_readable` → `conn_read` (`tls->read`
or `kl_sockdef_recv`) → `kl_conn_ingest_body`/`kl_conn_dispatch_request` → handler → response →
`kl_event_mod` re-arms interest / keep-alive. EAGAIN → return, wait again.

**Completion receive (io_uring/pollcomp):** driver posts `kl_comp_post_recv` (io_uring:
`io_uring_prep_recv` into `read_buf` slice with the op as `user_data`; TLS: into a cipher buffer)
→ `kl_comp_drain` (`io_uring_submit_and_wait_timeout`, reap CQE, skip the `LIBURING_UDATA_TIMEOUT`
+ cancel sentinels) → `iou_complete` maps `cqe->res` → `KL_COMP_READ{target=conn, bytes, ok}` →
`kl_comp_run` → `comp_on_read` → (TLS: `feed_input` then `comp_tls_drive`) → the *same*
`kl_conn_dispatch_request`/`ingest_body` core → `kl_comp_post_recv` (next) or pause. No synthetic
readiness; bytes are real kernel-delivered data.

**Accept:** readiness; `kl_event_wait` on the listen fd → `accept` via the seam →
`kl_conn_acquire`. Completion: `kl_comp_post_accept` (`io_uring_prep_accept` / `AcceptEx`) →
CQE → `KL_COMP_ACCEPT{accepted_fd, peer}` → `comp_on_accept` → `kl_conn_acquire` +
`kl_comp_post_recv` + `kl_comp_post_accept` (refill; single accept outstanding).

**Send + backpressure:** readiness; `kl_response_send`/`try_writev` with `send_offset` for
partial writes; on EAGAIN the remainder buffers in `KlDrain` (bounded, `on_drain` callback);
listen paused (`listen_paused`) when the pool is full. Completion: `kl_comp_post_send` copies the
iovec into an owned buffer (or a registered pool buffer, io_uring 8f-2); a short `cqe->res` re-preps
the tail (`iou_prep_send_tail`) so only a fully-completed send surfaces a `KL_COMP_WRITE`; UDP send
queue caps outstanding overlapped bytes. Equivalent Keel-level semantics, different mechanism.

**Close with outstanding work:** idle sweep `kl_server_sweep_conn_timeouts` → `kl_comp_cancel`
(io_uring `prep_cancel` by `user_data` + `aborted` flag) → the op completes `-ECANCELED`/error →
`comp_close` releases the conn through its normal completion (invariant: a conn is released only
from a completion: no dangling op, no double release). A removed readiness watch is kept LINKED in
`st->watches` until its poll-cancel CQE frees it, or `kl_event_close` frees it at shutdown
(the #111 fix, otherwise it leaked). Graceful drain runs in *both* run-loop branches via
`kl_server_drain_progress` (the #113 fix, the completion branch previously never exited drain).

---

## 3. Findings

| # | Sev | Files/symbols | Principle | Assessment | Smallest fix |
|---|-----|---------------|-----------|------------|--------------|
| F1 | **Informational** | `include/keel/event.h` `KlEventLoop.fd` (public `int`, "epoll_fd or kqueue_fd, -1 for io_uring") | G3 (no event-model leak) | Not a real leak: `KlEventLoop` is the loop object, not consumed by protocols; the field is documented backend-internal and frozen (PAL Appendix A). No protocol reads it. A future non-fd backend (lwIP raw) would want a portable handle here. | None now; revisit at Phase 9 (lwIP raw) with a `KlSocketHandle`-style widening if needed. |
| F2 | **Low** | `IOURING_TEST_SUITES` (Makefile) vs the ~14 default-provider suites (`client`, `client_stream`, `redirect`, `peer_*`, `tls_integration`, `udp*`, `unix_socket`, `dns_resolver`, `request`, `cross_module`) | Decision std: combos *tested*, not assumed | Test-coverage gap, not an architecture defect. These init over completion (5a) but have per-suite behavioural gaps; the completion backend is covered by 36 unit suites + full smokes + benchmark. | Incremental per-suite triage (deferred, documented in `phase8f5` §3). |
| F3 | **Low** | Windows + IOCP + **real mbedTLS** TLS runtime (BYO, out of CI) | Decision std: tested | The IOCP TLS *code path* is exercised via the identity mock-TLS (`smoke-iocp-tls`); real mbedTLS-over-IOCP is not CI-gated (mbedTLS is BYO everywhere). Consistent with the mbedTLS policy, but the combo's production-readiness is asserted, not proven. | A local/self-hosted `KEEL_TLS=mbedtls` Windows-IOCP smoke, or document the gap explicitly (already noted in `phase6_winsock_design.md`). |
| F4 | **Informational** | `tests/test_async` over completion (manual `KlConn` with NULL `ctx`) | G6 (lifetime) / testing | Crashes over completion because the test hand-builds a conn without `ctx` and drives the resume path, a test-harness artifact (would equally hit pollcomp/IOCP), not a backend bug. | A completion-aware async test fixture (or exclude, as now). |
| R1 | **Resolved (this session)** | `event_iouring.c` `kl_event_del` | G6 (lifetime) | A removed watch with an in-flight `POLL_ADD` was unlinked + its free deferred to a CQE dropped at `io_uring_queue_exit` → 48 B leak/server. Fixed (#111): keep linked, free in CQE or `kl_event_close`. LSan-clean. | Done. |
| R2 | **Resolved (this session)** | `server.c` completion run-loop branch | G2/G7 (semantics) | Graceful drain (`draining` → close-idle/deadline → stop) ran only in the readiness path, so a completion-loop server with `drain_timeout_ms` never exited drain → deadlock. Fixed (#113): shared `kl_server_drain_progress` in both branches. | Done. |

**Clean (verified, no finding):**
- **G1**: event backends reference zero protocol symbols; socket providers reference zero event
  engines/protocols (only a descriptive comment). No `#ifdef` merges the two axes (Makefile
  selects `EVENT_SRC` and `SOCKET_SRC` independently; POSIX socket TU builds under every POSIX
  event backend).
- **G4**: protocol TUs import **no** platform net/event headers, make **no** direct
  epoll/kqueue/io_uring/WSA/OVERLAPPED calls, and do **no** raw socket syscalls (all via
  `conn_read`/`conn_write` + the seam). Mechanically grep-clean.
- **G2**: completion delivers real kernel bytes (never synthetic readiness); readiness handles
  EAGAIN and never claims completion on mere readability. The event.h `kl_event_wait` on a
  completion loop is a documented no-op (the loop uses `kl_comp_run`).
- **G3**: no event-model mechanics in `include/keel/` consumable types; `KL_EVENT_CAP_COMPLETION`
  / `KL_SOCK_CAP_OVERLAPPED` are internal. Readiness/completion are explicit *internal* axis
  concepts; protocols consume the stable `KlConn`/`conn_read`/`conn_write` contract.
- **G5**: `KlSocketHandle = intptr_t`, `KL_INVALID_SOCKET`, `kl_handle_valid()` (never `<0`);
  `closesocket`/`WSAGetLastError` are confined to `event_iocp.c` + the `*_win` TUs; the seam has
  real `writev`/`sendfile` ops (POSIX + `WSASend`/`TransmitFile`). Winsock is first-class.
- **G6/G7**: single in-flight op per conn (driver invariant); partial send re-prep in every
  backend; sentinels handled; release-only-from-completion. (R1/R2 were the exceptions, now fixed.)
- **G8**: `KlDrain` (bounded write buffer + `on_drain`), `listen_paused` accept gating, capped
  UDP send queue, growable-but-capped read buffer (`max_header_size`): model-independent.
- **G9**: `kl_sock_errno_to_error` maps to stable `KlError`; the Winsock seam translates
  `WSAGetLastError` → errno first. Equivalent errors across axes.
- **G10**: `kl_comp_cancel` (native `ASYNC_CANCEL` + `aborted` sentinel) vs readiness
  registration teardown; idle/read timeouts in both branches; release only from a terminal
  completion.
- **G11**: single-threaded event loop per worker (Node/Redis/Nginx model); cross-thread work via
  `KlThreadPool` + a `KlPlatWakeup` self-pipe relayed as `KL_COMP_WATCHER`/watcher; `kl_server_stop`
  wakes the loop from any thread/signal (async-signal-safe write).
- **G12**, registration vs submission preserved: readiness `kl_event_add` = persistent interest;
  completion `kl_comp_post_*` = one op with its own buffer/`user_data`; the `KL_COMP_WATCHER` relay
  bridges a readiness watch onto a completion loop without conflating the two.
- **G13**: `BACKEND=` selection; `kl_event_caps` observable; `io_uring_queue_init` failure → init
  `-1` (no silent bad combo); the negotiation rejects incompatible loop×provider pairings.

---

## 4. Compatibility matrix

| Combination | Implemented | Buildable | Tested (CI) | Production-ready |
|---|---|---|---|---|
| Linux sockets + epoll | ✅ | ✅ | ✅ full suite + smoke | ✅ (default Linux) |
| Linux sockets + poll (fallback) | ✅ | ✅ | ✅ full suite | ✅ |
| Linux sockets + io_uring (completion) | ✅ | ✅ | ✅ 36-suite gate + smokes + **LSan** | ✅ (default io_uring path, 8f-5) |
| Darwin sockets + kqueue | ✅ | ✅ | ✅ full suite (macOS CI) | ✅ (default macOS) |
| Winsock + WSAPoll | ✅ | ✅ | ✅ 47-suite subset (Windows CI) | ✅ |
| Winsock + IOCP (completion) | ✅ | ✅ | ✅ lifecycle + smokes (plaintext, TLS-via-mock) | ⚠️ prod-ready plaintext; real-mbedTLS TLS is BYO/out-of-CI (F3) |
| pollcomp (portable completion double) | ✅ | ✅ | ✅ smoke + tls/ws/async + ASan/LSan | n/a; **test double**, not a production backend |

Not built (by design / future): Winsock+io_uring (N/A), IOCP+non-Winsock (N/A), lwIP-raw
(Phase 9), UEFI (Phase 10).

---

## 5. Proposed internal contract (from the existing design)

- **Socket ownership**, a `KlConn`/`KlUdp` owns its `KlSocketHandle` for its lifetime; the
  `KlSocketProvider` is *borrowed* (must outlive its transports; owner calls
  `kl_socket_provider_destroy` after). Close routes through `kl_sock_close` (→ `closesocket` on
  Winsock).
- **Event-loop affinity**: one `KlEventCtx` per thread; a socket/op/watcher belongs to the loop
  it was registered/posted on. Cross-thread work enters via `KlThreadPool` + `KlPlatWakeup`; no
  op is submitted or completed off-loop.
- **Readiness notification**: level/edge interest via `kl_event_add/mod`; the consumer performs
  the op and handles EAGAIN; re-arm via `kl_watcher_rearm`/`kl_event_mod`.
- **Completion delivery**: one `kl_comp_post_*` = one op owning its buffer + `user_data`;
  `kl_comp_drain` surfaces exactly one `KlCompletionEvent` per finished op (partial sends re-prep
  internally; only whole-op completion surfaces).
- **Operation lifetime**, a conn holds ≤1 in-flight op; the op is freed when its completion is
  reaped; a conn is released **only** from a completion (cancel makes the op complete with error).
- **Cancellation**: `kl_comp_cancel` (native `ASYNC_CANCEL` / `poll_remove` + an `aborted`/
  `removed` sentinel); exactly one terminal result per op; late/duplicate CQEs are discarded via
  the sentinel; a removed watch is freed by its cancel CQE or by `kl_event_close`.
- **Timeout races**: idle/read timeouts cancel the pending op; the cancellation completion is the
  single terminal event (a datum arriving first just completes normally).
- **Error normalization**: platform error → `kl_sock_errno_to_error` → stable `KlError`, native
  detail retained; equivalent across axes.
- **Close semantics**: `comp_close`/`kl_conn_release` after the terminal completion; no I/O after
  close; `kl_event_close` tears down the loop + frees any watch whose cancel CQE never arrived.
- **Backpressure**: bounded write buffer (`KlDrain`), accept gating (`listen_paused`), capped
  read/send buffers; identical Keel-level limits regardless of axis.

---

## 6. Recommended incremental roadmap

- **Immediate correctness:** none open (R1/R2 fixed this session; nothing else surfaced).
- **Test coverage:** finish the F2 per-suite triage so the default-provider integration suites run
  over completion; add a completion-aware async fixture (F4); consider a self-hosted mbedTLS
  Windows-IOCP TLS smoke (F3).
- **Small architectural cleanup:** none warranted, the axes are clean; do not refactor.
- **Deferred:** Phase 9 (lwIP raw, a third event model; would revisit F1's `KlEventLoop.fd` as a
  portable handle), Phase 10 (UEFI). QUIC/HTTP-3 rides the existing UDP + completion groundwork.

---

## 7. Changes made

**None.** This pass is report-only, the decision standard is met and no clear, low-risk fix was
outstanding (the two real defects were already fixed under PRs #111 and #113 the same day). The
findings are a test-coverage gap (F2/F3), a test-harness artifact (F4), and one informational
API note (F1); none justify a code change under the "narrow, low-risk, design-clear" bar.

---

## Decision standard: met

Socket + event providers are separately replaceable ✅ · readiness + completion keep native
semantics ✅ · higher layers get consistent Keel-level behavior ✅ · protocols contain no
platform event/socket logic ✅ (grep-clean) · op/buffer/conn lifetimes safe ✅ (LSan-clean after
#111) · cancellation/close/partial-I/O/errors/backpressure explicitly defined ✅ · supported
combos tested, not assumed ✅ (matrix §4). **KEEL's networking architecture is orthogonal and
sound.**
