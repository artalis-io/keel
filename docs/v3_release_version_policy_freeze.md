# Keel v3 release-readiness: version-policy audit and design freeze (R3-0)

Status: PROPOSED (docs-only; nothing implemented). This is the frozen policy for how Keel 3.0.0 is
versioned, what "3.x compatibility" promises, and the verified breaking-change surface. It changes no
version macro, no build file, no workflow, and no tag. Later increments (R3-1..R3-6) implement it, each
separately reviewed and authorized.

Audited base commit: 88d5b3be65e4fbb43d3babda205c6ba7745569fc (origin/main, merge of PR #254).
Audit date: 2026-08-29.
Last released 2.x baseline: tag v2.9.0 (2026-07-18). Commits on main since v2.9.0: 682.

Note on the current state: the version macros on main still read 2.9.0, identical to the v2.9.0 tag,
while main carries 682 commits of work past that tag (the transport/datagram axis, the Windows/UEFI/lwIP
providers, the S4/C-series structural work, and the F2 public-surface hardening). The version is
therefore stale relative to main's content, and 3.0.0 captures the whole accumulated delta.

## 1. Version-source inventory

Machine-readable authorities (each hardcodes the value independently; all must change for 3.0.0):

| Source | Line(s) | Current value | Role | Change for 3.0.0 |
|--------|---------|---------------|------|------------------|
| include/keel/keel.h | 42, 44, 46 | MAJOR 2, MINOR 9, PATCH 0 | Primary numeric macros | Yes |
| include/keel/keel.h | 48 | KL_VERSION_STRING "2.9.0" | String literal, NOT derived from the numeric macros | Yes |
| include/keel/freestanding.h | 39-42 | MAJOR 2, MINOR 9, PATCH 0, STRING "2.9.0" | Duplicate of the keel.h block, guarded by `#ifndef KL_VERSION_STRING` (line 38) | Yes |
| keel.pc.in | 7 | Version: 2.9.0 | pkg-config version; the Makefile recipe substitutes only `@PREFIX@`, so this literal passes through verbatim | Yes |
| sbom.cdx.json | 9 | "version": "1.0.0" | CycloneDX component version for keel; STALE and inconsistent with the headers (already drifted); hand-maintained, no generator | Yes (and correct the drift) |
| docs/contracts/compatibility.md | 26 | prose "currently 2.9.0" | Human-facing reference | Yes (update) |

Derived / not an independent authority (auto-follow the keel.h macros; must NOT be hand-edited):

- KL_VERSION_NUMBER: keel.h line 53 and freestanding.h line 44, computed as MAJOR*10000 + MINOR*100 + PATCH.
- kl_version() / kl_version_number(): src/version.c, return KL_VERSION_STRING / KL_VERSION_NUMBER from keel.h.
- tests/test_version.c: asserts kl_version() == KL_VERSION_STRING and NUMBER == MAJOR*10000 + MINOR*100 + PATCH. It does NOT assert that KL_VERSION_STRING matches the numeric macros, nor that freestanding.h, keel.pc.in, or sbom.cdx.json agree with keel.h. There is no cross-source drift check anywhere in the tree today.

Desired single source of truth (decision D3): the three numeric macros KL_VERSION_MAJOR/MINOR/PATCH in
include/keel/keel.h. Every other value becomes derived or checked:

- KL_VERSION_STRING should be produced from the numeric macros by a stringize helper, not written as a
  separate literal, so the string cannot drift from the numbers.
- freestanding.h should not carry an independent copy; it should derive from, or be checked equal to,
  the keel.h block.
- keel.pc.in Version should be generated from the macros (or checked against them).
- sbom.cdx.json component version should be regenerated (or checked) from the macros.
- The compatibility.md prose value should be checked (or generated) so it cannot go stale.

R3-1 introduces a drift gate that parses the keel.h macros and fails if any other source disagrees.

## 2. Release-mechanism inventory

- Tags: 26 tags, v2.1.0 through v2.9.0, format `vMAJOR.MINOR.PATCH` (leading `v`). Created manually; no
  workflow emits them.
- Workflows present: `.github/workflows/{ci,codeql,benchmark,deploy-site,docs,scorecard}.yml`. NONE
  produce a release: no archive, no checksum, no package, no SBOM regeneration, no GitHub Release. The
  only version-adjacent automation is CI (build/test/gates) and OpenSSF Scorecard.
- Archives / checksums / packages: absent (no automation, no manual scripts in-tree).
- SBOM: sbom.cdx.json is hand-maintained and stale (1.0.0); no target or workflow regenerates it.
- Version provenance: builds derive the version from COMPILED CONSTANTS (the keel.h macros), never from
  Git. A checkout at any tag reports whatever the macros say, so a tag and the embedded version can
  disagree (today every commit since v2.9.0 reports 2.9.0). There is no build-time tag-to-macro check.
- Tag naming for prereleases: none exists yet; to be decided (D2).

This increment does not modify the mechanism. R3-4 defines artifact/package production and the
tag-to-embedded-version proof.

## 3. Compatibility policy (reconciled with docs/contracts/compatibility.md)

Keel 3.x promises:

- Keel is a C11 library. Its public C API has C linkage (extern "C") and is usable from C++11
  consumers. No C++ wrapper API is promised; C++ is a consumption guarantee, not a second surface.
- Within a major version: SOURCE compatibility plus RECOMPILE / static-relink. Code that compiles
  against 3.y.z compiles against any later 3.y'.z' without edits; consumers rebuild their objects and
  re-link libkeel.a. Keel ships no versioned shared object.
- NOT promised within a major version: cross-version binary ABI, stable struct sizes, a stable shared
  object / soname, or stable opt-in `*_detail.h` layouts.
- Public configs and provider/vtable types evolve append-only with zero-default optional tails; callers
  zero-initialize and recompile (see docs/contracts/compatibility.md and
  docs/archive/f2/f2_c_extensibility_decision.md).
- Callback and factory signatures are frozen within the major version; behavior is extended by
  appending a vtable slot, never by retyping an existing callback.
- Opt-in `*_detail.h` layouts (for example stream_detail.h, datagram_detail.h, listener_detail.h,
  connect_op_detail.h) are explicitly NOT ABI-stable and may change between releases; embedders that
  opt in recompile.
- No dlopen, runtime plugin discovery, executable memory, or global mutable backend registration.

"3.x compatibility" therefore MEANS: recompile-and-relink against a newer 3.y.z with no source edits,
for consumers that use the public API through its documented functions and accessors and zero-initialize
caller-owned structs. It DOES NOT MEAN: swapping a prebuilt binary, relying on struct sizes or field
offsets, reaching into `*_detail.h` layouts, or a stable C++ (name-mangled) ABI.

## 4. Verified breaking-change ledger (v2.9.0 -> main)

Each item was verified against Git history and the live public surface, not inferred from summaries.
The public header count went from 40 (v2.9.0) to 55 (main). The dominant break is a wholesale public
taxonomy rename; the remaining items apply within the renamed surface.

BLK-1  Public-header taxonomy rename (install-path break). Severity: MAJOR.
  Evidence: `git ls-tree -r --name-only v2.9.0 include/keel` vs main. Representative renames:
  server.h -> http_server.h; client.h -> http_client.h; connection.h -> http_connection.h;
  router.h -> http_router.h; request.h -> http_request.h; response.h -> http_response.h;
  cors.h -> http_cors.h; sse.h -> http_sse.h; body_reader.h -> http_body_reader.h;
  body_reader_multipart.h -> http_body_reader_multipart.h; chunked.h -> http1_chunked.h;
  parser.h -> http1_parser.h; h2.h -> http2.h; h2_client.h -> http2_client.h;
  h2_server.h -> http2_server.h; client_pool.h -> http_client_pool.h; redirect.h -> http_redirect.h.
  Migration: update every `#include <keel/...>` path. Completeness is enforced going forward by the
  check-no-httplegacy gate (filenames arm).

BLK-2  Public type / symbol taxonomy rename. Severity: MAJOR.
  Evidence: v2.9.0 include/keel/server.h lines 107 and 128 define `typedef struct KlServer { ... }
  KlServer;`; on main the equivalent is KlHttpServer in include/keel/http_server.h. The same pattern
  covers the client, HTTP/2 (KlH2* -> KlHttp2*), and related families. Migration: rename the types,
  constants, and functions to the http_/http2_ taxonomy. Completeness enforced by check-no-httplegacy
  (types/constants/functions arm).

BLK-3  Integration headers removed from the installed public surface. Severity: MAJOR.
  Evidence: removed from include/keel on main: tls_mbedtls.h, compress_miniz.h, decompress_miniz.h
  (now under integrations/). Migration: include the adapter from its integration location and link the
  chosen backend; core no longer installs backend-specific headers.

BLK-4  KlEventLoop.fd removed; event API retyped to KlSocketHandle. Severity: MAJOR.
  Evidence: v2.9.0 include/keel/event.h line 17 has `int fd; /* epoll_fd or kqueue_fd */` in
  KlEventLoop, and kl_event_add took `int fd`. On main the descriptor is backend-owned (no KlEventLoop.fd)
  and the event ops take `KlSocketHandle fd` (include/keel/event.h lines 59-61). Migration: stop reading
  a loop fd; use KlSocketHandle throughout. Enforced by the check-no-eventloop-fd gate.

BLK-5  F2 opacity: types made opaque, direct field access replaced by accessors. Severity: MAJOR.
  Evidence: on main KlHttpConn (http_connection.h), KlWatcher and KlTimerEntry (event_ctx.h),
  KlHttpClientPoolEntry (http_client_pool.h), KlHttpMiddlewareEntry (http_router.h), and KlWsServerConn
  (websocket_server.h) are forward-declared with their layout moved to private src/ or opt-in detail
  headers. Migration: use the accessors (for example kl_http_conn_peer_addr / kl_http_conn_response,
  and the event-context / bound-port accessors added in F2-B) instead of reaching into fields.

BLK-6  Transport axis added (additive; not a break, listed for release notes). Severity: MINOR (new API).
  Evidence: new on main and absent from v2.9.0: datagram.h, datagram_batch.h, datagram_detail.h,
  stream.h, stream_detail.h, listener.h, listener_detail.h, socket.h, socket_dgram.h, sockaddr.h,
  handle.h, net.h, clock.h, connect_op.h, connect_op_detail.h, dns_resolver.h, freestanding.h. The
  KlUdp -> KlDatagram rename happened entirely after v2.9.0 (v2.9.0 shipped no udp/datagram public
  header), so it is not a v2.9.0-relative break; the check-no-kludp gate guards against the retired
  KlUdp object API reappearing.

BLK-7  Provider / backend contract surface. Severity: MIXED (mostly additive; BLK-4 is the break).
  Evidence: the socket-provider vtable (socket.h), the address-ABI-neutral KlSockAddr surface
  (sockaddr.h), and the runtime-injectable completion axis are new or reshaped since v2.9.0. Selection
  moved to build-time BACKEND= plus caller-passed providers. Net effect on the v2.9.0 consumer is
  captured by BLK-1/BLK-2/BLK-4; the rest is new surface.

BLK-8  Supported platform / toolchain expansion. Severity: MINOR (new capability + a new guarantee).
  Evidence: main adds Windows (Winsock, WSAPoll, IOCP, MinGW), UEFI (EFI_TCP4/UDP4), lwIP (BSD and raw
  NO_SYS), and Cosmopolitan, alongside the existing Linux/macOS backends. F2 additionally makes every
  installed header compile standalone under C11 and C++11 with strict flags (a new, gated guarantee).

R3-2 turns this ledger into a per-symbol changelog and migration guide; the enumerated gates
(check-no-httplegacy, check-no-eventloop-fd, check-no-kludp) are the machine proof that the renames and
removals are complete and cannot regress.

## 5. Version-policy decisions requiring review

D1  Exact next version. Recommendation: cut a release-candidate sequence rather than jumping straight to
    a final tag, because the breaking surface is large (BLK-1..BLK-5) and main has 682 unreleased
    commits. Proposed: 3.0.0-rc.1, iterate to rc.N, then promote to 3.0.0.

D2  Tag format. Recommendation: keep `vMAJOR.MINOR.PATCH`; prereleases use the SemVer prerelease suffix,
    `vMAJOR.MINOR.PATCH-rc.N` (for example v3.0.0-rc.1). SemVer precedence: 3.0.0-rc.1 < 3.0.0-rc.2 <
    3.0.0.

D3  Single authoritative version source. Recommendation: KL_VERSION_MAJOR/MINOR/PATCH in
    include/keel/keel.h. All other values (section 1) become derived or checked.

D4  Generated / check-only derived locations. Recommendation, enforced by the R3-1 drift gate:
    KL_VERSION_STRING derived from the numeric macros; freestanding.h checked equal to keel.h; keel.pc
    Version generated or checked; sbom.cdx.json component version regenerated or checked;
    compatibility.md prose checked. Open sub-question: KL_VERSION_NUMBER is purely numeric and cannot
    encode a `-rc.N` prerelease; recommendation is that KL_VERSION_STRING carries the full SemVer
    (including any prerelease) while KL_VERSION_NUMBER stays the numeric 30000 form.

D5  Prerelease ordering and promotion. Recommendation: SemVer precedence for ordering; promotion is
    dropping the `-rc.N` suffix when the RC is accepted unchanged. Proposed policy: after rc.1, only
    bug fixes and doc/release-metadata changes; any new feature or API change restarts at a new rc.

D6  2.x maintenance after 3.0.0. Decision needed. Options: (a) a fixed critical-fix-only window on the
    v2.9.x line for a stated number of months, then EOL; or (b) immediate EOL of 2.x at 3.0.0. This is a
    project/support-capacity call, not a technical one.

D7  soname / package-name change. Recommendation: none required. Keel ships no shared object, so there
    is no soname to bump and no parallel-install concern. The pkg-config module stays `keel` (no `keel3`
    split); the major bump lives in the pkg-config Version field. Confirm no downstream packaging assumes
    a soname or a versioned module name before finalizing.

D8  Artifact-version-proves-tag. Recommendation: because the version is compiled from constants, add a
    release-time check (R3-4/R3-5) asserting the tag equals `v` + KL_VERSION_STRING (prerelease suffix
    included), on top of the R3-1 cross-source drift gate. An artifact whose embedded kl_version() does
    not match its tag fails the release.

## 6. Proposed implementation sequence (later increments)

- R3-0  This policy freeze (docs-only). [current]
- R3-1  Single-source version implementation: derive KL_VERSION_STRING; de-duplicate freestanding.h;
        generate/check keel.pc and sbom; add a drift gate that fails on any cross-source disagreement.
- R3-2  Changelog and migration guide: the per-symbol v2.9.0 -> 3.0.0 mapping built on section 4.
- R3-3  Compatibility / platform statement: publish the section 3 promise and the supported
        platform/toolchain/backend matrix as a first-class document.
- R3-4  Release artifact and package verification: define archive/checksum/SBOM production and the
        tag-to-embedded-version proof (D8); no publication.
- R3-5  Release-candidate matrix: full CI + sanitizers + the gate set on the RC; artifact checks green.
- R3-6  Separately authorized version bump, tag, and publish (never bundled with R3-0..R3-5).

## 7. Rollback / recovery notes for an incorrect prerelease or tag

- Treat a pushed tag as immutable once it may have been consumed. Prefer rolling FORWARD (rc.2) over
  moving or reusing a tag; never reuse a version number for different content.
- If a bad tag was pushed but is provably unconsumed (no downloads, no dependents), it may be deleted:
  `git push --delete origin vX.Y.Z[-rc.N]` and delete the matching GitHub release (mark RCs as
  prerelease so they are clearly non-final and safe to remove). Then regenerate SBOM/checksums for the
  corrected version.
- If macros were bumped but not yet tagged or published, simply revert the macro change; nothing external
  has observed it.
- Because a build embeds the compiled version, a mismatched artifact is caught by the D8 tag-to-embedded
  check before publication; recovery is to rebuild at the corrected version, not to re-tag the old build.

## 8. Validation plan

For R3-0 (this increment): git diff --check; make check-doc-refs; make check-no-em-dash;
make check-no-milestones; confirm every cited path and symbol exists; confirm only this document
changed; confirm a clean working tree after the commit.

For later increments: R3-1 drift gate green across every source in section 1 (fail-on-drift proven by a
deliberate mismatch in the gate's self-test); a build at the candidate version whose kl_version() equals
the tag; pkg-config reporting the new version to an out-of-tree consumer; and the full CI matrix plus
sanitizers plus the standing gate set green on the release candidate (R3-5).
