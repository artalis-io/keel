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
  New ops are only appended at the end (see the append-only policy below).

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

## Extending vtables and configs: append-only, zero-default

Keel's sole forward extensibility policy for public configuration structs and
provider/vtable types is **append-only, zero-default** evolution (rationale and
the rejected alternatives are recorded in `docs/archive/f2/f2_c_extensibility_decision.md`):

1. **Append only.** A public caller-constructed config or vtable grows only by
   APPENDING trailing members. Never reorder, insert-in-the-middle, remove,
   rename-in-place, retype, or resize an existing member.
2. **Zero is the default.** Every appended member's zero/NULL value MUST be a
   valid, behavior-preserving default ("not supplied" / "use the built-in
   default"). A feature whose off-state is not zero needs a new function or an
   opt-in flag, not a silent field.
3. **Callers zero-initialize and recompile.** Consumers zero-initialize (`= {0}`,
   a designated initializer, or `memset`) and recompile against the header they
   build against. That recompile, already required by source + static-relink, is
   the only migration cost of an appended member: C zero-fills omitted trailing
   members, so existing designated and `memset`-zero initializers stay correct.
4. **Optional ops are NULL-checked; a required subset is validated once.** Core
   NULL-checks optional appended vtable ops; a required subset is enforced at an
   installation/factory/init boundary by a `*_vtable_valid` validator (as `KlTls`
   does via `kl_tls_vtable_valid`), not repeatedly in hot paths. A provider built
   against a newer header that leaves a new optional slot zero is treated as
   "not supplied".
5. **Callback and factory signatures are frozen.** Extend behavior by APPENDING a
   new vtable slot, never by changing an existing callback's or factory's
   signature; pass new construction inputs through the owning config or context.
6. **Keep any slot marked "MUST stay last"** (for example `KlEventOps.completion`)
   last.

Keel does **not** use a `struct_size`/`api_version` field. Such a field only
detects an old-binary-vs-new-binary mismatch, which the source + static-relink
promise and the W^X/no-dlopen posture (no runtime binary plugin ever meets the
library) make impossible; append-only zero-default already delivers compatible
growth with less apparatus. Each config and vtable header states its own
required-versus-optional members and its append point, so this contract is not
restated per type.

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

## Supported platforms and backends

Which toolchains, event backends, socket/platform providers, and integrations are supported (and at
what level: standing CI, strict compile gate, local-only, or bring-your-own) is documented separately
and authoritatively in [platform support](../operations/platform_support.md). This contract governs how
the API evolves; that document governs where it is built and tested.
