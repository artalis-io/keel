# KlDatagram synchronous teardown — design freeze (framework prerequisite for M3)

**Status:** design freeze. **Inventory + decisions only — no production code.** This is the reviewed
framework prerequisite the M3 review required before the DNS resolver can migrate to the public
`KlDatagram` (see [datagram_consolidation_design.md](datagram_consolidation_design.md) §2/§6 M3). It is
its own increment: freeze → review → implement → then complete M3 and re-run the teardown probes. The
experimental "abandon" spike is NOT folded into M3; it only informs this design and is preserved out of
tree (git tag `m3-send-machine-wip`).

## 0. The problem M3 surfaced

`KlResolver.destroy()` is a **synchronous** contract: it must fully reclaim the resolver before
returning, and production callers (`kl_client_free`, `client_async.c:1485`; the request-start unwind at
`client_async.c:1342`, reachable inline from a sync-completion resolver) call it without pumping the
loop, then tear down the `KlEventCtx`. The public `KlDatagram` lifecycle
(`kl_datagram_close_begin`/`_cancel` then `kl_datagram_free`) is **confirmed-detachment**: on a
completion backend the recv op's cancel terminal drains on a LATER loop tick, and `free` is refused
until then. So a `KlDatagram`-owning object that must be destroyed synchronously cannot use it.

**Empirically confirmed (Linux io_uring, ASan/UBSan/LSan):**
- Deferred destroy + immediate `kl_event_ctx_free` (no pump) leaks **12,560 bytes** (the whole resolver
  + core + rx storage) — `ctx_free` does not drive a pending datagram close to `CLOSED`.
- A synchronous "abandon" spike (force-detach without waiting for op terminals) fixes the common case —
  a create→destroy→`ctx_free` no-pump probe is **leak-free** — because the recv storage is
  life-token-owned and outlives the reap. **But** when a send is in-flight at destroy
  (`dns_destroy_with_inflight`, `send_inflight=1`), it leaks **64 bytes** (the send FIFO ring): the send
  machine's storage is object-owned and `kl_dgram_send_free` refuses to free it while a send is
  outstanding.

So: **synchronous owner destruction is compatible with completion safety only when every
provider-referenced buffer outlives its posted op.** The recv side already satisfies that; the question
is whether the send side genuinely must too.

## 1. Core finding — the send slot is NOT provider-referenced after submit

The completion seam's ownership contract is **frozen (§2.5.1, `src/io_engine.h:36`)**:

> *"A send op's payload AND dest/src/tos are COPIED before a successful return; a recv op's `buf` is
> LENT (the life-owned inbound slot — the backend writes it, never frees it)."*

Verified in every completion backend:

| Backend | Send payload at submit | Recv buffer | Evidence |
|---|---|---|---|
| io_uring | **copied** (`op->sendbuf` owned) | lent | `event_iouring.c:111` |
| IOCP | **copied** (`op->sendbuf`) | lent | `event_iocp.c:115` |
| lwIP-raw | **copied** ("stores a BOUNDED copy") | lent (retained pbuf → copied out) | `event_lwip_raw.c:478` |
| EFI_UDP4 | **copied** ("A copy (not a reference) is required") | lent (RxToken writes it → EFI quarantine) | `event_efi.c:44` |
| readiness (epoll/kqueue/poll/WSAPoll/pollcomp) | **synchronous** send — never in-flight | — | `sockets->dgram->send` |

The `KlDgramSlots` header states the same intent (`datagram_slots.h:12-14`): *"payload-COPIED into
backend op storage at submit, so the object may free this."*

**Conclusion:** the outbound slot's payload + addresses are **copied** into backend-owned op storage
before `post_dgram_send` returns success. After a successful submit, **no backend references the send
slot** (or the send FIFO ring). This is the exact OPPOSITE of the recv buffer, which is *lent* to the
backend (written directly), and is precisely why the recv holder (`KlDgramCoreRx`) is life-token-owned
and the send storage is not.

**Therefore the send storage does NOT need to move into a life-owned holder.** The premise that it is
"provider-referenced" does not hold under the frozen copy-at-submit seam contract. The recv/send
asymmetry is intentional and correct: *lent* storage must outlive; *copied* storage need not.

## 2. Why the 64-byte leak actually happens

`kl_dgram_send_free` (`datagram_send.c:269-280`) refuses when `inflight_n > 0`:

```c
if (s->inflight_n > 0)
    return -1;   /* provider-referenced send outstanding — slot storage must not be freed */
```

Two things are conflated here:

1. **The comment's stated reason** ("provider-referenced ... must not be freed") is **not accurate**
   under §1 — the backend holds a copy, not a reference.
2. **The guard's real protective value** is for the ORDINARY close path: while a send is in-flight, its
   completion WILL call `kl_dgram_core_send_on_complete` → the send machine touches the ring/slots to
   release the slot and fire edges. Freeing the ring first would UAF *that* path.

Under **abandon**, protection (2) is already provided by a different mechanism: `core_on_close` marks
the life token dead (`kl_dgram_life_mark_dead`), so a late send completion dispatches with
`core == NULL` and **skips** `send_on_complete` entirely (`datagram.c:107-108`), only releasing the
op's life ref. So in the abandon path the send machine is never touched by a late completion, and — by
§1 — no backend references the slots. **Both hazards are absent → freeing the ring + slots with an
in-flight send is safe in the abandon path.** The blanket guard is simply too conservative for it.

`kl_dgram_core_free` ignores `send_free`'s `-1`, so today the ring silently leaks — that is the 64 bytes.

## 3. Decision — Option A (recommended): abandon-aware send teardown, no storage move

Add a distinct **abandon** teardown to the send machine that frees the ring + releases every occupied
slot back to the (about-to-be-freed) pool **regardless of `inflight_n`**, valid ONLY because its two
preconditions are guaranteed by the caller:

- **P1 (no external reference):** the frozen §2.5.1 copy-at-submit contract — asserted as an invariant
  of every completion backend; a future backend that *lends* the send buffer would violate the seam and
  is out of contract.
- **P2 (no live completion path):** the life token is already marked dead before this runs, so a late
  send completion cannot re-enter the send machine.

Shape (names illustrative; frozen in this doc, implemented in the next increment):

- `kl_dgram_send_abandon(KlDgramSend *s)` — like `kl_dgram_send_free` but **skips the `inflight_n`
  guard**: discard queued-but-unsubmitted slots, release any in-flight slot back to the pool (its
  metadata only — the payload copy lives in the backend op), free the FIFO ring, zero the machine.
  Asserts P2 (dead token) is the caller's responsibility.
- `kl_dgram_core_abandon(KlDgramCore *core)` — reach a **silent** terminal without waiting for physical
  op terminals and WITHOUT invoking `core_on_close`/`user_on_close` (§4a): mark the token dead, cancel/
  close backend ops, release the owner ref (after cancel/close), then `_free` the object-owned parts
  using the abandon-aware send teardown. Post-condition: `CLOSED` with no public terminal reported;
  object-owned parts freed; the life-owned rx holder + any in-flight/quarantined op's ref outlive,
  reclaimed at the token's final release when every op is finally reaped (or intentionally pinned by an
  EFI-quarantined recv OR send, §5).
- `kl_datagram_teardown(KlDatagram *dg)` — INTERNAL facade entry (declared in `src/datagram_open.h`, NOT
  the public `keel/datagram.h`): `kl_dgram_core_abandon` → free the heap core → memset the handle. This
  is the **owner-destruction** path a synchronously-freed owner (the DNS resolver) calls instead of
  `close_begin/cancel + free`.

**No storage moves; no new allocation; no hot-path change.** The only new heap effect is *earlier* (not
additional) freeing of the same ring. Sizing arithmetic is unchanged (§7).

### 3-alt. Option B (the review's proposal): move send slot storage into a life-owned holder

Allocate the outbound `KlDgramSlots` (+ FIFO ring) inside a life-token-owned holder parallel to
`KlDgramCoreRx`, so an in-flight send keeps it alive and `on_final` frees send + recv storage together;
`core_free` then never frees send storage.

- **Works, but heavier and unjustified here.** It adds an allocation/indirection and a second on_final
  free path to solve a reference hazard that **does not exist** under §1 (the backend copied). It would
  be the correct design *only if* a completion backend *lent* the send buffer — which the frozen §2.5.1
  contract forbids. Adopting B would also enlarge the always-live footprint (send storage pinned until
  the last op reaps) with no safety gain over A.
- **Recommendation:** choose **A**. Keep B on record as the design that would be required if the
  copy-at-submit seam contract were ever relaxed; if that happens, it becomes a seam-contract change
  with its own freeze, and B is the follow-on.

The remainder of this freeze specifies **Option A**.

## 4. Ownership at every lifecycle stage (Option A)

| Stage | Send slot payload | Send FIFO ring | Recv inbound slot | Notes |
|---|---|---|---|---|
| **init** | object-owned (pool, one alloc) | object-owned (one alloc) | **life-owned** (rx holder) | unchanged from today |
| **enqueue** (`kl_datagram_send`) | copied caller→slot; slot occupied | slot pointer pushed | — | atomic accept-or-refuse; no alloc |
| **submit** (`post_dgram_send`) | **copied slot→backend op** (§2.5.1) | slot stays occupied (FIFO) | — | after this the backend references neither |
| **completion** (ordinary) | slot released to pool | slot popped | slot delivered + re-armed | `send_on_complete` on the LIVE machine |
| **async cancellation** (close_cancel) | queued discarded; in-flight cancel posted | queued slots released | recv cancel posted | terminal drains later → CLOSED (confirmed-detachment, unchanged) |
| **abandon** (owner destroy) | queued discarded + in-flight slot released to pool (P1) | ring freed (P2) | **not freed** — life-owned, pinned by the op's ref | SILENT terminal (no on_close, §4a); token dead first; owner ref released AFTER cancel/close; core_free frees object-owned parts; rx holder pinned if a recv OR send op is EFI-quarantined |
| **ordinary close** (begin/free) | freed by `send_free` once `inflight_n==0` | freed by `send_free` | freed by rx `on_final` at final release | **unchanged**; the `inflight_n` guard stays for this path |
| **alloc-failure unwind** (init) | pool free + ring free + rx holder free, in reverse order; fd NOT adopted | — | — | pre-adoption; no token/op exists yet |
| **final life release** (last op reaped) | already returned to pool at abandon | already freed | rx holder freed by `core_rx_final` | frees recv storage exactly once; send storage was object-owned and already freed |

**Exactly-once guarantees:** send ring freed once (either by `send_free` on the ordinary path OR by
`send_abandon` on the owner-destruction path — never both, since abandon transitions to CLOSED and
`core_free` is called exactly once after). Recv holder freed exactly once by `core_rx_final` on the
token's final release. No path frees send storage twice; no path leaves it unfreed (the ordinary path's
guard is discharged by `inflight_n==0`; the abandon path discharges it via P1+P2).

## 4a. Abandon ordering — no destructive public callback, no false terminal (FROZEN)

The abandon must NOT run the ordinary confirmed-detachment terminal. `core_on_close()` forwards
`user_on_close`, whose callback may free the facade/owner — so a mid-teardown invocation would free the
object out from under the very steps that follow. And the internal synchronous teardown must never
report a public "confirmed detached" terminal it did not actually confirm. So the abandon is **silent**
(the coordinator reaches CLOSED with `notified=1` set WITHOUT calling `on_close`), and `core_abandon`
performs the token-death + owner-ref drop DIRECTLY rather than through `core_on_close`.

**Frozen ordering** (one variance from the review's list, called out and justified):

1. **Suppress the user callback.** The abandon-close is silent: it sets the coordinator to CLOSED with
   `notified=1` and does NOT invoke `on_close` (so `core_on_close`/`user_on_close` never fire). No public
   terminal is delivered.
2. **Mark the life token dead.** `kl_dgram_life_mark_dead` → every late completion dispatches with a NULL
   owner and drops (releasing only its own op ref).
3. **Cancel/close backend operations.** Discard queued-but-unsubmitted sends, request cancellation of any
   in-flight recv/send, and close the fd exactly once (unconditionally for abandon — the send payload is
   copied per §1, so closing the fd cannot strand a referenced buffer). This step touches the recv
   machine (`recv_stop`/`cancel_recv`), which lives in the life-owned rx holder — so it MUST run while
   the rx holder is still alive, i.e. BEFORE the owner ref is released.
4. **Release the owner ref.** ONLY now (after step 3 is done touching the recv machine). If no op still
   holds a ref, the token's final release runs `core_rx_final` and frees the rx holder; if any op is
   in-flight or **EFI-quarantined** (recv OR send, §5), the ref is retained and the rx holder stays
   pinned (fail-closed).
   > **Variance from the review's step order (justified):** the review listed "mark dead AND release owner
   > ref" together, before cancel/close. Releasing the owner ref can free the rx holder (when no op is
   > posted — e.g. a datagram that never called `recv_start`), and the coordinator's cancel/close touches
   > the recv machine inside that holder. So the owner-ref RELEASE is deferred to AFTER cancel/close to
   > avoid a use-after-free; only `mark_dead` is done early (step 2). This preserves the review's intent
   > (token dead before any backend teardown) while keeping the recv machine alive for step 3.
5. **Abandon/free object-owned send + coordinator state.** `send_abandon` (frees ring, releases in-flight
   slot metadata to the pool — safe by §1/P1/P2), `close_free`, `slots_free`, memset the core. Does NOT
   touch the rx holder (token-owned) or fire any callback.
6. **Clear/free the facade.** `kl_free` the heap core, memset the handle (reusable). The owner then frees
   its containing object.

The **ordinary confirmed-detachment path is UNCHANGED**: `close_begin`/`close_cancel` still run
`close_run_terminal` → `close_detach` → `core_on_close` → `user_on_close` with a true DETACHED/
QUARANTINED/CLOSE_ERROR classification, on the loop tick when the ops actually retire. Abandon is a
separate, additional, silent owner-destruction entry, never a substitute.

## 5. Per-backend behavior

- **readiness (epoll / kqueue / poll / WSAPoll / pollcomp-as-readiness):** send is synchronous →
  `inflight_n` is 0 at teardown; abandon degenerates to today's `send_free`; close reaches CLOSED
  synchronously. No behavior change.
- **pollcomp (completion double):** send copied at submit; abandon frees ring + slots with the dead
  token; late cancel terminal drops. (Validated leak-free in the spike on macOS ASan/UBSan.)
- **io_uring:** send copied (`sendbuf`); the in-flight send's aborted/cancelled CQE drains at `ctx_free`
  → dead-token dispatch releases the op ref → rx holder freed. This is the exact case that leaked 64
  bytes; Option A closes it.
- **IOCP:** send copied (`sendbuf`); the fd-close/cancel posts the completion; overlapped op storage
  (`sendbuf`) freed in `iocp_op_free` on reap. The copy is what makes abandon safe here specifically
  (an overlapped `WSASend` reads its own `sendbuf`, never the slot).
- **lwIP-raw:** send copied (bounded copy); passive recv retained-pbuf path unaffected; the raw glue's
  own teardown already frees per-conn buffers.
- **EFI_UDP4:** send copied (`event_efi.c:44`). **BOTH recv AND send can quarantine** — `event_efi.c`
  classifies `EFI_DG_RECV` and `EFI_DG_SEND` as QUARANTINED/INVALID (`event_efi.c:67-86,231-237`), and a
  quarantined op (either kind) **retains its life ref forever** (fail-closed): an abandoned firmware
  RxToken may still write the *lent* recv buffer, and an abandoned TxToken may still read firmware state,
  so its op stays `in_use` with a borrowed/retained ref (`retain_life`). Two consequences for abandon:
  1. **Freeing the OBJECT-owned send slots stays safe** — EFI copied the payload into its per-op buffer,
     so nothing firmware-side references the slot. This is the same §1 guarantee; it holds on EFI.
  2. **The SHARED life token (and hence the life-owned rx holder) may remain intentionally pinned** by a
     quarantined SEND, not only by a quarantined recv. When the abandon releases the OWNER ref, the
     token's final release runs ONLY if no op still holds a ref; a quarantined send (or recv) keeps its
     ref, so the rx holder is deliberately NOT reclaimed (fail-closed) — an intentional, bounded EFI
     leak, exactly as today for a quarantined recv. This is correct and must not be "fixed."
  So the abandon must treat send and recv symmetrically for token pinning: it always releases the OWNER
  ref, and the rx holder is reclaimed iff NO op (recv or send) is quarantined-and-still-holding. The
  object-owned send slots are freed regardless (copy). No public terminal is reported (see §4a), so the
  DETACHED-vs-QUARANTINED classification is internal only; the pinning is enforced by each op's own ref
  retention on the EFI drain, not by the abandon's suppressed result.

## 6. Validation matrix (for the implementation increment)

Deterministic, leak-checked (LSan on Linux; ASan/UBSan everywhere). Backends: readiness (kqueue/epoll),
pollcomp, io_uring; IOCP + lwIP-raw + EFI via their existing gates/host-mocks.

- **recv-only abandon** — create + recv_start, no send, destroy + immediate `ctx_free`. (The current
  leak-free probe; keep it.)
- **send-only abandon** — post a send that stays in-flight (gating provider / slow peer), destroy before
  its completion; assert leak-free and no `send_on_complete` after free.
- **recv+send simultaneous abandon** — recv armed AND a send in-flight at destroy (the
  `dns_destroy_with_inflight` shape); the 64-byte regression; assert leak-free.
- **late completion after owner reuse** — abandon, then re-init a new datagram on the same
  `KlEventCtx` (and, adversarially, reusing the freed handle's memory); drive the old op's late CQE;
  assert it drops via the dead token and never touches the new object.
- **synchronous vs asynchronous cancellation** — readiness (sync retire) and completion (async cancel
  terminal) both reach a clean terminal under abandon.
- **allocation failure** — `kl_datagram_init` alloc-failure unwind (pool/ring/rx holder) leaves the fd
  unadopted and leaks nothing (extend the existing `alloc_failure_during_prep` coverage to the send
  storage).
- **repeated teardown / idempotence** — `kl_datagram_teardown` twice; abandon then `free`; assert no
  double-free.
- **zero leaks** across all of the above under LSan; **no new hot-path allocation** (abandon touches
  only teardown); **overflow-safe** sizing unchanged (the ring `cap * sizeof(KlDgramSlot*)` guard and
  the pool block arithmetic are untouched).
- **EFI quarantine — recv AND send** (host-mock, `integrations/uefi`): abandon with (a) a quarantined
  RECV op and (b) a quarantined SEND op. Each must: free the object-owned send slots (copy → safe),
  retain the quarantined op's life ref, leave the shared token + rx holder intentionally pinned
  (fail-closed, storage reclaimed only when/if the op is later reconciled), fire NO public terminal, and
  not double-free. The send-quarantine case is the one the freeze correction added; it must be covered,
  not only the recv case.
- **silent terminal / no false detachment** — a user `on_close` callback registered on a datagram that
  is then torn down via `kl_datagram_teardown` must NOT be invoked, and `close_result` must not report a
  fabricated DETACHED. (Guards correction 4a.)
- **confirmed-detachment unregressed** — the public `close_begin/cancel + free` path and its terminal
  classification (`test_datagram_public`, `test_datagram_live`, `test_dgram_close`) stay green: abandon
  is an ADDITIONAL owner-destruction entry, not a replacement.

## 7. Non-goals / invariants preserved

- **No public API/ABI change.** The owner-destruction entry is internal (`src/datagram_open.h`); the
  public `keel/datagram.h` contract and layout are untouched (STABLE banner holds).
- **Confirmed-detachment close semantics are preserved** for every existing consumer; the abandon path
  is explicitly the synchronous owner-destruction path, chosen only by an owner bound to a synchronous
  free contract.
- **No hot-path allocation added; sizing stays overflow-safe.** Option A only reorders/relaxes teardown
  frees; it introduces no new allocation and no new arithmetic.
- **I8 (allocation-free steady state)** and the frozen §2.5.1 copy-at-submit seam contract are relied on
  as invariants, not changed.

## 8. Increment plan

1. **This freeze → review.** (No code.)
2. **Framework increment (own commit):** `kl_dgram_send_abandon` + `kl_dgram_close_abandon` +
   `kl_dgram_core_abandon` + internal `kl_datagram_teardown`; the §6 tests; per-backend gates. Public
   surface unchanged.
3. **Complete M3 (own commit):** re-apply the DNS send state machine (preserved at
   `m3-send-machine-wip`), point `dns_destroy` at `kl_datagram_teardown`, drop the public `send_slots`
   (internal test hook), and re-run the immediate-`KlEventCtx` teardown probes (readiness + pollcomp +
   io_uring/LSan, including `dns_destroy_with_inflight`) to prove zero leaks.

If review prefers **Option B** (life-owned send holder) despite §1, step 2 implements B instead; the
M3 completion in step 3 is identical either way.
