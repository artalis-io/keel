# R3b — Completion-target lifetime: decision + coverage freeze (docs-only)

**Status:** R3b of [keel_improvement_roadmap.md](keel_improvement_roadmap.md) — **design freeze,
docs-only.** No production-code and no test changes are made here. R3b records the decision that
follows from the [R3a inventory](r3a_completion_lifetime_inventory.md), inventories the existing
regression coverage per lifetime mechanism, and proposes the *smallest* missing coverage — each as a
later, independently-reviewed increment.

## Decision (frozen)

R3a found no live UAF and one defense-in-depth asymmetry. The remedy decision is therefore
**"no remedy now; preserve and test the existing class-specific contracts":**

1. **No generalized `KlOpLife`.** No target class other than datagram needs a stable liveness token;
   the other four have sufficient, cheaper, class-specific guards. A shared token is justified (per
   roadmap R3b) only if ≥2 classes need identical semantics — they do not.
2. **No stream-local generation token today.** The stream raw-`containerof` recovery is safe under the
   two facts R3a verified (inflight-pin + single-shot backends). Adding a per-op generation field
   would cost a hot-path word (invariant I8) to guard a sequence no current backend can produce.
3. **Preserve the five class-specific lifetime mechanisms** exactly as documented in R3a:
   datagram = `KlDgramLife` token + quarantine; stream = inflight-pin + confirmed-detachment gate;
   accept = credit-lease + `in_dispatch` + force-reap; connect = physical-abort + depth guards;
   watcher = ctx-list liveness scan.
4. **Single-shot completion is a load-bearing stream contract.** The safety of the stream
   raw-`containerof` recovery depends on every completion backend emitting **exactly one** completion
   per submitted op (no duplicate, no post-retirement completion). This is now stated as an explicit
   contract, not an incidental backend property — see [architecture_invariants.md](architecture_invariants.md)
   I5. Any new completion backend must uphold it or supply its own stale-completion guard.
5. **Define the smallest missing regression coverage per mechanism** (below).
6. **Split any resulting test additions into later, independently-reviewed increments** (R3b-T1,
   R3b-T2). They are proposed here, not implemented.

## Coverage inventory (per mechanism)

Traced against the current suites. "Covered" = a deterministic test already pins the mechanism and
its dangerous sequences (R3a (a)–(f)); "Gap" = the smallest missing piece.

| Class / mechanism | Existing coverage (representative cases) | Verdict |
|---|---|---|
| **Datagram** — token + quarantine | `tests/test_datagram_life.c` (`op_outlives_owner`, `many_ops_final_once`, `mark_dead_then_release`, `idempotent_and_null_safe`); `tests/test_dgram_close.c` (graceful/abortive/reentrant/error-sync+async/close-from-callback + classifier `one_quarantined_is_quarantined`, `quarantine_dominates_transport_error`, `pending_stays_closing_then_detaches`); `tests/test_datagram_public.c` (fixed-slot FIFO, pause-holds-one, registration ordering, `alloc_failure_during_prep_leaves_fd_unregistered`); `tests/test_datagram_live.c` | **Covered** — no addition |
| **Stream** — inflight-pin + confirmed-detachment | `tests/test_stream_close.c` (graceful waits for recv/send/both, `abortive_sync_cancel_detaches_exactly_once`, `abortive_async_cancel_detaches_on_later_completions`, reentrant-cancel-once, escalation, idempotent double-begin); `tests/test_stream_read.c` (`completion_close_leaves_inflight_until_retirement`, `duplicate_completion_dropped`, `duplicate_completion_while_held_preserves_held`, `terminal_callback_cannot_restart`, bounded sync completions); `tests/test_stream.c` (`free_refused_while_send_in_flight`, `completion_one_send_in_flight_then_pump`) | **Covered for sequences (a)–(d),(f)** — **Gap on the single-shot contract itself** (see R3b-T1) |
| **Accept** — credit-lease + `in_dispatch` + force-reap | `tests/test_listener.c` (reentrant close from arm/release/reserve hooks, `double_lease_release_is_noop`, `completion_close_cancels_and_waits_for_straggler`, `spurious_accept_disposed`, `close_during_accept/dispose_callback_defers_detach`, window top-up/exhaustion, `credit_conservation_through_lifecycle`, `accepted_stream_lease_outlives_listener`, `lease_liveness_guard_noops_when_pool_gone`) | **Covered** — no addition |
| **Connect** — physical-abort + depth guards | `tests/test_connect_op.c` (`happy_eyeballs_winner_cancels_loser_once`, `straggler_connect_after_win_does_not_refire`, `duplicate_completions_dropped`, `stale_delay/deadline_event_dropped`, reentrant-cancel-once, `straggler_fd_disposed`, `deadline_fails_and_cancels_all_pending`); `tests/test_client_happy_eyeballs.c` (`he_detach` winner/loser, cancel-during-dns, multi-attempt) | **Covered** — no addition |
| **Watcher** — ctx-list liveness scan | `tests/test_async.c` (`watcher_add_fires_on_read`, `watcher_mod_changes_interest`, `watcher_del_stops_events`, `watcher_del_not_found`, `watcher_mask_churn_soak`; runs over both readiness and io_uring) | **Gap: the *same-batch* cross-delete scan is untested** — every existing `kl_watcher_del` happens before the event or across ticks, never from inside another watcher's callback for an event already in the current drained batch (see R3b-T2) |

## Proposed test increments (later, independently reviewed — NOT implemented here)

Two focused additions cover the two gaps. Each is a separate review unit with its own scope; neither
is written in R3b.

### R3b-T1 — pin single-shot as the stream contract

- **What:** a deterministic test (mock/scripted completion driver, no timing sleeps) asserting a
  retired stream op receives **no second completion** — i.e. the driver is single-shot. Pair it with
  a note that the existing `duplicate_completion_dropped` is the stream-machine backstop *on a live
  stream*, and that the drop-guard is not designed to survive across free/slot-reuse (which single-
  shot makes unreachable). The point is to test the property we *rely on* (single-shot), not an
  impossible-to-trigger post-free UAF.
- **Why smallest:** it converts an implicit backend property into an asserted contract without adding
  any production code or hot-path field. It is the minimum that makes decision bullet 4 enforceable.
- **Scope guard:** does not add a generation token; does not change the stream machine.
- **Backends:** runs under pollcomp and (enrolled) io_uring; the scripted-mock variant is host-portable.

### R3b-T2 — same-batch watcher cross-delete liveness scan

- **What:** a deterministic test where watcher A's callback calls `kl_watcher_del` on watcher B (and a
  self-delete variant) whose event is **already in the same drained batch**, asserting the
  `kl_event_dispatch` ctx-list pointer-scan drops the stale event with no deref of freed storage
  (clean under ASan). Add a completion-mode (`KL_COMP_WATCHER`) variant if the existing `test_async`
  fixture makes it cheap.
- **Why smallest:** it is the one R3a dangerous sequence (watcher (a)) with no dedicated regression;
  the guard exists in code but is unexercised by a targeted case.
- **Scope guard:** no change to the watcher list representation or dispatch; test-only.

## Non-goals reaffirmed (R3b)

- No `KlOpLife` and no stream generation token (decision 1–2).
- No production-code change and **no test change in R3b** — R3b is the frozen decision + coverage
  plan only; R3b-T1/T2 are separate increments.
- No change to the doc-reference gate scope or any Makefile/CI target.

## Definition of done for R3 (after R3b-T1/T2 land, separately)

R3 is complete when: the single-shot stream contract is asserted (R3b-T1); the watcher same-batch
scan has a regression (R3b-T2); and every other class mechanism remains covered as inventoried above.
No lifetime remedy beyond the existing class-specific contracts is required.
