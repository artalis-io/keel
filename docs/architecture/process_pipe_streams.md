# Process pipe streams: audit and design

**Status:** design only; nothing here is implemented. Audit of the tree at `79655c9`
(post-3.2.0 `main`), 2026-09-29.

**Question:** can `KlStream` honestly carry one direction of an anonymous pipe, so an embedder (Hull)
can feed a child's stdin and read its stdout/stderr through ordinary streams, while process
lifecycle stays entirely outside Keel?

**Answer:** yes. Two generic stream corrections come first: graceful close after a write error
(F2), and a generic writable-again edge on `KlStream` (F3). The transport itself fits the existing
architecture without a single `if (pipe)` above `KlStream`. One platform is unresolved: Hull's actual
Windows build (cosmocc), where the POSIX path runs over Cosmopolitan's emulation and write readiness
is unreliable (§4.4). Nothing here claims Hull-on-Windows acceptance until that is resolved.

The unit is an anonymous pipe **pair**, not a stdio stream:

```text
                    anonymous pipe pair
              endpoint A            endpoint B
                  │                     │
          async, Keel-owned       native, handed to the child
                  │                (by the embedder's spawner)
         KlPipeStream / KlStream
         (read-only | write-only)
```

stdin, stdout and stderr are assignments the embedder makes (a write-direction pair for stdin, a
read-direction pair for each of stdout and stderr). Keel never names them.

```text
POSIX:   endpoint A = pipe2 fd, watcher readiness
Windows: endpoint A = overlapped end of a private single-instance named pipe, IOCP (completion_pipe.h, KlCompLife)
```

---

## 1. Current-state audit: is KlStream directional?

The stream is three independently installed facets (`stream.h`, `stream_detail.h`); a facet that
was never `_init`ed is inert. Traced, not inferred from names:

| # | Question | Answer | Evidence |
|---|---|---|---|
| 1 | Only READ initialized? | Yes | `kl_stream_read_init` touches only read fields; nothing requires `wq_inited` (`stream_read.c`). |
| 2 | Only WRITE initialized? | Yes | `kl_stream_write_init` touches only write fields (`stream_write.c`). |
| 3 | Code assuming both facets? | No, in the stream core | `stream_fully_retired` reads `recv_inflight` / `send_inflight`, both 0 when uninitialized, and `kl_stream_write_pending`, which returns 0 when `!wq_inited`. Close calls `kl_stream_read_close` only `if (s->read_inited)` (`stream_close.c`). The one both-facet assumption is in an **adapter**: `pipe_new` installs both unconditionally (`pipe_stream.c`). |
| 4 | Close assumes a receive exists? | No | Retirement waits on a recv only if `recv_inflight`. |
| 5 | Close assumes a write exists? | No | Waits on a send only if `send_inflight`, and on the queue only if it is non-empty. |
| 6 | Write-only graceful close without waiting for a nonexistent read? | **Yes, if the writes succeed** | No recv is ever armed, so detachment waits only for the queue to drain and the last send to retire. **But see F2:** after a write *error*, graceful close never detaches. |
| 7 | Read-only clean close after EOF? | Yes | The terminal `ok=0` sets `read_closed` and clears `recv_inflight`; `close_begin` then detaches at once. Before EOF on a completion engine, graceful close waits for the posted read, which is the documented completion semantics; `kl_stream_cancel` stops waiting. |
| 8 | `kl_stream_cancel` per direction | Well defined | Write-only: drop the queue, cancel an in-flight send (completion) or nothing to cancel (readiness), then detach. Read-only: cancel the posted read (completion) or drop interest (readiness), then detach. |
| 9 | Pause/resume for read-only | Natural | Readiness `disarm` drops READ interest, so the kernel pipe fills and the child blocks. Completion holds exactly one completed read. |
| 10 | Backpressure for write-only | Natural, but not observable | The bounded queue returns `KL_STREAM_WOULD_BLOCK`. **But see F3:** the contract offers no "writable again" signal, so a write-only producer cannot learn when to retry. |
| 11 | Socket shutdown in generic close? | None | No `shutdown()`. EOF to the peer is produced by the adapter closing the native endpoint (for `KlPipeStream`, at the final release after `kl_pipe_free`). |
| 12 | Does the contract already permit directional streams? | Structurally yes | A write on a read-only stream returns `KL_STREAM_ERROR` (`!wq_inited`), and `kl_stream_read_start` on a write-only one returns -1. That is honest refusal with no new API. F2 is a fix to an unstated edge (a hang), not a change to stated semantics. |

**Most important question:** can a write-only stream's graceful close finish without waiting for a
read that was never installed? **Yes.** The only way it can hang is F2, which is generic.

### Generic findings (not process-specific)

- **F2. Graceful close hangs after a write error when bytes remain queued.** `stream_fully_retired`
  requires `kl_stream_write_pending(s) == 0` for a graceful close. Once a write has failed, those
  bytes can never drain:
  - readiness: `kl_drain_flush` sets the sticky `d->error` and keeps the buffer;
  - completion: `kl_stream_on_write_complete(!ok)` sets `wq_err` but leaves any batch queued behind
    the failed one.

  So `close_begin` never detaches, on sockets and named pipes too. A child that exits with stdin data
  still queued makes it the common case. **Smallest correction:** once the write side is in error,
  graceful close treats the queue as undeliverable and detaches as an abortive close would. That
  means the retired check reads "no pending bytes, **or** the write side has failed", and
  `kl_stream_flush` records the error in `wq_err` as the completion path already does.
- **F3. No writable-again signal for a producer.** `stream.md` states it plainly: "`KlStream` exposes
  no low-water writable callback". `KL_STREAM_WOULD_BLOCK` is part of the generic contract, so the way
  to learn that it has cleared belongs to the generic contract too. It is not a pipe feature. Any
  producer on a bounded stream (a socket, a named pipe, a pipe feeding a child's stdin) needs it, and
  `KlDatagramWritableFn` already expresses the same full→non-full edge for datagrams.

  The machinery nearly exists. The stream's queue is a `KlDrain`, which already has `on_writable`
  (low-water crossing) and `on_drain` (empty) (`drain.h:53-55`, `drain.c:110-132`). It cannot simply
  be exposed, for two reasons:
  - it counts only queued bytes, while the stream's pending count also includes bytes copied into an
    in-flight send (`kl_stream_write_pending`);
  - with a copying backend (both pipe paths), the queue is consumed at submit, **inside** the
    producer's own `kl_stream_write`, so the drain callback would fire reentrantly at a moment
    nothing was delivered.

  **Correction:** a `KlStream` write-facet callback on the stream's own pending count. It fires once
  per full→non-full transition, only from `kl_stream_on_write_complete` (completion) or
  `kl_stream_flush` (readiness), never from inside `kl_stream_write`. It is an append-only STABLE
  addition. An owner-level `on_drained` in the pipe config was the first draft and is rejected: it
  would fix a generic limitation one layer too low. Implemented as `kl_stream_on_writable` (see
  `docs/contracts/stream.md`).

### Engine findings

- **F4. HUP/ERR are reported as READ.** epoll (`event_epoll.c:69`), poll (`event_poll.c:206`) and the
  io_uring watcher relay (`event_iouring.c:1021`) map `HUP|ERR → KL_EVENT_READ` only. epoll is
  edge-triggered. The write end of a pipe whose reader has gone reports `ERR`, which arrives at a
  WRITE-interested watcher as READ. A write-only adapter must therefore treat **any** readiness as
  "attempt the write" and let `write()` report `EPIPE`, or it can wait forever. kqueue reports it on
  `EVFILT_WRITE` with `EV_EOF`, which is harmless under the same rule.
- **F5. Pre-existing fd leak into children.** `KlWakeup`'s `pipe()` (`platform_wakeup_posix.c:20`)
  and io_uring's splice pipe (`pipe2(O_NONBLOCK)`, `event_iouring.c:506`) are not close-on-exec, so
  every child Hull spawns inherits them. Sockets are close-on-exec (`socket_posix.c:57`, `:133`). This
  is independent of process pipes and fixed separately.
- **F6. SIGPIPE.** Keel avoids it only for sockets (`MSG_NOSIGNAL`, `SO_NOSIGPIPE`), plus a
  **process-global** `signal(SIGPIPE, SIG_IGN)` in the HTTP server (`http_server_plat_posix.c:58`).
  `write()` on a pipe has no per-call flag.

---

## 2. What has to change in generic code

Only F2, F3 and one adapter parameterization. Nothing else above the transport knows pipes exist:

1. **F2 (stream core).** On the write side, error means undeliverable for graceful close, and the
   readiness flush error becomes sticky. The new tests must fail before the fix.
2. **F3 (stream level).** Add a write-facet writable-again callback to `KlStream` (append-only,
   optional). It is not a pipe-config field.
3. **Directional adapters.** `pipe_new` (Windows) and the new POSIX adapter install only the facet
   the direction needs. That lives in the transport, not in `KlStream`.

No capability bits: a direction is structural (only one facet exists), and the creator of the
stream already knows which direction it asked for. `KL_STREAM_CAP_*` stays unadded until some
consumer has to discover it dynamically.

---

## 3. POSIX design

- **Endpoint.** `pipe2(O_CLOEXEC)`. Both ends are close-on-exec: the spawner's `dup2` onto 0/1/2 clears
  the flag on the target only, so the original descriptor closes in the child. Only the **parent** end
  is `O_NONBLOCK`; the child end stays blocking, because children expect blocking stdio. On platforms
  without `pipe2` (macOS), use `pipe()` + `fcntl(FD_CLOEXEC)`. That leaves a window where a concurrent
  `fork` in another thread inherits both ends. It is documented, and it is the spawner's to close with
  its own spawn lock.
- **I/O.** Plain `read()` / `write()` in a PAL TU (`platform_pipe_posix.c`, today only a stub),
  EINTR-retried. **Never** through `KlSocketProvider`: a pipe fd is a file descriptor, not a socket.
- **Readiness registration.** Through `kl_watcher_add` / `_mod` / `_del`, the path `KlWakeup` already
  uses for its POSIX pipe. That API's parameter is typed `KlSocketHandle`, but on the native-fd
  engines it is in fact "a pollable descriptor". This is the one place a pipe fd passes through that
  type. It is registration only, never a socket op, and the gate forbids pipe code from calling
  provider ops. Renaming the registration type is a separate, larger decision (§12).
- **Engines.** Anything with `KL_EVENT_CAP_NATIVE_FD` works: epoll, kqueue and poll natively, and
  io_uring and pollcomp through their watcher relay (poll-add). A runtime-installed provider without
  `NATIVE_FD` (lwIP-raw, EFI) gets `KL_PIPE_UNSUPPORTED`.
- **Read adapter.**
  - `arm` adds READ interest; `disarm` removes it.
  - On readiness, one `read()` into the stream buffer. `> 0` delivers; `0` is EOF (`ok=0`); `EAGAIN` is
    spurious (stay armed, deliver nothing; required, see §4.4); any other error is terminal (`ok=0`).
- **Write adapter.**
  - The `write_fn` passed to the stream calls `write()`. On a short write or `EAGAIN` it arms interest
    and returns what was written, which is the only point where the adapter learns the queue is
    non-empty. On `EPIPE` or another error it returns -1, and F2 makes that terminal for graceful close.
  - On **any** readiness (F4) it calls `kl_stream_flush`. When the queue is drained it disarms; the
    stream's own writable edge (F3) tells the producer to continue.
- **SIGPIPE, local.**
  - macOS/BSD: `fcntl(F_SETNOSIGPIPE)` on the parent write end where available.
  - Elsewhere: around a write, block SIGPIPE in the **calling thread** (`pthread_sigmask`). On `EPIPE`,
    remove a SIGPIPE that this write caused (`sigtimedwait` with a zero timeout, only if none was
    pending before), then restore the mask. No handler is installed, nothing process-global changes,
    and it is verified under Cosmopolitan (§4.4).
- **Out of scope:** `mkfifo`, PTYs, adopting an arbitrary inherited fd (see §11).

## 4. Windows design

### 4.1 Why not `CreatePipe`
Its handles are not opened `FILE_FLAG_OVERLAPPED`, so they cannot be driven through IOCP. Keel does not
fake asynchrony with a thread per handle. `CreatePipe` handles are therefore unsupported. They are not
detected and emulated.

### 4.2 The pair: a private, single-instance, overlapped named pipe
1. **Name:** `\\.\pipe\keel-anon-<pid>-<counter>-<128 random bits>`, with the random bits from
   BCryptGenRandom (`bcrypt` is already linked).
2. **Parent end:** `CreateNamedPipeW` with `FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE`,
   access direction-limited (`PIPE_ACCESS_INBOUND` when the parent reads, `PIPE_ACCESS_OUTBOUND` when
   it writes), `PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS`, **max instances 1**,
   and the existing current-user + SYSTEM DACL. Not inheritable.
3. **Child end:** `CreateFileW` on the same name, **synchronous** (no `FILE_FLAG_OVERLAPPED`, because
   the child's CRT expects ordinary handles), access matching the child's direction, identification
   SQOS, **not inheritable**.
4. **Verification:** `GetNamedPipeClientProcessId(parent) == GetCurrentProcessId()`. Any other
   process that raced to the name, which is only possible by guessing 128 random bits, fails the pair.
5. `ConnectNamedPipe` is not needed. The client already connected, and I/O on the parent end works.
   This is the established construction for an anonymous-equivalent overlapped pipe; it is
   **verified before relying on it** (§16).

Once the child end is open, the single instance is taken, so nobody else can connect.

### 4.3 Reuse
The parent end is wrapped by the existing `pipe_new(ctx, h, …)` (`pipe_stream.c`). That brings the same
overlapped `ReadFile` / `WriteFile`, `KlCompLife`, `KL_COMP_PIPE_*` routing, cancel, quiesce and final
release as the named-pipe client. `pipe_new` gains a direction argument so it installs one facet.
Named pipes pass duplex. **No new I/O code and no new lifetime token.** The creation code (§4.2) is new
PAL code in `platform_pipe_win.c` (`kl_plat_pipe_create_pair`), beside `kl_plat_pipe_create_instance`,
and sharing its DACL and name helpers.

### 4.4 Hull's actual Windows target: cosmocc (the POSIX path on a Windows host)
Hull ships Windows as a Cosmopolitan APE, where Keel builds the **POSIX** design on the `poll` backend.
Measured with a throwaway probe (cosmocc from Hull's tool cache, Windows 11, 2026-09-29):

| Behaviour | Result | Consequence |
|---|---|---|
| `pipe2(O_NONBLOCK\|O_CLOEXEC)` | works, cloexec set | ok |
| blocking `poll` woken by a writer | 308 ms for a 300 ms delay | ok |
| EOF | `POLLIN`, then `read() == 0` | ok |
| reader gone | `write() → EPIPE`, SIGPIPE raised; block + `sigtimedwait` clears it | ok |
| pipe capacity | **4096 bytes** | a single MCP message can exceed it |
| `POLLOUT` on a **full** write end | **reported writable** | a readiness writer **spins** while the child is not reading |
| zero-timeout `poll` on an empty pipe | returned `n=0` but set `revents=POLLIN` | spurious read readiness, so `EAGAIN` must be benign (§3) |

Reading from a child is sound under cosmo. **Feeding a child's stdin with backpressure is not**, and
Keel does not paper over it:
- no timer backoff in generic Keel to compensate for one runtime's poll;
- no "keep messages under 4 KiB" rule, which is not a transport guarantee.

The honest options sit outside Keel's core:
- the defect is reported upstream to Cosmopolitan;
- Hull decides how its cosmocc build treats a writer it cannot back-pressure.

Until one of these lands, **Hull-on-Windows process pipes are not claimed as supported.** The probe
becomes a tracked test on a Windows runner (§13).

This is the second consumer-driven feature (after the 3.2 named pipes) whose clean Windows
implementation is IOCP-native while Hull's Windows runtime is cosmo's poll emulation. The mismatch is
growing, and it is evidence for Hull's own decision about a native Windows/IOCP build.

---

## 5. Reuse relationship with Windows Named Pipes

| Layer | Named pipe (3.2) | Process pipe |
|---|---|---|
| Owner object | `KlPipeStream` | **same** `KlPipeStream` |
| Stream I/O, lifetime (Windows) | `pipe_new`, `completion_pipe.h`, `KlCompLife` | **same**, direction-parameterized |
| Stream I/O (POSIX) | n/a (unsupported) | new readiness adapter |
| Endpoint creation | `kl_plat_pipe_open_client` / `_create_instance` | new `kl_plat_pipe_create_pair` (Windows), `pipe2` (POSIX) |
| Public creation | `kl_pipe_connect` / `kl_pipe_listen` | new `kl_anon_pipe_create` (§11) |
| `KlListener` | yes (object family) | **no**: a pair is not accepted |

Creation stays separate from I/O semantics, as it already is for named pipes.

## 6. Readiness / completion mapping

| Platform / engine | Mechanism | Status |
|---|---|---|
| Linux epoll, macOS kqueue, POSIX poll | watcher readiness + `read` / `write` | supported |
| Linux io_uring, pollcomp | same watchers, relayed by poll-add | supported |
| Cosmopolitan (poll), POSIX hosts | as poll | supported |
| Cosmopolitan, Windows host | as poll | read supported; write readiness defective (§4.4) |
| Windows IOCP (MinGW, MSVC) | overlapped pair + completion seam | supported |
| Windows WSAPoll | none (cannot watch a `HANDLE`) | `KL_PIPE_UNSUPPORTED` |
| runtime provider without `NATIVE_FD` (lwIP-raw, EFI) | none | `KL_PIPE_UNSUPPORTED` |

No combination is emulated. The readiness adapter is used only where the engine natively watches
descriptors.

## 7. Ownership and lifetime

| Resource | Created by | Owner until handoff | Released by | When |
|---|---|---|---|---|
| parent native end (endpoint A) | `kl_anon_pipe_create` | `KlPipeStream` | the transport | Windows: final `KlCompLife` release (after `kl_pipe_free` and every op retired). POSIX: `kl_pipe_free` once detached (readiness has no in-flight op). |
| child native end (endpoint B) | `kl_anon_pipe_create` | the **caller**, as `KlAnonPipeEnd` | the caller: `kl_anon_pipe_end_close` | right after spawning (or on spawn failure). A parent that keeps it open never sees EOF on the reader side. |
| `KlPipeStream`, `KlStream`, read buffer, write queue | create | caller | `kl_pipe_free` | any time. The memory outlives posted ops via `KlCompLife` (Windows) or is freed once watchers are removed (POSIX). |
| `KlCompLife` (Windows) | create | transport | final release | after the owner ref and every op ref |
| watcher registration (POSIX) | first arm | transport | `disarm` or `kl_pipe_free` | synchronous, loop thread |
| event loop | caller | caller | `kl_event_ctx_free` | **after** every pipe is freed (the teardown rule) |

| Case | Outcome |
|---|---|
| full success | the caller holds the stream plus `KlAnonPipeEnd` |
| second endpoint fails | the first end is closed inside create; status returned; nothing handed out |
| stream init fails after both ends exist | both ends closed inside create; status returned |
| child never starts | the caller closes `KlAnonPipeEnd`. The parent reader gets EOF; the parent writer gets `EPIPE` / broken pipe on its next write. |
| parent closes first | write-only: graceful close drains, detaches, and `kl_pipe_free` closes the end, so the child reads EOF. Read-only: the child's next write gets `EPIPE` / `ERROR_NO_DATA` (the child's SIGPIPE is the child's business). |
| child closes first | reader: terminal `ok=0`. Writer: next write fails, then graceful close detaches (with F2). |
| cancel, read pending | the `kl_stream_cancel` path, the same as for named pipes |
| cancel, write pending | queue dropped; completion send cancelled |
| free from a callback | legal, as for `KlPipeStream` today |
| loop teardown | the rule on `kl_event_ctx_free`: free first. Not usable afterwards. |

## 8. EOF and broken-pipe semantics

No process-specific EOF. Everything maps onto the existing single terminal:

| Native | Surfaces as |
|---|---|
| POSIX `read() == 0` / `POLLHUP` + 0 | read terminal `ok=0` |
| Windows `ERROR_BROKEN_PIPE`, `ERROR_PIPE_NOT_CONNECTED` on read | read terminal `ok=0` (existing IOCP mapping) |
| POSIX `EPIPE` on write | `write_fn` -1: sticky write error, `KL_STREAM_ERROR` on the next write, and graceful close detaches (F2) |
| Windows `ERROR_NO_DATA` / `ERROR_BROKEN_PIPE` on write | WRITE completion `ok=0`: the same |

Stdin EOF for the child is **closing the parent write end**: `kl_stream_close_begin`, then (in or
after `on_close`) `kl_pipe_free`. `KlStream` has no half-close, and a directional stream needs none.

## 9. Backpressure

- **Parent writes (a write-direction pair).** A bounded queue (`write_capacity`) returns
  `WOULD_BLOCK`; the stream's writable-again edge (F3) says when to continue. With a full kernel pipe, the adapter keeps interest armed and the queue
  bounded, and nothing is buffered beyond `write_capacity`.
- **Parent reads (a read-direction pair).** Pause stops delivery. Readiness drops interest; completion holds
  one completed read. The kernel pipe then fills and the **child blocks**, which is the intended
  pressure. There is no hidden buffering.
- **Both directions with one reader paused.** The classic deadlock, where the child blocks on stdout
  while the parent waits to write stdin, is the embedder's to avoid (keep reading). It is the same on
  every platform.

## 10. Security and inheritance

- **POSIX.** Both ends are `O_CLOEXEC`; the spawner `dup2`s the child end. F5 is fixed so Keel's own
  wakeup and splice pipes stop leaking into children.
- **Windows.**
  - Both ends are created **non-inheritable**. The spawner makes only the child end inheritable,
    ideally through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (an explicit list), so concurrent spawns do
    not cross-inherit.
  - The parent end is never inheritable.
  - The private pipe carries the random name, first-instance and max-1 flags, `PIPE_REJECT_REMOTE_CLIENTS`,
    and the current-user + SYSTEM DACL, and is checked with `GetNamedPipeClientProcessId`. So it is
    never a reachable IPC endpoint: once the pair exists the single instance is taken.
  - The child end uses identification-level SQOS: it never grants impersonation.

## 11. API

Required operations: create a pair; get the parent end as a stream; hand the child end's native value
to the spawner; close the child end in the parent.

| Option | Verdict |
|---|---|
| **A. Pair creation in Keel** | **Chosen.** Windows needs a Keel-shaped parent end (overlapped, on the port, private and secure); only the creator can make it correctly. POSIX needs the right flags on each end. |
| B. Adopt a native endpoint made by Hull | Rejected for now. On Windows Keel cannot reliably reject a non-overlapped handle, so this invites the unsupported case. On POSIX it is easy, and can be added later, POSIX-only, if a consumer adopts inherited stdio. |
| C. A new owner type | Rejected: `KlPipeStream` already is "a stream over a pipe end" with the right lifetime. |
| D. `on_drained` in the pipe config | Rejected: F3 is a generic `KlStream` limitation and is fixed there. |

Proposed, all append-only:

```c
/* <keel/anon_pipe.h> */
typedef enum { KL_ANON_PIPE_READS = 1, KL_ANON_PIPE_WRITES = 2 } KlAnonPipeDir;  /* endpoint A's role */
typedef struct { intptr_t _native; } KlAnonPipeEnd;   /* endpoint B, for the child; opaque */

KlPipeStatus kl_anon_pipe_create(KlEventCtx *ctx, KlAnonPipeDir dir, const KlPipeConfig *cfg,
                                 KlPipeStream **stream, KlAnonPipeEnd *peer);
void kl_anon_pipe_end_close(KlAnonPipeEnd *peer);     /* idempotent; call after spawning */
/* KlPipeConfig: on_data may be NULL for a KL_ANON_PIPE_WRITES pair (no read facet). */

/* <keel/anon_pipe_native.h>: explicitly platform-specific, for the spawner only */
#if defined(_WIN32)
void *kl_anon_pipe_end_handle(const KlAnonPipeEnd *peer);  /* a HANDLE, as void * (no windows.h) */
#else
int   kl_anon_pipe_end_fd(const KlAnonPipeEnd *peer);
#endif
```

- **Naming.** "Pipe" already has a public meaning in `kl_pipe_connect` / `kl_pipe_listen`: a
  connection-oriented Windows Named Pipe. So pair creation gets its own literal prefix,
  **`kl_anon_pipe_*`**. `process` was considered and rejected because it reads as process management,
  which Keel does not do. There are no stdio-role names. The one shared name is the stream owner,
  `KlPipeStream` (with `kl_pipe_stream` / `kl_pipe_free` / `kl_pipe_bind`), because it really is the
  same object with the same lifetime over either kind of pipe end. Its header banner will say so.
- **Types.** No `HANDLE` in a portable header; no `int fd` as a cross-platform type.
- **Direction misuse.** Calling the other direction's operation is refused through the existing
  stream results (`KL_STREAM_ERROR`, read_start -1).

## 12. Rejected designs

- **`KlStdin` / `KlStdout` / `KlProcess`:** process roles and lifecycle are Hull's.
- **Capability bits:** directionality is structural (§2).
- **`CreatePipe` handles plus a thread per handle:** fake asynchrony. **WSAPoll support:** cannot
  watch a `HANDLE`.
- **POSIX `socketpair` instead of `pipe2`:** it would get `MSG_NOSIGNAL` for free, but the child
  would see a socket on its stdio, which some tools treat differently, and the design would drift
  back onto the socket axis.
- **Routing pipe I/O through `KlSocketProvider`** because the fd is an `int`: this is exactly the
  confusion the named-pipe work removed.
- **A process-global `SIG_IGN`** from the transport: it changes the embedder's signal disposition.
- **A new I/O or lifetime implementation for Windows:** `pipe_new` and `KlCompLife` already fit.
- **Renaming the readiness registration type** (`KlSocketHandle` in `kl_watcher_add`) now: correct in
  the long run, but a public, cross-cutting change that this feature does not need. It is recorded
  as a follow-up.

## 13. Test plan

Core transport tests use **both ends in the test process**: the test reads or writes the `KlAnonPipeEnd`
with plain blocking I/O on a helper thread. Real children are left to Hull.

- **Directionality.**
  - A read-only stream refuses writes (`KL_STREAM_ERROR`).
  - A write-only stream refuses `read_start`.
  - No facet the stream lacks is ever called (a planted assertion in the adapter).
- **Data.**
  - Small writes and large ones: a 1 MiB fragmented transfer with a small read buffer.
  - Short writes and ordering.
  - Queue bounded (`WOULD_BLOCK` observed), and the stream's writable edge firing once per
    full→non-full transition, never from inside `kl_stream_write` (F3; its own suite, generic).
  - Pause holding data while the peer blocks.
- **EOF.**
  - Peer write end closed → reader `ok=0`, and read-only close after EOF detaches immediately.
  - Write-only graceful close → the peer reads EOF.
  - Peer read end closed → writer `KL_STREAM_ERROR`, then graceful close still detaches (F2).
  - SIGPIPE not delivered to the process (POSIX; the test fails if the default action runs).
- **Lifetime.**
  - Cancel with a read pending and with a write pending.
  - Free from a callback.
  - 200 repeated close/cancel races.
  - Allocator back to baseline, and no callback after free.
  - Create failure paths: injected second-end failure and stream-init failure leave no fd or handle
    open.
  - F5 regression: after `kl_anon_pipe_create`, a `fork`/`exec`ed helper sees none of Keel's
    internal descriptors (wakeup, splice) on POSIX.
- **Engines.** epoll, poll, kqueue, io_uring and pollcomp (CI). IOCP under MinGW and MSVC. WSAPoll
  and non-`NATIVE_FD` engines asserted `UNSUPPORTED`.
- **Cosmopolitan.** The §4.4 probe turned into a test run under cosmocc on a **Windows runner**. It
  asserts the *correct* behaviour, and is expected to fail until the upstream defect is resolved, so
  it tracks the gap rather than assuming it away.
- **Negative (planted failures).** Each must be caught:
  - install both facets in a directional stream;
  - an unbounded queue;
  - the writable edge firing inside `kl_stream_write`, or twice per transition;
  - a callback after free;
  - closing the native end before retirement;
  - a SIGPIPE left pending;
  - the write adapter reacting only to the WRITE bit (F4).

## 14. Architecture gates

These extend `check-pipe-seam` rather than add gate count. Each has a self-canary:

1. **Native pipe calls stay in the PAL.** `pipe(` / `pipe2(` are allowed only in `platform_pipe_posix.c`,
   plus the existing `platform_wakeup_posix.c` and `event_iouring.c` (splice). `CreateNamedPipe` /
   `ConnectNamedPipe` are already confined by R1.
2. **No socket-provider use from pipe code.** The pipe TUs never call `kl_sock_*`, `kl_sockdef_*` or
   provider ops; this extends R3.
3. **No process management in Keel.** Default-deny across `src/`, `include/` and `integrations/` for
   `fork`, `vfork`, `exec*`, `posix_spawn*`, `waitpid`, `CreateProcess*` and `TerminateProcess`. The
   tree has none today (verified).
4. **No process-global SIGPIPE disposition outside the one existing HTTP-server site**
   (`http_server_plat_posix.c`).
5. **Consumer-neutral transport.** R5 extended with `stdin`, `stdout`, `stderr`, `mcp` and `jsonrpc`
   tokens in the pipe TUs.

Blocking I/O on the loop needs no gate on POSIX, because the parent end is `O_NONBLOCK` and a test
asserts it. On Windows it is already covered by R2 (overlapped only).

## 15. Implementation sequence

Each step is its own PR:

1. **This design document.**
2. **F2: graceful close after a terminal write failure.** A generic stream correctness fix with no
   pipe code. Socket and named-pipe regression tests, failing first, in readiness and completion
   modes. It includes exactly-once `on_close` checks. The invariant it states: graceful close drains
   accepted output while delivery remains possible; a terminal write failure makes delivery
   impossible, the undeliverable queue is abandoned under the existing ownership rules, and close
   progresses.
3. **F5: close-on-exec** for `KlWakeup` and the io_uring splice pipe, with a fork/exec regression
   where the platform allows.
4. **F3: the `KlStream` writable-again edge**, generic, with its own tests over sockets and named
   pipes. If implementation shows it does not belong in `KlStream` after all, the reason is recorded
   here before anything pipe-specific is added.
5. **Windows anonymous pairs.** `kl_plat_pipe_create_pair`, direction in `pipe_new`,
   `kl_anon_pipe_create`, `KlAnonPipeEnd`, and tests (IOCP; MinGW and MSVC).
6. **POSIX anonymous pairs.** Readiness adapter and PAL, local SIGPIPE handling, the F4 rule, and tests
   on epoll, kqueue, poll, io_uring and pollcomp. Also the cosmocc Windows-runner probe as a tracked,
   expected-fail test. Gates and docs (stream contract on directional streams, capability matrix,
   platform support, CHANGELOG) land with the step that introduces each rule.
7. **Hull-side spike, outside Keel.** Spawn a real MCP stdio echo server using the pair, to validate
   the spawner handoff (`dup2` / handle list) end to end, on native engines first. Hull-on-Windows
   (cosmocc) stays unclaimed until §4.4 is resolved.

## 16. Risks and open questions

- **Cosmopolitan on Windows write readiness (§4.4)** is the largest risk for Hull specifically. It is
  reported upstream and decided in Hull; Keel does not work around it.
- **The Windows no-`ConnectNamedPipe` pair construction** must be verified empirically, including
  `GetNamedPipeClientProcessId` and direction-limited access, before step 3 relies on it.
- **Availability of `F_SETNOSIGPIPE`** per platform, and correctness of the per-thread
  block/`sigtimedwait` sequence when SIGPIPE was already pending or blocked by the embedder.
- **macOS lacks `pipe2`:** the close-on-exec race is documented for the spawner.
- **io_uring watcher relay on pipe fds:** expected to work (poll-add), and to be proven by the step 4
  tests.
- **F3 edge semantics:** whether the edge is "non-full" (any room) or a configurable low-water mark,
  and how it interacts with a synchronous refill from inside the callback. `KlDrain`'s
  `drain_notify` already settles the refill question and is the model.

## 17. Recommendation

**Implement**, in the §15 order. The audit shows directional pipes fit without distorting the
architecture:

- `KlStream`'s facets are already independent;
- the Windows path reuses the 3.2 pipe I/O and `KlCompLife` unchanged;
- the POSIX path uses the native readiness engines through the existing watcher registration.

The generic defect (F2), the generic gap (F3) and the descriptor leak (F5) are worth fixing on their
own merits, and each lands first as its own PR. Hull-on-Windows under cosmocc is not claimed until
§4.4 is resolved.
