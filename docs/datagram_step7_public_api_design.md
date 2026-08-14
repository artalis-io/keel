# Step 7 — Public `KlDatagram` API: Design Freeze (v3)

**Status:** DESIGN FREEZE (v3 — addresses v2 review). No code. For re-review before 7A-1. Grounds every
decision in the frozen Tier-1 contract (`docs/datagram_contract.md`) and the already-built machine
(`src/datagram_{slots,send,recv,close,life}.h`, `include/keel/datagram{,_detail}.h`).

**v3 changes (from v2 review):** (1) **the neutral, cross-platform retirement-classification seam is
now designed** (new §4.3) — a `KlDgramRetireResult` the close coordinator collects per op, with the
per-backend mapping and the CLOSE_ERROR-only-when-all-retired rule; (2) a **`KL_DGRAM_CLOSE_NONE = 0`
sentinel** so a zeroed object never looks DETACHED; (3) the **copy-before-accept test is strengthened**
to prove the *provider* contract (retain the submit pointer, force QUARANTINED, poison the freed
outbound pool, assert no late access) — the caller-buffer facade test is kept too; (4) §5 states the
send-copy / recv-lend **asymmetry** that justifies the storage split.

**v2 changes (retained):** quarantine is an explicit terminal *result* (not "confirmed detachment"); one
frozen close/retirement sequence with backend close/cancel as the classifying step (fd close in the
close machine, not `free`); `KlUdp` KEEPS its byte-budget queue (not re-based); copy-before-accept is a
provider contract; lwIP-raw one-held-slot is an explicit 7A increment; STABLE-banner wording clarified.

---

## 0. Current state and the two coupled deliverables (7A incremental)

The Tier-1 machine exists as **standalone, unit-tested TUs**, but only **recv + the B.6 life-token are
live-wired**; the packet-slot **send** queue, strict pause, and close coordinator are ⚙ (`KlDatagram`
still carries the legacy `q_head/q_tail` byte-FIFO — `src/udp.c:84`, `include/keel/udp.h`).

Step 7 = **7A wire the machine** then **7B public surface**. Per review, **7A is itself incremental**,
each a focused reviewed commit with its own regression tests:

- **7A-1 — storage separation.** Split the single `KlDgramSlots` block into an **object-owned outbound
  pool** and a **life-token-owned inbound slot** (§5). Recv inbound storage is already life-owned (6.1);
  this makes the split explicit and reusable.
- **7A-2 — close/retirement integration.** Wire `KlDgramClose` with the frozen retirement sequence (§4)
  and the neutral retirement-classification seam (§4.3, `KlDgramRetireFn`/`KlDgramRetireResult`), incl.
  backend close/cancel as the *classifying* step and the terminal result.
- **7A-3 — send integration + compatibility.** Assemble `KlDgramSend`+`KlDgramSlots` into the public
  `KlDatagram`. Per the frozen D-COMPAT policy (§6) this does **not** re-base `KlUdp`'s send — `KlUdp`
  keeps byte-budget. Includes the send-queue-geometry tests that byte-budget green tests would NOT prove.
- **7A-4a — strict-pause CORE conformance.** Prove `KlDgramCore` drives the interest-drop latch
  (readiness drops interest; completion holds one) over neutral scripted adapters, incl. reentrant
  pause/stop DURING a readiness drain. Completes the core recv-control surface (`recv_stop`,
  `recv_held`). **This does NOT wire any live provider onto the core** — it establishes the core
  contract only.
- **7A-4b (folded into 7B) — LIVE backend seams.** Wire the actual poll/kqueue/epoll · pollcomp ·
  io_uring · IOCP · EFI · lwIP-raw recv seams onto the core and flip the `§10` strict-pause rows. Shared
  with `KlUdp`'s recv — recv behavior preserved (tested). Deferred to 7B with the public surface, since
  it is the same "make `KlDatagram` the live facade" step.
- **7A-5 — lwIP-raw one-held-slot cleanup.** Convert lwIP-raw's 16-entry copy ring to the one-held-packet
  contract. **This is provider state-machine work, not generic seam wiring** — its own increment with
  dedicated overflow / pause-under-backlog / close-with-backlog tests.

**7B** = the public `kl_datagram_*` surface + `<keel/datagram_detail.h>` opt-in ABI split + STABLE
banner + `tests/test_datagram_public.c`.

The `§10` matrix's ⚙ rows (packet-slot send queue, strict pause; lwIP-raw serial-recv) flip to ✅ per
backend only when the LIVE provider is wired onto the core and §8 validates it — i.e. in 7A-4b/7B, NOT
from the neutral-core conformance increments (7A-3/7A-4a) alone. Step 7 is not "done" until they are ✅
on the completion backends §8 validates.

---

## 1. Public `KlDatagram` API and ownership

**Ownership.** `KlDatagram` is a caller-owned value type, single-threaded, event-loop-thread-driven.
Its layout lives in `<keel/datagram_detail.h>` (opt-in). **STABLE-banner wording (7B, review Low):** the
`<keel/datagram.h>` banner MUST state — *"STABLE covers the `kl_datagram_*` function + type CONTRACT.
The struct LAYOUT is not ABI-stable: a consumer that stack-/embed-allocates a `KlDatagram` opts into the
layout by including `<keel/datagram_detail.h>` and MUST recompile when it changes. A consumer that only
holds a `KlDatagram *` (created behind the API) is insulated from layout."*

**Surface** (new `kl_datagram_*`, facade over the `kl_dgram_*` machine):

```c
/* lifecycle */
int  kl_datagram_init(KlDatagram *dg, const KlDatagramConfig *cfg);   /* fd ownership transfers ONLY on success */
int  kl_datagram_close_begin(KlDatagram *dg);     /* graceful */
int  kl_datagram_close_cancel(KlDatagram *dg);    /* abortive */
int  kl_datagram_free(KlDatagram *dg);            /* releases object memory AFTER on_close; -1 before */

/* data plane */
KlDatagramSendStatus kl_datagram_send(KlDatagram *dg, const KlDatagramMessage *m);
int  kl_datagram_recv_start(KlDatagram *dg, KlDatagramRecvFn on_recv, void *ud);
void kl_datagram_pause(KlDatagram *dg);
int  kl_datagram_resume(KlDatagram *dg);
void kl_datagram_recv_stop(KlDatagram *dg);

/* edges + terminal result + queries */
void kl_datagram_on_writable(KlDatagram *dg, KlDatagramWritableFn cb, void *ud);
void kl_datagram_on_drain(KlDatagram *dg, KlDatagramDrainFn cb, void *ud);
void kl_datagram_on_close(KlDatagram *dg, KlDatagramCloseFn cb, void *ud);   /* cb receives the RESULT */

unsigned              kl_datagram_caps(const KlDatagram *dg);          /* KL_DGRAM_CAP_* */
KlDgramCloseState     kl_datagram_close_state(const KlDatagram *dg);   /* OPEN/CLOSING/CLOSED (phase) */
KlDatagramCloseResult kl_datagram_close_result(const KlDatagram *dg);  /* terminal classification (§4) */
size_t   kl_datagram_send_queued(const KlDatagram *dg);
size_t   kl_datagram_send_inflight(const KlDatagram *dg);
uint64_t kl_datagram_dropped(const KlDatagram *dg);
uint64_t kl_datagram_truncated(const KlDatagram *dg);
KlError  kl_datagram_last_error(const KlDatagram *dg);
```

`KlDatagramConfig` (all borrowed): `{ KlEventCtx *ctx; KlAllocator *alloc; const KlSocketProvider
*sockets; KlSocketHandle fd; size_t send_slots; size_t send_slot_cap; size_t recv_cap; unsigned
want_caps; }`. The `fd` is already `socket()`'d / `configure()`'d / `bound` by the caller through
`sockets->dgram`. **fd ownership transfers to `KlDatagram` ONLY when `kl_datagram_init` returns 0**
(review High-2); on failure `init` touches nothing the caller must reclaim and the caller retains the
fd. From success to terminal close, `KlDatagram` owns the fd and closes it as part of the close
machine's backend-retirement step (§4) — **not** in `free`. Sockopts stay OUT of this surface (§6).

**Reuse** (invariant 8): `kl_datagram_init` on a fully-detached (memset-zero) object.

---

## 2. Fixed-slot send: capacity, FIFO ordering, backpressure

The frozen `KlDgramSend`+`KlDgramSlots` contract, surfaced verbatim: `send_slots` fixed slots ×
`send_slot_cap` bytes (one init-time alloc, object-owned); FIFO ring by submission order; single-flight
(Tier-1); atomic accept with a total `KlDatagramSendStatus` (`ACCEPTED | WOULD_BLOCK | TOO_LARGE |
UNSUPPORTED | CLOSED | ERROR`) — no ownership on refusal (invariant 4); full→non-full fires
`on_writable` (reentrant, non-destructive); non-empty→empty fires `on_drain` (destructive tail).

**Backpressure geometry is a DELIBERATE contract change vs. `KlUdp` (review High-3).** Fixed
equal-capacity slots back-pressure at a **datagram count** (`send_slots`), NOT a byte budget: many small
datagrams can exhaust the slot count while well under a legacy byte budget. This is the public
`KlDatagram` contract; `KlUdp` does NOT inherit it (§6). 7A-3 ships explicit tests for this geometry
(count-based `WOULD_BLOCK`, hole-reuse FIFO) precisely because they exercise behavior the byte-budget
tests never touch.

---

## 3. Receive pause/resume and one-held-packet semantics

The frozen `KlDgramRecv` contract, unchanged from v1: serial receive (invariant 1); one-held-packet
(completion holds one on pause, delivered once on resume; readiness drops interest); `pause`/`resume`/
`recv_stop`; delivery contract (one per callback; `peer` mandatory → violation fails safely with error;
`local` iff `KL_DGRAM_HAS_LOCAL`; oversized → one delivery with `KL_DGRAM_TRUNCATED` + counter);
callback confinement (invariant 9 — may pause/close but not free/re-init before `on_close`; readiness
re-checks paused/closing after every callback).

---

## 4. Close, retirement sequence, and terminal classification (v2 — the core correction)

The v1 error was firing `on_close` on a **logically** cancelled-but-**physically**-unretired op while
still calling it "confirmed detachment." v2 separates the two and reports which one happened.

### 4.1 One frozen close/retirement sequence (review High-2)

`close_begin` (graceful) / `close_cancel` (abortive) drive a single ordered sequence; the backend
close/cancel is the step that **classifies** the outcome (for EFI, closing/cancelling the child is what
establishes RETIRED vs QUARANTINED — it cannot wait until after the coordinator has already declared
retirement):

1. **Stop admission** — refuse new sends (`CLOSED`), stop arming recv.
2. **Discard / drain** — abortive: discard queued-but-unsubmitted datagrams (release slots); graceful:
   drain the queued output first (or observe sticky error — failure never wedges close).
3. **Backend retirement, exactly once** — invoke the provider's cancel/close on the outstanding recv/
   send op **and close the fd**. Each op is cancel-requested at most once. This is the single point that
   touches the backend and where the fd is closed.
4. **Receive terminal classification** — each op resolves to one of the neutral **`KlDgramRetireResult`**
   values `PENDING | RETIRED | QUARANTINED` (§4.3), and may additionally flag a terminal transport
   failure via `transport_err`. There is no per-op `ERROR` result: a transport failure is carried
   alongside `RETIRED` and only becomes the object-level `CLOSE_ERROR` when *every* op is confirmed
   RETIRED (§4.3 join). The object's result = the §4.3 join over its ops (any QUARANTINED ⇒ QUARANTINED;
   else `transport_err` with all RETIRED ⇒ CLOSE_ERROR; else DETACHED).
5. **Fire `on_close(ctx, result)`** — exactly once, as a destructive tail (state `CLOSED` set first).
6. **Release wrapper resources** — `kl_datagram_free` (or the caller in the callback) releases
   object-owned memory. On QUARANTINED the life-token-owned inbound storage is NOT released here (§5).

### 4.2 Terminal result — DETACHED vs QUARANTINED (review High-1, Medium-5)

```c
typedef enum {
    KL_DGRAM_CLOSE_NONE = 0, /* SENTINEL: no terminal reached yet (also the value of a zeroed/reused
                                object). kl_datagram_close_result() returns this until state == CLOSED. */
    KL_DGRAM_DETACHED,       /* CONFIRMED physical retirement: recv+send ops retired, buffers returned,
                                life fully released → live_count 0. The strong Tier-1 guarantee. */
    KL_DGRAM_QUARANTINED,    /* Wrapper-SAFE but NOT physically retired: an op could not be confirmed
                                retired (EFI). The KlDatagram object + fd wrapper are detached and
                                reusable/freeable; the life-token-owned inbound buffer stays pinned to
                                process/firmware teardown (bounded fail-closed leak). NOT "confirmed
                                detachment". */
    KL_DGRAM_CLOSE_ERROR     /* Terminal transport error AND all ops nevertheless confirmed retired
                                (see §4.3 — an uncertain-ownership close error is QUARANTINED, not this). */
} KlDatagramCloseResult;

typedef void (*KlDatagramCloseFn)(void *ctx, KlDatagramCloseResult result);
```

- **`KL_DGRAM_CLOSE_NONE = 0` is the sentinel** (review Medium): a memset-zero (reused, invariant 8) or
  not-yet-closed object reports NONE, never a spurious DETACHED. All terminal results are nonzero.
  `kl_datagram_close_result()` is only meaningful once `close_state() == CLOSED`; before that it returns
  NONE.
- **Invariant 6 ("confirmed detachment") applies ONLY to `KL_DGRAM_DETACHED`.** `on_close` firing with
  QUARANTINED is explicitly NOT confirmed detachment — it promises only that no completion will
  reference the wrapper (guaranteed by the life token, §5), not that the op physically retired.
- `kl_datagram_close_state()` stays the lifecycle phase (`OPEN/CLOSING/CLOSED`); `CLOSED` no longer
  conflates outcomes — `kl_datagram_close_result()` (and the callback argument) carries the distinction.
- **`free` refuses before `CLOSED`.** After `CLOSED` it releases object memory; on QUARANTINED it must
  NOT reclaim the pinned inbound storage (life-owned) — safe because that storage is not in the object.

### 4.3 The neutral retirement-classification seam (7A-2 — review High-1, the remaining blocker)

The close sequence (§4.1 step 3–4) needs each backend to classify each op as retired / quarantined /
pending. Today generic `close` returns only `0/-1` and the rich result lives only in EFI primitives
(`kl_uefi_udp_op_state` → `KlUefiUdpOpResult`). 7A-2 freezes a **neutral, transport-agnostic seam** the
close coordinator (`KlDgramClose`) collects, so no backend-specific type leaks into the machine:

```c
typedef enum {
    KL_DGRAM_RETIRE_PENDING = 0,  /* not yet terminal — object stays CLOSING; re-poll on the next
                                     drained terminal event for this op. */
    KL_DGRAM_RETIRE_RETIRED,      /* physically confirmed: buffer returned, life ref releasable. */
    KL_DGRAM_RETIRE_QUARANTINED,  /* ownership UNCONFIRMED — retain the life ref forever, fail-closed. */
} KlDgramRetireResult;

typedef enum { KL_DGRAM_OP_RECV = 0, KL_DGRAM_OP_SEND } KlDgramOpKind;

/* Installed by the live-wiring layer (udp.c / the public KlDatagram) alongside the existing cancel
 * hooks. The close coordinator calls it at step 3 (backend retirement) and again whenever a cancelled
 * op's terminal completion is drained (the existing on_complete retire path notifies the coordinator).
 * `transport_err` is set to 1 by the op iff a terminal transport error occurred on it. */
typedef KlDgramRetireResult (*KlDgramRetireFn)(void *ctx, KlDgramOpKind kind, int *transport_err);
```

**Per-backend mapping (frozen):**

| Backend / situation | Classification |
|---|---|
| POSIX / readiness — synchronous disarm + `close(fd)` | `RETIRED` immediately (no async op outstanding). |
| Completion (pollcomp / io_uring / IOCP) — normal cancel | `PENDING` until the op's terminal completion event is drained (which releases the life ref), then `RETIRED`. IOCP dequeues-before-free; io_uring's `IORING_OP_CANCEL` may miss but the op still eventually completes → still resolves to `RETIRED`. |
| EFI — unconfirmed cancellation (`kl_uefi_udp_op_state` = QUARANTINED/INVALID) | `QUARANTINED`. |
| Any provider — close/cancel error with **uncertain** buffer ownership | `QUARANTINED` (never `CLOSE_ERROR`), or stay `PENDING`/`CLOSING` if it may yet resolve. |

**Object-result join (step 4).** The coordinator computes the terminal `KlDatagramCloseResult` only once
**no op is PENDING**:
- any op `QUARANTINED` ⇒ **`KL_DGRAM_QUARANTINED`** (retain those life refs; object detaches wrapper-safe);
- else if any op set `transport_err` (with **all** ops `RETIRED`) ⇒ **`KL_DGRAM_CLOSE_ERROR`**;
- else ⇒ **`KL_DGRAM_DETACHED`**.

**The safety rule (review High-1):** `CLOSE_ERROR` is legal **only when every op is confirmed
`RETIRED`.** A close/cancel failure that leaves ownership uncertain is `QUARANTINED`, never
`CLOSE_ERROR` — because only `DETACHED` and `CLOSE_ERROR` permit the object's storage to be reclaimed,
and reclaiming under unknown ownership is the exact UAF the stable token exists to prevent. Until every
op is terminal, the object stays `CLOSING` and `on_close` does not fire.

---

## 5. B.6 stable-token, quarantine, and copy-before-accept

- **Storage split (D-STORAGE, approved) — and WHY it is asymmetric (v3).** The split follows the
  buffer-ownership asymmetry between recv and send:
  - **Receive = buffer-LENT.** The backend writes the inbound datagram *into* the object's inbound slot
    across the async op, so it holds a pointer to that slot for the op's whole lifetime. That storage
    must therefore outlive any op that can be left dangling (quarantine) → it is **life-token-owned**
    (`on_final` frees it, event-ctx-allocated, outlives the wrapper).
  - **Send = payload-COPIED.** The backend copies the payload + dest/src/tos into **backend-owned op
    storage before returning** from submit, so it holds NO pointer into the object's outbound slot after
    `ACCEPTED`. That pool is therefore safe to reclaim at detachment even if a send op is later
    quarantined → it is **object-owned** (freed at detachment / on QUARANTINED).
- **Copy-before-accept is a PROVIDER CONTRACT (review D-STORAGE + Medium-3).** The send half above holds
  ONLY IF every provider `send`/`post_dgram_send` truly copies before returning (the ops table documents
  "backend copies datagram + destination for op's lifetime"; io_uring/IOCP copy into `op->sendbuf`). A
  provider that retained a pointer into the object's outbound slot would UAF the moment the object frees
  its pool at a QUARANTINED close. 7B promotes copy-before-accept to a normative clause in `datagram.h`
  and `datagram_contract.md`; §8 proves it with a provider-conformance test that poisons the freed pool
  (not just a facade-copy test). A provider that cannot copy-before-accept may not use object-owned
  outbound storage.
- **Quarantine terminal (§4.2).** A backend that cannot confirm a cancel retains its op's life ref
  forever; `on_close` fires with **QUARANTINED**; the object detaches (wrapper-safe) but the inbound
  buffer stays pinned to EBS. A UDP completion with `life == NULL` is dropped (no wrapper deref).

---

## 6. Compatibility policy with legacy `KlUdp` (D-COMPAT — frozen)

**Frozen policy: `KlUdp` KEEPS its documented byte-budget send semantics and is NOT re-based onto fixed
slots in Step 7.** (Review High-3 + D-COMPAT: v1's "KlUdp behavior identical via rebase" is withdrawn —
fixed equal-capacity slots change *when* backpressure occurs vs. a byte budget, so a rebase is a
behavior change, not a refactor.)

- `KlUdp` remains the ergonomic byte-budget UDP surface: `kl_udp_*` keep their exact signatures,
  semantics, `max_send_queue` **byte** budget, and one-node-per-packet queue. `KlUdpServer` (6.2) and the
  DNS resolver (6.3) are untouched. Its send path is NOT migrated in Step 7.
- The public `KlDatagram` is the **fixed-slot Tier-1 primitive**, exposed and validated **independently**
  (raw `KlDatagram` over a scripted provider + new consumers), NOT by re-routing `KlUdp`'s send.
- They coexist and share the transport substrate that does NOT change acceptance geometry — recv, the
  life token, the fd, and (where a consumer uses `KlDatagram` directly) the close coordinator. `KlUdp`'s
  existing teardown is unchanged; 7A does not route `KlUdp`'s send through `KlDgramSend`.
- **Object-model note (7A-1/7A-3):** the concrete reconciliation of "does `KlUdp`'s embedded transport
  share `struct KlDatagram` with the fixed-slot public type" is an implementation detail bounded by this
  policy — *whatever the layout, `KlUdp`'s byte-budget acceptance behavior must not change*, and the
  fixed-slot geometry must not leak into `kl_udp_*`. If they cannot cleanly share one struct without
  changing `KlUdp`, they don't share it.
- A future **intentional** migration of `KlUdp` onto fixed slots — with a documented backpressure/memory
  contract change and its own tests — is a separate step, explicitly **out of Step 7 scope**. Sockopts
  (multicast/TOS/GSO/pktinfo) stay on `KlUdp` / the provider `dgram` ops, off the core `KlDatagram`
  surface.

---

## 7. Backend capability and degradation rules

- `kl_datagram_caps(dg)` → `KL_DGRAM_CAP_*` bitmask from the provider's declared datagram caps.
- **Required-if-requested MUST fail** (§9 of the contract): non-NULL `msg->local` without `SOURCE_PIN`,
  or per-packet TOS without `TOS` → `KL_DATAGRAM_UNSUPPORTED`, nothing sent; connected/multicast/broadcast
  without support → config/init error. Never silently dropped.
- **Optional recv metadata degrades silently** (`local` absent when `PKTINFO` off). Truncation is not a
  capability — mandatory.
- **Copy-before-accept (§5) is a REQUIRED provider behavior**, not a queried capability — a provider that
  cannot satisfy it cannot serve object-owned outbound storage.
- Per-backend truth = `datagram_contract.md` §10; the ⚙ rows are the 7A wiring targets.

---

## 8. Deterministic unit, sanitizer, backend, and teardown validation

`tests/test_datagram_public.c` (analogue of `test_transport_public`), plus per-increment 7A regression
tests, run as a matrix over a scripted in-test provider double:

- **Send queue (7A-3):** FIFO across hole-reuse; single-flight; **count-based** `WOULD_BLOCK` at
  `send_slots` (the geometry byte-budget tests miss); `on_writable`/`on_drain` edges; `TOO_LARGE`;
  `UNSUPPORTED` (required cap absent); `CLOSED` after `close_begin`; no-ownership-on-refusal.
  **Copy-before-accept — TWO distinct tests (review Medium-3):**
  1. *Facade ownership:* mutate+free the **caller's** buffer right after `ACCEPTED`; assert the
     transmitted bytes are the original (proves `kl_datagram_send` copied caller→slot).
  2. *Provider contract:* the scripted provider **retains the exact pointer passed to `submit`** (the
     object's outbound slot), then the test forces a **QUARANTINED** close and releases the object-owned
     outbound pool **through the test allocator**, which poisons the freed region (or the fake provider
     is instrumented to record any deref) — the test never hand-dereferences freed storage itself.
     Assert the provider had already copied all payload **and** address metadata into backend op storage
     at submit time and performs **no** later read/write through the retained pointer (ASan on the
     allocator-poisoned region catches a violation). This proves the *backend* copied — a facade
     copy into the slot would still pass test 1 but fail here if the backend kept a slot pointer.
- **Receive (7A-4):** one per callback; `peer` mandatory (violation → error, no callback); pause
  holds one (completion) / drops interest (readiness); resume delivers once then re-arms; stop discards
  held; truncation flagged + counted; zero-length ≠ failure.
- **Close + terminal result (7A-2):** graceful drains then DETACHED; abortive discards + cancels;
  `on_close(result)` fires **exactly once**; **DETACHED** path asserts confirmed retirement + `live_count
  == 0`; **QUARANTINED** path (scripted unconfirmable cancel) asserts the object detaches, `on_close`
  reports QUARANTINED, the inbound buffer stays pinned (LSan-suppressed on that exact allocation), and a
  late completion with `life == NULL` is dropped; **CLOSE_ERROR** path asserts a terminal transport
  error is reported; `free` refused before `CLOSED`; reuse-after-detach clean.
- **lwIP-raw one-held-slot (7A-5):** dedicated overflow (>1 transport-owned packet), pause-under-backlog,
  and close-with-backlog tests proving the copy-ring→one-held-slot conversion.
- **Backends:** readiness (POSIX `poll`) + the four completion backends — pollcomp double (deterministic
  CI/ASan gate), io_uring (container), IOCP (MinGW compile; runtime where available), lwIP-raw
  (loopback), EFI_UDP4 (host mock-EFI harness; the QUARANTINED terminal is exercised here).
- **Sanitizers:** ASan+UBSan+LSan across all facet tests; the `uefi-dgram-gate` + freestanding gates
  continue to pass. Each 7A increment carries its own focused regression test; a green `KlUdp` suite is
  explicitly NOT accepted as proof of send-geometry equivalence (review High-3).

---

## 9. Decisions (updated per review calls)

1. **D-STORAGE — APPROVED** with the copy-before-accept provider contract + test (§5, §8).
2. **D-COMPAT — coexistence APPROVED; rebase NOT approved.** Policy frozen (§6): `KlUdp` keeps
   byte-budget, not re-based in Step 7; public `KlDatagram` fixed-slot is separate; migration out of scope.
3. **D-FD-OWNERSHIP — APPROVED:** ownership transfers only on successful `init`; backend close/cancel
   (and fd close) happen in the close machine's step 3 as the classifying act (§4.1), before detachment
   is declared; sockopts stay off the core surface.
4. **7A/7B split — APPROVED, and 7A is incremental** (§0: storage sep → close/retirement → send
   integration/compat → strict-pause backend wiring → lwIP one-held-slot), each with focused regression
   tests.

**Remaining v2 blocker (retirement-classification seam) is now designed in §4.3**, and the two v2 test/API
gaps are closed (§4.2 `KL_DGRAM_CLOSE_NONE` sentinel; §8 provider-contract copy test). Per the review,
this makes the design ready for **7A-1** on acceptance.

## 10. Out of scope / non-goals

Tier-2 (`mmsg`, GSO/GRO split/coalesce) entry points; new backend capabilities; a `KlUdp` fixed-slot
migration (a separate future step with a documented contract change); `KlUdpServer`/DNS API changes;
threading (single-threaded, unchanged).

---

**No implementation begins until this v2 freeze is reviewed.** On acceptance, 7A lands in the five
increments above (each committed + paused for review), then 7B (public surface + `datagram_detail.h`
opt-in + STABLE banner + `test_datagram_public`).
