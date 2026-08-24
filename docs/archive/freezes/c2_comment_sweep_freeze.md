# Phase C2 — Source/Header Comment Sweep — Design Freeze

Status: frozen design, docs-only inventory. Classifies the *active source and header comments* that
carry stale milestone / migration / deleted-path narration and defines the rules for removing or
rewriting them. It performs **no code edits** — those land in the reviewed C2-1…C2-4 increments after
this freeze is accepted. **No behavioral code changes anywhere in C2**: every edit is comment-only
(a compile per increment proves it). Governing roadmap: [roadmap/roadmap.md](../../roadmap/roadmap.md),
Phase C / C2.

## 0. Goal

Comments should explain the code as it exists **now**. C2 removes the *archaeology* — the
milestone/phase/step tags, migration narration, and deleted path/name references that accumulated
during the restructure — while **preserving** every comment that explains a current invariant,
ownership rule, safety property, or non-obvious behavior. When a stale tag is *attached to* a live
explanation, the tag is stripped and the explanation kept (a rewrite, not a deletion).

## 1. Scope

**In scope** — **every** active, first-party `.c`/`.h` file (the roadmap requires the whole tree):
- `include/keel/*.h` — the public headers (incl. the STABLE banners).
- `src/*.{c,h}` — the substrate.
- `src/protocols/*/*.{c,h}` — the protocol implementations.
- `integrations/**/*.{c,h}` — the bring-your-own adapters (production).
- `tests/**/*.{c,h}` and `integrations/**/tests/**/*.{c,h}` — the unit / smoke / harness sources.

Tests carry the same archaeology (`7B-3`, `M6.0a`, `PAL Phase 8`, `Finding N`) and are swept under the
same rules, with one refinement: a test comment's description of the **scenario or finding a suite
regression-guards** is current, non-obvious behavior and is PRESERVED — only the milestone *label* is
stripped (e.g. "regression test for the 7B-8 close hang" → "…for the empty-armed-recv close hang").
Tests are the final source increment (C2-5) so the production-source reviews (C2-1…C2-4) stay
manageable.

**Out of scope:**
- `vendor/**` — third-party, compiled `-w`; never edited.
- `.github/workflows/**` — no CI-config changes during C2–C5 (the C2-6 gate's CI enrollment is
  deferred to the combined C1–C5 validation, per §6a).
- `docs/**` — handled in C1.

The **Makefile** is touched only in **C2-6**, to add the permanent milestone-marker gate target
(§6a) — no other Makefile comment or logic is altered this phase.

## 2. Classification (three dispositions)

Each targeted comment fragment is classified as one of:

- **REWRITE** (the common case) — a stale tag/reference is embedded in an otherwise-current comment.
  Strip the tag / fix the path, keep the explanation. *No sentence of live meaning is lost.*
- **REMOVE** — the entire comment is pure archaeology (a milestone marker or migration note with no
  current-behavior content). Delete it.
- **PRESERVE** (default) — the comment explains a current invariant, ownership, safety property, or
  non-obvious behavior. Untouched, even if it contains a word that pattern-matches a target below.

## 3. What to REMOVE / REWRITE (with rules and examples)

### 3a. Milestone / phase / step tags
Tokens: `7A-N` / `7B-N`, `Phase A–F`, `Step N`, `M1…M9`, `6.4a–c`, `8f-5` / `8e2` / other `8x-N`,
`RC-N`, `P9-N`, `S3.5`, `F1`, `Finding N`. These name a point in the build history, not the code.

- **Rule:** strip the tag; keep the sentence. If the sentence is *only* the tag, remove it.
- Example (REWRITE) — `include/keel/datagram.h:5`
  `datagram.h — the public fixed-slot KlDatagram API (Phase B, step 7B-3).`
  → `datagram.h — the public fixed-slot KlDatagram API.`
- Example (REWRITE) — `include/keel/datagram.h:16–19` the STABLE banner lists each backend with a
  `(7B-N)` tag; keep "STABLE across every supported backend" + the backend list, drop the `(7B-N)`
  and "as of 7B-10".
- Example (REWRITE) — `include/keel/datagram.h:59` `… multicast join/leave …) — M2` → drop `— M2`.
- Example (REWRITE) — `src/protocols/http/completion_http_server.c:802`
  `/* ACCEPT recovers the server from the event ctx (6B-3: KlAcceptTarget removed), … */`
  → `/* ACCEPT recovers the server from the event ctx, … */` (the current-behavior clause stays).

**M-milestone as a feature label:** where `M5` etc. names a shipped capability the reader still needs
(the batch extension), rephrase to the feature, not the milestone — e.g. "the M5 batch extension" →
"the optional datagram batch extension (`datagram_batch.h`)". Do not leave a bare `— M5` tag.

### 3b. Migration / history narration
Phrases that describe *what changed*, not *what is*: `formerly`, `renamed (to/from)`, `used to be`,
`was moved / relocated`, `superseded`, `consolidated into … and removed`, `the old X`, `X removed`,
`no longer <exists>` (as opposed to current behavior — see §4).

- **Rule:** rewrite to state only the current behavior; delete a note whose sole content is the change.
- Example (REWRITE) — `…: KlAcceptTarget removed` → describe what ACCEPT does now (see 3a example).

### 3c. Deleted paths / names in comments
Cross-references to files/types the restructure deleted or moved: `h2.c`, `udp.c`, `udp.h`,
`udp_server.h`/`udp_server.c`, `io_engine.h`, `connection.c`, `KlAcceptTarget`, and any bare
`http_connection.c`-style path now under `src/protocols/…`.

- **Rule:** repoint to the current path/name, or drop the reference if it no longer aids the reader.
- Example (REWRITE) — `include/keel/http_router.h:237` `… counterpart to … http_connection.c / h2.c`
  → `… src/protocols/http/http_connection.c` and the HTTP/2 adapter under `src/protocols/http2/`.
- Example (REWRITE) — `include/keel/net.h:10` header list `(… udp.h, udp_server.h)` → `datagram.h`,
  `socket_dgram.h` (the current public headers that expose socket addresses).
- Example (REWRITE) — `src/protocols/http2/completion_http2.c:21` `… not h2.c — h2.c only exposes …`
  → name the current HTTP/2 adapter TU / seam, not `h2.c`.

Note: the deleted **type** names already banned by the gates (`KlUdp*`, `parsers/`, `spikes/`) are
absent from the tree today (`check-no-kludp` / `check-old-layout` keep them out, comments included);
this category is the *un-gated* residue (`h2.c`, `udp.h`, `io_engine.h`, `KlAcceptTarget`, …).

### 3d. Obsolete commit / phase references
Bare commit SHAs or "see commit …" notes. **Inventory result: none present** in the in-scope source
(scanned). Recorded here so the rule exists if one appears: remove the SHA, keep any surviving
explanation.

## 4. What to PRESERVE (the boundary)

These pattern-match a target above but describe the **current** code — do **not** touch:

- **Lifecycle vocabulary** — `retired`, `detached`, `quarantined`, `terminal`, `reserved` are the
  live state names (e.g. `async.h` "already-retired op is a no-op"; `connect_op.h` "all ops retired").
- **Current-behavior negations** — `no longer` / `no more` describing what the running code does now
  (e.g. `drain.c:274` "buffer gone — no longer in reservation mode"; `event_iocp.c:317` "Receives no
  longer own a buffer"). These are invariants, not history.
- **Invariants / ownership / safety / non-obvious behavior** — the substance the sweep exists to keep:
  UAF/lifetime rules, ownership-transfer notes, sync-completion cautions, overflow guards, the reasons
  a thing is done a particular way.

When in doubt, PRESERVE. The sweep must never trade a live explanation for tidiness.

## 5. Inventory (surface, by subtree)

Scanned counts of the milestone and migration patterns (occurrences; the migration figure is noisy —
it includes §4 false positives resolved per-comment during the sweep):

| Subtree | Files (in scope) | Milestone hits | Migration-verb hits |
|---|---|---|---|
| `include/keel/*.h` | 55 | 54 | 21 |
| `src/*.{c,h}` | 91 | 303 | 112 |
| `src/protocols/*/*.{c,h}` | 48 | 60 | 21 |
| `integrations/**/*.{c,h}` (production) | ~105 | 246 | 94 |
| `tests/**` + `integrations/**/tests/**` | ~110 | ~318 | ~89 |

This freeze classifies by **category and rule** (with the representative examples above), not
comment-by-comment: ~980 in-scope occurrences across ~170 files are applied under §§3–4 in the
reviewable sweep increments below, where the diffs are inspected.

## 6. Sequencing (each a separate commit; pause after each)

1. **C2-0 (this freeze).** Docs-only. Commit, pause.
2. **C2-1 — public headers** (`include/keel/*.h`). Highest-visibility surface (the STABLE banners).
3. **C2-2 — substrate** (`src/*.{c,h}`). The largest subtree.
4. **C2-3 — protocols** (`src/protocols/*/*.{c,h}`).
5. **C2-4 — integrations** (`integrations/**/*.{c,h}`, production).
6. **C2-5 — tests + harnesses** (`tests/**/*.{c,h}`, `integrations/**/tests/**/*.{c,h}`). The final
   source increment; test scenario/finding descriptions preserved, milestone labels stripped (§1).
7. **C2-6 — permanent gate + freeze archival** (§6a). Add the milestone-marker gate, run it green over
   the whole first-party tree, and archive the C1 + C2 freezes to `docs/archive/freezes/`.

## 6a. The permanent milestone-marker gate (C2-6)

The roadmap requires a standing gate so the archaeology cannot return. C2-6 adds one Makefile target
(e.g. `check-no-milestones`, wired into the `.PHONY` + gate list like the other `check-*` gates):

- **Scans** every first-party `.c`/`.h` — derived from `git ls-files '*.c' '*.h'` with **only**
  `vendor/**` excluded (no directory-, file-, or line-level exceptions).
- **Rejects** the known marker families as whole-token patterns: `7A-N`/`7B-N`, `8x-N`, `Phase A–F`,
  `Phase 0–9`, `PAL Phase N`, `Step N`, `M1–M9` (including dotted forms like `M6.0a`), `6.4a–c`,
  `RC-N`, `P9-N`, `S3.5`, `F1`, `Finding N`. On a hit it prints `file:line` and fails.
- **Self-canaried** — a positive sample (a synthetic `7B-3`) MUST be caught and a negative sample (a
  clean line) MUST NOT, per the existing gate idiom; BSD+GNU-portable, binary-skip (`-I`).
- **External-standard allowlist** — a small set of **exact tokens** only (a genuine standards
  reference that lexically collides), never a directory or "lines-containing-X" exception. It starts
  empty; a token is added by exact string with a one-line rationale if a real collision appears. RFC
  numbers, `§N` section refs, and lowercase "phase" do not match the patterns and need no entry.
- **CI enrollment is deferred** to the combined C1–C5 validation (adding the workflow step then), to
  avoid `.github/workflows` churn during this text-only sequence. The target lands in C2-6 and is run
  locally at every remaining increment.

## 7. Validation (proportionate; per increment)

- `git diff --check` (no whitespace damage).
- A **release compile** (`make`) — comment-only edits must not change behavior; a clean build proves
  no code was accidentally altered. `make test` additionally on C2-2 (substrate) and C2-5 (tests) as a
  spot check.
- The stale-name / layout / doc gates stay green: `check-no-kludp`, `check-no-httplegacy`,
  `check-old-layout`, `check-doc-refs`, `check-test-layout` — confirming the sweep introduces no
  gated token and repoints deleted-path references correctly.
- From **C2-6**, the new `check-no-milestones` gate must pass over the whole first-party tree, and is
  re-run at the tail of any later increment.
- No remote CI between C2–C5.

## 8. Recorded note (C1 tail)

`docs/c1_documentation_taxonomy_freeze.md` and this file are the two remaining freezes at the `docs/`
root; per the C1 freeze §2d they archive to `docs/archive/freezes/` once their phase completes. That
doc move is folded into **C2-6** (alongside the permanent gate), not into a comment-sweep increment.

No source comment is edited until this freeze is accepted.
