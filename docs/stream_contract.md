# Keel Stream Transport Contract (normative)

**Date:** 2026-08-08
**Status:** normative contract for the generic `KlStream` / `KlListener` layer. Written before
migrating HTTP code (per the Phase-1 audit, `docs/generic_transport_audit.md`). It describes the
semantics BOTH the readiness and completion implementations, and EVERY transport provider
(TCP / Unix / lwIP / EFI / future pipes / vsock), must present identically. Where possible it
codifies behavior Keel already implements today so migration is retype-not-redesign.

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
   (§5.6).
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
  needs them longer must copy (the HTTP parser copies what it retains; body readers copy into
  their own storage). After the callback returns, Keel may reuse/overwrite the buffer.
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
  - Completion: a posted write **copies or pins** the bytes for the op's lifetime (invariant 2).
    The completion contract is **copy-required unless the caller guarantees validity to
    completion** — Keel's default is copy (proven necessary: the completion TLS send path frees its
    ciphertext right after posting; a reference would be a use-after-free).
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

- `kl_stream_pause(stream)` — stop read-delivery. While paused:
  - readiness backends drop READ interest (no readable dispatch);
  - completion backends do **not** post a new recv (at most the one already-outstanding recv
    completes, its bytes delivered, then no re-post);
  - Keel does not accumulate beyond that one in-flight buffer.
- `kl_stream_resume(stream)` — re-enable delivery: readiness re-arms READ; completion re-posts a
  recv. **Idempotent and safe to call multiple times** (resume-when-not-paused is a no-op;
  double-resume does not double-post). Pause is likewise idempotent.
- This is the existing `read_paused` mechanism, renamed off the HTTP request onto the stream.

---

## 6. Close, half-close, and terminal outcomes

### 6.1 The close/terminal verbs

| Verb | Meaning | Effect |
|---|---|---|
| `kl_stream_shutdown_write` | orderly local finish of the *write* side | send FIN; further writes → `CLOSED`; reads continue until peer EOF |
| `kl_stream_close` | orderly full close | flush-then-close best effort (bounded); release the stream after outstanding ops retire |
| `kl_stream_abort` | reset / hard close | send RST where supported; drop queued output; retire ops immediately |
| `kl_stream_cancel(op)` | cancel one in-flight op | that op reaches the **cancelled** terminal; the stream stays usable |

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

## 7. Lifecycle / destruction (invariants 1, 3, 7)

- Destroying a `KlStream` with outstanding completion ops: the stream enters a **draining**
  state — no new callbacks are delivered to the caller, outstanding ops are cancelled, and the
  underlying handle/buffers are released only once the backend confirms every op retired (today:
  the EFI provider's quarantine-on-failed-cancel + generation guard; the pool free path guards
  every vtable call). **No callback fires after destruction begins.**
- `close`/`abort` inside a callback is legal (recorded; applied after the callback returns).
- `cancel` inside a callback is legal.
- Parent (`KlListener`/`KlServer`) destruction closes its streams under the same rules.

---

## 8. Connect (client) and accept (listener)

- **Connect:** `kl_stream_connect(endpoint, deadline, done)` is an async op with the §6.3 terminal
  set: **connected** | **refused** (`KL_IO`/`KL_ERR_CONNECT`) | **timed-out** | **cancelled**.
  Cancellation racing a successful connect resolves per §6.3 (completion wins if it already
  happened). The client's Happy-Eyeballs address racing is an implementation of this op, invisible
  to the caller.
- **Accept:** `kl_listener_accept` yields the next connected `KlStream` (readiness: listener
  readable → `accept`; completion: posted accept → `KL_COMP_ACCEPT`), identical to the caller.
  Accept is **capacity-gated**: a listener never accepts beyond the caller's configured capacity
  (today's backpressure-floor + the EFI capacity-gated arming). `cancel` before/racing an accept,
  and listener close with a pending accept, follow §6/§7.

---

## 9. Endpoints (§0)

`KlEndpoint` = `KlSockAddr` (already neutral: IPv4/IPv6/Unix) + a transport-kind tag so
`connect`/`bind`/`listen` dispatch to the correct provider. Non-socket kinds (future pipe/vsock)
carry their own address payload and are NOT forced into `sockaddr`. `local_endpoint`/`peer_endpoint`
return a `KlEndpoint`; a caller reads host/port/path via the existing `kl_sockaddr_*` accessors.

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
resume is idempotent; `shutdown_write` then writes → `CLOSED` while reads continue; every op
reaches exactly one terminal; cancel/timeout/peer-close obey §6.3 precedence; close/cancel inside a
callback are safe; no callback after destruction; no UAF / double-callback / leak under ASan+UBSan;
identical results under readiness and completion and across TCP/Unix/lwIP where available.
