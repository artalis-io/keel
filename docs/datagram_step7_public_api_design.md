# Step 7 — Public `KlDatagram` API: Design Freeze

**Status:** DESIGN FREEZE — no code. For review before any implementation (per the datagram-consolidation
cadence). Grounds every decision in the frozen Tier-1 contract (`docs/datagram_contract.md`) and the
already-built machine (`src/datagram_{slots,send,recv,close,life}.h`, `include/keel/datagram{,_detail}.h`).

Scope: the **public stabilization** step of the roadmap (`datagram_contract.md` §13) — expose a
consumer-facing `KlDatagram` surface + the `<keel/datagram_detail.h>` opt-in ABI split + a STABLE
banner, over the Tier-1 machine.

---

## 0. Current state (what Step 7 inherits) and the two coupled deliverables

The Tier-1 machine exists as **standalone, unit-tested TUs** with the exact semantics the eight
topics below require, but they are only PARTLY live-wired:

| Component | File | Built? | Live-wired into `KlDatagram`/`KlUdp`? |
|---|---|---|---|
| B.6 stable token (`KlDgramLife`) | `datagram_life.h` | ✅ | ✅ (`KlDatagram.rx_life`, all completion backends) |
| Serial recv + strict pause/resume + one-held-packet (`KlDgramRecv`) | `datagram_recv.h` | ✅ | ✅ (`KlUdp.rx`, wired 6.1) — **recv is live** |
| Fixed-slot pool (`KlDgramSlots`) | `datagram_slots.h` | ✅ | ✖ — `KlDatagram` still uses the **legacy `q_head/q_tail` byte-FIFO** |
| Single-flight slot send queue (`KlDgramSend`) | `datagram_send.h` | ✅ | ✖ (`§10` "packet-slot send queue" = ⚙ for every backend) |
| Confirmed-detachment close coordinator (`KlDgramClose`) | `datagram_close.h` | ✅ | ✖ |
| Strict pause row | — | ✅ (in `KlDgramRecv`) | ✖ (`§10` "strict pause" = ⚙ — the *interest-drop latch* isn't wired at the backend seam) |

So Step 7 is **two coupled deliverables**, proposed as sub-steps:

- **7A — Live-wire the Tier-1 machine** into `KlDatagram` (replace the legacy `q_head/q_tail` send queue
  with `KlDgramSlots`+`KlDgramSend`; wire `KlDgramClose`; wire the strict-pause interest-drop at each
  backend seam). This flips the `§10` "packet-slot send queue" + "strict pause" rows ⚙ → ✅ and makes
  `KlUdp` run over the new machine. No public surface change yet.
- **7B — Public surface** — the `kl_datagram_*` API + `<keel/datagram_detail.h>` opt-in layout + STABLE
  banner, plus the validation suite.

Each sub-step lands as its own reviewed commit. 7A is a pure internal refactor (KlUdp behavior
identical, its tests unchanged); 7B is additive.

---

## 1. Public `KlDatagram` API and ownership

**Ownership model.** `KlDatagram` is a **caller-owned value type** (stack- or embed-allocatable, like
`KlUdp`/`KlConn`), single-threaded, driven on the event-loop thread. Its full layout lives in
`<keel/datagram_detail.h>` (opt-in — an embedder that wants to embed it by value `#include`s the detail
header and recompiles on layout change); the umbrella `<keel/datagram.h>` keeps `KlDatagram` opaque for
consumers that only hold a pointer. This is the same **STABLE-with-opt-in-ABI** split the contract §4
mandates and that `datagram.h`/`datagram_detail.h` already scaffold.

**The surface** (new `kl_datagram_*`, thin facade over the existing `kl_dgram_*` machine):

```c
/* lifecycle */
int  kl_datagram_init(KlDatagram *dg, const KlDatagramConfig *cfg);   /* wire slots+send+recv+close+life */
int  kl_datagram_close_begin(KlDatagram *dg);     /* graceful */
int  kl_datagram_close_cancel(KlDatagram *dg);    /* abortive */
int  kl_datagram_free(KlDatagram *dg);            /* refuses before detachment; -1 if still attached */

/* data plane */
KlDatagramSendStatus kl_datagram_send(KlDatagram *dg, const KlDatagramMessage *m);
int  kl_datagram_recv_start(KlDatagram *dg, KlDatagramRecvFn on_recv, void *ud);
void kl_datagram_pause(KlDatagram *dg);
int  kl_datagram_resume(KlDatagram *dg);
void kl_datagram_recv_stop(KlDatagram *dg);

/* edges + queries */
void   kl_datagram_on_writable(KlDatagram *dg, KlDatagramWritableFn cb, void *ud);
void   kl_datagram_on_drain(KlDatagram *dg, KlDatagramDrainFn cb, void *ud);
void   kl_datagram_on_close(KlDatagram *dg, KlDatagramCloseFn cb, void *ud);
unsigned         kl_datagram_caps(const KlDatagram *dg);         /* KL_DGRAM_CAP_* bitmask */
KlDgramCloseState kl_datagram_close_state(const KlDatagram *dg); /* OPEN/CLOSING/CLOSED */
size_t   kl_datagram_send_queued(const KlDatagram *dg);
size_t   kl_datagram_send_inflight(const KlDatagram *dg);
uint64_t kl_datagram_dropped(const KlDatagram *dg);
uint64_t kl_datagram_truncated(const KlDatagram *dg);
KlError  kl_datagram_last_error(const KlDatagram *dg);
```

`KlDatagramConfig` carries the **borrowed** dependencies + the fixed sizing, resolving the layering
(see §6): `{ KlEventCtx *ctx; KlAllocator *alloc; const KlSocketProvider *sockets; KlSocketHandle fd;
size_t send_slots; size_t send_slot_cap; size_t recv_cap; unsigned want_caps; }`. The `fd` is
**already `socket()`'d, `configure()`'d, and `bound`** by the caller through `sockets->dgram` (KlUdp
does this today); `KlDatagram` owns it from `init` to detachment and closes it via `sockets->close` as
the terminal act of the close machine. Sockopt/multicast/TOS/GSO stay OUT of this core surface (§6).

**Reuse.** `kl_datagram_init` (memset-zero precondition) is the reuse reset, legal **only on a
fully-detached** object (invariant 8).

---

## 2. Fixed-slot send: capacity, FIFO ordering, backpressure

Exactly the frozen `KlDgramSend`+`KlDgramSlots` contract, surfaced verbatim:

- **Capacity.** `send_slots` fixed outbound slots × `send_slot_cap` bytes, allocated **once** at
  `init` (object-owned, in the outbound pool block); acquire/release never touch the allocator.
- **FIFO ordering.** The occupied slots form a FIFO ring by submission order (the LIFO free-list only
  feeds allocation; the ring orders sends). A freed hole never lets a newer send jump an older one.
- **Single-flight (Tier-1).** At most one send op is in flight; the rest wait queued. Retirement
  (`kl_dgram_send_on_complete` in completion mode / `kl_dgram_send_flush` on writable in readiness)
  pumps the next FIFO slot.
- **Atomic accept.** `kl_datagram_send` copies the whole datagram (payload + dest/src/tos) before
  returning and returns a **total** status — never partial (invariant 3):
  `ACCEPTED | WOULD_BLOCK | TOO_LARGE | UNSUPPORTED | CLOSED | ERROR` (the existing
  `KlDatagramSendStatus`). On any non-`ACCEPTED` status it takes **no ownership** and mutates no queue
  (invariant 4).
- **Backpressure.** All slots occupied → `WOULD_BLOCK`. A full→non-full edge fires the registered
  `on_writable` (reentrant, non-destructive). A non-empty→empty edge fires `on_drain` (destructive
  tail). Over-cap accounting increments `dropped` only where a *policy* drops (KlUdp's cap wrapper);
  the core `kl_datagram_send` never silently drops — it returns `WOULD_BLOCK`.
- **`KlDatagramMessage`** (borrowed): `{ data, len, peer(NULL=connected), local(NULL=no source-pin),
  tos(-1=none), flags }`.

---

## 3. Receive pause/resume and one-held-packet semantics

Exactly the frozen `KlDgramRecv` contract:

- **Serial receive** (invariant 1): at most one recv op outstanding; a readiness READ interest is an
  *armed source*, not an operation.
- **One-held-packet.** In **completion** mode a posted recv that arrives while paused is *held* (one
  datagram, full metadata snapshot) and delivered exactly once on `resume`. In **readiness** mode
  pause drops READ interest so nothing completes; resume re-arms.
- **`kl_datagram_pause`** — strict pause, idempotent, callable from inside the deliver callback.
  **`kl_datagram_resume`** — deliver the held datagram (if any) exactly once, then re-arm.
  **`kl_datagram_recv_stop`** — logical stop: no future delivery/re-arm; a held datagram is discarded;
  a physically-outstanding completion retires (dropped) on arrival.
- **Delivery contract** (invariants 2, 5, 7): one datagram per callback; `peer` (source) **mandatory**
  (a completion/pull with no peer is a provider-contract violation → fail safely, no callback, error
  state); `local` present iff `KL_DGRAM_HAS_LOCAL`; oversized → delivered once with `KL_DGRAM_TRUNCATED`
  + `truncated` counter, never disguised as complete.
- **Callback confinement** (invariant 9): the deliver callback may pause or close the object but MUST
  NOT free/re-init before `on_close`; the readiness drain re-checks paused/closing after every callback
  and stops immediately.
- `KlDatagramRecvFn = void(*)(void *ud, const void *data, size_t len, const KlSockAddr *peer, const
  KlSockAddr *local, unsigned flags)`.

---

## 4. Confirmed-detachment close state machine

Exactly the frozen `KlDgramClose` coordinator (invariant 6):

- **States** `KL_DGRAM_CLOSE_{OPEN, CLOSING, CLOSED}` via `kl_datagram_close_state`.
- **`kl_datagram_close_begin`** (graceful): refuse new sends (`send.closing`), stop receiving, drain
  the queued output; may escalate to abortive.
  **`kl_datagram_close_cancel`** (abortive): discard queued-but-unsubmitted datagrams, request cancel
  of the outstanding recv/send op (each op cancel-requested **at most once**).
- **Detachment predicate:** `on_close` fires **exactly once**, only after `recv_inflight == 0 &&
  send_inflight == 0`; graceful additionally requires `send_pending == 0` (drain first) OR the queue
  becomes undrainable via sticky error (a failure never wedges close). `on_close` is a **destructive
  tail** — state set to `CLOSED` first, no self-access after (the callback may free/teardown).
- **Reentrancy.** The `busy`/`on_activity(±1)` handshake around every send/recv public op defers
  detachment while a cancel hook or callback is on the stack; the outermost frame re-attempts on
  unwind. This is why `on_drain`/`on_close` are destructive tails.
- **`kl_datagram_free`** refuses (-1) before confirmed detachment; after detachment it tears down
  send + recv + coordinator + closes the fd, and the object is memset-zero-reusable (invariant 8).

---

## 5. B.6 stable-token and quarantine interaction (the crux)

The close machine (§4) tracks **logical** retirement; the `KlDgramLife` token (§0) tracks **physical**
storage lifetime. Step 7 must keep these aligned so a late completion never touches freed memory.

- **Storage split (KEY DECISION D-STORAGE).** The **receive inbound storage** (the one-held-packet
  slot + recv machine) is owned by the **life token** (`on_final` frees it), allocated from the
  **event-ctx allocator** so it outlives the `KlDatagram`/`KlUdp` wrapper — exactly as 6.1 wired it.
  The **outbound send-slot pool** is owned by the **`KlDatagram`** (object-owned, freed at
  detachment once `send_inflight == 0`). Rationale: only the *recv* op can be left dangling by a
  firmware/kernel that hasn't returned the buffer; the send pool is safe to free once the single
  in-flight send retires. This means `KlDgramSlots` is used in **two instances** per object — an
  object-owned outbound pool and a life-token-owned inbound slot — NOT one shared block. (Contrast the
  standalone `datagram_slots.h`, which today allocates both in one block; 7A must split them.)
- **Happy detachment.** `on_close` drops the owner life ref (`kl_dgram_life_mark_dead` + `release`);
  the FINAL release runs `on_final`, freeing the inbound storage; `live_count == 0`.
- **Quarantine (EFI unconfirmed cancel).** A backend that cannot confirm a cancel retains its op's life
  ref **forever** (fail-closed to ExitBootServices). Consequence, frozen here: `on_close` **still
  fires** (the object logically detaches — `recv_inflight` reaches 0 from the object's view once the op
  is cancel-requested and will never deliver), the `KlDatagram` struct is safely reusable/freeable,
  **but the inbound storage is NOT reclaimed** — the abandoned life ref pins it until EBS. Because that
  storage is life-token-owned (not embedded in `KlDatagram`), freeing/reusing the object after
  `on_close` is memory-safe; the leak is bounded, deliberate, and provider-local. The public contract
  states: *after `on_close`, the object is detached and reusable; on a quarantining backend the receive
  buffer for the unconfirmed op remains pinned until process/firmware teardown.*
- A UDP completion with `life == NULL` yields a NULL owner and is dropped — no object deref. No
  completion backend dereferences `KlDatagram` after post.

---

## 6. Compatibility boundary with legacy `KlUdp`

**`KlUdp` is retained, unchanged in surface, and re-based onto `KlDatagram` (source- and behavior-
compatible).** Decision (D-COMPAT):

- `KlUdp` remains the **ergonomic UDP surface**: it owns socket creation + `configure()` (sockopts:
  reuse, buffers, broadcast, multicast, TOS, GSO/GRO, pktinfo), embeds a `KlDatagram` (`dg`), and
  forwards `kl_udp_send*`/`recv`/`pause`/`free` to `kl_datagram_*`. Every existing `kl_udp_*` function
  keeps its signature and semantics; `KlUdpServer` (6.2) and the DNS resolver (6.3) are untouched.
- `KlDatagram` is the **raw Tier-1 primitive**: send/recv/pause/close over a prepared `(provider, fd)`,
  no sockopt surface. New consumers that need the bare confirmed-detachment/fixed-slot contract (a
  future QUIC/HTTP-3 endpoint, a custom protocol) use `KlDatagram` directly; consumers that want the
  convenience layer use `KlUdp`.
- The multicast/TOS/GSO/pktinfo **capabilities** are configured through `KlUdp` (or a raw caller's own
  `sockets->dgram->{configure,set_tos,mcast_membership}` calls) — they are provider/socket-option
  concerns, deliberately **not** on the core `KlDatagram` send/recv/close surface, matching
  `datagram.h`'s "the provider holds no UDP machine state" split.
- 7A makes `KlUdp` run over the new machine with **no test changes** (behavior identical); it is the
  regression proof that the wiring is faithful.

---

## 7. Backend capability and degradation rules

- **Query:** `kl_datagram_caps(dg)` returns a `KL_DGRAM_CAP_*` bitmask sourced from the socket
  provider's declared datagram capabilities (`KL_DGRAM_CAP_{PKTINFO, SOURCE_PIN, TOS, CONNECTED,
  MULTICAST, BROADCAST}`; the `_BATCH_*`/`GSO`/`GRO` Tier-2 flags remain reserved).
- **Required-if-requested MUST fail** (invariant, §9): a non-NULL `msg->local` without
  `SOURCE_PIN`, or per-packet TOS without `TOS`, returns `KL_DATAGRAM_UNSUPPORTED` from `send` and
  sends nothing; connected-mode / multicast / broadcast without support fail at config/init. Never
  silently dropped.
- **Optional recv metadata degrades silently:** `local` is simply absent when `PKTINFO` is off
  (`local == NULL` at delivery). Truncation is **not** a capability — mandatory for every backend.
- **Per-backend truth** is the `datagram_contract.md` §10 matrix; the public API surfaces exactly what
  a backend reports and refuses the rest. The matrix's remaining ⚙ (shared packet-slot send-queue +
  strict-pause rows, plus lwIP-raw's serial-recv one-held-slot rework) are the 7A wiring targets —
  they flip to ✅ per backend as 7A lands, and Step 7 is not "done" until they are ✅ on the completion
  backends it validates (§8).

---

## 8. Deterministic unit, sanitizer, backend, and teardown validation

A new `tests/test_datagram_public.c` (the datagram analogue of `test_transport_public`), plus the
existing standalone facet tests, run as a matrix:

- **Facets (deterministic, over a scripted in-test provider double):**
  - *Send queue* — FIFO ordering across a hole-reuse sequence; single-flight; `WOULD_BLOCK` at
    capacity; `on_writable` full→non-full edge; `on_drain` non-empty→empty edge; `TOO_LARGE`;
    `UNSUPPORTED` (required cap absent); `CLOSED` after `close_begin`; no-ownership-on-refusal.
  - *Receive* — one datagram per callback; `peer` mandatory (violation → error, no callback); pause
    holds exactly one (completion) / drops interest (readiness); resume delivers once then re-arms;
    stop discards held; truncation flagged + counted; zero-length ≠ failure.
  - *Close* — graceful drains then detaches; abortive discards queued + cancels in-flight;
    `on_close` fires **exactly once**; `close`/`free` refused before detachment; **reuse after
    detachment** (re-init on the same storage) works; a completion after legal reuse is impossible.
- **Backends (the same suite over each seam):** readiness (POSIX `poll`) + the four completion
  backends — the **pollcomp completion double** (deterministic CI/ASan driver), io_uring (Linux
  container), IOCP (MinGW cross at least compiles; runtime where available), lwIP-raw (loopback), and
  EFI_UDP4 via the **host mock-EFI harness** (`build_mock_efi_test.sh`). The pollcomp double is the
  deterministic gate; the rest are capability-gated.
- **Teardown/lifetime (ASan+UBSan+LSan):** after every close path assert **confirmed detachment**
  (`on_close` once), **`live_count == 0`** on the happy path (0 UDP + 0 quarantined for
  EFI), **no op references the object after detach** (the stable-token invariant — a late completion
  with `life == NULL` is dropped), and **reuse-after-detach** is clean. Quarantine paths assert the
  object detaches while the inbound storage stays pinned (leak intentional, LSan-suppressed on that
  exact allocation).
- **Gates:** the deterministic facet suite over readiness + pollcomp runs in CI under ASan/UBSan; the
  `uefi-dgram-gate` (both PE arches, both configs) continues to compile the EFI datagram path; the
  freestanding datagram/dns gates are unaffected.

---

## 9. Open decisions requiring reviewer confirmation

1. **D-STORAGE (§5):** split `KlDgramSlots` into an object-owned outbound pool + a life-token-owned
   inbound slot (vs. the current single-block allocation). *Recommendation: split — it is what B.6
   requires and what 6.1 already implies for recv.*
2. **D-COMPAT (§6):** keep `KlUdp` as an ergonomic wrapper re-based on `KlDatagram`, both public
   surfaces coexisting (vs. deprecating `kl_udp_*`). *Recommendation: coexist, no `kl_udp_*` break.*
3. **D-FD-OWNERSHIP (§1):** `KlDatagram` takes a prepared `(provider, fd)` and owns the fd close at
   detachment; sockopts stay off the core surface (vs. `KlDatagram` doing socket creation/config).
   *Recommendation: prepared-fd + off-surface sockopts — keeps the Tier-1 primitive transport-pure.*
4. **Sub-step split (§0):** 7A (wire the machine, flip ⚙→✅, KlUdp unchanged) then 7B (public surface +
   ABI split + banner + validation). *Recommendation: yes — 7A is a reviewable behavior-preserving
   refactor; 7B is additive.*

## 10. Out of scope / non-goals

- Tier-2 (`mmsg` batching, GSO/GRO split/coalesce) — the flags stay reserved; the public API does not
  add batch entry points in Step 7.
- New capabilities on any backend (no new multicast/TOS/EFI features) — Step 7 only exposes what the
  matrix already reports.
- `KlUdpServer`/DNS API changes — they ride `KlUdp` unchanged.
- Threading — single-threaded event-loop model, unchanged.

---

**No implementation begins until this freeze is reviewed.** On acceptance, 7A lands first (machine
wiring + KlUdp regression), then 7B (public surface + `datagram_detail.h` opt-in + STABLE banner +
`test_datagram_public`), each committed and paused for review.
