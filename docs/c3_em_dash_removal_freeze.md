# C3 - Em-dash removal + permanent no-em-dash gate (inventory + freeze)

Status: PROPOSED (docs-only). No em-dash is removed and no gate lands until this freeze is
accepted. Governing roadmap: [roadmap/roadmap.md](roadmap/roadmap.md), Phase C / C3. This file is
written em-dash-free on purpose, so it models the target style and passes its own future gate.

## 1. Goal

Remove the em-dash (U+2014, "-") from authored KEEL text and add a permanent gate so it cannot
return. The em-dash is a stylistic tell; the house style uses the ASCII hyphen-minus. This is a
text-only pass with the same discipline as C2: mechanical, reviewed per increment, behavior
preserved (a compile proves it for code).

## 2. Inventory (tracked, vendor/ excluded)

Population of U+2014 by area (files / occurrences):

| Area          | files | occ.  |
|---------------|-------|-------|
| include/      | 45    | 282   |
| src/          | 133   | 1283  |
| src/protocols (subset of src) | - | - |
| tests/        | 184   | 1382  |
| integrations/ | 142   | 1347  |
| examples/     | 34    | 87    |
| bench/        | 5     | 15    |
| fuzz/         | 5     | 10    |
| docs/         | 76    | 3858  |
| tools/        | 3     | 37    |
| README*.md    | 12    | 241   |
| CLAUDE.md     | 1     | 82    |
| CONTRIBUTING.md | 1   | 10    |
| Makefile(s)   | 10    | 205   |

Total: 572 files, ~9100 occurrences.

Spacing is uniform: **7586 spaced ` - ` forms vs 1 tight `x-y`** across `.c/.h/.md`. The canonical
substitution is therefore trivial and safe (see §4).

## 3. The safety split (the one real decision)

Not all em-dashes are authored prose. In `.c/.h` they divide into:

- **Comments: 3529 occurrences** - authored text. Safe to sweep, byte-identical-code provable
  (exactly the C2 comment-only proof).
- **String literals / program output: 145 lines** - behavior-bearing. Many are **CI-oracle markers**
  that harness scripts grep for verbatim, e.g. `"PASS B (%s) - %zu bytes"`,
  `"S-4: GO - served GET / (200)"`, `"PASS T6 (glue: one-held overflow - ...)"`. Rewriting these
  changes program output and can break the oracle scripts. They are runtime text, not authored
  style.

The Makefile has the same split: ~176 comment occurrences plus **23 `@echo`/`printf` recipe strings**
that print to build output.

**Recommendation (mirrors the accepted C2 ruling):** the removal and the gate cover **comments and
prose docs only**; **string literals and program-output strings are out of scope** (exempt), exactly
as C2 exempted string literals. The em-dash-as-AI-tell concern is about authored text, not runtime
output, and the oracle risk is real. If instead the reviewer wants program output em-dashes gone
too, that is a separate, later increment done in lockstep with the harness-oracle scripts (an
explicit, non-mechanical change), and the gate would move to full-text - this freeze does not assume
it.

Open decision D1 for review: **exempt string literals / program output (recommended), or include
them with lockstep oracle updates.**

## 4. Canonical substitution

- Spaced form ` - ` (space, U+2014, space) -> ` - ` (space, hyphen-minus, space). Covers 7586 of
  7587 cases.
- The single tight `x-y` case is handled by hand (most likely `x - y`).
- Leading/trailing in-comment forms (` * - foo`, `foo -`) map to the hyphen-minus in place.

Open decision D2 for review: the replacement glyph. **` - ` (single spaced hyphen-minus, recommended
- cleanest, matches existing hyphen usage)** vs ` -- ` (double hyphen, unambiguously "was an
em-dash"). One choice, applied uniformly.

## 5. Explicitly NOT in scope

- **Box-drawing characters** (U+2500 "-" horizontal, U+2502 "|", corners) used in `-- section --`
  banners and boxed headers are NOT em-dashes and are preserved. The gate targets the exact codepoint
  U+2014, so it can never match them.
- **En-dash U+2013 (161 occ.)** and **minus sign U+2212 (11 occ.)** are a separate, smaller concern;
  C3 is scoped to U+2014 only, per the "em-dash" mandate. They may be a later pass. (Noted so the
  numbers are not a surprise.)
- **vendor/** is never touched.

## 6. The permanent gate (`check-no-em-dash`)

Lands in the final C3 increment, wired into `.PHONY` and the local gate set like the other `check-*`
gates. Design, consistent with §3:

- **Scans** every tracked first-party file, `vendor/` excluded.
- **Scope by file type**, honoring the §3 exemption:
  - `.c` / `.h` (and Makefile): **comment text only**, reusing the C2 splice-aware C-comment
    extractor (`tools/check_no_milestones.pl`'s proven state machine) so string literals / program
    output are ignored naturally. (Makefile uses `#`-to-EOL comment extraction.)
  - `.md` and other prose docs: **full text** (no string-literal concept).
- **Rejects** exactly U+2014. Prints `file:line` on a hit and fails. **No file, line, or marker
  exceptions.**
- **Self-canaried**, positive and negative, run before the tree scan: a positive (an em-dash in a
  comment MUST flag); negatives (an em-dash in a `.c` string literal MUST pass; a box-drawing U+2500
  banner MUST pass; an en-dash U+2013 MUST pass). Abort loudly on any regression.

Open decision D3 for review: whether the gate is **comment-only for code (recommended, per D1)** or
full-text repository-wide (only if D1 chooses to include program output).

## 7. Increment sequence (proposed; per-increment review, no push)

Mirrors C2's cadence; each is mechanical and compile-verified:

1. **C3-1** - public headers `include/keel/*.h` (comments).
2. **C3-2** - substrate `src/*.{c,h}` (comments).
3. **C3-3** - protocols `src/protocols/**/*.{c,h}` (comments).
4. **C3-4** - integrations non-test `.c/.h` (comments).
5. **C3-5** - tests + harnesses `.c/.h` (comments); string-literal oracle markers preserved per D1.
6. **C3-6** - prose: `docs/**`, `README*.md`, `CLAUDE.md`, `CONTRIBUTING.md`, Makefile comments
   (and `@echo` recipe strings only if D1 includes program output).
7. **C3-7** - permanent `check-no-em-dash` gate, run green over the whole tree; archive the C3 freeze
   to `docs/archive/freezes/`.

The reviewer may regroup (the substitution is uniform, so fewer increments are viable).

## 8. Validation (per increment)

- `git diff --check` (no whitespace damage).
- For every `.c/.h` increment, the **comment-only byte-identical proof** (comment-strip diff vs HEAD)
  - the code must not change; only comment bytes differ.
- A **release compile** (`make`), plus `make test` on the substrate and tests increments.
- The stale-name / layout / doc gates stay green, including `check-no-milestones` and, from C3-7, the
  new `check-no-em-dash`.
- A repo-wide em-dash count reaches **zero in the scoped surface** (comments + prose; program output
  excluded per D1).
- No remote CI between C3 increments.

## 9. Open decisions for review (summary)

- **D1** - string literals / program output: exempt (recommended) or include with lockstep oracle
  updates.
- **D2** - replacement glyph: ` - ` (recommended) or ` -- `.
- **D3** - gate scope: comment-only for code (recommended, follows D1) or full-text.

No source or doc text is edited until this freeze is accepted.
