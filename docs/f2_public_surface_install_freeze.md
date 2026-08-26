# F2 public-surface and staged-install freeze

Status: FROZEN for review (F2-0, revised). Docs-only. No header, Makefile, install rule, example,
test, or workflow changed by this document. Grounded in the live tree at base commit `4e667e1` on
branch `main`, not in the roadmap's assumptions; the mechanical claims (standalone compile, staged
install, DESTDIR staging, out-of-tree consumer) were reproduced locally.

Goal: make Keel's v3 public and installed surface intentional, minimal, internally consistent,
test-covered, and usable exclusively through installed headers and pkg-config. This is a
release-hardening phase. It does not redesign the architecture, move source directories, add
features, or perform unrelated cleanup. The flat public-header layout under `include/keel/` is
preserved unless a header is proven to not be public at all.

This revision corrects four conclusions from the first draft that the review found unsupported:
C++ support was overstated (linkage, not just syntax); the layout/public classification contained
factual errors (KlHttpServer is not opaque; dropping the umbrella include does not isolate
`http_connection.h`); coverage was overstated (caller candidates are not verified coverage); and the
install audit conflated PREFIX with DESTDIR. The blanket "zero accidentally internal" conclusion is
withdrawn pending a symbol-level census (see 1b).

The increment order (section 9) is sequenced so that no decision is made before the inventory that is
its evidence base exists: the inventories and classification scaffolding are checked in first (F2-1a),
then the decision increments run in dependency order (F2-A, F2-C, F2-D, then F2-B, which consumes the
complete census and the accepted F2-C/F2-D policies), and only then are the mechanical gates and edits
applied (F2-1b, F2-2..F2-6).

## 0. Method and scope

- Base: `main` at `4e667e1`.
- Header set: `git ls-files 'include/keel/*.h'` yields 55 tracked headers.
- Mechanical experiments reproduced (`keel.pc` is gitignored, so regenerating it does not dirty the
  tree; staged prefixes live under `mktemp -d` and are removed):
  - C11 standalone syntax compile of every installed header as first-and-only include.
  - C++ syntax compile of every header, plus a C++ linkage assessment (see section 3); a syntax pass
    does not imply a link pass.
  - `make install PREFIX=<stage>` (custom logical prefix) and `make install DESTDIR=<stage>
    PREFIX=/usr` (packaging staging root); both manifests captured.
  - `pkg-config` resolution and an out-of-tree C consumer compile+link+run from `/tmp`.
- Struct/coverage classification was checked at the declaration level against real code, not filenames
  or grep hits, and every claim below cites the evidence that supports it. Where a census tool was
  used its limits are stated so the numbers are not read as more complete than they are.

## 1. Authoritative header inventory (55 headers)

Roles: U = Umbrella; SC = Public substrate contract; PC = Public built-in protocol contract;
OPT = Optional public extension/detail intentionally installed (opt-in). No header currently
classifies as shim or generated. Whether any header is "implementation accidentally installed" is an
open question resolved by the symbol-level census in 1b, not asserted here.

"umb" = reachable from `include/keel/keel.h`. "layout" = publishes a concrete struct layout.

| header | role | axis | umb | layout | recommendation |
|---|---|---|---|---|---|
| keel.h | U | umbrella | self | no | retain as-is |
| freestanding.h | U | umbrella | sibling umbrella | no | retain as-is |
| allocator.h | SC | substrate | yes | KlAllocator (caller-set vtable) | retain as-is |
| async.h | SC | substrate | yes | no | retain as-is |
| clock.h | SC | substrate | yes | no | retain as-is |
| error.h | SC | substrate | yes | enum | retain as-is |
| drain.h | SC | substrate | yes | KlDrain (caller-inspectable) | retain as-is |
| file_io.h | SC | substrate | yes | KlFileIOResult | retain as-is |
| thread_pool.h | SC | substrate | yes | config/workitem | retain as-is |
| timer.h | SC | substrate | yes | no | retain as-is |
| url.h | SC | substrate | yes | KlUrl (caller-inspectable) | retain as-is |
| handle.h | SC | socket/transport | yes | no | retain as-is |
| sockaddr.h | SC | socket/transport | yes (via socket.h) | KlSockAddr (opaque-by-convention) | retain as-is |
| socket.h | SC | socket/transport | yes | vtable+KlIoVec; has extern "C" | retain as-is |
| socket_dgram.h | SC | socket/transport | yes (via socket.h) | vtable+rx/tx descriptors | retain as-is |
| net.h | SC (platform include boundary) | socket/transport | yes (transitive) | no | retain as-is |
| event.h | SC | event | yes | KlEventLoop (concrete; KlEventLoop.fd) | retain; layout is a v3 decision (7.1/7.2) |
| event_ctx.h | SC | event | yes | KlEventCtx, KlWatcher (concrete runtime state) | retain; layout is a v3 decision (7.2) |
| stream.h | SC | socket/transport | yes | opaque handle + close/status enums | retain as-is |
| listener.h | SC | socket/transport | yes | opaque handle + hooks/state | retain as-is |
| connect_op.h | SC | socket/transport | yes | opaque handle + hooks/state | retain as-is |
| stream_detail.h | OPT | socket/transport | no | KlStream (opt-in unstable) | retain as-is |
| listener_detail.h | OPT | socket/transport | no | KlListener (opt-in unstable) | retain as-is |
| connect_op_detail.h | OPT | socket/transport | no | KlConnectOp (opt-in unstable) | retain as-is |
| datagram.h | PC | datagram | no | config/message; opaque core; has extern "C" | retain; add to umbrella (7.8) |
| datagram_batch.h | OPT | datagram | no | opaque; has extern "C" | retain; add to umbrella with datagram.h (7.8) |
| datagram_detail.h | OPT | datagram | no | KlDatagram (opt-in unstable); has extern "C" | retain as-is |
| resolver.h | SC | dns | yes | vtable + KlResolveResult | retain as-is |
| resolver_cache.h | PC | dns | yes | config | retain as-is |
| dns_resolver.h | PC | dns | yes | config | retain as-is |
| tls.h | PC | protocol | yes | vtable + KlPeerCert/config | retain as-is |
| compress.h | PC | protocol | yes | config | retain as-is |
| decompress.h | PC | protocol | yes | config + stream | retain as-is |
| http_compress.h | PC | protocol/http | yes | KlHttpCompressStream | retain as-is |
| proxy_protocol.h | PC | protocol/http | yes (via http_server.h) | KlCidr/KlProxyResult | retain as-is |
| http1_parser.h | PC | protocol/http | yes | vtable | retain as-is |
| http1_chunked.h | PC | protocol/http | yes | decoder state | retain as-is |
| http_request.h | PC | protocol/http | yes | KlHttpParam (documented zero-alloc) | retain as-is |
| http_response.h | PC | protocol/http | yes | KlHttpResponse (caller-inspectable) | retain as-is |
| http_body_reader.h | PC | protocol/http | yes | vtable + buf reader | retain as-is |
| http_body_reader_multipart.h | PC | protocol/http | yes | config/event/meta | retain as-is |
| http_router.h | PC | protocol/http | yes | KlHttpRouter/Route/MiddlewareEntry | retain as-is |
| http_cors.h | PC | protocol/http | yes | config | retain as-is |
| http_sse.h | PC | protocol/http | yes | KlHttpSse | retain as-is |
| http_redirect.h | PC | protocol/http | yes | config | retain as-is |
| http_client.h | PC | protocol/http | yes | config/response/headers | retain as-is |
| http_client_pool.h | PC | protocol/http | yes | config/entry/conn (native fd in API) | retain as-is |
| http_connection.h | PC | protocol/http | yes | KlHttpConn, KlHttpConnPool (concrete) | see 7.3; umbrella removal alone does not isolate it |
| http_server.h | PC | protocol/http | yes | KlHttpServer full layout (47 lines) + config/stats/transport | see 7.2/7.3; layout is a v3 decision |
| http2.h | PC | protocol/http2 | yes | constants | retain as-is |
| http2_server.h | PC | protocol/http2 | yes | vtable | retain as-is |
| http2_client.h | PC | protocol/http2 | yes | vtable (by-value tail; see 7.4) | retain as-is |
| websocket.h | PC | protocol/websocket | yes | frame parser state | retain as-is |
| websocket_server.h | PC | protocol/websocket | yes | callbacks | retain as-is |
| websocket_client.h | PC | protocol/websocket | yes | config/callbacks | retain as-is |

Summary (counts only, not a safety conclusion): 2 umbrellas, 20 substrate contracts, 29 protocol
contracts, 4 opt-in detail/extension headers. The judgment of whether any published layout is an
implementation seam that should not be installed is deferred to 1b and decision F2-B; it is not
asserted here.

## 1b. Symbol-level type classification (census done; decision withdrawn to F2-B)

The complete public type surface is enumerated by `tools/f2_public_inventory.sh` into
`docs/f2/public_types.tsv` (F2-1a): 193 public types across five kinds (89 struct, 25 enum, 16 opaque,
60 callback, 3 alias; no unions). The alias count includes the one intentionally-public lowercase Keel
typedef, `kl_ssize_t` (handle.h); type extraction accepts the `Kl*` convention plus that `kl_*` typedef
form, and requires a `typedef` keyword so struct fields and lowercase implementation identifiers are
never collected. This supersedes a naive `grep '^} Kl...;'` closer count, which finds
only 87 and is itself incomplete in two ways the review flagged: it misses tag-form bodies
(`struct KlName { ... };`, for example `KlHttpRequest` and the public vtables `KlTls`/`KlResolver`/
`KlEventOps`, whose layout is on the caller surface), and it misses every opaque forward-declared
handle (`KlDatagram`, `KlStream`, `KlListener`, `KlHttpClient`, ...), so the `opaque` category could
never be assigned from it and F2-B would have reviewed only concrete closers rather than the whole type
surface. Callback typedefs (60) are tagged distinctly so F2-B never mistakes an API signature for an
object layout. The first draft's "zero accidentally internal" claim is withdrawn: it was asserted per
file, but the correct unit is the individual declaration; the per-declaration decision itself is F2-B.

Every public type is classified (in `docs/f2/type_classification.tsv`, all rows `UNRESOLVED` until
F2-B) into exactly one of:

- caller-constructed: the caller allocates and fills it (configs, `KlIoVec`, callback/hook structs,
  vtables such as `KlSocketOps`, `KlDatagramOps`, `KlTls`);
- caller-inspectable: the library fills it and the caller reads defined fields (`KlUrl`,
  `KlHttpServerStats`, `KlProxyResult`, `KlResolveResult`, `KlPeerCert`, `KlHttpClientResponse`);
- opaque: handle-only in public headers, layout in a `*_detail.h` or private (the
  `KlStream`/`KlListener`/`KlConnectOp`/`KlDatagram` handles);
- opt-in unstable layout: layout deliberately behind a `*_detail.h` the umbrella excludes
  (`stream_detail.h`, `listener_detail.h`, `connect_op_detail.h`, `datagram_detail.h`);
- implementation layout accidentally installed: runtime-state structs exported through `include/`
  only because protocol/substrate TUs consume them, with no caller contract;
- unresolved v3 decision: the classification itself is the decision (KlEventLoop, KlEventCtx,
  KlHttpServer, KlHttpConn/Pool).

Verified contested cases (evidence, not filename inference):

- `KlHttpServer` (http_server.h:138-184) publishes its full 47-line runtime layout; it is NOT opaque.
  It embeds `KlHttpConnPool pool` by value (http_server.h:145) and `#include <keel/http_connection.h>`
  (http_server.h:13). Candidate: unresolved v3 decision (implementation layout on the caller surface).
- `KlHttpConn`/`KlHttpConnPool` (http_connection.h) are concrete runtime state. The declared
  functions are dominated by implementation seams consumed by protocol TUs, not a caller API:
  `kl_http_conn_pool_init/reserve/return_credit/acquire/release/pool_free`,
  `kl_http_conn_on_handshake/on_readable/on_writable/on_file_complete`,
  `kl_http_conn_read_proxy_header/ingest_proxy`. The one plain caller accessor is
  `kl_http_conn_peer_addr` (http_connection.h:214). Candidate: implementation layout accidentally
  installed, or unresolved v3 decision.
- `KlEventLoop` (event.h:22-31) and `KlEventCtx`/`KlWatcher` (event_ctx.h) are concrete runtime state
  embedded and stack-allocated by consumers. Candidate: unresolved v3 decision (see 7.1, 7.2).

Consequence: "a first-party source consumer exists" does not make a header a genuine caller API,
because a protocol or substrate TU under `src/` is exactly the consumer an implementation seam would
have. The presence-of-caller signal is retained only as a coverage candidate (section 5), not as a
public-API justification. The full per-declaration census is an F2-1a deliverable that feeds the F2-B
decision; until it exists, no header is affirmed as free of accidental exposure.

## 2. Umbrella audit (keel.h)

`keel.h` re-exports 43 headers, defines the hosted version macros, and deliberately omits the four
`*_detail.h` layout headers (`keel.h:106-112`). That opt-in mechanism is the target pattern.

Reachability of the ten headers not listed directly in `keel.h`:

- Transitively reachable (fine): `sockaddr.h` (via `socket.h`), `socket_dgram.h` (via `socket.h`),
  `proxy_protocol.h` (via `http_server.h`), `net.h` (via the http headers).
- Correctly opt-in and out (fine): `stream_detail.h`, `listener_detail.h`, `connect_op_detail.h`,
  `datagram_detail.h`.
- Not reachable at all (the notable gap): `datagram.h` and `datagram_batch.h`; the 32 `kl_datagram_*`
  facade functions are unreachable from a `#include <keel/keel.h>` consumer even though the datagram
  provider seam (`socket_dgram.h`) and address currency (`sockaddr.h`) are pulled in. See 7.8.

Correction to the first draft: removing the direct `http_connection.h` include from `keel.h` does NOT
remove `http_connection.h` from the umbrella closure, because `http_server.h` (in the umbrella)
includes it and embeds `KlHttpConnPool` by value. There is no include-graph edit that isolates the
connection layout while the server publishes its full layout and embeds the pool by value. Real
isolation requires the server/connection layout relationship to change (make `KlHttpServer` and/or the
pool opaque), which is decision F2-B, not a mechanical umbrella edit.

Other umbrella findings: version macros duplicated across `keel.h:42-53` and `freestanding.h:38-44`
(and a third copy of the version string in `keel.pc.in:7`), see 7.6; the umbrella `@brief` frames the
library as HTTP-only (`keel.h:2-3`), see 7.7.

Intended v3 umbrella policy (frozen intent; the layout-isolation parts depend on F2-B):

- Keep the `*_detail.h` layout headers and integration headers out of the umbrella.
- Add `datagram.h` and `datagram_batch.h` to the umbrella if decision 7.8 is accepted (additive).
- Single-source the version macros (7.6).
- Do not claim connection/server layout isolation via umbrella edits; that is F2-B.

## 3. Header self-containment and language support

C result: all 55 installed headers compile as first-and-only include under `-std=c11 -fsyntax-only`
against the staged include dir alone (no `vendor`/`src`), 0 failures. This baseline is clean.

C++ result and correction: a C++ `-fsyntax-only` pass also succeeds for all 55, but that does NOT mean
C++ consumption works. Only 4 of 55 headers wrap their declarations in `extern "C"`: `datagram.h`,
`datagram_batch.h`, `datagram_detail.h`, and `socket.h`. The other 51 (including `keel.h`, all http/
websocket/tls/event/stream headers) declare their `kl_*` functions with C++ language linkage when
included from C++. `libkeel.a` is compiled as C (C symbol linkage), so a C++ translation unit that
includes those headers and calls their functions can fail at link time with unresolved or mismatched
symbols even though every header compiled. The first draft's "no C++ linkage problems" statement was
therefore unsupported and is withdrawn.

C++ is thus an explicit v3 decision (F2-A), with two admissible outcomes:

- Declare C++ consumption unsupported for v3: document it, and the `check-public-headers` gate checks
  C11 self-containment only (C++ syntax may be a non-blocking advisory, not a contract).
- Support C++ consumption: add `extern "C"` guards to every public header as its own reviewed API
  increment, and extend the gate with a staged C++ compile-AND-LINK consumer (include headers, call
  functions, link against the installed `libkeel.a`), not a syntax-only check.

Recommended F2-A ruling: support C++ linkage consistently, keeping Keel C-first. The four already-
guarded headers make the current surface inconsistent; adding the guards is additive for C++ consumers
and has no effect on C consumers or the C ABI; and without them a C++ compile can succeed while the
link against the C-built `libkeel.a` fails. The v3 contract statement is: "Keel is a C11 library whose
public C API can be included and linked from C++." It does not promise an idiomatic C++ wrapper API.
Implementation requirements when F2-A ratifies this:

- Apply the standard guard to every installed header that declares functions or external objects,
  including both umbrellas (`keel.h`, `freestanding.h`):
  `#ifdef __cplusplus` / `extern "C" {` / declarations / `#ifdef __cplusplus` / `}`.
- Keep platform and system `#include`s outside the linkage block where practical.
- Ensure every opened block closes on every preprocessor path (guarded includes, conditional
  sections), verified both as a standalone include and through umbrella inclusion.
- Add a staged C++ consumer that calls real library functions and links and runs against the installed
  `libkeel.a`; syntax-only compilation is insufficient. Cover representative substrate, HTTP,
  datagram, and freestanding declarations.
- Add a negative canary: a deliberately unguarded C declaration must fail to link from C++, proving
  the guard is what makes linkage work.

Until F2-A ratifies this, no header is edited and the freeze makes no C++ support promise.

Future gate (design; implemented in F2-1b per the F2-A outcome): `check-public-headers` compiles every
header on the approved public-install manifest, individually, as first-and-only include, under
`-std=c11 -Werror`, using only the source `include/` path (no `vendor`/`src`/integration paths). It is
default-deny: an unmanifested header in `include/keel/` fails; a manifested header that is missing or
fails to compile fails, with a `file:line` diagnostic. It is self-canaried (a header missing a needed
include must fail; a trivially-correct one must pass). Its C++ obligation is exactly what F2-A sets:
either none, or a compile-and-link consumer.

## 4. Installation and pkg-config

Current mechanism (`Makefile:793-810`, `keel.pc.in`): `install` copies `$(LIB)` to `$(PREFIX)/lib`,
`include/keel/*.h` (a wildcard glob) to `$(PREFIX)/include/keel`, and generated `keel.pc` to
`$(PREFIX)/lib/pkgconfig`; `DESTDIR` is prefixed to all destinations; `PREFIX` defaults to
`/usr/local`; there is no separate `libdir`/`includedir` override. `uninstall` does `rm -f` the
archive and `keel.pc` but `rm -rf $(PREFIX)/include/keel`. `keel.pc` is `sed`-generated from
`keel.pc.in` and is gitignored.

### 4a. PREFIX vs DESTDIR (both semantics frozen)

- PREFIX is the logical install path and is the only path recorded in `keel.pc` (`prefix=@PREFIX@`).
- DESTDIR is a packaging staging root prefixed to the filesystem destinations at install time and
  must never appear in `keel.pc`.

Reproduced, custom logical prefix (`make install PREFIX=<stage>`): 57-file manifest (55 headers +
`lib/libkeel.a` + `lib/pkgconfig/keel.pc`); the out-of-tree C consumer builds/links/runs once
`keel.pc` carries the staged prefix.

Reproduced, packaging staging root (`make install DESTDIR=<stage> PREFIX=/usr`): files land under
`<stage>/usr/{lib,include/keel,lib/pkgconfig}`; `keel.pc` records `prefix=/usr` and does NOT contain
the DESTDIR path (grep for the stage path in `keel.pc`: no match). DESTDIR/PREFIX separation is
therefore correct in the install rule as written.

### 4b. Findings

- 4.1 Default-allow install glob. `install -m 644 include/keel/*.h` installs whatever is present. A
  future internal or misplaced header would be installed silently. F2-3 replaces the glob with an
  explicit reviewed public-header manifest (default-deny).
- 4.2 keel.pc staleness. `keel.pc` depends only on `keel.pc.in`, not on `PREFIX`/`DESTDIR`.
  Reproduced: after one install generated `keel.pc` for one prefix, a second install with a different
  prefix did not regenerate it (up-to-date versus `keel.pc.in`), installing the wrong prefix; `rm -f
  keel.pc` first fixed it. F2-3 makes `keel.pc` regeneration depend on the prefix value; the future
  install-consumer gate always regenerates into a clean staged prefix.
- 4.3 uninstall over-removal (reproduced). `rm -rf $(DESTDIR)$(PREFIX)/include/keel` removes the whole
  directory. In the DESTDIR experiment, an unrelated file placed at
  `<stage>/usr/include/keel/UNRELATED_KEEP.txt` was destroyed by uninstall, while the targeted
  `rm -f` left an unrelated `<stage>/usr/lib/UNRELATED_KEEP.a` intact. F2-3 changes uninstall to
  remove exactly the manifested files and the directory only if it becomes empty; the future
  install-consumer gate asserts an unrelated file under the include dir survives uninstall.
- 4.4 Libs.private. `-lpthread` is correct for a POSIX hosted static link; it does not cover a Windows
  static link (`-lws2_32 -lmswsock -lbcrypt -liphlpapi`). `keel.pc` is a POSIX packaging artifact
  today; F2-3 records this scope explicitly rather than silently widening it.
- 4.5 pkg-config Description is HTTP-only (`keel.pc.in:5`); see 7.7.
- 4.6 Version in `keel.pc.in:7` is a third hardcoded copy of the version string; see 7.6.
- 4.7 No release/package script; install is reproducible from a clean tree via `make install`;
  `keel.pc` is gitignored so generation cannot dirty the repository.

## 5. Public declaration coverage (candidates, not verified coverage)

Inventory: 316 unique public `kl_*` function names, 318 declarations (`kl_version` and
`kl_version_number` are each declared in both umbrellas), across `include/keel/*.h`. Produced by
`tools/f2_public_inventory.sh` (comment-stripped, preprocessor-stripped, statement-split, typedefs and
function-pointer vtable fields excluded) and checked in under `docs/f2/` in F2-1a. The counts below are
the prior manual assessment; F2-4 recomputes them exactly against the checked-in
`docs/f2/function_coverage.tsv`.

Coverage evidence, stated honestly (the first draft over-claimed direct coverage):

- About 290 of the 316 have at least one by-name caller in `tests/`/`examples/`/`integrations/`. A
  by-name caller is a COVERAGE CANDIDATE, not verified coverage.
- 40 of those were manually verified as behavioral tests with assertions on observable effects (for
  example `kl_datagram_send` in `test_datagram_socket.c`, `kl_url_parse` in `test_url.c`,
  `kl_http_server_stats` in `test_http_server_stats.c`, `kl_proxy_parse` in `test_proxy_protocol.c`,
  `kl_http_sse_begin` in `test_http_sse.c`).
- 28 have no by-name caller. These are dominated by internal dispatch hooks reached only through the
  server/websocket/http2 paths (`kl_ws_server_on_readable/_on_writable/_upgrade`,
  `kl_http2_server_on_readable/_upgrade_from_h1`, `kl_http_conn_on_readable`, `kl_http_conn_ingest_proxy`,
  `kl_watcher_rearm`, `kl_event_init_provider`, `kl_platform_caps`, `kl_peer_cred_fd`), plus a few
  `static inline` helpers; the notable public-looking entry points with no direct test are
  `kl_sockaddr_is_multicast` and `kl_datagram_on_writable`.
- The remaining candidate functions (about 250) are UNVERIFIED until the F2-4 manifest classifies each
  one.

Coverage mechanism (design; implemented in F2-4):

- A tree-derived generated inventory from a declaration-aware extractor (AST/libclang preferred where
  CI has it, the dependency-free extractor as the repo-standard fallback).
- A checked-in reviewed coverage manifest, one row per function, classifying it as one of:
  direct-assertion, indirect-execution (exercised via a higher-level path, not asserted by name),
  compile-only, example, or intentional-public-but-untested:<reason> with a `file::function` citation.
  These categories are distinct; a by-name caller does not auto-map to direct-assertion.
- A default-deny gate `check-public-coverage`: fails if any inventory name lacks a manifest entry or
  any manifest entry is stale (symbol gone).
- Internal dispatch hooks are NOT auto-exempted. A public-looking symbol reached only indirectly
  triggers a public-surface decision under F2-B (should it be public at all?), and only after that
  decision may it be recorded as indirect-execution or moved off the public surface.

## 6. Installed-style examples and consumers

Representative consumers that must build against the staged install with no repository-private include
paths: minimal event-loop/transport (`event_ctx.h`+`stream.h`), HTTP server (`http_server.h`), HTTP
client (`http_client.h`), datagram (`datagram.h`), DNS (`dns_resolver.h`/`resolver.h`), and public TLS
seam (`tls.h`). The freestanding consumer (`freestanding.h`) stays under its own `make
freestanding-headers` gate and is not folded into the hosted install gate. If F2-A elects C++ support,
one representative consumer is duplicated as a C++ compile-AND-LINK consumer.

Future gate (design; implemented in F2-3): `check-installed-consumer`:

- Installs into a fresh `mktemp -d` prefix with `keel.pc` regenerated for that prefix.
- Builds each consumer from outside the repository using only `pkg-config --cflags/--libs [--static]`
  against `PKG_CONFIG_PATH=<stage>/lib/pkgconfig`, and in a second variant with direct compiler flags;
  never adds a source-tree `include/`/`src/`/integration path.
- Asserts includes resolve only from the staged prefix and linking uses the staged `libkeel.a`.
- Asserts uninstall removes only manifested files (an unrelated file under the include dir survives).
- Portable across BSD and GNU userlands, safe for prefixes with spaces, self-cleaning on success and
  failure.

## 7. v3 public-surface review: explicit release-sensitive decisions

Severity: BLOCK = needs an explicit reviewed call before 3.0.0; SHOULD = should fix in F2; INFO.

- 7.1 KlEventLoop.fd exposure (BLOCK; own increment F2-D). `event.h:22-31` inlines `KlEventLoop` with
  `int fd`, `void *_backend`, `KlAllocator *alloc`, `const KlEventOps *ops`, embedded by value as the
  first member of `KlEventCtx` (event_ctx.h). Its remove/reserve/opaque decision is F2-D.
- 7.2 KlEventCtx / event-state inline layout (BLOCK; F2-B). `event_ctx.h` publishes the full
  `KlEventCtx`/`KlWatcher` runtime layout with no `event_ctx_detail.h` split, unlike
  `KlStream`/`KlListener`/`KlConnectOp`. Decision: opaque+detail treatment, or an explicit
  recompile-required embed blessing.
- 7.3 HTTP server/connection layout exposure (BLOCK; F2-B). `KlHttpServer` publishes its full 47-line
  layout (http_server.h:138-184), embeds `KlHttpConnPool` by value, and pulls in the concrete
  `KlHttpConn`/`KlHttpConnPool` runtime layout whose functions are mostly implementation seams (1b).
  Because the server is not opaque and embeds the pool by value, no umbrella edit isolates the
  connection layout. Decision: make `KlHttpServer` and/or the pool opaque (heap handle), or bless the
  full layout as a stable caller-inspectable contract. This is the correction to the first draft's
  "drop http_connection.h from the umbrella" action, which achieved nothing.
- 7.4 Vtable extension risk (BLOCK worst case; SHOULD rest; F2-C). No public vtable carries a
  `struct_size`/`version` field; only `KlTls` has a required-subset validator (`kl_tls_vtable_valid`,
  tls.h:203-206). Worst case: `KlHttp2ClientSession` (http2_client.h:67-84) has a by-value
  `keel_cbs`+`keel_ctx` tail and cannot grow compatibly. `KlSocketOps` (socket.h) documents
  append-only growth but ends in a non-fn-ptr `name` member (ambiguous append point). `KlEventOps` is
  best disciplined ("MUST stay last").
- 7.5 Config-struct versioning (BLOCK; F2-C). All public `*Config` structs lack `struct_size`/version.
  Evidence it already bites: `kl_datagram_init_ex` (datagram.h:176) added a function parameter rather
  than a config field because `KlDatagramConfig` layout is frozen (datagram.h:24-26).
- 7.6 Triplicated version macros (SHOULD). `keel.h:42-53`, `freestanding.h:38-44` (guarded by
  `#ifndef KL_VERSION_STRING`, hiding drift), and `keel.pc.in:7`. Single-source for 3.0.0.
- 7.7 HTTP-only framing (SHOULD). Umbrella `@brief` (keel.h:2-3) and pkg-config Description
  (keel.pc.in:5) present Keel as HTTP-only, omitting the datagram/DNS/event/TLS/thread-pool/stream
  substrate. Reword for 3.0.0.
- 7.8 Datagram facade absent from the umbrella (BLOCK; folds into F2-2 if accepted). Additive,
  compatibility-safe: add `datagram.h`/`datagram_batch.h` to the umbrella, keeping `datagram_detail.h`
  opt-in; or document the omission as intentional in the `keel.h` banner (ties to 7.7).

Good patterns to preserve (INFO): the `*_detail.h` opt-in split and umbrella exclusion; the opaque
`KlDgramCore`; the `KlEventOps` "must stay last" discipline; `kl_tls_vtable_valid`.

## 8. Proposed permanent gates (design only; not implemented in F2-0)

All default-deny, tree-derived where reliable, manifest-backed where semantic classification cannot be
inferred, self-canaried, portable across BSD/GNU userlands, safe for paths with spaces, self-cleaning,
`file:line` diagnostics. Enrolled in CI in F2-6.

- check-public-headers (F2-1b): per-header first-and-only-include compile per section 3 and F2-A.
- check-install-manifest (F2-3): staged `make install` into `mktemp -d` equals exactly the approved
  archive + pkg-config + public-header manifest; unmanifested-installed or missing-manifested fails.
- check-installed-consumer (F2-3): out-of-tree consumers over staged pkg-config in both flag variants;
  asserts no source-tree include leak and that uninstall preserves unrelated files.
- check-public-coverage (F2-4): regenerate inventory; every function has a reviewed manifest entry;
  no stale entries.

## 9. Proposed increments

Sequencing principle: no decision is made before the inventory that is its evidence base exists, and
no decision that depends on another decision runs before it. Concretely, the census is checked in
first (F2-1a); the extensibility policy (F2-C) precedes the layout decision (F2-B) because
size/versioning determines how any retained caller-constructed layout is frozen; the KlEventLoop.fd
decision (F2-D) precedes the KlEventCtx decision (part of F2-B) because KlEventLoop is embedded within
KlEventCtx; and the standalone-compile gate (F2-1b) is implemented only after the accepted surface and
the C/C++ policy exist. Each increment is one reviewed step, validated locally, then paused.

- F2-0: this docs-only inventory and freeze. Commit and pause. (current)
- F2-1a: checked-in public-header, public-function, and complete public-type inventories plus
  classification scaffolding only. No API changes, no header edits, no gate wiring. The 1b
  per-declaration census (193 types: struct/union/enum/opaque/callback/alias) is produced here as
  reviewable data by `tools/f2_public_inventory.sh`, whose `--check` verifies the raw inventories
  reproduce and that both scaffolds are exact key-set joins (missing/extra/duplicate/malformed/drifted
  keys fail) and whose `--selftest` canaries the extractor (concrete struct/union/enum, opaque handle,
  callback-not-a-layout, cross-umbrella duplicate function, comment-embedded fake, multiline decl,
  preprocessor guard). Commit and pause; do NOT begin F2-A in this increment.
- F2-A: C and C++ support and linkage policy (section 3). Recommended ruling: support C++ linkage via
  `extern "C"` guards on every installed header, C-first. Ratifying it schedules the guard addition
  and the staged compile-and-link C++ consumer (with the negative canary).
- F2-C: vtable and config extensibility policy (7.4, 7.5) - whether to adopt `struct_size`/version and
  validators, and how to resolve `KlHttp2ClientSession`. Precedes F2-B.
- F2-D: KlEventLoop.fd remove/reserve/opaque decision (7.1). Precedes the KlEventCtx part of F2-B
  because KlEventLoop is embedded in KlEventCtx.
- F2-B: public layouts and exposure - decided using the complete F2-1a census and the accepted F2-C
  and F2-D policies: `KlHttpServer`/`KlHttpConn`/`KlHttpConnPool` connection/server exposure (7.3) and
  `KlEventCtx` (7.2). Decide opaque vs blessed layout per struct.
- F2-1b: implement `check-public-headers` against the accepted surface (section 3), per the F2-A
  outcome. Approved public-header install manifest + coverage-inventory wiring land here.
- F2-2: umbrella policy per section 2 (single-source version macros; add datagram headers if 7.8
  accepted). No layout-isolation claims here; that is F2-B. Validate and pause.
- F2-3: manifest-driven install/uninstall (replace glob and `rm -rf`), keel.pc prefix/staleness fix,
  Libs.private scope note, DESTDIR/PREFIX assertions, `check-install-manifest` +
  `check-installed-consumer`. Validate and pause.
- F2-4: coverage manifest + `check-public-coverage` + focused tests for the untested public entry
  points, applying the F2-B decisions for indirect-only symbols. Validate and pause.
- F2-5: living documentation - Doxygen input list, README, pkg-config Description and umbrella `@brief`
  reword (7.7), installed-style examples. Validate and pause.
- F2-6: combined local release-style validation and CI enrollment of the four gates. Pause before any
  push.

## 10. F2-0 validation

Performed for this docs-only change (reported with the commit): `git diff --check`;
`make check-doc-refs`; `make check-old-layout`; `make check-no-milestones`; `make check-no-em-dash`;
in-repo Markdown-link resolution; confirmation this file is pure ASCII; and confirmation that no
build, header, test, workflow, or installation file changed. Nothing is pushed and no remote CI is run
in F2-0.
