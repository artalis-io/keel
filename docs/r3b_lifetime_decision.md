# R3b — Completion-target lifetime: decision + coverage freeze (docs-only)

**Status:** R3b of [keel_improvement_roadmap.md](keel_improvement_roadmap.md) — **design freeze,
docs-only.** No production-code and no test changes are made here. R3b records the decision that
follows from the [R3a inventory](r3a_completion_lifetime_inventory.md), inventories the existing
regression coverage per lifetime mechanism, and proposes the *smallest* missing coverage — each as a
later, independently-reviewed increment.

## Decision (frozen)

R3a found no live UAF, one defense-in-depth asymmetry (stream), **and one real correctness gap
(watcher pointer-reuse ABA, R3a finding 1a)**. The decision is therefore **"no remedy for four
classes; one required remedy for the watcher ABA, frozen separately":**

1. **No generalized `KlOpLife`.** No target class other than datagram needs a stable liveness token;
   the shared-token bar (≥2 classes needing identical semantics) is not met. *Note:* the watcher ABA
   remedy is evaluated on its own in R3b-W and must not be conflated with a generalized op token.
2. **No stream-local generation token today.** The stream raw-`containerof` recovery is safe under the
   two facts R3a verified (inflight-pin + single-shot backends). Adding a per-op generation field
   would cost a hot-path word (invariant I8) to guard a sequence no current backend can produce.
3. **Preserve four class-specific lifetime mechanisms as-is** (datagram = `KlDgramLife` token +
   quarantine; stream = inflight-pin + confirmed-detachment gate; accept = credit-lease +
   `in_dispatch` + force-reap; connect = physical-abort + depth guards). **The watcher ctx-list scan
   is NOT preserved as-is** — it is UAF-safe but ABA-unsafe (misdelivers a stale event when a freed
   node's address is reused mid-batch). Its remedy is chosen in R3b-W
   ([r3b_w_watcher_aba_remedy.md](r3b_w_watcher_aba_remedy.md)), a separate docs-only freeze;
   production code stays out until that freeze is reviewed.
4. **Single-shot completion is a load-bearing stream contract.** The safety of the stream
   raw-`containerof` recovery depends on every completion backend emitting **exactly one** completion
   per submitted op (no duplicate, no post-retirement completion). Stated explicitly in
   [architecture_invariants.md](architecture_invariants.md) I5. **This does not cover the watcher
   ABA** — the stale watcher event *is* B's one legitimate single completion; single-shot is
   satisfied and the misdelivery still occurs. The watcher gap is orthogonal to single-shot.
5. **Define the smallest missing regression coverage per mechanism** (below).
6. **Split any resulting test additions into later, independently-reviewed increments** (R3b-T1,
   R3b-T2). They are proposed here, not implemented. R3b-T2's watcher test must include the ABA
   address-reuse case — a delete-only test would miss it.

## Coverage inventory (per mechanism)

Traced against the current suites. "Covered" = a deterministic test already pins the mechanism and
its dangerous sequences (R3a (a)–(f)); "Gap" = the smallest missing piece.

| Class / mechanism | Existing coverage (representative cases) | Verdict |
|---|---|---|
| **Datagram** — token + quarantine | `tests/test_datagram_life.c` (`op_outlives_owner`, `many_ops_final_once`, `mark_dead_then_release`, `idempotent_and_null_safe`); `tests/test_dgram_close.c` (graceful/abortive/reentrant/error-sync+async/close-from-callback + classifier `one_quarantined_is_quarantined`, `quarantine_dominates_transport_error`, `pending_stays_closing_then_detaches`); `tests/test_datagram_public.c` (fixed-slot FIFO, pause-holds-one, registration ordering, `alloc_failure_during_prep_leaves_fd_unregistered`); `tests/test_datagram_live.c` | **Covered** — no addition |
| **Stream** — inflight-pin + confirmed-detachment | `tests/test_stream_close.c` (graceful waits for recv/send/both, `abortive_sync_cancel_detaches_exactly_once`, `abortive_async_cancel_detaches_on_later_completions`, reentrant-cancel-once, escalation, idempotent double-begin); `tests/test_stream_read.c` (`completion_close_leaves_inflight_until_retirement`, `duplicate_completion_dropped`, `duplicate_completion_while_held_preserves_held`, `terminal_callback_cannot_restart`, bounded sync completions); `tests/test_stream.c` (`free_refused_while_send_in_flight`, `completion_one_send_in_flight_then_pump`) | **Covered for sequences (a)–(d),(f)** — **Gap on the single-shot contract itself** (see R3b-T1) |
| **Accept** — credit-lease + `in_dispatch` + force-reap | `tests/test_listener.c` (reentrant close from arm/release/reserve hooks, `double_lease_release_is_noop`, `completion_close_cancels_and_waits_for_straggler`, `spurious_accept_disposed`, `close_during_accept/dispose_callback_defers_detach`, window top-up/exhaustion, `credit_conservation_through_lifecycle`, `accepted_stream_lease_outlives_listener`, `lease_liveness_guard_noops_when_pool_gone`) | **Covered** — no addition |
| **Connect** — physical-abort + depth guards | `tests/test_connect_op.c` (`happy_eyeballs_winner_cancels_loser_once`, `straggler_connect_after_win_does_not_refire`, `duplicate_completions_dropped`, `stale_delay/deadline_event_dropped`, reentrant-cancel-once, `straggler_fd_disposed`, `deadline_fails_and_cancels_all_pending`); `tests/test_client_happy_eyeballs.c` (`he_detach` winner/loser, cancel-during-dns, multi-attempt) | **Covered** — no addition |
| **Watcher** — ctx-list liveness scan | `tests/test_async.c` (`watcher_add_fires_on_read`, `watcher_mod_changes_interest`, `watcher_del_stops_events`, `watcher_del_not_found`, `watcher_mask_churn_soak`; runs over both readiness and io_uring) | **Gap + real defect.** No same-batch cross-delete test exists (every `kl_watcher_del` is before-the-event or across ticks). More importantly, the scan itself is **ABA-unsafe** (R3a 1a) — a remedy is required (R3b-W), and R3b-T2 must cover the address-reuse case, not just delete-only |

## Proposed test increments (later, independently reviewed — NOT implemented here)

Two focused additions cover the two gaps. Each is a separate review unit with its own scope; neither
is written in R3b.

### R3b-T1 — pin single-shot as the stream contract (through a real seam)

- **What:** a deterministic test that drives the single-shot contract **through a real completion
  seam**, not a hand-written script. A scripted mock that emits one event only proves its own script;
  it does not exercise the driver's actual submit→complete→retire path. **pollcomp** is the
  deterministic contract host — its `poll()`-driven drain lets a test submit a stream op, complete it
  once, retire it, and assert the driver produces **no second completion** for the retired op — with
  the whole path under ASan. Native **io_uring** (and **IOCP** in Windows CI) enroll the same
  scenario as validation on the production seams.
- **Why smallest:** it makes decision bullet 4 enforceable on the code path that actually matters
  (the seam), without adding any production code or hot-path field.
- **Scope guard:** does not add a generation token; does not change the stream machine.
- **Backends:** pollcomp = the deterministic oracle; io_uring/IOCP = native validation.

### R3b-T2 — same-batch watcher cross-delete **including the ABA reuse case**

- **What:** a deterministic test with two parts. **(1) Delete-only:** watcher A's callback deletes
  watcher B (and a self-delete variant) whose event is already in the same drained batch — asserts the
  scan drops the stale event, ASan-clean. **(2) ABA reuse (the load-bearing case):** A deletes B,
  then A *adds* watcher C forcing the allocator to reuse B's freed address (a fixture allocator that
  hands back the just-freed same-size block makes this deterministic), then B's already-captured event
  is dispatched — asserts B's stale event is **not** misdelivered to C. Under the current code this
  part **fails** (documents the R3a 1a defect); it turns green only once the R3b-W remedy lands, so
  R3b-T2 is sequenced *with* that remedy, not before it.
- **Why it must include (2):** a delete-only test passes today and would give false confidence — it
  misses the ABA entirely (reviewer finding). Part (2) is the actual regression for the defect.
- **Scope guard:** test + fixture allocator only; the dispatch/list change itself is R3b-W's remedy
  increment, reviewed separately.

## Non-goals reaffirmed (R3b)

- No `KlOpLife` and no stream generation token (decision 1–2).
- No production-code change and **no test change in R3b** — R3b is the frozen decision + coverage
  plan only; R3b-T1/T2 are separate increments.
- No change to the doc-reference gate scope or any Makefile/CI target.

## Definition of done for R3 (increments land separately)

R3 is complete when: the watcher ABA remedy is designed (R3b-W), reviewed, implemented, and the
ABA regression (R3b-T2 part 2) is green on it; the single-shot stream contract is asserted through a
real seam (R3b-T1); the watcher same-batch delete-only scan has a regression (R3b-T2 part 1); and
every other class mechanism remains covered as inventoried above. Sequencing: **R3b-W (remedy design)
→ its implementation increment → R3b-T2**; R3b-T1 is independent and may land anytime. One lifetime
remedy (watcher ABA) *is* required, contrary to the pre-review "no remedy" reading — the other four
classes need none.
