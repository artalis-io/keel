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
`kl_malloc`/`kl_free`. Batches are drained then dispatched one-by-one by **three** internal loops —
`kl_comp_run` (`src/completion_core.c`), `kl_event_ctx_run` (`src/event_ctx.c`), **and the readiness
server loop `src/server.c:381-416`** — and `kl_event_dispatch` is itself a **public inline** in the
installed `include/keel/event_ctx.h`, so external code can drain its own batch and call it directly.
The ABA:

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
5. **All three internal loops:** the fix must hold for readiness (`kl_event_ctx_run`), completion
   (`kl_comp_run`), **and the readiness server loop (`src/server.c`)** — all three drain a batch and
   call `kl_event_dispatch`, and server-owned watchers ride the third one.
6. **Public direct-dispatch path:** `kl_event_dispatch` is a public inline; the freeze must state what
   happens when external code drains a batch and calls it directly, and either make that safe or
   explicitly bound it.
7. **Reentrancy:** correct when a watcher callback adds/deletes watchers, including self-delete, and
   when dispatch nests (a callback runs its own tick).

## Candidate A — deferred node reclamation to batch end

**Idea:** during a dispatch batch, `kl_watcher_del` still *unlinks* the node from `ctx->watchers`
(so the scan already treats it as not-live) but **defers the `kl_free`** to the end of the batch.
While the batch is in flight the node's memory is not returned to the allocator, so a mid-batch
`kl_watcher_add` **cannot** receive that address — ABA is impossible by construction.

**Central begin/end helpers (no per-loop duplication).** Add one pair, used by every batch dispatcher:

```
int  kl_event_ctx_dispatch_begin(KlEventCtx *ctx);   /* 0 ok; -1 at INT_MAX depth (overflow guard) */
void kl_event_ctx_dispatch_end(KlEventCtx *ctx);     /* if (--dispatch_depth == 0) drain retired */
```

`begin` is **checked**: it returns -1 rather than incrementing past `INT_MAX` (these are public helpers,
so pathological/unbalanced nesting must not trigger signed-overflow UB). On -1 the caller MUST NOT
dispatch the batch and MUST NOT call `end`. All three internal loops propagate the failure — they skip
dispatching the drained batch and surface an error (the server loop fail-stops its run loop).

- `kl_watcher_del`: unlink from `ctx->watchers` and call `kl_event_del` (backend unregister) **now**
  (so no further events are armed); if `dispatch_depth > 0`, thread the node onto the `retired` list
  **through its existing `next` field** and defer; else `kl_free` immediately (today's behavior).
- The scan is **unchanged** — it already returns "not live" for an unlinked node; the fix only stops
  that address being reused before the batch ends.

**All three internal loops route through the helpers (requirement 5):** `kl_comp_run`,
`kl_event_ctx_run`, and the `src/server.c` readiness loop each call `dispatch_begin` before their
per-event loop and `dispatch_end` after — so server-owned watchers on the third loop are covered.
The helpers are the single source of the nesting/cleanup logic; the three call sites hold no bespoke
depth or reclaim code.

**Public direct-dispatch decision (requirement 6) — option (a): a documented batch contract.**
`kl_event_dispatch` stays public and unchanged. The begin/end helpers become the **public batch
contract**: a caller that drains ≥2 events and calls `kl_event_dispatch` per event must bracket the
loop with `dispatch_begin`/`dispatch_end` to be ABA-safe. A single `kl_event_dispatch` outside a
batch is inherently safe (a del in its callback runs at depth 0 → frees immediately, and there is no
sibling event to go stale). **Compatibility:** existing external multi-event direct-dispatch callers
that do not bracket are **no more exposed than they are today** (no regression) and opt into the fix
with two calls; all *Keel-internal* loops are fixed unconditionally. This does **not** transparently
fix un-bracketed external direct batch dispatch — that gap is the price of A, and is the axis on
which Candidate B differs (B needs no caller contract). Option (b) — narrowing/deprecating direct
multi-event dispatch — is the same contract stated as a restriction; option (c) is "choose B".

**`kl_event_ctx_free` handling:** freeing a context **during** batch dispatch is **invalid** — captured
events on the live dispatch stack may still reference retired nodes, so reclaiming them mid-batch would
be unsafe. The precondition is `dispatch_depth == 0`, **asserted in hosted debug builds** (guarded
`#ifndef KEEL_FREESTANDING` so freestanding is unaffected). Defensive draining of `retired` (normally
already empty, having been drained when the outermost bracket closed) is performed **only at depth 0**;
at nonzero depth the list is deliberately left untouched. The reset-and-drain framing is dropped: free
does **not** silently "cover" a missing `dispatch_end`.

**State / ownership:**

| Node state | In `ctx->watchers`? | Memory owner | Scan verdict |
|---|---|---|---|
| live | yes | ctx | dispatched |
| deleted mid-batch | no (unlinked) | ctx `retired` list (freed when the outermost bracket closes, depth→0; or at ctx_free only if depth is already 0) | not live ⇒ stale event consumed |
| deleted outside batch | no | freed immediately | not live |

**Failure paths:** allocation is not on the del path (only a list re-link); `kl_event_del` failure is
handled as today; nesting reclaims only at the outermost boundary (depth→0), so a callback that runs
its own tick does not free a parent batch's nodes early.

**ABI / layout classification (requirement, honest):** `KlEventCtx` has a **public, installed,
caller-allocated layout** (`include/keel/event_ctx.h`; embedded in `KlServer`, stack-allocated by
clients). Adding `int dispatch_depth` and `KlWatcher *retired` is a **layout change, NOT a no-op** —
it is an **additive, appended, pre-release layout change that requires consumers to recompile.** This
is consistent with the branch's existing stance (the header already documents intentional pre-release
ABI breaks); it must be classified as such, not sold as "no ABI impact." No change to `KlEvent`, the
tagged-pointer encoding, or any backend.

**Backends:** none touched — the change lives in `event_ctx.c`, the begin/end helpers, and the three
loop call sites. No `event_epoll/kqueue/poll/iouring/iocp/pollcomp` change.

**Cost (corrected):** exactly **two persistent fields per `KlEventCtx`** (`dispatch_depth`,
`retired`). A retired node reuses its **existing `next` link** to thread the `retired` list, so there
is **no extra pointer per retired node** and no allocation on the del path. No per-event or
per-live-watcher cost; no `KlEvent` growth. Reclamation is O(deleted-this-batch).

## Candidate B — stable watcher identity / generation

**Idea:** give each watcher a generation. The event carries a `(slot, generation)` handle instead of a
raw node address; `kl_event_dispatch` decodes it, looks up the watcher table, and matches the
generation. A reused slot bumps its generation, so a stale handle's generation mismatches and the
stale event is dropped — **per-event, with no batch boundary**.

**Its decisive advantage (requirement 6):** validation is entirely self-contained in
`kl_event_dispatch`, so **any** caller — including external code draining its own batch and calling
the public inline directly, with no begin/end brackets — is **transparently ABA-safe**. This is the
one option that satisfies "existing direct batch dispatch must remain transparently safe" without a
new caller contract.

**Why it is heavier here:**
- The event's reference to a watcher is a **tagged pointer** (LSB=1). To carry a generation without
  growing the core `KlEvent` struct (req. 4), the cleanest form replaces the heap-node registry with
  a **generational slotmap** and packs `(slot, gen)` into the same `uintptr_t` word (LSB tag
  preserved). This reworks `kl_watcher_add/mod/del`, the scan, and **every site + backend arm-path
  that stores/reads the tagged watcher reference** (`event->udata`/`target`), because the stored
  token is now a handle, not a pointer.
- New invariants: generation overflow policy and slot recycling. On **32-bit** the `uintptr_t` bit
  budget is tight (e.g. ~15-bit slot / ~16-bit gen / 1 tag) and must be justified; 64-bit is ample.
- Also a `KlEventCtx` layout change (the `watchers` list head becomes a table pointer + counts), so B
  is **not** ABI-free either — it is a *different* layout change plus an encoding change, versus A's
  two appended fields.

**When B is the right call:** if the project requires the public direct-dispatch path to be
transparently safe with no caller contract, or if a future need arises for a stable watcher identity
that survives across batches (cross-thread references, handing a watcher id to external code). Absent
those, B buys the same intra-Keel correctness A gives, at more surface and a bit-budget constraint.

## Comparison (decision reopened — the axis is the public direct-dispatch path)

| Criterion | A — deferred reclamation | B — identity/generation |
|---|---|---|
| Defeats ABA (intra-Keel) | yes (address not reusable mid-batch) | yes (gen mismatch) |
| UAF-safe | yes | yes |
| **Public direct-dispatch (un-bracketed external caller)** | **not fixed** — needs the begin/end batch contract; unchanged callers no worse than today | **transparently fixed** — per-event, no caller contract |
| Caller contract required | yes (bracket multi-event loops) | no |
| `KlEvent` growth / tagged-encoding change | none | none if slotmap-packed; but changes token meaning at every reader + backend arm-path |
| `KlEventCtx` layout change | yes — 2 appended fields (recompile) | yes — list head → table (recompile) |
| Public ABI classification | additive pre-release layout change | layout **and** encoding change |
| TUs touched | `event_ctx.c` + begin/end helpers + **3** loop sites | registry + scan + every `event->udata`/`target` reader + **backend arm-paths** |
| 32-bit constraint | none | tight `uintptr_t` bit budget (slot/gen) |
| Complexity / new invariants | low (defer + reclaim at depth 0) | higher (gen overflow, slot recycling) |

## Recommendation (conditional — for review)

The choice turns on **one policy question:** *must the public `kl_event_dispatch` be transparently
ABA-safe for an external caller that drains its own multi-event batch without adopting a new batch
contract?*

- **If yes → Candidate B.** Only B makes the un-bracketed public path safe with no caller contract;
  its extra surface (slotmap + encoding + backend arm-paths + 32-bit bit budget) is the price.
- **If no (a batch contract for custom multi-event loops is acceptable) → Candidate A.** A fully fixes
  all three *internal* loops via the central begin/end helpers, leaves un-bracketed external callers
  exactly as safe as today (no regression), and is far smaller — `event_ctx.c` + helpers + three call
  sites, no backend churn, no encoding change, two appended `KlEventCtx` fields.

**Lean, not a decision:** given the branch is pre-release, the internal loops are the dominant
exposure, and custom-loop external callers are a niche that can opt into the bracket API, **A is the
smaller, lower-risk fix** — provided the project accepts the documented batch contract for external
multi-event direct dispatch. If that contract is unacceptable, choose **B**. This freeze does not
force the pick; it resolves the boundaries so review can decide A vs B.

## Implementation & validation plan (separate, reviewed increments — not done here)

1. **R3b-W-impl** (production): implement the chosen candidate. For A: add
   `kl_event_ctx_dispatch_begin/end` + the two `KlEventCtx` fields in `src/event_ctx.c` /
   `include/keel/event_ctx.h`; drain `retired` in `kl_event_ctx_free`; route `kl_comp_run`
   (`src/completion_core.c`), `kl_event_ctx_run` (`src/event_ctx.c`), and the `src/server.c` readiness
   loop through the helpers; document the public batch contract on `kl_event_dispatch`. This is an
   **additive pre-release `KlEventCtx` layout change → consumers recompile.**
2. **R3b-T2 part 2** (test): the ABA regression — delete B, force same-address C via a fixture
   allocator, dispatch B's captured event, assert no misdelivery; ASan-clean; run over readiness and
   (enrolled) io_uring, **and** exercise the `src/server.c` server-loop path. It must **fail before**
   R3b-W-impl and **pass after**.
3. On landing, flip invariant I5's watcher caveat from "open gap" to "resolved", and update R3a 1a /
   R3b accordingly.

No production or test change is made in this freeze; it pauses here for review of A vs B and the
public-dispatch policy question.
