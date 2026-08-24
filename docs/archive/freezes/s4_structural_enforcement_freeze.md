# S4, Structural Enforcement and Reconciliation, Design Freeze

Status: frozen design. Docs-only. This document authorizes the S4-1 and S4-2 implementation
increments; it changes no build, code, or CI behavior by itself. Governing roadmap:
[keel_improvement_roadmap.md](../../roadmap/roadmap.md), Phase S / S4.

Each S4 implementation increment lands with its build, CI, cleanup, documentation, and enforcement
updates in the same commit (roadmap principle #9) and is validated and paused for review.

## 0. Scope and relationship to R4

R4 already delivered seven of the eight gates the roadmap lists for S4, under the exact roadmap
names, all enrolled in the CI Static-Analysis job:

| Roadmap S4 gate | Status |
|---|---|
| `check-substrate-purity` | DONE (R4 G1) |
| `check-protocol-no-integration` | DONE (R4 G2) |
| `check-integration-seam` | DONE (R4 G3) |
| `check-protocol-home` | DONE (R4 G4) |
| `check-test-layout` | DONE (pre-existing; committed `tests/test-layout.manifest`) |
| `check-no-kludp` | DONE (pre-existing) |
| `check-no-httplegacy` | DONE (pre-existing) |
| `check-old-layout` | MISSING: R4's `check-no-interim-paths` is a narrow subset |

The current tree already satisfies every resurrection rule below (no top-level `protocols/`, no old
flat integration homes, no `spikes/`, no `parsers/`, every integration test under a backend `tests/`,
no protocol test at the `tests/` root). S4 is therefore **preventive enforcement plus consistency
reconciliation**, not a move. Its delta is exactly two things:

1. **S4-1**: build the consolidated `check-old-layout` gate (absorbing and retiring
   `check-no-interim-paths`).
2. **S4-2**, the reconciliation sweep (manifests, CI labels, clean rules, living path references,
   minimal contributor-path accuracy). Kept a separate increment unless it proves literally empty.

## 1. `check-old-layout`: the single authoritative resurrection gate (S4-1)

One gate, one authority. It **absorbs and retires** `check-no-interim-paths`; its CI step, `.PHONY`
entry, and the architecture-invariant wording (I11 G5) are updated to the new name in the same commit.

Implemented as a helper `tools/check_old_layout.sh` in the established idiom of
`tools/check_boundaries.sh`: default-deny, positive-and-negative self-canary per case, `file:line`
diagnostics, binary-skip, BSD+GNU-portable, inventories derived from the tree (`git ls-files`) rather
than fixed name lists, except where an exact committed manifest or an explicit reviewed allowlist is
the specified mechanism (cases 2 and 5).

It rejects resurrection of the six deleted layouts. Each case is a **structural** check (over tracked
paths) and/or a **reference** check (over code plus living docs). The reference scan set is the same
curated code-plus-living-docs set used by `check-no-interim-paths` today; historical design/freeze/
audit docs and the Makefile that defines the patterns are excluded (they legitimately name old paths),
exactly as the existing stale-name gates handle self-reference.

### Case 1: top-level `protocols/`
- Structural: `git ls-files 'protocols/*'` must be empty.
- Reference: no `protocols/<fam>/` path (fam in http, http2, websocket, dns, proxy_protocol) in the
  scan set unless prefixed `src/` or `tests/`. (This is exactly today's `check-no-interim-paths`
  interim-path branch, absorbed here, retaining its per-token file:line diagnostics.)

### Case 2: old flat integration homes
- Structural (explicit reviewed allowlist, ruling #3): every direct child directory of `integrations/`
  must be an allowlisted **role**, `codec`, `http2`, `platform`, `tls`, and the only permitted
  top-level files are `Makefile` and `README.md`. Any other entry directly under `integrations/`
  (`integrations/lwip/`, `integrations/mbedtls/`, `integrations/nghttp2/`, ...) fails. Adding a new
  architectural role is a deliberate, reviewed edit to this allowlist with a rationale, not an
  accident the gate silently permits.
- Reference: no `integrations/<backend>/` old-flat path (a known former backend name not preceded by
  a role segment) in the scan set.

### Case 3: `spikes/`
- Structural: `git ls-files 'spikes/*'` must be empty.
- Reference: no `spikes/` path in the scan set.

### Case 4: deleted directory paths
- `parsers/`: structural (`git ls-files 'parsers/*'` empty) plus reference (`parsers/http1...`). This
  is the second branch absorbed from `check-no-interim-paths`.
- Ownership split (no overlap, no gap): `check-old-layout` owns deleted **directory** paths;
  `check-no-httplegacy` continues to own deleted **module basenames** (`connection.c`, `server.h`, ...).

### Case 5: integration tests outside a backend `tests/` (EXACT OWNERSHIP, ruling / correction)
A filename heuristic (`test_*`/`*_test.c`/...) is **not** genuinely default-deny: a novel integration
test named, say, `probe.c` could sit at a backend root undetected. S4 uses an **exact committed
ownership manifest** instead, mirroring R4's `src/substrate-tus.manifest` for `check-protocol-home`.

- A committed manifest, working name `integrations/non-test-files.manifest`, enumerates every
  tracked file under `integrations/` that legitimately lives at the umbrella or a backend **root**
  (production implementation `.c`/`.h`, promoted ABI headers, `mbedtls_shim/*`, `Makefile`,
  `README.md`, `.gitignore`, and the two umbrella files `integrations/Makefile`,
  `integrations/README.md`).
- The gate partitions `git ls-files 'integrations/**'` into: files under a `.../tests/` segment
  (**test content**, unrestricted) and everything else (**backend-root / production**). Every
  production file must appear in the manifest; a backend-root file **not** in the manifest is
  unclassified and **fails** (it must be declared production there or moved under the backend's
  `tests/`). A manifest entry with no matching tracked file **fails** (stale). This makes ownership
  exact, a misplaced `probe.c` fails regardless of its name, with no name-guessing.
- The manifest is regenerated intentionally, documented in its own header, exactly as
  `src/substrate-tus.manifest` and `tests/test-layout.manifest` are.

### Case 6: protocol tests at the `tests/` root (DELEGATED, ruling #2)
`check-test-layout` already provides exact, default-deny ownership for every test in the top-level
`tests/` tree, both root and nested `tests/protocols/<fam>/`, via `tests/test-layout.manifest`. A
second basename-based rule here would be weaker and redundant. `check-old-layout` therefore does **not**
reimplement case 6: it **documents** that `check-test-layout` is the companion authority for protocol
test placement (in its header comment and its OK/summary line), and both gates run together in the CI
Static-Analysis job. (`check-test-layout` scopes to the `tests/**` tree; the integration `tests/`
placement of case 5 is a separate concern owned by the case-5 manifest above.)

## 2. Reconciliation sweep (S4-2, narrow; deferred scope explicit)

Per ruling #5, S4 is strictly **paths, manifests, build/clean recipes, CI, and minimal contributor-
path accuracy**. It does not touch narrative comments, guidance modernization, site work, archival or
deletion, or em-dash removal: all deferred to Phase C.

- **Manifests:** confirm `src/substrate-tus.manifest` and `tests/test-layout.manifest` are current and
  exhaustive; add the new `integrations/non-test-files.manifest` (S4-1 introduces it; S4-2 confirms
  no drift).
- **CI labels:** the folded `check-no-interim-paths` step is replaced by `check-old-layout`; confirm
  the boundary-gate step names read correctly against the final layout.
- **Clean rules:** confirm final-tree coverage (largely completed in the R3.4 / R3.5 / R4
  reconciliations); spot-fix any residual token.
- **Living path references:** sweep living docs and active build/config path references for any
  old-flat / interim / `spikes` / `parsers` path. The gates make this true by construction; S4-2 fixes
  any straggler a gate would flag.
- **Contributor instructions:** only the minimal path/layout **accuracy** fixes in `AGENTS.md` /
  `CONTRIBUTING.md` (correct directory references). The full guidance rewrite: current `KlHttp*`
  names, test-placement narrative, flat-header policy, no-em-dash rule; remains **C4**.

If, after S4-1, this sweep is literally empty (every item already consistent), S4-2 is recorded as a
no-op in the S4-1 commit message and skipped; otherwise it is its own increment (ruling: keep S4-1 and
S4-2 separate unless the sweep proves empty).

## 3. Validation matrix (per increment)

- `check-old-layout` teeth-tested against a real injected instance of **each** of cases 1-5 (a
  resurrected path / an unclassified backend-root file), asserting correct `file:line` and a non-zero
  exit, then reverted; case 6 covered by `check-test-layout`'s existing teeth.
- The full structural gate set green on a clean tree; enumerated so this freeze cannot drift through
  a count: `check-sockaddr-neutral`, `check-tier1-boundary`, `check-substrate-purity`,
  `check-protocol-no-integration`, `check-integration-seam`, `check-protocol-home`, `check-old-layout`,
  `check-test-layout`, `check-no-kludp`, `check-no-httplegacy`, `check-doc-refs` (and `wx-guard`, the
  separate W^X surface guard).
- Default build + full test suite + cppcheck + `check-doc-refs` green.
- The freestanding matrix and the CI matrix (via a draft PR, the S4-established pattern) validate the
  CI-only legs; S4 changes are test/CI/manifest/doc only, so the core `libkeel.a` build is untouched.

## 4. Increment sequence

- **S4-0 (this document).** Docs-only: this freeze plus committing the governing
  `docs/keel_improvement_roadmap.md` (ruling #4). The protected restructure prompt stays untracked.
  Commit, pause.
- **S4-1.** `tools/check_old_layout.sh` + `integrations/non-test-files.manifest` + Makefile target +
  CI enrollment + retire `check-no-interim-paths` (target, `.PHONY`, CI step) + reword invariant I11
  (G5 -> `check-old-layout`) + teeth-tests. Validate, pause.
- **S4-2.** Reconciliation sweep (§2), unless empty. Validate, pause.

## 5. Recorded decisions

1. `check-no-interim-paths` is retired into `check-old-layout`: one authoritative resurrection gate.
2. No case-6 basename check; `check-test-layout` is the companion authority for protocol test
   placement, invoked/documented by `check-old-layout`, not partially reimplemented.
3. The integration-role allowlist (`codec`, `http2`, `platform`, `tls`) is explicit; a new role
   requires a reviewed gate change with rationale.
4. `docs/keel_improvement_roadmap.md` is committed in S4-0 as the governing tracked roadmap; the
   restructure prompt stays untracked.
5. S4 is strictly paths / manifests / build-clean recipes / CI / minimal contributor-path accuracy.
6. Case-5 correction: exact committed ownership manifest, not a filename heuristic.

## 6. Non-goals (deferred to Phase C)

Narrative/history comment sweeps (C2), documentation taxonomy and archival/deletion (C1), contributor-
guidance modernization (C4), site rebuild (C5), and Unicode em-dash removal (C3) are explicitly out of
S4. S4 establishes the structural enforcement those later phases rely on.
