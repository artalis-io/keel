# Keel Stream Transport Contract (DRAFT — not yet normative)

**Date:** 2026-08-08
**Status: DRAFT.** A prior version was labelled "normative"; that was premature — it promised
capabilities Keel does not have yet (per-op cancel, half-close/abort, prefix-accept writes,
TLS-wrapped-stream semantics) as though existing primitives already guaranteed them. This revision
(a) demotes it to DRAFT, (b) tiers the semantics into what exists vs. must-be-built vs. optional,
and (c) records the resolved design decisions for the must-be-built parts. It becomes normative
only after the Tier-2 machinery lands (see `docs/generic_transport_audit.md` §8).

### Capability tiers (which parts of this doc are real *today* vs. new work)

> **Tiers describe capability levels, NOT implementation order.** The build order is the
> **Phase A/B/C/Optional** plan in `docs/generic_transport_audit.md` §8. The mapping is exact:
> **Tier 1 = Phase B** (the baseline public surface), **Tier 2 = Phase C** (advanced semantics),
> **Tier 3 = Optional**. Crucially the Tier-1 capabilities are **not** free extraction — they need new
> machinery (write reservation, the listener carve, deferred close + detachment) built first.
> **Phase A is a semantics-preserving *structural* extraction** (internal, no public API) — **not** a
> pure/no-logic retype: it moves the receive-buffer boundary above the provider and retypes the
> listener accept target (see the audit). So Tier 1 is the shippable target, **not** "safe to
> implement immediately."

- **Tier 1 = Phase B (baseline public surface):** push read-delivery callback; `kl_stream_write`/
  `writev` with an **atomic-accept** result (over a **preallocated** bounded queue — no hot-path
  alloc; oversized → `TOO_LARGE`); low-water writable notification; stream-level strict
  `pause`/`resume`; **stream-level cancellation**; graceful `kl_stream_close` + detachment `on_close`;
  a cancellable `KlConnectOp` (client) and a **push `on_accept`** listener; **plaintext only**. On read
  termination Tier 1 delivers a **conservative close** (`KL_STREAM_CLOSED_UNKNOWN` — "the stream ended;
  cause unknown"), **not** a claim of orderly EOF: today's `KlCompletionEvent` carries only `bytes`+`ok`
  and collapses orderly EOF and error into `ok==0`, so orderly-EOF-vs-error is **not** representable on
  completion backends yet. There is **no `recv≤0 → EOF` rule** — that is not a valid generic-stream
  rule. *(The write-result type, the bounded-queue backpressure API, and deferred-destruction/
  detachment ownership are all part of THIS tier — they are new machinery, but they are Tier-1/Phase-B
  work, not Tier 2. Tier 2 does not re-introduce them.)*
- **Tier 2 = Phase C (advanced semantics — designed here, must be built before this is normative):**
  the **precise close taxonomy** (model-neutral orderly-EOF vs reset vs error — see the table below);
  **per-read / per-write operation identity** (the operation-lifecycle records + §6.3 precedence
  enabling per-op cancel/terminals, beyond Tier 1's coarse stream-level cancel); the
  **TLS-wrapped-stream** state machine. Tier 2 refines Tier-1 capabilities; it does not restate them.
  - **The close taxonomy (why it is Phase C):** `KlCompletionEvent` carries just `bytes`+`ok` today,
    so a completion backend cannot distinguish orderly FIN from generic failure at all. Phase C adds a
    **`KlIoStatus status`** field to `KlCompletionEvent`; the driver then classifies:
    | condition | outcome |
    |---|---|
    | `bytes > 0` | data delivered |
    | `bytes == 0` && `status == KL_IO_CLOSED` | orderly **EOF** |
    | `status == KL_IO_RESET` | **reset** |
    | `status == KL_IO_WOULD_BLOCK`/`KL_IO_INTERRUPTED` | **not terminal** (retry) |
    | any other `status` | **fatal error** |
    Until that field lands, Tier 1's read termination is only `KL_STREAM_CLOSED_UNKNOWN`.
- **Tier 3 (OPTIONAL — NOT in the initial contract):** `shutdown_write` half-close, `abort`/RST
  (both are new `KlSocketOps` provider work — no such op exists today), a tagged non-socket
  `KlEndpoint` union, file-transfer (`sendfile`) extensions.

### Resolved decisions (this DRAFT commits to these; sections below are read through them)

1. **Write acceptance = ATOMIC.** `kl_stream_write` accepts *all* bytes or *none*; it returns a
   `KlStreamWriteResult { status; accepted; io_status }` where `accepted ∈ {0, len}` for the atomic
   policy. Maps naturally onto bounded buffering + today's `KlDrain` (0/-1). Prefix-acceptance is a
   possible later change (requires extending `KlDrain`); not the initial policy.
2. **Backpressure signal = one writable callback at low-water.** Today `KlDrain.on_drain` fires
   only empty→non-empty→empty; the API defines separate high/low-water marks and one `on_writable`
   at low-water — a small `KlDrain` extension (**Tier 1 / Phase B**), not a new subsystem.
3. **Pause = STRICT.** No read callback fires after `pause` returns; a completion recv already
   outstanding at pause time completes into the single receive buffer and its bytes are delivered
   only after `resume`. Bounded to one buffer. (Resolves the earlier §5 contradiction.)
4. **Cancellation = stream-level for the first cut.** No per-*write* cancel (that needs the Tier-2
   op-lifecycle). `kl_stream_cancel(stream)` cancels the stream's in-flight I/O; `connect` is a
   separately cancellable op *handle* (it precedes a live stream). Accept is push-only — cancel it by
   closing the listener (§8.2), not via a per-accept handle.
5. **Destruction = caller-owned + deferred close + `on_close` on confirmed DETACHMENT (normative,
   §7).** `close` *begins* teardown; the single `on_close` fires when the backend confirms
   **detachment** — no future event can reference the caller's stream storage — which is NOT the
   same as physical provider-token retirement (EFI may quarantine a token that never physically
   retires before ExitBootServices). Only after `on_close` may the caller free/reuse the (embedded
   or pooled) storage. No callback after `on_close`. The caller never waits on physical retirement.
6. **Endpoints = `KlSockAddr` directly.** No `TCP`/`Unix`/`lwIP`/`EFI` endpoint "kinds" — provider
   selection stays in `KlEventCtx`/config. A tagged union appears only for a real non-socket
   address (Tier 3).
7. **TLS = plaintext first.** The initial `KlStream` is plaintext; a TLS-wrapped stream is a later
   phase with the state machine in §6bis. (`KlTls` already wraps the socket seam, but the
   generic-stream TLS semantics below are not yet implemented.)

The remainder describes the intended semantics both event models and every provider must present
identically once each tier lands. Read every section through the decisions above.

A protocol author programs to *this contract only*. They must never need to know the event model
(epoll/kqueue/WSAPoll/io_uring/IOCP), the provider (native vs lwIP vs EFI), or that a native
descriptor exists. Below the contract lives mechanism; above it lives protocol.

Terminology: **caller** = the protocol layer using a stream. **backend** = the readiness or
completion implementation. **provider** = the socket/transport implementation (TCP/lwIP/…).

---

## 0. Objects and invariants

- `KlStream` — an ordered, connected, reliable byte stream (TCP / Unix / lwIP TCP; TLS-wrapped and
  future pipe/vsock are later tiers). Not a datagram (see `KlUdp`/`KlDatagram`). It carries its own
  **internal read + write operation state** (a recv slot and a send slot with generation + terminal
  flags); it does **not** reuse `KlAsyncOp` (which is HTTP-handler suspension, per the audit §2.5).
- `KlListener` — accepts connected `KlStream`s.
- `KlEndpoint` — **initially just `KlSockAddr`** (a conceptual alias; already neutral IPv4/IPv6/Unix).
  It is address identity only and carries **no provider/transport kind** — provider selection stays
  in `KlEventCtx`/config (§9). A tagged non-socket union is a later tier.
- Accept is **push-based**: the listener delivers each connected stream via an **`on_accept(stream)`**
  callback (matching the server model). There is **no** public per-accept handle — mixing a pull
  `KlAcceptOp` with push/pool acceptance was ambiguous, so the listener is push-only (§8.2).
- **`KlConnectOp`** — the one pre-stream, separately cancellable, caller-owned handle: the client
  `connect` (inherently a single pull operation, §8.1). There is **no** generic public `KlOp` type;
  it is not introduced until a real generic operation type is actually needed.

**Global invariants (all sections below refine these):**
1. Every async operation reaches **exactly one** terminal outcome: completed | failed | cancelled
   | timed-out | peer-closed. Late/duplicate events for a retired op are dropped by the backend
   (today: generation guards + op retirement).
2. A submitted completion op **owns or pins every referenced buffer** until its terminal event.
3. No callback fires after its owning `KlStream`/`KlListener` has reached terminal destruction.
4. Output buffering is **bounded**; read-side delivery is **pausable** and never accumulates
   unboundedly.
5. Cancellation, timeout, peer-close and explicit close have a **deterministic precedence**
   (§6.3).
6. The stream is single-threaded / one-event-loop-affine (Keel's model): all callbacks for a
   stream run on its loop thread; no cross-thread op submission on a stream.

---

## 1. Read semantics

**Model:** Keel is push-based on the delivery side. The caller registers a read-interest and
receives bytes via a callback; it does not block-poll. (Matches today's `on_data`-style body
readers and the completion `comp_on_read` → dispatch.)

- **Buffer ownership: Keel supplies the buffer.** The callback receives `(const uint8_t *data,
  size_t len)` pointing into a Keel-owned receive buffer. Rationale: it unifies readiness (recv
  into the stream's `read_buf`) and completion (recv into a posted buffer) without the caller
  pre-posting buffers, and it's what the HTTP parser + body readers already assume.
- **Validity duration: the bytes are valid only for the duration of the callback.** A caller that
  needs them longer must copy. (Nuance — not a stream promise: Keel's HTTP layer deliberately
  *retains* some pointers into the read/header buffer with documented lifetime limits, e.g. header
  values live until the buffer is reset; that is a per-caller optimization built on knowledge of
  the buffer, not a guarantee the stream makes. The generic guarantee is only "valid for the
  callback.") After the callback returns, Keel may reuse/overwrite the buffer.
- **Reentrancy: a read callback MAY synchronously request another read / pause / write / close.**
  These are recorded and take effect without deep recursion (the backend re-arms or posts after
  the callback returns; it must not recurse per delivered chunk). A caller must tolerate its next
  read callback occurring only after the current one returns.
- **Short/fragmented reads:** a read callback delivers *whatever arrived* — 1 byte or many. The
  caller must maintain its own framing state across callbacks (the Phase-8 echo test exercises
  1-byte fragmentation). A readiness event ≠ "a whole message"; a completion ≠ "the requested
  count."
- **Read termination:**
  - **Tier 1 (conservative):** when the read side ends, the stream delivers a single
    `KL_STREAM_CLOSED_UNKNOWN` close — "the stream ended; cause not distinguishable." There is **no
    `recv≤0 → EOF` rule**: `recv==0` is orderly EOF but `recv<0` may be WOULD_BLOCK / INTERRUPTED
    (not terminal) / reset / fatal, and the completion event cannot tell these apart today. After
    the close, no further read callbacks fire.
  - **Tier 2/Phase C (precise):** once `KlCompletionEvent.status` exists (header table), the close
    is classified as orderly **EOF** (`bytes==0` && `KL_IO_CLOSED`), **reset** (`KL_IO_RESET`), or
    **fatal** — matching the readiness seam. EOF = "peer finished sending, cleanly"; reset =
  "connection aborted, in-flight data may be lost." The caller can distinguish them and MUST NOT
  treat a reset as a clean end-of-message.
- **Backpressure interaction:** while paused (§4) no read callbacks fire and the backend does not
  accumulate (readiness: interest dropped; completion: no recv posted).

---

## 2. Write semantics (ATOMIC acceptance — decision #1)

- **Ownership chain (decision #3): the caller buffer is borrowed for the call only; anything that
  outlives the call lives in stream-owned stable storage — but a fully-inline write copies nothing.**
  ```
  caller buffer --(reserve remainder cap; try direct send)-->
     • all consumed inline  -> nothing retained, no snapshot (zero-copy readiness fast path)
     • prefix consumed / completion submit -> remainder copied into the RESERVED stream-owned queue
                                              -> provider op copies or pins the STREAM storage
  ```
  - **After `kl_stream_write*` returns, the caller may reuse/free its buffer** — unconditionally.
  - Copying is **not** mandatory: a readiness send that consumes the whole buffer synchronously
    keeps Keel's zero-copy fast path (no snapshot). Only a remainder (partial send) or a completion
    submission is copied into the pre-reserved stream-owned queue.
  - A provider (e.g. lwIP-raw) may **pin the *stream-owned* storage** for the op lifetime; it may
    **never** pin the original caller buffer past the call. (Completion backends that copy — e.g.
    the TLS ciphertext path — copy from the stream-owned storage.) Pin/copy is below the contract.
  - Reservation-before-send is what makes this atomic (§ reservation requirement below): the
    remainder's queue space is secured *before* the direct send, so a prefix send can never strand
    bytes it cannot buffer.
  - A later explicit `write_owned` API can transfer caller ownership for true zero-copy; not default.
- **Atomic acceptance.** `kl_stream_write`/`writev` accept **all** the bytes or **none** — there is
  no caller-visible partial write. Return is a result struct (not a bare enum):
  ```c
  typedef struct { KlStreamWriteStatus status; size_t accepted; KlIoStatus io_status; } KlStreamWriteResult;
  ```
  | `status` | `accepted` | meaning |
  |---|---|---|
  | `KL_STREAM_ACCEPTED` | `== len` | all bytes taken (sent inline and/or queued); Keel owns delivery |
  | `KL_STREAM_WOULD_BLOCK` | `0` | `len ≤ total_capacity` but insufficient space **right now**; **nothing** taken — retry after the writable signal |
  | `KL_STREAM_TOO_LARGE` | `0` | `len > total_capacity`; **permanent** — no amount of draining can ever accept it. Caller must chunk (§below). A programming error, not backpressure |
  | `KL_STREAM_CLOSED` | `0` | write side gone (Tier-3 half-closed, or peer-closed) |
  | `KL_STREAM_ERROR` | `0` | fatal; `io_status` carries the category |
  Provider-level short writes (a `send` that took a prefix) are **internal**: the stream keeps the
  remainder in its own storage and reports `ACCEPTED`. They are never observable as partial API
  acceptance.
- **`writev` length computation + input validation are NORMATIVE and must precede the capacity check
  (Finding 5).** The total-length sum feeds the atomic reservation, so it must be computed safely or the
  all-or-none check itself could overflow. Before reserving or comparing to `total_capacity`,
  `kl_stream_writev` must, in order:
  - reject `iovcnt < 0` or `iovcnt` beyond the supported/provider limit (e.g. `IOV_MAX`,
    `MSG_MAXIOVLEN`, or the backend's `WSABUF`/`iovec` cap) → `KL_STREAM_ERROR` / `KL_IO_INVALID`;
  - reject `iov == NULL` when `iovcnt > 0`; and reject any entry with `len > 0 && base == NULL`;
  - sum lengths with an **overflow guard** — for each entry, `if (iov[i].len > SIZE_MAX - total)` →
    error (never wrap); zero-length entries are skipped (a `NULL` base is allowed only when `len == 0`);
  - after summing, apply the `TOO_LARGE` / `WOULD_BLOCK` rules on the validated `total`;
  - **`total == 0`** (zero entries, or all-empty vector) is a **no-op success**: `ACCEPTED` with
    `accepted == 0`, no provider op posted;
  - when a backend takes a **narrower length type** (`int`/`DWORD`/`ULONG` per iovec, or a narrower
    total), the stream validates each segment and the total against that type's max **before** handing
    it down — a value that doesn't fit is an error at the seam, never a silent truncation.
  `kl_stream_write(buf, len)` is the `iovcnt == 1` case of these rules.
- **Reservation requirement — a capacity CHECK, not a hot-path allocation (decision #1 + Finding 3).**
  All-or-none is only safe if the stream **reserves queue capacity for the remainder before exposing
  any byte to the provider** (else a prefix `send` could strand bytes it cannot buffer). But
  reservation must **not `realloc` during `kl_stream_write`** — that would violate Keel's
  allocation-free event-loop / response-send rule and break freestanding. So the policy is:
  - each stream has a **finite configured write capacity**, backed by storage **preallocated at
    stream/pool init** (or drawn from a preallocated bounded arena);
  - `kl_stream_write` performs **no allocation** — reservation is a capacity *check* against that
    preallocated space;
  - the check distinguishes two cases so an oversized write is never mistaken for transient
    backpressure (Finding 2): **`len > total_capacity` → `KL_STREAM_TOO_LARGE`** (permanent — an empty
    queue still can't hold it, so no low-water transition or writable callback could ever unblock it;
    the caller must chunk); **`len ≤ total_capacity` but insufficient free space now → `WOULD_BLOCK`**
    (retry after the writable signal). It never grows the buffer inline.
  Optional dynamic growth may exist as a **hosted-only** mode, but it is NOT the default/conformance
  path and is **unavailable in freestanding builds**. This is Phase-B machinery (a bounded,
  preallocating `KlDrain` extension), not free from today's `KlDrain`.
- **Chunking large logical writes is the caller's job.** A message larger than `total_capacity` must
  be written in `≤ total_capacity` pieces, each pumped after the prior drains (writable signal). The
  contract makes no promise that an arbitrarily large single `kl_stream_write` eventually succeeds —
  it returns `TOO_LARGE` immediately. (Streaming bodies already work this way: the response/body layer
  feeds the stream in bounded chunks.)
- **Ordering:** bytes are delivered to the peer in submission order. Queued bytes flush before any
  later write is sent — a write while the buffer is non-empty appends (never reorders).
- **Concurrent writes:** not supported within one stream from multiple call sites racing; the
  single-loop-affinity invariant means writes are serialized by the caller on the loop thread.
  Overlapped completion writes are bounded to **at most one in flight** (today: `comp_stream_pump`
  posts one send, pumps the next on `KL_COMP_WRITE`) so ordering + buffer bounds hold.
- **Inline completion:** a write MAY complete synchronously (readiness fast path sends it all).
  The caller must not assume a later writable callback will fire if the write was fully ACCEPTED
  inline.
- **[Tier 3] After write half-close (`shutdown_write`):** further `kl_stream_write*` returns
  `CLOSED`; reads continue until peer EOF. FIN-sent, connection-still-readable. Not in the initial
  contract (no `shutdown` provider op exists today).

---

## 3. writev / scatter-gather

`kl_stream_writev(iov, iovcnt)` has the same disposition + backpressure semantics as §2. The iov
*array* is consumed by the call (copied/flattened as needed — the caller's `KlIoVec[]` need not
outlive the call). Providers without native writev fall back to sequential sends below the
contract (already handled by the socket seam's `kl_sockdef_writev`); the caller sees identical
semantics.

---

## 4. Backpressure (bounded, one mechanism)

**Write side.** A single bounded buffer per stream (`KlDrain` + reservation, `max_size` cap). Every
write returns a `KlStreamWriteResult` (§2), whose `status` is one of (atomic — `accepted` is `len`
or `0`):

| `status` | `accepted` | Caller action |
|---|---|---|
| `KL_STREAM_ACCEPTED` | `len` | all bytes taken (sent and/or queued); continue |
| `KL_STREAM_WOULD_BLOCK` | `0` | high-water reached; **nothing** taken — wait for the **writable** signal, then retry the whole write |
| `KL_STREAM_TOO_LARGE` | `0` | `len > total_capacity`; **permanent** — do **NOT** wait for writable (no notification will ever unblock it); **chunk** the message (§2) |
| `KL_STREAM_CLOSED` | `0` | write side gone (Tier-3 half-close, or peer-closed) — begin teardown |
| `KL_STREAM_ERROR` | `0` | fatal; `io_status` carries the category — begin teardown |

**One resume mechanism.** When buffered output falls below the low-water mark the stream fires a
single **writable** callback (today: `KlDrain.on_drain`). There is exactly one such notification
concept — no protocol-specific "drain" variants leak into the contract. The writable callback may
synchronously issue more writes (subject to §2 reentrancy).

**Read side.** See §5 (pause/resume). The two together guarantee invariant 4: neither direction
accumulates unboundedly.

---

## 5. Read pause / resume

**STRICT pause (resolved decision #3).** No read callback fires after `kl_stream_pause(stream)`
returns.
- readiness backends drop READ interest (no readable dispatch);
- completion backends do **not** post a new recv; a recv **already outstanding** at pause time
  completes into the single receive buffer but its bytes are **held, not delivered** — they are
  handed to the read callback only after `kl_stream_resume`. (This is the fix for the earlier
  contradiction, which allowed one post-pause delivery.)
- Keel does not accumulate beyond that one in-flight buffer.
- `kl_stream_resume(stream)` re-enables delivery (first flushing any held bytes), then readiness
  re-arms READ / completion re-posts a recv. **Both pause and resume are idempotent** (double-call
  is a no-op; resume does not double-post).
- This is the existing `read_paused` mechanism, renamed off the HTTP request onto the stream, plus
  the strict "hold the already-in-flight buffer" rule (new, **Tier 1 / Phase B**).

---

## 6. Close, half-close, and terminal outcomes

### 6.1 The close/terminal verbs

**`close` (graceful) and `cancel` (discard) are distinct — they do not overlap (Finding 5).**

| Verb | Tier | Meaning | Effect |
|---|---|---|---|
| `kl_stream_close` | 1 | **graceful** full close | reject new writes; **flush already-accepted output** (bounded by a **close deadline** — see below); then close and fire `on_close` (§7). Does NOT cancel the queued output it is flushing. |
| `kl_stream_cancel(stream)` | 1 | **discard** + detach + close | stop read delivery, **drop queued output**, cancel in-flight I/O by handle, detach (§7), fire `on_close`. Used when the caller does not want the flush. |
| `kl_stream_shutdown_write` | **3 (OPTIONAL)** | orderly finish of the *write* side | send FIN; writes → `CLOSED`; reads continue. **New provider work: no `shutdown` op today.** |
| `kl_stream_abort` | **3 (OPTIONAL)** | reset / hard close | send RST where supported; drop queued output; detach per §7. New provider work (no `abort` op today). |

- **Peer error** (RST / fatal I/O) is a **fatal close**: queued output is lost, the stream detaches
  and fires `on_close` with the error.
- **Close deadline / never-writable peer:** graceful `close`'s flush is bounded — if the peer never
  becomes writable within the stream's (or a caller-supplied) close deadline, `close` degrades to a
  discard+detach (queued bytes dropped) and still fires `on_close`. A graceful close never blocks
  the loop indefinitely.
- **Tier-1 cancel does NOT promise per-operation terminals.** `kl_stream_cancel` is coarse: stop
  delivery, cancel by handle where applicable, detach, and eventually fire `on_close` — it does
  **not** deliver separate "cancelled" terminals for the read op and the write op. Per-op terminals
  require the Tier-2 operation-identity records. Client `connect` is a separately cancellable
  `KlConnectOp` (§8.1); accept is push `on_accept` (no per-accept handle — cancel by closing the
  listener, §8.2).

### 6.2 Which terminal callback fires for each event

| Event | Tier | Terminal delivered to caller |
|---|---|---|
| peer sent FIN (orderly) | 1→2 | **Tier 1:** `KL_STREAM_CLOSED_UNKNOWN` (cause not distinguishable); **Tier 2:** orderly **EOF** (needs `KlCompletionEvent.status`, §header). (Tier-3 half-open keeps the write side open.) |
| peer sent RST | 1→2 | **Tier 1:** `KL_STREAM_CLOSED_UNKNOWN` (NOT "EOF-like" — an unknown termination is not a clean message boundary); **Tier 2:** read **error** `KL_IO_RESET` (needs `KlCompletionEvent.status`, §header) |
| local `close` (graceful) | 1 | flush accepted output (close deadline), detach, then exactly one `on_close` (§7) |
| local `cancel` (discard) | 1 | drop queued output, detach, then `on_close` (§7) — no per-op terminals in Tier 1 |
| `connect` op cancelled | 1 | that **`KlConnectOp`** → cancelled (exactly once) — the only per-op terminal in Tier 1 |
| `connect` deadline | 1 | that `KlConnectOp` → timed-out (exactly once); precedence per §6.3 |
| listener closed with accepts armed | 1 | armed accepts discarded; no `on_accept` fires for them (§8.2) — a listener-level terminal, not per-accept |
| local `shutdown_write` | **3** | no local callback; subsequent writes → `CLOSED` (optional; no provider op today) |
| local `abort` | **3** | drop queued output, detach per §7; no post-`on_close` callbacks (optional; no provider op today) |
| per-*stream-op* cancelled/timed-out terminals | **2** | separate read-op / write-op terminals require the Tier-2 operation-identity records |

### 6.3 Precedence (deterministic, invariant 5)

For a single op, exactly one terminal wins, resolved in this order at the moment the backend
retires the op:
1. If the op already produced a real completion this drain/tick → **completed/failed** (a
   cancel/timeout that races a real completion loses; the real result stands).
2. Else if cancelled → **cancelled**.
3. Else if past deadline → **timed-out**.
4. Else peer close/reset → **peer-closed** / **error**.

A cancel or timeout requested *after* the terminal event is a no-op (the op is already retired).
This is the completion axis's existing "exactly-one terminal + drop late events" discipline,
lifted to the stream contract.

---

## 6bis. TLS-wrapped stream (Tier 2 — deferred, plaintext ships first)

The initial `KlStream` is **plaintext**. A TLS-wrapped stream is a later phase; `KlTls` already
wraps the socket seam (socket-BIO: `t->sp = ctx->sockets`; or completion memory-BIO via
`feed_input`/`drain_output`), but the *generic-stream* TLS semantics below are not yet implemented
and are not normative. When TLS wrapping lands it must specify — so the wrapped stream stays
model-neutral and does not accidentally become readiness-shaped:

- **Handshake-before-delivery, and its failure ownership (Finding 2):** a newly connected/accepted TLS
  stream is delivered to the protocol only *after* a successful handshake (no app bytes before). The
  ownership terminal for a **failed/timed-out handshake** differs by side, and critically an accepted
  stream that never handshook is **never owned by the protocol**, so it must NOT receive the protocol's
  `on_close`:
  - **Client:** handshake failure/deadline resolves the `KlConnectOp` terminal as `refused`/`timed-out`
    (§8.1) — the caller owns the `KlConnectOp`, not a stream, so this is coherent; no stream `on_close`
    (the stream was never handed over).
  - **Accepted (server/listener):** the listener/TLS wrapper still owns the **half-built** stream. On
    handshake failure it performs **internal teardown only** — DETACHMENT per §7, then **credit-lease
    release** (§8.2) — and does **not** fire the public `on_close` (there was no `on_accept`, so the
    protocol never took ownership; a public `on_close` would break the "an accepted stream is
    application-ready" promise). The listener MAY expose an optional listener-level
    **`on_accept_error(listener, cause)`** for observability, which never conveys stream ownership.
    `on_accept` fires **only** on handshake success. This preserves the invariant that every stream
    delivered by `on_accept` is application-ready, and that `on_close` is paired 1:1 with a prior
    ownership handoff (`on_accept` or a `connect` terminal).
- **Cross-direction wants:** a `write` may need to read (`WANT_READ`) and a `read` may need to write
  (`WANT_WRITE`); the wrapped stream drives the underlying transport for the opposite direction
  without surfacing it to the protocol.
- **`pending()` drain:** after decrypting, buffered plaintext (`tls->pending()`) must be delivered
  without waiting for another transport event (else completion mode stalls).
- **Memory-BIO ciphertext ownership** (completion): the wrapped stream owns the ciphertext in/out
  rings for the op lifetime (the copy-required send lesson from the completion-TLS review).
- **Close:** orderly TLS `close_notify` vs transport FIN are distinct; TLS shutdown precedes (Tier-3)
  transport `shutdown_write`.
- **ALPN** is exposed via the existing `KlTls.alpn_protocol` accessor on the wrapped stream.

Until then, TLS continues to work through the current HTTP paths unchanged; migrating it onto the
generic stream is explicitly a separate, later step.

---

## 7. Lifecycle / destruction — NORMATIVE ownership model (invariants 1, 3, 4)

Ownership is a UAF boundary, so it is normative even in this DRAFT (resolved decision #5). The
central rule (Finding 4): **`on_close` signals confirmed *detachment* from caller storage, NOT
physical retirement of the backend's provider token.** These are different, and conflating them is
unimplementable on EFI (a firmware token may be quarantined and never physically retire before
ExitBootServices).

- **Two distinct facts:**
  - **DETACHMENT** — the backend guarantees no future callback or event can reference the caller's
    `KlStream` storage. Achieved via generation invalidation + stable backend-owned/quarantined op
    records. This is the UAF boundary the caller cares about.
  - **Physical retirement** — the OS/firmware no longer owns the backend token. May lag arbitrarily
    (EFI quarantine) or never happen before EBS. The caller must NOT wait on this.
- **The stream storage is caller-owned** — embedded (e.g. `KlHttpConn { KlStream stream; … }`) or in
  a caller pool. Keel never `malloc`s the stream behind the caller's back (preserves the
  preallocated-slot, allocation-free accept model).
- **`close` is deferred, not synchronous.** `kl_stream_close(stream)` *begins* teardown and returns;
  the stream enters **draining**: no further read/write callbacks are delivered, and the backend
  works toward **detachment** (cancel-by-handle; on a failed cancel it moves the op *record* into
  stable backend-owned/quarantined storage so a late **callback** lands there, not in caller memory —
  the EFI discipline generalized). **Quarantining the record permits detachment ONLY for case-(1)
  backend-owned-buffer ops (next bullet); an op that gave an external agent a pointer into stream
  storage (case (2)/(3)) stays *attached* until its retirement / release acknowledgement** — moving the
  record does not reclaim the buffer the kernel/stack still uses.
- **The DETACHMENT rule covers EVERY external reference into stream storage — reads AND writes, not
  just kernel recv buffers (Finding 1).** The general invariant is:
  > **DETACHMENT requires that no external agent (kernel, firmware, or an in-provider stack such as
  > lwIP) retain any pointer into caller-owned `KlStream` storage — whether for reading OR writing.**

  Invalidating a generation or quarantining an *operation record* protects the callback pointer; it
  does **not** reclaim memory an external agent still reads from or writes into. So a backend must
  classify **each in-flight op** (read *or* write) into one of three cases and know which it is:
  - **(1) Backend-owned copy/scratch.** The op references stable storage the *backend* owns — never the
    caller's `KlStream`. Reads: all TLS ciphertext (`IOU_TLS_RECV`/`KL_IOCP_TLS_RECV`/`PC_TLS_RECV`
    land in a transient `op->sendbuf`/`g_tls_cipher` scratch, then `feed_input`) + EFI plaintext (the
    firmware `ReceiveData` runs *synchronously inside drain*, so no firmware op holds `read_buf`
    between ticks). Writes: every backend that **copies** the send payload into a backend-owned op
    buffer before submitting (EFI's inline `sndbuf`; io_uring/IOCP `WRITE` copies the remainder). Here
    force-detach at the close deadline is safe — invalidate the generation, quarantine the record,
    **synchronously ack `DETACHED`** — because no external agent ever held a pointer into `KlStream`.
  - **(2) External op referencing caller-stream storage.** The op hands an external agent a pointer
    **into the `KlStream`'s own read or write storage.** Reads: `IOU_READ`
    (`op->buf = c->read_buf + c->read_len`), `KL_IOCP_READ` (`buf.buf = c->read_buf + c->read_len`) —
    the kernel may write there until the op retires. Writes: any provider that **pins the stream-owned
    queue storage** for the op lifetime (§2 permits this — e.g. lwIP-raw referencing the send buffer)
    — the stack may read there until the op retires. Here detachment **requires that op's
    cancellation/retirement to actually complete** (the CQE / completion packet is dequeued, or the
    provider's release/ack callback fires). The close deadline may escalate graceful→cancel, but the
    parent may **not** free/reuse the stream on a timer alone, and there is **no synchronous-ack
    shortcut** — freeing the buffer with a live kernel/stack op still referencing it is a UAF the
    quarantine cannot prevent.
  - **(3) Provider pin with an explicit release.** A special case of (2): a provider that pins
    stream storage but delivers a **release/acknowledgement callback** (lwIP `tcp_sent`-style) — that
    callback firing **is part of detachment**. `on_close` may not fire until every such release has
    landed for the stream.
  A backend in case (1) for all its outstanding ops may take the synchronous-ack path; a backend with
  **any** outstanding case-(2)/(3) op (io_uring/IOCP plaintext read, lwIP write pin) must drive
  cancel-/release-to-completion (below) before `on_close`.
- **`on_close(stream)` fires exactly once, when DETACHMENT is confirmed** — which may be *before*
  physical token retirement. Only then may the caller free/reuse the (embedded/pooled) storage;
  **no callback of any kind fires afterward.** A backend signals this via a `DETACHED`
  close-acknowledgement ("no future reference to this `KlStream` is possible; I may still hold
  quarantined provider-owned storage"). This lets a server pool slot / embedded stream retire
  safely even while EFI keeps a quarantined token — synchronous server destruction never blocks on
  firmware.
- `close`/`cancel` **inside a callback** are legal (recorded; applied after the callback returns).
- **Synchronous parent teardown drives detachment; it does not merely wait for it (Finding 6).**
  `close` is deferred, but `kl_server_free`/`kl_listener_free`/any parent destructor is a *blocking*
  call that must not return while a stream can still reference the pool it is about to release. It
  reconciles the two by **driving** detachment to completion rather than passively waiting:
  - **Backends with outstanding case-(2)/(3) ops (io_uring/IOCP plaintext reads; lwIP write pins)** —
    it `close`s every live stream (submitting the cancels), then **runs a completion-drain loop** —
    dequeue completions/releases, matching each retiring **read *or* write** op to its stream — until
    every stream has confirmed DETACHMENT. "Bounded" here bounds the *graceful flush* (the close
    deadline escalates graceful→cancel) and the *submission* work — it does **not** license freeing a
    stream whose read cancel, write cancel, or write-pin release has not yet completed. The parent
    blocks until every op's **retirement observation** lands (a kernel recv into `read_buf`, or a
    stack still reading a pinned write buffer, is waited out — not force-detached on a timer;
    Finding 1).
    - **Termination is per-op, not "cancel always completes" (Finding 4).** The guarantee is:
      > **every *successfully submitted* op eventually produces — or has already produced — exactly one
      > retirement observation, whether natural or cancelled.**
      The drain loop must therefore handle, per op: (i) **cancel submission failing** (SQE/alloc
      exhaustion) → the op is still live, so retry the cancel or wait for its natural completion,
      never treat the stream as detached; (ii) **natural completion winning the race** with the
      cancel → that natural completion *is* the one retirement observation (a following "cancel
      not-found" is discarded, not a second terminal); (iii) **"operation not found"** because
      retirement is already queued → also discarded; (iv) **event-loop/backend failure during
      destructor draining** → the destructor cannot silently free; it must surface the failure
      (an op whose retirement can no longer be observed keeps the stream quarantined rather than
      freed). A stream detaches only when each of its submitted ops has hit exactly one of these.
  - **Backends whose outstanding ops are all case-(1)** — a provider whose `close` can **synchronously
    acknowledge detachment** (invalidate the generation, move any in-flight op record into
    provider-owned/quarantined storage, then return `DETACHED` from the close call itself) lets the
    parent detach without a drain loop at all. **EFI takes exactly this path**: it cannot guarantee
    physical token retirement before ExitBootServices, so it quarantines and acks detachment
    synchronously — the pool is freed immediately, the token retires (or not) independently. This is
    legal *only because* EFI's reads are backend-owned/synchronous (first bullet of the DETACHMENT
    rule above); it is the general form of today's EFI quarantine-on-close.
  Either way the invariant holds: **the parent never frees storage a live op can still write into**,
  and it never blocks on physical retirement.
- "Retirement is confirmed, not immediate" — `abort` (Tier 3) obeys the same deferred `on_close`;
  it never assumes synchronous op retirement (EFI cannot guarantee it).
- **`on_close` is a DESTRUCTIVE TAIL callback.** It is the *final* action the implementation takes
  involving that stream: after invoking `on_close` the implementation must **never** touch the
  `KlStream` again — no field read, no logging through it, no list removal referencing it, no further
  callback. The caller may free the stream (or the parent object containing it) *inside* `on_close`.
  The same destructive-tail rule applies to the terminal callback of a caller-owned `KlConnectOp`
  (§8.1) and to the accepted stream's `on_accept`/`on_close` pair (§8.2).
- **Fixed teardown order — internal release runs BEFORE the public destructive tail (Finding 4).**
  Because `on_close` is destructive (the caller may free the stream/parent *inside* it), no
  Keel-internal work may run after it or *through* it. So a closing stream executes, in this exact
  order:
  1. **Detach backend operations** — cancel/quarantine per the DETACHMENT rule; confirm no external
     agent can reference the stream (incl. no kernel write into `read_buf`).
  2. **Run internal resource/release hooks** — release the accept lease (§8.2): **always return the
     stream slot** via the pool-owned slot-release capability, and **update listener credit + re-arm
     only if the listener-credit target is still live** (null after standalone listener teardown → skip,
     never a slot leak). Also unlink from internal lists and release any Keel-owned buffers. These touch
     *Keel* state, never the caller's `KlStream` beyond reading it, and the credit half must be
     idempotent w.r.t. an **already-destroyed listener** (§8.2 split lease).
  3. **Invoke public `on_close(stream)`** — the last access to the stream; nothing follows.
  Credit is therefore returned in step 2, never "after `on_close`" or via work the caller schedules
  from inside it.

---

## 8. Connect (client) and accept (listener)

- **Connect:** `kl_stream_connect(endpoint, deadline, done)` is an async op with the §6.3 terminal
  set: **connected** | **refused** (`KL_IO`/`KL_ERR_CONNECT`) | **timed-out** | **cancelled**.
  Cancellation racing a successful connect resolves per §6.3 (completion wins if it already
  happened). The client's Happy-Eyeballs address racing is an implementation of this op, invisible
  to the caller.
- **Accept is push-based, not pull.** A listener is configured with an `on_accept(listener,
  stream)` callback and started; Keel keeps it armed and delivers each connected `KlStream` by
  invoking `on_accept` (readiness: listener readable → `accept`; completion: posted accept →
  `KL_COMP_ACCEPT`). There is **no** `kl_listener_accept` pull call and **no** public per-accept
  handle — a single push model avoids the ambiguity of mixing a pull `KlAcceptOp` with pool/credit
  arming (Finding 4). This matches how the HTTP server already consumes accepts.

### 8.1 Pre-stream operation handle — `KlConnectOp` (ownership)

The client `connect` precedes a live `KlStream`, so it has its own record with the same ownership
model as a stream:
- **Caller-owned** storage (embedded or pooled); Keel does not allocate it behind the caller's
  back. It must remain valid until its terminal callback fires.
- **Exactly one terminal callback** (connected/refused/timed-out/cancelled, per §6.3), which is a
  **destructive tail** (§7) — no access to the handle afterward; the caller may reuse its storage in
  the callback.
- **Cancellation is deferred**, not synchronous: `cancel` requests it; the terminal still arrives
  exactly once (as `cancelled`, unless a real result already won per §6.3).
- **Stream transfer:** a successful `connect` hands the resulting `KlStream` to the caller **in**
  the terminal callback (the stream is live for the duration of that call and beyond, owned per §7);
  it is not delivered before the terminal.

Accept has **no** analogous caller-owned handle — it is push-only (§8.2). Cancel an accept by
closing the listener; the listener-level terminal (§6.2) is that armed accepts are discarded with
no `on_accept`.

### 8.2 Listener seam (the generic accept carve — Phase A/B)

- **Retyped ops:** `prime_accepts(KlListener*)` / `post_accept(KlListener*)`; the completion event
  identifies its target as `KL_COMP_ACCEPT.target = KlListener*` (today it reaches `KlServer`).
- **Multiple listeners share one `KlEventCtx`** — each carries its own target + credit; the drain
  routes each `KL_COMP_ACCEPT` to the listener named in `target`.
- **Arming RESERVES a concrete slot; credit is not an advisory count (Finding 3).** The listener owns
  a caller-supplied acquire hook (the HTTP server passes its existing connection pool — no new pool, no
  per-accept allocation). **Before** it arms/posts an accept, it **atomically reserves a concrete
  stream slot + credit lease** from that hook — arming an accept *is* the reservation, and the
  completion merely **consumes** what was already reserved. So at any moment **armed accepts ≤
  min(reservable slots, backend arming capacity)**, and a completed accept can never fail to find
  backing storage. This generalizes today's server-pool backpressure floor + the EFI capacity-gated
  arming (currently `KlServer`-scoped).
- **On each completed accept:** bind the `KlStream` to the **already-reserved** slot, initialize it,
  then invoke `on_accept(listener, stream)` (an ownership handoff; §7 rules govern the stream from
  there). Because the slot was reserved at arm time, acquisition never fails *here*.
- **If reservation itself fails** — on the **conforming (preallocated, allocation-free) path** the only
  failure causes are **pool exhaustion, listener/parent shutdown, or acquire-hook rejection /
  inconsistent external-pool state** (Finding 6). *Allocator failure is NOT a conforming-path cause* —
  the accept path never allocates; only a **hosted, optional allocator-backed pool** could additionally
  fail on allocation, and that is explicitly **non-conforming/optional**, not part of the normal accept
  path. On any such failure the listener simply **does not arm** that accept (it drops below full
  arming) and, if it holds an accepted-but-unbindable handle from a backend that accepted anyway, it
  **closes that handle, restores the credit, reports `on_accept_error`, and re-arms only if the listener
  is still live.** A generic acquire hook is thus allowed to fail — the contract just never lets that
  failure surface *after*
  `on_accept` has handed a stream over. "Credit" always means reserved backing, never a hope.
- **`on_accept` / `on_accept_error` reentrancy — liveness lives in a backend-owned token, never a
  re-read of the possibly-freed listener (Finding 2).** The callback runs on the loop thread and **may
  synchronously**: close/free the accepted stream; `close`/free the **listener**; stop accepting; or
  destroy the parent server. Re-reading the `KlListener` *after* the callback to check liveness would
  itself be a UAF if the callback freed it. So the mechanism is:
  - **The accept dispatch holds a stable, backend-owned registration/token** (carrying the listener's
    **generation + a liveness/accepting flag**) that **outlives the callback** and is **not** part of
    `KlListener` storage. After the callback the dispatch inspects **that token**, never the
    `KlListener` pointer.
  - **The token is cleared/invalidated by listener teardown itself** (the same generation-invalidation
    used for streams). So `on_accept` freeing the listener marks the token dead; the post-callback
    dispatch reads the token, sees dead, and **does not re-arm and does not touch `KlListener`.**
  - Equivalently, a backend MAY do **all** re-arm/credit bookkeeping **before** invoking the callback
    and treat `on_accept`/`on_accept_error` as an **unconditional destructive tail** (no post-callback
    dispatch work at all). Either is conforming.
  - (A backend that instead **prohibits freeing the listener inside the callback** — allowing only a
    *deferred* `kl_listener_close` — is also legal and needs no token; it is the more restrictive
    option.)
  In all cases: **no read of the `KlListener` after the callback through the original pointer.** The
  earlier "re-reads the listener's generation/state" wording is replaced by "reads the backend-owned
  token."
- **`on_accept` conveys ownership; a stream that fails before `on_accept` is never handed over
  (Finding 2).** For a **TLS listener**, the handshake runs on the half-built stream *before*
  `on_accept`. On handshake failure/deadline the listener does internal teardown (§7 DETACHMENT +
  credit-lease release) and fires the optional listener-level **`on_accept_error`** — but **no
  `on_accept` and no stream `on_close`**, because the protocol never took ownership. `on_close` is
  paired 1:1 with a prior `on_accept` (or `connect` terminal); it never fires for a stream the caller
  never received. (Plaintext accept has no pre-handoff failure window — the stream is ready at accept.)
- **Credit is an internal lease, not a raw back-pointer (Finding 3 + Finding 4).** Each accepted
  stream carries an internal **credit lease** naming the listener's credit accounting, not the public
  `KlListener` storage. The lease indirection exists so a stream outliving the listener never
  dereferences freed listener storage.
- **The lease carries TWO independently-lived capabilities — a nullable listener-credit target and a
  stable slot-release capability (Finding 1 + Finding 4).** Conflating them means neutralizing the
  lease on listener death also loses the slot-return path, leaking the slot. So the embedded lease holds
  two separable things:
  - **Slot-release capability** — a **stable `release_slot(ctx, stream)` callback + context owned by
    the pool/caller, NOT by `KlListener` storage.** It remains valid until the stream closes,
    *independent of listener lifetime*. Returning the stream slot to the pool goes through this.
  - **Listener-credit target** — a **nullable** pointer to the listener's credit accounting. Updating
    credit + arming the next accept goes through this, and only when it is non-null (listener live).
  The lease record itself is **allocation-free** — embedded in the preallocated stream/connection slot
  (or a preallocated listener arena), never `malloc`d per accept. Its internal release (step 2 of §7
  teardown) therefore does, in order:
  1. **return the stream slot** via the stable `release_slot` capability — **always**, unconditionally;
  2. **if the listener-credit target is non-null**, return/update credit accounting so the listener can
     arm the next accept; if it is null (listener already destroyed), **skip credit + rearm**.
  This closes the **accepted-TLS-failure** path (Finding 2): since no public `on_close` fires there, the
  internal teardown runs this **same** step-2 release — the half-built stream's slot returns to the pool
  via `release_slot`, credit returns iff the listener lives — not a bare counter decrement.
- **Listener lifetime vs accepted streams (Finding 1): slot-release outlives the listener; credit does
  not.** An accepted stream may outlive the listener, and closing it must still return its slot without
  touching freed listener storage:
  1. **Parent-owned (the HTTP server, today):** the parent owns *both* the listener and the stream pool
     and tears down in order — close all accepted streams, drive them to DETACHMENT + lease release
     (§7), *then* free the listener. Streams never outlive the listener, so credit accounting is always
     live at release. (Slot-release still goes through the pool-owned capability, as always.)
  2. **Standalone listener close:** `kl_listener_free`/close **null-outs only the listener-credit
     target** of every outstanding accepted stream's lease — it does **not** touch the slot-release
     capability (pool-owned, still valid). A later stream close then: **returns its slot** (step-2.1,
     via the still-valid `release_slot`), **skips credit update/rearm** (step-2.2, target is null), and
     **never dereferences the listener.** The listener may be freed while accepted streams are still
     live, and no slot is leaked.
  The invariant the release obeys: **the slot always comes back; the credit update is best-effort and
  guarded by a non-null (live) listener target.** Discipline (1) ships in Phase A/B (the current
  `KlServer`-owns-pool model); (2) is the general form for a standalone `KlListener`. (A simpler Tier-1
  option — *forbid accepted streams outliving the listener* — is also legal but less generic; the
  split-lease model is preferred so a standalone listener works.)
- **Credit return:** on close a live-listener stream returns its lease's credit (step 2.2), letting the
  listener arm the next accept; a stream whose listener already died just returns its slot. Accept never
  exceeds outstanding credit.
- **Listener close** detaches outstanding armed (not-yet-accepted) accepts under §7 (deferred
  `on_close` semantics for any half-built stream), draining/quarantining any posted accept op the
  same way streams do; no `on_accept` fires for a discarded armed accept. *Already-accepted* streams
  are governed by the lifetime disciplines above, not by this bullet.

---

## 9. Endpoints (§0) — `KlSockAddr` directly (resolved decision #6)

The initial `KlEndpoint` **is `KlSockAddr`** (already neutral: IPv4/IPv6/Unix). It carries
**address identity only** — it does NOT carry a provider/transport "kind" (`TCP`/`Unix`/`lwIP`/
`EFI`). Those are two different axes: address family is intrinsic to the address, but *which
provider* handles it (native vs lwIP vs EFI) is selected through `KlEventCtx`/config — putting
provider identity in the endpoint would violate the axis separation the project established. So
`connect`/`bind`/`listen` take a `KlSockAddr` and dispatch through the *configured* provider;
`local_endpoint`/`peer_endpoint` return a `KlSockAddr` read via the existing `kl_sockaddr_*`
accessors.

A **tagged `KlEndpoint` union** (address family/kind + payload) is introduced ONLY when a genuinely
non-socket address type appears that cannot live in `KlSockAddr` (e.g. a Windows named pipe path or
vsock CID/port) — Tier 3. Even then the tag distinguishes *address kinds*, never *providers*.

---

## 10. Explicitly out of contract (belongs below or above)

- **Below (provider/backend, not the caller's concern):** fd/SOCKET/OVERLAPPED/io_uring_sqe/
  lwip_pcb, readiness re-arm, completion submission, `TCP_NODELAY`/`cork`/`sendfile`
  (optional provider capabilities, never required by the contract).
- **Above (protocol, not the transport's concern):** HTTP headers/status, WebSocket opcodes,
  HTTP/2 stream IDs, framing, request/response. The transport moves bytes only.
- **Not introduced:** futures/promises/coroutines/async-await; a stream⇄datagram merge; native
  descriptors in the generic API; mandatory heap/threads/TLS; protocol autodetect; runtime plugins.

---

## 11. Conformance gates (drive Phase-8/10 tests), by tier

Observable through `KlStream`/`KlListener` only, under ASan+UBSan, identical across readiness and
completion and across TCP/Unix/lwIP where available.

**Tier-1 gate (the first cut must pass all of these):**
- 1-byte fragmented reads reassemble; large reads stream within the read buffer bound.
- `kl_stream_write`/`writev` return `KlStreamWriteResult` with **atomic** acceptance (`accepted` is
  `len` or `0`); `writev` matches `write`. A write `> total_capacity` returns `TOO_LARGE`
  (permanent); the caller **chunks** logical messages larger than the bounded queue (§2).
- `WOULD_BLOCK` takes 0 bytes; the single low-water **writable** callback lets the caller retry.
- `pause` halts delivery **and** accumulation (strict: an in-flight recv is held, not delivered);
  `resume` is idempotent.
- **Read termination** (Tier 1) surfaces as a single `KL_STREAM_CLOSED_UNKNOWN` close — "the stream
  ended; cause not distinguishable." There is **no `recv≤0 → EOF` rule**; distinguishing orderly FIN
  from reset/error is a Phase-C capability (the `KlIoStatus status` field, §header).
- `close` (graceful) flushes accepted output within the close deadline then fires exactly one
  `on_close`; `cancel` discards + detaches then fires `on_close`; the `connect` op's cancel +
  deadline each yield exactly one terminal (accept is push-only — no per-accept terminal; §8.2).
- `close`/`cancel` inside a callback are safe; **no callback after `on_close`**; caller may reuse the
  embedded/pooled storage after `on_close`; no UAF / double-callback / leak.
- `on_close` fires on **detachment**, not physical token retirement (server destruction does not
  block on EFI-quarantined tokens).
- **Detachment covers writes, not just reads (Finding 1):** `close`/parent-teardown **while an
  outstanding write** pins stream-owned queue storage (and, where available, an lwIP-style write-pin
  release) waits out that op's retirement before `on_close` — under ASan, no send buffer is freed
  while an op still references it. The matrix runs this for *both* a posted receive and an outstanding
  write.
- **Cancel-vs-completion race (Finding 4):** cancel racing a natural completion yields **exactly one**
  retirement (natural wins; the late "cancel not-found" is discarded — no second terminal, no missed
  `on_close`); a failed cancel submission does not detach the stream.
- **Standalone-listener slot recovery (Finding 1):** free a standalone `KlListener` while an accepted
  stream is still live, then close that stream — under ASan/LSan the **stream slot returns to the pool**
  (no leak), credit update + re-arm are **skipped**, and the freed `KlListener` is **never
  dereferenced**. (Slot-release capability outlives the listener; credit target is nulled.)
- **Callback frees the listener (Finding 2):** an `on_accept`/`on_accept_error` that synchronously frees
  the listener (or stops accepting) does not cause a post-callback read of the freed listener — the
  driver consults the backend-owned token, does not re-arm, and is ASan-clean.

**Tier-2 gate (once the new machinery lands):**
- EOF **vs** RST distinguishable identically across models (requires the extended completion event).
- Per-stream read-op / write-op terminals (operation identity) with §6.3 precedence.
- Write reservation guarantees atomicity even when the direct send takes a prefix and the cap is hit.
- TLS-wrapped stream (§6bis): handshake-before-delivery, cross-direction wants, `pending()` drain,
  memory-BIO ownership, close-notify vs FIN, handshake deadline.
- **Accepted-TLS handshake failure ownership (Finding 2):** a failed/timed-out accepted handshake fires
  **no `on_accept` and no stream `on_close`** — only internal teardown + credit-lease return (+ the
  optional `on_accept_error`); the credit is reclaimed so the listener can arm the next accept. Assert
  `on_close` is 1:1 with a prior `on_accept`/`connect` handoff (never for a stream the caller never
  received).

**Tier-3 gate (optional capabilities, when implemented):**
- `shutdown_write` then writes → `CLOSED` while reads continue (half-open); `abort`/RST; tagged
  non-socket endpoint; file-transfer extension.
