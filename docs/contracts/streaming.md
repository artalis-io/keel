# Keel streaming & body-read contract (Phase 1)

Authoritative semantics for request-body and response streaming, **identical across the readiness
and completion axes** (the observable contract is the same; only the internal mechanism differs).
Companion to `docs/archive/designs/core_completion_plan.md`. Where a behavior is already enforced in
code, the enforcing symbol is named.

## Write side (response streaming / outbound)

Producers (the `KlHttpResponseWriteFn` returned by `kl_http_response_begin_stream`, SSE
`kl_http_sse_event`, chunked, WebSocket server frames, a handler writing a body) write into the
per-connection **outbound buffer** (`KlDrain`), never a
socket or event engine.

**Ownership: bytes are copied immediately.** A streaming write copies the supplied bytes before
returning; the caller may pass transient/stack memory and reuse or free it at once. A completion
backend may copy transport-owned bytes into its operation or borrow their submitted region until
physical retirement. Keel pins a borrowed region until that completion; growing or compacting a
queue must not move or release it. This backend policy does not change the immediate copying of
application bytes: Keel never borrows or takes ownership of the caller's write buffer.

**Backpressure: four outcomes** (via the drain, `KlDrain.max_size` bound):
- *accepted*: buffered (and, on readiness, opportunistically sent inline);
- *would-block / queue-full*: the write hit `max_size`; the producer must stop and wait for drain;
- *stream closed*: the peer/connection is gone;
- *error*: allocation or transport failure.

There is **one** writable/drain notification: the drain empties → the producer may write more (a
producer that still owns the stream: see "Producing later" below). On
the completion axis the bytes go onto the connection's output queue (the one TLS, WebSocket and HTTP/2
output use), which keeps ≤1 send in flight and moves a producer's backlog onto the queue from the WRITE
completion (`comp_tls_on_write`); nothing is ever sent synchronously on the loop thread. On readiness
it flushes on writability, including while the connection is suspended: a suspended connection whose
stream output is waiting has its fd watched for writability (on the same watcher as the hang-up watch)
until the drain empties, so a producer that stops at a high-water mark and resumes from `on_drain`
needs nothing else to wake it.
Both surface the same "buffer drained, resume producing" signal; no parallel callbacks with
divergent meaning.

**Bounded streaming: `kl_http_response_stream_try_writev`.** The non-fatal form of the same
outcomes, with KlStream's write contract (`docs/contracts/stream.md`, "Write") for a chunked
response:

- *One bound.* Every byte Keel holds for the response and has not handed to the kernel counts:
  its outbound buffer and, on a completion loop, the connection's output queue (posted sends
  included; ciphertext with TLS). The bound is the response's stream capacity,
  `KlHttpServerConfig.stream_capacity` (default 1 MiB, per response; `kl_http_response_stream_pending`
  and `kl_http_response_stream_capacity` read both).
- *Atomic admission.* A write is framed as one chunk and admitted whole (`ACCEPTED`) or not at all:
  `WOULD_BLOCK` (it fits the capacity, not the room left now), `TOO_LARGE` (it never fits),
  `CLOSED` (no open stream), `ERROR` (the stream failed; sticky). No byte of a refused write, framing
  included, reaches any queue. Because admission is decided against the one total before any byte
  moves, no layer below can refuse part of an admitted chunk; only an allocation or transport failure
  can stop one part way, and that is `ERROR` and ends the connection.
- *Writable edge* (`kl_http_response_stream_on_writable`): armed only by `WOULD_BLOCK`, disarmed by
  `ACCEPTED`, fired at most once per arming when pending falls below the capacity, from a progress
  point (a readiness writable flush of a suspended connection, a completion send completion), never
  from inside a write. A retry that does not fit yet returns `WOULD_BLOCK` again and re-arms. It fires
  only while the connection is suspended (a producer owns the stream), and never after the stream
  ended, failed or was reset, or after the op completed or was cancelled: `on_cancel` is the terminal
  signal. No wakeup is lost: `WOULD_BLOCK` means bytes are pending, so a progress point is coming (the
  suspended connection's write watch on readiness, the in-flight send's completion on completion).
- *End of stream.* `kl_http_response_end_stream` is never refused by the bound: its 5-byte terminal
  chunk goes behind everything accepted, outside the admission check.
- *Memory.* The capacity bounds pending bytes, not memory. Allocated capacity, for capacity C, the
  16 KiB flush chunk F and the TLS overhead O of what is queued (one record per TLS write, about 22
  to 29 bytes each; an admitted chunk is written in up to three writes, its size line with the first
  4 KiB, the rest, and the CRLF, so about 66 to 87 bytes per chunk), for a response written only
  through the bounded API:
  - outbound buffer: at most C + 5 (grows from 4 KiB by doubling, clamped to its cap C; the 5-byte
    terminator's allowance; never shrinks before the response is reset). A drain the caller enabled
    with no cap (`kl_http_response_enable_drain(..., 0)`) can reach just under 2(C + 5);
  - completion output queue: at most 2P, where P < 2(C + 5 + O + F) is its buffer (the smallest
    F times a power of two holding what is queued plus one absorbed TLS chunk, never smaller than
    before) and the second P is the previous buffer, kept while a send from it is in flight;
  - a completion backend's copy of the send in flight: at most min(256 KiB, C + O);
  - the response headers (a 512-byte buffer and up) and the TLS engine's fixed buffers.
  So readiness holds at most C + 5 plus headers and fixed TLS buffers, and a completion loop at most
  (C + 5) + 4(C + 5 + O + F) + min(256 KiB, C + O) plus the same: about 5C + 64 KiB + min(256 KiB, C).
  With TLS pending can exceed C by O (admission counts plaintext; the queue holds ciphertext): for
  C up to 1 MiB on a completion loop that is the last admitted chunk's records, since the outbound
  buffer then never holds plaintext (the queue's own 1 MiB producer bound is never reached); above
  that, plaintext waiting in the outbound buffer adds its records when it is encrypted. None of it
  grows with the response's length. (Measured:
  relaying 32 MiB at C = 64 KiB, the relay server's own allocations grew by 77 KiB on kqueue and
  under 300 KiB on pollcomp.)
- *The legacy writer* (`begin_stream`'s `KlHttpResponseWriteFn`) keeps its semantics: fatal at its
  outbound buffer's cap (`stream_capacity`, 1 MiB by default), and on a completion loop not counting
  the output queue, so a producer pacing itself on `kl_drain_buffered` keeps working. A suspended
  producer's total stays finite (outbound buffer plus the queue's own 1 MiB producer bound); inside
  the handler, on a completion loop, the queue still takes everything the handler writes until it
  returns (nothing can be sent meanwhile), as before. The bounded API counts the queue in both
  cases, so it is the one to use when the total must be bounded.
- *Mixing the two writers on one response.* The capacity bound holds only for a response written
  exclusively through the bounded API. Legacy writes count toward pending output but are not refused
  by the bound, so pending can exceed the capacity. Framing and order stay intact either way (both
  append whole chunks to one ordered pipeline); a bounded write then returns `WOULD_BLOCK` until
  pending falls back under the capacity, and its edge fires when it does.
- *Compressed streams* (`kl_http_compress_stream_try_write` / `_try_end` / `_release`,
  `<keel/http_compress.h>`): the same statuses, capacity and edge.
  - *Admission.* A write is admitted against the codec's worst case for that input
    (`KlCompress.flush_bound`, framed as one chunk) before the codec is touched, so a refused write
    leaves the codec and the output unchanged. The bound's overflow value, `SIZE_MAX`, is
    `TOO_LARGE`. An accepted write is compressed and flushed whole (`KL_COMPRESS_FLUSH_SYNC`): its
    bytes go out as one chunk at once, decodable on arrival, and the codec retains nothing for later
    output. A HEAD response takes writes without running the codec.
  - *Failure.* A codec without `flush_bound` is refused (`ERROR`) and never fed. Once a write is
    admitted, a failed staging allocation, a codec failure, or output past the codec's own bound is
    `ERROR` for the compressed stream and the response's stream alike (sticky; the connection
    closes): the codec may have advanced, so nothing could resume it.
  - *Finish.* `_try_end` admits the codec's finish the same way: `WOULD_BLOCK` arms the edge and
    leaves the codec untouched however often it is retried; the finish's worst case is fixed once
    nothing more is written, so it fits once pending drains far enough. `TOO_LARGE` means it never
    can (a capacity below the codec's finish, a few dozen bytes for gzip): permanent, and the
    stream is released with `_release`, which writes nothing (also the way to abandon a stream from
    `on_cancel`). The terminal chunk then follows outside the bound as above.
  - *Mixing with the legacy compressed writer* (`kl_http_compress_stream_write`, which does not
    flush): `flush_bound` counts the output still owed for input such a write left in the codec, so
    a later bounded write or finish stays within what it was admitted against. The capacity bound
    itself holds, as for uncompressed streams, only for a response written exclusively through the
    bounded API: the legacy writer is not refused by it.
  - *Memory.* In addition to the stream's own terms above: the codec's session (fixed; miniz's
    deflate state is 319,352 bytes) and one staging buffer holding a flush's output before it is
    admitted, at most the largest admitted bound and so under C (kept and reused, freed when the
    stream ends or is released). Nothing else is retained: with every accepted write flushed the codec
    holds no input or output for later. So a compressed stream adds at most C plus the codec's fixed
    state, independent of the body's length. (Measured: 12 MiB through a stalled reader at
    C = 64 KiB peaked at 458 KB of server allocations in all, codec state included.)
  - *Cost.* Flushing every write costs ratio on very small writes: measured with miniz at level 6, a
    stream of ~180-byte SSE events became about 15% of its size on the wire instead of 1.4%
    unflushed, while 1 to 4 KiB writes cost 1.6 points and 64 KiB writes nothing measurable; the
    unflushed stream delivered none of those events until it ended.

**Producing later: suspend.** A stream belongs to its producer only while the handler runs or the
connection is suspended (`kl_async_suspend`). To push events later (an SSE feed driven by a timer,
a watcher or another connection), begin the stream, suspend, write while suspended, and end the
stream from the resume (`on_resume`). A suspended connection is out of the event loop for reads (only
its hang-up, per `KlHttpServerConfig.suspended_hangup`, is watched; on a readiness loop, while stream
output waits, so is its writability, which moves the output) and exempt from the idle timeout, so
a quiet stream costs nothing and is not cut off at `read_timeout_ms`. A
handler or resume that returns without suspending gives the response up: what it wrote is flushed
(the outbound buffer, if anything is still in it) and the connection then ends, on both axes. A
stream that was never ended (no terminating chunk) is never kept alive for another request. A
drain callback registered on the response (`kl_drain_on_drain` on `res->drain`) does not keep an
unsuspended stream open: once the drain empties with the stream unended and the connection not
suspended, the response is over.

## Read side (request-body streaming / inbound)

A body reader (`KlHttpBodyReader`: `on_data` / `on_complete` / `on_error` / `destroy`) receives body
chunks. `on_data` returns `0` to continue or `-1` to **abort** (→ 413 / connection teardown).

**Flow control: continue / pause / abort:**
- *continue*: `on_data` returns 0.
- *pause*: `kl_http_request_pause_body(req)`: stop reading more body bytes off the connection, bounding
  accumulation, without aborting. Idempotent; loop-thread only; callable from `on_data` or later
  (a watcher/timer/thread-pool completion when a downstream sink drains). Readiness drops READ
  interest immediately; completion stops posting the next recv; the one already-submitted recv may
  still deliver ≤1 more chunk (bounded). A paused conn holds no unbounded buffer: unread bytes stay
  in the kernel socket buffer, and the parser retains only its partial frame.
- *resume*: `kl_http_request_resume_body(req)`: re-enable reading. Idempotent (a no-op unless a pause is
  in effect). Readiness re-arms READ; completion posts a fresh recv (`kl_http_comp_post_read`).
- *abort*: `on_data` → -1.

A conn that stays paused is **not** exempt from the idle-read timeout: an indefinitely paused
consumer is eventually timed out (backpressure/slowloris defense).

## Termination (both sides): exactly one terminal outcome

- **orderly finish**: response fully sent / body fully read → `on_complete`.
- **protocol/application abort**: `on_data` → -1, or the handler errors.
- **cancellation**: the consumer is no longer interested (aborts the body / closes the stream).
- **peer close**: recv 0 / reset → `on_error` then teardown.
- **timeout**: idle/read deadline → teardown.

The connection driver guarantees a single terminal path: the body reader gets `on_complete` XOR
`on_error` (never both), then `destroy`. On teardown while paused, the conn releases through the
normal completion/close path (no dangling op, no double release; verified under ASan by
`test_read_flow_control.shutdown_while_paused`).

## Lifecycle & reentrancy

- The `KlHttpRequest`/`KlHttpConn` (and thus `kl_http_request_pause_body`/`resume_body`) are valid throughout the
  body-read callbacks. `pause`/`resume` may be called from inside `on_data`.
- The body reader object is owned by Keel and destroyed after `on_complete`/`on_error`; do not use
  it afterward. Recording results into caller-owned state (not the reader) survives destruction.
- All pause/resume/stream calls are single-loop-thread only. Cross-thread hand-off goes through
  `KlThreadPool` + the loop (the pause/resume are then issued from the done-callback on the loop).

## Tests

`tests/protocols/http/test_read_flow_control.c` is model-independent and runs over readiness (`make test`) and the
completion backends (pollcomp locally, io_uring via `IOURING_TEST_SUITES`): pause mid-body → async
timer resume → full body delivered + `on_complete`; and shutdown-while-paused → clean teardown
(ASan-clean). Fragmentation / split header-body completions / large streams / short writes /
backpressure enter-exit / peer reset / timeout are exercised across the existing smoke + integration
suites over both axes.
