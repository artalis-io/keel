# F2-C extensibility decision record (vtables and configuration structs)

Status: DECISION for review (F2-C step 1). Docs-only. No header, struct, consumer, test, or
integration changed by this document. Grounded in the live tree at base commit `4e667e1` on branch
`main` plus the F2-1a type inventory (`docs/f2/public_types.tsv`).

Scope: decide how Keel's public vtables and configuration structs evolve across 3.x, so that realistic
future extension is possible without a needless ABI apparatus. This is the F2-C decision that F2-B
consumes (it fixes how any retained caller-constructed layout is allowed to grow). Per the increment
rule, this decision is committed on its own and no header or consumer is edited until it is reviewed.

## 0. The compatibility promise this decision is measured against

Keel promises SOURCE compatibility plus STATIC RELINK within a major version. It does NOT promise
binary compatibility: a 3.x consumer is expected to recompile against the 3.y headers and statically
relink against `libkeel.a`. Two live facts make this the real contract, not an aspiration:

- The W^X posture (`make wx-guard`: no `mmap(PROT_EXEC)`, `dlopen`, JIT, or `popen` under `src/`)
  means Keel loads no binary plugin at runtime. Every provider and integration (socket providers,
  event providers, TLS/compression/HTTP2/resolver adapters, the lwIP and UEFI ports) is compiled from
  source and statically linked. There is no path by which an old-ABI caller meets a new-ABI library at
  runtime.
- Integrations reach core only through `include/keel/*.h` (gates G2/G3) and are built together with
  the core they target.

Consequence: an append-only, zero-default field change is fully compatible under this promise, because
every translation unit that constructs or copies the struct is recompiled against the same definition
before it is linked. A `struct_size`/`api_version` field earns its keep only when an old binary must
interoperate with a new binary WITHOUT recompiling, which Keel's promise and W^X posture exclude.

## 1. Audit method and the audited surface

Audited: the 18 concrete public configuration structs and the caller/provider vtables and factories on
the public surface, using the F2-1a census plus per-type reads and a tree-wide usage sweep. For each
type the audited dimensions are: allocation and initialization owner; copied-by-value or by-pointer;
whether core reads the whole object or selected fields; whether zero-initialization has defined
semantics; whether it crosses an independently compiled integration boundary; every `sizeof`,
positional initializer, compound literal, and field-by-field copy in the tree; existing append-only or
"must stay last" promises; the current tail member and a viable extension point; freestanding
consequences; and the tests/examples/integrations that would migrate.

The 18 configuration structs (the F2-0 count reconciles as the 16 that match a digit-free name plus
`KlHttp2ClientConfig` and `KlHttp2ServerConfig`): `KlCompressConfig`, `KlDatagramConfig`,
`KlDatagramSocketConfig`, `KlDecompressConfig`, `KlDnsResolverConfig`, `KlHttpClientConfig`,
`KlHttpClientPoolConfig`, `KlHttpCorsConfig`, `KlHttpMultipartConfig`, `KlHttpProxyConfig`,
`KlHttpRedirectConfig`, `KlHttpServerConfig`, `KlResolverCacheConfig`, `KlThreadPoolConfig`,
`KlTlsConfig`, `KlWsClientConfig`, `KlHttp2ClientConfig`, `KlHttp2ServerConfig`. (`KlWsServerConfig` is
opaque: the caller does not construct its layout, so it is outside the caller-init concern.)

Tree-wide initialization evidence (the migration-cost basis):

- Config initialization is universally memset-zero or DESIGNATED (`.field = value`): 719 `memset(&x, 0,
  sizeof x)` sites across examples/tests/integrations, and every config initializer found uses either
  `= {0}` or `.field =` designated form. No purely-positional config initializer (values without field
  names) exists in the tree.
- Zero first-party `sizeof(Kl...Config)` sites: no code hardcodes a config's size for copying or
  allocation; `memset(&cfg, 0, sizeof cfg)` and by-value assignment both track the struct size at
  compile time.
- By-value storage exists (for example `KlHttpServer` embeds `KlHttpServerConfig config;` by value, and
  `KlAllocator` is copied by value throughout), but every such copy is `dst = src` over the
  compile-time size, so it grows automatically on recompile; there is no truncating fixed-size copy.

Because C zero-fills omitted trailing members of a brace initializer, both `= {0}` and designated
initializers remain correct when a field is APPENDED. The only thing that breaks a designated or
positional initializer is REORDERING or INSERTING a field, never appending one.

## 2. Per-type audit of the named vtables and factories

| type | header | who fills | passed | core reads | zero-init | crosses integ. boundary | tail / extension point | existing promise |
|---|---|---|---|---|---|---|---|---|
| KlSocketOps | socket.h | provider | by ptr (via KlSocketProvider) | selected fields; optional ops NULL-checked | defined (NULL = not supplied) | yes (posix/winsock/lwIP/uefi) | `name` after the op block | explicit "append-only: a zero/NULL slot means the op is not supplied" |
| KlEventOps | event.h | provider | by ptr | calls each op; some optional | defined | yes (lwIP) | `completion` reserved, "MUST stay the last member" | reserved-last-slot discipline |
| KlTls | tls.h | factory/adapter | by ptr (self) | required subset via `kl_tls_vtable_valid`; 8 optional ops NULL-checked | defined | yes (mbedtls/openssl/boringssl/libressl) | `at_eof` (append after) | required-subset validator |
| KlResolver | resolver.h | caller/adapter | by ptr (self) | all three ops | defined | yes (bring-your-own resolver) | `destroy` | documented sync-completion contract |
| KlDatagramOps | socket_dgram.h | provider | by ptr | selected fields | defined | yes | `send_batch` | provider seam |
| KlCompress / KlDecompress | compress.h / decompress.h | factory | by ptr (self) | all ops | defined | yes (miniz) | `destroy` | factory pattern |
| KlCompressFactory / KlDecompressFactory | compress.h / decompress.h | caller | by value (fn ptr) | invoked | n/a | yes | the typedef signature itself | signature is frozen |
| KlHttp2ClientSession | http2_client.h | user vtable + KEEL-managed | by ptr (self) | user ops read; `keel_cbs`/`keel_ctx` WRITTEN by core | defined | yes (nghttp2 adapter) | `keel_ctx` (void*), preceded by by-value `keel_cbs` | none |
| KlHttp2ServerSession | http2_server.h | user vtable + KEEL-managed | by ptr | as above | defined | yes (nghttp2) | KEEL-managed tail | none |
| KlAllocator | allocator.h | caller | by value (copied widely) | all three ops + ctx | defined | yes (every integration) | `ctx` | none (stable de facto) |
| the 18 configs | various | caller | by value or by ptr-then-copied | selected fields; 0 = default | defined (memset-zero idiom) | yes (server/client/datagram/tls configs built by integrations) | last field | `KlHttpServerConfig` etc. memset-zero initialized |

Notable specifics:

- `KlSocketOps` and `KlIoStatus` already carry an explicit append-only zero-default contract in their
  comments. `KlEventOps` already reserves a trailing `completion` slot marked "MUST stay the last
  member". `KlTls` already ships `kl_tls_vtable_valid` enforcing a required subset while the other 8
  ops are optional and NULL-checked. The codebase already practices the policy this record proposes to
  make explicit.
- `KlHttp2ClientSession` is the one awkward shape: a single struct that the user fills with 4 ops
  (`recv`/`submit_request`/`flush`/`destroy`) AND that core writes with a by-value
  `KlHttp2ClientCallbacks keel_cbs` plus `void *keel_ctx`. Growing the KEEL-managed `keel_cbs` (a
  by-value embedded struct) resizes `KlHttp2ClientSession`, so the session adapter must be recompiled.
  Under static relink that recompilation is exactly what the promise already requires, so this is not
  an extensibility blocker; it is only a blocker under binary compatibility, which Keel does not
  promise. It is a mild design smell (mixed ownership in one struct), not an ABI problem.
- `KlDatagramConfig` is the live cautionary case: `kl_datagram_init_ex` (datagram.h) added a FUNCTION
  PARAMETER (`send_byte_budget`) rather than a config field, on the belief that the config layout was
  frozen. Under the append-only policy below, that value should instead have been an APPENDED
  zero-default config field, avoiding parameter bloat. This is the concrete future-extension pattern
  the policy is meant to enable.

## 3. The four options, evaluated

- (a) Retain append-only, zero-default evolution. Fields and vtable slots are only ever appended;
  their zero/NULL value is the compatible default; callers zero-initialize. Cost: a documented
  discipline and occasional NULL-checks. Sufficient under source + static relink + W^X.
- (b) Add reserved tail capacity (padding/reserved slots) to pre-provision growth. Cost: guessing how
  much to reserve; dead fields; still requires recompile to USE a reserved slot as a typed field, so
  it buys nothing over (a) under static relink. Two purpose-specific reserved slots already exist
  (`KlEventOps.completion`, the datagram `dropped` counter); generalizing them is not warranted.
- (c) Add `struct_size`/`api_version` only to genuinely independent provider vtables. Cost: every
  provider must set it correctly and every core call-site must branch on it; it only detects
  old-binary-vs-new-binary skew, which W^X/no-dlopen precludes. No runtime scenario in Keel produces
  the skew it guards against.
- (d) Retrofit `struct_size`/`api_version` broadly as a deliberate v3 break. Cost: a churn across all
  18 configs, every vtable, and every consumer/integration/test, plus a permanent per-call-site
  version dance, to purchase binary compatibility Keel deliberately does not offer.

## 4. Decision

Adopt (a): append-only, zero-default evolution is the sole v3 extensibility policy for public configs
and vtables. Reject (b), (c), and (d). Under Keel's actual promise (source compatibility plus static
relink, reinforced by the W^X/no-dlopen posture that removes every runtime binary-plugin boundary), a
`struct_size`/`api_version` apparatus guards against a mismatch that cannot occur, while append-only
zero-default already delivers compatible growth and is substantially simpler. The codebase is already
most of the way there (KlSocketOps/KlIoStatus append-only contracts, KlEventOps reserved-last slot,
kl_tls_vtable_valid required-subset).

No v3 break is required for extensibility. Therefore there is no break to cost: every realistic 3.x
extension below is handled by appending, whose only migration cost is the recompile the promise
already mandates.

## 5. The policy (rules to be recorded in headers by a later, separately reviewed increment)

1. Append only. Public caller-constructed configs and vtables may only APPEND members. Never reorder,
   insert-in-the-middle, remove, rename in place, retype, or resize an existing member.
2. Zero is the default. Every appended field's zero/NULL value MUST be a valid, behavior-preserving
   default ("not supplied" / "use the built-in default"). If zero cannot mean that, the feature needs a
   new function or a new opt-in flag, not a silent field.
3. Callers zero-initialize. Consumers MUST zero-initialize configs and caller-built vtables
   (`= {0}`, a designated initializer, or `memset`). The tree already does this universally; partial
   designated or positional initializers stay correct because C zero-fills omitted trailing members.
4. Optional appended vtable ops are NULL-checked by core; required ops are enforced by a
   `*_vtable_valid` validator (as `KlTls` already does). A provider compiled against a newer header
   that leaves a new optional slot zero is treated as "not supplied".
5. Callback typedef signatures are frozen. Extend behavior by APPENDING a new vtable slot, never by
   changing an existing callback's or factory's signature.
6. Never change a member's type or size in place, and keep any slot marked "MUST stay last" last.

## 6. Concrete future-extension examples (append-only handles each; zero break)

- KlSocketOps: add `int (*recvmmsg)(void *ctx, ...)`. Append after `name`; core NULL-checks it and
  falls back to per-datagram recv. Providers that do not implement it leave it zero. Migration: none.
- KlEventOps: add a new op immediately before the reserved `completion` tail. Recompiled providers pick
  it up; core NULL-checks it. Migration: none.
- KlTls: add `int (*export_keying_material)(KlTls *self, ...)`. Append after `at_eof`; it is optional
  so `kl_tls_vtable_valid` is unchanged and existing adapters keep working. Migration: none.
- KlHttp2ClientSession: add a user op such as `int (*rst_stream)(KlHttp2ClientSession *self, int32_t)`.
  Append after `keel_ctx`; adapters recompile (already required). Growing the KEEL-managed `keel_cbs`
  is likewise safe under recompile. Migration: recompile the nghttp2 adapter, no source edit at call
  sites.
- KlCompress / KlDecompress: add `int (*set_level)(KlCompress *self, int level)`. Append optional op.
  Do NOT change `KlCompressFactory`'s signature; pass new construction parameters through
  `KlCompressCtx` or `KlCompressConfig` instead. Migration: none.
- KlResolver: add `KlResolveReq *(*resolve_srv)(...)` as an optional appended op. Migration: none.
- The 18 configs: append a zero-default field, for example `KlDatagramConfig.send_byte_budget`
  (retiring the `kl_datagram_init_ex` parameter-bloat workaround), or
  `KlHttpServerConfig.max_h2_concurrent_streams`. Every existing memset-zero or designated initializer
  keeps compiling with the new field defaulting to zero. Migration: none.

## 7. Proposed breaks and their migration cost

None. The decision introduces no v3 break. For the record, the two shapes that would otherwise tempt a
break are handled without one:

- KlHttp2ClientSession mixed ownership: leave as-is. A break to split user ops from KEEL-managed state
  would force every HTTP/2 session adapter (currently the nghttp2 integration) to be rewritten, for no
  gain under static relink. If cleanliness is ever pursued it belongs in F2-B's layout review, not
  here, and only as an explicitly costed break.
- KlSocketOps trailing `name`: leave as-is. New ops append after `name`; the append-only contract
  already documents that a zero slot means "not supplied", so the position of `name` is immaterial
  under recompile.

## 8. Additive, non-breaking follow-ups (deferred; NOT part of this docs commit)

These are compatible robustness/clarity additions, to be proposed and reviewed as a SEPARATE F2-C
implementation commit only after this decision is accepted (no header is touched now):

- Add `*_vtable_valid` required-subset validators mirroring `kl_tls_vtable_valid` to the other
  core-called vtables that lack one (KlEventOps, KlSocketOps, KlResolver, KlDatagramOps,
  KlHttpBodyReader, KlFileIO, the parser vtables, the HTTP/2 sessions), so a mis-filled provider fails
  loudly rather than via a NULL call.
- Add a one-line "append-only; callers zero-initialize; a zero/NULL field means default/not-supplied"
  contract comment to each config and caller-built vtable header, generalizing the wording KlSocketOps
  already carries.

Both are additive (new inline function, new comments) and change no existing member, so they are
compatible under the very policy they document.

## 9. Consequences for consumers, tests, examples, integrations, and freestanding

None from this decision. Because the policy forbids reordering and mandates append-only zero-default,
existing designated and memset-zero initializers, by-value copies, and provider vtables remain correct
as the surface grows. Freestanding builds (UEFI, lwIP) construct the same configs and provider vtables
and are recompiled under static relink like every other consumer, so appended zero-default fields and
optional NULL-checked ops reach them with no special handling. The freestanding stock resolver's
`kl_datagram_init_ex` workaround becomes the first candidate to simplify under the policy, but that is
a later, separately reviewed change.

## 10. Validation

Docs-only. `git diff --check`; `make check-doc-refs`, `make check-old-layout`, `make
check-no-milestones`, `make check-no-em-dash`; pure ASCII; no build, header, test, or integration file
changed. Nothing pushed; no remote CI. Header edits (the additive follow-ups in section 8) are held for
a separate reviewed increment.
