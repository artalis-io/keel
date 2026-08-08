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
> **Phase A/B/C/Optional** plan in `docs/generic_transport_audit.md` §8. Crucially, the Tier-1
> *capabilities* below are the **baseline public surface = Phase B**, which is **not** pure
> extraction — it needs new machinery (write reservation, the listener carve, deferred close +
> detachment) built first (Phase A is the pure-extraction step, internal, no public API). So Tier 1
> is the shippable target, **not** "safe to implement immediately."

- **Tier 1 (baseline public surface — built in Phase B):** push read-delivery callback;
  `kl_stream_write` with an **atomic-accept** result (over `KlDrain` reservation); low-water
  writable notification; stream-level strict `pause`/`resume`; graceful `kl_stream_close` +
  detachment `on_close`; cancellable `KlConnectOp`/`KlAcceptOp`; listener credits; **plaintext
  only**. Delivers **EOF** as a terminal (recv≤0), matching today's readiness/HTTP behavior.
- **Tier 2 also owns model-neutral EOF-vs-RST.** The distinction is only reliable on the
  synchronous/readiness seam today; the completion event (`KlCompletionEvent`) currently carries
  just `bytes`+`ok` and **no `KlIoStatus` category**, so completion drivers cannot distinguish
  orderly FIN from reset without extending the event (add a target-side error field). Until that
  extension lands, EOF-vs-RST is NOT identical across models — so it is Tier 2, not "already
  observable" (correcting an earlier claim).
- **Tier 2 (NEW machinery — designed here, must be built before this is normative):** the concrete
  write-result type + backpressure API; operation identity + cancellation; deferred-destruction
  ownership; TLS-wrapped-stream state machine.
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
   at low-water — a small `KlDrain` extension (Tier 2), not a new subsystem.
3. **Pause = STRICT.** No read callback fires after `pause` returns; a completion recv already
   outstanding at pause time completes into the single receive buffer and its bytes are delivered
   only after `resume`. Bounded to one buffer. (Resolves the earlier §5 contradiction.)
4. **Cancellation = stream-level for the first cut.** No per-*write* cancel (that needs the Tier-2
   op-lifecycle). `kl_stream_cancel(stream)` cancels the stream's in-flight I/O; `connect`/`accept`
   are separately cancellable op *handles* (they precede a live stream).
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
- Pre-stream operation handles: **`KlConnectOp` / `KlAcceptOp`** — separately cancellable records for
  the connect and accept operations that *precede* a live stream (§8). There is **no** generic
  public `KlOp` type; it is not introduced until a real generic operation type is actually needed.

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
- **EOF (orderly peer close, FIN):** delivered as a distinct terminal read outcome — a zero-length
  EOF signal (today: `recv`==0 → `KL_COMP_READ` with `bytes==0`/`ok` → driver closes; readiness:
  `recv`==0). After EOF no further read callbacks fire. **Tier 1: EOF ends the stream** (today's
  HTTP behavior maps `recv≤0` to close). **[Tier 3] half-open** — keeping the write side usable
  after peer FIN — depends on `shutdown_write` provider support and is NOT in the initial contract.
- **[Tier 2] Peer reset (RST) vs orderly EOF:** reset is *intended* to be a read **error** with
  category `KL_IO_RESET` (`KlIoStatus`), distinct from EOF, but this is Tier 2 (§ header): the
  completion event carries no error category yet. EOF = "peer finished sending, cleanly"; reset =
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
  | `KL_STREAM_WOULD_BLOCK` | `0` | high-water reached; **nothing** taken — retry after the writable signal |
  | `KL_STREAM_CLOSED` | `0` | write side gone (Tier-3 half-closed, or peer-closed) |
  | `KL_STREAM_ERROR` | `0` | fatal; `io_status` carries the category |
  Provider-level short writes (a `send` that took a prefix) are **internal**: the stream keeps the
  remainder in its own storage and reports `ACCEPTED`. They are never observable as partial API
  acceptance.
- **Reservation requirement (implementation note for decision #1).** All-or-none is only safe if the
  stream **reserves queue capacity for the whole write before exposing any byte to the provider** —
  otherwise a direct `send` could take a prefix and then the buffer cap/alloc could reject the
  remainder, which is neither atomic nor recoverable. Implement via one of: (1) preflight enough
  queue capacity, then attempt the direct send; (2) copy the whole write into owned storage first,
  then pump; (3) extend `KlDrain` with a reservation op. Without reservation, atomic acceptance is
  not implementable — this is a Tier-2 requirement, not free from today's `KlDrain`.
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
  the strict "hold the already-in-flight buffer" rule (new, Tier 2).

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
  require the Tier-2 operation-identity records. `connect`/`accept` are separately cancellable op
  handles (`KlConnectOp`/`KlAcceptOp`, §8) because they precede a live stream.

### 6.2 Which terminal callback fires for each event

| Event | Tier | Terminal delivered to caller |
|---|---|---|
| peer sent FIN (orderly) | 1 | read **EOF** (§1); Tier-1 ends the stream (Tier-3 half-open keeps write side open) |
| peer sent RST | 2 | read **error** `KL_IO_RESET` (needs the completion-event error field, §header); Tier-1 sees it as EOF-like close |
| local `close` (graceful) | 1 | flush accepted output (close deadline), detach, then exactly one `on_close` (§7) |
| local `cancel` (discard) | 1 | drop queued output, detach, then `on_close` (§7) — no per-op terminals in Tier 1 |
| `connect`/`accept` op cancelled | 1 | that **`KlConnectOp`/`KlAcceptOp`** → cancelled (exactly once) — these are the only per-op terminals in Tier 1 |
| `connect`/`accept` deadline | 1 | that op → timed-out (exactly once); precedence per §6.3 |
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

- **Handshake-before-delivery:** a newly connected/accepted TLS stream is delivered to the protocol
  only *after* a successful handshake (no app bytes before). Handshake **failure** and **deadline**
  are connect/accept terminals (§8), distinct from a plaintext connect.
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
  works toward **detachment** (cancel-by-handle; on a failed cancel it moves the op record into
  stable backend-owned/quarantined storage so a late event lands there, not in caller memory — the
  EFI discipline generalized).
- **`on_close(stream)` fires exactly once, when DETACHMENT is confirmed** — which may be *before*
  physical token retirement. Only then may the caller free/reuse the (embedded/pooled) storage;
  **no callback of any kind fires afterward.** A backend signals this via a `DETACHED`
  close-acknowledgement ("no future reference to this `KlStream` is possible; I may still hold
  quarantined provider-owned storage"). This lets a server pool slot / embedded stream retire
  safely even while EFI keeps a quarantined token — synchronous server destruction never blocks on
  firmware.
- `close`/`cancel` **inside a callback** are legal (recorded; applied after the callback returns).
- Parent (`KlListener`/HTTP server) destruction closes its streams under the same rules and waits
  for their `on_close` before releasing the pool.
- "Retirement is confirmed, not immediate" — `abort` (Tier 3) obeys the same deferred `on_close`;
  it never assumes synchronous op retirement (EFI cannot guarantee it).
- **`on_close` is a DESTRUCTIVE TAIL callback.** It is the *final* action the implementation takes
  involving that stream: after invoking `on_close` the implementation must **never** touch the
  `KlStream` again — no field read, no logging through it, no list removal referencing it, no further
  callback. The caller may free the stream (or the parent object containing it) *inside* `on_close`.
  The same destructive-tail rule applies to the terminal callback of a caller-owned `KlConnectOp`/
  `KlAcceptOp` (§8).

---

## 8. Connect (client) and accept (listener)

- **Connect:** `kl_stream_connect(endpoint, deadline, done)` is an async op with the §6.3 terminal
  set: **connected** | **refused** (`KL_IO`/`KL_ERR_CONNECT`) | **timed-out** | **cancelled**.
  Cancellation racing a successful connect resolves per §6.3 (completion wins if it already
  happened). The client's Happy-Eyeballs address racing is an implementation of this op, invisible
  to the caller.
- **Accept:** `kl_listener_accept` yields the next connected `KlStream` (readiness: listener
  readable → `accept`; completion: posted accept → `KL_COMP_ACCEPT`), identical to the caller.
  Accept is **capacity-gated by a listener-intrinsic credit**, not by reaching into a server pool:
  the listener owns its accepted-stream target (a caller-supplied pool + a credit count), and
  never accepts beyond that credit (this generalizes today's server-pool backpressure floor + the
  EFI capacity-gated arming, which are currently `KlServer`-scoped). `cancel` before/racing an
  accept, and listener close with a pending accept, follow §6/§7 (deferred `on_close`).

### 8.1 Pre-stream operation handles — `KlConnectOp` / `KlAcceptOp` (ownership)

These precede a live `KlStream`, so they have their own records with the same ownership model as
streams:
- **Caller-owned** storage (embedded or pooled); Keel does not allocate them behind the caller's
  back. Each must remain valid until its terminal callback fires.
- **Exactly one terminal callback** (connected/refused/timed-out/cancelled, per §6.3), which is a
  **destructive tail** (§7) — no access to the handle afterward; the caller may reuse its storage in
  the callback.
- **Cancellation is deferred**, not synchronous: `cancel` requests it; the terminal still arrives
  exactly once (as `cancelled`, unless a real result already won per §6.3).
- **Stream transfer:** a successful `connect`/`accept` hands the resulting `KlStream` to the caller
  **in** the terminal callback (the stream is live for the duration of that call and beyond, owned
  per §7); it is not delivered before the terminal.

### 8.2 Listener seam (the generic accept carve — Phase A/B)

- **Retyped ops:** `prime_accepts(KlListener*)` / `post_accept(KlListener*)`; the completion event
  identifies its target as `KL_COMP_ACCEPT.target = KlListener*` (today it reaches `KlServer`).
- **Multiple listeners share one `KlEventCtx`** — each carries its own target + credit; the drain
  routes each `KL_COMP_ACCEPT` to the listener named in `target`.
- **Acquire/release seam:** the listener draws the accepted `KlStream` from a **caller-supplied
  pool** via an acquire hook (the HTTP server passes its existing connection pool — no new pool, no
  per-accept allocation) and **returns the accept credit when that stream closes** (its `on_close`,
  §7). Accept never exceeds the outstanding credit.
- **Listener close** detaches outstanding accepts under §7 (deferred `on_close`), draining/
  quarantining any posted accept op the same way streams do.

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
  `len` or `0`); `writev` matches `write`; large writes stream within the bounded queue.
- `WOULD_BLOCK` takes 0 bytes; the single low-water **writable** callback lets the caller retry.
- `pause` halts delivery **and** accumulation (strict: an in-flight recv is held, not delivered);
  `resume` is idempotent.
- **EOF** ends the stream (recv≤0).
- `close` (graceful) flushes accepted output within the close deadline then fires exactly one
  `on_close`; `cancel` discards + detaches then fires `on_close`; `connect`/`accept` op cancel +
  deadline each yield exactly one terminal.
- `close`/`cancel` inside a callback are safe; **no callback after `on_close`**; caller may reuse the
  embedded/pooled storage after `on_close`; no UAF / double-callback / leak.
- `on_close` fires on **detachment**, not physical token retirement (server destruction does not
  block on EFI-quarantined tokens).

**Tier-2 gate (once the new machinery lands):**
- EOF **vs** RST distinguishable identically across models (requires the extended completion event).
- Per-stream read-op / write-op terminals (operation identity) with §6.3 precedence.
- Write reservation guarantees atomicity even when the direct send takes a prefix and the cap is hit.
- TLS-wrapped stream (§6bis): handshake-before-delivery, cross-direction wants, `pending()` drain,
  memory-BIO ownership, close-notify vs FIN, handshake deadline.

**Tier-3 gate (optional capabilities, when implemented):**
- `shutdown_write` then writes → `CLOSED` while reads continue (half-open); `abort`/RST; tagged
  non-socket endpoint; file-transfer extension.
