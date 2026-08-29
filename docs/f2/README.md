# F2 public-surface inventories and classification manifests

This directory holds the reproducible, tree-derived inventories of Keel's installed public surface and
the reviewed classification manifests the F2 gates enforce. It began (F2-1a) as pure DATA scaffolding;
the classifications have since been decided and are now the machine-consumed manifests behind the
standing CI gates (`make check-public-headers`, `check-public-coverage`, `check-allocator-boundaries`,
`check-no-eventloop-fd`, `check-install`, `check-installed-consumer`). This directory stays LIVE (the
manifests are regenerated and checked in place); the frozen F2 narrative and the decision records are
archived under `docs/archive/f2/` (see `docs/archive/f2/f2_public_surface_install_freeze.md` for the
full freeze and the increment sequence).

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

- `type_classification.tsv` - `<header>\t<type>\t<kind>\t<category>\t<note>`. Filled in F2-B1: all 193
  rows carry a final category (zero `UNRESOLVED`) and a note. Key: (header, type, kind).
- `function_coverage.tsv` - `<function>\t<header>\t<classification>\t<citation>`. A newly seeded row
  starts at classification `unreviewed` (which is NOT an accepted value, so an unclassified function
  fails `--check`: default-deny). Decided in F2-4: all rows now carry an accepted classification and a
  concrete non-`-` citation. Key: function name.

An empty `note` or `citation` field is written as `-` (a literal empty final column would be trailing
whitespace).

## Type categories (the accepted F2-B nine-category taxonomy)

Each public type is classified into exactly one (see `docs/archive/f2/f2_b_public_layout_decision.md`):

- `caller-constructed` - the caller/provider allocates AND fills it (configs, `KlIoVec`, callback/hook
  containers, vtables such as `KlSocketOps`, `KlDatagramOps`, `KlTls`). Append-only per F2-C.
- `caller-inspectable` - the library fills it and the caller reads defined fields (`KlUrl`,
  `KlHttpServerStats`, `KlResolveResult`, `KlPeerCert`, `KlHttpClientResponse`).
- `caller-owned-value` - the caller allocates the storage via an init-in-place function; the library
  owns most fields; some fields are documented public facets, the rest are visible solely to permit
  allocation and evolve append-only (`KlHttpServer`, `KlEventCtx`, `KlEventLoop`, `KlHttpRouter`, ...).
- `opaque` - handle-only; layout private to `src/` or an internal header. A concrete definition
  classified `opaque` (a v3 relocation, e.g. `KlHttpConn`) MUST carry a migration note.
- `opt-in-unstable-layout` - opaque handle whose layout is in a `*_detail.h` the umbrella excludes
  (`KlStream`, `KlListener`, `KlConnectOp`, `KlDatagram`).
- `api-signature` - a callback typedef (a signature, not a layout).
- `type-alias` - a typedef alias.
- `enum-constants` - a public enumeration; append-only value set.

The prior scaffold placeholders `unresolved-v3-decision`, `borrowed-handle-published-layout`, and
`impl-layout-installed` are gone: F2-B/F2-B1 gave every type a concrete v3 destination, so the accepted
vocabulary is these eight categories (`impl-layout-installed` described a migration finding, not a
surface Keel freezes, and reached zero).

`--check` enforces kind-to-category validity: a `callback` must be `api-signature`, an `alias` must be
`type-alias`, an `enum` must be `enum-constants`, an `opaque`-kind must be `opaque` or
`opt-in-unstable-layout`, a concrete-def `opaque` requires a migration note, and any `UNRESOLVED` (or
otherwise unknown) category fails (zero-UNRESOLVED, default-deny for a new unclassified type).

## Coverage classifications (decided in F2-4)

Each function is classified into exactly one:

- `direct-assertion` - a test asserts on its observable effect.
- `indirect-execution` - exercised via a higher-level path, not asserted by name.
- `compile-only` - referenced only to compile, not behaviorally checked.
- `example` - demonstrated in `examples/` without a test assertion.
- `intentional-untested` (or `intentional-public-but-untested:<reason>`) - a reviewed decision to ship
  it untested, with the rationale recorded in the citation column.

The default seed value `unreviewed` is deliberately NOT in this set, so a newly added public function
fails the gate until it is classified (default-deny). A by-name caller does not auto-map to
`direct-assertion`. Internal dispatch hooks reached only indirectly trigger a public-surface decision
under F2-B (should the symbol be public at all), not an automatic exemption.

Current counts (F2-4/F2-5, all 302 functions classified): 252 direct-assertion, 45 indirect-execution,
3 intentional-untested, 2 compile-only. The prioritized gap list closed in F2-5 is in
`coverage_gaps.md`.

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

Type categories are final (zero `UNRESOLVED`, validated per the kind-to-category rules above). Function
coverage is fully decided (zero `unreviewed`; F2-4/F2-5). Function extraction is a documented heuristic.
`--check` and `--selftest` are wired into the standing `make` gates: `check-public-headers` (headers +
inventories/joins), `check-public-coverage` (the coverage manifest + extraction canaries),
`check-install` / `check-installed-consumer` (install + out-of-tree consumer), `check-allocator-boundaries`,
and `check-no-eventloop-fd`. All six run in the CI gates job.
