# R3b-W — Watcher pointer-reuse ABA: remedy design freeze (docs-only)

**Status:** design freeze, **docs-only**. No production code, no test changes here. This document
scopes the remedy for the watcher ABA gap ([R3a](r3a_completion_lifetime_inventory.md) finding 1a),
compares two candidate mechanisms, and recommends one — pausing for review before any implementation.
Sibling docs: the [R3b decision](r3b_lifetime_decision.md) and invariant I5 in
[architecture_invariants.md](architecture_invariants.md).

## The defect (grounded in code)

`kl_event_dispatch` (`include/keel/event_ctx.h`) validates a watcher event by **pointer identity**:

```
KlWatcher *w = (tagged event udata) & ~1;
for (it = ctx->watchers; it; it = it->next) if (it == w) { live = 1; break; }
if (!live) return 1;   /* stale — consumed */
w->on_ready(w->fd, event->ready, w->user_data);
```

`kl_watcher_add`/`kl_watcher_del` (`src/event_ctx.c`) allocate/free each node with same-size
`kl_malloc`/`kl_free`. Batches are drained then dispatched one-by-one (`kl_comp_run` in
`src/completion_core.c`; `kl_event_ctx_run` in `src/async.c`). The ABA:

```
batch = [ evA(→A), evB(→B), ... ]        # both already drained
dispatch evA → A.on_ready:
    kl_watcher_del(B)     → B unlinked + B's node kl_free'd
    kl_watcher_add(Cfd)   → kl_malloc returns B's just-freed address → C lives at &B
dispatch evB → scan finds a live node at &B (it is C) → C.on_ready(B.fd, evB.ready, C.user_data)
```

B's stale event is **misdelivered to C**. Properties:
- **Not a UAF** — the dispatched node (C) is live; no freed memory is read.
- **Not covered by single-shot** — evB is B's one legitimate completion; single-shot holds.
- **Realistic** — a pool/arena/default allocator recycling a just-freed same-size block makes the
  address reuse near-certain.
- **Scope: watcher only.** The same scan serves connect (`KL_COMP_CONNECT` targets a watcher node),
  but an aborted connect op is dropped *at drain*, before it becomes a dispatched event
  (`src/event_iouring.c:344`), so no stale connect event reaches the scan. Watcher has no such
  drain-time suppression.

## Requirements a remedy must meet

1. **Correctness:** a stale event whose original watcher was deleted must never be delivered to a
   *different* watcher, even under address reuse within the same batch.
2. **No UAF regression:** must remain safe against the original freed-node case.
3. **Hot-path cost (I8):** no per-event allocation; ideally no growth of the core `KlEvent` struct.
4. **ABI:** avoid changing the installed public event ABI if possible (`KlEvent`,
   the tagged-pointer encoding).
5. **Both engines:** the fix must hold for readiness (`kl_event_ctx_run`) and completion
   (`kl_comp_run`) dispatch, since both use `kl_event_dispatch`.
6. **Reentrancy:** correct when a watcher callback adds/deletes watchers, including self-delete.

## Candidate A — deferred node reclamation to batch end (recommended)

**Idea:** during a dispatch batch, `kl_watcher_del` still *unlinks* the node from `ctx->watchers`
(so the scan already treats it as not-live) but **defers the `kl_free`** to the end of the batch.
While the batch is in flight the node's memory is not returned to the allocator, so a mid-batch
`kl_watcher_add` **cannot** receive that address — ABA is impossible by construction.

**Mechanism (minimal):**
- Add to `KlEventCtx`: a `dispatch_depth` counter and a `retired` singly-linked list of unlinked
  nodes awaiting reclamation.
- `kl_watcher_del`: unlink from `ctx->watchers` and call `kl_event_del` (backend unregister) **now**
  (so no further events are armed); if `dispatch_depth > 0`, push the node onto `retired` instead of
  freeing; else free immediately (unchanged behavior outside a batch).
- The two dispatch loops bump `dispatch_depth` around the per-event dispatch loop and, on exit to
  depth 0, drain `retired` with `kl_free`.
- The scan is **unchanged** — it already returns "not live" for an unlinked node; the fix only
  prevents that unlinked node's address from being reused mid-batch.

**State / ownership:**

| Node state | In `ctx->watchers`? | Memory owner | Scan verdict |
|---|---|---|---|
| live | yes | ctx | dispatched |
| deleted mid-batch | no (unlinked) | ctx `retired` list (freed at depth→0) | not live ⇒ stale event consumed |
| deleted outside batch | no | freed immediately | not live |

**Failure paths:** allocation is not on the del path (only a list push); `kl_event_del` failure is
handled as today; nested/reentrant dispatch reclaims only at the outermost batch boundary
(depth→0), so a callback that runs its own tick does not free a parent batch's nodes early.

**Compatibility:** internal only — no `KlEvent`/tagged-pointer/public-ABI change; `KlEventCtx` gains
two private fields (it is not a stack-ABI-frozen public type in the same sense as the transports).
**Backends:** engine-agnostic — the change lives in `event_ctx.c` + the two dispatch loops; no
backend (`event_epoll/kqueue/poll/iouring/iocp/pollcomp`) is touched.

**Cost:** one extra pointer per *deleted-during-batch* node for the duration of a batch; no per-event
or per-live-watcher cost; no hot-path field on `KlEvent`. Reclamation is O(deleted-this-batch).

## Candidate B — stable watcher identity / generation

**Idea:** give each watcher a generation (a ctx counter bumped on node reuse, or a generational
slotmap index). The event carries `(identity, generation)`; the scan matches both, so a reused
address with a new generation fails the match and the stale event is dropped.

**Why it is heavier here:**
- The event's reference to a watcher is a **tagged pointer** (LSB=1) with no room for a generation.
  Carrying one requires either (i) adding a generation field to the event — but readiness dispatch
  flows through `KlEvent.udata`, so this grows the **core `KlEvent` struct** (ABI/ size, req. 4), or
  (ii) replacing the heap-node registry with a **generational slotmap** and encoding a
  `(slot, gen)` handle into the tagged word — a larger rework of `kl_watcher_*` and every site that
  reads `event->udata`/`target` as a `KlWatcher*`.
- More surface, more invariants (generation overflow policy, slot recycling), for the same
  correctness result A achieves structurally.

**When B would be preferable:** if a future need arises for a *stable watcher identity that survives
across batches* (e.g. cross-thread references, or handing a watcher id to external code), the
generational handle becomes worth its cost. No current code needs that.

## Comparison

| Criterion | A — deferred reclamation | B — identity/generation |
|---|---|---|
| Defeats ABA | yes (address not reusable mid-batch) | yes (gen mismatch) |
| UAF-safe | yes | yes |
| Hot-path / `KlEvent` growth | none | grows `KlEvent` **or** reworks registry + encoding |
| Public ABI impact | none | likely (`KlEvent`) or large internal rework |
| TUs touched | `event_ctx.c` + 2 dispatch loops | registry + all `event->udata`→watcher readers + encoding |
| Complexity / new invariants | low (defer + reclaim at depth 0) | higher (gen overflow, slot recycling) |
| Backends touched | none | none, but touches shared event encoding |

## Recommendation

**Candidate A (deferred reclamation to batch end).** It is the smallest change that makes the ABA
*structurally impossible*, needs no `KlEvent`/ABI change, no hot-path field, and touches only
`event_ctx.c` and the two dispatch loops. Candidate B is recorded as the heavier alternative to
revisit only if a future requirement needs cross-batch stable watcher identity.

## Implementation & validation plan (separate, reviewed increments — not done here)

1. **R3b-W-impl** (production): implement Candidate A in `src/event_ctx.c` + `src/completion_core.c`
   (`kl_comp_run`) + `src/async.c` (`kl_event_ctx_run`). No public-header change expected.
2. **R3b-T2 part 2** (test): the ABA regression — delete B, force same-address C via a fixture
   allocator, dispatch B's captured event, assert no misdelivery; ASan-clean; run over readiness and
   (enrolled) io_uring. It should **fail before** R3b-W-impl and **pass after**.
3. On landing, flip invariant I5's watcher caveat from "open gap" to "resolved (deferred
   reclamation)", and update R3a 1a / R3b accordingly.

No production or test change is made in this freeze; it pauses here for review of the chosen remedy.
