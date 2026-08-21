# R1 — Physical Restructure Inventory & Design Freeze (protocols/ + integrations/)

Status: **DOCS-ONLY FREEZE — no code moved.** Pause for review before R2.
Branch: `restructure/protocols-integrations` (off merged `main`, taxonomy T1–T4 present).

## 0. Scope, constraints, sequencing

Behavior-neutral **physical/module-boundary** reorganization only. The `KlHttp*` taxonomy rename
(T1–T4) is complete and accepted; the post-rename names/files are authoritative. This increment does
**not** rename any public API, redesign any ABI/state-machine, add abstractions, resurrect `KlUdp*`, or
change completion/readiness/freestanding/capability behavior. No push/PR without authorization; no
`Co-Authored-By`; preserve the untracked restructure prompt; never use destructive reset/clean.

**Public-header ruling (frozen).** Installed public headers stay flat under `include/keel/`. This
increment moves **implementation files only** (`src/*.c`, internal `src/*.h`, `parsers/*.c`,
integration sources). No public include path (`<keel/*.h>`) and no public symbol changes.

## 1. Target layout

```
src/                         # substrate: transport + runtime machinery (unchanged role)
include/keel/                # ALL public headers stay here, flat (ruling above)
protocols/
  http/                      # shared HTTP model + HTTP/1 impl + HTTP-family coordination seams
  http2/                     # HTTP/2 engine + sessions + completion adapter
  websocket/                 # WebSocket framing + client/server + completion adapter (KlWs*)
  dns/                       # built-in DNS resolver + wire parser + hosted DNS-system config
  proxy_protocol/            # PROXY v1/v2 parser + CIDR
integrations/
  platform/{lwip,uefi}
  tls/{openssl,mbedtls,boringssl,libressl}
  http2/nghttp2
# NOTE: no transports/quic/ (reserved, no impl → not created).
# NOTE: no integrations/codec/miniz/ — miniz is external BYO (MINIZ_DIR); the adapter
#       TUs (compress_miniz.c/decompress_miniz.c) are substrate. See §8-D5.
```

## 2. Classification inventory

Counts: 90 `src/*.c`, 42 internal `src/*.h`, 2 `parsers/*.c`, 55 public `include/keel/*.h` (all stay).

### 2.1 Substrate — STAYS in `src/`

**`.c` (61):** allocator.c, allocator_default_stdlib.c, async.c*, completion_absent.c, completion_core.c,
completion_dispatch.c, completion_readiness_stub.c, compress.c†, compress_miniz.c, decompress.c,
decompress_miniz.c, connect_op.c, datagram.c, datagram_batch.c, datagram_close.c, datagram_core.c,
datagram_life.c, datagram_open.c, datagram_recv.c, datagram_send.c, datagram_slots.c, drain.c, error.c,
event_ctx.c, event_dispatch.c, event_epoll.c, event_iocp.c, event_iouring.c, event_kqueue.c,
event_poll.c, event_pollcomp.c, event_pollcomp_builtin.c, event_wsapoll.c, file_io.c, kl_cstr.c,
kl_cstr_builtin.c, listener.c, platform_posix.c, platform_win.c, platform_wakeup_posix.c,
platform_wakeup_win.c, resolve_sync.c‡, resolver_cache.c, sockaddr.c, socket_posix.c, socket_winsock.c,
socket_dgram_posix.c, socket_dgram_win.c, stream.c, stream_close.c, stream_read.c, stream_write.c,
thread_pool.c, timer.c, udp_cmsg.c, udp_cmsg_win.c, url.c, version.c.

**`.h` (substrate, stay):** completion.h, completion_internal.h§, io_engine.h, socket.h, sockaddr_native.h,
sockcompat.h, stream.h→stream_close.h/stream_read.h/stream_write.h, datagram*.h (7), dgram_recv_classify.h,
listener.h, connect_op.h, event_builtin.h, event_caps.h, event_pollcomp_internal.h, platform.h, kl_cstr.h,
drain_reserve.h, watcher_internal.h, resolve_sync.h, udp_cmsg.h, udp_cmsg_win.h.

Footnotes flag files whose classification is a **finding/decision** (see §4):
`*` async.c is HTTP-coupled — proposed → `protocols/http/` (§4.2, open decision D1).
`†` compress.c is mixed codec+HTTP-adapter — proposed HTTP-adapter extraction (§4.4, open decision D2).
`‡` resolve_sync.c is a getaddrinfo system-resolver — substrate vs dns (§8, open decision D3).
`§` completion_internal.h is the HTTP-completion seam — proposed → `protocols/http/` (§4.3, D4).

### 2.2 `protocols/http/` — HTTP shared model + HTTP/1

**`.c`:** http_body_reader_buffer.c, http_body_reader_multipart.c, http_client_async.c,
http_client_common.c, http_client_pool.c, http_client_proxy.c, http_client_sync.c, http_connection.c,
http_cors.c, http_proto_hooks.c, http_redirect.c, http_response.c, http_router.c,
http_server_activation.c, http_server_core.c, http_server_plat_posix.c, http_server_plat_win.c,
http_server.c, http_sse.c, http1_chunked.c, completion_http_server.c.
**`parsers/`:** parsers/http1_parser_llhttp.c, parsers/http1_response_parser_llhttp.c → `protocols/http/` (D6: flat vs `protocols/http/parsers/`).
**internal `.h`:** internal.h, http_client_internal.h, http_client_proxy.h, http_conn_internal.h,
http_response_internal.h, http_server_plat.h, http_proto_hooks.h, completion_internal.h (§4.3).
**Proposed additions (from splits/extractions, §4):** async.c (D1), http_compress.c + the
`kl_http_compress_stream_*` bodies extracted from compress.c (D2).

### 2.3 `protocols/http2/`

**`.c`:** http2_client.c, http2_server.c, completion_http2.c. **`.h`:** http2_internal.h.

### 2.4 `protocols/websocket/` (stays `KlWs*`)

**`.c`:** websocket.c, websocket_client.c, completion_ws.c, http_server_ws.c (the WS-upgrade bridge — D7).
**`.h`:** base64.h, sha1.h, utf8.h (RFC 6455 handshake utils; **included only by WS TUs** — verified).

### 2.5 `protocols/dns/`

**`.c`:** dns_resolver.c, dns_sys_posix.c, dns_sys_win.c. **`.h`:** dns_sys.h.
(Generic `KlResolver` interface `resolver.h` + `resolver_cache.c` decorator stay substrate.)

### 2.6 `protocols/proxy_protocol/`

**`.c`:** proxy_protocol.c. (Public `proxy_protocol.h` stays in `include/keel/`.)

### 2.7 integrations/ reorg (R3)

Real dirs only. `openssl` hosts the shared adapter that `boringssl`/`libressl` recompile.

| current | → new | role |
|---|---|---|
| integrations/lwip/ | integrations/platform/lwip/ | socket+event provider (readiness + raw completion) |
| integrations/uefi/ | integrations/platform/uefi/ | EFI platform (alloc/event/socket TCP4+UDP4/time/entropy) |
| integrations/openssl/ | integrations/tls/openssl/ | TLS adapter (shared source) |
| integrations/mbedtls/ | integrations/tls/mbedtls/ | TLS adapter |
| integrations/boringssl/ | integrations/tls/boringssl/ | recompiles ../openssl/tls_openssl.c |
| integrations/libressl/ | integrations/tls/libressl/ | recompiles ../openssl/tls_openssl.c |
| integrations/nghttp2/ | integrations/http2/nghttp2/ | HTTP/2 session adapter |

## 3. Exact path-migration table (implementation files)

R2 (`git mv src/X → protocols/<fam>/X`; parsers likewise):

| new dir | files (basename unchanged) |
|---|---|
| protocols/http/ | http_body_reader_buffer.c http_body_reader_multipart.c http_client_async.c http_client_common.c http_client_pool.c http_client_proxy.c http_client_sync.c http_connection.c http_cors.c http_proto_hooks.c http_redirect.c http_response.c http_router.c http_server_activation.c http_server_core.c http_server_plat_posix.c http_server_plat_win.c http_server.c http_sse.c http1_chunked.c completion_http_server.c http1_parser_llhttp.c http1_response_parser_llhttp.c internal.h http_client_internal.h http_client_proxy.h http_conn_internal.h http_response_internal.h http_server_plat.h http_proto_hooks.h completion_internal.h + async.c(D1) + http_compress.c(D2) |
| protocols/http2/ | http2_client.c http2_server.c completion_http2.c http2_internal.h |
| protocols/websocket/ | websocket.c websocket_client.c completion_ws.c http_server_ws.c(D7) base64.h sha1.h utf8.h |
| protocols/dns/ | dns_resolver.c dns_sys_posix.c dns_sys_win.c dns_sys.h |
| protocols/proxy_protocol/ | proxy_protocol.c |

R3 integration moves: see §2.7 table (whole-directory `git mv`).

## 4. Dependency findings (the crux — verified by tracing, not inference)

### 4.1 No upward edges in the completion/event/socket substrate (GOOD)
`completion_core.c` and `completion_dispatch.c` route connection completions **only** through the opaque
`KlEventCtx.comp_conn_dispatch` function-pointer hook (installed by `completion_http_server.c` at server
init) — neither includes any protocol header. `event_*.c`, `socket_*.c`, `platform_*.c`, `stream*.c`,
`datagram*.c`, `timer.c`, `thread_pool.c` name no HTTP symbol. **The completion adapters can move to
`protocols/` and the substrate completion core stays clean, unchanged.**

### 4.2 `async.c` is HTTP-coupled → propose `protocols/http/` (open decision **D1**)
`async.c` operates on `KlHttpConn`/`KlHttpServer`, calls `kl_http_server_conn_release`,
`kl_http_conn_on_writable`, reads `KlHttpConnState`. It is the HTTP-server async connection-suspension
driver, not a generic primitive. Tension: taxonomy freeze §5.5 kept `async.{h,c}` "generic" **by name**.
Resolution: the *public* `async.h` (the `KlAsyncOp`/`KlAsyncFn` event-loop primitive) stays in
`include/keel/` and keeps its generic name; the *implementation* `async.c` moves to `protocols/http/`
because it is HTTP-server-coupled. Naming is unchanged; only physical location moves.

### 4.3 `internal.h` moves WHOLESALE to `protocols/http/` — no split needed
`internal.h` mixes substrate `kl_stream_*` inlines (on `KlStream`) with HTTP `conn_read/conn_write/…`
(on `KlHttpConn`) + `kl_http_server_*`/`KlHttp2WriteFn`/`kl_http2_server_feed` decls. **Verified:** the
substrate `kl_stream_*` half is consumed by exactly one file — `http_connection.c` (HTTP) — and every
includer of `internal.h` is an HTTP-family TU (async.c[D1], http_*/http2_server/completion_http_server).
So it moves as one HTTP internal header; **no neutral extraction required**. (If a future *substrate*
consumer needs `kl_stream_recv/send`, extract them then — not now.)

### 4.4 `compress.c` mixed codec + HTTP adapter → propose extraction (open decision **D2**)
`compress.c` defines the generic codec wrappers **and** `kl_http_compress_stream_begin/write/end`
(operating on `KlHttpResponse`/`KlHttpCompressStream`). `decompress.c` is **fully generic**
(`kl_decompress_stream_*` per taxonomy §5.6, no HTTP include) → stays substrate untouched. No *circular*
dependency exists (compress.c includes only `<keel/compress.h>`), so a split is a **cleanliness** choice,
not a hard requirement. Proposed (recommended): extract the three `kl_http_compress_stream_*` bodies into
`protocols/http/http_compress.c`; the generic codec vtable stays in `src/compress.c`. Behavior-neutral
(same functions, different TU). Alternative: keep compress.c substrate as a documented dual-role file
(the HTTP adapter reaches only *public* headers, so it violates no impl-header rule).

### 4.5 `kl_monotonic_ms()` — substrate clock declared in a public HTTP header (open decision **D5**)
`kl_monotonic_ms()` is a generic monotonic clock **defined** in `platform_posix.c`/`platform_win.c`
(substrate) but **declared** in `include/keel/http_connection.h`. Substrate `timer.c` and
`resolver_cache.c` include `<keel/http_connection.h>` **only** to obtain it — a substrate→HTTP
public-header edge. Not an impl-header violation (it's a public header), but a smell. Proposed: relocate
the *declaration* to a substrate public header (`<keel/timer.h>` or a new `<keel/clock.h>`); definition
stays in `platform_*.c`; symbol name unchanged. This is a **public-header content change** (which header
declares the symbol), so it needs reviewer sign-off under the public-header ruling. Alternative: accept
the public-header edge and document it.

### 4.6 `resolver_cache.c` borrows an HTTP client constant (minor)
`resolver_cache.c` (substrate decorator) includes `<keel/http_client.h>` for
`KL_HTTP_CLIENT_HOSTNAME_MAX`. Minor substrate→HTTP public-header edge. Proposed: give the cache its own
`KL_RESOLVER_CACHE_HOSTNAME_MAX` (or move the constant to a substrate header). Folds into D5's decision.

### 4.7 HTTP-family completion/coordination seam (http ↔ http2 ↔ websocket)
The HTTP-family shares three internal seam headers, all owned by `protocols/http/`:
- `internal.h` — `conn_read/conn_write` connection I/O (used by completion_http2.c, completion_ws.c, http_server_ws.c).
- `http_proto_hooks.h` — the upgrade/hook registry (http2/ws register into it; http_connection.c dispatches through it).
- `completion_internal.h` — declares `kl_comp_http2_drive`/`kl_comp_ws_drive` (defined in http2/ws) + `kl_comp_close`/`kl_comp_tls_flush` (defined in completion_http_server.c).

Per prompt §147 ("HTTP may coordinate HTTP/2 and WebSocket through explicit protocol seams"), the edges
`protocols/http2/` → `protocols/http/{internal.h,http_proto_hooks.h,completion_internal.h}` and
`protocols/websocket/` → same are **permitted** cross-protocol seam includes. The build resolves them via
`-Iprotocols/http` (added for the http2/ws/… compiles). The R4 gates must **allow** intra-`protocols/`
includes while forbidding substrate→protocol and protocol→integration-impl edges (§6).

## 5. Build-system / CI / test / fuzz / integration reference inventory

Every reference that must be updated as files move (Makefile line numbers approximate — verify at edit
time; the taxonomy commits shifted some).

**Makefile — core manifests (must repoint `src/…` → `protocols/<fam>/…`):**
- `CORE_SRC` (~L187–200): all HTTP/HTTP2/WS/DNS/PROXY `.c` + the `$(SERVER_PLAT_SRC)`/`$(DNS_SYS_SRC)` per-OS vars.
- `LLHTTP_SRC` (~L207–209): parsers/http1_*.c.
- `SERVER_PLAT_SRC` (L60 win / L153 posix) → `protocols/http/http_server_plat_*.c`.
- `DNS_SYS_SRC` (L63 win / L164 posix) → `protocols/dns/dns_sys_*.c`.

**Makefile — gate file-lists (must repoint AND widen scan roots):**
- `AXIS_PROTO_TUS` (~L856–860, check-sockaddr-neutral): 15 HTTP/HTTP2/WS `.c` paths → new dirs.
- `TIER1_INFRA` (~L891–895, check-tier1-boundary): `$(wildcard src/http_server_plat_*.c)` and
  `$(wildcard src/dns_sys_*.c)` will **stop matching** after the move — repoint to
  `protocols/http/http_server_plat_*.c` / `protocols/dns/dns_sys_*.c`; keep `src/http_server_core.c`
  etc. entries repointed to `protocols/http/`.
- **check-tier1-boundary scan loop `for f in src/*.c parsers/*.c` (~L902): MUST add `protocols/*/*.c`**
  or protocol TUs silently escape the Tier-1 gate. (Same for any `src/*.c`-only gate loop.)

**Makefile — freestanding manifests (repoint):** `FREESTANDING_CLIENT_SRC` (~L1259–1261, http_client_* +
http1_response_parser), `FREESTANDING_SERVER_SRC` (~L1371–1375, completion_http_server + http_connection +
http_response + http_router + http1_chunked + http_body_reader_buffer + http_server_core + http_proto_hooks +
http1_parser), `FREESTANDING_DNS_SRC` (~L1487, dns_resolver), plus the `*_SC_SRC`/`*_HARNESS_SRC` mirrors.
These are the highest-risk: freestanding/EFI link exact object lists.

**Makefile — pattern/glob/clean:** the generic `%.o: %.c` rule (L273) works for any dir. `clean`
(L767–813) wildcards `src/*.o`/`src/*.d` etc.; **add `find protocols -name '*.[od]' -delete`** (and the
`.freestanding.o`/`.fuzz.o` `find`s are already location-agnostic — good).

**Makefile — fuzz:** `FUZZ_LIB_OBJ` is derived from `CORE_SRC`+`LLHTTP_SRC` via `%.fuzz.o` — repointing
those manifests is sufficient; the 8 fuzz targets (parser, response_parser, multipart=http; websocket;
dns; proxy; url=substrate; decompress=codec on-demand) need no per-target path edits.

**Makefile — test suite lists:** `WIN_TEST_SUITES` (~L367) and `IOURING_TEST_SUITES` (~L654) reference
test **basenames** (test files are NOT moving this phase) → **no change**. `check-no-httplegacy`
`HTTPLEGACY_FILES_RE` matches filenames not paths → **still valid** after the move.

**CI (`.github/workflows/ci.yml`):** delegates entirely to `make` targets — **no hardcoded src/ path
references** (verified). One exception already handled: the `tests/test_http_router` hardening step (T3).
No CI edits needed beyond what the Makefile changes cover. (Note: ci.yml was recently updated upstream.)

**Integration builds (KEEL_ROOT-relative — R3 depth change):** every integration Makefile uses
`KEEL_ROOT ?= ../..`. After nesting one level deeper (`integrations/tls/mbedtls/` etc.), `KEEL_ROOT`
becomes `../../..`. Cross-adapter refs update: nghttp2's ALPN test `../mbedtls` → `../../tls/mbedtls`;
lwip's `loopback-*-tls` `../mbedtls` → `../../tls/mbedtls`; boringssl/libressl `../openssl/tls_openssl.c`
stays `../openssl/…` (siblings under `tls/`). EFI: `build_mock_efi_test.sh` uses `-I$KEEL_ROOT/src` and
`-I$KEEL_ROOT/spikes/uefi` and compiles EFI TUs + links `$KEEL_ROOT/libkeel.a`; the `-I…/src` must ALSO
gain `-I…/protocols/http` etc. if any host-mock TU includes a moved internal seam (verify at R3). lwIP
`loopback-raw` uses `-I$(KEEL_ROOT)/src` similarly. These are per-integration and validated in R3.

**Header include-path (core build):** compiles of `protocols/http2/*.c`, `protocols/websocket/*.c`, and
(if any) files that include HTTP-family seams need `-Iprotocols/http` (and http itself needs
`-Iprotocols/http`). Substrate compiles must NOT get `-Iprotocols/*` (keeps the boundary honest). The
core object rule needs per-directory include flags (or a small `-Iprotocols/http` added only to the
protocol object recipes).

## 6. Proposed permanent gates (R4)

Mirror the existing token/path gates (`check-tier1-boundary`, `check-no-kludp`, `check-no-httplegacy`):
default-deny, token/path-oriented, permanent self-canary, BSD+GNU, `-I` binary skip, `file:line`
diagnostics, no whole-line allowlist masking.

- **G1 `check-substrate-purity`** — no `src/*.c` or `src/*.h` may `#include "…protocols/…"` or a
  protocol-owned basename (http_*, http2_*, websocket/base64/sha1/utf8, dns_sys, proxy_protocol internal
  headers). Default-deny over `src/`. Canary: a substrate file including a protocol header must fail.
- **G2 `check-protocol-no-integration`** — no `protocols/*/*.{c,h}` may include an integration impl
  header (integrations/*/*.h that is not a declared public seam). Default-deny.
- **G3 `check-integration-seam`** — integrations reach core/protocol only through `include/keel/*.h` or a
  declared internal seam header (enumerated allowlist), never a moved protocol impl `.c`/private `.h`.
- **G4 `check-protocol-home`** — a first-party protocol impl basename (the frozen §3 file list) may exist
  **only** under its `protocols/<fam>/` home, never re-added to `src/` (default-deny new files in src/
  matching protocol name patterns).
- **G5** — old `src/…`/`parsers/…` protocol paths cannot reappear: extend `check-no-httplegacy`'s
  filename scan (or a sibling) with the **old `src/<proto>.c` / `parsers/http1_*.c` paths** as
  path-qualified deleted references (allow the new `protocols/…` paths).
- **Widen existing gates:** `check-tier1-boundary` scan loop + `AXIS_PROTO_TUS`/`TIER1_INFRA` to include
  `protocols/*/*.c` (§5). `check-sockaddr-neutral` `AXIS_PROTO_TUS` repointed.

The permitted intra-`protocols/` HTTP-family seam edges (§4.7) are explicitly **allowed** by G1/G2 (they
are protocol→protocol, not substrate→protocol nor protocol→integration).

## 7. Reviewable increment sequence

- **R1 (this doc)** — inventory + freeze, docs-only. Commit, pause.
- **R2 — move first-party protocols, one family per commit, validate + pause each:**
  - R2a `protocols/http/` (incl. the seam headers, async.c[D1], http_compress split[D2], parsers).
    *Largest; establishes the HTTP-family seam + `-Iprotocols/http`.*
  - R2b `protocols/http2/` (consumes http seam).
  - R2c `protocols/websocket/` (consumes http seam; http_server_ws.c[D7]).
  - R2d `protocols/dns/`.
  - R2e `protocols/proxy_protocol/`.
- **R3 — integrations by role, one category per commit:** platform/lwip, platform/uefi, http2/nghttp2,
  tls/{openssl,mbedtls,boringssl,libressl} (update KEEL_ROOT depth, cross-adapter paths, EFI/lwIP/QEMU
  commands, standalone build scripts, host mocks). Pause each.
- **R4 — gates G1–G5 + widen existing gates + doc reconciliation** (README/CLAUDE/architecture/CONTRIBUTING/
  diagrams/module counts/CI comments/freestanding docs → new paths; historical design docs may keep
  historical paths). Wire gates into `.PHONY` + CI next to the existing checks.

Validation after every code-bearing increment (per prompt §280): make test; debug-test; cppcheck;
check-{tier1-boundary,sockaddr-neutral,doc-refs,no-kludp,no-httplegacy} + the new G-gates; freestanding
header/lib/link gates; pollcomp suites; container epoll + io_uring under ASan/UBSan/LSan; MinGW/IOCP;
lwIP raw + BSD provider; EFI host-mock + QEMU/OVMF; fuzz compile; `git diff --check`.

## 8. Open decisions (need reviewer ruling before R2)

- **D1 — `async.c` → `protocols/http/`?** Recommended yes. **The generic/HTTP separation the reviewer
  raised has already happened structurally, not in naming:** the *generic* async substrate is
  `KlWatcher` + `kl_event_ctx_run` + `kl_event_dispatch` (in `event_ctx.{c,h}`, staying substrate — this
  is what clients and the thread pool use); `async.{c,h}` is *wholly* the HTTP-connection-suspension
  layer — all four functions take `KlHttpServer*`/`KlHttpConn*` (7 HTTP refs, 1 generic-ctx ref), and
  `async.h` itself forward-declares those HTTP types. There is **no generic core hiding inside `async.c`
  to extract** — it is already the "HTTP-specific part." So the correct move is simply `async.c` →
  `protocols/http/`. A **future, optional** taxonomy follow-on (OUT of this behavior-neutral restructure's
  scope, and the taxonomy series is merged) could rename `KlAsyncOp`→`KlHttpAsyncOp` / `kl_async_*`→
  `kl_http_async_*` to make the coupling explicit in the public name — recorded here as a deferred
  naming cleanup, not proposed for this increment.
- **D2 — split `compress.c`?** Recommended: extract `kl_http_compress_stream_*` → `protocols/http/http_compress.c`
  (behavior-neutral). Alternative: keep compress.c substrate as a documented dual-role file.
- **D3 — `resolve_sync.c` substrate vs `protocols/dns/`?** Recommended substrate (a thin getaddrinfo/
  `KlSockAddr` wrapper with **no DNS wire logic**; sibling of the generic `resolver_cache.c`). The prompt
  (§72) allows "hosted DNS-system adapters" under dns — reviewer may prefer dns.
- **D4 — `completion_internal.h` home.** Recommended `protocols/http/` (HTTP owns completion
  coordination; http2/ws include it as a declared seam). Confirms the §4.7 seam model.
- **D5 — relocate `kl_monotonic_ms()` declaration** out of `<keel/http_connection.h>` to a substrate
  public header (cuts the substrate→HTTP edge for timer.c/resolver_cache.c), and the D6 `resolver_cache`
  constant. This is a **public-header content change** — needs explicit sign-off. Recommended, but can be
  deferred/accepted-as-is if the reviewer prefers zero public-header churn this increment.
- **D6 — parsers layout:** `protocols/http/http1_*_llhttp.c` (flat, recommended) vs `protocols/http/parsers/`.
- **D7 — `http_server_ws.c` home:** `protocols/websocket/` (recommended — it is WS server protocol/handshake
  logic bootstrapped through HTTP upgrade, per §70) vs `protocols/http/` (it is the HTTP server's upgrade
  entry). Either way it consumes the http seam (§4.7).

**Nothing moves until this freeze is accepted.**
