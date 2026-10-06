# Keel async operation lifecycle (Phase 4)

Authoritative contract for `KlAsyncOp`, the primitive that suspends a connection
while a handler waits on out-of-band work (a thread-pool job, a timer, an async
resolve, a completion from another fd). Companion to `docs/contracts/streaming.md`
and `docs/operations/capability_matrix.md`; identical across the readiness and completion
axes (only the resume mechanism differs).

## States

An op is **pending** from `kl_async_suspend()` until exactly one **terminal**
transition retires it. There are exactly two terminals:

- **resume**: `kl_async_complete()` fires `on_resume`, re-arms the fd (readiness)
  or re-drives the completion send path, and advances the connection state
  machine. The success path.
- **cancel**: `kl_async_cancel()` fires `on_cancel` and closes the connection
  without a response. The abnormal-termination path. A connection the server itself tears down (it died,
  or the server is freed) has its op cancelled the same way, minus the close the
  server does anyway.

Either terminal may be reached from inside a callback that is already driving the
connection (the handler that suspended it, a body reader's `on_data`, another op's
`on_resume`): a completion then sends the response once that callback returns, a
cancel closes the connection then, so it is driven and released once.

`on_deadline` is **not** a terminal; it is a *trigger*. When `deadline_ms` is
reached the loop fires `on_deadline` exactly once; that callback must resolve the
op by calling either `kl_async_complete()` (deadline-as-success, e.g. a sleep) or
`kl_async_cancel()` (deadline-as-failure, e.g. an HTTP timeout). This split lets
one mechanism serve opposite semantics (see the `KlAsyncOp` doc in `async.h`).

## Guarantees (exactly one terminal result)

1. **At most one terminal callback.** `on_resume` XOR `on_cancel` fires, never
   both, never twice. Enforced by an internal `_terminal` flag flipped by the
   shared `async_retire()` helper; `kl_async_complete()` and `kl_async_cancel()`
   both no-op on an already-retired op.
2. **`on_deadline` fires at most once.** The deadline sweeps clear `deadline_ms`
   before invoking it, so a callback that fails to retire the op cannot cause a
   re-fire on the next tick.
3. **A cancel racing a completion is safe.** Whichever runs first retires the op;
   the other is a no-op. No double release, no use-after-free, no callback after
   the owner has torn down.
4. **No silent loss.** `kl_http_server_free()` cancels every still-pending op
   (`kl_async_cancel` on each), so `on_cancel` runs and the caller's async
   context is always cleaned up. This guarantee is specific to `KlAsyncOp`, which the server
   cancels explicitly **before** it frees its own loop. Freeing an event loop by itself delivers no
   callbacks to anything still attached (see `kl_event_ctx_free` in `event_ctx.h`).
5. **A connection that dies cancels its op.** When a suspended connection is released because it
   died (on a completion loop, a send posted while it was suspended, such as a stream chunk, failed
   because the client went), its op is cancelled then: `on_cancel` runs from inside the loop's I/O
   processing, and a later `kl_async_complete()` is a no-op. Work the caller started for the op (a
   thread-pool job, a timer) may still be outstanding at that point, so `on_cancel` marks the
   caller's context dead and frees it only once that work has finished; it does not write to the
   response.
6. **Op reuse.** `kl_async_suspend()` re-arms the op (clears `_terminal`), so the
   same `KlAsyncOp` struct may back a fresh suspension after a prior terminal
   (e.g. a handler that yields repeatedly).

## Threading

`kl_async_complete()` / `kl_async_cancel()` are **not** thread-safe; they
mutate the server's active-ops list. Call them only on the event-loop thread:
from a watcher callback, a timer callback, or the thread pool's `done_fn` (which
runs on the loop thread), never from a worker thread. See the thread-pool section
in `CLAUDE.md`.

## Tests

`tests/protocols/http/test_http_async.c` covers the guarantees directly: double `kl_async_complete`
fires `on_resume` once; `kl_async_cancel` is idempotent; cancel-after-complete
and complete-after-cancel are both no-ops; and a re-suspend after a terminal makes
the op pending again. The completion-axis resume path is exercised by
`smoke-pollcomp-async` and the io_uring `/astream` case.
