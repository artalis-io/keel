# C4 - Active-guidance rewrite (inventory + freeze)

Status: PROPOSED (docs-only). No guidance file is rewritten until this freeze is accepted.
Governing roadmap: [roadmap/roadmap.md](../../roadmap/roadmap.md), Phase C / C4. This file is written
em-dash-free and milestone-token-free so it passes the C2 and C3 gates.

## 1. Goal

Bring the active guidance documents into agreement with the FINAL architecture and the CURRENT
commands. C1 fixed the documentation taxonomy (moves, links), C2 removed milestone archaeology from
comments, C3 removed em-dashes. C4 is about CONTENT: the human- and agent-facing guidance still
carries drift (volatile counts, a deleted-path reference, and prose that predates the datagram
consolidation and the integrations restructure). This is a docs-only accuracy pass, reviewed per
increment, with no code change.

## 2. Scope

Primary (the reviewer's named set):
- `AGENTS.md` (295 lines)
- `CLAUDE.md` (498 lines) - the project-instructions file loaded every session
- `CONTRIBUTING.md` (49 lines)

Related active guidance (assessed and reconciled in the same phase):
- `README.md` (957 lines) - the public entry doc: build commands, architecture, module list
- `SECURITY.md` - references the deleted `parsers/` tree
- `.claude/skills/axis-audit/SKILL.md`, `.claude/skills/c-audit/SKILL.md` - the audit-skill guidance

Out of scope: `docs/architecture/**`, `docs/contracts/**`, `docs/operations/**` (the canonical
living docs, reconciled in C1; they are the ground truth C4 checks against), and `docs/archive/**`
(historical record).

## 3. Drift found (concrete, from an initial scan)

- **Volatile hard-coded counts.** `CLAUDE.md` states `make test` builds "84 suites" and `make
  examples` builds "23 example programs (25 with TLS, 27 with compression)". Ground truth today:
  105 test sources, 31 example programs, 89 test binaries. These numbers drift with every increment.
  (The C1 freeze already removed volatile counts from `capability_matrix.md`; C4 applies the same
  policy to the guidance docs.)
- **Deleted-path reference.** `SECURITY.md` says the W^X guard scans "`src/` or `parsers/`", but
  `parsers/` was removed and `tests/no_codegen_surface.sh` now scans `src/` recursively (covering
  `src/protocols/`). `README.md` carries the same stale `parsers/` mention.
- **Architecture-narrative currency.** The module list and Key Types must be verified against the
  final code: the datagram consolidation (KlUdp removed, `KlDatagram` is the Tier-1 primitive), the
  three-axis model, and the `integrations/<role>/<backend>/` layout. The datagram/dns entries look
  current; the whole list and the Key Types table need a line-by-line check against `include/keel/`
  and `src/`.
- **Command currency.** The Build/test/command blocks must match the actual Makefile targets (for
  example the retired `BACKEND=lwipraw`, and the two new gates `check-no-milestones` /
  `check-no-em-dash` that guidance should mention where it lists the gate set).

## 4. Ground-truth sources (what each claim is reconciled against)

- Commands / targets: the root `Makefile` (and integration Makefiles).
- Counts / file inventories: `git ls-files` at rewrite time (or, preferably, no hard count at all).
- Public types: `include/keel/*.h`.
- Modules / layout: `src/`, `src/protocols/`, `integrations/`.
- Canonical narrative: `docs/architecture/overview.md`, `docs/architecture/invariants.md`,
  `docs/contracts/*.md`, `docs/operations/*.md`.

## 5. Reconciliation policy (proposed; decisions flagged for review)

- **D1 - volatile counts.** Recommended: REMOVE hard-coded suite/example/backend counts from the
  guidance prose (say "the unit-test suite", "the example programs"), matching the C1 capability-matrix
  decision, so the docs cannot drift. Alternative: keep a count but derive/annotate it as approximate.
- **D2 - depth.** Recommended: an ACCURACY pass, not a restructure - fix wrong facts, stale paths,
  and outdated command/type/module descriptions; preserve each doc's existing structure and voice.
  A larger reorganization, if wanted, is a separate later effort.
- **D3 - the CLAUDE.md module list + Key Types table.** Recommended: verify every entry against
  `include/keel/` and `src/` and correct any stale name, path, or description; do not renumber or
  drop modules unless the module itself is gone.

## 6. Increment sequence (proposed; per-increment review, no push)

1. **C4-1** - `CLAUDE.md`: commands vs the Makefile; the 34-module list + Key Types table vs
   `include/keel/` + `src/`; remove volatile counts (D1); patterns/conventions still accurate.
2. **C4-2** - `AGENTS.md`: reconcile against the same ground truth (commands, layout, workflow).
3. **C4-3** - `CONTRIBUTING.md`, `SECURITY.md` (fix `parsers/` -> `src/` recursive), and the two
   `.claude/skills/*/SKILL.md` guidance files.
4. **C4-4** - `README.md`: the public build/architecture/module narrative, stale `parsers/`, counts.

The reviewer may regroup (for example fold README into an earlier step, or split CLAUDE.md).

## 7. Validation (per increment)

- Docs-only: no build or test behavior changes, but the guards must stay green - `check-doc-refs`,
  `check-no-milestones`, and the new `check-no-em-dash` (guidance edits must not reintroduce an
  em-dash or a milestone token, and must not break a link).
- A release build + `make test` as a sanity check on the increments that touch command text, to
  confirm every documented command still runs.
- Repo-wide markdown-link validation after each increment.
- Every claim changed is cross-checked against its §4 ground-truth source; no new hard-coded count is
  introduced.
- No remote CI between C4 increments; the single combined C1-C5 PR remains the validation point.

## 8. Decisions for review (summary)

- **D1** - volatile counts: remove (recommended) or keep-with-caveat.
- **D2** - depth: accuracy pass (recommended) or restructure.
- **D3** - module list / Key Types: line-by-line verify-and-correct (recommended).

No guidance file is edited until this freeze is accepted.
