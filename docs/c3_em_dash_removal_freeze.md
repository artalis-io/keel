# C3 - Em-dash removal + permanent no-em-dash gate (inventory + freeze)

Status: PROPOSED (docs-only). No em-dash is removed and no gate lands until this freeze is
accepted. Governing roadmap: [roadmap/roadmap.md](roadmap/roadmap.md), Phase C / C3. This file is
written em-dash-free on purpose, so it models the target style and passes its own future gate.

Amended after review: the roadmap resolves D1 (include string literals and program output, no
exemptions), D2 (context-appropriate ASCII punctuation, not a universal hyphen), and D3 (full-text
exact-codepoint gate over all prose-bearing formats, no exceptions). Those rulings are folded in
below.

## 1. Goal

Remove the em-dash (U+2014) from ALL authored KEEL text, active and archived, and add a permanent
gate so it cannot return. The em-dash is a stylistic tell; the house style uses ASCII punctuation.
This is text-only for logic, but it is NOT universally code-byte-identical: emitted string literals
change too, so it carries an output-to-oracle obligation (below).

## 2. Inventory (the single scope definition)

Scope is defined ONCE and shared by this freeze, the sweep, and the gate: **every tracked file
(`git ls-files`), minus `vendor/`, minus binaries (any file containing a NUL byte).** There is no
format allowlist - the set is derived from the tree, so a future extension (`.css`, `.js`, `.svg`,
`.json`, or any new site asset) is covered automatically and cannot smuggle the code point back.

U+2014 population over that population, by extension (complete, not an allowlist):

| Ext / kind      | files | occ.  |
|-----------------|-------|-------|
| `.md`           | 92    | 4257  |
| `.c`            | 294   | 2793  |
| `.h`            | 141   | 797   |
| no-ext (Makefiles, etc.) | 10 | 205 |
| `.sh`           | 28    | 158   |
| `.yml`          | 1     | 39    |
| `.pl`           | 2     | 17    |
| `.manifest`     | 1     | 2     |
| `.lua`          | 2     | 2     |
| `.gitignore`    | 1     | 2     |

Total: **572 files, 8272 occurrences.** This population explicitly includes the text the earlier
allowlist missed: Perl tooling (`tools/*.pl` - note `tools/check_no_milestones.pl` itself carries 11),
Lua (`bench/*.lua`), a manifest (`src/substrate-tus.manifest`), and `.gitignore`. Tracked site assets
with zero em-dashes today (`site/index.html`) are in scope by construction. Spacing is uniform
(7586 spaced forms vs 1 tight across `.c/.h/.md`), but per D2 the replacement is context-dependent,
not a single glyph (see §4).

## 3. Handling model (D1 resolved: include everything, no exemptions)

Em-dashes in `.c/.h` divide by locus, and BOTH are in scope now:

- **Comments (3529 in `.c/.h`) and prose (docs, README, CLAUDE, CONTRIBUTING, shell/Makefile
  comments):** authored text. Rewritten per D2. For code files the proof is code-token identity
  (§8), not full byte-identity, because string literals may also change in the same file.
- **String literals / program output (145 `.c/.h` lines, 23 Makefile `@echo`/`printf` recipes, and
  emitted text in shell scripts):** behavior-bearing. Rewritten per D2 AND reconciled with every
  consumer in the SAME increment.

### 3a. Output-to-oracle inventory (REQUIRED before editing any emitted literal)

Before an increment changes an emitted literal, it MUST first produce an inventory row per affected
literal:

- `file:line` of the literal;
- the emitted text (the marker/message);
- its consumer(s): every harness / oracle / `grep` / `awk` / `expect` that reads it (for example the
  `run_*.sh` oracles, `tests/e2e_examples.sh`, CI workflow asserts).

Every consumer is updated in lockstep in the same commit, and the affected harness is run, so a CI
marker can never silently diverge from its matcher.

Preliminary evidence (recorded, not a substitute for the per-literal inventory): a scan of all
tracked `*.sh` shows **no oracle matches on the em-dash glyph** - consumers key on ASCII prefixes
(`PASS`, `GO`, `DONE`, `FAIL`) and structural fields, never on `"... U+2014 ..."`. So most marker
rewrites are expected to be oracle-neutral; the inventory proves it case by case rather than
assuming it.

## 4. Substitution (D2 resolved: context-appropriate ASCII punctuation, not a universal hyphen)

A blind replacement to ` - ` would strip the code point but degrade thousands of sentences. Each
occurrence is rewritten by its grammatical role:

- **Sentence break** (two independent clauses): period or semicolon.
- **Explanation / list introduction** (the dash introduces what follows): colon.
- **Parenthetical aside** (a dash pair, or a mid-sentence aside): parentheses or commas.
- **Label / range / genuine dash** (compound label, numeric range, "A to B"): ASCII hyphen `-`, or
  `--` where a longer dash genuinely reads better.

This makes the sweep a per-occurrence judgment, not a `sed`. It is mechanical only in the sense that
it changes no logic; it is editorial in that each site is read. Increments are sized accordingly
(finer than a blind pass), and machine assistance may draft rewrites but every hunk is reviewed.

## 5. Explicitly NOT in scope

- **Box-drawing characters** (U+2500 horizontal, U+2502 vertical, corners) in `-- section --` banners
  and boxed headers: preserved. The gate targets the exact codepoint U+2014, so it never matches
  them.
- **En-dash U+2013 (161 occ.)** and **mathematical minus U+2212 (11 occ.)**: outside C3 (per the
  ruling). Noted so the numbers are not a surprise; a later pass may address them.
- **vendor/**: never touched.

## 6. The permanent gate (`check-no-em-dash`) (D3 resolved: full-text, all formats, no exceptions)

Lands in the final C3 increment, wired into `.PHONY` and the local gate set like the other `check-*`
gates.

- **Full-text** scan (NOT comment-only): the entire file content, because emitted literals are in
  scope too. No contextual, string-literal, directory, or historical exceptions.
- **File set (no format allowlist):** derived from `git ls-files`, excluding only `vendor/` (`.git/`
  is untracked and never appears). Each file is checked for a NUL byte and skipped if binary; every
  remaining text file is searched in full. This is the SAME scope definition as §2, so freeze, sweep,
  and gate share one population - and any future tracked text format (`.css`, `.js`, `.svg`, `.json`,
  a new site asset) is covered automatically, with no gate edit.
- **Rejects** exactly U+2014. Prints `file:line` on a hit and fails. No exceptions of any kind.
- **Self-canaried**, positive and negative, run before the tree scan: a positive (a U+2014 anywhere,
  in prose OR a string literal, MUST flag); negatives that MUST pass (a box-drawing U+2500 banner; an
  en-dash U+2013; a mathematical minus U+2212) - proving the gate is exact-codepoint and does not
  over-reach into the out-of-scope glyphs. Abort loudly on any regression.

## 7. Increment sequence (proposed; per-increment review, no push)

Area-based, per D-ruling that the sequence may stay area-based but output/oracle changes are
separated when they materially increase review risk:

1. **C3-1** - public headers `include/keel/*.h` (comments; any header string literals inventoried).
2. **C3-2** - substrate `src/*.{c,h}` (comments + any emitted literals with their §3a inventory).
3. **C3-3** - protocols `src/protocols/**/*.{c,h}`.
4. **C3-4** - integrations non-test `.c/.h`.
5. **C3-5** - tests + harnesses `.c/.h` AND their `.sh` oracles. This is the highest output/oracle
   density (marker strings + `run_*.sh`), so the emitted-literal rewrites and their oracle updates
   are grouped with, or split into a dedicated sub-commit from, the comment rewrites when that lowers
   review risk (per the ruling).
6. **C3-6** - ALL remaining tracked text outside the C/H area increments above. Explicitly:
   `docs/**` (active + archived), `README*.md`, `CLAUDE.md`, `CONTRIBUTING.md`,
   `.github/workflows/*.yml`, all shell (`tools/*.sh` and the harness `run_*.sh`/`build_*.sh` not
   already taken with C3-5), **Perl tooling (`tools/*.pl`, including this gate's sibling
   `check_no_milestones.pl`)**, **Lua (`bench/*.lua`)**, **manifests (`*.manifest`)**, **ignore/config
   files (`.gitignore`)**, `site/index.html` and any future site assets (SVG/CSS/JS/JSON), and Makefile
   comments plus `@echo`/`printf` recipe strings. This is defined as "the tracked-text population of
   §2 minus what earlier increments already cleared," so nothing tracked is left carrying the code
   point.
7. **C3-7** - permanent `check-no-em-dash` gate (itself em-dash-free), run green over the whole
   tracked-text population; archive the C3 freeze to `docs/archive/freezes/`.

## 8. Validation (per increment; adjusted for D1)

Because string literals change, `.c/.h` increments are NOT universally code-byte-identical. The
proof obligation splits:

- **Code-token identity (new):** strip comments AND blank string-literal CONTENTS (keep delimiters
  and code structure), then diff vs HEAD. The non-comment, non-string code tokens MUST be identical -
  this proves no logic changed even though comments and literal text did. (A small helper extends the
  C2 comment-stripper to also blank string contents.)
- **Output/oracle lockstep:** for every changed emitted literal, the §3a inventory's consumers are
  updated in the same commit and the affected harness is run (locally; the UEFI/QEMU and lwIP
  harnesses where reachable, otherwise the ASCII-prefix invariant is shown to hold).
- `git diff --check` (no whitespace damage).
- A **release compile** (`make`), plus `make test` on the substrate and tests increments.
- The stale-name / layout / doc gates stay green, including `check-no-milestones` and, from C3-7, the
  new `check-no-em-dash`.
- A repo-wide U+2014 count reaches **zero** across the full scoped surface (all formats in §6).
- No remote CI between C3 increments.

## 9. Decisions (resolved by review)

- **D1 - RESOLVED: include string literals and program output; no exemptions.** Rewrite emitted text
  and update every dependent grep/oracle in the same increment; the §3a output-to-oracle inventory is
  produced before editing so CI markers cannot silently diverge.
- **D2 - RESOLVED: context-appropriate ASCII punctuation** (sentence break: period/semicolon;
  explanation or list intro: colon; parenthetical: parentheses/commas; genuine label or range: hyphen
  or `--`), never a universal replacement.
- **D3 - RESOLVED: full-text exact-codepoint gate** over EVERY tracked, non-binary file (derived from
  `git ls-files`, NUL-skip for binaries, only `vendor/` excluded) - no format allowlist, so future
  site assets are covered automatically. No contextual, string-literal, directory, or historical
  exceptions. Box-drawing, en-dashes, and mathematical minus remain outside C3.

No source or doc text is edited until this amended freeze is accepted.
