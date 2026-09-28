# Windows Named Pipes: audit and design

**Status:** implemented: client and listener (each connection a `KlStream`) on the IOCP engine; the
listener arrived after the client, once `KlListener` gained an object handoff family (§5). §1 and §2
are the pre-implementation audit, kept as written; §3 onward describe what was built. Scope: a Windows Named Pipe as a local-IPC ordered
byte stream under the Tier-1 `KlStream` contract, beside the existing POSIX `AF_UNIX` path. Keel knows
nothing about SSH agents or any other consumer protocol.

## 1. Audit answers

### Q1: Is `KlStream` an ordered byte stream, or a disguised socket?

The **contract** is an ordered byte stream. [stream.h](../../include/keel/stream.h) is a hook-driven
state machine: an adapter supplies the writer/submit, arm/disarm, and cancel hooks and drives
`kl_stream_on_recv` / `kl_stream_on_write_complete`. Nothing in the public function surface names a
socket operation.

Socket residue sits **below** the contract:

| Where | Residue | Consequence for pipes |
|---|---|---|
| [stream_detail.h](../../include/keel/stream_detail.h) | `KlSocketHandle fd`, `KlSockAddr peer_addr` | Unused by a pipe stream: `fd` stays `KL_INVALID_SOCKET`, `peer_addr` stays `KL_AF_UNSPEC` ("unavailable", as documented). The detail layout is opt-in and not ABI. |
| [completion.h](../../src/completion.h) `post_recv` / `post_send(KlStream *)` | The IOCP backend casts `stream->fd` to `SOCKET` and calls `WSARecv` / `WSASend` | Pipes must **not** use this seam. |
| [completion_core.c](../../src/completion_core.c) | `KL_COMP_READ` / `KL_COMP_WRITE` route only through `ctx->comp_conn_dispatch`, which the HTTP server installs | There is no generic owner routing for a standalone stream's completions, so a pipe stream needs owner-typed routing. |

The answer is yes: the contract already fits. The completion seam that feeds it is socket-typed and
server-routed, and is the wrong seam to reuse.

### Q2: Can a Named Pipe listener honestly satisfy `KlListener`?

**Not without changing its public contract.** The listener state machine itself (window, credit
reservation, cancel-once, confirmed detachment) is hook-driven and transport-agnostic. The
**handoff** is not: `kl_listener_on_accepted(l, KlSocketHandle fd)`, `KlListenerAcceptFn(ctx,
KlSocketHandle fd, KlSlotLease)` and `KlListenerDisposeFn(ctx, KlSocketHandle fd)` all name the
accepted connection as a socket handle. A pipe listener would have to smuggle either the pipe
`HANDLE` or a pointer to its stream object through `KlSocketHandle`, and the brief forbids both.

See §5 for the options. **Mismatch recorded; listener support is gated on a decision.**

### Q3: Which structures hold native socket handles directly?

`KlStream.fd`, `KlCompletionEvent.accepted_fd`, `KlDgramSendOp.fd` / `KlDgramRecvOp.fd`, the
`KlListener` and `KlConnectOp` handoffs, `KlWatcher` (`KlWatcherFn` takes a `KlSocketHandle`), and
inside IOCP `KlIocpOp.op_sock` / `accept_sock` (typed `SOCKET`, used as the `CancelIoEx` target).
No structure holds a generic non-socket native handle. **No internal generic native-I/O handle type
exists today.**

### Q4: Can a non-socket stream use the completion machinery without pretending to be a socket?

**Yes, append-only.**

- The IOCP port associates any overlapped `HANDLE` (`CreateIoCompletionPort` is not
  socket-specific). `ReadFile` / `WriteFile` / `ConnectNamedPipe` complete on the same port and are
  drained by the same `GetQueuedCompletionStatusEx` loop.
- **Lifetime:** [completion_life.h](../../src/completion_life.h) (`KlCompLife`) is, by its own banner, a
  *transport-neutral* stable-liveness token. It carries an owner reference plus one reference per
  posted op, transfers the reference into the completion event, releases it exactly once after
  dispatch, and routes through a per-owner `dispatch` fn. That is exactly I3/I5 for an object whose
  completions outlive it. A pipe op reuses it unchanged. (It was named for datagrams at audit time and renamed to
  `KlCompLife` once pipes became its second owner; §8.) Pipes add no second IOCP lifetime model.
- **Routing:** `completion_core.c` already routes `KL_COMP_DGRAM_*` through the token's dispatch.
  Pipe kinds join that same `case`, a one-line change.

### Q5: Does any public API need to change?

**One addition is unavoidable; nothing existing changes.** Keel has no public "open a live
`KlStream` on an endpoint" call today. [http_connection.c](../../src/protocols/http/http_connection.c)
is the only production caller of `kl_stream_init`, and an `AF_UNIX` client uses the socket provider
plus `KlConnectOp`. A pipe cannot go through the provider (Q3, brief), so it needs its own entry
point. The object handed out is still a `KlStream`: the consumer calls `kl_stream_write`,
`kl_stream_pause` / `_resume`, `kl_stream_close_begin` / `kl_stream_cancel`, exactly as over any
other stream.

### Q6: Can the pipe be implemented entirely in internal driver machinery?

Yes, apart from the one public entry header (§3.1). The Win32 mechanics sit in the IOCP backend and
one Windows PAL TU; the stream glue is generic substrate and includes no platform header.

### Q7: Which gates would reject a wrong implementation?

| Wrong move | Gate that catches it |
|---|---|
| `windows.h` in a protocol or generic TU | `check-tier1-boundary` (default-deny header regex) |
| Protocol code reaching the pipe driver | `check-substrate-purity` (G1) in reverse, via the Tier-1 regex on `completion*.h` |
| A new bare `src/*.c` not declared | `check-protocol-home` (G4 manifest) |
| Win32 types in an installed header | `freestanding-headers` |
| Routing the pipe through `KlSockAddr` | `check-sockaddr-neutral` (partial) |
| Pipe `HANDLE` through the socket provider, narrowed to `int`, blocking I/O on the loop | **none today**, so new gates are added (§6) |

## 2. The consumer finding (blocks completion criterion 2 as written)

Hull, the named consumer, cannot use an IOCP-native Keel pipe as Hull is built today:

1. **Hull on Windows is a cosmocc APE**, not a native Windows build (Hull `Makefile`: "native
   Windows is future; Windows runs via the cosmo APE today"). Under cosmocc Keel builds on the `poll`
   backend with no `_WIN32` and no IOCP, so a `_WIN32` IOCP transport does not exist in that binary.
2. **Hull's byte transport is not `KlStream`.** `hull/src/hull/cap/net_stream.c` drives `KlConnectOp`
   plus raw socket-provider `send`/`recv` on readiness watchers through `HlAsyncBackend`, which on a
   native Keel build defaults to WSAPoll. The brief forbids readiness emulation for pipes.

This Keel work stands on its own (native MinGW/MSVC consumers, and the `KlStream` claim this note
exists to test). Hull consumption needs, on Hull's side, a native-Windows build running Keel on IOCP
plus a `KlStream`-based (or completion-aware) path for local IPC. Keel cannot supply either.


**Decision (2026-09-28):** build the Keel side anyway. The acceptance proof is a Hull-shaped consumer
inside Keel's own tests: a u32be-length framed exchange driven only through `KlStream`
(`tests/test_pipe_stream.c`, `framed_exchange_through_kl_stream_only`). Listener support is deferred
(§5).

## 3. Design as built

### 3.1 Public surface: `include/keel/pipe.h` (new, append-only)

```c
typedef struct KlPipeStream KlPipeStream;          /* opaque; owns the handle + the KlStream */
typedef enum { KL_PIPE_OK, KL_PIPE_ABSENT, KL_PIPE_BUSY, KL_PIPE_DENIED, KL_PIPE_INVALID,
               KL_PIPE_UNSUPPORTED, KL_PIPE_NOMEM, KL_PIPE_ERROR } KlPipeStatus;
typedef struct { size_t read_capacity, write_capacity;
                 KlPipeDataFn on_data; KlPipeCloseFn on_close; void *user_data; } KlPipeConfig;

KlPipeStatus kl_pipe_connect(struct KlEventCtx *ctx, const char *path, const KlPipeConfig *cfg,
                             KlPipeStream **out);
KlStream    *kl_pipe_stream(KlPipeStream *p);   /* the Tier-1 object the consumer drives */
void         kl_pipe_free(KlPipeStream *p);     /* any time; reclaims after physical retirement */
const char  *kl_pipe_status_str(KlPipeStatus s);
```

- **Everything after connect is `KlStream`:** `kl_stream_read_start` / `_write` / `_pause` /
  `_resume` / `_close_begin` / `_cancel`. The two callbacks have the `KlStreamReadDeliverFn` and
  `KlStreamCloseFn` shapes. Nothing above the stream is pipe-specific.
- **Endpoint** is a plain path string, not a `KlSockAddr` family. A pipe name is not a socket
  address, and widening `KlSockAddr` would pull pipes onto the socket-address axis. Keel has no
  generic endpoint/URI type (stream.md: "there is no `KlEndpoint` type"), so none is invented.
- **Its own status enum**, not `KlError`: a caller has to tell "no server" (ABSENT) from "retry
  later" (BUSY). `KlError` has neither, and inserting codes would move `KL_ERR__COUNT`.
- **Connect is synchronous and never waits.** `CreateFileW` on a local pipe returns a handle, or
  fails at once with `ERROR_PIPE_BUSY` or `ERROR_FILE_NOT_FOUND`. `WaitNamedPipe` blocks the loop, so
  it is not used and the gate forbids it. There is no connect deadline, because connect never pends;
  a caller's retry budget is its deadline. There is no retry policy in Keel.
- **Unsupported engines fail early.** On a non-IOCP loop, or on a runtime-installed event provider,
  `kl_pipe_connect` returns `KL_PIPE_UNSUPPORTED` before allocating or touching the OS. The functions
  link everywhere, so portable code needs no `#ifdef`.

### 3.2 Layering

```text
consumer ── KlStream API (read_start / write / pause / resume / close_begin / cancel)
              │
   src/pipe_stream.c      generic substrate: the KlStream adapter (submit / arm / cancel hooks),
              │           completion routing, lifetime. Includes no platform header.
              │   completion_pipe.h: kl_comp_pipe_available / _attach / _post / _cancel
              │   platform_pipe.h:   kl_plat_pipe_open_client / _close on an opaque KlPipeHandle *
   ┌──────────┴────────────────┐
 src/event_iocp.c           src/platform_pipe_win.c            (all Win32 lives in these two TUs)
 ReadFile / WriteFile on    CreateFileW, SQOS, byte read mode,
 the port, CancelIoEx,      GetFileType check, CloseHandle
 drain → KL_COMP_PIPE_*
```

Non-IOCP builds link `src/completion_pipe_absent.c` (available = 0) and, on POSIX,
`src/platform_pipe_posix.c` (UNSUPPORTED).

- **Handle type.** `KlPipeHandle` is an **incomplete struct**, used only as `KlPipeHandle *`. It is
  pointer-width like a `HANDLE`, but a distinct type the compiler will not implicitly convert to
  `KlSocketHandle` (`intptr_t`) or to `int`. The Win32 TU is the only place it becomes a `HANDLE`. It
  appears in no installed header.
- **Why a separate seam, not `KlCompletionOps` slots.** Every `KlCompletionOps` table (IOCP, io_uring,
  pollcomp, lwIP-raw) uses positional initializers. Appending slots would trip
  `-Wmissing-field-initializers` under `-Werror` in all of them. It would also make the dispatcher
  read past the end of a runtime-installed provider's table built against the old layout. Pipes exist
  on one engine, so the IOCP TU defines `completion_pipe.h` directly and every other build links the
  absent stub. No existing backend or provider changed.
- **Completion kinds.** `KlCompKind` gains `KL_COMP_PIPE_READ` / `KL_COMP_PIPE_WRITE`, appended.
  `completion_core.c` routes them in the **same `case`** as the datagram kinds: by the event's
  `KlCompLife` token, never by `target`, and never through the HTTP server's `comp_conn_dispatch`
  hook.
- **IOCP ops.** Two new `KlIocpOpType`s carry a `HANDLE op_handle`. `iocp_op_cancel_target()` picks
  the handle or the socket for `CancelIoEx`. Every pipe op joins the global outstanding-op registry,
  so `iocp_quiesce_port_for_close` cancels and dequeues it before the port closes. The drain maps
  outcomes as follows:
  - Success → `ok=1` with the byte count.
  - A successful **zero-byte read** (a peer's zero-length `WriteFile`) is re-issued, never delivered.
  - A **partial write** is re-issued until whole, so the stream sees one WRITE per submit.
  - `ERROR_BROKEN_PIPE`, `ERROR_PIPE_NOT_CONNECTED`, `ERROR_OPERATION_ABORTED` and any other error
    → `ok=0`. `KlStream` has no close taxonomy, so all of these are the single terminal.
  - `ERROR_MORE_DATA` (a message-mode server with a byte-mode reader) keeps the bytes.

### 3.2a Relationship to the existing completion semantics (review)

The pipe seam is a separate **submission** path: `completion_pipe.h` instead of `KlCompletionOps`
slots. It is not a separate **completion model**. The review, point by point:

| Concern | Pipe path | Relationship to the existing machinery |
|---|---|---|
| Op-ref ownership | `KlCompLife`: retain before post; transfer into the op on success; caller releases on a failed post; released after dispatch unless `retain_life` | identical to datagrams (the same token type and rule) |
| Routing in `kl_comp_run` | the datagram `case` arm, by token | identical |
| Outstanding-op tracking + loop-close quiesce | the shared IOCP `st->ops` registry | identical |
| Partial write / zero-byte read | re-issued inside the backend; one completion per logical op | the socket WRITE / SENDFILE re-post pattern |
| Retirement query | none: `KlStream` retires on delivery | datagrams need `retire_dgram` only for their close coordinator; nothing duplicated |
| **Stream routing class** | token-routed (I5 token class) | **differs** from a socket `KlStream`, which is raw-`target`-routed through the server hook (I5 raw-target class). Both classes already exist in I5; the pipe uses the stronger one, because it has no server hook to route through and its storage must outlive `kl_pipe_free`. |
| **Immediate OS failure at issue** | self-queued with `PostQueuedCompletionStatus`; the post succeeds and the error arrives as the one completion | **differs** from socket and datagram posts, which return -1 synchronously. The op-level contract (every accepted post completes exactly once) is unchanged. The difference is where the error surfaces. It is needed because a `KlStream` arm failure closes the read side without a terminal delivery (§3.3). |

A refusal to post while the loop was quiescing, which no other op kind has, was removed in review:
it was unreachable, since nothing posts during teardown, and it was a divergence without a reason.

The token was datagram-named when this work landed, which was naming debt rather than semantic
duplication. It was renamed to `KlCompLife` (`src/completion_life.{h,c}`) in its own mechanical change.

### 3.3 Lifetime (I3, I4, I5)

- **One token owns everything.** The whole `KlPipeStream` is owned by one `KlCompLife`: the embedded
  `KlStream`, its write queue, the receive buffer and the handle. The owner holds one reference and
  every posted op holds another, transferred into its completion event and released after dispatch.
  The token's final release (`pipe_final`) closes the handle and frees the memory. So:
  - a `ReadFile` the kernel is still filling never lands in freed memory, even after `kl_pipe_free`;
  - the handle is closed only after every op on it has retired, so no completion can target a
    recycled handle;
  - success, EOF, cancellation, and loop teardown (which releases op refs without dispatching) all
    end at the same final release.
- **The token is never marked dead.** The target stays valid exactly as long as the memory. A
  `freed` flag alone decides whether the consumer still hears anything, so no callback fires after
  `kl_pipe_free`.
- **Every accepted post completes exactly once, asynchronously (I4/I5).** The OS queues a packet for
  success and for `ERROR_IO_PENDING`, but not for a failure it reports at issue time. A `ReadFile` on
  a disconnected pipe fails at once with `ERROR_BROKEN_PIPE`. In that case the backend records the
  error in the op and queues the packet itself with `PostQueuedCompletionStatus`. EOF therefore
  arrives through the ordinary completion path, not as a synchronous arm failure that would close the
  read side with no terminal delivery. Handles never set `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`.
- **Cancellation is advisory and single-shot.** The stream's cancel hooks call `CancelIoEx` on the
  specific `OVERLAPPED`. The op still completes: aborted, or with its real result if it won the race.
  The cancel also sets `pipe_cancelled` on the op, so a zero-byte read or partial write that
  completes in the race is finished, not re-issued. A re-issued op would have no cancel pending, and
  the close would hang until the peer acted.
- **Graceful close.** `kl_stream_close_begin` is unchanged from the `KlStream` contract
  (`test_stream_close.c` `graceful_waits_for_recv_completion`). It drains output, then waits for the
  posted read to retire, which happens when the peer writes or disconnects. `kl_stream_cancel` is the
  escalation. Pipes add no special case.
- **Teardown order.** `kl_pipe_free` comes before `kl_event_ctx_free`, and before
  `kl_http_server_free` when the pipe shares the server's own loop, which that call destroys. Loop
  destruction delivers no further completion to anyone. `kl_http_server_free`'s teardown reap
  dispatches accepts only, and `kl_event_ctx_free`'s quiesce dispatches nothing. So a pipe still live
  at that point gets no terminal `on_data` and no `on_close`. Its outstanding ops are still cancelled
  and their refs released (memory-safe), but a later `kl_pipe_free` would touch the freed ctx. The
  header documents the order. This is the loop's existing rule, not a pipe behaviour (§8).

### 3.4 Security

**Client** (implemented):

- Requests `GENERIC_READ | GENERIC_WRITE`, `OPEN_EXISTING`, `FILE_FLAG_OVERLAPPED`.
- Passes `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`, so a server that squats the name can
  identify the client but **not impersonate** it. Test `server_cannot_impersonate_client` impersonates
  from the server side and asserts the token level is `SecurityIdentification`.
- **Local only:** the path must be `\\.\pipe\<name>`. `\\host\pipe\`, `\\?\pipe\`, other device
  namespaces and plain file paths are refused before `CreateFileW`, because a remote pipe would
  authenticate the user to another machine over SMB. `GetFileType == FILE_TYPE_PIPE` is checked after
  open, since the prefix test is only textual.
- **Byte read mode** is set explicitly with `SetNamedPipeHandleState`, which also rules out
  message-mode partial-read semantics.

**Server** (for a future listener, §5):

- `PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED`, plus `FILE_FLAG_FIRST_PIPE_INSTANCE` on the first
  instance.
- `PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS`.
- An explicit DACL for the current user's SID and `LocalSystem` only. The default pipe DACL grants
  read to Everyone and Anonymous, which would let another local user open an instance read-only.

### 3.5 What does not change

`KlSocketProvider` and every socket provider, `KlSockAddr`, `KlCompletionOps` and every other
completion backend, WSAPoll, the `KlStream` contract, `KlListener`, the `AF_UNIX` path
(`unix_socket_node_*`, `KL_HTTP_SERVER_TRANSPORT_UNIX`), and every protocol TU. No `LocalIpc`
abstraction wraps `AF_UNIX` and pipes: `AF_UNIX` stays socket provider + `KlStream`; a pipe is pipe
transport + `KlStream`.

## 4. Support matrix

| | `KlStream` (client) | Pipe listener | Engine |
|---|---|---|---|
| Windows, `BACKEND=iocp` (MinGW + MSVC) | supported | supported (§5) | IOCP, native |
| Windows, WSAPoll (default) | `KL_PIPE_UNSUPPORTED` | `KL_PIPE_UNSUPPORTED` | none; no readiness emulation |
| Windows, runtime-installed event provider | `KL_PIPE_UNSUPPORTED` | `KL_PIPE_UNSUPPORTED` | none |
| POSIX (any engine), cosmocc | `KL_PIPE_UNSUPPORTED` (use `AF_UNIX`) | n/a | n/a |

## 5. Listener: the mismatch and the options

**Resolved (listener implemented).** Option 1 was taken, in its narrowest form: `KlListener` gained an
append-only **object handoff family** (`on_accept_obj` / `dispose_obj`, `kl_listener_on_accepted_obj`),
and the Named Pipe listener (`kl_pipe_listen`) is built on it as the acceptance test. The design,
including why a `KlStream *` output and a handle union were rejected, is
[listener_accept_handoff.md](listener_accept_handoff.md). The analysis below is the pre-implementation
record.

1. **Append-only `KlListener` widening.** Add `kl_listener_on_accepted_obj(l, void *conn)` plus an
   optional `on_accept_obj` / `dispose_obj` hook pair. The window/credit/detach machine is reused
   unchanged and the sockets path is untouched. This is a public contract change and needs sign-off.
2. **Separate pipe-listener object.** It exposes `kl_pipe_listen(…, on_accept(ud, KlPipeStream *))`,
   does not reuse `KlListener`, and duplicates its credit/detach machine.
3. **Defer.** Chosen for the client change; superseded once the listener design was done (above).

Whichever option is chosen later, two Windows behaviours have a fixed treatment:

- **`ERROR_PIPE_CONNECTED` race.** A client that connects between `CreateNamedPipeW` and
  `ConnectNamedPipe` makes the latter return `ERROR_PIPE_CONNECTED` synchronously, with **no**
  completion queued. The listener must treat that as an immediate accept (or self-queue a packet, as
  the pipe I/O ops do) and never wait for a completion. The test harness's `server_accept` already
  handles this case.
- **Instance recycling.** An instance is recycled (`DisconnectNamedPipe`, then reuse or close) only
  after its stream's `on_close`.

## 6. Gates

`make check-pipe-seam` (`tools/check_pipe_seam.sh`) is default-deny over every tracked `src/`,
`include/keel/` and `integrations/` file, with a self-canary. It runs in the CI Static-Analysis job.

| Rule | Keeps out |
|---|---|
| R1 | Win32 pipe calls (`CreateNamedPipe`, `ConnectNamedPipe`, `DisconnectNamedPipe`, `PeekNamedPipe`, `SetNamedPipeHandleState`, `TransactNamedPipe`, `CallNamedPipe`, `ImpersonateNamedPipeClient`) and `ReadFile` / `WriteFile` anywhere except `event_iocp.c` and `platform_pipe_win.c` |
| R2 | `WaitNamedPipe` anywhere, and any `ReadFile` / `WriteFile` in the mechanics TUs without an `OVERLAPPED` (`&op->ov`): no blocking pipe I/O on the loop |
| R3 | any pipe symbol in a socket provider / socket-seam TU or public socket header, and any cast of a pipe handle to `KlSocketHandle` / `SOCKET` / an integer type |
| R4 | `<keel/pipe.h>` or a pipe seam header included from `src/protocols/` |
| R5 | an `ssh` / `agent` token in the pipe transport TUs |

Two existing gates also changed:

- `check-tier1-boundary`'s forbidden-header regex now includes `completion_pipe.h` and
  `platform_pipe.h`, with the self-canary extended. `pipe_stream.c` joins `TIER1_INFRA`.
- `freestanding-headers` compiles `<keel/pipe.h>` freestanding, which proves no Win32 type is in the
  public API.

Type safety backs R3 at compile time: `KlPipeHandle *` does not convert implicitly to an integer.

## 7. Verification record (2026-09-28, Windows 11, local)

- **`test_pipe_stream` (21 cases).** MinGW-w64 GCC 16.2 IOCP and MSVC 19.44 `cl` IOCP: 20 passed, 1
  expected skip (`unsupported_off_iocp`). MinGW WSAPoll: 2 passed (argument validation, unsupported),
  19 skipped. The cases cover:
  - connect: absent, busy, DACL-denied, non-local names, identification-only impersonation;
  - data: echo, 1 MiB fragmented transfer (1000-byte reads, 64 KiB bounded queue with `WOULD_BLOCK`
    observed, `TOO_LARGE` above capacity), orderly and abortive server disconnect, strict pause/resume;
  - close: cancel with a read or a write pending, graceful drain, free with ops outstanding, free from
    inside the terminal callback, loop teardown with ops posted;
  - reconnect, 200 repeated cancel/close races, and the framed exchange.

  Every lifetime case checks a counting allocator back to its pre-connect level.
- **The tests can fail.** Three mutations, each reverted:
  - an immediate pipe error failing the post instead of self-queuing → `server_abort_delivers_one_terminal`
    failed;
  - the final release leaking the read buffer → every balance check failed;
  - `on_close` not gated by `kl_pipe_free` → the free and race cases failed.
- **The gate can fail.** One injected violation per rule R1 to R5, plus a protocol-TU include of
  `completion_pipe.h` against `check-tier1-boundary`: each was reported, and the tree was restored.
- **Regression.** The full `WIN_IOCP_TEST_SUITES` set (MinGW and MSVC, clean MSVC objects) and `WIN_TEST_SUITES` set (MinGW,
  WSAPoll) are green. All structural and hygiene gates are green, as are the F2 inventory
  (`check-public-headers`, `check-public-coverage`).
- **Not run locally:** POSIX builds (Linux / macOS / cosmocc; no WSL or Docker on the host). There
  the suite reduces to the refusal and argument cases, and the standing CI jobs cover it.

## 8. Findings outside this change (recorded, not fixed here)

1. **The generic loop-teardown rule is undocumented.** Destroying an event loop (`kl_event_ctx_free`,
   or `kl_http_server_free` on the server's own loop) delivers no further completion. Every
   transport still alive on it silently loses its terminal callbacks; memory stays safe through
   quiesce and life tokens. `pipe.h` states this for pipes. Nothing in `event_ctx.h` or the stream
   and datagram contracts states it as the general rule, and `async_lifecycle.md`'s "no silent loss"
   covers only `KlAsyncOp`, which the server cancels explicitly. It is a documentation gap in the
   generic completion contract and belongs in its own change.
2. **Resolved:** `KlListener`'s accepted-connection handoff was socket-typed (§5). It now has an
   object handoff family for transports that are not sockets, without widening `KlSocketHandle` into
   a universal handle ([listener_accept_handoff.md](listener_accept_handoff.md)).
3. **Resolved:** the lifetime token, named for datagrams when pipes became its second owner, is now
   `KlCompLife` (§3.2a), and `make check-no-dgram-life` keeps the old names from returning.
