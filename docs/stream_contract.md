# Keel Stream Transport Contract (DRAFT — not yet normative)

**Date:** 2026-08-08
**Status: DRAFT.** A prior version was labelled "normative"; that was premature — it promised
capabilities Keel does not have yet (per-op cancel, half-close/abort, prefix-accept writes,
TLS-wrapped-stream semantics) as though existing primitives already guaranteed them. This revision
(a) demotes it to DRAFT, (b) tiers the semantics into what exists vs. must-be-built vs. optional,
and (c) records the resolved design decisions for the must-be-built parts. It becomes normative
only after the Tier-2 machinery lands (see `docs/generic_transport_audit.md` §8).

### Capability tiers (which parts of this doc are real *today* vs. new work)

- **Tier 1 (extraction — safe to implement now):** push read-delivery callback; `kl_stream_write`
  with an **atomic-accept** result; stream-level `pause`/`resume`; `kl_stream_close`; EOF vs RST on
  the read side (already observable via `KlIoStatus`). **Plaintext only.**
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
5. **Destruction = caller-owned + deferred close + `on_close` terminal (normative, §7).** `close`
   *begins* teardown; the object stays internally retained until every outstanding op is confirmed
   retired (interoperates with EFI quarantine — retirement is *confirmed*, never "immediate"), then
   the single `on_close` callback fires; only then may the caller free/reuse the (embedded or
   pooled) storage. No callback after `on_close`.
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

- `KlStream` — an ordered, connected, reliable byte stream (TCP / Unix / lwIP TCP / TLS-wrapped /
  future pipe / vsock). Not a datagram (see `KlUdp`/`KlDatagram`).
- `KlListener` — accepts connected `KlStream`s.
- `KlEndpoint` — a transport-kind-tagged address (`KlSockAddr` + kind), enough to dispatch to the
  right provider; never forced into native `sockaddr` for non-socket kinds.
- `KlOp` — an async operation's lifecycle, realized by the existing `KlAsyncOp` + `KlTimer` +
  `kl_comp_cancel`; not a new future/promise type.

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
  `recv`==0). After EOF no further read callbacks fire. Writes MAY still be possible until the
  caller closes (half-open), subject to provider support.
- **Peer reset (RST) vs orderly EOF:** reset is delivered as a read **error** with category
  `KL_IO_RESET` (`KlIoStatus`), distinct from EOF. EOF = "peer finished sending, cleanly"; reset =
  "connection aborted, in-flight data may be lost." The caller can distinguish them and MUST NOT
  treat a reset as a clean end-of-message.
- **Backpressure interaction:** while paused (§4) no read callbacks fire and the backend does not
  accumulate (readiness: interest dropped; completion: no recv posted).

---

## 2. Write semantics

- **Buffer disposition: borrowed by default; the backend copies iff it must outlive the call.**
  - Readiness: `kl_stream_write(data,len)` attempts an immediate `send`; the accepted prefix is
    consumed from `data` inline and any remainder is **copied** into the stream's bounded write
    buffer (`KlDrain`) — so after the call returns the caller may free `data`. I.e. the caller's
    buffer need only remain valid *for the duration of the call*.
  - Completion: a posted write **copies or pins** the bytes for the op's lifetime (invariant 2) —
    this is backend-specific and NOT a single "copy is the default." Some backends copy (the
    completion TLS send path frees its ciphertext right after posting — there a reference would be
    a use-after-free); others pin (lwIP-raw legitimately retains live response segments). The
    *caller-facing* rule is the invariant, not the mechanism: **the caller's buffer need only remain
    valid for the duration of the `kl_stream_write*` call**; whether the backend copied or pinned is
    below the contract.
  - Net caller rule: **after `kl_stream_write*` returns, the caller may reuse/free its buffer.**
    (A future zero-copy `write_owned` that transfers ownership until a completion callback may be
    added as an explicit opt-in; it is not the default contract.)
- **Return value distinguishes four outcomes** (one enum, §4): `ACCEPTED` (all bytes taken, queued
  or sent), `WOULD_BLOCK`/high-water (buffer full — caller must wait for the writable/resume
  signal), `CLOSED` (write side gone: half-closed/peer-closed), `ERROR` (fatal, with `KlIoStatus`
  detail). This mirrors today's `KlIoStatus` + `KlDrain` cap semantics.
- **Partial writes:** always possible. `kl_stream_write` reports how many bytes were accepted; a
  scatter/gather `kl_stream_writev(iov,iovcnt)` accepts a prefix and reports the count. The caller
  resumes the remainder after the writable signal — OR relies on the stream's bounded buffer having
  queued it (ACCEPTED means "Keel owns delivery of these bytes now").
- **Ordering:** bytes are delivered to the peer in submission order. Queued (buffered) bytes are
  flushed before any later write is sent — a write while the buffer is non-empty appends (never
  reorders).
- **Concurrent writes:** not supported within one stream from multiple call sites racing; the
  single-loop-affinity invariant means writes are serialized by the caller on the loop thread.
  Overlapped completion writes are bounded to **at most one in flight** (today: `comp_stream_pump`
  posts one send, pumps the next on `KL_COMP_WRITE`) so ordering + buffer bounds hold.
- **Inline completion:** a write MAY complete synchronously (readiness fast path sends it all).
  The caller must not assume a later writable callback will fire if the write was fully ACCEPTED
  inline.
- **After write half-close (`shutdown_write`):** further `kl_stream_write*` returns `CLOSED`. Reads
  continue until peer EOF. This is FIN-sent, connection-still-readable.

---

## 3. writev / scatter-gather

`kl_stream_writev(iov, iovcnt)` has the same disposition + backpressure semantics as §2. The iov
*array* is consumed by the call (copied/flattened as needed — the caller's `KlIoVec[]` need not
outlive the call). Providers without native writev fall back to sequential sends below the
contract (already handled by the socket seam's `kl_sockdef_writev`); the caller sees identical
semantics.

---

## 4. Backpressure (bounded, one mechanism)

**Write side.** A single bounded buffer per stream (`KlDrain`, `max_size` cap). Outcomes are the
one enum every write returns:

| Outcome | Meaning | Caller action |
|---|---|---|
| `KL_STREAM_ACCEPTED` | all bytes taken (sent and/or queued within the cap) | continue |
| `KL_STREAM_WOULD_BLOCK` | high-water reached; not all bytes taken | stop writing; wait for the **writable** signal |
| `KL_STREAM_CLOSED` | write side is gone (half-closed / peer-closed) | stop; begin teardown |
| `KL_STREAM_ERROR` | fatal; `KlIoStatus`/`KlError` carries detail | begin teardown |

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

| Verb | Tier | Meaning | Effect |
|---|---|---|---|
| `kl_stream_close` | 1 | orderly full close | flush-then-close best effort (bounded); deferred release per §7 (`on_close` when ops confirmed retired) |
| `kl_stream_cancel(stream)` | 1 | cancel the stream's in-flight I/O | in-flight recv/send reach **cancelled** (once each); stream then closes per §7 |
| `kl_stream_shutdown_write` | **3 (OPTIONAL)** | orderly local finish of the *write* side | send FIN; further writes → `CLOSED`; reads continue until peer EOF. **New provider work: `KlSocketOps` has no `shutdown` op today** (POSIX/Winsock/lwIP/EFI impls or explicit UNSUPPORTED). Not in the initial contract. |
| `kl_stream_abort` | **3 (OPTIONAL)** | reset / hard close | send RST where supported; drop queued output; ops retire per §7 (**confirmed**, never "immediate" — must interoperate with EFI quarantine). Also new provider work (no `abort` op today). |

Note: per resolved decision #4, the first cut has **stream-level** cancel, not per-*write* cancel
(that needs the Tier-2 op-lifecycle). `connect`/`accept` are separately cancellable op handles
(§8) since they precede a live stream.

### 6.2 Which terminal callback fires for each event

| Event | Terminal delivered to caller |
|---|---|
| peer sent FIN (orderly) | read **EOF** (§1); write side may remain open (half-open) |
| peer sent RST | read **error** `KL_IO_RESET` (§1); outstanding writes → `ERROR`/`CLOSED` |
| local `shutdown_write` | no callback for the local side; subsequent writes synchronously return `CLOSED` |
| local `close` | no further callbacks after outstanding ops retire; then destruction (§7) |
| local `abort` | outstanding ops retire as **cancelled**/`ERROR`; no post-destruction callbacks |
| op cancelled | that op → **cancelled** (exactly once); no completed/failed for it |
| op deadline | that op → **timed-out** (exactly once); precedence per §6.3 |

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

Ownership is a UAF boundary, so it is normative even in this DRAFT (resolved decision #5):

- **The stream storage is caller-owned** — embedded in the caller's struct (e.g.
  `KlHttpConn { KlStream stream; … }`) or in a caller pool. Keel never `malloc`s the stream behind
  the caller's back (preserves Keel's preallocated-slot, allocation-free accept model).
- **`close` is deferred, not synchronous.** `kl_stream_close(stream)` *begins* teardown and returns;
  the stream enters a **draining** state: no further read/write callbacks are delivered, any
  outstanding op is cancelled, and the underlying handle/buffers are released only once the backend
  **confirms** every op retired (today: the EFI quarantine-on-failed-cancel + generation guard; the
  pool-free path NULL-guards every vtable call).
- **The caller learns teardown finished via exactly one `on_close(stream)` terminal.** Only after
  `on_close` fires may the caller free or reuse the (embedded/pooled) storage. **No callback of any
  kind fires after `on_close`.** (For an embedded stream, the parent must not be released until
  `on_close`.)
- `close`/`cancel` **inside a callback** are legal (recorded; applied after the callback returns).
- Parent (`KlListener`/HTTP server) destruction closes its streams under the same rules and waits
  for their `on_close` before releasing the pool.
- "Retirement is confirmed, not immediate" — `abort` (Tier 3) obeys the same deferred `on_close`;
  it never assumes synchronous op retirement (EFI cannot guarantee it).

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

## 11. Conformance checklist (drives Phase-8/10 tests)

A backend/provider conforms iff, observably through `KlStream`/`KlListener` only:
1-byte fragmented reads reassemble; large reads/writes stream within bounds; EOF ≠ reset; write
returns the four-way outcome; `writev` matches `write`; pause halts delivery + accumulation;
resume is idempotent; [Tier 3, when implemented] `shutdown_write` then writes → `CLOSED` while
reads continue; every op
reaches exactly one terminal; cancel/timeout/peer-close obey §6.3 precedence; close/cancel inside a
callback are safe; no callback after destruction; no UAF / double-callback / leak under ASan+UBSan;
identical results under readiness and completion and across TCP/Unix/lwIP where available.
