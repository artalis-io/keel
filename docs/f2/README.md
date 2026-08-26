# F2-1a public-surface inventories and classification scaffolding

This directory holds the reproducible, tree-derived inventories of Keel's installed public surface and
the classification scaffolds that later F2 increments decide against. It is DATA scaffolding: F2-1a
adds no API change, edits no public header, and wires no gate. See
`docs/f2_public_surface_install_freeze.md` for the full freeze and the increment sequence.

## Files

Regenerated on every run (authoritative, tree-derived by `tools/f2_public_inventory.sh`):

- `public_headers.txt` - one installed header basename per line (from `git ls-files include/keel/*.h`).
  55 headers.
- `public_functions.tsv` - `<function>\t<header>`: distinct public `kl_*` function declarations, one
  row per (name, declaring header). 318 rows over 316 unique names (`kl_version` and
  `kl_version_number` are declared in both umbrellas).
- `public_types.tsv` - `<type>\t<kind>\t<header>`: the COMPLETE public type surface, one resolved row
  per type. 193 types. `kind` is one of:
  - `struct` / `union` / `enum` - a concrete body on the main (non-detail) surface. Detected from both
    typedef closers (`} KlName;`) and tag-form bodies (`struct KlName { ... };`), so vtables like
    `KlTls`/`KlResolver`/`KlEventOps` and header structs like `KlHttpRequest` are included (a plain
    `} KlName;` grep misses these).
  - `opaque` - a forward-declared handle whose layout is in a `*_detail.h` (opt-in) or private to
    `src/`, not on the main surface (`KlDatagram`, `KlStream`, `KlListener`, `KlHttpClient`, ...).
  - `callback` - a function-pointer typedef. This is an API SIGNATURE, not an object layout, and is
    tagged distinctly so F2-B never confuses a callback with a struct layout.
  - `alias` - a typedef alias to a primitive or other type. Accepts the `Kl*` convention
    (`KlSocketHandle`) plus the one intentionally-public lowercase Keel typedef, `kl_ssize_t`
    (handle.h); a `typedef` keyword is required, so struct fields and arbitrary lowercase
    implementation identifiers are never collected.

Current kind counts: 89 struct, 25 enum, 16 opaque, 60 callback, 3 alias, 0 union.

Seeded once, then human-owned (a re-run appends new keys with default rows but never rewrites existing
ones):

- `type_classification.tsv` - `<header>\t<type>\t<kind>\t<category>\t<note>`. Every row starts at
  category `UNRESOLVED`. Decided in F2-B. Key: (header, type, kind).
- `function_coverage.tsv` - `<function>\t<header>\t<classification>\t<citation>`. Every row starts at
  classification `unreviewed`. Decided in F2-4. Key: function name.

An empty `note` or `citation` field is written as `-` (a literal empty final column would be trailing
whitespace).

## Type categories (decided in F2-B)

Each public type is classified into exactly one:

- `caller-constructed` - the caller allocates and fills it (configs, `KlIoVec`, callback/hook structs,
  vtables such as `KlSocketOps`, `KlDatagramOps`, `KlTls`).
- `caller-inspectable` - the library fills it and the caller reads defined fields (`KlUrl`,
  `KlHttpServerStats`, `KlProxyResult`, `KlResolveResult`, `KlPeerCert`, `KlHttpClientResponse`).
- `opaque` - handle-only on the public surface; layout in a `*_detail.h` or private.
- `opt-in-unstable-layout` - layout deliberately behind a `*_detail.h` the umbrella excludes.
- `impl-layout-installed` - runtime-state struct exported through `include/` only because a
  protocol/substrate TU consumes it, with no caller contract.
- `unresolved-v3-decision` - the classification itself is the decision (KlEventLoop, KlEventCtx,
  KlHttpServer, KlHttpConn/Pool).
- `api-signature` - a callback typedef (a signature, not a layout).
- `type-alias` - a primitive alias.

All 193 rows are `UNRESOLVED` until F2-B classifies them against the accepted F2-C (extensibility) and
F2-D (KlEventLoop.fd) policies. No decision is pre-made here.

## Coverage classifications (decided in F2-4)

Each function is classified into exactly one:

- `direct-assertion` - a test asserts on its observable effect.
- `indirect-execution` - exercised via a higher-level path, not asserted by name.
- `compile-only` - referenced only to compile, not behaviorally checked.
- `example` - demonstrated in `examples/` without a test assertion.
- `intentional-public-but-untested:<reason>` - a reviewed decision to ship it untested.

A by-name caller does not auto-map to `direct-assertion`. Internal dispatch hooks reached only
indirectly trigger a public-surface decision under F2-B (should the symbol be public at all), not an
automatic exemption.

## Regenerating and checking

```
tools/f2_public_inventory.sh             # (re)generate the inventories, seed the scaffolds
tools/f2_public_inventory.sh --check     # verify the raw inventories reproduce AND that both
                                         # scaffolds are exact key-set joins: a missing row, a stale
                                         # row for a removed symbol, a duplicate key, a malformed or
                                         # unknown classification value, or a drifted header
                                         # attribution all fail (non-zero)
tools/f2_public_inventory.sh --selftest  # run the extraction canaries on a synthetic fixture:
                                         # concrete struct/union/enum, opaque handle, callback (must
                                         # not be a layout), cross-umbrella duplicate function, a fake
                                         # declaration inside a comment, a multiline declaration, and
                                         # a preprocessor-guarded declaration
```

During the undecided stage `UNRESOLVED` and `unreviewed` are valid classification values; later gates
(F2-B, F2-4) tighten by forbidding those defaults. Function extraction is a documented heuristic; an
AST-grade extractor is the F2-4 concern. `--check` is not yet wired into any `make` gate; that wiring
is F2-1b (headers), F2-3 (install), and F2-4 (coverage).
