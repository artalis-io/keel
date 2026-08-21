# R1 — Physical Restructure Inventory & Design Freeze (protocols/ + integrations/)

Status: **DOCS-ONLY FREEZE, rev 2 — no code moved.** Pause for review before R2.
Branch: `restructure/protocols-integrations` (off merged `main`, taxonomy T1–T4 present).

Rev 2 folds in the reviewer rulings on D1–D7, the **governing classification principle**, the concrete
splits it implies (compress, internal.h, clock, resolver bound), the **gate-timing rule** (existing gates
repointed in the same commit as each move), and the **enumerated integration seam allowlist** (incl.
same-role TLS adapter reuse). Corrects the stale §-references from rev 1.

## 0. Scope & constraints

Behavior-neutral **physical/module-boundary** reorganization. Taxonomy T1–T4 is authoritative. No
public-API rename, ABI/state-machine redesign, new abstraction, `KlUdp*` resurrection, or
completion/readiness/freestanding/capability change. No push/PR without authorization; no
`Co-Authored-By`; preserve the untracked restructure prompt; never destructive reset/clean.

**Public-header ruling.** Installed public headers stay **flat** under `include/keel/` (no `keel/http/`
subdirs). This increment moves implementation files and performs the **narrow, symbol/ABI-preserving
public-header ownership cleanups** ruled below (new flat `clock.h`, `http_compress.h`) — no symbol
renames, no aliases.

## 0.1 Governing classification principle (frozen — applies everywhere)

> **Every generic function/type belongs to a generic substrate TU/header. Every protocol- or
> integration-specific function/type belongs to its protocol/integration TU/header. There are no
> permanently mixed-role implementation files.**

Operationally, for every file touched:
1. Classify each **function/type independently** (not the file by majority).
2. **Split** a mixed file rather than assign it wholesale.
3. `src/` keeps only transport / runtime / **generic codec** primitives.
4. HTTP, HTTP/2, WebSocket, **DNS-wire**, and PROXY logic live in their protocol homes.
5. Provider/backend implementations live under their integration home.
6. Cross-layer communication only through **explicit public** or **narrowly-documented internal seam**
   headers.

The concrete mixed points this principle forces are frozen in §4 (compress split, `internal.h` 3-way
split, `clock.h` extraction, resolver bound). R2 applies the same per-function test to every file as it
moves.

## 1. Target layout

```
src/                         # substrate: transport + runtime + generic codec
include/keel/                # ALL public headers, flat (+ new clock.h, http_compress.h)
protocols/{http,http2,websocket,dns,proxy_protocol}/
integrations/{platform/{lwip,uefi}, tls/{openssl,mbedtls,boringssl,libressl}, http2/nghttp2}
# no transports/quic/ (reserved, no impl). no integrations/codec/miniz/ — miniz is external
# BYO (MINIZ_DIR); its adapter TUs (compress_miniz.c/decompress_miniz.c) are substrate (§2.7).
```

## 2. Classification inventory

90 `src/*.c`, 42 internal `src/*.h`, 2 `parsers/*.c`, 55 public `include/keel/*.h`.

### 2.1 Substrate — STAYS in `src/`

**`.c` (substrate):** allocator.c, allocator_default_stdlib.c, completion_absent.c, completion_core.c,
completion_dispatch.c, completion_readiness_stub.c, **compress.c (generic codec only — HTTP adapter
extracted, §4.4)**, compress_miniz.c, decompress.c, decompress_miniz.c, connect_op.c, datagram*.c (11),
drain.c, error.c, event_*.c (11), file_io.c, kl_cstr.c, kl_cstr_builtin.c, listener.c, platform_posix.c,
platform_win.c, platform_wakeup_posix.c, platform_wakeup_win.c, resolve_sync.c, resolver_cache.c,
sockaddr.c, socket_posix.c, socket_winsock.c, socket_dgram_posix.c, socket_dgram_win.c, stream.c,
stream_close.c, stream_read.c, stream_write.c, thread_pool.c, timer.c, udp_cmsg.c, udp_cmsg_win.c, url.c,
version.c. (`async.c` LEAVES substrate → `protocols/http/`, §4.2.)

**`.h` (substrate):** completion.h, io_engine.h, socket.h, sockaddr_native.h, sockcompat.h, stream*.h,
datagram*.h (7), dgram_recv_classify.h, listener.h, connect_op.h, event_builtin.h, event_caps.h,
event_pollcomp_internal.h, platform.h, kl_cstr.h, drain_reserve.h, watcher_internal.h, resolve_sync.h,
udp_cmsg.h, udp_cmsg_win.h. **NEW substrate `.h`: `stream_io.h`** (the generic `kl_stream_*` I/O inlines
extracted from `internal.h`, §4.3). **NEW public substrate `include/keel/clock.h`** (`kl_monotonic_ms`,
§4.5).

### 2.2 `protocols/http/`

**`.c`:** http_body_reader_buffer.c, http_body_reader_multipart.c, http_client_async.c,
http_client_common.c, http_client_pool.c, http_client_proxy.c, http_client_sync.c, http_connection.c,
http_cors.c, http_proto_hooks.c, http_redirect.c, http_response.c, http_router.c,
http_server_activation.c, http_server_core.c, http_server_plat_posix.c, http_server_plat_win.c,
http_server.c, http_sse.c, http1_chunked.c, completion_http_server.c, **async.c** (§4.2),
**http1_parser_llhttp.c, http1_response_parser_llhttp.c** (from `parsers/`, flat — D6), **http_compress.c**
(NEW — HTTP compress-stream bodies, §4.4).
**internal `.h`:** **http_internal.h** (the HTTP half of `internal.h`, §4.3), http_client_internal.h,
http_client_proxy.h, http_conn_internal.h, http_response_internal.h, http_server_plat.h,
http_proto_hooks.h, **completion_internal.h** (§4.7 seam — D4).
**public `.h` added:** **`include/keel/http_compress.h`** (NEW — HTTP compress adapter types/fns, §4.4).

### 2.3 `protocols/http2/`
**`.c`:** http2_client.c, http2_server.c, completion_http2.c.
**`.h`:** http2_internal.h (gains the HTTP/2 seam decls split out of `internal.h`, §4.3).

### 2.4 `protocols/websocket/` (stays `KlWs*`)
**`.c`:** websocket.c, websocket_client.c, completion_ws.c, **http_server_ws.c** (WS-upgrade bridge — D7).
**`.h`:** base64.h, sha1.h, utf8.h (RFC 6455 handshake utils; included only by WS TUs — verified).

### 2.5 `protocols/dns/`
**`.c`:** dns_resolver.c, dns_sys_posix.c, dns_sys_win.c. **`.h`:** dns_sys.h.
(`resolve_sync.c` STAYS substrate — D3: a replaceable blocking name-resolution/platform seam, no DNS
wire logic. Generic `resolver.h` + `resolver_cache.c` stay substrate.)

### 2.6 `protocols/proxy_protocol/`
**`.c`:** proxy_protocol.c. (Public `proxy_protocol.h` stays in `include/keel/`.)

### 2.7 integrations/ reorg (R3) — real dirs only

| current | → new | notes |
|---|---|---|
| integrations/lwip/ | integrations/platform/lwip/ | socket+event provider (readiness + raw completion) |
| integrations/uefi/ | integrations/platform/uefi/ | EFI platform (alloc/event/socket TCP4+UDP4/time/entropy) |
| integrations/openssl/ | integrations/tls/openssl/ | shared TLS adapter source |
| integrations/mbedtls/ | integrations/tls/mbedtls/ | TLS adapter |
| integrations/boringssl/ | integrations/tls/boringssl/ | recompiles `../openssl/tls_openssl.c` (compat macro) |
| integrations/libressl/ | integrations/tls/libressl/ | recompiles `../openssl/tls_openssl.c` (compat macro) |
| integrations/nghttp2/ | integrations/http2/nghttp2/ | HTTP/2 session adapter |

**miniz:** external BYO via `MINIZ_DIR` (defaults to `../miniz`, outside the repo). The adapter TUs
`compress_miniz.c`/`decompress_miniz.c` are **substrate** (generic codec backends). **No
`integrations/codec/miniz/` is created** — no files would move there.

## 3. Exact path-migration table

`git mv` in R2/R3 (basenames unchanged unless a split creates a new file). Splits (§4) create the NEW
files `src/stream_io.h`, `include/keel/clock.h`, `protocols/http/http_compress.c`,
`include/keel/http_compress.h`, `protocols/http/http_internal.h` (from `internal.h`).

| new home | files |
|---|---|
| protocols/http/ | http_body_reader_buffer.c http_body_reader_multipart.c http_client_async.c http_client_common.c http_client_pool.c http_client_proxy.c http_client_sync.c http_connection.c http_cors.c http_proto_hooks.c http_redirect.c http_response.c http_router.c http_server_activation.c http_server_core.c http_server_plat_posix.c http_server_plat_win.c http_server.c http_sse.c http1_chunked.c completion_http_server.c async.c http1_parser_llhttp.c http1_response_parser_llhttp.c http_compress.c(NEW) http_internal.h(from internal.h) http_client_internal.h http_client_proxy.h http_conn_internal.h http_response_internal.h http_server_plat.h http_proto_hooks.h completion_internal.h |
| protocols/http2/ | http2_client.c http2_server.c completion_http2.c http2_internal.h(+seam decls) |
| protocols/websocket/ | websocket.c websocket_client.c completion_ws.c http_server_ws.c base64.h sha1.h utf8.h |
| protocols/dns/ | dns_resolver.c dns_sys_posix.c dns_sys_win.c dns_sys.h |
| protocols/proxy_protocol/ | proxy_protocol.c |
| src/ (new/changed) | stream_io.h(NEW) compress.c(generic-only) |
| include/keel/ (new) | clock.h(NEW) http_compress.h(NEW) |
| integrations/ | whole-dir moves per §2.7 |

## 4. Dependency findings & rulings

### 4.1 Substrate completion/event/socket is clean (no upward edges — verified)
`completion_core.c`/`completion_dispatch.c` route connection completions only through the opaque
`KlEventCtx.comp_conn_dispatch` hook (installed by the server at init); `event_*.c`, `socket_*.c`,
`platform_*.c`, `stream*.c`, `datagram*.c`, `timer.c`, `thread_pool.c` name no protocol symbol. The
completion adapters move to `protocols/` with the substrate core untouched.

### 4.2 `async.c` → `protocols/http/` (D1 — ACCEPTED)
`async.c` is wholly HTTP-connection-suspension (all four fns take `KlHttpServer*`/`KlHttpConn*`; calls
`kl_http_server_conn_release`). The **generic** async substrate already lives separately as `KlWatcher` +
`kl_event_ctx_run` + `kl_event_dispatch` (`event_ctx.{c,h}`, substrate — used by client + thread pool);
there is no generic core to carve out of `async.c`. It moves whole to `protocols/http/`. **Deferred
(not this increment):** public `async.h` is HTTP-owned in substance; a taxonomy rename
`KlAsyncOp`→`KlHttpAsyncOp` / `kl_async_*`→`kl_http_async_*` (and header → `http_async.h`) is recorded as
future taxonomy work, executed only if this effort is explicitly expanded beyond physical restructuring.

### 4.3 `internal.h` — 3-WAY SPLIT (D-principle; supersedes rev 1's "move whole")
Per §0.1, `internal.h` is split by function nature, not moved by majority:
- **Substrate → NEW `src/stream_io.h`:** `kl_stream_provider`, `kl_stream_recv`, `kl_stream_send`,
  `kl_stream_recv_peek`, `kl_stream_io_status` (inlines over `KlStream`/`kl_sock_*` — generic).
- **HTTP → `protocols/http/http_internal.h`:** `kl_http_conn_from_stream`, `conn_provider`, `conn_read`,
  `conn_write`, `conn_write_all`, `best_effort_conn_write`, and the `kl_http_server_*` + `kl_http_server_log*`
  forward decls. Includes `src/stream_io.h` (protocol→substrate, allowed).
- **HTTP/2 → `protocols/http2/http2_internal.h`:** `kl_http2_server_feed`, `KlHttp2WriteFn`,
  `kl_http2_server_set_writer`.

The HTTP connection-I/O seam (`conn_read`/`conn_write` in `http_internal.h`) is consumed by
`completion_http2.c`, `completion_ws.c`, `http_server_ws.c` — permitted cross-protocol HTTP-family seam
includes (§4.7). `http_connection.c` includes `http2_internal.h` to drive `kl_http2_server_feed`
(HTTP coordinating HTTP/2 via seam, §147).

### 4.4 `compress.c` + `compress.h` — FULL SPLIT (D2 — ACCEPTED per §0.1)
`compress.c`/`compress.h` mix the generic `KlCompress*` codec (vtable/factory/config) with the HTTP
adapter (`KlHttpCompressStream`, `kl_http_response_body_compress`, `kl_http_compress_stream_begin/write/end`),
and `compress.h` pulls `<keel/http_response.h>`. `decompress.{c,h}` are **fully generic** (verified — no
HTTP type/include) → stay substrate whole.
- **Generic stays:** `src/compress.c` (codec impl); `include/keel/compress.h` (`KlCompress`/`KlCompressCtx`/
  `KlCompressFactory`/`KlCompressConfig` — **drop the `<keel/http_response.h>` include**; pure codec).
- **HTTP moves:** NEW `protocols/http/http_compress.c` (the `kl_http_compress_stream_*` +
  `kl_http_response_body_compress` bodies); NEW public `include/keel/http_compress.h` (`KlHttpCompressStream`
  + those fns; includes `<keel/compress.h>` + `<keel/http_response.h>`). Umbrella `keel.h` adds it.
- Consumers update includes; **no aliases; symbols + ABI unchanged** (public-header ownership cleanup,
  explicitly authorized).

### 4.5 `kl_monotonic_ms()` → NEW public `include/keel/clock.h` (D5 — ACCEPTED, revised)
Declared today in both public `<keel/http_connection.h>:208` and internal `src/platform.h:29`; defined in
`platform_posix.c`/`platform_win.c` (substrate). Split by ownership:
- NEW `include/keel/clock.h` declares `kl_monotonic_ms()` (public substrate); umbrella `keel.h` adds it.
- **Remove** the declaration from `<keel/http_connection.h>`; `src/platform.h` includes `<keel/clock.h>`
  (single source, drops its private dup). Definitions stay in `platform_*.c`.
- All consumers (substrate `timer.c`, `resolver_cache.c`; protocol http/*) include `<keel/clock.h>`.
- Symbol/ABI unchanged; no aliases.

### 4.6 `resolver_cache.c` — drop HTTP header deps (D5-adjacent)
`resolver_cache.c` includes `<keel/http_client.h>` (for `KL_HTTP_CLIENT_HOSTNAME_MAX`) and
`<keel/http_connection.h>` (for the clock). Ruling: use `<keel/clock.h>` for the clock, and a **private
internal hostname bound** in `resolver_cache.c` (a local `#define`) — **do not introduce or rename a
public constant**. Result: `resolver_cache.c` includes **no HTTP header**; stays clean substrate.

### 4.7 HTTP-family coordination seam (http ↔ http2 ↔ websocket)
`protocols/http/` owns the family seam headers: `http_internal.h` (`conn_read`/`conn_write`),
`http_proto_hooks.h` (upgrade/hook registry), `completion_internal.h` (declares `kl_comp_http2_drive`/
`kl_comp_ws_drive` [defined in http2/ws] + `kl_comp_close`/`kl_comp_tls_flush` [defined in
completion_http_server.c]). Per §147, `protocols/http2/` and `protocols/websocket/` are **permitted** to
include these declared http seams; the build adds `-Iprotocols/http` to the http2/ws/http protocol
compiles. Substrate compiles get **no** `-Iprotocols/*`. The R4 gates allow intra-`protocols/` seam
includes; they forbid substrate→protocol and protocol→integration-impl (§6).

## 5. Build / CI / test / fuzz / integration reference inventory

Line numbers approximate (verify at edit time).

**Makefile — repoint `src/…`→`protocols/<fam>/…`:** `CORE_SRC` (~L187–200, all HTTP/HTTP2/WS/DNS/PROXY
`.c` + `$(SERVER_PLAT_SRC)`/`$(DNS_SYS_SRC)`); `LLHTTP_SRC` (~L207–209 → `protocols/http/http1_*_llhttp.c`);
`SERVER_PLAT_SRC` (L60/L153 → `protocols/http/`); `DNS_SYS_SRC` (L63/L164 → `protocols/dns/`); the split
adds `src/compress.c` (unchanged path) + `protocols/http/http_compress.c` to CORE_SRC and `src/stream_io.h`
is header-only. `FREESTANDING_{CLIENT,SERVER,DNS,*_SC,*_HARNESS}_SRC` (~L1259/1371/1487/1567/1583/1706/1729)
— highest-risk exact object lists; repoint every moved TU incl. `async.c` (freestanding client) and
`http_compress.c` if a freestanding server compresses.

**Makefile — include flags:** add `-Iprotocols/http` to the object recipes for `protocols/{http,http2,
websocket}/*.c` (and any TU including a family seam). Do NOT add `-Iprotocols/*` to substrate recipes.

**Makefile — gate file-lists + scan roots (see §6 gate-timing):** `AXIS_PROTO_TUS` (~L856–860, repoint);
`TIER1_INFRA` (~L891–895: `$(wildcard src/http_server_plat_*.c)`→`protocols/http/…`,
`$(wildcard src/dns_sys_*.c)`→`protocols/dns/…`, + explicit `http_server_core.c`/`http_server.c`/
`async.c`→`protocols/http/…`); **`check-tier1-boundary` scan loop `for f in src/*.c parsers/*.c` (~L902)
MUST become `src/*.c parsers/*.c protocols/*/*.c`.**

**Makefile — clean/fuzz/tests:** add `find protocols -name '*.[od]' -delete` to `clean` (the
`.freestanding.o`/`.fuzz.o` `find`s are already location-agnostic). Fuzz `%.fuzz.o` derives from the
repointed `CORE_SRC`/`LLHTTP_SRC` → no per-target edits. Test suite lists (`WIN_TEST_SUITES`,
`IOURING_TEST_SUITES`) reference **basenames**; tests do NOT move this phase → no change.
`check-no-httplegacy` `HTTPLEGACY_FILES_RE` matches filenames not paths → still valid.

**CI (`.github/workflows/ci.yml`):** delegates to `make` targets — no hardcoded `src/` path refs. No CI
edits beyond what Makefile changes cover.

**Integrations (R3 depth change):** `KEEL_ROOT ?= ../..` → `../../..` after nesting. Cross-adapter refs:
nghttp2 ALPN `../mbedtls`→`../../tls/mbedtls`; lwip `loopback-*-tls` `../mbedtls`→`../../tls/mbedtls`;
boringssl/libressl `../openssl/tls_openssl.c` stays `../openssl/…` (siblings under `tls/`). EFI/lwip build
scripts use `-I$KEEL_ROOT/src` (substrate seams — unaffected; add `-I$KEEL_ROOT/protocols/http` only if a
host-mock TU includes a moved http seam — verify per-integration at R3). Validated per integration in R3.

## 6. Gates

### 6.1 Gate-timing rule (frozen — P1)
**Every R2/R3 move commit MUST, in the SAME commit, repoint/widen every EXISTING gate that enumerates or
scans the moved files** — `check-tier1-boundary` (scan-loop roots + `TIER1_INFRA`), `check-sockaddr-neutral`
(`AXIS_PROTO_TUS`). No moved TU may spend a single increment outside existing enforcement. The NEW gates
G1–G5 may mature incrementally across R2–R4, but they never substitute for keeping the existing gates
current within each move commit. R2a is the first commit to apply this (it moves the HTTP files AND
repoints those gates together).

### 6.2 New gates (mature across R2–R4)
Mirror the existing token/path gates (default-deny, self-canary, BSD+GNU, `-I` binary skip, `file:line`,
no whole-line allowlist masking):
- **G1 `check-substrate-purity`** — no `src/*.{c,h}` may include a protocol header (`protocols/**` or a
  protocol-owned basename). Default-deny over `src/`.
- **G2 `check-protocol-no-integration`** — no `protocols/*/*.{c,h}` may include an integration impl header.
- **G3 `check-integration-seam`** — integrations reach core only through PUBLIC `include/keel/*.h` OR a
  **frozen allowlist of substrate internal seam headers** (§6.3), never a moved protocol impl `.c`/private
  `.h`. Integration-local headers are unrestricted.
- **G4 `check-protocol-home`** — a first-party protocol impl basename (frozen §3 list) may exist only
  under its `protocols/<fam>/` home, never re-added to `src/`. **Explicit carve-out:** this does NOT
  restrict same-role TLS adapter source reuse under `integrations/tls/` (§6.4).
- **G5** — old `src/<proto>.c` / `parsers/http1_*.c` paths cannot reappear (path-qualified deleted refs;
  allow the new `protocols/…` paths). Fold into / sibling of `check-no-httplegacy`.

### 6.3 G3 — frozen integration→internal-seam allowlist
Integrations legitimately consume SUBSTRATE provider/platform seam headers (observed today):
`socket.h`, `platform.h`, `io_engine.h`, `event_caps.h`, `event_builtin.h`, `completion.h`,
`sockaddr_native.h`, `sockcompat.h`, `watcher_internal.h`, `resolve_sync.h`, `datagram_life.h` (and
`datagram_*.h` detail seams), `udp_cmsg*.h`. **These are the allowlist.** All are substrate; none is a
protocol header. Extend the allowlist only for a new substrate seam, explicitly, with a reason. G3 forbids
any integration including a `protocols/**` header or a moved protocol private header/`.c`.

### 6.4 G4 — same-role TLS adapter source reuse (explicit permission)
`integrations/tls/boringssl/` and `integrations/tls/libressl/` **recompile the sibling
`integrations/tls/openssl/tls_openssl.c`** (with `OPENSSL_IS_BORINGSSL` / `LIBRESSL_VERSION_NUMBER` compat
macros). This is same-role adapter **source reuse within `integrations/tls/`** and is explicitly
permitted — it is NOT re-adding a protocol implementation `.c` to `src/`. G4/G5's "no moved
implementation `.c` reappears" clause applies to **first-party protocol** files, not to a TLS integration
compiling its sibling's adapter source. This keeps G4 consistent with the frozen TLS layout.

## 7. Reviewable increment sequence

- **R1 (this doc)** — freeze, docs-only. Commit, pause.
- **R2a `protocols/http/`** — moves the HTTP files + `async.c`; performs the `internal.h` 3-way split
  (creates `src/stream_io.h`, `protocols/http/http_internal.h`, seeds `http2_internal.h` decls) and the
  `compress` split (`protocols/http/http_compress.c` + `include/keel/http_compress.h`, generic
  `compress.{c,h}` purified); adds `-Iprotocols/http`; **repoints `check-tier1-boundary` +
  `check-sockaddr-neutral` in the same commit** (§6.1); begins G1/G4/G5 as they mature. Validate, pause.
- **R2b http2**, **R2c websocket** (incl. `http_server_ws.c`), **R2d dns**, **R2e proxy_protocol** — each
  moves its family, consumes the http seam where needed, repoints the existing gates in the same commit,
  validates, pauses.
- **R2 clock/resolver cleanup** — the `clock.h` extraction (§4.5) and `resolver_cache.c` bound (§4.6) land
  with R2a (they unblock substrate purity for `timer.c`/`resolver_cache.c`).
- **R3** — integrations by role (platform/lwip, platform/uefi, http2/nghttp2, tls/*), fixing KEEL_ROOT
  depth + cross-adapter paths + EFI/lwIP/QEMU commands; G2/G3 mature here. Pause each.
- **R4** — finalize G1–G5 + doc reconciliation (README/CLAUDE/architecture/CONTRIBUTING/diagrams/module
  counts/CI comments/freestanding docs → new paths; historical design docs may keep historical paths).

Validation after every code-bearing increment (prompt §280): make test; debug-test; cppcheck;
check-{tier1-boundary,sockaddr-neutral,doc-refs,no-kludp,no-httplegacy} + matured G-gates; freestanding
header/lib/link gates; pollcomp suites; container epoll + io_uring under ASan/UBSan/LSan; MinGW/IOCP;
lwIP raw + BSD provider; EFI host-mock + QEMU/OVMF; fuzz compile; `git diff --check`.

## 8. Rulings (D1–D7) — all resolved; no open decisions

- **D1** async.c → `protocols/http/`. **ACCEPTED.** (async.h HTTP-owned; rename deferred, §4.2.)
- **D2** compress: **FULL split** (public header + TU) per §0.1. **ACCEPTED (revised):** generic
  `compress.{c,h}` purified; HTTP adapter → `protocols/http/http_compress.c` + `include/keel/http_compress.h`
  (§4.4). (Supersedes rev 1's "keep mixed"/"TU-only".)
- **D3** resolve_sync.c **stays substrate** — replaceable blocking name-resolution/platform seam, not DNS
  wire logic. **ACCEPTED.**
- **D4** completion_internal.h → `protocols/http/` (HTTP-family coordination seam). **ACCEPTED** (§4.7).
- **D5** `kl_monotonic_ms()` → **new public `include/keel/clock.h`**; removed from `http_connection.h`;
  consumers include `clock.h`; resolver cache gets a **private** hostname bound (no public constant).
  **ACCEPTED (revised)** (§4.5–4.6).
- **D6** parsers **flat** `protocols/http/http1_*_llhttp.c`. **ACCEPTED.**
- **D7** http_server_ws.c → `protocols/websocket/`. **ACCEPTED.**

**Deferred (out of this restructure's scope):** the `KlAsyncOp`→`KlHttpAsyncOp` public rename (§4.2), a
future `include/keel/clock.h`-style hierarchical/public-API decisions beyond the two narrow ownership
cleanups ruled above. **Nothing moves until this rev-2 freeze is accepted.**
