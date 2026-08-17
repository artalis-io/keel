# Keel improvement roadmap — harden the axes, do not redesign them

**Status:** proposed roadmap and Claude Code implementation prompt.  This document authorizes no
implementation by itself.  Each code-bearing increment requires its own reviewed design freeze.

**Living docs this roadmap maintains** (produced/kept current by R0):
[architecture.md](architecture.md) (Transport / Engine / Provider entry point),
[architecture_invariants.md](architecture_invariants.md) (enforceable invariants, each anchored to
code/contract/gate), and the historical [audits/README.md](audits/README.md) index.

## Executive direction

Keel's architecture is already on the right boundary:

```text
protocols
    |
    v
KlListener / KlStream / KlDatagram       transport semantics
    |
    +-------------------+----------------+
    |                                    |
    v                                    v
readiness / completion                   socket provider
engine model                             POSIX / Winsock / lwIP / EFI
```

The next phase is consolidation, not reinvention.  Preserve `KlListener`, `KlStream`, and
`KlDatagram` as the canonical Tier-1 transport primitives.  Preserve readiness and completion as
different engine models.  Preserve `KlSocketProvider` as the network-stack axis.  Improve lifetime
safety, reduce legacy ambiguity, remove protocol knowledge from platform backends, and expand
cross-backend conformance evidence.

Architectural assessment at the start of this roadmap: **approximately 8.5/10**.  The risk center is
completion/callback lifetime correctness, not the choice of abstractions.

---

## Claude Code master prompt

Copy the prompt below into a fresh Claude Code session rooted at the Keel repository.

```text
You are working in the Keel C11 networking repository. Read AGENTS.md completely and obey it.
Before changing anything, inspect the current branch, git status, relevant architecture documents,
public headers, backend capability tables, Makefile gates, and tests. Preserve all pre-existing user
changes. Do not push, open or modify a PR, mark a PR ready, merge, or perform any other outward action
unless I explicitly authorize that exact action.

GOAL

Execute the Keel improvement roadmap in docs/keel_improvement_roadmap.md. The governing principle is:

    stop redesigning the major axes; harden them, reduce legacy duplication,
    codify their invariants, and broaden conformance evidence.

The canonical Tier-1 semantic transports are KlListener, KlStream, and KlDatagram. Readiness and
completion remain separate peer engine models. KlSocketProvider remains the network-stack/provider
axis. Integrations implement Keel-owned seams and must not leak dependency-owned types into
include/keel/*.h.

NON-GOALS — DO NOT DO THESE

- Do not rewrite the networking abstraction.
- Do not merge KlEventOps and KlCompletionOps.
- Do not introduce futures, promises, a generic task runtime, or a giant transport vtable.
- Do not re-base KlUdp onto KlDatagram or silently change KlUdp behavior.
- Do not mechanically replace every completion target with a new token.
- Do not change public ABI merely to make the type graph look cleaner.
- Do not mix documentation cleanup, behavioral fixes, and CI/toolchain fixes in one commit.
- Do not claim a backend confidence level that its native runtime tests do not support.

WORKING METHOD

1. Treat every numbered roadmap increment as a separate review unit.
2. For any cross-cutting lifetime, ABI, backend-seam, or ownership change, first write a docs-only
   design freeze containing the exact state transitions, ownership table, failure paths, compatibility
   impact, backend matrix, and validation plan. Pause for review before implementation.
3. Trace behavior from actual code. Do not infer backend semantics from comments or old audits.
4. Prefer mechanical architecture checks and shared contract tests over prose-only promises.
5. Keep hot paths allocation-free. Use KlAllocator for every allocation. Check all arithmetic,
   capacities, pointer inputs, allocation results, system-call results, and cleanup paths.
6. A logical cancel/close is not physical retirement. Any completion-referenced storage must remain
   valid until the backend proves retirement or explicitly quarantines it.
7. Assume callbacks may run synchronously, reentrantly, and earlier in the same drained completion
   batch than another event that references related state.
8. Test failure paths and allocation failure, not only successful round trips.
9. Commit one logical increment at a time with a concise why-focused message and no Co-Authored-By
   trailer. Do not amend an already-reviewed increment unless explicitly asked.
10. At each pause, report: files changed, invariants established, compatibility impact, exact tests and
    native environments run, unverified environments, commit hash, push status, and worktree status.

STARTING TASK

Begin with R0 only: establish the current-state baseline and reconcile the living architecture docs.
Do not start R1 or any code change. Produce the R0 deliverables, validate documentation references and
mechanical checks, commit the docs-only increment if asked, and pause for review.
```

---

## Roadmap rules

### Required invariant vocabulary

Use these terms consistently in code, tests, and documentation:

| Noun | Meaning |
|---|---|
| **Transport** | Semantic contract: `KlListener`, `KlStream`, or `KlDatagram` |
| **Engine** | Execution model: readiness or completion |
| **Provider** | Network stack/platform: POSIX, Winsock, lwIP, or EFI |
| **Driver/adapter** | Code translating a transport state machine onto an engine/provider seam |
| **Integration** | Optional third-party/platform implementation of a Keel-owned interface |
| **Retirement** | Proof that an operation can no longer access its submitted storage |
| **Quarantine** | Fail-closed retention when retirement cannot be proved |

“Event backend,” “completion backend,” and provider-specific names remain valid implementation terms,
but introductory documentation should begin with Transport / Engine / Provider.

### Review cadence

Every increment ends at a review checkpoint.  A later increment must not be folded into an earlier one
for speed.  Native backend validation may be delegated to CI or a purpose-built container/VM, but a
cross-compile alone is not runtime proof.

---

## R0 — establish a trustworthy current-state baseline

**Purpose:** replace historical ambiguity with a small set of living documents without deleting the
audit trail.

### Deliverables

1. Make `docs/architecture.md` the concise current architecture entry point, organized around
   Transport / Engine / Provider.
2. Add or refresh `docs/architecture_invariants.md` with enforceable invariants:
   - semantic transports do not expose readiness/completion differences;
   - readiness registers interest; completion submits owned operations;
   - logical close is distinct from physical retirement;
   - callback reentrancy and synchronous completion are supported;
   - completion-batch targets outlive every event that can reference them;
   - platform backends are transport-mechanical, not protocol-aware;
   - integration-owned types do not enter `include/keel/*.h`;
   - hot transport paths allocate no memory;
   - uncertain retirement quarantines storage rather than guessing.
3. Convert append-only audit documents into clearly dated historical evidence, or add prominent
   “historical; verify against current code” banners and an index under `docs/audits/`.
4. Find and correct stale Makefile/docs claims, including statements contradicted by current IOCP,
   io_uring, datagram, listener, or EFI/lwIP gates.
5. Add links from README and the existing roadmap without duplicating the full contracts.

### Acceptance gates

- Every architecture claim is linked to current code, a contract, or an executable gate.
- Historical findings are not presented as current defects after they have been fixed.
- No public API or behavior changes.
- Documentation link/reference check passes; `git diff --check` passes.

---

## R1 — canonize the Tier-1 transport boundary

**Purpose:** make architectural dependency direction explicit without changing runtime behavior.

### Deliverables

1. Declare `KlListener`, `KlStream`, and `KlDatagram` canonical Tier-1 transports in the architecture
   and contribution guides.
2. Document the permitted dependency direction:

   ```text
   new protocol -> semantic transport -> driver/adapter -> engine + provider
   ```

   A protocol may use a lower seam only when the missing semantic is documented and reviewed.
3. Add a lightweight architecture check that prevents new protocol TUs from directly including
   private backend headers or host socket-address types.
4. Inventory existing exceptions.  Classify each as intentional, transitional, or a defect; do not
   mechanically rewrite them in this increment.

### Acceptance gates

- Mechanical check is narrow enough to avoid suppressing legitimate integration code.
- Existing intentional exceptions are allowlisted with reasons and owners.
- Default, no-completion, freestanding-header, and provider-neutrality gates remain green.

---

## R2 — position `KlUdp` without breaking it

**Purpose:** end conceptual competition between the two public datagram APIs.

### Frozen direction

```text
KlDatagram = canonical bounded Tier-1 message transport
KlUdp      = compatibility and extended UDP facility
```

`KlUdp` retains its behavior and advanced UDP features, including batching, GSO/GRO, multicast,
packet-info controls, and legacy queue semantics.  It is not deprecated merely because
`KlDatagram` is canonical.

### Deliverables

1. Update API and architecture documentation with a decision table: when to use `KlDatagram`, when to
   use `KlUdp`, and which semantics intentionally differ.
2. Add a contributor rule that new portable message protocols use `KlDatagram` unless they require a
   documented `KlUdp` extension.
3. Inventory DNS and other datagram consumers.  Propose migrations separately; do not combine them
   with this documentation increment.
4. If a protocol migration is justified, freeze and implement it one protocol at a time with behavior
   parity, failure-path tests, and every applicable backend gate.

### Acceptance gates

- No source or ABI break for `KlUdp` users.
- No hidden change from byte-budget to slot-budget semantics.
- Documentation does not promise that every `KlUdp` extension exists in `KlDatagram`.

---

## R3 — completion-target lifetime audit and design freeze

**Purpose:** formalize the largest remaining correctness risk before generalizing any token.

### R3a: inventory only

Build a table for every object directly or indirectly referenced by a completion event:

| Target class | Event owner representation | Ref/lease mechanism | Same-batch destruction risk | Cancel path | Retirement proof | Quarantine support |
|---|---|---|---|---|---|---|
| datagram operation | `KlDgramLife` | stable token ref | controlled | backend cancel | retire classifier/terminal | yes |
| stream read/write | inspect | inspect | inspect | inspect | inspect | inspect |
| listener accept | inspect | slot lease/op state | inspect | inspect | inspect | inspect |
| connect attempt | inspect | inspect | inspect | inspect | inspect | inspect |
| watcher | inspect | inspect | inspect | inspect | inspect | inspect |

Explicitly model:

- two related events in one drained batch;
- the first callback closing/freeing the second event's target;
- synchronous completion during submit/arm;
- cancellation that completes inline;
- late completion after logical close;
- fd/handle reuse;
- backend teardown with operations still registered;
- confirmed retirement versus quarantine.

### R3b: choose the smallest remedy

After the inventory, choose independently for each unsafe class:

1. reorder or defer destruction until the batch ends;
2. transfer an existing lease/ref into the event;
3. use a target-specific stable token;
4. introduce a shared internal `KlOpLife` only if at least two target classes need identical semantics.

A generalized token is not automatically the desired outcome.  If proposed, freeze:

- owner/operation reference counts;
- target invalidation and generation rules;
- finalizer context and thread;
- transfer versus borrow event disposition;
- no-handler routing behavior;
- quarantine ownership;
- overflow policy for refcounts/generations;
- allocation and reuse rules;
- ABI visibility (prefer private/internal).

### Acceptance gates

- Design note reviewed before code.
- Deterministic regression for every proven unsafe sequence.
- Same-batch, reentrant, duplicate, stale, cancel, no-handler, teardown, and allocation-failure tests.
- ASan/UBSan/LSan on pollcomp plus native IOCP/io_uring validation where touched.
- EFI/lwIP tests where callback-native or quarantine semantics are touched.

---

## R4 — remove protocol state from platform completion backends

**Purpose:** make platform TUs mechanically transport-oriented while allowing driver code to know
connection/protocol state.

### Deliverables

1. Audit `event_iocp.c`, `event_iouring.c`, `event_pollcomp.c`, and integration completion engines for
   references to HTTP, PROXY, TLS, WebSocket, HTTP/2, `KlConn` state enums, or protocol buffers.
2. Classify every reference:
   - platform-mechanical and acceptable;
   - transport-driver responsibility;
   - genuine protocol leakage.
3. For genuine leakage, freeze the smallest neutral descriptor/flag/callback passed by the driver.
   The backend should receive operation kind, opaque target/lifetime, buffers, lengths, and mechanical
   flags—not protocol state enums.
4. Add a mechanical include/symbol check preventing new protocol-state dependencies in platform
   backend TUs.

### Acceptance gates

- No giant replacement vtable.
- No duplicated protocol decisions across backends.
- TLS remains above `KlStream`.
- Existing readiness and completion behavior remains byte-for-byte compatible at the public surface.
- Native IOCP and io_uring tests plus pollcomp sanitizer coverage pass.

---

## R5 — one semantic transport conformance harness

**Purpose:** increase confidence by running the same contracts over genuinely different engines and
providers rather than adding more abstraction.

### Design

Factor contract suites into backend-neutral scenarios with thin environment fixtures:

```text
contract scenario
    +-- hosted socket fixture
    +-- Windows fixture
    +-- lwIP raw fixture
    +-- EFI/QEMU fixture
```

The scenario logic and assertions must be shared.  Fixture code may prepare sockets, pump an engine,
or translate test markers, but must not weaken the contract.

### Required scenario families

For `KlStream`:

- ordered bounded writes and backpressure;
- pause/resume with one held completion result;
- synchronous/reentrant receive completion;
- graceful drain and abortive close;
- callback-triggered close;
- physical retirement before reuse/free.

For `KlDatagram`:

- atomic admission and fixed-slot backpressure;
- message boundaries, zero-length messages, source/local metadata, truncation;
- FIFO single-flight send;
- strict pause with exactly one held packet;
- graceful and abortive close;
- confirmed retirement, quarantine, and no-double-release.

For `KlListener`:

- credit reservation and lease transfer;
- readiness window versus multi-accept completion window;
- cancel/close with outstanding accepts;
- callback-triggered teardown;
- physical retirement and exact slot release.

### Backend matrix

Run where supported:

| Engine/provider | Stream | Datagram | Listener | Required proof |
|---|---:|---:|---:|---|
| epoll/POSIX | yes | yes | yes | Linux sanitizer CI |
| kqueue/POSIX | yes | yes | yes | macOS CI |
| poll/POSIX | yes | yes | yes | fallback CI |
| WSAPoll/Winsock | yes | yes | yes | Windows runtime CI |
| pollcomp/POSIX | yes | yes | yes | ASan/UBSan/LSan |
| io_uring/POSIX | yes | yes | yes | native Linux kernel CI |
| IOCP/Winsock | yes | yes | yes | native Windows CI |
| lwIP raw | supported subset | yes | supported subset | raw integration sanitizer gate |
| EFI | supported subset | yes | supported subset | host mock + QEMU/OVMF |

Unsupported cells must be explicit capability limits, not silently skipped tests.

### Acceptance gates

- Shared scenario assertions are not forked per backend.
- Every skip reports the missing capability.
- Stress repetitions and randomized callback/close timing use reproducible seeds.
- CI time remains bounded; extended stress can run on a scheduled/manual job.

---

## R6 — production-confidence campaign

**Purpose:** separate architectural support from demonstrated operational maturity.

### Deliverables

1. Publish a backend confidence matrix with evidence-based labels, not marketing labels.
2. Add scheduled or manually dispatched stress jobs for:
   - io_uring cancellation, close, fd reuse, queue pressure, and registered-buffer fallback;
   - IOCP send-only, receive-only, concurrent close, AcceptEx windows, and teardown drain;
   - real TLS over IOCP, including the currently underrepresented real-mbedTLS path;
   - pollcomp randomized completion ordering as a portable semantic oracle.
3. Add bounded watchdogs that fail with operation-registry diagnostics; never “fix” a hang merely by
   replacing an infinite wait with silent abandonment.
4. Archive the seed, backend capabilities, OS/kernel version, and outstanding operation inventory on
   stress failure.

### Acceptance gates

- A timeout produces actionable ownership/operation diagnostics.
- Diagnostic instrumentation is gated and does not remain enabled in production builds.
- “Production” confidence requires native runtime evidence, sanitizer coverage where feasible, and a
  documented teardown/retirement story.

---

## R7 — `KlEventLoop.fd` modernization (deferred ABI work)

**Purpose:** remove the last visibly readiness-shaped field only when compatibility economics justify
it.

### Required first step: usage and ABI audit

Determine:

- whether external consumers can legally access `KlEventLoop.fd`;
- every internal read/write and backend-specific meaning;
- installed layout and source/ABI compatibility consequences;
- whether the field is merely dead compatibility state;
- whether an opaque loop layout is possible without harming stack allocation or freestanding builds.

### Preferred end-state

Conceptually:

```c
typedef struct KlEventLoop {
    void             *backend;
    const KlEventOps *ops;
    KlAllocator      *alloc;
} KlEventLoop;
```

This sketch is not authorization to change the ABI.  If compatibility prevents removal, retain the
field, mark it legacy/reserved, and forbid new consumers.  Removal belongs in an explicitly versioned
ABI transition.

### Acceptance gates

- Design freeze includes source and binary compatibility analysis.
- No pointer truncation or assumption that native handles fit in `int`.
- All event backends and freestanding/header gates pass.
- This increment may be permanently deferred with no architectural penalty.

---

## R8 — add breadth only after hardening

**Purpose:** validate the architecture through real consumers rather than another abstraction pass.

Candidate work, each separately designed and scoped:

- move a suitable portable DNS path onto `KlDatagram` while retaining advanced `KlUdp` paths where
  needed;
- add mDNS or CoAP as a bounded-message consumer;
- use `KlDatagram` as groundwork for QUIC experiments without claiming QUIC is “just UDP”;
- add another provider only when it exercises a genuinely new ownership/execution model.

New consumers must not cause protocol-specific fields to appear in core engine/provider interfaces.
If a new protocol does not fit, first determine whether it needs a real missing transport semantic or
merely an adapter—not a new universal abstraction.

---

## Dependency order and suggested milestones

```text
R0 current docs
 |
 +--> R1 canonical boundary --> R2 KlUdp positioning
 |
 +--> R3 lifetime audit/freeze --> targeted lifetime fixes
 |
 +--> R4 backend decontamination
 |
 +--> R5 conformance harness --> R6 production confidence
 |
 +--> R7 event-loop ABI work (optional/versioned)
 |
 `--> R8 new consumers/providers (after relevant hardening)
```

Recommended review milestones:

1. **Architecture baseline:** R0–R2, primarily documentation and enforcement.
2. **Lifetime hardening:** R3 inventory, freeze, then narrowly justified implementations.
3. **Axis enforcement:** R4 plus mechanical guards.
4. **Evidence expansion:** R5–R6.
5. **Optional ABI modernization:** R7 only with an explicit compatibility window.
6. **Breadth:** R8 as independent protocol/provider projects.

R3 and R4 may be researched in parallel after R0, but their code changes must remain separate.  R5
should reuse the stabilized contracts from R1–R4 rather than freeze incidental backend internals.

---

## Definition of done

This roadmap is successful when:

- contributors can explain Keel using Transport / Engine / Provider without reading historical audits;
- new protocols normally depend on `KlStream`, `KlDatagram`, or `KlListener`;
- `KlUdp` has a clear, stable role and no accidental semantic migration;
- every completion target has a documented lifetime and retirement proof;
- platform backend TUs contain no protocol-state decisions;
- the same semantic contract scenarios run across all applicable backends;
- native IOCP/io_uring failures leave actionable operation-lifetime diagnostics;
- backend confidence claims match native test evidence;
- integration-owned types remain outside the installed core API;
- no new universal async abstraction was required.

The desired end state is not fewer layers at any cost.  It is a system where each layer has one clear
job, lifetime ownership is explicit, and backend diversity strengthens rather than distorts the
transport contracts.
