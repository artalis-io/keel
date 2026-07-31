# Phase 8g — Streaming over the completion loop (design, corrected)

Status: **design / scoping (2026-07-31, rev 2).** No code in this document.
Scope: give streaming responses (SSE, chunked, WebSocket server frames, incrementally-
produced bodies) a transport-neutral outbound path so they drive correctly — and
*overlapped*, without head-of-line blocking — over a completion backend (io_uring / IOCP /
pollcomp), while also fixing streaming backpressure on the readiness backends. This is the
last **functional** gap in the completion backends (see `docs/keel_axis_audit.md`).

> **Implementation status (2026-07-31).** 8g-0 shipped (#126: the outbound `KlDrain` seam
> wired by default + readiness backpressure fix). **8g-2's functional goal is already met and
> now proven:** an async / long-lived streaming handler (write a chunk → `kl_async_suspend` on
> a one-shot timer → resume → write the next chunk → end, i.e. body produced across event-loop
> ticks) drives correctly over io_uring — verified by a new `/astream` case in
> `smoke-iouring` (ASan/LSan-clean on the async-ctx lifecycle). The async-suspend/resume
> completion infra (`kl_io_engine_resume_completion` + `comp_after_state(SUSPENDED)`), the
> #121 timer-fire fix, and 8g-0's drain together already make async streaming work over
> completion. So no `KL_CONN_STREAMING` lifecycle rewrite is needed.
>
> **8g-1 shipped (#130): head-of-line blocking is fixed.** Completion stream sends were
> synchronous (busy-spinning `kl_drain_flush` on the accepted socket), so a slow client stalled
> the loop thread. They are now overlapped: `comp_send_stream`/`comp_stream_pump` post the
> outbound buffer via `kl_comp_post_send` (bounded, ≤1 send in flight via
> `KlResponse.stream_inflight`), and `comp_on_write` re-pumps the next chunk or completes when
> the stream ended and the buffer drained — using the same overlapped drive as buffered/file
> responses, over the two new `KlDrain` peek/consume accessors (`kl_drain_data`/
> `kl_drain_consume`). A `/bigstream` slow-reader + concurrent-fast-client HOL test on all three
> completion smokes guards it. This closes the last **functional** gap in the completion backends
> (see `docs/keel_axis_audit.md`); the §7 synchronous-handler memory tension is unchanged (a
> bounded-synchronous stream must fit the drain cap). Only non-functional refinements remain
> (io_uring multishot/buf-rings, `TransmitFile` >2 GiB chunking).

> **Revision note.** Rev 1 assumed streaming was `KlDrain`-buffered and framed 8g-1 as
> "post the first chunk, drain the rest in `comp_on_write`". Reading the actual wiring
> corrected that: `kl_response_enable_drain` has **no in-tree callers** — the drain is
> opt-in and **not wired into streaming**. Default streaming is a **synchronous spin-write**
> (`kl_response_send` → `stream_writev_all` → blocking socket send, inside `response.c`), and
> long-lived streams are handled by `kl_async_suspend`/resume writing a chunk at a time. So
> the real work is introducing a transport-neutral outbound path, not draining an existing
> one. This rev reflects that.

---

## 1. Problem (accurate)

`kl_response_send` → `stream_writev_all` writes stream chunks **synchronously to the socket
from inside `response.c`** (the protocol layer). Three consequences:

1. **Head-of-line blocking on every backend.** A slow client stalls the loop on the
   blocking chunk send; there is no default backpressure (the drain exists as a module but is
   unwired).
2. **Completion streaming can't be overlapped.** The protocol layer must not call the
   completion axis (`kl_comp_post_send`) — that would couple protocols to io_uring/IOCP — so
   there's no seam through which the driver can post stream chunks overlapped.
3. **Async / long-lived streams don't drive over completion.** `comp_send_stream` runs the
   synchronous spin-write once during dispatch and finishes; a producer that emits over later
   events (SSE-over-time, server-push, a body fed from a timer / thread-pool / another conn
   via `kl_async_suspend`/resume) is never re-driven on the completion loop.

The completion driver already sends **buffered** responses and **file** bodies the right way
— `kl_comp_post_send` + `comp_on_write` (+ per-chunk sequencing for files, so memory stays
bounded). Streaming just needs to become a third producer feeding that same overlapped drive.

---

## 2. Core abstraction — one outbound-buffer seam, per-transport flush

Insert a single indirection between the streaming producer and the socket: a per-connection
**outbound stream buffer** (`KlDrain` is exactly this shape). Producers write to the buffer,
**never the socket**; each **transport supplies a flush strategy**.

```
 producer (SSE / chunked / WS / handler)              <- transport-neutral
        |  write_fn: append; return would-block when buffer at max_size
        v
 outbound stream buffer  (KlDrain: bounded FIFO)      <- the seam
        |  flush strategy (chosen by the event model, not the producer)
        |-- readiness   : kl_conn_on_writable -> non-blocking send + re-arm write while pending
        `-- completion  : kl_comp_post_send(front) -> comp_on_write posts next while pending
        |  on_drain: buffer emptied -> producer may write more
        v
 resume producer   (sync handler: continue; async: kl_async_complete-style resume)
```

This mirrors the driver's existing buffered/file drives; streaming becomes a third feeder of
the same `comp_post_send`/`comp_on_write` machinery. The `write_fn`/`on_drain` shape is
identical on both axes, so **producers are transport-agnostic**.

---

## 3. Layers

1. **Producer** — SSE (`kl_sse_write`), chunked (`kl_response_stream_write`), WS frame writes,
   or a handler. Writes chunks via `write_fn`; on backpressure (buffer at `max_size`) registers
   `on_drain` and stops. Knows nothing about the event engine. A synchronous handler that fits
   the buffer just writes and returns; genuine streaming uses the async pattern (write -> maybe
   yield -> resume on `on_drain`) — **the same on readiness and completion**.
2. **Outbound buffer** — `KlDrain`, wired **by default** for `KL_BODY_STREAM` (the correction:
   it's currently opt-in and unused). Bounded; the single place pending bytes live.
3. **Transport flush strategy:**
   - **Readiness** — already largely exists: `kl_conn_on_writable` flushes on write-readiness;
     point it at the buffer and keep write interest armed while pending.
   - **Completion (new)** — post the buffer front via `kl_comp_post_send`; `comp_on_write`
     posts the next while bytes remain; fire `on_drain` when it empties. **Bounded memory**
     (one buffer + <=1 outstanding op), overlapped, no HOL. The completion counterpart of
     `on_writable`.

---

## 4. Connection lifecycle (the substantive part)

A streaming conn must live across later events:

- **State** — a `KL_CONN_STREAMING` mode on the completion path: open, <=1 outstanding send,
  buffer may hold bytes, producer possibly suspended awaiting a timer / thread-pool / watcher.
- **Idle sweep** — exempt while a stream is active, as suspended conns already are.
- **End condition** — close (or `kl_comp_post_recv` for keep-alive) only when `stream_ended`
  **and** the buffer is empty **and** no send is outstanding.
- **Cancel / close mid-stream** — release the buffer + the outstanding send op through the
  existing release-from-completion invariant (no dangling op, no double free).
- **Async producers** — resume on the loop thread only (same contract as `kl_async_complete`);
  a resumed producer calls the same `write_fn`. No new threading model.

---

## 5. Orthogonality

Producers write to the buffer (transport-neutral); each backend supplies its flush strategy;
**no protocol module touches a socket or event engine**, **no public API change** (the
streaming API is unchanged), **no `#ifdef`**. The completion flush lives in
`completion_driver.c` over the existing `kl_comp_post_send`/`comp_on_write` primitives;
readiness is unaffected structurally (it already flushes on writability). Wiring the buffer as
the default streaming path also **fixes readiness streaming backpressure** (removes its
spin-write blocking) — a bonus of doing it right rather than completion-only.

---

## 6. Phasing (corrected)

- **8g-0 — foundation: transport-neutral outbound streaming buffer.** Wire `KlDrain` (bounded)
  as the default for `KL_BODY_STREAM`; route `kl_response_stream_write` / `kl_sse_write` /
  chunked through it; add a transport flush hook. Readiness points it at `kl_conn_on_writable`.
  This is the seam everything else builds on and improves both axes. *Prerequisite.*
- **8g-1 — completion overlapped flush.** Implement the completion flush strategy:
  `comp_on_write` drives the buffer via `kl_comp_post_send`; `on_drain` resumes the producer.
  Removes HOL for handler-produced streams over completion, bounded memory. (This is the real
  8g-1; it depends on 8g-0.)
- **8g-2 — async / long-lived lifecycle.** The `KL_CONN_STREAMING` state, idle-sweep exemption,
  external-producer resume, cancel/close cleanup. Enables SSE-over-time / long-poll /
  server-push over completion. The bulk of the work and the main win.
- **8g-3 — WebSocket server-push over completion** (incoming frames already handled, 8e-1; add
  server-initiated frame writes on the 8g-2 drive).
- **8g-4 — TLS streaming over completion** (generalize `comp_tls_*` chunk sequencing; buffering-
  BIO TLS only).

---

## 7. The synchronous-handler tension (explicit)

A **synchronous** handler that produces a whole large body can't interleave overlapped sends
without buffering it all — it never yields. So the design blesses the **async producer** as the
real streaming path (write -> backpressure -> resume on `on_drain`), and treats bounded-
synchronous production as a convenience that must fit the buffer (else backpressure / error).
This is why **8g-2 (async) is the substantive win** and 8g-0/8g-1 are the enabling plumbing —
not the other way round.

---

## 8. Testing

Gate over io_uring **and** pollcomp (macOS-testable): `sse`, the server-side streaming tests,
then `websocket` (8g-3). Add a streaming-over-time case to `smoke-iouring` / `smoke-pollcomp`
(produce chunks across several loop ticks; assert ordered delivery + backpressure). Validate
lifecycle under ASan/LSan (`smoke-iouring-asan`) — the open-across-events conn + outstanding
send + buffer are the UAF surface. A dedicated cancel/timeout-mid-stream test guards the
release-from-completion invariant. Cross-check readiness parity (`make test`) since 8g-0
changes the readiness streaming path too.

## 9. Risks

- **8g-0 changes the readiness streaming path** (spin-write -> buffered + writable-flush). Real
  blast radius; validate `sse`/streaming/`websocket` on readiness before/after. Mitigation:
  keep the spin-write fallback for the "fits in one write, no backpressure" fast path.
- **8g-2 lifecycle** is the same edge-case class as this session's memlock/timer/drain fixes
  (conn open across events x idle sweep x cancel x keep-alive). Sequence 8g-0/8g-1 first.
- **Memory** — the buffer must stay bounded (`max_size`); a producer outrunning the flush hits
  backpressure, not unbounded growth.

## 10. Recommendation

Start with **8g-0** (the outbound-buffer seam + readiness wiring) — it's the prerequisite, it's
testable on both axes, and it already removes spin-write HOL for handler-produced streams. Then
**8g-1** (completion overlapped flush) falls out of it. Gate **8g-2** (async lifecycle) on a
real async-producer-over-completion need. After 8g, the completion backends reach behavioral
parity with readiness for every response mode.
