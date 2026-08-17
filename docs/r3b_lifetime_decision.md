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
   was NOT preserved as-is** — it was UAF-safe but ABA-unsafe (misdelivered a stale event when a
   freed node's address was reused mid-batch). Its remedy was chosen in R3b-W
   ([r3b_w_watcher_aba_remedy.md](r3b_w_watcher_aba_remedy.md)) — Candidate A, batch-bracketed
   deferred reclamation — now **implemented and regression-tested** (R3b-T2,
   `tests/test_watcher_aba.c`). **RESOLVED.**
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
| **Watcher** — ctx-list scan + R3b-W bracketed deferred reclamation | `tests/test_async.c` (add/mod/del/soak) **+ `tests/test_watcher_aba.c`** (R3b-T2): deterministic same-address-reuse ABA over readiness, completion, and the server loop, plus nested-bracket / unmatched-end / overflow-refusal / depth-0 reclamation | **Resolved.** The ABA (R3a 1a) is fixed by R3b-W and covered by a regression demonstrated to fail against the pre-fix behavior and pass at HEAD |

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

### R3b-T2 — ABA regression **(DONE — `tests/test_watcher_aba.c`)**

- **What (delivered):** a deterministic test using a fixture allocator (a LIFO free-list keyed on
  `sizeof(KlWatcher)` that hands a just-freed watcher node back to the very next watcher allocation,
  forcing same-address reuse). It drives the ABA — A deletes B, adds C at B's address, then B's
  already-captured stale event is dispatched — over **all three loops**: readiness (mock `.wait`),
  completion (mock `.drain` → `kl_comp_run`, `KL_COMP_WATCHER`), and the **real server loop**
  (`kl_server_run`, probe inside the batch). Asserts no address reuse and no misdelivery. Plus
  nested-bracket reclaim-at-outermost, unmatched-`end` no-op, overflow refusal, and depth-0 immediate
  free.
- **Demonstrated regression:** simulating the pre-fix behavior (`kl_watcher_del` freeing immediately
  regardless of depth) makes the readiness/completion/server + nested cases **FAIL**; at HEAD all
  pass, ASan/UBSan/LSan-clean.
- **Scope guard:** test + fixture allocator only; the dispatch/list change itself is R3b-W's remedy
  increment, reviewed separately.

## Non-goals reaffirmed (R3b)

- No `KlOpLife` and no stream generation token (decision 1–2).
- No production-code change and **no test change in R3b** — R3b is the frozen decision + coverage
  plan only; R3b-T1/T2 are separate increments.
- No change to the doc-reference gate scope or any Makefile/CI target.

## Definition of done for R3 (increments land separately)

R3 status: the watcher ABA remedy is designed (R3b-W), implemented (R3b-W-impl), and
regression-tested (R3b-T2, `tests/test_watcher_aba.c` — **DONE**, green at HEAD, fails pre-fix). The
only remaining R3 item is **R3b-T1** (assert the single-shot stream contract through a real seam),
independent and landable anytime. Every other class mechanism remains covered as inventoried above.
One lifetime remedy (watcher ABA) *was* required, contrary to the pre-review "no remedy" reading — the other four
classes need none.
