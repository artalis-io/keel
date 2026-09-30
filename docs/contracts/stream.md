# Keel Stream Transport Contract

**Status: STABLE.** The function+ownership contract below is the committed
public surface, derived from `include/keel/stream.h`, `include/keel/listener.h`, and
`include/keel/connect_op.h`. The struct layouts are **not** ABI; they live in the opt-in
`*_detail.h` headers (embedders recompile); use the accessors. This document describes what ships
today; the historical design plan (Phase A/B/C sequencing, proposed extensions) is preserved in
[stream_transport_design.md](../archive/designs/stream_transport_design.md), and future evolution is
tracked in [the roadmap](../roadmap/roadmap.md).

`KlDatagram` is the sibling atomic-message transport; see [datagram.md](datagram.md).

## The three primitives

| Object | Header | Role |
|---|---|---|
| `KlStream` | `stream.h` | an ordered, connected, reliable byte stream (write queue + strict read pause/resume + graceful-close lifecycle) |
| `KlListener` | `listener.h` | the accept-side state machine (bounded accept window, pool-credit reservation, confirmed detachment) |
| `KlConnectOp` | `connect_op.h` | one outbound connection: resolve → Happy-Eyeballs race → terminal-once |

All three are **model-agnostic**: the same object drives readiness (epoll/kqueue/poll/WSAPoll) and
completion (io_uring/IOCP/pollcomp) backends; the difference is confined to the adapter hooks each
installs. None of them own a socket, timer, or event loop; an adapter supplies those through hooks
and drives the `on_*` entry points. TLS lives **above** the raw stream (the adapter's read/write
hooks encrypt/decrypt); it is not a stream facet.

## KlStream: facets and lifecycle

`kl_stream_init(s, read_buffer, read_capacity)` establishes the base object over a caller-owned
stable read buffer. Three facets are then installed independently and are dormant until their `_init`
(so a plain `KlHttpConn` that never installs them is unaffected): **write**, **read**, **close**.

### Write (atomic, bounded queue)

The write queue is preallocated once at `kl_stream_write_init(s, alloc, capacity)`; steady state is
allocation-free. `kl_stream_write(s, data, len)` is **all-or-none** and returns `KlStreamWriteStatus`:

| Status | Meaning |
|---|---|
| `KL_STREAM_ACCEPTED` | all `len` bytes taken (sent inline and/or queued); Keel owns delivery |
| `KL_STREAM_WOULD_BLOCK` | `len ≤ capacity` but no room right now; **nothing** taken; retry after the queue drains |
| `KL_STREAM_TOO_LARGE` | `len > capacity`; **permanent**; the caller must chunk |
| `KL_STREAM_CLOSED` | the write side is closing; new writes refused |
| `KL_STREAM_ERROR` | fatal writer/submission failure |

Bytes are copied before the call returns; the caller may reuse or free its buffer immediately.
Provider-level short writes are internal (the stream keeps the remainder in its own queue and still
reports `ACCEPTED`); a partial write is never caller-visible.

Draining is driven by the adapter, per model: readiness installs a writer
(`kl_stream_set_writer`) and calls `kl_stream_flush(s)` on a writable signal; completion installs a
submit hook (`kl_stream_set_submit`) and calls `kl_stream_on_write_complete(s, ok)` when a posted
WRITE completes (≤1 send in flight). `kl_stream_write_pending(s)` reports the pending byte count:
queued bytes plus any a copying backend already took for the send in flight.

**Writable-again edge.** `kl_stream_on_writable(s, fn, ctx)` answers one question for a producer
that was told `KL_STREAM_WOULD_BLOCK`: *when should I retry?* After a `WOULD_BLOCK`, `fn` fires at
most once, when the blocked condition has ended. It ends one of two ways, and the retry's own status
tells them apart, so the callback carries no status argument:

```text
WOULD_BLOCK ─► physical progress ─► total pending < capacity ─► fn ─► retry → ACCEPTED
WOULD_BLOCK ─► terminal write failure (capacity never useful) ─► fn ─► retry → KL_STREAM_ERROR
```

"Physical progress" is `kl_stream_flush` (readiness) or `kl_stream_on_write_complete` (completion).
"Total pending" is `kl_stream_write_pending`, in-flight bytes included.

- Only `WOULD_BLOCK` arms it; an `ACCEPTED` write disarms it. Bytes draining never fire it on their
  own: the producer must have observed the blocked state.
- It fires once per arming, whether or not the callback retries. A retry that returns `WOULD_BLOCK`
  again (the write needs more room than has freed) re-arms it, and the next progress point fires it
  again; progress is guaranteed because a `WOULD_BLOCK` means bytes are pending. A retry that
  returns `KL_STREAM_ERROR` does not arm it.
- It never fires from inside `kl_stream_write`.
- In-flight bytes count toward "full". That is why the stream's internal `KlDrain` low-water callback
  is not the signal: with a copying backend the queue is emptied at submit, while those bytes are
  still unacknowledged.
- It never fires once a close has begun; `on_close` is the terminal signal.
- The callback may write (a buffered remainder is flushed at the next progress point, and
  `kl_stream_flush` returns 1 while bytes remain) and may begin a close; detachment is deferred
  until the callback returns. It must not free the stream.

A producer's callback therefore needs no bookkeeping beyond the retry:

```c
static void on_writable(void *ctx) {
    Producer *p = ctx;
    switch (kl_stream_write(p->s, p->next, p->next_len)) {
    case KL_STREAM_ACCEPTED:    /* advance; keep writing until WOULD_BLOCK */ break;
    case KL_STREAM_WOULD_BLOCK: /* re-armed; wait for the next edge */       break;
    default:                    /* terminal: stop producing, close */         break;
    }
}
```

### Read (strict pause/resume)

`kl_stream_read_init(s, completion_mode, deliver, arm, disarm, ctx)` installs the read hooks;
`kl_stream_read_start(s)` arms the first receive. The transport reports a completed receive with
`kl_stream_on_recv(s, len, ok)`; the stream delivers it through `KlStreamReadDeliverFn(ctx, buf, len,
ok)` where `buf` is the stream's stable read buffer.

- **Strict pause**: `kl_stream_pause(s)` stops delivery **and** accumulation. In readiness mode
  `disarm` drops READ interest; in completion mode a recv already posted still completes and is
  **held** undelivered (`kl_stream_read_held(s)` reports this). `kl_stream_resume(s)` delivers the
  single held completion exactly once, then re-arms. Both are idempotent.
- **Termination**: read termination is `ok == 0` ("the stream ended; cause not distinguishable").
  There is **no `recv ≤ 0 → EOF` rule**: `recv == 0` is orderly FIN but `recv < 0` may be
  would-block / interrupted (not terminal) / reset / fatal, and today's completion event carries only
  `bytes`+`ok`. After termination no further read callbacks fire.

### Close (confirmed detachment)

`kl_stream_close_init(s, on_close, ctx)` installs the lifecycle; `kl_stream_set_cancel(...)`
optionally installs recv/send cancel hooks (frozen once closing begins).
`kl_stream_close_begin(s)` is a **graceful** close (drain queued output) and `kl_stream_cancel(s)` is
an **abortive** close (drop the queue, cancel outstanding ops). `kl_stream_close_state(s)` reports
`OPEN`/`CLOSING`/`CLOSED`. `on_close` fires **exactly once**, only after both the receive and send
ops are physically retired (`kl_stream_is_detached(s) == 1`); reuse/free is legal only then.

A graceful close drains accepted output **while delivery remains possible**. A terminal write failure
makes delivery impossible: a failed completion send, or a readiness writer returning -1 (which is now
sticky in both modes, reported as `KL_STREAM_ERROR` by later writes). From then on the undeliverable
queue no longer holds the close, which progresses as an abortive close would. The queued bytes stay
the stream's own memory, released by the owner with `kl_stream_write_free` after `on_close`.
Physical retirement is unaffected: a posted recv or an in-flight send must still retire first, and
`on_close` still fires exactly once.

## KlListener: accept path

`kl_listener_init(l, completion_mode, hooks, ctx)` installs the hooks; `kl_listener_start(l)` begins
accepting. The listener keeps up to `window` accepts posted concurrently
(`kl_listener_set_accept_window`, default 1; readiness is always 1, IOCP uses the AcceptEx backlog),
each holding one reserved pool credit. A completed accept is reported with
`kl_listener_on_accepted(l, fd)` (delivered to the owner through the required `on_accept` hook, which
takes ownership of `fd` and a by-value `KlSlotLease`) or `kl_listener_on_accept_failed(l, error)`
(returns the credit). `kl_listener_notify_slot_free(l)` resumes a listener paused for lack of credit.
A transport whose accepted connection is not a socket uses the **object handoff family** instead: hooks
`on_accept_obj` / `dispose_obj` and `kl_listener_on_accepted_obj(l, conn)`, where `conn` is the adapter's own
connection object, passed through untouched exactly as an fd is (a listener uses exactly one family; the
Windows Named Pipe listener hands over a `KlPipeStream`). See
[listener_accept_handoff.md](../architecture/listener_accept_handoff.md).
`kl_listener_close(l)` retires every posted accept; `on_close` fires once after all have retired
(`kl_listener_is_detached`). Pool-credit accounting is optional (NULL reserve/release = unbounded);
a `KlSlotLease` carries a pool-owned release capability plus a nullable liveness token, so it stays
valid after the listener is freed and is released exactly once by the accepted-connection owner.

## KlConnectOp: outbound connect

`kl_connect_op_init(op, hooks, ctx)` + `kl_connect_op_start(op)` establish one outbound connection:
name resolution (`start_resolve` → `kl_connect_op_on_resolved` / `on_resolve_failed`) followed by
Happy Eyeballs (RFC 8305) racing connect over the resolved list, staggered by an optional Connection
Attempt Delay and bounded by an optional overall deadline (both armed through hooks). The **terminal
fires exactly once** through `on_done`: `KL_CONNECT_SUCCESS` (the winning fd transfers), `..._FAILED`,
or `..._CANCELLED` (`kl_connect_op_cancel`). Every non-winning connected fd is routed to the required
`dispose_fd`. `on_detach` fires once after the terminal and all ops and both timers retire
(`kl_connect_op_is_detached`); re-init is the reuse reset.

## A non-socket KlStream: Windows Named Pipes

`kl_pipe_connect(ctx, "\\\\.\\pipe\\name", &cfg, &p)` (`pipe.h`) hands back an ordinary `KlStream`
(`kl_pipe_stream(p)`) over a named-pipe `HANDLE`. The pipe is not a socket and never touches
`KlSocketProvider` or `KlSocketHandle` (the stream's `fd` stays `KL_INVALID_SOCKET`). The transport
installs the completion-mode hooks above, and the consumer drives it with the same calls as any other
stream. Differences sit below the contract:

- **Engine:** IOCP only. Any other engine returns `KL_PIPE_UNSUPPORTED` before an OS call.
- **Read termination:** EOF / broken pipe / error is the single `ok == 0`, exactly as here.
- **Graceful close:** as for every completion-mode stream, it waits for the posted read to retire
  (peer data or disconnect); `kl_stream_cancel` stops waiting.
- **Lifetime:** `kl_pipe_free` is legal at any time. The memory, the handle and the receive buffer
  outlive every posted op (a completion life token), so a free with I/O outstanding is safe.

Design: [windows_named_pipes.md](../architecture/windows_named_pipes.md).

### Directional streams: anonymous pipe pairs

`kl_anon_pipe_create(ctx, dir, &cfg, &p, &peer)` (`anon_pipe.h`) makes a one-directional pipe. Endpoint
A is the same `KlPipeStream`, carrying **only one facet**: a `KL_ANON_PIPE_READS` pair's stream is
read-only and a `KL_ANON_PIPE_WRITES` pair's is write-only. Endpoint B, the child's end, is a
`KlAnonPipeEnd` the embedder hands to its spawner (native value via `anon_pipe_native.h`) and closes
with `kl_anon_pipe_end_close`. The stream contract needs nothing new for this:

- **The missing facet is refused**, not emulated: a write on a read-only stream returns
  `KL_STREAM_ERROR`, and `kl_stream_read_start` on a write-only stream returns -1.
- **Graceful close of a write-only stream** drains the queue and detaches without waiting for a read
  that was never posted. That close, then `kl_pipe_free`, is how the child sees end of input.
- **Backpressure** on a write-only stream is `KL_STREAM_WOULD_BLOCK` plus the writable-again edge.
- **Engine:** Windows IOCP (a private, single-instance, overlapped named pipe; see
  [process_pipe_streams.md](../architecture/process_pipe_streams.md) §4). Every other engine and
  platform returns `KL_PIPE_UNSUPPORTED`.

## Loop teardown

Detachment needs the loop. Close a stream (or listener) and drive the loop until its close callback
fires, then free the loop. Freeing the loop first delivers no further callback: a posted operation is
reclaimed, but `on_close` never fires. The rule, and exactly which calls stay defined afterwards, is on
`kl_event_ctx_free` in `event_ctx.h`.

## Cross-cutting guarantees

- **Confirmed detachment.** All three objects fire their detach/close callback exactly once, only
  after every outstanding operation (and, for connect, both timers) is physically retired; never
  mid-callback. Reuse is legal only after detachment.
- **Synchronous-completion safe.** `arm`/`resolve`/`attempt`/`submit` hooks may complete inline; the
  machines bound the C stack (iterative arm trampoline) and never re-fire or detach mid-callback.
- **Cancel-once.** Each outstanding op is cancel-requested at most once; a reentrant cancel from a
  callback is safe.
- **Total descriptor ownership.** Every accepted/connected fd is either handed over exactly once
  (`on_accept` / `on_done`) or disposed through the required dispose hook.

## Not currently supported

These are **not** part of the shipped surface (some are tracked in the roadmap / archived design):

- **Scatter-gather write**: there is only `kl_stream_write`; no `writev`.
- **A distinguished close taxonomy**: read termination is a single `ok == 0`; orderly-FIN vs
  reset vs error are not separated (that needs a status field on the completion event).
- **Per-operation cancellation identity**: cancellation is stream-level (`kl_stream_cancel`), not
  per read/write op; the only per-op terminal is the `KlConnectOp`.
- **TLS as a stream facet**: TLS wraps the stream from above (the adapter's hooks), not inside it.
- **Half-close / abort (`shutdown_write` / RST)**: no such provider op today.
- **A tagged address-kind union**: addresses are `KlSockAddr`; there is no `KlEndpoint` type. (A named
  pipe's endpoint is a path argument to `kl_pipe_connect`, not an address kind.)

## Conformance evidence

The contract is exercised model-independently. Readiness runs under `make test`; the completion axis
runs the same suites over io_uring (`make BACKEND=iouring test-iouring`, the `IOURING_TEST_SUITES`
set) and the `pollcomp` double (`make smoke-pollcomp-asan`), plus IOCP on the Windows CI.

| Area | Suites |
|---|---|
| Stream write/read/close | `tests/test_stream.c`, `test_stream_read.c`, `test_stream_close.c`, `test_stream_single_shot.c` |
| Writable-again edge | `tests/test_stream_writable.c` (rules, plus a live socket producer); `test_pipe_stream.c` (`producer_resumes_only_on_the_writable_edge`, IOCP) |
| Public transport surface | `tests/test_stream_transport.c`, `test_transport_public.c` |
| Listener | `tests/test_listener.c` |
| Connect op | `tests/test_connect_op.c` |
| Non-socket stream + listener (named pipe, IOCP) | `tests/test_pipe_stream.c`; object handoff family in `tests/test_listener.c` (`listener_obj`) |
