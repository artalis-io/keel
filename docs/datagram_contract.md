# Keel Datagram Transport Contract (FROZEN — Tier-1 normative)

**Date:** 2026-08-10
**Status:** **FROZEN / NORMATIVE (Tier 1), 2026-08-10.** The Tier-1 semantics below are the
committed contract; Phase A may begin. Changes to a frozen Tier-1 clause require a new review
round. Sibling of `docs/stream_contract.md`. Grounded in `docs/generic_datagram_audit.md` (the
approved implementation roadmap) and `docs/keel_datagram_ops_design.md` (the already-landed
data-plane provider vtable).

> **`KlDatagram` is a sibling of `KlStream`, not an extension of it.** A stream is an ordered
> byte sequence with partial writes, EOF, and byte-granular backpressure; a datagram is a
> sequence of discrete whole messages with atomic sends, no EOF, and packet-granular
> backpressure. They share the socket axis, the event axis, and the lifecycle *disciplines*
> (bounded queue, strict pause, cancel-once, confirmed detachment) — but not their data
> contract. This document freezes the **Tier-1** datagram semantics and the per-backend
> compatibility matrix. No extraction begins until the matrix (§10) is complete.

---

## Capability tiers (what is real *today* vs. new work)

- **Tier 1 (this contract)** — the neutral `KlDatagram` object + normative send/recv/pause/
  close semantics + capability reporting. Data plane already exists as `KlDatagramOps`
  (`include/keel/datagram.h`); the object + lifecycle is the new work (audit §8, Phase A/B).
- **Tier 2 (deferred)** — advanced provider capabilities that are opt-in and MUST NOT change
  the Tier-1 surface: mmsg batching, GSO/GRO, connected-mode, source-pinned send, multicast,
  ECN/TOS. Most already exist on `KlUdp`; they are re-exposed *as capabilities*, not baseline.
- **Out of contract** — QUIC (consumes `KlDatagram`), a variable-size byte-budget queue, and
  any assumption that a datagram socket is connected or fd-backed. See §11.

## Resolved decisions (frozen — the contract commits to these)

1. **Receive operations are serial; exactly one datagram per operation (all backends).** A
   *receive operation* is one synchronous provider `recv` invocation (readiness) or one posted
   completion recv (completion); each operation yields exactly one datagram observation.
   Operations happen **serially — never concurrently** (at most one in flight at any instant). A
   readiness READ interest is an *armed receive source*, **not** an in-flight operation: while
   armed, the object performs a bounded sequence of serial operations (drain ≤ N/tick for
   fairness), re-checking pause/close **after every callback** before attempting the next.
   **Tier-1 performs no `recvmmsg` batch delivery and no GRO coalescing or splitting** — those
   surface multiple already-received datagrams in one operation, which cannot coexist with the
   one-held-packet pause rule (§4): if a batch of 16 arrives and the first callback pauses, the
   other 15 are already received and cannot be un-received or held. Batching and GRO are
   deferred to Tier 2, whose contract must separately specify batch ownership, what happens when
   a callback pauses mid-batch, and the maximum held-packet count. Serial one-per-operation
   gives every backend identical pause, cancel, buffer-ownership, and detachment semantics.
2. **Bounded queue measured in packet slots, with separate inbound storage.** The outbound send
   queue is `slot_count` fixed slots, each holding bounded payload + peer/local + length +
   flags. The receive side has **one dedicated inbound slot** (payload buffer + metadata),
   *separate from the send slots* — a full send queue never starves the posted receive of stable
   storage. That same inbound slot becomes the *held* packet while paused (§4); no receive is
   re-armed until the held slot is delivered or discarded. Specify `slot_count`,
   `payload_capacity` (outbound), and the inbound payload capacity at init with overflow-safe
   validation of
   `slot_count × (payload_capacity + metadata_size) + (inbound_payload_capacity + metadata_size)`.
   **`TOO_LARGE`** = the packet exceeds a hard configured send-path capacity *permanently* — one
   slot (`payload_capacity`), or, under the `BOTH` policy, the whole byte budget (caller error);
   **`WOULD_BLOCK`** = admission refused *transiently* — no send slot, or the `BOTH` byte gate.
   Allocation-free steady state.
   **Send-queue policy (M1).** The default `SLOT` policy (`kl_datagram_init`) bounds admission by the
   slot COUNT only. `kl_datagram_init_ex(send_byte_budget > 0)` selects `BOTH` — admission is bounded
   by both the slot count AND a byte budget (bytes of queued+in-flight payload), a scalar admission
   gate over the same fixed slots (it never changes storage layout; zero-length packets are bounded by
   the slot count, so `count` — not the byte total — remains the sole drain predicate). There is **no**
   pure byte-only policy: an allocation-free queue is count-bounded, and zero-length datagrams make a
   byte budget unable to bound the packet count. Exact `KlUdp` byte-only/count-unbounded backpressure
   stays in the `KlUdp` compatibility wrapper's hot-path-allocating queue, not the Tier-1 core.
3. **Addresses are `KlSockAddr`, never `struct sockaddr`.** Matches the neutral `KlDatagramOps`
   vtable and the completion event fields. (The legacy `KlUdp` public API's `struct sockaddr`
   is a provider-config detail, not the neutral surface.)
4. **One datagram observation per callback; boundaries never merged or split; payload complete
   unless truncated.** Delivery is one datagram per callback; the payload is complete unless
   `KL_DGRAM_TRUNCATED` is set (decision #7). Tier-1 performs no GRO split or batch coalescing
   (decision #1).
5. **Atomic send.** A send is accepted completely or not at all; there is no partial datagram.
6. **`WOULD_BLOCK` takes no ownership and mutates no state.** The caller's buffer is untouched
   and may be retried or freed.
7. **Truncation is reported explicitly, never silently delivered as complete.** Tier-1 freezes
   **captured-prefix delivery**: an oversized datagram is delivered as the bytes that fit
   (`len` = captured prefix) with `KL_DGRAM_TRUNCATED` set — not a metadata-only notification.
   The callback distinguishes a genuinely short packet from a clipped one by the flag.
8. **Connected UDP is a configuration mode, not an assumption** baked into the abstraction.

---

## 0. Objects and invariants

### The object shape (sibling of `KlStream`)

`KlDatagram` is an opaque, embeddable transport object. Public header carries the boxed STABLE
banner + ABI policy verbatim from `stream.h:4-10`; layout lives in
`<keel/datagram_detail.h>` (opt-in, embedders recompile). Facets (SEND-queue, RECV, CLOSE) are
dormant until their `_init`, so a bare object is inert — mirroring the stream's base+facet
decomposition (`stream.h:12-20`).

```c
typedef struct KlDatagram KlDatagram;   /* opaque; layout in <keel/datagram_detail.h> */

/* Outbound message (borrowed by the caller until the send call returns). */
typedef struct {
    const void       *data;
    size_t            len;
    const KlSockAddr *peer;    /* destination; NULL iff connected-mode send */
    const KlSockAddr *local;   /* optional source/interface selection (pktinfo pin) */
    unsigned          flags;   /* provider hints (e.g. TOS override present) */
} KlDatagramMessage;

/* Inbound message (borrowed by the callback; valid only for the call — copy to retain). */
typedef struct {
    void       *data;
    size_t      len;           /* delivered length (may be < original if truncated) */
    KlSockAddr  peer;          /* packet source — ALWAYS filled (Tier 1) */
    KlSockAddr  local;         /* destination/interface, iff KL_DGRAM_HAS_LOCAL in flags */
    unsigned    flags;         /* KL_DGRAM_TRUNCATED | _HAS_LOCAL | _BROADCAST | _MULTICAST … */
} KlDatagramReceived;

typedef enum {
    KL_DATAGRAM_ACCEPTED = 0,  /* whole packet taken (sent or slot-queued) */
    KL_DATAGRAM_WOULD_BLOCK,   /* transient: no slot now, or the BOTH byte gate; NOTHING taken */
    KL_DATAGRAM_TOO_LARGE,     /* permanent: exceeds one slot, or the whole BOTH byte budget; no retry */
    KL_DATAGRAM_CLOSED,        /* closing/closed: no further sends */
    KL_DATAGRAM_UNSUPPORTED,   /* an explicitly-requested capability is unavailable; nothing sent */
    KL_DATAGRAM_ERROR          /* hard local/provider error (see last_error) */
} KlDatagramSendStatus;
```

### Invariants (normative)

1. **Serial receive.** At most one receive operation is outstanding per `KlDatagram` at any
   instant; operations are serial, never concurrent. A readiness READ interest is an *armed
   source*, not an operation (decision #1).
2. **Datagram observation.** Exactly one datagram observation per callback; boundaries are
   never merged or split; the payload is complete unless `KL_DGRAM_TRUNCATED` is set
   (decisions #4, #7).
3. **Atomic send.** `kl_datagram_send` accepts the whole packet or none (decision #5); the
   return status is total, never partial.
4. **No-ownership on refusal.** `WOULD_BLOCK`/`TOO_LARGE`/`UNSUPPORTED`/`CLOSED`/`ERROR` take no
   ownership and mutate no queue state (decision #6).
5. **Explicit truncation.** An oversized receive sets `KL_DGRAM_TRUNCATED`; the callback is
   never handed a truncated payload disguised as complete (decision #7).
6. **Confirmed detachment.** `on_close` fires exactly once, only after the in-flight recv AND
   all in-flight sends are physically retired (mirror `stream_close.c:5-7`).
7. **Source always present.** Every delivered packet carries `peer` (the source), because the
   canonical consumer (DNS) uses it as the anti-spoof filter on an unconnected socket
   (audit §2.4). `local` is present iff the pktinfo capability is active.
8. **Reuse only after detachment.** `kl_datagram_init` (memset-zero) is the reuse reset, legal
   only on a fully-detached object (mirror `stream.h:42-48`).
9. **Callback confinement.** A receive or completion callback may pause or close the object,
   but MUST NOT free or re-init it before `on_close` (invariants 6, 8). The readiness receive
   loop re-checks paused/closing after every callback and stops the drain immediately.

---

## 1. Send semantics (ATOMIC — decision #5)

- `kl_datagram_send(dg, const KlDatagramMessage *msg)` → `KlDatagramSendStatus`.
- **Immediate path:** if the send queue is empty, attempt the provider `send` synchronously.
  Full success → `ACCEPTED`. Transient would-block → copy the whole packet + destination into
  one slot, arm WRITE interest / post a send op → `ACCEPTED` (queued). *(Unlike today's
  `KlUdp`, `ACCEPTED` may mean "queued"; the caller learns backpressure from
  `kl_datagram_send_pending` / the queue-full `WOULD_BLOCK`, not from a hidden 0.)*
- **Ordering:** if anything is already queued, the new packet is queued behind it (FIFO); the
  socket is not attempted out of order (preserves `udp.c:141`).
- **`TOO_LARGE`:** `msg->len > payload_capacity` — permanent; the packet can never fit a slot. Under
  the `BOTH` policy (M1) it *also* covers `msg->len > send_byte_budget` (a packet larger than the whole
  budget can never be queued — refused after the readiness fast path's direct-send attempt, or upfront
  in completion mode, mirroring `KlUdp`). Distinct from `WOULD_BLOCK`. (Fixes the audit's lossy-status
  finding.)
- **`WOULD_BLOCK`:** all `slot_count` send slots occupied. Nothing copied, no counter bumped as
  a drop — the caller retries after `on_writable` (the full→non-full transition, §3).
- **Copy timing:** the caller's `msg->data` is borrowed only for the duration of the call
  (copied at enqueue, as `udp.c:102-103`); safe to reuse/free immediately after return.
- **Unsupported requested features fail; nothing is sent (§9).** A send that explicitly requests
  a capability the object lacks returns `KL_DATAGRAM_UNSUPPORTED` and transmits **nothing** —
  never a silent send with the feature dropped (a silent drop can send from the wrong interface
  or with the wrong network policy). Specifically: a non-NULL `msg->local` without
  `KL_DGRAM_CAP_SOURCE_PIN`; a per-packet TOS/ECN request without `KL_DGRAM_CAP_TOS`; a
  connected-mode send (`peer == NULL`) without `KL_DGRAM_CAP_CONNECTED`. Fallback is legal only
  for features the caller did **not** require.
- **Completion mode:** exactly one send batch (one packet, or one `sendmmsg` batch as a Tier-2
  capability) is submitted at a time; the next is pumped on `on_send_complete`. The ownership
  policy (copy vs reference) is captured *with the op* so a config change can't reinterpret a
  live op (mirror `stream_write.c:77`).

### Connected-mode send (Tier 2 capability, decision #8)

If the object is in connected mode, `msg->peer` MUST be NULL and the provider uses the
kernel-fixed peer. Unconnected mode requires a non-NULL `peer`. Connected-mode is reported via
`KL_DGRAM_CAP_CONNECTED` (§9); providers that lack it (lwIP-raw today rejects connected-send)
report its absence rather than silently misbehaving.

---

## 2. Receive semantics (one datagram per callback — decisions #4, #7)

- `kl_datagram_on_recv(dg, KlDatagramRecvFn cb, void *ud)` registers the delivery callback:
  `void cb(KlDatagram *dg, const KlDatagramReceived *rx, void *ud)`.
- **Serial receive operations, one datagram each** (decision #1). A receive *operation* is one
  provider `recv` (readiness) or one posted completion recv; each yields exactly one datagram.
  On readiness a single READ interest is an *armed source* — the object performs a bounded
  sequence of serial operations (drain ≤ N/tick, default 64, `udp_design.md:104-115`), never
  concurrently. On completion this is one posted recv op, re-posted after each delivery
  (self-re-arming — the DNS requirement, audit §7). Pause/resume mechanics are §4.
- **Buffer ownership:** the `KlDatagram` owns the recv storage — the **single dedicated inbound
  slot** (§3), universally, on every backend. `rx->data` is *borrowed* for the callback only
  (mirror `udp.h:60`). The callback must copy to retain. Because the object (not `KlUdp`) owns
  the storage and its lifetime is bound to detachment (§5, §6), no provider op can reference it
  after `on_close`. *(A provider that today receives into its own copy-ring — lwIP-raw — is
  non-conforming until adapted to the dedicated inbound slot; see §10.)*
- **`peer` is always filled** (invariant 7). `local` is filled iff `KL_DGRAM_HAS_LOCAL` is set
  in `rx->flags` (pktinfo capability active).
- **Truncation (decision #7):** if the datagram exceeded `inbound_payload_capacity`, deliver with
  `KL_DGRAM_TRUNCATED` set and `len` = bytes captured; the counter `kl_datagram_truncated` also
  increments. The callback can distinguish a genuine short packet from a clipped one. *(IOCP
  MUST parse `WSAMSG.dwFlags` to honor this — audit §6.)*
- **No batching / GRO in Tier 1 (deferred to Tier 2).** Tier-1 delivers exactly one datagram
  per operation. `recvmmsg` batch delivery and GRO coalescing/splitting are Tier-2 capabilities
  — they surface multiple already-received datagrams at once, which cannot honor the
  one-held-packet pause rule (§4). A Tier-2 batch contract must define batch ownership, what
  happens when a callback pauses mid-batch, and the maximum held-packet count.
- **Receive-loop rules.** The readiness drain re-checks paused/closing **after every callback**
  and stops the operation sequence immediately if either is set (it reads no further datagram
  that tick). A callback may pause or close the object, but MUST NOT free or re-init it before
  `on_close` (invariant 9).

---

## 3. Backpressure (bounded, packet slots — decision #2)

- Init specifies `slot_count` + `payload_capacity` (outbound send slots) **and** the inbound
  payload capacity; the object validates
  `slot_count × (payload_capacity + metadata_size) + (inbound_payload_capacity + metadata_size)`
  overflow-safely and preallocates **both** the send slots and the one dedicated inbound slot.
  Steady state is allocation-free.
- Each outbound slot holds: bounded payload storage, `peer`, `local`, `len`, `flags` (and
  per-packet TOS / gso hints as Tier-2 fields). The dedicated inbound slot holds one received
  datagram's payload + `peer`/`local`/`len`/`flags`.
- Send queue full → `WOULD_BLOCK` (transient, invariant 4).
- **Writable notification (frozen):** `on_writable(dg)` fires **once when a full queue first
  gains at least one free slot** (the full→non-full transition), so a producer that hit
  `WOULD_BLOCK` resumes promptly rather than stalling until the queue empties.
- **`on_drain(dg)` (optional, distinct):** fires on the non-empty→empty transition, mirroring
  the `KlDrain` distinction (`udp.c:245-246`). Both signals may be installed independently — use
  `on_writable` to resume a backpressured producer, `on_drain` to know the queue is fully idle.
- The receive side uses its **dedicated inbound slot**, separate from the send slots: at most
  one receive operation is outstanding (decision #1), and while paused that same inbound slot
  holds exactly one packet (§4). There is no unbounded receive accumulation, and a full send
  queue never blocks the receive.

---

## 4. Receive pause / resume (STRICT — mirror `stream_read.c`)

- `kl_datagram_pause(dg)` / `kl_datagram_resume(dg)`.
- **Strict pause:** post/arm no further receive operation. Readiness drops READ interest and
  cancels any pending readable (nothing completes). Completion leaves an *already-posted* recv
  physically in flight; when it completes it is **held in the dedicated inbound slot** (§3) as
  exactly one complete packet (`KlDatagramReceived` captured, not delivered) — never dropped,
  never a second op posted. **No receive is re-armed while a held slot is occupied.**
- **Resume:** deliver the single held packet exactly once (freeing the inbound slot), then
  re-arm one receive operation (mirror `stream_read.c:145-162`). At most one packet is ever
  held; a discard (on close) frees the slot without delivery.
- **Sync-completion safety:** set the in-flight sentinel before arming so a synchronous
  delivery sees it; convert nested re-arms into an iterative loop to bound the C stack under
  EFI/lwIP inline completion (mirror the `stream_read.c:49-79` arm trampoline).

---

## 5. Completion in-flight ownership (decision #1)

- At most one completion recv op posted, and at most one send batch outstanding, per object.
- **Lifetime ownership is the UAF defense — NOT a generation guard.** A completion cannot
  safely compare a generation against a `KlDatagram` that has already been freed; a stamp
  cannot replace lifetime ownership. Tier-1 requires that **confirmed detachment (invariant 6)
  guarantee no provider operation can reference the object or its buffers after `on_close`** —
  free and re-init are prohibited before detachment (invariant 8). Each completion backend
  satisfies this one of two ways:
  1. **Reap before release** — cancel every posted op and dequeue its completion before the
     object is released (IOCP's `iocp_quiesce_port_for_close` model, `event_iocp.c:942-976`); or
  2. **Backend-owned stable token** — key each op on a token that outlives every operation and
     lives in backend state (not the object), so a late completion lands on live backend state,
     never freed object memory.
  A generation/duplicate stamp MAY *additionally* reject a stale or duplicate completion **while
  the object is still alive**, but under strict detachment a completion after legal reuse is
  impossible, so the stamp is defensive, not the primary guarantee.
- **Status (Phase B.6, 2026-08-12): the backend-owned stable token is implemented on ALL FOUR
  completion backends** — pollcomp (B.6.1), io_uring (B.6.2), IOCP (B.6.3), lwIP-raw (B.6.4). The
  neutral token lives in `src/datagram_life.{h,c}` (a single-thread refcount + nullable target +
  `on_final`, allocated from the event-ctx allocator so it outlives `KlUdp`); `KlCompletionEvent`
  carries it in `life`. Each backend captures the recv buffer + flags at post and retains one token
  ref per posted op; the completion transfers that ref to the event (released after dispatch), and
  the op-free path releases a still-held ref on every drop-without-event path (post-failure unwind,
  silent cancel, loop teardown). No completion backend dereferences the `KlDatagram` (or its
  embedding `KlUdp`) after post. lwIP-raw keeps its per-slot copy-ring (already lifetime-safe) as
  the staging buffer — the token replaces only its legacy `ev->target` owner recovery; the ref lives
  in the udp slot (one per armed recv / pending send, transferred on drain, released on close).
  Replacing that copy-ring with a single dedicated inbound slot (§10 row) stays a deferred Tier-1
  cleanup, intentionally out of the token conversion.
- The recv buffer and any queued send slots are the object's own memory; a backend MUST NOT be
  handed a pointer it can dereference after the object detaches. `kl_datagram_*_free` refuses
  while an op is in flight (mirror `stream_write.c:160-168`).

---

## 6. Close, cancel-once, and confirmed detachment (invariant 6)

- **Verbs:** `kl_datagram_close_begin(dg)` (graceful: refuse new sends, flush queued sends,
  stop receiving) and `kl_datagram_cancel(dg)` (abortive: discard the queue, cancel in-flight
  ops now). Graceful→abortive escalation allowed (mirror `stream_close.c:95`).
- **Fully-retired predicate:** `recv_inflight == 0 && send_inflight == 0`; graceful
  additionally requires `kl_datagram_send_pending() == 0` (drain first). Abortive does not wait
  on the queue (queued slots are the object's memory, freed by the owner at `on_close`).
- **Cancel-once:** each in-flight op is cancel-requested at most once; the request flag is set
  before the hook so a synchronous retirement is safe (mirror `stream_close.c:110-121`).
- **Reentrancy:** an `in_close_cancel` depth counter defers finalize while a cancel hook is on
  the stack; the outermost frame re-attempts on unwind (mirror `stream_close.c:46-51`).
- **`on_close` fires exactly once, only after physical retirement** (invariant 6). State is set
  to CLOSED before the callback so a late retirement is a no-op. After `on_close`, no provider
  operation may reference the object or its buffers; free/re-init is legal only now
  (invariant 8), so a completion after legal reuse is impossible (§5).
- **Lifecycle-state enum + accessor** (mirror `KlStreamCloseState`):

```c
typedef enum {
    KL_DATAGRAM_STATE_OPEN = 0,
    KL_DATAGRAM_STATE_CLOSING,
    KL_DATAGRAM_STATE_CLOSED
} KlDatagramState;
KlDatagramState kl_datagram_state(const KlDatagram *dg);
int             kl_datagram_is_detached(const KlDatagram *dg);
```

This replaces `kl_udp_free`'s silent discard + the completion UAF window (audit §5).

---

## 7. Connected vs unconnected (decision #8)

- Unconnected is the baseline (DNS + `KlUdpServer` both need send-to-arbitrary-peer on an
  unconnected socket, audit §2.4). `peer` is required on send; `peer` is delivered on recv.
- Connected is a config mode (`KL_DGRAM_CAP_CONNECTED`): `peer` NULL on send, kernel-fixed peer,
  kernel inbound filtering. The abstraction never *assumes* connected; it reports the mode.

---

## 8. Source / destination metadata (invariant 7)

- `rx->peer` (source) is always filled — mandatory Tier-1 (the DNS anti-spoof filter,
  `dns_ns_index` `dns_resolver.c:691-703`).
- `rx->local` (destination / receiving interface) is filled iff pktinfo is active
  (`KL_DGRAM_CAP_PKTINFO`); `KlUdpServer` uses it for source-pinned replies on wildcard binds
  (`udp_server.c:80-82`).
- `msg->local` on send optionally pins the source/interface (source-pinned send,
  `KL_DGRAM_CAP_SOURCE_PIN`; today `kl_udp_send_to_from`).
- All addresses are `KlSockAddr` (decision #3).

---

## 9. Capability reporting (mirror `KlListener` state/caps conventions)

Capabilities are reported per-object (folding the provider's `KL_SOCK_CAP_DATAGRAM` +
`configure`-accepted `KL_DGRAM_RX_*` + provider op presence), queried via
`kl_datagram_caps(dg)`. **A behavior the caller explicitly requests but the object does not
support MUST fail — `KL_DATAGRAM_UNSUPPORTED` on send, or a config/init error at setup — and do
nothing.** It is never silently dropped (a silent drop can send from the wrong interface or with
the wrong network policy). Graceful degradation applies **only** to features the caller did not
require — e.g. recv `local` is simply absent when pktinfo is off.

| Capability flag | Meaning | Absent-behavior contract |
|---|---|---|
| `KL_DGRAM_CAP_PKTINFO` | `local` on recv (not required → graceful) | `local` simply absent on recv |
| `KL_DGRAM_CAP_SOURCE_PIN` | `msg->local` send pinning (required if requested) | non-NULL `msg->local` → send returns `UNSUPPORTED`, nothing sent |
| `KL_DGRAM_CAP_TOS` | ECN/DSCP marking (required if requested) | requested TOS/ECN → send returns `UNSUPPORTED` (per-packet) or config error (default); nothing sent |
| `KL_DGRAM_CAP_CONNECTED` | connected-mode send | connect config rejected with error; unconnected only |
| `KL_DGRAM_CAP_MULTICAST` | join/leave membership | multicast config + join/leave rejected with error |
| `KL_DGRAM_CAP_BROADCAST` | `SO_BROADCAST` sends | broadcast config rejected with error; a broadcast send returns `UNSUPPORTED` |
| `KL_DGRAM_CAP_BATCH_RECV` / `_BATCH_SEND` (Tier 2) | mmsg coalescing | not used in Tier 1 (one datagram per op) |
| `KL_DGRAM_CAP_GSO` / `_GRO` (Tier 2) | segmentation offload | not used in Tier 1 (no split/coalesce) |

**Truncation is not a capability.** Detecting and flagging truncation (invariant 5, decision #7)
is **mandatory for every Tier-1 implementation** — its absence is non-conformance, not a
runtime-degradable feature — so there is no `KL_DGRAM_CAP_TRUNCATION` to publish (a conforming
object would always set it). The last backend to lack it, IOCP, now parses `WSAMSG.dwFlags` /
`WSAEMSGSIZE` (Step 5, via the pure `dgram_recv_classify.h` helper), so every completion backend
is ✅ on the §10 truncation row.

---

## 10. Backend compatibility matrix (MUST be complete before extraction)

For each backend, every Tier-1 requirement must have a **defined implementation** or a
**documented limitation**. Legend: ✅ implemented · ▲ fallback/degraded · ✖ documented
limitation · ⚙ to build.

| Tier-1 requirement | POSIX rdy | Winsock rdy | pollcomp | io_uring | IOCP | lwIP-raw | EFI_UDP4 |
|---|---|---|---|---|---|---|---|
| Serial recv, one per op (armed source ≠ op) | ✅ (drain≤N/tick, serial) | ✅ | ✅ | ✅ | ✅ | ✅ (7A-5: single held slot — the former 16-entry ring was replaced; holds exactly ONE datagram + drops the second, matching KlDgramRecv one-in-flight/one-held; test T6 `raw_udp_test` + container lwIP-raw suite) | ✅ (1 self-rearming Rx token, 6.4b) |
| Atomic whole-packet send | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ (1 Tx token, 6.4b) |
| Packet-slot bounded **send** queue | ✅ (7B-6: KlDatagram fixed-slot over the readiness facade) | ✅ (7B-10: Winsock/WSAPoll readiness — the same backend-agnostic readiness adapter as POSIX; CI Windows `smoke-datagram` public-KlDatagram roundtrip green) | ✅ (7B-4: KlDatagram fixed-slot over the live facade) | ✅ (7B-5: live io_uring) | ✅ (7B-7: live Windows IOCP — CI windows-iocp `smoke-udp` + `smoke-datagram` green after the init-association fix) | ✅ (7B-8: live lwIP-raw, container ASan/UBSan/LSan) | ✅ (7B-9: host-mock ASan/UBSan + QEMU/OVMF public-KlDatagram e2e — round-trip + DETACHED close over EFI_UDP4, clean teardown udp_live=0/quar=0) |
| Strict pause (post no more recv) | ✅ (7B-6: drop-interest latch over the readiness facade) | ✅ (7B-10: Winsock/WSAPoll readiness — same readiness adapter as POSIX; CI Windows `smoke-datagram` green) | ✅ (7B-4: strict latch over the live facade) | ✅ (7B-5: live io_uring) | ✅ (7B-7: live Windows IOCP — CI windows-iocp `smoke-udp` + `smoke-datagram` green) | ✅ (7B-8: live lwIP-raw, container ASan/UBSan/LSan) | ✅ (7B-9: host-mock ASan/UBSan + QEMU/OVMF public-KlDatagram e2e over EFI_UDP4) |
| Cancel-once + confirmed detachment | ✅ (no async op) | ✅ | ✅ (stable token) | ✅ (stable token) | ✅ (stable token + dequeue-before-free) | ✅ (stable token + copy-ring/memset) | ✅ (stable token + Cancel + confirmed-retire-or-quarantine, 6.4b) |
| Lifetime: no op refs object after detach | ✅ (no async op) | ✅ | ✅ (stable token) | ✅ (stable token) | ✅ (stable token + dequeue-before-free) | ✅ (stable token + copy-ring) | ✅ (B.6 stable token, 6.4b) |
| `peer` (source) on every recv | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ (native `EFI_UDP4_SESSION_DATA`) |
| Truncation detected + flagged | ✅ (`MSG_TRUNC`) | ✅ | ✅ | ✅ | ✅ (`dwFlags`/`WSAEMSGSIZE`, Step 5) | ✅ (ring flag) | ✅ (6.4b) |
| Recv storage = object's dedicated inbound slot (no `KlUdp` deref) | ✅ | ✅ | ✅ (token pins slot) | ✅ (token pins slot) | ✅ (token pins slot) | ▲ (no `KlUdp` deref via token; copy-ring still stages into the inbound slot — single-slot swap deferred) | ✅ (token pins slot, 6.4b) |
| **Capabilities** | | | | | | | |
| pktinfo (`local`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✖ | ✅ (native dest in `EFI_UDP4_SESSION_DATA`) |
| multicast | ✅ | ✅ (no v4 by-index ✖) | ✅ | ✅(ctl) | ✅(ctl) | ✖ | ✖ |
| broadcast | ✅ | ✅ | ✅ | ✅(ctl) | ✅ | ✖ | ✖ |
| TOS/ECN | ✅ | ✅ | ✅ | ✅(ctl) | ✅(ctl) | ✖ | ✖ |
| mmsg batch (Tier 2) | ✅(Linux) | ✖ | ✖ | ✖ | ✖ | ✖ | ✖ |
| GSO / GRO (Tier 2) | ✅(Linux) | ✖ | ▲ | ✅ GRO / ✖ GSO | ✖ | ✖ | ✖ |
| source-pin send | ✅ | ✅ | ✅ | ▲ (sync seam) | ▲ (sync seam) | ✖ | ✖ |
| connected mode | ✅ | ✅ | ✅ | ✅ | ✅ | ✖ (rejected) | ✖ (unconnected only) |

**Reading the matrix:** lifetime ownership is now ✅ across ALL FIVE completion backends
(pollcomp/io_uring/IOCP/lwIP-raw **and EFI_UDP4**) via the backend-owned stable token (**Phase B.6
complete**, §5 status). **EFI_UDP4 is a real provider** now (`socket_efi_udp4.c` + `event_efi.c`, 6.4b;
implemented + host-mock-tested + firmware-proven end-to-end in 6.4c — the seed `dns_uefi.c` token
machine was retired, audit §6): its serial recv, atomic send, cancel-once/confirmed-retirement +
quarantine, stable-token lifetime, native `peer`/`local` (from `EFI_UDP4_SESSION_DATA`), truncation,
and dedicated inbound slot are all ✅. The packet-slot bounded **send** queue and strict pause are now
✅ across EVERY supported backend via the public KlDatagram facade (7B-4 pollcomp, 7B-5 io_uring, 7B-6
POSIX readiness, 7B-7 IOCP, 7B-8 lwIP-raw, 7B-9 EFI_UDP4, 7B-10 Winsock/WSAPoll readiness) — the
`kl_datagram_*` contract is **STABLE** as of 7B-10 (see the `<keel/datagram.h>` banner). **Every Tier-1
requirement row is ✅ for every supported backend** — including lwIP-raw's serial receive: 7A-5 replaced
its former 16-entry receive ring with a single held slot (holds exactly one datagram, drops the second —
test T6 `raw_udp_test`), so it matches the one-in-flight/one-held contract exactly. No ⚙ ("to build")
cell remains in any supported-backend column, consistent with the no-caveat STABLE banner. The remaining
non-✅ cells are `▲` (implemented but degraded, with a documented bound) or `✖` (a documented capability
limitation the consumer queries via caps, §9, and degrades around) — neither is a contract gap. The ✖
capability cells are *documented
limitations* — consumers query caps (§9) and degrade. lwIP-raw's object-owned-buffer row is ▲: the
token removed its `KlUdp` deref, but it still stages through its copy-ring before the machine copies
into the dedicated inbound slot — replacing the copy-ring with a single inbound slot is a deferred
Tier-1 cleanup (the copy-ring was deliberately preserved).

---

## 11. Explicitly out of contract

- **QUIC / HTTP-3.** QUIC consumes `KlDatagram` (it needs sessions, streams, timers, congestion
  control, packet protection); it MUST NOT shape the baseline. The Tier-1 additive knobs
  (source-addr on wildcard binds, GSO/GRO, ECN, batching) are the declared QUIC foundation
  (`udp_design.md:220-236`) but remain capabilities, not baseline.
- **Variable-size byte-budget queue.** A future optional capability atop the packet-slot model
  (decision #2), not Tier 1.
- **DoT / TCP fallback / any stream transport.** The DNS TCP fallback + `(fd, KlTls*)` DoT hook
  is a **byte-stream** helper (audit §2.4) — it belongs to `KlStream`/the socket seam, never
  folded into `KlDatagram`.
- **Freestanding (UDP-only) DNS build.** When the built-in resolver (`src/dns_resolver.c`) is
  compiled `-DKEEL_FREESTANDING` (the datagram/DNS freestanding archive that a bare EFI_UDP4
  consumer links — 6.4a-2), it performs **UDP-only Do53 against an explicitly configured
  nameserver**, with three *documented, consumer-visible* limitations vs the hosted build:
  1. **Explicit nameserver required** — there is no `resolv.conf` discovery; the caller MUST set
     `KlDnsResolverConfig.nameserver` (a numeric NS address). Creation fails otherwise.
  2. **No `/etc/hosts` lookup** — there is no filesystem, so the hosts-file shortcut is absent
     (literal-IP and `localhost` shortcuts still work — they need no filesystem).
  3. **No RFC 7766 TCP recovery on truncation** — a truncated (TC) response cannot be recovered
     over TCP, so it **fails that query leg promptly and clearly** (the resolution completes with
     `KL_ERR_DNS`), rather than opening a TCP socket or silently waiting for the leg timeout.
     Runtime-proven by `tests/freestanding_dns_harness.c` (`make freestanding-dns-harness`).
  The UDP query engine itself is otherwise identical (dual-family A+AAAA, 0x20, EDNS0, cookies,
  the bounds-safe parser). The DoT/TCP hook above is the stream helper these limitations drop.
- **Connected-socket assumptions.** Connected is a mode (§7), never assumed.
- **fd exposure.** `kl_udp_server_fd`-style raw-handle leaks are provider-specific; providers
  without fds (lwIP/EFI) report the absence rather than fabricate one.

---

## 12. Conformance gates (drive the Phase-B tests)

A backend conforms to Tier-1 iff, over that backend (readiness natively; completion via
`pollcomp`/`iouring`/`iocp`; lwIP-raw + EFI via their harnesses):

1. **Atomic send** — a `payload_capacity`-fits packet returns `ACCEPTED` and arrives whole; an
   over-capacity packet returns `TOO_LARGE` and is never partially sent.
2. **Backpressure** — filling `slot_count` send slots returns `WOULD_BLOCK` with no state
   change; retiring a send frees a slot and `on_writable` fires on the full→non-full transition
   (`on_drain` only on non-empty→empty); no packet is silently dropped under backpressure.
3. **Serial recv, one per operation** — at most one receive operation is in flight at any
   instant (a readiness interest is an armed source, not an op); a burst is delivered
   one-datagram-per-callback in order per source, pause/close re-checked between operations.
4. **Strict pause** — after `pause`, no further recv is posted; a completion posted before pause
   is held and delivered exactly once on `resume`.
5. **Truncation** — an oversized datagram is delivered with `KL_DGRAM_TRUNCATED` and clipped
   `len`, never as a complete packet (explicit IOCP test).
6. **Source present** — every delivery carries `peer`; an unconnected socket rejects a spoofed
   source at the consumer (DNS-style filter test).
7. **Close/detachment** — `close_begin` drains then detaches; `cancel` retires in-flight ops;
   `on_close` fires exactly once after physical retirement; no leak/UAF under ASan+LSan with an
   op in flight at close (the audit's completion-backend hazard).
8. **Lifetime** — no provider operation references the object or its buffers after `on_close`;
   with an op in flight at close, teardown reaps/cancels it before releasing the object
   (ASan+LSan clean). A completion after legal reuse is impossible under strict detachment.
9. **Capability honesty** — `kl_datagram_caps` matches actual behavior. A **required** feature
   the caller explicitly requests but the object lacks MUST fail and send nothing —
   `KL_DATAGRAM_UNSUPPORTED` on send (non-NULL `msg->local` without source-pin; per-packet
   TOS/ECN without TOS) or a configuration/init error at setup (connected-mode, multicast/
   broadcast config without support). Only **optional receive metadata** may be absent (e.g.
   `local` when pktinfo is off). A capability is never silently ignored.

The stream conformance suite (`stream_contract.md` §11, tests `test_stream*`,
`test_transport_public`) is the template; the datagram suite adds packet-integrity, truncation,
and source-presence gates.

---

## 13. Roadmap (follow the proven Phase A/B sequence)

Per `docs/generic_datagram_audit.md` §8: **internal carve** (neutral `KlDatagram` out of
`KlUdp`; re-type the completion identity off `KlUdp*`) → **packet-slot bounded queue** →
**strict receive pause** → **serial receive + lifetime ownership (no stale ref)** →
**close/cancel-once/confirmed detachment** → **live-wire UDP + DNS + `KlUdpServer`**
(behavior-preserving) →
**public stabilization** (STABLE banner + `<keel/datagram_detail.h>` opt-in ABI split).

Do not begin the carve until §10 is complete for every backend — every Tier-1 requirement has a
defined implementation (⚙ scoped) or a documented limitation (✖).

---

## Architectural placement

```
Protocols / applications
    ├── KlStream       ordered byte transport          (stream_contract.md)
    ├── KlListener     stream admission
    ├── KlConnectOp    outbound stream establishment
    └── KlDatagram     atomic message transport         (this contract)
             │
             └── UDP / Unix datagram / lwIP UDP / EFI UDP   (KlDatagramOps providers)
```

`KlDatagram` completes the transport layer as a sibling of `KlStream`: same axes, same
lifecycle disciplines, a deliberately different data contract. QUIC, when it comes, sits
*above* `KlDatagram` as a consumer.
