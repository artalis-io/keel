# R3b: Completion-target lifetime: decision + coverage freeze (docs-only)

**Status:** R3b of [keel_improvement_roadmap.md](../../roadmap/roadmap.md); **design freeze,
docs-only.** No production-code and no test changes are made here. R3b records the decision that
follows from the [R3a inventory](r3a_completion_lifetime_inventory.md), inventories the existing
regression coverage per lifetime mechanism, and proposes the *smallest* missing coverage, each as a
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
   was NOT preserved as-is**: it was UAF-safe but ABA-unsafe (misdelivered a stale event when a
   freed node's address was reused mid-batch). Its remedy was chosen in R3b-W
   ([r3b_w_watcher_aba_remedy.md](r3b_w_watcher_aba_remedy.md)): Candidate A, batch-bracketed
   deferred reclamation, now **implemented and regression-tested** (R3b-T2,
   `tests/test_watcher_aba.c`). **RESOLVED.**
4. **Single-shot completion is a load-bearing stream contract.** The safety of the stream
   raw-`containerof` recovery depends on every completion backend emitting **exactly one** completion
   per submitted op (no duplicate, no post-retirement completion). Stated explicitly in
   [architecture_invariants.md](../../architecture/invariants.md) I5. **This does not cover the watcher
   ABA**: the stale watcher event *is* B's one legitimate single completion; single-shot is
   satisfied and the misdelivery still occurs. The watcher gap is orthogonal to single-shot.
5. **Define the smallest missing regression coverage per mechanism** (below).
6. **Split any resulting test additions into later, independently-reviewed increments** (R3b-T1,
   R3b-T2). They are proposed here, not implemented. R3b-T2's watcher test must include the ABA
   address-reuse case; a delete-only test would miss it.

## Coverage inventory (per mechanism)

Traced against the current suites. "Covered" = a deterministic test already pins the mechanism and
its dangerous sequences (R3a (a)–(f)); "Gap" = the smallest missing piece.

| Class / mechanism | Existing coverage (representative cases) | Verdict |
|---|---|---|
| **Datagram**: token + quarantine | `tests/test_datagram_life.c` (`op_outlives_owner`, `many_ops_final_once`, `mark_dead_then_release`, `idempotent_and_null_safe`); `tests/test_dgram_close.c` (graceful/abortive/reentrant/error-sync+async/close-from-callback + classifier `one_quarantined_is_quarantined`, `quarantine_dominates_transport_error`, `pending_stays_closing_then_detaches`); `tests/test_datagram_public.c` (fixed-slot FIFO, pause-holds-one, registration ordering, `alloc_failure_during_prep_leaves_fd_unregistered`); `tests/test_datagram_live.c` | **Covered**: no addition |
| **Stream**: inflight-pin + confirmed-detachment | `tests/test_stream_close.c` (graceful waits for recv/send/both, `abortive_sync_cancel_detaches_exactly_once`, `abortive_async_cancel_detaches_on_later_completions`, reentrant-cancel-once, escalation, idempotent double-begin); `tests/test_stream_read.c` (`completion_close_leaves_inflight_until_retirement`, `duplicate_completion_dropped`, `duplicate_completion_while_held_preserves_held`, `terminal_callback_cannot_restart`, bounded sync completions); `tests/test_stream.c` (`free_refused_while_send_in_flight`, `completion_one_send_in_flight_then_pump`) **+ `tests/test_stream_single_shot.c`** (R3b-T1) | **Covered.** Sequences (a)–(d),(f) by the close/read/write suites; the single-shot contract itself by the real-seam regression (pollcomp oracle + native io_uring/IOCP) |
| **Accept**: credit-lease + `in_dispatch` + force-reap | `tests/test_listener.c` (reentrant close from arm/release/reserve hooks, `double_lease_release_is_noop`, `completion_close_cancels_and_waits_for_straggler`, `spurious_accept_disposed`, `close_during_accept/dispose_callback_defers_detach`, window top-up/exhaustion, `credit_conservation_through_lifecycle`, `accepted_stream_lease_outlives_listener`, `lease_liveness_guard_noops_when_pool_gone`) | **Covered**: no addition |
| **Connect**: physical-abort + depth guards | `tests/test_connect_op.c` (`happy_eyeballs_winner_cancels_loser_once`, `straggler_connect_after_win_does_not_refire`, `duplicate_completions_dropped`, `stale_delay/deadline_event_dropped`, reentrant-cancel-once, `straggler_fd_disposed`, `deadline_fails_and_cancels_all_pending`); `tests/test_http_client_happy_eyeballs.c` (`he_detach` winner/loser, cancel-during-dns, multi-attempt) | **Covered**: no addition |
| **Watcher**: ctx-list scan + R3b-W bracketed deferred reclamation | `tests/test_async.c` (add/mod/del/soak) **+ `tests/test_watcher_aba.c`** (R3b-T2): deterministic same-address-reuse full ABA (assert-C-not-called) over readiness, completion, and the real server loop (scripted `KlEventProvider`), plus nested-bracket / unmatched-end / depth-0 reclamation / begin-before-acquire ordering (loop entries at INT_MAX acquire nothing) | **Resolved.** The ABA (R3a 1a) is fixed by R3b-W and covered by a regression demonstrated to fail against both pre-fix states (deferral and ordering) and pass at HEAD |

## Proposed test increments (later, independently reviewed; NOT implemented here)

Two focused additions cover the two gaps. Each is a separate review unit with its own scope; neither
is written in R3b.

### R3b-T1: single-shot stream contract through a real seam **(DONE: `tests/test_stream_single_shot.c`)**

- **What (delivered):** a backend-adaptive test (`kl_event_ctx_init`) that drives the single-shot
  contract **through the real completion seam**: a bare `KlStream` over a **full-width
  `KlSocketHandle`** loopback pair built via the KEEL default socket seam (`kl_sockdef_*`, so the
  handles are real pointer-width, overlapped-capable SOCKETs on IOCP, not int-truncated test
  handles), `kl_comp_post_recv_raw` / `kl_comp_post_send_raw`, driven by `kl_event_ctx_run` →
  `kl_comp_run`, counting completions via `ctx.comp_conn_dispatch` (no scripted mock). For **READ and
  WRITE**: post one op, drain the sole terminal completion, then, without rearming/resubmitting,
  trigger more peer activity and drain again, asserting **no second completion**. **All peer transfers
  and every drain return are asserted** (exact byte counts + nonnegative `kl_event_ctx_run`), so a
  failed peer write or drain cannot masquerade as "no duplicate". On a readiness build (no
  submitted-op model) it skips.
- **Backends:** **pollcomp** = deterministic oracle (a new CI step); **io_uring** = native (enrolled
  in `test-iouring`); **IOCP** = native (enrolled in `test-win-iocp`, Windows CI); the full-width
  handles make that enrollment operate on real overlapped SOCKETs.
- **Demonstrated teeth:** injecting a real second recv makes the counter reach 2 and the assertion
  **FAIL**: the test genuinely detects a second completion. Verified passing over pollcomp (local)
  and io_uring (container); default readiness build skips.
- **Scope guard:** no generation token; no stream-machine or production change (test + CI enrollment
  only).

### R3b-T2: ABA regression **(DONE: `tests/test_watcher_aba.c`)**

- **What (delivered):** a deterministic test using a fixture allocator (a LIFO free-list keyed on
  `sizeof(KlWatcher)` that hands a just-freed watcher node back to the very next watcher allocation,
  forcing same-address reuse). It drives the **full ABA**: A deletes B, adds C at B's address, then
  B's already-captured stale event is dispatched, asserting **C is not called** and C did not reuse
  B's address, over **all three loops**, each fed a deterministic `[A, stale-B]` batch: readiness
  (mock `.wait`), completion (mock `.drain` → `kl_comp_run`, `KL_COMP_WATCHER`), and the **real
  `kl_server_run` loop** via a **scripted readiness `KlEventProvider`** injected through
  `cfg.event_provider` (not a probe). Plus: nested-bracket reclaim-at-outermost, unmatched-`end`
  no-op, depth-0 immediate free, and **begin-before-acquire ordering**: readiness and completion
  loop entries at `dispatch_depth == INT_MAX` return −1 with `.wait`/`.drain` **never called** and no
  scripted event consumed.
- **Demonstrated regressions (both fixes):** (a) simulating the ABA-fix pre-state (`kl_watcher_del`
  freeing immediately regardless of depth) makes readiness/completion/**server**/nested **FAIL**;
  (b) simulating the ordering pre-state (begin *after* acquire) makes both overflow-ordering cases
  **FAIL** (`.wait`/`.drain` gets called). At HEAD all 9 pass, ASan/UBSan/LSan-clean.
- **Scope guard:** test + fixture allocator only; the dispatch/list change itself is R3b-W's remedy
  increment, reviewed separately.

## Non-goals reaffirmed (R3b)

- No `KlOpLife` and no stream generation token (decision 1–2).
- No production-code change and **no test change in R3b**; R3b is the frozen decision + coverage
  plan only; R3b-T1/T2 are separate increments.
- No production build/behavior change. (Test increments R3b-T1/T2 do add test-suite enrollment, new
  suites in the io_uring/IOCP subsets and a pollcomp oracle CI step, which is not production code.)

## Definition of done for R3: **COMPLETE**

R3 is complete: the watcher ABA remedy is designed (R3b-W), implemented (R3b-W-impl), and
regression-tested (R3b-T2, `tests/test_watcher_aba.c`, green at HEAD, fails pre-fix); the single-shot
stream contract is asserted through the real completion seam (R3b-T1, `tests/test_stream_single_shot.c`,
pollcomp oracle + native io_uring/IOCP). Every other class mechanism remains covered as inventoried
above. No lifetime remedy beyond the watcher ABA fix was required; the four other classes need none.
One lifetime remedy (watcher ABA) *was* required, contrary to the pre-review "no remedy" reading; the other four
classes need none.
