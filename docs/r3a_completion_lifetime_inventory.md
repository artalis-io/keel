# R3a — Completion-target lifetime inventory (read-only)

**Status:** R3a of [keel_improvement_roadmap.md](keel_improvement_roadmap.md) — **inventory only, no
remedy.** This document traces, from actual code, every object a completion event can reference, how
its lifetime is protected, and whether each known-dangerous sequence is prevented. It proposes **no
code change**: choosing the smallest remedy for any gap is R3b, a separate reviewed increment.

Method: traced from source (not comments or historical audits), one target class at a time, across
the completion backends (pollcomp, io_uring, IOCP, lwIP-raw, EFI). Related invariants:
[architecture_invariants.md](architecture_invariants.md) I3 (logical close ≠ physical retirement),
I5 (batch targets outlive referencing events), I9 (quarantine on uncertain retirement).

## How a completion event names its target

`KlCompletionEvent` (`src/completion.h`) reaches its owner two structurally different ways:

- **Raw `target` pointer** (KL_COMP_READ/WRITE → `KlStream*`; KL_COMP_ACCEPT → `NULL`, server
  recovered from ctx; KL_COMP_CONNECT/WATCHER → a tagged `KlWatcher`/udata). Validity rests on
  **ordering + a liveness discipline**, not on a token embedded in the event.
- **Stable liveness token** `life` (`struct KlDgramLife *`) for KL_COMP_DGRAM_RECV/SEND, plus
  `retain_life` (borrow-vs-transfer). The datagram op recovers its owner through the token and
  touches it only while live; a stale token yields a NULL owner and is safely dropped.

**This is the central finding of R3a:** the datagram class carries a generation/liveness token and a
quarantine flag; the other four classes do not — they are kept safe by *different, class-specific*
mechanisms (inflight-pin, reentrancy-depth guards, exactly-one-completion-per-op, a watcher-list
liveness scan). No live UAF exists in any class. One of those guards was initially **not sufficient**:
the watcher pointer-scan is UAF-safe but had an **ABA misdelivery gap** under same-batch node
address-reuse (finding 1a) — now **RESOLVED** by R3b-W (batch-bracketed deferred reclamation,
regression `tests/test_watcher_aba.c`). So the picture is: the stream raw-`containerof` is a
defense-in-depth question (safe today under single-shot + inflight-pin); the watcher ABA is fixed;
the other three classes (datagram, accept, connect) are sound.

## Inventory table

| Target class | Event owner repr. | Ref / lease mechanism | Same-batch destruction | Cancel path | Retirement proof | Quarantine |
|---|---|---|---|---|---|---|
| **Datagram op** (DGRAM_RECV/SEND) | stable `life` token (`KlDgramLife*`), never a transport deref | generation token + refcount; `retain_life` borrow flag | token → NULL owner if dead ⇒ dropped | `kl_comp_cancel_dgram` (kind-specific) | `retire_dgram` classifier: PENDING / RETIRED / QUARANTINED | **yes** — `retain_life=1` retains ref fail-closed (EFI) |
| **Stream read/write** (READ/WRITE) | raw `KlStream*`; HTTP recovers `KlConn` via `kl_conn_of_stream` containerof | **none on the stream** — raw pointer + inflight-pin | pinned: `stream_fully_retired` blocks detach while either op inflight (`src/stream_close.c:36-38`) | `kl_stream_cancel` / timeout `kl_comp_cancel`; `in_close_cancel` depth defers finalize | `recv_inflight`+`send_inflight` counters; `on_close` fires only when both 0 (`src/stream_close.c:23-50`) | implicit: unretired op pins the conn (fail-stop pin, not UAF). The timeout sweep *initiates* cancel but cannot retire a genuinely-lost terminal — the pin holds until backend/context teardown |
| **Listener accept** (ACCEPT) | `target=NULL`; server via `server_of_ctx` containerof (always valid on a server loop) | `KlSlotLease` by value (release fn+ctx baked in) + nullable `alive` liveness token | `in_dispatch` depth defers finalize; second accept in CLOSING state is disposed, no callback (`src/listener.c`) | readiness disarm + return credits; completion `cancel_accept` once, force-complete via shutdown/quiesce | per-listener `inflight` counter; `on_close` only when `inflight==0` & not in dispatch | none — accepts force-completed + reaped before free |
| **Connect attempt** (CONNECT) | tagged `KlWatcher` (detached node), Happy-Eyeballs races several | none/token; abort is **physical** (backend frees/skips aborted op) | winner's cancel marks losers aborted; drain skips aborted before emit; `in_dispatch`/`in_start` depth defers detach (`src/connect_op.c`) | first-wins + deadline timer → `co_request_cancels`; `kl_comp_cancel` marks aborted | `terminal`+`co_finalize`: all attempts cleared, timers disarmed, resolve retired | none — op owned by KlClient, reused via reset |
| **Watcher** (WATCHER) | tagged `KlWatcher` udata | none — ctx-owned node | `kl_event_dispatch` **pointer-scans** `ctx->watchers` (`include/keel/event_ctx.h`): UAF-safe (never derefs a freed node), **but ABA-exposed** — a freed node's address reused by a new watcher in the same batch misdelivers the stale event (finding 1a; remedy R3b-W) | `kl_watcher_del` unlinks+frees synchronously; `dispatch_dirty` suppresses auto-rearm | node unlinked from ctx list before `kl_event_del`; backend fires no further events | none — freed synchronously |

## Dangerous-sequence matrix

Verdicts: **P** = prevented (guard cited) · **N/A** = not applicable · **DiD** = prevented today but
relies on a class-specific guarantee rather than a generation token (defense-in-depth note).

| Sequence | Datagram | Stream | Accept | Connect | Watcher |
|---|:--:|:--:|:--:|:--:|:--:|
| (a) two events/batch, first frees second's target | P (token) | P (inflight-pin) | P (`in_dispatch`+CLOSING) | P (abort-skip) | P (scan + R3b-W bracketed deferred reclamation; ABA **resolved**, 1a) |
| (b) synchronous completion during submit/arm | P (arm trampoline) | P (`completed_inline`, flag before arm) | P (`before=inflight` sync detect) | P (depth guards) | N/A |
| (c) cancel completes inline | P (retire classifier) | P (`in_close_cancel` depth) | P (`in_dispatch`) | P (`cancel_requested` set first) | N/A |
| (d) late completion after logical close | P (token→NULL owner) | P (`read_closed`; drop, still retire) | P (CLOSING ⇒ dispose) | P (`terminal` early-return) | P (list scan) |
| (e) fd/handle reuse | P (token generation) | **DiD** (raw containerof; safe via inflight-pin + single-shot) | P (no reuse window before detach) | P (client owns fds; explicit dispose) | P (keyed by fd; idempotent add) |
| (f) backend teardown with ops registered | P (release/quarantine) | P (quiesce/drain on loop close) | P (shutdown+quiesce reaps all) | P (client disarms; ctx frees watchers) | P (ctx frees all watchers) |

## Findings

1. **No live UAF found in any class**, but **one real correctness gap** (see finding 1a). Every
   enumerated sequence is UAF-safe; the mechanisms differ by class and each is honest to its model
   (I2): datagram = generation token; stream = inflight-pin + confirmed-detachment gate; accept =
   credit-lease + `in_dispatch` + force-reap; connect = physical-abort + depth guards; watcher =
   ctx-list liveness scan.

1a. **Watcher pointer-reuse ABA — a real stale-event *misdelivery* gap (not a UAF).** The
   `kl_event_dispatch` liveness scan (`include/keel/event_ctx.h`) matches a watcher by **pointer
   identity** (`it == w`) against `ctx->watchers`, and `kl_watcher_add`/`del` use same-size
   `kl_malloc`/`kl_free` (`src/event_ctx.c`). So within one drained batch: event A dispatches → A
   deletes watcher B (B's node freed) → A adds watcher C → the allocator hands C **B's freed
   address** → B's stale event, still later in the batch, is scanned → the scan finds a *live* node
   at B's address (now C) → **B's stale event is dispatched to C's callback** with B's fd/mask. This
   is misdelivery, not a UAF (no freed memory is touched). **Single-shot does not prevent it** — the
   stale event is B's one legitimate completion from B's old arm. It is realistic (near-certain under
   a pool/arena allocator that recycles a just-freed same-size block). Scope: this is a property of
   the shared tagged-watcher scan, but the **connect** class is additionally protected by drain-time
   abort-skip (an aborted connect op is freed in the drain loop *before* it becomes a dispatched
   event — `src/event_iouring.c:344`), so the **watcher class is the one exposed**. **RESOLVED** by
   R3b-W ([r3b_w_watcher_aba_remedy.md](r3b_w_watcher_aba_remedy.md)) — batch-bracketed deferred node
   reclamation (a deleted node's address cannot be reused mid-batch). Regression:
   `tests/test_watcher_aba.c`, demonstrated to fail against the pre-fix behavior and pass now.

2. **The one asymmetry — sequence (e) for the stream class.** `KL_COMP_READ/WRITE` carries a raw
   `KlStream*` and the HTTP adapter recovers `KlConn` by containerof with **no generation guard**.
   This is safe *today* because two independent facts hold together: (i) `stream_fully_retired`
   pins the conn — it cannot be released/reused while either op is inflight (`src/stream_close.c:36-38`,
   detach/reuse legal only after `on_close`, `:23-27`); and (ii) every completion backend is
   **single-shot** — one submission yields exactly one completion (`src/event_iouring.c` IOU_READ/
   WRITE, IOCP one OVERLAPPED per op, pollcomp synchronous). A duplicate/late completion *after*
   retirement — the only way to hit fd/slot reuse — cannot arise under (ii). The datagram class does
   not depend on this pair; its token makes a stale completion a safe no-op regardless. So the stream
   class has **less defense-in-depth**, not a bug.

3. **Quarantine exists only where retirement is genuinely unprovable** (I9): the datagram/EFI
   `retain_life=1` path. Stream/accept/connect/watcher retire deterministically (inflight counters,
   force-reap, synchronous free), so they need no quarantine under a *correctly* terminating backend.
   Note the failure mode of a genuinely-lost terminal completion (a backend defect): the timeout
   sweep *initiates* cancellation, but it cannot retire an op whose terminal never arrives, so the
   result is a persistent **fail-stop** pin/hang that holds until backend or context teardown — still
   never a UAF, but not automatically cleared.

4. **Reentrancy and synchronous completion are handled uniformly but via different primitives** —
   arm trampoline (datagram/stream read), `completed_inline`/`before=inflight` sync-detect (stream/
   accept), `in_dispatch`/`in_start`/`in_close_cancel` depth counters (stream/accept/connect). No
   class recurses unboundedly or double-fires its terminal.

## Hand-off to R3b (decisions, not made here)

R3b chooses the smallest remedy per class, if any. R3a records the open questions:

- **Watcher ABA (finding 1a) — RESOLVED.** The one place R3 concluded "preserve as-is" was not
  acceptable. R3b-W ([r3b_w_watcher_aba_remedy.md](r3b_w_watcher_aba_remedy.md)) chose Candidate A
  (batch-bracketed deferred node reclamation) over a stable identity/generation; implemented and
  regression-tested (`tests/test_watcher_aba.c`, R3b-T2).
- **Stream (e) defense-in-depth.** Is the "inflight-pin + single-shot backend" pair a strong enough
  contract to leave as-is, or should the stream class gain a lightweight generation check so a
  hypothetical duplicate/late completion is a safe no-op like datagram? Weigh against the cost of a
  hot-path field (I8) and the fact that no current backend can emit a second completion.
- **Shared `KlOpLife`?** Only datagram needs a generation token today. Per roadmap R3b, a shared
  internal lifetime token is justified **only if ≥2 target classes need identical semantics** —
  R3a finds they do **not** (stream/accept/connect/watcher each have a sufficient, cheaper,
  class-specific guard). Recommendation to R3b: do **not** generalize the token pre-emptively;
  revisit only if the stream (e) question is answered "add a generation guard," and even then prefer
  a stream-local one unless a second class needs it.
- **Regression coverage.** R3b should add a deterministic regression for any sequence it changes; the
  pollcomp double already exercises (a)–(d) and (f) for datagram/stream, and is the natural host for
  a scripted duplicate-completion test if the stream (e) question is pursued.

No code was changed in R3a.
