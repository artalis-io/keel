# Bounded HTTP response streaming (P0-4b): design assessment

Status: **P0-4b-a merged (#516); P0-4b-b implemented** on top of `main` `01a2ee9` (#514, #515 and
#516 merged). P0-4b-c (compression) is implemented as section 8 describes. The design below is revision 2 as approved; the
implementation departs from it in four places, each for a measured or source-level reason:

1. **No `kl_drain_reserve_writev`.** Atomicity comes from deciding admission once, against the one
   pending total, before any byte moves: the outbound buffer's own cap is at least the capacity, so
   no layer below can refuse part of an admitted chunk. A reservation would only have made an
   *allocation* failure leave nothing behind, and a transport failure part way cannot be undone
   anyway; both are `ERROR`, which ends the connection. No generic primitive was needed.
2. **No `kl_http_response_stream_set_capacity`.** `KlHttpServerConfig.stream_capacity` (per
   response) covers the gateway; a per-response override would add the "capacity changed while
   bytes are pending" semantics with no consumer yet. It can be appended later.
3. **The legacy writer is not admitted through the unified accounting** (section 5 said it would
   be). Measured: counting the completion output queue against `begin_stream`'s writer failed seven
   existing tests. The documented pacing pattern (`kl_drain_buffered` against a high-water mark)
   never saw the drain fill before the shared 1 MiB was spent, and a handler streaming 16 MiB inline
   on a completion loop failed at 1 MiB, since no send can complete while the handler runs. The
   legacy writer keeps its existing (finite, since #516) bounds; `stream_capacity` also sets its
   outbound-buffer cap. The bounded API counts everything.
4. **The writable edge fires only while the connection is suspended**, the state in which a producer
   owns the stream (`docs/contracts/streaming.md`): after the handler returns or the op completes,
   the response is the driver's and no producer may write.

The callback has a named type, `KlHttpStreamWritableFn`, as KlStream's does (an inline
function-pointer parameter also hid the function from the public-API inventory gate).
The memory bound of section 4 is stated exactly in `docs/contracts/streaming.md`, "Bounded
streaming", counting allocated capacity: at most C + 5 on readiness and
(C + 5) + 4(C + 5 + O + F) + min(256 KiB, C + O) on a completion loop (about 5C + 64 KiB +
min(256 KiB, C)), plus headers and fixed TLS buffers. Section 4's "2x" for the queue was too low:
the retained previous buffer can be as large as the current one. The relay test enforces the
formula with a counting allocator on the relay server.

Revision 1 (a delegated draft) is superseded: it proposed HTTP-local notification rules that
duplicate an existing generic contract (section 2).

## 1. Problem, in one paragraph

A streamed HTTP/1.1 response cannot tell its producer "full, wait" without killing the stream, does
not admit a framed chunk atomically, and on completion backends lets a producer exceed its bound. A
reverse proxy that pauses its upstream client (3.4.0) when the downstream response is full therefore
has no correct signal to pause on and none to resume from. Measured and cited in
[gateway_readiness.md](gateway_readiness.md) sections C and F; the defects below are re-verified
against the source.

## 2. Layering: what already exists, and who owns what

| Layer | Owns | Already provides for this |
|---|---|---|
| Event | readiness / completion dispatch | write interest (readiness), WRITE completions |
| Socket provider | byte transmission, partial writes, transport errors | nothing more is needed |
| **Generic stream** (`KlStream`, `KlDrain`, `src/drain_reserve.h`) | bounded byte queues, all-or-none admission, flush, writable edge | **the whole admission and notification contract** (below) |
| TLS | encryption, ciphertext staging | engine ring (completion), `WANT_WRITE` (readiness) |
| **HTTP** | response framing, response capacity, logical write admission, response lifecycle | today: a fatal-at-cap drain; the completion connection queue `comp_tlsq` |
| Application | production rate, SSE formatting, upstream pause/resume, policy | uses the HTTP API |

**`KlStream`'s write facet already defines the contract P0-4b needs** (`include/keel/stream.h:60-118`,
`docs/contracts/stream.md` "Write"):

* `kl_stream_write` is all-or-none: `ACCEPTED`, `WOULD_BLOCK` (nothing taken), `TOO_LARGE`
  (permanent), `CLOSED`, `ERROR`; bytes copied before return; provider short writes stay internal.
* `kl_stream_write_pending` counts queued plus in-flight bytes.
* `kl_stream_on_writable` is the writable-again edge: armed only by `WOULD_BLOCK`, disarmed by
  `ACCEPTED`, fires at most once per arming when total pending falls below capacity, only from a
  progress point (`kl_stream_flush`, `kl_stream_on_write_complete`), never from inside a write, never
  once a close has begun; **a retry that returns `WOULD_BLOCK` re-arms it**, so a large admissible
  write is retried at each progress point until it fits and is never stranded.
* It is built on `src/drain_reserve.h` (`kl_drain_reserve_write` / `_buffer`: atomic admission over a
  `KlDrain`), and used today only by `pipe_stream.c`.

**HTTP does not use it.** A `KlHttpConn` embeds a `KlStream` but uses only its base fields (fd, read
buffer, ctx). Its output goes through the response's `KlDrain` (`http_response.c`) and, on
completion backends, `comp_tlsq` (`completion_http_server.c:244-351`), which also carries TLS
ciphertext, WebSocket and HTTP/2 output.

### Decision on reuse (pushing back on part of the review)

1. **Contract: reuse KlStream's exactly.** The public HTTP API mirrors `KlStreamWriteStatus` and
   `kl_stream_on_writable` semantics one for one. Same statuses, same arming rule, same re-arm on a
   repeated `WOULD_BLOCK`. This answers the "768 KiB write and a 50% low-water mark" concern without
   a threshold: the edge fires when pending drops below capacity, the retry sees `WOULD_BLOCK` if it
   still does not fit and re-arms, and the next progress point fires again. **I recommend against a
   50% low-water mark in the contract.** It is a second rule with weaker guarantees (a write larger
   than half the capacity needs extra handling) and it duplicates what KlStream already solves. If
   measurement later shows retry chatter (a relay resuming per freed fragment), hysteresis can be
   added as an *optional* capacity-relative resume hint without changing the guarantee.
2. **Mechanism: do not move HTTP output onto `KlStream`'s queue in this work.** HTTP already owns a
   single output pipeline per connection (drain, then `comp_tlsq`), shared with TLS, WebSocket and
   HTTP/2. Replacing it with `KlStream`'s preallocated queue is the broad refactor the review rules
   out, and `KlStream`'s write facet preallocates its full capacity per stream (`kl_drain_prealloc`),
   which for HTTP would reserve 1 MiB per streamed response up front instead of growing on demand.
   P0-4b adds **admission and notification on top of the existing pipeline** (no new queue), reusing
   the `drain_reserve.h` substrate where it fits.
3. **One genuinely missing generic primitive:** atomic admission of a *vector*. A framed chunk is
   three pieces (hex header, payload, CRLF); `drain_writev_coalesced` (`http_response.c:723-749`)
   writes them as separate drain writes, so one piece can be taken and a later one refused. The
   generic layer gets an internal `kl_drain_reserve_writev` (and its buffer-only twin) beside
   `kl_drain_reserve_write`: check the total against free capacity, then commit every piece, or
   nothing. HTTP framing stays in HTTP. No HTTP semantics enter `KlDrain`, `KlStream`, providers or
   backends.

## 3. Current invariants (verified)

* **Readiness:** a streamed response's drain is capped at `KL_STREAM_DRAIN_MAX` = 1 MiB
  (`http_response.c:24,829-830`); reaching it sets a sticky error (`drain.c:46-48`) and every later
  write and `end_stream` returns -1 (`http_response.c:753,869`). Effective bound: kernel send buffer
  plus 1 MiB, fatal.
* **Completion:** the connection queue accepts up to `KL_COMP_STREAM_QUEUE_MAX` = 1 MiB of a
  producer's unposted output (`completion_http_server.c:408-416,466-470`); past that, writes are
  refused as would-block to the response drain, which then buffers up to its own 1 MiB. **Two
  independent 1 MiB limits**, which is why a producer sees its first fatal -1 at about 2 MiB, not
  1 MiB.
* **Completion bypass (defect):** inside the handler (`PROCESSING`) and while the driver flushes
  (`comp_stream_flushing`), `kl_http_comp_stream_write` takes everything (`:466`). `comp_tls_on_write`
  sets `comp_stream_flushing = 1` around `kl_drain_flush` (`:1229-1232`), and `kl_drain_flush` fires
  the legacy `on_drain` from inside that flush (`drain.c:158-160`), so a producer that refills from
  `on_drain` is never bounded (measured: 8 MiB accepted, RSS +15 MiB on pollcomp; on kqueue the same
  producer fails at 1.14 MiB).
* **`end_stream` refusal (defect):** the terminating chunk goes through the capped drain, so a stream
  filled to its cap cannot be ended (measured).
* **Retained memory exceeds pending bytes:** the drain grows by doubling (up to 2x its content), and
  `comp_tlsq` doubles too and keeps its old buffer while that buffer's bytes are in flight
  (`completion_http_server.c:266-298`), so for a moment up to about 3x its content.

## 4. Quantities and the capacity invariant

Three quantities are kept distinct:

| Quantity | Definition | Where it is read |
|---|---|---|
| **Pending output** | bytes accepted for this response and not yet handed to the kernel: drain `buf_len`, plus on completion `comp_tlsq` unposted + in-flight bytes (ciphertext when TLS) | `kl_http_response_stream_pending()` |
| **Capacity** | the per-response bound on pending output (default 1 MiB, server-wide default, per-response override) | `kl_http_response_stream_capacity()` |
| **Retained memory** | what the pipeline actually holds allocated: drain `buf_cap`, `comp_tlsq_cap` (+ `comp_tlsq_old_cap` while in flight), the TLS engine's fixed buffers, the codec's state | measured in tests, not an API |

**Invariant:** pending output never exceeds capacity, except for the bytes of one terminal chunk
(section 6), on both axes, inside or outside the handler, from any callback. Admission is checked on
framed bytes (chunk header + payload + CRLF), with overflow-checked arithmetic.

**Memory bound (stated honestly):** retained memory is bounded by a constant multiple of capacity
plus fixed per-connection state, independent of response duration and size: drain up to 2x
capacity (doubling growth), queue up to 2x capacity plus the old buffer while in flight, TLS engine
fixed, codec fixed. "1 MiB capacity" is **not** a 1 MiB memory bound, and the documentation will say
so. The acceptance tests measure pending bytes and RSS separately. Tightening growth (allocate to
`min(2x, capacity)`) is a small optional improvement measured in the same PR.

TLS: admission is checked on plaintext; what is counted is what is held (plaintext in the drain,
ciphertext in the queue; ciphertext overhead is bounded per record). Readiness TLS holds at most one
record in the engine (`WANT_WRITE` drains to the drain), which is not counted (fixed size).

## 5. Public API (additive; appended fields only)

```c
typedef enum {
    KL_HTTP_STREAM_ACCEPTED,      /* the whole logical write was taken (copied); Keel owns delivery */
    KL_HTTP_STREAM_WOULD_BLOCK,   /* nothing taken; retry after the writable edge */
    KL_HTTP_STREAM_TOO_LARGE,     /* its framed size exceeds capacity: permanent, the caller must split */
    KL_HTTP_STREAM_CLOSED,        /* end requested, response ended, or connection gone */
    KL_HTTP_STREAM_ERROR          /* allocation or transport failure (sticky) */
} KlHttpStreamStatus;

KlHttpStreamStatus kl_http_response_stream_try_write (KlHttpResponse *res, const void *data, size_t len);
KlHttpStreamStatus kl_http_response_stream_try_writev(KlHttpResponse *res, const KlIoVec *iov, int iovcnt);
size_t kl_http_response_stream_pending (const KlHttpResponse *res);
size_t kl_http_response_stream_capacity(const KlHttpResponse *res);
int    kl_http_response_stream_set_capacity(KlHttpResponse *res, size_t capacity);   /* before or after begin */
int    kl_http_response_stream_on_writable(KlHttpResponse *res, void (*fn)(void *ctx), void *ctx);
/* KlHttpServerConfig.stream_capacity: the default per-response capacity (0 = 1 MiB). Not an
 * aggregate server budget. */
```

Semantics, defined:

* **Ownership:** accepted bytes are copied before return; rejected input is untouched and still the
  caller's. Nothing is ever borrowed.
* **Atomicity:** `try_writev` admits the whole vector as one chunk or nothing; no header, payload or
  CRLF of a rejected write enters any queue. Transport partial writes stay internal.
* **Zero length:** `ACCEPTED`, no bytes, **never** a chunk (a zero-size chunk would end the body).
* **Overflow:** a total that overflows `size_t` or exceeds `SIZE_MAX/2` is `TOO_LARGE`.
* **Allocation failure:** `ERROR`, sticky, nothing taken.
* **After end requested / after the response ended / connection gone:** `CLOSED`.
* **Capacity change while pending:** takes effect for the next admission and the next edge
  evaluation; lowering it below pending never discards accepted bytes (the next write is
  `WOULD_BLOCK` until pending falls below the new capacity). An armed edge stays armed.
* **Callback location: the response** (agrees with the review): writability is a property of
  response production, mirroring `kl_stream_on_writable` on the stream object. `KlAsyncOp` keeps the
  operation lifecycle; nothing in it changes.
* **Legacy writer** (`begin_stream`'s `KlHttpResponseWriteFn`) keeps its fatal-at-cap semantics, but
  is admitted through the same accounting (so it can no longer bypass the bound, section 7).

## 6. Writable edge and end of stream

* **Edge rule (KlStream's):** armed by `WOULD_BLOCK`, disarmed by `ACCEPTED`, fires at most once per
  arming when pending < capacity, at a progress point: readiness, the end of #514's suspended-stream
  flush (`async_stream_progress`) or the connection's writable flush; completion, after
  `comp_tlsq_retire` in `comp_tls_on_write` with `comp_stream_flushing` already cleared. Deferred:
  never from inside a write. A retry returning `WOULD_BLOCK` re-arms. It never fires after the op's
  `on_cancel`, after the response ended, or after the connection is released (cleared in the
  release path before any user callback). Terminal failure does not fire it: `on_cancel` is the
  terminal signal (a deliberate difference from KlStream, whose `on_close` plays that role).
* **No lost wakeup:** `WOULD_BLOCK` implies pending > 0, which implies a progress point will come
  (readiness: #514's write watch on a suspended connection, the writable flush otherwise;
  completion: the in-flight send's completion).
* **End of stream:** `end_stream` records the intent, refuses further writes (`CLOSED`), and appends
  the 5-byte terminal chunk behind all accepted output **outside** the capacity check (a fixed,
  reserved allowance, not an exemption for arbitrary output). A second `end_stream` is a no-op
  returning 0. Cancellation discards pending output as today (`kl_http_response_reset`,
  `conn_comp_tlsq_reset`). Transport and allocation failures stay observable (`ERROR`, `on_cancel`).
  With compression, `end_stream` finalizes the codec first; its tail output is admitted the same way
  (bounded by the codec, section 8).

## 7. Fixing the legacy completion bypass

`comp_stream_flushing` must describe the driver's own data movement, not permit application writes.
Fix: in `comp_tls_on_write`, move the drain's bytes with the flag set but **without** user callbacks,
clear the flag, then fire the legacy `on_drain` if the drain emptied. The lifecycle checks: a
callback may write (now bounded), end the stream, complete or cancel the op (the connection may be
released: nothing touches it after the callback without re-validation), and TLS ring absorption runs
before the callback. CHANGELOG: on completion backends a producer that refills from `on_drain`
becomes bounded like everywhere else and gets -1 at the cap instead of unbounded growth.

## 8. Compression

As implemented (P0-4b-c). Admitting a write needs the most output it can cause, and a codec that
buffers (deflate's no-flush mode) owes output for earlier input at any later write: a single
100-byte write was measured to release 50 KB from miniz. Two designs were compared:

* **Reserve for retained output, keep normal buffering.** Sound for miniz only from its internals
  (one block's output fits its 85,196-byte output buffer), so a standing per-stream reservation and a
  state-reading codec op; each codec retains differently. It also leaves the latency defect: with no
  flush, an SSE stream of 20,000 token events (3.5 MB) delivered nothing until it ended.
* **Flush every accepted write (chosen).** The codec then retains nothing between writes, so each
  write's output is bounded by its own length alone and decodes on arrival. Measured with miniz 3.0.2
  at level 6, wire bytes including chunk framing, flushed against unflushed: ~180-byte SSE events
  15.3% of input against 1.4% (about 27 bytes per event; still 6.7 times smaller than uncompressed);
  1 to 4 KiB text 20.4% against 18.8%; 64 KiB blocks 18.78% against 18.72%; random data +0.07 to
  +1.6 points. CPU is unchanged for bulk and about 1.4 us more per tiny event. A producer that
  prefers ratio to latency can batch small writes.

The codec contract is one optional appended op, `size_t (*flush_bound)(KlCompress *, size_t len,
int final)`: the most one `feed` with `KL_COMPRESS_FLUSH_SYNC` (or the finish, when `final`) can
emit given the session's state, including a header not yet sent and output an earlier unflushed
`feed` still owes; it never modifies the session and answers `SIZE_MAX` on overflow. Supplying it
declares support for the sync flush. It knows nothing of HTTP. `kl_http_compress_stream_try_write`
asks the response, through the internal `kl_http_response_stream_admit` (the same checks
`try_writev` makes, nothing written), whether a chunk of that bound fits; only then does it feed the
codec, into a per-stream staging buffer (grown to the largest admitted bound, freed at the end), and
submit the output as one chunk, which is then necessarily admitted. Output past the bound is a codec
defect and fails the stream before any of it leaves. `kl_http_compress_stream_try_end` admits the
finish the same way and then appends the terminal chunk outside the bound. A codec without the op is
refused, never fed; nothing else about compression changes. No conservative generic fallback: it
could not be sound for a codec whose retention Keel cannot see.

After review: a failure once a write is admitted (staging allocation, codec, output past its bound)
fails the response's stream too, since the codec may have advanced; a HEAD response takes writes
without running the codec (nothing is sent, and the staging buffer must stay under the capacity);
`kl_http_compress_stream_release` abandons a stream without writing (`on_cancel`, or a finish that
can never fit). The miniz sync flush is complete only after a call that compressed with nothing
left in the lookahead: a call that only copies out an earlier block's output does not flush, and
stopping there lost up to 258 bytes of a write between about 31 and 33 KB (found by review; a test
now decodes every size across that window).

## 9. Revised PR decomposition

| PR | Content | Depends on | Public API |
|---|---|---|---|
| **P0-4b-a** | completion `on_drain` bypass fix (section 7); `end_stream` never refused by the cap (section 6) | nothing (independent of #514 and the Windows backend) | none |
| **P0-4b-b** | `kl_drain_reserve_writev` (generic, internal); HTTP `try_write`/`try_writev`/`pending`/`capacity`/`set_capacity`/`on_writable`, `stream_capacity` config; unified accounting on both axes; legacy writer admitted through it; the relay end-to-end test | #514 (readiness progress) rebased on the Windows backend | the full contract at once (admission and notification together: separating them would ship a non-fatal write with no usable wakeup) |
| **P0-4b-c** | compression-aware admission | P0-4b-b | `kl_http_compress_stream_try_write`, `KlCompress.bound` |

## 10. Tests

* P0-4b-a: on pollcomp/io_uring/IOCP, a producer refilling from `on_drain` is bounded (pending never
  exceeds the cap; RSS flat); a stream filled to the cap still ends (`0\r\n\r\n` arrives after all
  data). Mutation: removing either fix fails its test.
* P0-4b-b: unit tests for every status and edge rule (arming, disarm, re-arm, large write retried to
  completion, zero length, overflow, capacity change, end then write, cancel with armed edge);
  atomicity (a rejected chunk leaves no byte on the wire); both axes.
* **Decisive end-to-end relay** (generic, no Hull): upstream server -> Keel client -> relay -> Keel
  server -> slow client. The relay keeps a rejected fragment (it already accepted it from the client
  callback before returning `PAUSE`), pauses upstream, retries on the edge, resumes upstream. Asserts:
  byte-exact, in-order output; pending never above capacity; RSS bounded; the upstream producer
  observably stalls (its send blocks) while the client is paused; abandonment while paused, cancel
  races, graceful drain. On epoll, kqueue, poll, io_uring, pollcomp, Windows `select()` and IOCP, with
  real TLS (mbedTLS).

## 11. Disagreements with the review, summarized

1. **No 50% low-water mark in the contract:** KlStream's re-arming edge already guarantees progress
   for any admissible write; a threshold adds a weaker second rule (section 2.1).
2. **No migration of HTTP output onto KlStream's queue:** reuse KlStream's contract and the
   `drain_reserve.h` substrate, add the one missing generic primitive (vectored reservation), keep
   HTTP's existing single pipeline (section 2.2). Migration is a separate, larger refactor with no
   current requirement.
3. **Admission and notification ship together (P0-4b-b)**, as the review anticipated.
4. Agreed: callback on the response; server-wide default per-response capacity plus a per-response
   override; 1 MiB default; P0-4b-a independent and first.
