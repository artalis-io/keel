# KlListener accept handoff: design

**Status:** implemented as designed (§3, §4); verification in §7. Motivated by the Windows Named Pipe client
([windows_named_pipes.md](windows_named_pipes.md) §5, §8): `KlStream` carried a non-socket transport
unchanged, but `KlListener` could not, because its successful-accept handoff is typed `KlSocketHandle`.
This note locates that assumption exactly and picks the smallest change that removes it without
making `KlListener` own `KlStream` construction.

## 1. Where the socket assumption actually is

`src/listener.c` (252 lines) never interprets the accepted value. It is a pure pass-through:

| Site | Use of the accepted `KlSocketHandle fd` |
|---|---|
| `kl_listener_on_accepted(l, fd)` | received from the adapter |
| `l->on_accept(ctx, fd, lease)` | handed to the owner, with the committed lease |
| `l_dispose(l, fd)` → `l->dispose_fd(ctx, fd)` | handed back to the adapter, on a spurious accept or during teardown |

There is no `kl_handle_valid`, no close, and no provider call. Every transport-specific act is already
an adapter hook: `arm_accept`, `disarm_accept`, `cancel_accept`, `on_accept`, `dispose_fd`. The window,
credit, cancel-once, trampoline and detachment machinery (everything that makes `KlListener` worth
reusing) is handle-agnostic.

So the listener is already a transport-neutral accept state machine, structurally option C of the
review ("the transport-specific accept driver materializes the connection; the listener never
understands `SOCKET` or `HANDLE`"). The socket assumption is **nominal**: it is the declared type of
three pass-through parameters, and nothing more.

## 2. Options, measured against that

| Option | Change | Verdict |
|---|---|---|
| **A.** Listener yields a `KlStream *` | `KlListener` would construct or receive a stream before the owner assigns a pool slot | **Rejected.** It inverts ownership for the HTTP server, whose `on_accept` maps the fd onto a pre-allocated `KlHttpConn` (which embeds its `KlStream`) under the lease. The listener would need to know how to build streams. |
| **B.** Tagged union `{kind; socket \| handle \| ...}` | one handoff carrying every transport's native value | **Rejected.** It is the universal-handle union in disguise, and every new transport edits a shared type. |
| **C.** Adapter-materialized object, opaque to the listener | append a sibling handoff family: `kl_listener_on_accepted_obj(l, void *conn)` with hooks `on_accept_obj(ctx, conn, lease)` and `dispose_obj(ctx, conn)` | **Chosen.** The adapter that performed the accept decides what a connection is: a pipe adapter hands over a `KlPipeStream *`, which carries a `KlStream`. The listener passes it through exactly as it passes an fd today. |

Why `void *` is not a universal handle here: a universal handle is one type that every *native* I/O
object is squeezed into, so the value can reach code that operates on it. That is what forcing a pipe
`HANDLE` through `KlSocketHandle` would have been. Here the listener is the only intermediary and it
never operates on the value. The value is the adapter's own object, typed at both ends by the same
adapter, exactly like every `void *ctx` in Keel's hook tables. Native handles stay inside their
drivers.

## 3. The change (append-only, no existing behaviour moves)

- `KlListenerHooks` gains two trailing fields: `on_accept_obj` and `dispose_obj`. The struct is
  classified "caller-built; F2-C append-only", and every in-tree initializer is designated, so
  existing callers get `NULL` and are unaffected.
- `kl_listener_init` requires **exactly one** handoff family: `on_accept` + `dispose_fd`, or
  `on_accept_obj` + `dispose_obj`. Mixed or half-configured hooks are rejected (-1).
- `kl_listener_on_accepted_obj(l, conn)` is new. It shares the retire/commit/dispose/pump body with
  `kl_listener_on_accepted`, so window, credit, lease, cancel-once and detachment are one
  implementation.
- Calling the entry point of the family not configured is a contract violation. It is refused: the
  accept is retired as failed, and the credit is returned rather than leaked. `last_error` is set.
- `listener_detail.h` gains the two hook pointers. That layout is opt-in and not ABI.

`KlListenerAcceptFn`, `KlListenerDisposeFn`, `kl_listener_on_accepted` and the HTTP server's two
adapters (readiness `http_server.c`, completion `completion_http_server.c`) are unchanged.

## 4. Acceptance test: a Named Pipe listener

The widening only earns its place if a non-socket transport uses it, so it ships with one:

- **Public API** (`<keel/pipe.h>`, appended). `kl_pipe_listen(ctx, path, &cfg, &out)` returns a
  `KlPipeListener`. The accept callback receives a connected `KlPipeStream *` whose read side is not
  started. The owner installs its per-connection callbacks with `kl_pipe_bind(p, on_data, on_close,
  user_data)`, then drives the `KlStream`. Then `kl_pipe_listener_close` → `on_close` → free, the
  `KlListener` contract.
- **Driver.** Each posted accept is one pipe instance: `CreateNamedPipeW` plus an overlapped
  `ConnectNamedPipe` on the port, a new `KL_PIPE_OP_ACCEPT` op on the instance's own `KlCompLife`.
  Window = instance count.
  - A connected instance becomes the accepted `KlPipeStream` (the same object and lifetime as a client
    stream).
  - A failed or cancelled one is freed through the ordinary final release, so an instance handle
    closes only after its op has physically retired and is never reused.
- **`ERROR_PIPE_CONNECTED`.** A client that connects between `CreateNamedPipeW` and
  `ConnectNamedPipe` makes the latter report success synchronously, with no packet queued. The driver
  self-queues the completion, the same rule as immediate pipe I/O failures. The arm hook therefore
  never completes inline, and the race is deterministic to test at the seam.
- **Security.**
  - An explicit DACL for the current user's SID and `LocalSystem` only. The default grants Everyone
    read.
  - `FILE_FLAG_FIRST_PIPE_INSTANCE` on the first instance, so a pre-existing squatter fails the
    listen instead of sharing the name.
  - `PIPE_REJECT_REMOTE_CLIENTS`, byte type and byte read mode.
- **Gates.** `check-pipe-seam` R1 already confines `CreateNamedPipe` / `ConnectNamedPipe` /
  `DisconnectNamedPipe` to the two mechanics TUs.

## 5. What this does not do

- It does not give `KlListener` a `KlStream *` output (A), a handle union (B), or a universal handle.
- It does not move the HTTP server to the object family. Its fd handoff is correct for sockets and
  stays.
- It does not add WSAPoll pipe listening (refused, like the client), pool-credit accounting for pipes
  (no pool: `reserve` / `release` stay NULL), or instance recycling via `DisconnectNamedPipe` (every
  instance is closed after retirement; recycling is an optimization no consumer needs).

## 6. Resulting Tier-1 picture

```text
KlListener   accept state machine; hands off an fd (sockets) or an adapter object (pipes)
KlStream     ordered byte stream over sockets or named pipes
KlDatagram   atomic messages
```

The listener's output is no longer socket-typed for a transport that is not a socket, and the
socket path pays nothing for it.

## 7. Verification record (2026-09-28, Windows 11, local)

- **`KlListener` object family**, `tests/test_listener.c` (`listener_obj`, 6 new cases, 47 in the suite):
  - init accepts exactly one complete family (neither, both, half and crossed halves are rejected);
  - handoff with lease; teardown disposal with the credit returned; spurious-accept disposal;
  - cross-family entry refused as a failed accept with no credit leak;
  - a completion window of objects behaves identically to fds.

  The 41 pre-existing fd-family cases are unchanged and pass.
- **Named Pipe listener**, `tests/test_pipe_stream.c` (11 new cases, 32 in the suite; MinGW and MSVC on
  IOCP):
  - Keel client to Keel listener echo on one loop; client disconnect gives the server stream EOF;
  - sequential clients; 8 concurrent clients through a window of 2 (BUSY then retry while the loop
    refills);
  - accepted streams outliving a listener freed from inside its own `on_close`; an accepted stream
    staying unbound until `kl_pipe_bind`;
  - close with 5 instances pending (free refused until `on_close`; the name is reusable afterwards);
  - `KL_PIPE_IN_USE` for a squatter and for a second listener;
  - the instance DACL is exactly the current user plus SYSTEM (white-box);
  - `ERROR_PIPE_CONNECTED` yields exactly one asynchronous successful completion (white-box,
    deterministic);
  - 100 repeated close races.

  Every lifetime case checks allocator balance. The refusal on non-IOCP engines is asserted in the
  all-engines case.
- **The tests can fail.** Five mutations, each reverted:
  - the name not claimed → the in-use test failed;
  - `ERROR_PIPE_CONNECTED` not treated as success → the race test failed;
  - a failed accept leaking its instance → four balance and close tests failed;
  - `KlListener` handing off an object while closing → the object teardown test failed, and two
    pre-existing fd-family tests failed too (one shared path);
  - the DACL also granting Everyone → the DACL test failed.
- **`check-pipe-seam` R2** now also requires `ConnectNamedPipe` to be overlapped.
