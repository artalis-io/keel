# Phase 8g — Async / long-lived streaming over the completion loop (design)

Status: **design / scoping (2026-07-31).** No code in this document.
Scope: let streaming responses whose data is produced *over time* (SSE, server-push,
incrementally-produced chunked bodies, WebSocket server frames) drive over a completion
backend (io_uring / IOCP / pollcomp), and make handler-produced stream sends *overlapped*
instead of synchronous. This is the one remaining **functional** gap in the completion
backends (see the completion inventory / `docs/keel_axis_audit.md`); UDP/IOCP pktinfo and
the io_uring perf items are separate and lower-priority.

---

## 1. Problem

The completion driver drives only **synchronous** streams today. `comp_send_stream`
(`completion_driver.c`) runs the whole stream during dispatch:

```c
do { r = kl_response_send(&c->res); } while (r == 1 && c->res.stream_ended);
```

`kl_response_send` routes through the socket seam, which on a completion backend is a
**blocking** send on the accepted socket. Two consequences, both documented in-code:

1. **No async / long-lived streams.** A stream whose data arrives on *later* events —
   SSE emitting over time, long-poll, WebSocket server-initiated frames, a chunked body
   fed from a timer / thread-pool / another connection — cannot be driven: the driver
   expects the handler to produce the entire body during dispatch, then closes.
2. **Head-of-line blocking.** Even a fully-produced stream is sent with **synchronous**
   sends, so a slow client stalls the whole event loop for that send.

### Readiness reference (the behavior to match)

On a readiness backend the same streams work because the loop has **write-readiness**:
`kl_conn_on_writable` (`connection.c:951`) fires on `EPOLLOUT` and calls `kl_response_send`
to flush more; a would-block send buffers into `KlDrain` and is flushed on the next
write-readiness. Producers (SSE `kl_sse_write`, chunked `kl_stream_write`, WS frame writes)
call a **`write_fn`** that goes through the drain, never the socket directly — so producers
are already decoupled from the transport. Long-lived producers keep the connection open and
write whenever new data appears (often after a `kl_async_suspend` / resume).

The completion axis has **no write-readiness** — you post a send and get a completion. So
async streaming needs a *completion-driven* equivalent of the write-readiness flush loop.

---

## 2. The mechanism already exists — generalize it

`comp_on_write` **already sequences** a multi-part send today: the TLS file body is sent one
bounded chunk per `KL_COMP_WRITE` completion (`comp_tls_send_file_chunk`, "sequenced by
comp_on_write so memory stays bounded to one chunk regardless of file size"). That is exactly
the drive model async streaming needs — **each send completion pulls the next chunk** — just
generalized from "the next file chunk" to "the next queued stream chunk / the producer's next
write".

**Core idea:** on a completion loop, a stream's `write_fn` does not send synchronously — it
**enqueues** into the response's `KlDrain` buffer and ensures **one overlapped send is in
flight** (`kl_comp_post_send`). Each `KL_COMP_WRITE` completion (in `comp_on_write`) flushes
the next buffered chunk; when the buffer drains and no send is outstanding, the producer is
free to write more. This is the completion mirror of readiness's `on_writable → drain flush`.

---

## 3. Design

### 3.1 Stream write path (completion)
- Add a completion `write_fn` for `KL_BODY_STREAM` that: append to `c->res.drain`; if no send
  is in flight, `kl_comp_post_send` the front of the drain buffer. Return would-block-style
  backpressure (like readiness) when the drain hits `max_size`.
- `comp_on_write` (on a stream conn): release the completed send, advance the drain buffer,
  and if bytes remain post the next send; if the drain is empty, fire the drain's `on_drain`
  callback (producers use it to resume writing) — the completion counterpart of write-readiness.

### 3.2 Connection lifecycle (the real work)
- A streaming conn must **stay open** after the handler returns (today `comp_send_stream`
  closes). Introduce a `KL_CONN_STREAMING`-style state on the completion path: the conn is
  live, one send may be outstanding, the drain may hold buffered bytes, and the producer may
  be external (suspended awaiting a timer / thread-pool / watcher).
- **Stream end:** close (or, for keep-alive, `kl_comp_post_recv` for the next request) only
  when `stream_ended` **and** the drain is empty **and** no send is outstanding.
- **Interaction with `kl_async_suspend`:** long-lived producers already suspend/resume via the
  async op; the streaming conn must be exempt from the idle-timeout sweep while a stream is
  active (as suspended conns are), and cancellation/close-while-streaming must release the
  drain buffer and the outstanding send op cleanly (the existing `kl_comp_cancel` +
  release-from-completion invariant applies).

### 3.3 Backpressure
- Reuse `KlDrain.max_size` semantics: bound outstanding + buffered bytes; `write_fn` signals
  backpressure to the producer exactly as on readiness. No unbounded posted sends.

### 3.4 Completion-thread-safety for external producers
- An external event (timer, thread-pool `done_fn`, watcher) that produces stream data must run
  on the **loop thread** and call the stream `write_fn` there — the same contract as
  `kl_async_complete` (§ Async Pattern). No new threading model; document it.

### 3.5 TLS streaming
- The TLS path already has `comp_tls_*` (encrypt → `comp_tls_post_encrypted`, file chunks
  sequenced by `comp_on_write`). Extend the same sequencing to general stream chunks
  (encrypt each produced chunk, post, continue on completion). Requires a buffering-BIO TLS
  (the existing completion-TLS constraint).

---

## 4. Phasing (incremental, each independently shippable + gated)

- **8g-1 — overlapped handler-produced streams.** Replace the synchronous `comp_send_stream`
  loop with: post the first chunk, drain subsequent chunks in `comp_on_write`. Removes the
  head-of-line-blocking caveat for streams the handler produces during dispatch. Smallest,
  highest-certainty step; gates `sse` + the server-side streaming tests over completion.
- **8g-2 — async / long-lived streams.** The `KL_CONN_STREAMING` lifecycle: conn stays open,
  external producers (suspended → resumed) write over time. Enables SSE-over-time, long-poll,
  incrementally-fed chunked bodies. The bulk of the work (lifecycle + timeout/cancel + drain).
- **8g-3 — WebSocket server-push over completion.** Bidirectional long-lived: incoming frames
  already handled (`kl_ws_server_on_readable_data`, 8e-1); add server-initiated frame writes on
  the 8g-2 streaming drive. Gates `websocket` server push over completion.
- **8g-4 — TLS streaming over completion.** Generalize `comp_tls_*` chunk sequencing to
  streams (buffering-BIO TLS only).

---

## 5. Orthogonality

Confined to `completion_driver.c` + the existing `response` / `KlDrain` internals. The
`write_fn` / drain abstraction already decouples producers (SSE/WS/chunked) from the transport,
so **no protocol module changes** — SSE/WS keep calling `write_fn`; only its completion-backend
implementation changes. **No public API change** (streaming API is unchanged). The **event
axis is untouched** — the only primitives used are the existing `kl_comp_post_send` +
`comp_on_write`; readiness is unaffected (it already works). No `#ifdef`, no platform symbol in
the driver (backends supply post/drain). This is the completion counterpart of the readiness
`on_writable` loop, nothing more.

## 6. Testing

- Gate over io_uring **and** pollcomp (macOS-testable): `sse`, the server-side streaming
  tests, then `websocket` (8g-3). Add a streaming-over-time case to `smoke-iouring` /
  `smoke-pollcomp` (produce chunks across several loop ticks, assert ordered delivery +
  backpressure). Validate lifecycle under ASan/LSan (`smoke-iouring-asan`) — the open-across-
  events conn + outstanding-send + drain buffer are the UAF-risk surface.
- Cancellation/timeout while streaming: a dedicated test (close mid-stream, idle-timeout a
  suspended streaming conn) — the release-from-completion invariant must hold.

## 7. Risks

- **Lifecycle is the hard part** (8g-2): a conn open across arbitrary later events, with one
  outstanding send + a drain buffer + possible suspension, must interact correctly with the
  idle-timeout sweep, graceful drain, cancellation, and keep-alive reuse — the same class of
  edge cases the memlock/timer/drain fixes this session were about. Sequence 8g-1 first (no
  lifecycle change) to de-risk.
- **Head-of-line vs complexity:** 8g-1 alone removes the synchronous-send stall for the common
  case; 8g-2+ is only needed for genuinely async producers. Ship 8g-1, then gate 8g-2 on a real
  need (SSE-over-time / server-push over a completion backend).
- **TLS streaming** depends on a buffering-BIO TLS; direct-I/O TLS remains out (documented
  completion-TLS constraint).

## 8. Recommendation

Do **8g-1** first (small, removes the HOL caveat, gates `sse`/streaming over completion), then
decide 8g-2 based on whether an async-producer-over-completion workload is actually needed. The
whole of 8g is the last functional gap; after it, the completion backends reach behavioral
parity with readiness for every response mode.
