# Keel streaming & body-read contract (Phase 1)

Authoritative semantics for request-body and response streaming, **identical across the readiness
and completion axes** (the observable contract is the same; only the internal mechanism differs).
Companion to `docs/core_completion_plan.md`. Where a behavior is already enforced in code, the
enforcing symbol is named.

## Write side (response streaming / outbound)

Producers (`kl_response_stream_write`, SSE `kl_sse_write`, chunked, WebSocket server frames, a
handler writing a body) write into the per-connection **outbound buffer** (`KlDrain`), never a
socket or event engine.

**Ownership — bytes are copied immediately.** A streaming write copies the supplied bytes before
returning; the caller may pass transient/stack memory and reuse or free it at once. The completion
backends copy again into the op at post time (`kl_comp_post_send` in all three backends), so no
submitted completion op ever references caller or `KlDrain` memory after the call returns. Keel
never borrows or takes ownership of the caller's write buffer.

**Backpressure — four outcomes** (via the drain, `KlDrain.max_size` bound):
- *accepted* — buffered (and, on readiness, opportunistically sent inline);
- *would-block / queue-full* — the write hit `max_size`; the producer must stop and wait for drain;
- *stream closed* — the peer/connection is gone;
- *error* — allocation or transport failure.

There is **one** writable/drain notification: the drain empties → the producer may write more. On
the completion axis the overlapped flush keeps ≤1 send in flight (`KlResponse.stream_inflight`) and
re-pumps from the WRITE completion (`comp_stream_pump`); on readiness it flushes on writability.
Both surface the same "buffer drained, resume producing" signal — no parallel callbacks with
divergent meaning.

## Read side (request-body streaming / inbound)

A body reader (`KlBodyReader`: `on_data` / `on_complete` / `on_error` / `destroy`) receives body
chunks. `on_data` returns `0` to continue or `-1` to **abort** (→ 413 / connection teardown).

**Flow control — continue / pause / abort:**
- *continue* — `on_data` returns 0.
- *pause* — `kl_request_pause_body(req)`: stop reading more body bytes off the connection, bounding
  accumulation, without aborting. Idempotent; loop-thread only; callable from `on_data` or later
  (a watcher/timer/thread-pool completion when a downstream sink drains). Readiness drops READ
  interest immediately; completion stops posting the next recv — the one already-submitted recv may
  still deliver ≤1 more chunk (bounded). A paused conn holds no unbounded buffer: unread bytes stay
  in the kernel socket buffer, and the parser retains only its partial frame.
- *resume* — `kl_request_resume_body(req)`: re-enable reading. Idempotent (a no-op unless a pause is
  in effect). Readiness re-arms READ; completion posts a fresh recv (`kl_io_engine_post_read`).
- *abort* — `on_data` → -1.

A conn that stays paused is **not** exempt from the idle-read timeout: an indefinitely paused
consumer is eventually timed out (backpressure/slowloris defense).

## Termination (both sides) — exactly one terminal outcome

- **orderly finish** — response fully sent / body fully read → `on_complete`.
- **protocol/application abort** — `on_data` → -1, or the handler errors.
- **cancellation** — the consumer is no longer interested (aborts the body / closes the stream).
- **peer close** — recv 0 / reset → `on_error` then teardown.
- **timeout** — idle/read deadline → teardown.

The connection driver guarantees a single terminal path: the body reader gets `on_complete` XOR
`on_error` (never both), then `destroy`. On teardown while paused, the conn releases through the
normal completion/close path (no dangling op, no double release — verified under ASan by
`test_read_flow_control.shutdown_while_paused`).

## Lifecycle & reentrancy

- The `KlRequest`/`KlConn` (and thus `kl_request_pause_body`/`resume_body`) are valid throughout the
  body-read callbacks. `pause`/`resume` may be called from inside `on_data`.
- The body reader object is owned by Keel and destroyed after `on_complete`/`on_error`; do not use
  it afterward. Recording results into caller-owned state (not the reader) survives destruction.
- All pause/resume/stream calls are single-loop-thread only. Cross-thread hand-off goes through
  `KlThreadPool` + the loop (the pause/resume are then issued from the done-callback on the loop).

## Tests

`tests/test_read_flow_control.c` is model-independent and runs over readiness (`make test`) and the
completion backends (pollcomp locally, io_uring via `IOURING_TEST_SUITES`): pause mid-body → async
timer resume → full body delivered + `on_complete`; and shutdown-while-paused → clean teardown
(ASan-clean). Fragmentation / split header-body completions / large streams / short writes /
backpressure enter-exit / peer reset / timeout are exercised across the existing smoke + integration
suites over both axes.
