# C5 - Site content and information-architecture rebuild (audit + freeze)

Status: PROPOSED (docs-only). No site file is changed until this freeze is accepted. Governing
roadmap: [roadmap/roadmap.md](../../roadmap/roadmap.md), Phase C / C5. Written em-dash-free so it passes
the C3 gate. C5 is a genuine content and information-architecture rebuild against the final
architecture, not a cosmetic name substitution.

## 1. Inventory (what is tracked)

`git ls-files 'site/*'` is exactly four files:

- `site/index.html` (991 lines) - the entire single-page landing site.
- `site/Makefile` - copies the static files into `build/` (`STATIC_FILES = index.html mark.svg
  og-card.svg`), plus `serve` (python3 http.server) and `clean`.
- `site/mark.svg` - the brand mark (used as the favicon, `rel="icon"`).
- `site/og-card.svg` - the Open Graph / Twitter card image (1200x630).

## 2. Tracked build output (decision D1: RESOLVED by evidence, confirm policy)

`site/build/` is **already .gitignore-d** (`.gitignore` line `site/build/`) and has **0 tracked
files** - no generated output is in Git. The reviewer's "remove tracked build output" is already
satisfied. Recommendation: keep `build/` untracked and generate it reproducibly with `make -C site`.
Add a deterministic validation target (D6) rather than committing
output.

## 3. External runtime dependencies (decision D2)

`index.html` loads three third-party resources at runtime:

- **Tailwind CDN** (`https://cdn.tailwindcss.com`) - this is Tailwind's PLAY/dev CDN, which Tailwind
  itself documents as not for production (it compiles utility CSS in the browser on every load).
- **Google Fonts** (`fonts.googleapis.com` + `fonts.gstatic.com`, the Inter family).
- **Lucide icons** (`https://unpkg.com/lucide@latest/...`) - `@latest` is an unpinned, mutable tag.

So the deployed page is network-dependent and non-reproducible (a CDN outage, a Tailwind CDN
deprecation, or an unpinned Lucide release breaks or changes it).

**D2 - RESOLVED (by review): remove the runtime dependencies WITHOUT vendoring the full Tailwind
toolchain or Google Fonts.** Use a committed, purpose-built stylesheet (hand-authored CSS covering
what the page actually uses, including its custom colors and the responsive + reduced-motion rules),
the system font stack instead of Inter, and inline/local SVG for only the icons actually used. This
yields a smaller, offline site with no Node, no browser-side CSS compilation, no font binaries, no
mutable CDN assets, and no new licensing/provenance overhead. Outbound navigation links and canonical
metadata URLs (the GitHub deep links, the OG image URL) remain allowed; the ban is on runtime
stylesheet / font / script / image dependencies on third-party hosts.

## 4. Content and IA audit (against the final architecture)

Current IA: a sticky nav (`#layers`, `#principles`, `#protocols`, `#compare`, `#performance`,
`#roadmap`, `#security`) over sections `layers / principles / compare / protocols / performance /
roadmap / how / security / trust`. Solid bones; the rebuild verifies every claim and tightens the
narrative.

- **Architecture / capability claims.** The hero and meta describe the async I/O core (epoll,
  kqueue, WSAPoll, io_uring, IOCP), the lwIP raw NO_SYS provider, the freestanding UEFI EFI_TCP4/UDP4
  provider, and TCP + UDP transports. These match the final tree and stay, but each backend/platform
  claim must be tied to EXACT evidence (which gate/harness proves it: the readiness/completion unit
  suites, the pollcomp/io_uring smokes, the lwIP raw loopback, the UEFI QEMU self-tests) and stated
  without "production-ready" marketing. No "production-ready" string is currently present (good);
  keep it that way.
- **Volatile and inconsistent counts (must go).** The page contradicts itself and the repo:
  "339K req/s benchmark peak" (line 217) vs README's 101K; "65 suites under ASan/UBSan, plus 8 fuzz
  targets" (line 331) vs "863 tests across 56 suites" (line 927). Policy (matching C4 D1): remove
  hard suite/test counts; the fuzz-target and backend enumerations may stay (stable). A single
  benchmark headline may remain only if it is reproducible and dated (from `make bench` /
  `bench/bench.sh`), and consistent with the README; otherwise it is removed.
- **API examples.** Any code snippets must compile against the current public headers (the same
  standard the C4 guidance now meets: `KlHttpServer`/`KlHttpServerConfig`, `kl_http_server_*`, the
  datagram surface). Verify each snippet.
- **Navigation + internal links + repo paths.** The `#anchor` nav must match real section ids; the
  favicon/OG asset links must resolve; the GitHub deep links currently referenced
  (`bench/bench.sh`, `.github/workflows/benchmark.yml`, `examples/`, `#readme`,
  `security/advisories/new`) all exist today and must be re-verified after any edit.
- **Metadata / Open Graph.** `<meta name="description">`, `og:*`, `twitter:card`, and the
  `ld+json` SoftwareApplication block are present and mostly current; reconcile the description text
  with the final capability set and keep the OG image dimensions (1200x630) correct.
- **Accessibility.** `<html lang="en">` and a `prefers-reduced-motion` block exist; there are only
  3 `aria-*` attributes and 0 `role=`/`alt=` (the SVGs are favicon/OG, not inline `<img>`). The
  rebuild adds proper landmarks/labels (nav, main, sections), visible focus states, color-contrast
  checks, and keyboard operability.
- **Responsive + reduced-motion.** 72 Tailwind responsive-breakpoint utilities and a correct
  `@media (prefers-reduced-motion: reduce)` rule are present; preserve both and verify at mobile /
  tablet / desktop widths after the rebuild.

## 5. Brand assets (decision D3)

`mark.svg` and `og-card.svg` are the existing brand. Recommendation: **preserve both**; avoid
gratuitous brand changes. `og-card.svg` is only updated if the headline claims it renders change
(so the card does not state a stale number). No new mark unless the reviewer requests one.

## 6. Deterministic validation target (D6)

Add a deterministic `make -C site check` target AND a root `check-site` target (D6 - RESOLVED)
that is deterministic and offline: HTML well-formedness / basic lint, internal-anchor and asset-link
resolution, presence of required meta/OG tags, no unpinned CDN reference once D2 lands, and the
em-dash gate already covers it. Enroll it in CI ONLY at the combined C1-C5 reconciliation (no
`.github` churn now).

## 7. Increment sequence (proposed; per-increment review, no push)

1. **C5-1** - build + dependencies: replace the Tailwind CDN with a committed purpose-built stylesheet
   (its custom colors + responsive + reduced-motion rules), the system font stack instead of Google
   Fonts, and inline/local SVG for only the icons used; update `site/Makefile` to ship the stylesheet;
   keep the build network-free, tool-light, and `build/`
   untracked.
2. **C5-2** - content + IA: rebuild the narrative and sections against the final architecture; fix
   every claim, snippet, link, and repo path; remove volatile counts; reconcile the benchmark
   headline; tighten navigation.
3. **C5-3** - metadata, accessibility, responsive: reconcile meta/OG/ld+json; add landmarks, labels,
   focus states, contrast, keyboard nav; verify responsive + reduced-motion.
4. **C5-4** - the deterministic `check-site` validation target (+ archive the C5 freeze).

## 8. Validation (per increment)

- The site builds offline and reproducibly (`make -C site` with no network once D2 lands).
- `check-site` (from C5-4) and `check-no-em-dash` pass; internal anchors and asset links resolve;
  referenced repo paths exist.
- No volatile test/suite count and no "production-ready" marketing remains; every backend/platform
  claim maps to a named piece of evidence.
- No remote CI between C5 increments; the single combined C1-C5 PR remains the validation point.

## 9. Decisions for review (summary)

- **D1 - RESOLVED**: keep `site/build/` untracked and reproducibly generated.
- **D2 - RESOLVED**: remove runtime CDNs; use a committed purpose-built stylesheet, the system font
  stack, and inline/local SVG icons (no vendored Tailwind toolchain or font binaries).
- **D3 - RESOLVED**: preserve `mark.svg` / `og-card.svg`; update the card only if its visible claims go stale.
- **D6 - RESOLVED**: `make -C site check` + a root `check-site` target; enroll the root target in CI only at the C1-C5
  reconciliation.

No site file is edited until this freeze is accepted.
