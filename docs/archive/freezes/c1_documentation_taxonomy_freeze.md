# Phase C1: Documentation Taxonomy -- Design Freeze

Status: frozen design, docs-only inventory. Classifies every tracked document and defines the target
`docs/` taxonomy. It performs **no moves, deletions, or content rewrites**; those land in the reviewed
C1-1/C1-2/C1-3 increments after this freeze is accepted. No executable C behavior changes anywhere in
Phase C. Governing roadmap: [keel_improvement_roadmap.md](../../roadmap/roadmap.md), Phase C / C1.

## 0. Scope

- **In C1:** one documentation taxonomy; classify every doc as **living / archived / deleted /
  temporary**; move historical records under `docs/archive/`; delete fully-superseded material and
  captured temporary prompts; deduplicate the roadmap; repoint the Makefile gate **scan-path lists**
  and living-doc link targets that name a moved/deleted doc (path reconciliation only, not a gate-logic
  or C-behavior change); and **audit every promoted living document against current headers, source
  paths, gates, and tests** (see §3: a promoted living doc is never a pure rename).
- **Not in C1** (later phases): source/header comment-history sweep (C2), the em-dash gate (C3), the
  `AGENTS.md`/`CLAUDE.md`/`CONTRIBUTING.md` guidance rewrite (C4), the `site/` rebuild (C5), and the
  backend-confidence evidence model (E3). C1 does not touch `site/`, the root guidance prose, or source
  comments.

## 1. Target taxonomy (only what C1 actually delivers: no stub or placeholder pages)

```text
docs/
  architecture/
    overview.md          (from architecture.md; reconciled in C1-2)
    invariants.md        (from architecture_invariants.md; reconciled in C1-2)
    public_api.md        (from transport_surface.md; reconciled in C1-2)
  contracts/
    stream.md            (from stream_contract.md; reconciled in C1-2)
    datagram.md          (from datagram_contract.md; reconciled in C1-2)
    streaming.md         (from streaming_contract.md; reconciled in C1-2)
    async_lifecycle.md   (from async_lifecycle.md; reconciled in C1-2)
    compatibility.md     (from compatibility.md; reconciled in C1-2)
    alpn_policy.md       (from alpn_policy.md; reconciled in C1-2, D2)
  operations/
    capability_matrix.md (from capability_matrix.md; reconciled in C1-2)
    testing.md           (authored in C1-3, evidence-linked)
    fuzzing.md           (authored in C1-3, evidence-linked)
  roadmap/
    roadmap.md           (from keel_improvement_roadmap.md, the sole canonical roadmap)
  archive/
    freezes/   designs/   phases/   audits/
```

**Deliberately NOT created in C1 (no stubs, per D3):** the finer architecture split
`architecture/{transport_substrate,event_axis,protocol_ownership,integration_model}.md` and
`contracts/listener.md`: genuine authoring is deferred to a later, separately-scoped increment rather
than landed as placeholders; `operations/backend_confidence.md` is deferred to **Phase E3** (do not
publish an empty promise before the evidence model exists); `operations/comparison.md` is **not
created** (see D2: the source doc is deleted); a net-new `docs/README.md` index is deferred (it would
otherwise be a stub).

## 2. Classification of every tracked `docs/*.md` (75) + `docs/audits/README.md`

### 2a. LIVING (11): promoted to canonical docs; **move PLUS current-state reconciliation** (§3)
`architecture.md`, `architecture_invariants.md`, `transport_surface.md`, `datagram_contract.md`,
`stream_contract.md`, `streaming_contract.md`, `async_lifecycle.md`, `compatibility.md`,
`capability_matrix.md`, `alpn_policy.md`, `keel_improvement_roadmap.md`.

### 2b. TEMPORARY (delete): captured by an accepted freeze
- `claude_code_test_layout_prompt.md`: captured by the §9 test-layout freeze + `tests/test-layout.manifest`.
- `claude_code_transport_taxonomy_prompt.md`: captured by `http_taxonomy_freeze.md`.
- **`docs/claude_code_project_restructure_prompt.md`** (untracked, previously protected): **deleted in
  C1-1 per ruling D1**: its decisions are captured by the accepted restructure and S4 freezes; keeping
  it would preserve a parallel source of truth. This freeze records that explicit authorization.

### 2c. DELETED: fully superseded, no independent value beyond Git history
- `roadmap.md` (D4): the old roadmap; duplicates and is materially stale versus the canonical
  `keel_improvement_roadmap.md`.
- `comparison.md` (D2): volatile test counts, stale capability claims, obsolete `KlUdp`/bare-metal
  guidance, and third-party marketing comparisons. Not promoted to `operations/`; deleted.

### 2d. ARCHIVED (historical record) → `docs/archive/<category>/` (pure move, no content change)

- **freezes/**: `protocols_restructure_freeze.md`, `http_taxonomy_freeze.md`,
  `s4_structural_enforcement_freeze.md`; and `c1_documentation_taxonomy_freeze.md` once C1 completes.
- **audits/**: `keel_audit.md`, `keel_axis_audit.md`, `generic_datagram_audit.md`,
  `generic_transport_audit.md`, `pal_review.md`, and the existing `audits/README.md`.
- **phases/**: `phase4_public_provider_design.md`, `phase6_winsock_design.md`,
  `phase7_capability_negotiation_design.md`, `phase8_iocp_design.md`, `phase8b_iocp_breadth_design.md`,
  `phase8b4_udp_completion_design.md`, `phase8b5_tls_completion_design.md`,
  `phase8d_h2_completion_design.md`, `phase8e_completion_subset_design.md`,
  `phase8e2_async_completion_design.md`, `phase8f_iouring_completion_design.md`,
  `phase8f5_iouring_default_migration_design.md`, `phase8g_async_streaming_completion_design.md`,
  `phase9_lwip_raw_design.md`, `phase10_uefi_feasibility_design.md`, `phase10_uefi_server_design.md`,
  `phase10_efi_udp4_provider_design.md`, `phase10_lwip_raw_client_design.md`,
  `phase10_dns_uefi_retirement_plan.md`.
- **designs/**: `client_identity_design.md`, `completion_axis_runtime_design.md`,
  `core_completion_plan.md`, `datagram_completion_srcpin_design.md`, `datagram_consolidation_design.md`,
  `datagram_iocp_smoke_handoff.md`, `datagram_m1_queue_policy_design.md`,
  `datagram_m2_capability_design.md`, `datagram_m4_udpserver_design.md`,
  `datagram_m5_batch_extension_design.md`, `datagram_m6_kludp_decision_design.md`,
  `datagram_step7_public_api_design.md`, `datagram_step7b_breakdown.md`,
  `datagram_step7b9_efi_close_design.md`, `datagram_sync_teardown_design.md`, `datagram_vs_udp.md`,
  `dns_parity_design.md`, `dns_resolver_hardening.md`, `dns_tcp_cookies_design.md`,
  `event_provider_design.md`, `keel_datagram_ops_design.md`, `keel_sockaddr_design.md`,
  `lwip_platform_design.md`, `pal_transformation_design.md`, `r3a_completion_lifetime_inventory.md`,
  `r3b_lifetime_decision.md`, `r3b_w_watcher_aba_remedy.md`, `server_provider_adoption_design.md`,
  `udp_batching_design.md`, `udp_design.md`, `udp_multicast_design.md`, `udp_pktinfo_design.md`.

## 3. A promoted living document is a move PLUS current-state reconciliation (P1 ruling)

The 11 living docs cannot be promoted as canonical by a pure rename; several carry deleted names and
stale paths. Confirmed examples:
- `transport_surface.md` names `KlUdpServer` and `udp_server.h` (both removed by the KlDatagram
  consolidation) and old test paths.
- `alpn_policy.md` cites `h2.c` (now `src/protocols/http2/`) and `src/http_connection.c` (now
  `src/protocols/http/http_connection.c`).

Therefore, in **C1-2**, each promoted living doc is **audited and rewritten against current headers,
source paths, gates, and tests**: every referenced type/function/header/path/test must exist and be
current; capability/behavior claims must match the code; and no deleted name (`KlUdp*`, old module
filenames, top-level `protocols/`, flat integration homes, `spikes/`, `parsers/`) may remain. The
existing stale-name / old-layout gates cover the code side; C1-2 extends the same discipline to the
promoted living prose. The archived docs (§2d) are pure moves; they record history and are not
reconciled.

## 4. Gate and living-link reconciliation (path-only; in the C1-1 move commit)

- **`DOC_REF_FILES`** = `docs/architecture.md docs/architecture_invariants.md` →
  `docs/architecture/overview.md docs/architecture/invariants.md`.
- **`HTTPLEGACY_SCAN`** living-doc tail → repoint the moved living paths; **drop** `docs/comparison.md`
  and `docs/roadmap.md` (both deleted); **add** `docs/roadmap/roadmap.md`.
- **`check-old-layout` / `check-boundaries` / `check-no-httplegacy`** scan/exclusion sets that name a
  moved or deleted doc → repoint or drop accordingly.
- **`check-doc-refs`** validates in-repo links in `DOC_REF_FILES`; a living-doc link to a now-archived
  design repoints to the archive path (still in-repo, so it resolves) or to its living replacement.
- `.claude/skills/*/SKILL.md` naming `docs/keel_audit.md` etc. are skill tooling → **left to C4**,
  except where a path they name is deleted (fix the dangling reference in C1-1).

Touching the Makefile scan-path lists means C1-1's local validation includes a release compile.

## 5. Recorded rulings

- **D1**: delete the untracked, previously-protected `docs/claude_code_project_restructure_prompt.md`
  in C1-1. Explicit authorization; its decisions are captured by accepted freezes.
- **D2**: keep `alpn_policy.md` under `contracts/` after correcting its paths/behavior in C1-2; delete
  `comparison.md` (not promoted to `operations/`).
- **D3**: no placeholder/stub docs. C1 lands in three reviewable subincrements (§6). Omit
  `backend_confidence.md` until E3.
- **D4**: delete `roadmap.md`.

## 6. Sequencing (each subincrement validated with proportionate local checks; pause after each)

1. **C1-0 (this freeze).** Docs-only. Commit, pause.
2. **C1-1: mechanical.** Create the taxonomy dirs; `git mv` the living docs to their paths and every
   archived doc into `archive/<category>/`; delete `roadmap.md`, `comparison.md`, the two captured
   prompts, and the protected restructure prompt (D1); repoint the gate scan-path lists + living-doc
   links (§4). No living-doc content rewrite yet. Validate (`git diff --check`, `make check-doc-refs`,
   stale-name/old-layout gates, and a release compile since the Makefile is touched). Pause.
3. **C1-2: living audit + rewrite.** Reconcile each promoted living doc against current code (§3).
   Validate (`git diff --check`, `check-doc-refs`, stale-name/old-layout gates; no compile, docs
   only). Pause.
4. **C1-3: operations authoring.** Author `operations/testing.md` and `operations/fuzzing.md` with
   evidence-linked commands. Validate as C1-2. Pause.

No documentation is moved, deleted, or rewritten until this freeze is accepted.
