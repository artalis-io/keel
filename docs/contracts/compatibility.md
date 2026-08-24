# Keel compatibility promise (Phase 6)

The contract for how Keel's public API evolves, so consumers (and the first-party
integrations) know what they can rely on. Authoritative; paired with the
`KL_VERSION_*` macros in `keel.h` and the `kl_version()` runtime accessors.

## The promise: source + static-relink

Keel guarantees **source and static-relink compatibility** within a major
version, **not** binary (ABI) compatibility.

- **Source compatible:** code that compiles against version *X* compiles against
  any *X.y.z* with y/z ≥, without edits.
- **Static-relink:** you rebuild your objects against the new headers and
  re-link the new `libkeel.a`. Because Keel is a static library that consumers
  embed (and because callers allocate many Keel structs *by value*; see below),
  a version bump is a **recompile**, never a drop-in `.so` swap.

Keel does **not** ship a versioned shared object or promise stable struct sizes
across versions. This is deliberate: it keeps the core dependency-light and
W^X/static-linking friendly (`keel.h` forbids `dlopen`/JIT; see `SECURITY.md`)
and avoids the ABI-freezing complexity that a plugin/shared-object model demands.

## Versioning (SemVer)

`KL_VERSION_MAJOR.MINOR.PATCH` (currently **2.9.0**), also as the packed
`KL_VERSION_NUMBER` (e.g. `20900`) for `#if` gating, and at runtime via
`kl_version()` / `kl_version_number()`, which report the *linked library*, so a
consumer that relinks a newer Keel can verify it at runtime.

- **MAJOR**: a breaking change to existing public API (removed/renamed function,
  changed signature or semantics, reordered struct fields or enum values).
- **MINOR**: additive, source-compatible: new functions, new trailing struct
  fields, new enum values appended at the end. **Requires a recompile** (struct
  sizes may grow) but no source edits.
- **PATCH**: bug fixes, no API surface change.

## Stable within a major version

- Public function names, signatures, and documented semantics.
- The order and meaning of existing fields in public structs.
- Existing `KlError` and other enum values.
- The function-pointer order of published vtables (`KlTls`, `KlHttpBodyReader`,
  `KlHttp2ClientSession`, `KlHttp2ServerSession`, `KlResolver`, `KlCompress`, …).

## May evolve without a major bump (minor)

- **Appending a trailing field** to a caller-allocated struct. Callers that
  zero-initialize (designated initializers or `memset`) are unaffected in source;
  they recompile for the new `sizeof`. Worked precedent: `KlAsyncOp._terminal`
  (Phase 4): internal, zero-initialized, set only by Keel.
- **New functions** (e.g. `kl_async_cancel` in Phase 4, `kl_version` in Phase 6).
- **New enum values appended** at the end.
- **New optional config knobs** as trailing fields (0 = prior default).

Because callers embed structs like `KlHttpServerConfig`, `KlAsyncOp`, and `KlHttpClientConfig`
by value, appending a field changes `sizeof`; hence the recompile. Never insert
or reorder existing fields within a major version.

## Extensible vtables: the `struct_size` / `api_version` convention

For a struct or vtable that Keel expects to extend heavily over time, the
robust pattern is to lead with a size/version field so the callee can detect
which fields the caller knows about:

```c
typedef struct {
    size_t struct_size;   /* sizeof(the caller's view); Keel checks before reading newer fields */
    /* ... fields ... */
} KlSomethingConfig;
```

This is a **convention for new extensible surfaces going forward**, not a
retrofit: adding a leading `struct_size`/`api_version` to an *existing* struct
would shift every field offset (a MAJOR break), so existing structs keep the
plain trailing-field rule above. New integration vtables that anticipate multiple
independent backends should adopt it from the start.

## Integrations

The first-party adapters in `integrations/` implement **existing core vtables**
(`KlTls` for mbedTLS; `KlHttp2ClientSession`/`KlHttp2ServerSession` for nghttp2). They
track those vtables and are versioned with the core; a vtable change is a source
change the adapter recompiles against. No integration adds a type to a core
public header, and none uses dynamic loading, a plugin registry, or global
mutable backend registration (see `integrations/README.md`).

## Out of scope (by policy)

Keel will not add (and a compatibility bump will never introduce) dynamic
loading (`dlopen`), runtime plugin discovery, executable memory, or global
mutable backend registration. Backends are selected at build time (`BACKEND=`,
`KEEL_TLS=`) or wired explicitly via a provider/vtable the caller passes in.
