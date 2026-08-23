# Keel async operation lifecycle (Phase 4)

Authoritative contract for `KlAsyncOp` — the primitive that suspends a connection
while a handler waits on out-of-band work (a thread-pool job, a timer, an async
resolve, a completion from another fd). Companion to `docs/streaming_contract.md`
and `docs/capability_matrix.md`; identical across the readiness and completion
axes (only the resume mechanism differs).

## States

An op is **pending** from `kl_async_suspend()` until exactly one **terminal**
transition retires it. There are exactly two terminals:

- **resume** — `kl_async_complete()` fires `on_resume`, re-arms the fd (readiness)
  or re-drives the completion send path, and advances the connection state
  machine. The success path.
- **cancel** — `kl_async_cancel()` fires `on_cancel` and does *not* touch the fd
  or state machine (the caller is tearing the connection down). The
  abnormal-termination path.

`on_deadline` is **not** a terminal — it is a *trigger*. When `deadline_ms` is
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
   context is always cleaned up.
5. **Op reuse.** `kl_async_suspend()` re-arms the op (clears `_terminal`), so the
   same `KlAsyncOp` struct may back a fresh suspension after a prior terminal
   (e.g. a handler that yields repeatedly).

## Threading

`kl_async_complete()` / `kl_async_cancel()` are **not** thread-safe — they
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
