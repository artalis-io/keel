# Keel Stream Transport Contract

**Status: STABLE.** The function+ownership contract below is the committed
public surface, derived from `include/keel/stream.h`, `include/keel/listener.h`, and
`include/keel/connect_op.h`. The struct layouts are **not** ABI — they live in the opt-in
`*_detail.h` headers (embedders recompile); use the accessors. This document describes what ships
today; the historical design plan (Phase A/B/C sequencing, proposed extensions) is preserved in
[stream_transport_design.md](../archive/designs/stream_transport_design.md), and future evolution is
tracked in [the roadmap](../roadmap/roadmap.md).

`KlDatagram` is the sibling atomic-message transport — see [datagram.md](datagram.md).

## The three primitives

| Object | Header | Role |
|---|---|---|
| `KlStream` | `stream.h` | an ordered, connected, reliable byte stream (write queue + strict read pause/resume + graceful-close lifecycle) |
| `KlListener` | `listener.h` | the accept-side state machine (bounded accept window, pool-credit reservation, confirmed detachment) |
| `KlConnectOp` | `connect_op.h` | one outbound connection: resolve → Happy-Eyeballs race → terminal-once |

All three are **model-agnostic** — the same object drives readiness (epoll/kqueue/poll/WSAPoll) and
completion (io_uring/IOCP/pollcomp) backends; the difference is confined to the adapter hooks each
installs. None of them own a socket, timer, or event loop — an adapter supplies those through hooks
and drives the `on_*` entry points. TLS lives **above** the raw stream (the adapter's read/write
hooks encrypt/decrypt); it is not a stream facet.

## KlStream — facets and lifecycle

`kl_stream_init(s, read_buffer, read_capacity)` establishes the base object over a caller-owned
stable read buffer. Three facets are then installed independently and are dormant until their `_init`
(so a plain `KlHttpConn` that never installs them is unaffected): **write**, **read**, **close**.

### Write (atomic, bounded queue)

The write queue is preallocated once at `kl_stream_write_init(s, alloc, capacity)`; steady state is
allocation-free. `kl_stream_write(s, data, len)` is **all-or-none** and returns `KlStreamWriteStatus`:

| Status | Meaning |
|---|---|
| `KL_STREAM_ACCEPTED` | all `len` bytes taken (sent inline and/or queued); Keel owns delivery |
| `KL_STREAM_WOULD_BLOCK` | `len ≤ capacity` but no room right now; **nothing** taken — retry after the queue drains |
| `KL_STREAM_TOO_LARGE` | `len > capacity`; **permanent** — the caller must chunk |
| `KL_STREAM_CLOSED` | the write side is closing; new writes refused |
| `KL_STREAM_ERROR` | fatal writer/submission failure |

Bytes are copied before the call returns; the caller may reuse or free its buffer immediately.
Provider-level short writes are internal (the stream keeps the remainder in its own queue and still
reports `ACCEPTED`) — a partial write is never caller-visible.

Draining is driven by the adapter, per model: readiness installs a writer
(`kl_stream_set_writer`) and calls `kl_stream_flush(s)` on a writable signal; completion installs a
submit hook (`kl_stream_set_submit`) and calls `kl_stream_on_write_complete(s, ok)` when a posted
WRITE completes (≤1 send in flight). `kl_stream_write_pending(s)` reports the queued byte count.
**`KlStream` exposes no low-water "writable" callback** — backpressure surfaces as
`KL_STREAM_WOULD_BLOCK`, and a caller that wants an explicit drained-notification composes `KlDrain`
(see `drain.h`) or polls `kl_stream_write_pending`.

### Read (strict pause/resume)

`kl_stream_read_init(s, completion_mode, deliver, arm, disarm, ctx)` installs the read hooks;
`kl_stream_read_start(s)` arms the first receive. The transport reports a completed receive with
`kl_stream_on_recv(s, len, ok)`; the stream delivers it through `KlStreamReadDeliverFn(ctx, buf, len,
ok)` where `buf` is the stream's stable read buffer.

- **Strict pause** — `kl_stream_pause(s)` stops delivery **and** accumulation. In readiness mode
  `disarm` drops READ interest; in completion mode a recv already posted still completes and is
  **held** undelivered (`kl_stream_read_held(s)` reports this). `kl_stream_resume(s)` delivers the
  single held completion exactly once, then re-arms. Both are idempotent.
- **Termination** — read termination is `ok == 0` ("the stream ended; cause not distinguishable").
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

## KlListener — accept path

`kl_listener_init(l, completion_mode, hooks, ctx)` installs the hooks; `kl_listener_start(l)` begins
accepting. The listener keeps up to `window` accepts posted concurrently
(`kl_listener_set_accept_window`, default 1; readiness is always 1, IOCP uses the AcceptEx backlog),
each holding one reserved pool credit. A completed accept is reported with
`kl_listener_on_accepted(l, fd)` (delivered to the owner through the required `on_accept` hook, which
takes ownership of `fd` and a by-value `KlSlotLease`) or `kl_listener_on_accept_failed(l, error)`
(returns the credit). `kl_listener_notify_slot_free(l)` resumes a listener paused for lack of credit.
`kl_listener_close(l)` retires every posted accept; `on_close` fires once after all have retired
(`kl_listener_is_detached`). Pool-credit accounting is optional (NULL reserve/release = unbounded);
a `KlSlotLease` carries a pool-owned release capability plus a nullable liveness token, so it stays
valid after the listener is freed and is released exactly once by the accepted-connection owner.

## KlConnectOp — outbound connect

`kl_connect_op_init(op, hooks, ctx)` + `kl_connect_op_start(op)` establish one outbound connection:
name resolution (`start_resolve` → `kl_connect_op_on_resolved` / `on_resolve_failed`) followed by
Happy Eyeballs (RFC 8305) racing connect over the resolved list, staggered by an optional Connection
Attempt Delay and bounded by an optional overall deadline (both armed through hooks). The **terminal
fires exactly once** through `on_done` — `KL_CONNECT_SUCCESS` (the winning fd transfers), `..._FAILED`,
or `..._CANCELLED` (`kl_connect_op_cancel`). Every non-winning connected fd is routed to the required
`dispose_fd`. `on_detach` fires once after the terminal and all ops and both timers retire
(`kl_connect_op_is_detached`); re-init is the reuse reset.

## Cross-cutting guarantees

- **Confirmed detachment.** All three objects fire their detach/close callback exactly once, only
  after every outstanding operation (and, for connect, both timers) is physically retired — never
  mid-callback. Reuse is legal only after detachment.
- **Synchronous-completion safe.** `arm`/`resolve`/`attempt`/`submit` hooks may complete inline; the
  machines bound the C stack (iterative arm trampoline) and never re-fire or detach mid-callback.
- **Cancel-once.** Each outstanding op is cancel-requested at most once; a reentrant cancel from a
  callback is safe.
- **Total descriptor ownership.** Every accepted/connected fd is either handed over exactly once
  (`on_accept` / `on_done`) or disposed through the required dispose hook.

## Not currently supported

These are **not** part of the shipped surface (some are tracked in the roadmap / archived design):

- **Scatter-gather write** — there is only `kl_stream_write`; no `writev`.
- **A distinguished close taxonomy** — read termination is a single `ok == 0`; orderly-FIN vs
  reset vs error are not separated (that needs a status field on the completion event).
- **Per-operation cancellation identity** — cancellation is stream-level (`kl_stream_cancel`), not
  per read/write op; the only per-op terminal is the `KlConnectOp`.
- **TLS as a stream facet** — TLS wraps the stream from above (the adapter's hooks), not inside it.
- **Half-close / abort (`shutdown_write` / RST)** — no such provider op today.
- **A tagged address-kind union** — addresses are `KlSockAddr`; there is no `KlEndpoint` type.

## Conformance evidence

The contract is exercised model-independently. Readiness runs under `make test`; the completion axis
runs the same suites over io_uring (`make BACKEND=iouring test-iouring`, the `IOURING_TEST_SUITES`
set) and the `pollcomp` double (`make smoke-pollcomp-asan`), plus IOCP on the Windows CI.

| Area | Suites |
|---|---|
| Stream write/read/close | `tests/test_stream.c`, `test_stream_read.c`, `test_stream_close.c`, `test_stream_single_shot.c` |
| Public transport surface | `tests/test_stream_transport.c`, `test_transport_public.c` |
| Listener | `tests/test_listener.c` |
| Connect op | `tests/test_connect_op.c` |
