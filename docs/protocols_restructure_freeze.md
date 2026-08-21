# R1 — Physical Restructure Inventory & Design Freeze (protocols/ + integrations/)

Status: **DOCS-ONLY FREEZE, rev 3 — no code moved.** Pause for review before R2.
Branch: `restructure/protocols-integrations` (off merged `main`, taxonomy T1–T4 present).

Rev 3 (this pass) applies §0.1 to the miniz codec **adapters** — backend-specific integration files that
move to `integrations/codec/miniz/` (not substrate) — and replaces G3's wildcard "datagram_*.h detail
seams" with the exact two headers integrations consume (`datagram_life.h`, `datagram_open.h`).
Rev 2 folded in the D1–D7 rulings, the **governing classification principle** (§0.1), the concrete splits
(compress, internal.h, clock, resolver bound), the **gate-timing rule**, and the enumerated integration
seam allowlist incl. same-role TLS adapter reuse.

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
integrations/{platform/{lwip,uefi}, tls/{openssl,mbedtls,boringssl,libressl}, http2/nghttp2, codec/miniz}
# no transports/quic/ (reserved, no impl). integrations/codec/miniz/ holds Keel's miniz ADAPTER TUs
# (compress_miniz.c/decompress_miniz.c) — backend-specific integration files (§2.7, §0.1 rule 5).
# The miniz LIBRARY itself stays external BYO (MINIZ_DIR); its public adapter headers stay flat.
```

## 2. Classification inventory

90 `src/*.c`, 42 internal `src/*.h`, 2 `parsers/*.c`, 55 public `include/keel/*.h`.

### 2.1 Substrate — STAYS in `src/`

**`.c` (substrate):** allocator.c, allocator_default_stdlib.c, completion_absent.c, completion_core.c,
completion_dispatch.c, completion_readiness_stub.c, **compress.c (generic codec only — HTTP adapter
extracted, §4.4)**, decompress.c, connect_op.c, datagram*.c (11),
drain.c, error.c, event_*.c (11), file_io.c, kl_cstr.c, kl_cstr_builtin.c, listener.c, platform_posix.c,
platform_win.c, platform_wakeup_posix.c, platform_wakeup_win.c, resolve_sync.c, resolver_cache.c,
sockaddr.c, socket_posix.c, socket_winsock.c, socket_dgram_posix.c, socket_dgram_win.c, stream.c,
stream_close.c, stream_read.c, stream_write.c, thread_pool.c, timer.c, udp_cmsg.c, udp_cmsg_win.c, url.c,
version.c. (`async.c` LEAVES substrate → `protocols/http/`, §4.2.)

**`.h` (substrate):** completion.h, completion_io.h, socket.h, sockaddr_native.h, sockcompat.h, stream*.h,
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
**`.h`:** (none WS-owned.) **Correction (R2c):** base64.h/sha1.h/utf8.h were listed here in rev 1–3
but per §0.1 they are **generic** utilities (Base64 encoding, SHA-1, UTF-8 validation) — classified by
*nature*, not by their sole current consumer (the RFC 6455 handshake). They **stay in substrate `src/`**;
the WS TUs reach them via `-Isrc`. (Original rev-1 "included only by WS TUs" was a classify-by-consumer
error, superseded by the governing principle.)

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
| src/compress_miniz.c src/decompress_miniz.c | integrations/codec/miniz/ | **NEW dir** — Keel's miniz codec ADAPTER TUs |

**miniz (rev-3 correction):** the miniz *library* is external BYO via `MINIZ_DIR` (defaults to `../miniz`,
outside the repo) — that is unchanged. But **Keel's adapter implementations** `compress_miniz.c` /
`decompress_miniz.c` are real **backend-specific integration files** (they `#include <miniz.h>` and
implement the `KlCompress`/`KlDecompress` vtables for that one backend). Under §0.1 rule 5 they belong in
`integrations/codec/miniz/`, not `src/`. They reach core **only** through the public headers
`<keel/compress_miniz.h>` / `<keel/decompress_miniz.h>` (→ `<keel/compress.h>`/`<keel/decompress.h>`) +
external `<miniz.h>` — verified: no internal `src/` seam include, so they need **no** G3 allowlist entry.
Their public adapter headers stay flat in `include/keel/`. **Build model preserved (behavior-neutral):**
the root Makefile keeps conditionally compiling them into `libkeel.a` when `KEEL_COMPRESS=miniz`, from the
new path (§5). A standalone integration Makefile/archive (KEEL_ROOT pattern like siblings) is an OPTIONAL
R3 refinement, not required for the move and deferred to avoid a build-model change.

## 3. Exact path-migration table

`git mv` in R2/R3 (basenames unchanged unless a split creates a new file). Splits (§4) create the NEW
files `src/stream_io.h`, `include/keel/clock.h`, `protocols/http/http_compress.c`,
`include/keel/http_compress.h`, `protocols/http/http_internal.h` (from `internal.h`).

| new home | files |
|---|---|
| protocols/http/ | http_body_reader_buffer.c http_body_reader_multipart.c http_client_async.c http_client_common.c http_client_pool.c http_client_proxy.c http_client_sync.c http_connection.c http_cors.c http_proto_hooks.c http_redirect.c http_response.c http_router.c http_server_activation.c http_server_core.c http_server_plat_posix.c http_server_plat_win.c http_server.c http_sse.c http1_chunked.c completion_http_server.c async.c http1_parser_llhttp.c http1_response_parser_llhttp.c http_compress.c(NEW) http_internal.h(from internal.h) http_client_internal.h http_client_proxy.h http_conn_internal.h http_response_internal.h http_server_plat.h http_proto_hooks.h completion_internal.h |
| protocols/http2/ | http2_client.c http2_server.c completion_http2.c http2_internal.h(+seam decls) |
| protocols/websocket/ | websocket.c websocket_client.c completion_ws.c http_server_ws.c (base64.h/sha1.h/utf8.h STAY in src/ — generic, §2.4 correction) |
| protocols/dns/ | dns_resolver.c dns_sys_posix.c dns_sys_win.c dns_sys.h |
| protocols/proxy_protocol/ | proxy_protocol.c |
| integrations/codec/miniz/ | compress_miniz.c decompress_miniz.c (from src/) |
| src/ (new/changed) | stream_io.h(NEW) compress.c(generic-only) |
| include/keel/ (new) | clock.h(NEW) http_compress.h(NEW) |
| integrations/ | whole-dir moves per §2.7 (+ codec/miniz above) |

## 4. Dependency findings & rulings

### 4.1 Substrate completion/event/socket — PARTIALLY neutral (corrected; see §4.8/R2f)
`completion_core.c` is fully neutral (routes connection completions only through the opaque
`KlEventCtx.comp_conn_dispatch` hook; `event_*.c` READ/WRITE target `KlStream`, datagram carries
`KlDgramLife`, connect carries fd+watcher). **CORRECTION (reviewer, post-R2e):** rev-1..3 overstated
this — the completion **contract, dispatch, and backends still name HTTP types** for the ACCEPT family
and SENDFILE. `src/completion.h` includes `<keel/http_connection.h>` and its `KlCompletionOps` vtable +
`kl_comp_*` entry points are HTTP-typed for accept (`prime_accepts`/`post_accept`/`shutdown_accepts` take
`struct KlHttpServer*`) and sendfile (`post_sendfile` takes `KlHttpConn*`); `completion_dispatch.c` and
`event_{iocp,iouring,pollcomp}.c` include `<keel/http_server.h>` + `<keel/http_connection.h>`. This is a
real G1 (substrate-purity) violation and is neutralized in **§4.8 / R2f** (a separate increment, not
folded into any R2 move). `completion_core.c`, the `KlCompletionEvent` struct (already HTTP-free), and
the READ/WRITE/datagram/connect ops are unaffected.

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

### 4.8 Completion axis ↔ HTTP neutralization (R2f — dedicated increment)

**The finding (verified by field-level trace).** The completion axis was neutralized for READ/WRITE
(`KlStream`), datagram (`KlDgramLife`+descriptors), and connect (fd+watcher), but **accept and sendfile
were left HTTP-typed**, so substrate still names HTTP:
- `src/completion.h:20` `#include <keel/http_connection.h>`; the `KlCompletionOps` vtable ops
  `prime_accepts`/`post_accept`/`shutdown_accepts` take `struct KlHttpServer*` and `post_sendfile` takes
  `KlHttpConn*`; the `kl_comp_*` accept/sendfile entry-point decls are likewise HTTP-typed.
- `src/completion_dispatch.c:19-21` includes `<keel/http_server.h>` + `<keel/http_connection.h>`; its
  `kl_comp_prime_accepts`/`post_accept`/`post_sendfile`/`shutdown_accepts` are **trivial forwarders**
  (they extract `&s->ev.loop` / `&c->stream.ctx->loop` and call the vtable — no HTTP-state deref).
- `src/event_{iocp,iouring,pollcomp}.c` each `#include <keel/http_server.h>` + `<keel/http_connection.h>`.
  Their accept impls deref **only** `s->listen_fd` + `s->ev.loop._backend` (no pool/conn/HTTP state);
  their sendfile impls deref **only** `c->stream` (`.fd`, `.alloc`, `.ctx->loop`). No HTTP logic.

`completion_core.c` and the `KlCompletionEvent` struct are already HTTP-free (target is `void*`/`KlStream*`;
accept surfaces as a neutral `KL_COMP_ACCEPT` event with `accepted_fd`, dispatched via the neutral
`comp_conn_dispatch` hook that the HTTP adapter owns).

**Also in scope — `src/completion_absent.c` (the `KEEL_NO_COMPLETION` stub).** In that build,
`completion_absent.c` is the **sole** completion TU: it substitutes for *both* the substrate
(`completion_core.c` + `completion_dispatch.c`) *and* the HTTP adapter (`completion_http_server.c`), so it
today `#include`s `io_engine.h` and defines HTTP-typed aborting stubs —
`kl_comp_prime_accepts(struct KlHttpServer*)`, `kl_comp_post_accept(struct KlHttpServer*)`,
`kl_comp_post_sendfile(KlHttpConn*)`, the `KlHttpConn`-typed `kl_comp_post_recv/post_send`, and the four
`kl_io_engine_*` run-loop stubs (`run_completion`/`quiesce_accepts`/`resume_completion`/`post_read`,
all `KlHttpServer*`/`KlHttpConn*`). It must split along the **identical** seam as the live path (below).

**Also in scope — `src/io_engine.h` (mixed-role substrate header).** `io_engine.h` is the shared
completion-seam header, but it too mixes roles: it declares the neutral completion surface (`kl_comp_run`,
`kl_comp_cancel`, `kl_completion_axis_available`, the datagram descriptors + `kl_comp_post_dgram_*` /
`kl_comp_cancel_dgram` / `kl_comp_retire_dgram`, `kl_comp_post_connect`) **and** four HTTP-typed run-loop
APIs (`kl_io_engine_run_completion(KlHttpServer*)`, `kl_io_engine_quiesce_accepts(KlHttpServer*)`,
`kl_io_engine_resume_completion(KlHttpServer*, KlHttpConn*)`, `kl_io_engine_post_read(KlHttpConn*)`) — the
**only** `kl_io_engine_*`-named symbols and the only HTTP-typed decls in the header. So the HTTP wrappers
alone are not enough: the run-loop APIs must move out of substrate too, or `io_engine.h` remains a
substrate header naming HTTP types (a residual G1 violation). Current includers (`grep`), all for the
**neutral surface only**: `completion.h`, `completion_{core,dispatch,absent}.c`, `event_ctx.c`
(`kl_comp_run`), `event_dispatch.c`, `datagram.c`, and `integrations/lwip/event_lwip_raw.c` (`kl_comp_run`)
— these stay on `io_engine.h`. Only the HTTP run-loop APIs are HTTP-typed, and their **call sites** are two
HTTP TUs: `protocols/http/http_server_core.c` (`run`/`quiesce_accepts`/`post_read`) and
`protocols/http/async.c` (`resume`); `completion_http_server.c` owns the definitions and
`completion_absent.c` the `NO_COMPLETION` stubs. (`event_ctx.c`, `event_efi.c`, and `event_lwip_raw.c`
merely *mention* `kl_io_engine_*` in explanatory comments — not calls; `event_efi.c` does not even include
`io_engine.h`.)

**The seam (generic descriptor / neutral-pointer — mirrors the existing `KlStream`/datagram ops).** Since
the backends already touch only neutral fields, retype the four ops off HTTP. Because C has no overloading,
the **neutral entry points take distinct `_raw` names** (matching the existing `kl_comp_post_recv_raw` /
`kl_comp_post_send_raw(KlStream*)` convention); the HTTP-typed thin wrappers keep the current names:

| concern | neutral substrate entry point (new name) | HTTP wrapper (unchanged name) → in `protocols/http/` |
|---|---|---|
| accept prime | `kl_comp_prime_accepts_raw(KlEventCtx*, KlSocketHandle listen_fd) -> int window` | `kl_comp_prime_accepts(KlHttpServer*)` → `&s->ev`, `s->listen_fd` |
| accept re-post | `kl_comp_post_accept_raw(KlEventCtx*)` | `kl_comp_post_accept(KlHttpServer*)` → `&s->ev` |
| accept shutdown | `kl_comp_shutdown_accepts_raw(KlEventCtx*)` | `kl_comp_shutdown_accepts(KlHttpServer*)` → `&s->ev` |
| sendfile | `kl_comp_post_sendfile_raw(KlStream*, head_iov, head_n, head_total, file_fd, count)` | `kl_comp_post_sendfile(KlHttpConn*)` → `&c->stream` |

(`recv`/`send` already follow this split: `kl_comp_post_recv_raw(KlStream*)` substrate +
`kl_comp_post_recv(KlHttpConn*)` HTTP wrapper.) The `KlCompletionOps` **vtable fields are struct members**
(no C symbol collision) — they simply retype to the neutral signatures; the backend still latches
`listen_fd` in prime and reaches its state via `ctx->loop._backend`, exactly as today.

**Naming — ownership-first, three tiers (frozen).** The prefix encodes the role, not the header:
- `kl_comp_*_raw` — neutral completion substrate (the vtable-backed entry points; `src/`).
- `kl_comp_*` — HTTP wrappers that **directly mirror** a neutral `_raw` operation 1:1 (`protocols/http/`).
- `kl_http_comp_*` — HTTP-only completion **orchestration with no neutral counterpart** (`protocols/http/`).

The four `kl_io_engine_*` run-loop APIs are tier 3 (HTTP-only orchestration, no `_raw` sibling), so they
are **renamed** on the move — the `io_engine` prefix was a header artifact, not a description:

| old (`src/io_engine.h`) | new (`protocols/http/completion_http.h`) |
|---|---|
| `kl_io_engine_run_completion(KlHttpServer*, int timeout_ms)` | `kl_http_comp_run(KlHttpServer *server, int timeout_ms)` |
| `kl_io_engine_quiesce_accepts(KlHttpServer*)` | `kl_http_comp_quiesce_accepts(KlHttpServer *server)` |
| `kl_io_engine_resume_completion(KlHttpServer*, KlHttpConn*)` | `kl_http_comp_resume(KlHttpServer *server, KlHttpConn *conn)` |
| `kl_io_engine_post_read(KlHttpConn*)` | `kl_http_comp_post_read(KlHttpConn *conn)` |

The actual production callers are the HTTP TUs — principally `protocols/http/async.c`
(`resume`) and `protocols/http/http_server_core.c` (`run`/`quiesce_accepts`/`post_read`);
`completion_http_server.c` owns the hosted definitions and the `completion_absent` split owns the
`NO_COMPLETION` stubs. `event_ctx.c` calls only the neutral `kl_comp_run` (unaffected). All call sites and
definitions are renamed **atomically in R2f**, and any living comments/tests that mention `kl_io_engine_*`
(e.g. the explanatory notes in `event_ctx.c`, `event_efi.c`, `event_lwip_raw.c`) are reconciled in the same
commit. These are private internal symbols → **no compatibility aliases**; the retired `kl_io_engine_*`
prefix is added to the eventual stale-internal-name gate so it cannot reappear.

Concretely:
- **`completion.h`:** drop the `<keel/http_connection.h>` include; the vtable + the `_raw` neutral entry
  points (accept/sendfile + the existing recv/send/drain/cancel/datagram/connect) are HTTP-free.
- **`completion_dispatch.c` + `event_{iocp,iouring,pollcomp}.c`:** drop the `<keel/http_server.h>` +
  `<keel/http_connection.h>` includes; retype the four op impls / `_raw` entry points to the neutral
  signatures. No behavioral change (the derefs were already neutral).
- **`src/io_engine.h`:** keep **only** the neutral completion surface (`kl_comp_run`, `kl_comp_cancel`,
  `kl_completion_axis_available`, datagram descriptors + `kl_comp_post_dgram_*`/`cancel_dgram`/
  `retire_dgram`, `kl_comp_post_connect`); drop the `struct KlHttpServer;`/`struct KlHttpConn;` forward
  decls and the four HTTP run-loop APIs. It then names no HTTP type and no `kl_io_engine_*` symbol.
- **`protocols/http/completion_http.h` (new HTTP completion seam):** declares the HTTP-typed wrapper
  surface (`kl_comp_prime_accepts(KlHttpServer*)`, `kl_comp_post_accept`, `kl_comp_shutdown_accepts`,
  `kl_comp_post_sendfile(KlHttpConn*)`, the `KlHttpConn` recv/send wrappers) **and** the renamed
  `kl_http_comp_run`/`quiesce_accepts`/`resume`/`post_read`. Defined in `completion_http_server.c` (hosted)
  and `completion_http_absent.c` (`NO_COMPLETION`). Every HTTP TU that used the run-loop APIs switches its
  include from `io_engine.h` → `completion_http.h`; TUs needing only the neutral surface keep `io_engine.h`.
- **`completion_dispatch.c` + `event_{iocp,iouring,pollcomp}.c`:** the wrapper definitions live in
  `completion_http_server.c`, so these substrate/backend TUs implement only the `_raw` neutral entry
  points + retyped vtable ops.
- **`completion_absent.c` splits:** the neutral `_raw` stubs (accept/sendfile + recv/send/drain/cancel/
  datagram/connect/`kl_comp_run`) stay in `src/completion_absent.c` (HTTP-free; drop `io_engine.h`'s HTTP
  decls — it now includes only the purified `io_engine.h`); the HTTP-typed stubs
  (`kl_comp_prime_accepts(KlHttpServer*)`, `post_accept`, `post_sendfile(KlHttpConn*)`, `shutdown_accepts`,
  the `KlHttpConn` recv/send wrappers, and the four `kl_http_comp_*`) move to a new
  **`protocols/http/completion_http_absent.c`** (includes `completion_http.h`), added to the
  `KEEL_NO_COMPLETION` source list next to `completion_absent.c`. This mirrors the hosted split (substrate
  dispatch vs `completion_http_server.c`) so the `NO_COMPLETION` build stays G1-clean too.

**Responsibility split (result):** substrate owns the *neutral* completion vtable + `_raw` entry points +
the purified `io_engine.h` (hosted **and** absent); `protocols/http/` owns the HTTP-typed wrapper surface +
`kl_http_comp_*` orchestration behind `completion_http.h` (hosted `completion_http_server.c`, absent
`completion_http_absent.c`). After R2f, `src/completion.h`, `io_engine.h`, `completion_dispatch.c`,
`completion_absent.c`, and the three backends name **no** HTTP type and no `kl_io_engine_*` symbol survives
anywhere → G1 (`check-substrate-purity`) is satisfiable for the completion axis. Behavior-neutral: no
accept/sendfile/lifetime semantics change; the `KL_COMP_ACCEPT`/dispatch path is untouched.

**Sequencing (frozen):** R2f runs **after** R2e (accepted) and **before** the test-layout refactor. It is
its own reviewed increment — NOT folded into a protocol move — and validated across every completion
backend (pollcomp/io_uring/IOCP) + readiness + `KEEL_NO_COMPLETION` + freestanding, since it retypes the
backend vtable and splits the absent stub.

**Implemented (R2f as-built) — three deltas from the frozen text above:**
1. **`src/io_engine.h` renamed → `src/completion_io.h`** (user directive, mid-implementation): once purified
   to the neutral surface it held nothing named `io_engine`, so the name was misleading. It is the neutral
   consumer-facing completion seam (kept in substrate), sitting between `completion.h` (the backend
   CONTRACT) and consumers. All includers repointed atomically; guard → `KEEL_SRC_COMPLETION_IO_H`. The
   `kl_io_engine_*` symbols were already retired to `kl_http_comp_*` (unchanged from the frozen table); the
   retired `io_engine.h` path + `kl_io_engine_*` prefix go into the eventual stale-name gate.
2. **Autonomous integration backends recover the server via containerof.** The frozen "backends deref only
   neutral fields" holds for the three CORE backends (iocp/iouring/pollcomp: `ctx->loop._backend` +
   `listen_fd`). The AUTONOMOUS integration backends need more — EFI's capacity gate reads `s->pool`, and
   lwIP reads `s->config` and **mutates** `s->listen_fd` — so `el_prime_accepts`/`lwr_comp_prime_accepts`
   recover the owning `KlHttpServer` from the neutral `KlEventCtx` via `containerof(ctx, KlHttpServer, ev)`
   (the sanctioned `server_of_ctx` pattern). This keeps the **vtable signature** neutral (substrate purity
   intact) while integration adapters — which legitimately know `KlHttpServer` (G1 governs `src/` only) —
   recover what they own. The passed `listen_fd` is used directly by the core backends; the autonomous
   backends read/write it through the recovered server.
3. **`completion_http.h` added to the Tier-1/G3 forbidden-header set** (alongside `completion.h`/
   `completion_io.h`): it exposes the HTTP completion seam and would otherwise be a transitive way to pull
   `completion.h` past the gate. All its includers are TIER1_INFRA, so the tightened gate stays green.

## 5. Build / CI / test / fuzz / integration reference inventory

Line numbers approximate (verify at edit time).

**Makefile — repoint `src/…`→`protocols/<fam>/…`:** `CORE_SRC` (~L187–200, all HTTP/HTTP2/WS/DNS/PROXY
`.c` + `$(SERVER_PLAT_SRC)`/`$(DNS_SYS_SRC)`); `LLHTTP_SRC` (~L207–209 → `protocols/http/http1_*_llhttp.c`);
`SERVER_PLAT_SRC` (L60/L153 → `protocols/http/`); `DNS_SYS_SRC` (L63/L164 → `protocols/dns/`); the split
adds `src/compress.c` (unchanged path) + `protocols/http/http_compress.c` to CORE_SRC and `src/stream_io.h`
is header-only. `FREESTANDING_{CLIENT,SERVER,DNS,*_SC,*_HARNESS}_SRC` (~L1259/1371/1487/1567/1583/1706/1729)
— highest-risk exact object lists; repoint every moved TU incl. `async.c` (freestanding client) and
`http_compress.c` if a freestanding server compresses.

**Makefile — miniz adapter (R3 codec):** `COMPRESS_MINIZ_SRC`/`COMPRESS_MINIZ_OBJ` (~L244–245) repoint
`src/compress_miniz.c src/decompress_miniz.c` → `integrations/codec/miniz/…`; they stay conditionally
linked into `$(LIB)` (~L259) when `KEEL_COMPRESS=miniz` (behavior-neutral). Repoint the `fuzz-decompress`
target and the `compress_server` example build (both gated on `KEEL_COMPRESS=miniz`). `MINIZ_DIR`/
`MINIZ_CFLAGS` (external include path) unchanged.

**Makefile — include flags:** add `-Iprotocols/http` to the object recipes for `protocols/{http,http2,
websocket}/*.c` (and any TU including a family seam). Do NOT add `-Iprotocols/*` to substrate recipes.
The miniz adapter compiles need only `MINIZ_CFLAGS` + public `include/` (no protocol/seam `-I`).

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
host-mock TU includes a moved http seam — verify per-integration at R3). **Relative-path depth:** the EFI
mock includes `"../../src/datagram_open.h"` (`mock_efi_test.c:34`); after `uefi` nests one level deeper
(`integrations/platform/uefi/`) this becomes `"../../../src/datagram_open.h"` — a mechanical R3 fixup.
Validated per integration in R3.

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

### 6.3 G3 — frozen integration→internal-seam allowlist (EXACT header names, no wildcards)
Integrations legitimately consume these SUBSTRATE provider/platform seam headers — the **exact,
enumerated** current set (no `*` patterns, so no future private header is silently authorized):
`socket.h`, `platform.h`, `completion_io.h`, `event_caps.h`, `event_builtin.h`, `completion.h`,
`sockaddr_native.h`, `sockcompat.h`, `watcher_internal.h`, `resolve_sync.h`, `datagram_life.h`,
`datagram_open.h`, `udp_cmsg.h`, `udp_cmsg_win.h`. **This exact list is the allowlist.** (`datagram_life.h`
← lwIP raw provider; `datagram_open.h` ← EFI host-mock via `"../../src/datagram_open.h"`.) All are
substrate; none is a protocol header. **A new seam must be added to this list individually, by exact
name, with a rationale** — never via a wildcard. G3 forbids any integration including a `protocols/**`
header or a moved protocol private header/`.c`. (The miniz codec adapters need no entry — they reach core
only through PUBLIC `<keel/compress_miniz.h>`/`<keel/decompress_miniz.h>`.)

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
  validates, pauses. *(R2a–R2e DONE.)*
- **R2f — completion↔HTTP neutralization (§4.8).** Retype the accept + sendfile completion ops off
  `KlHttpServer`/`KlHttpConn` onto neutral args (`KlEventCtx`+`listen_fd` / `KlStream`) under distinct
  `_raw` entry-point names (C has no overloading; matches `kl_comp_post_recv_raw`); drop the
  `<keel/http_connection.h>`/`<keel/http_server.h>` includes from `completion.h`,
  `completion_dispatch.c`, and `event_{iocp,iouring,pollcomp}.c`; **purify `src/io_engine.h`** to the
  neutral completion surface only, moving its four HTTP-typed run-loop APIs to the new
  `protocols/http/completion_http.h` seam **renamed `kl_io_engine_*` → `kl_http_comp_*`** (tier-3
  ownership-first naming; all callers renamed atomically, no aliases, old prefix added to the stale-name
  gate); move the HTTP-typed `kl_comp_*` accept/sendfile/recv/send wrappers to `completion_http.h` too
  (defined in `completion_http_server.c`); **split `completion_absent.c`** — neutral `_raw` stubs stay
  substrate, HTTP-typed stubs + `kl_http_comp_*` move to `protocols/http/completion_http_absent.c` (new
  `KEEL_NO_COMPLETION` source). Behavior-neutral; validated across pollcomp/io_uring/IOCP + readiness +
  `KEEL_NO_COMPLETION` + freestanding. Runs **before** the test-layout refactor. Its own reviewed commit.
- **R2 clock/resolver cleanup** — the `clock.h` extraction (§4.5) and `resolver_cache.c` bound (§4.6) land
  with R2a (they unblock substrate purity for `timer.c`/`resolver_cache.c`).
- **R3** — integrations by role (platform/lwip, platform/uefi, http2/nghttp2, tls/*, **codec/miniz**),
  fixing KEEL_ROOT depth + cross-adapter paths + EFI/lwIP/QEMU commands + the `datagram_open.h`
  relative-path depth; G2/G3 mature here. The **codec/miniz** step moves `compress_miniz.c`/
  `decompress_miniz.c`, repoints the `COMPRESS_MINIZ_SRC`/fuzz-decompress/compress-example references
  (behavior-neutral, still opt-in into `libkeel` via `KEEL_COMPRESS=miniz`), and validates
  `make ... KEEL_COMPRESS=miniz MINIZ_DIR=…` + `fuzz-decompress`. Pause each.
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
- **M (rev-3)** miniz codec ADAPTERS (`compress_miniz.c`/`decompress_miniz.c`) → `integrations/codec/miniz/`
  (backend-specific integration files per §0.1 rule 5; they are NOT substrate). **ACCEPTED.** Build model
  preserved (opt-in into `libkeel` via `KEEL_COMPRESS=miniz`, repointed path); public adapter headers stay
  flat; no G3 allowlist entry needed (public-header reach only).

**Deferred (out of this restructure's scope):** the `KlAsyncOp`→`KlHttpAsyncOp` public rename (§4.2), a
future `include/keel/clock.h`-style hierarchical/public-API decisions beyond the two narrow ownership
cleanups ruled above. **Nothing moves until this rev-2 freeze is accepted.**

## 9. Test-layout reconciliation (docs-only; per docs/claude_code_test_layout_prompt.md)

**Status:** R2a–R2f are COMPLETE (HTTP/HTTP2/WS/DNS/PROXY families moved to `protocols/<fam>/`; completion
axis neutralized). The implementation TUs now live under `protocols/…` but **their tests still all sit flat
in `tests/`** — the taxonomy the freeze established for `src/` vs `protocols/` is not yet mirrored in the
test tree. §9 freezes that reconciliation. **No test or production file moves in this increment — docs only.**

### 9.0 Governing rule (frozen)

Tests follow the ownership of the implementation they exercise (§0.1 applied to tests). **Classify by the
tested CONTRACT, not by incidental consumers/includes** (a generic test used by HTTP stays substrate). A
mixed test file is split, not assigned by majority. There is **no separate substrate test dir** — generic
and genuinely cross-layer tests stay directly under `tests/`. Target test tree:

```
tests/                                  # substrate + genuinely cross-layer + shared harness support
tests/protocols/{http,http2,websocket,dns,proxy_protocol}/
integrations/<role>/<backend>/tests/    # backend-specific (moves in R3 WITH its integration)
```
Public headers stay flat in `include/keel/` (unchanged). `fuzz/` stays flat (a separate fuzzing-harness
axis, not `tests/*.c`) — see §9.4.

### 9.1 Exhaustive `tests/` inventory + classification

110 tracked **C files** (`tests/*.c`); `tests/` also holds additional tracked `.h` support headers and
`tests/fixtures/*` / `tests/freestanding/shim/*` files (group H). Every file appears exactly once below.

**A. Stay in `tests/` — SUBSTRATE (48 pure UTEST TUs + 2 split remainders).** Event/completion axis, streams,
listeners, datagrams, sockets, sockaddr, timers, allocator, error, url, TLS *interface*, thread pool,
resolver cache, I/O status, cross-layer primitives:
`test_allocator, test_connect_op, test_datagram_batch, test_datagram_life,
test_datagram_live, test_datagram_multicast, test_datagram_open, test_datagram_public, test_datagram_socket,
test_decompress, test_dgram_close, test_dgram_core, test_dgram_recv, test_dgram_recv_classify, test_dgram_send,
test_dgram_slots, test_drain, test_error, test_event, test_event_caps, test_event_ctx, test_event_provider,
test_file_io, test_io_status, test_iocp_engine, test_kl_cstr, test_kl_cstr_builtin, test_listener,
test_resolver_cache, test_sockaddr, test_socket_provider,
test_stream, test_stream_close, test_stream_read, test_stream_single_shot, test_stream_transport,
test_thread_pool, test_timer, test_transport_public,
test_udp_cmsg, test_unix_socket, test_url, test_version, test_watcher_aba`.
Plus the **substrate remainders of two split files** (§9.2): `test_async` (generic watcher/`kl_event_ctx_run`
cases only) and `test_tls` (generic TLS-interface cases only). **NOTE (reviewer P1):** `test_compress`,
`test_cross_module`, `test_peer_addr`, `test_peer_cert`, `test_tls_vtable` are **NOT** substrate — they move
wholly to HTTP (group B). `test_overflow` and `test_tls_set_hostname_fail` are split entirely across protocol
families (§9.2) with **no** substrate remainder.

**B. → `tests/protocols/http/` (HTTP/1 + shared HTTP orchestration):**
`test_http1_chunked, test_http1_parser, test_http1_response_parser, test_http_body_reader, test_http_client,
test_http_client_happy_eyeballs, test_http_client_pool, test_http_client_proxy, test_http_client_stream,
test_http_connection, test_http_cors, test_http_integration, test_http_multipart_stream, test_http_proto_hooks,
test_http_redirect, test_http_request, test_http_response, test_http_router, test_http_server_integration,
test_http_server_stats, test_http_sse, test_tls_integration` (HTTPS = TLS-over-HTTP);
`test_alpn, test_read_flow_control, test_timeout` (§9.8 rulings → HTTP);
`test_compress, test_cross_module, test_peer_addr, test_peer_cert, test_tls_vtable` (reviewer P1 — moved
wholly to HTTP); and the **HTTP halves of the split files** (§9.2) `test_http_async` (KlAsyncOp/server
suspension), `test_http_tls` (HTTP connection/response/pool TLS-adapter cases), `test_http_overflow`,
`test_http_client_hostname_fail`.

**C. → `tests/protocols/http2/`:** `test_http2, test_http2_client`; + split halves (§9.2) `test_http2_overflow,
test_http2_client_hostname_fail`.
**D. → `tests/protocols/websocket/`:** `test_websocket, test_websocket_client`; + split halves (§9.2)
`test_websocket_overflow, test_websocket_client_hostname_fail`.
**E. → `tests/protocols/dns/`:** `test_dns_resolver`.
**F. → `tests/protocols/proxy_protocol/`:** `test_proxy_protocol`.

**G. Smoke programs (standalone, not UTEST) — by SUBJECT (§9.8 rulings applied):**
- **Stay in `tests/`** (the completion/event BACKEND axis is the tested contract; HTTP + the identity/mock TLS
  are only the vehicle — rule 5): `smoke_completion_inject, smoke_datagram, smoke_iocp, smoke_iocp_async,
  smoke_iocp_tls, smoke_iouring, smoke_iouring_async, smoke_iouring_client, smoke_pollcomp, smoke_pollcomp_async,
  smoke_pollcomp_client, smoke_pollcomp_tls`. (Ruling: the `*_client` and `*_tls`/`*_async` variants stay —
  `*_client`'s documented contract is the **neutral completion-connect path**, and the `*_tls` variants drive
  **mock** TLS over the backend, not a real backend.)
- `tests/protocols/http/`: `smoke_tcp` (§9.8 ruling → HTTP roundtrip).
- `tests/protocols/websocket/`: `smoke_pollcomp_ws`.
- `tests/protocols/dns/`: `smoke_dns`.
- **`integrations/tls/mbedtls/tests/`** (§9.8 ruling — both explicitly validate the **mbedTLS backend**; they
  move in R3 with the TLS integration): `smoke_tls, smoke_tls_completion`.

**H. Shared harness support (NOT UTEST suites) — STAY in `tests/` (included by many TUs across families):**
`datagram_test_util.h, mock_tls.h, net_compat.h, net_compat_posix.c, net_compat_win.c, win_prelude.h`, the
`tests/fixtures/*` check fixtures, and the freestanding-gate harness set `freestanding_headers.c,
freestanding_harness.c, freestanding_dns_harness.c, freestanding_host_platform.c, freestanding_link_main.c,
freestanding_platform_test.h` + `tests/freestanding/shim/*` (a cross-cutting build-gate concern, not a
protocol suite). Per the §9.8 ruling the whole freestanding harness set — including the family-flavored
`freestanding_harness` (HTTP/1 client mock) and `freestanding_dns_harness` (DNS) — **stays flat in `tests/`**.

### 9.2 Mixed test files that must be split (reviewer P1)

Per §0.1 (split, don't assign by majority). Split files get **family-prefixed, globally-unique basenames**
(required by the §9.5 resolver + §9.6 uniqueness gate). The exact per-`UTEST`-case partition is performed at
move time; the frozen destinations + new basenames are:

| current file | split into | home |
|---|---|---|
| `test_async.c` (generic watcher + HTTP KlAsyncOp/server-suspension) | `test_async.c` (generic watcher / `kl_event_ctx_run`) | `tests/` |
| | `test_http_async.c` (KlAsyncOp suspend/resume, server async) | `tests/protocols/http/` |
| `test_tls.c` (generic TLS interface + HTTP conn/response/pool TLS) | `test_tls.c` (generic `KlTls` vtable/interface) | `tests/` |
| | `test_http_tls.c` (HTTP connection/response/pool TLS-adapter) | `tests/protocols/http/` |
| `test_overflow.c` (independent HTTP / HTTP2 / WS cases) | `test_http_overflow.c` | `tests/protocols/http/` |
| | `test_http2_overflow.c` | `tests/protocols/http2/` |
| | `test_websocket_overflow.c` | `tests/protocols/websocket/` |
| `test_tls_set_hostname_fail.c` (independent HTTP / HTTP2 / WS clients) | `test_http_client_hostname_fail.c` | `tests/protocols/http/` |
| | `test_http2_client_hostname_fail.c` | `tests/protocols/http2/` |
| | `test_websocket_client_hostname_fail.c` | `tests/protocols/websocket/` |

`test_overflow` and `test_tls_set_hostname_fail` have **no substrate remainder** (all cases are per-protocol);
`test_async` and `test_tls` keep a generic-substrate remainder at the original basename. Each split re-homes
its `UTEST` cases verbatim (no case weakened/dropped) and updates the curated-suite lists (§9.4) that named the
original stem.

**Looks-mixed-but-isn't:** `test_alpn.c` exercises the shared ALPN→adapter dispatch and lands assertions on
BOTH the HTTP/1 and HTTP/2 REST paths, but its *contract* is the shared HTTP ALPN-negotiation seam — it moves
**whole** to `tests/protocols/http/` (the H2 leg is coverage of the shared seam, not an independent
H2-internals suite; §9.8 ruling).

### 9.3 Exact old→new path map

- **Whole moves (unsplit), group B–F:** `tests/<f>.c` → `tests/protocols/<fam>/<f>.c`, basename preserved
  (basenames stay globally unique — §9.5). E.g. `tests/test_http_router.c → tests/protocols/http/test_http_router.c`;
  `tests/test_http2_client.c → tests/protocols/http2/test_http2_client.c`;
  `tests/test_websocket.c → tests/protocols/websocket/test_websocket.c`;
  `tests/test_dns_resolver.c → tests/protocols/dns/test_dns_resolver.c`;
  `tests/test_proxy_protocol.c → tests/protocols/proxy_protocol/test_proxy_protocol.c`;
  the reviewer-P1 whole-moves `tests/test_compress.c → tests/protocols/http/test_compress.c`,
  `test_cross_module`, `test_peer_addr`, `test_peer_cert`, `test_tls_vtable` likewise → `tests/protocols/http/`.
- **Splits (§9.2):** original → the new family-prefixed basenames in the §9.2 table (e.g.
  `tests/test_overflow.c → {tests/protocols/http/test_http_overflow.c, http2/test_http2_overflow.c,
  websocket/test_websocket_overflow.c}`; `tests/test_async.c` → keep `tests/test_async.c` +
  `tests/protocols/http/test_http_async.c`).
- **Smokes (§9.1-G / §9.8):** `tests/smoke_tcp.c → tests/protocols/http/smoke_tcp.c`;
  `tests/smoke_pollcomp_ws.c → tests/protocols/websocket/smoke_pollcomp_ws.c`;
  `tests/smoke_dns.c → tests/protocols/dns/smoke_dns.c`;
  `tests/smoke_tls.c` + `tests/smoke_tls_completion.c → integrations/tls/mbedtls/tests/` (in R3). The
  completion-backend smokes (incl. `*_client`, `*_tls`, `*_async`) do **not** move.
- **Group A remainders + group H (helpers/fixtures/freestanding) do NOT move.**

**Integration tests (move in R3 WITH their integration, not here):** `integrations/lwip/*_test.c` +
`lwip_loopback_test.c` + `lwip_raw_testclient.{c,h}` → `integrations/platform/lwip/tests/`;
`integrations/uefi/{s,u}*_selftest.c, host_map_test.c, mock_efi_test.c, dgram_*_selftest.c` →
`integrations/platform/uefi/tests/`; `integrations/nghttp2/e2e/test_roundtrip.c` →
`integrations/http2/nghttp2/tests/` (role dirs land in R3, §1/§7).

### 9.4 Affected build / CI / fuzz / bench / script / doc / gate references

- **Makefile discovery (L329):** `TEST_SRC = $(filter-out …, $(wildcard tests/test_*.c))` →
  `$(wildcard tests/test_*.c tests/protocols/*/test_*.c)` (GNU Make 3.81: multiple wildcard patterns, one
  subdir level — no `**` needed). `TEST_BIN = $(TEST_SRC:.c=)` then yields nested paths automatically.
- **Pattern rule (L392):** add a sibling `tests/protocols/%$(EXE): tests/protocols/%.c $(LIB) $(TEST_COMPAT_OBJ)`
  (make 3.81 `%` spans `/`, so `%`=`http/test_http_router`).
- **Curated suite sets (L376 `WIN_TEST_SUITES`, L412 `WIN_IOCP_TEST_SUITES`, L663 `IOURING_TEST_SUITES`)** use
  bare suite names via `$(addprefix tests/test_,…)`. After the move a suite may live in a nested dir — and on
  Windows the binary needs the `$(EXE)` suffix, which resolving against the extensionless `TEST_BIN` does NOT
  produce. Freeze an explicit **source-path** resolver, then derive each platform's binary from it:
  ```make
  # resolve a bare suite name to its .c wherever it lives (flat tests/ or nested tests/protocols/<fam>/)
  test_src_for = $(filter tests/test_$(1).c tests/protocols/%/test_$(1).c,$(TEST_SRC))
  test_bin_for = $(patsubst %.c,%$(EXE),$(call test_src_for,$(1)))
  ```
  Use `test_bin_for` for `WIN_TEST_BIN`, `WIN_IOCP_TEST_BIN`, and `IOURING_TEST_BIN` (e.g.
  `IOURING_TEST_BIN = $(foreach s,$(IOURING_TEST_SUITES),$(call test_bin_for,$(s)))`). The **suite-name lists
  stay stable** (CI/scripts unchanged) and the `$(EXE)` suffix is correct on every platform. Add a **fail-loud
  uniqueness check**: each `$(call test_src_for,$(s))` must resolve to **exactly one** source (a make-time
  `$(if $(word 2,…),$(error dup suite $(s)))` / `$(if $(test_src_for…),,$(error unknown suite $(s)))`), so a
  duplicate basename or a typo'd suite fails the build rather than silently building the wrong/no target.
- **Smoke targets** (`smoke-iouring`, `smoke-pollcomp*`, `smoke-datagram`, `smoke-tls*`, `smoke-*-ws`, …) +
  the freestanding targets that name `tests/freestanding*` — repoint each moved smoke path in the target
  that builds it (per §9.1-G). Freestanding harness paths in group H do NOT move.
- **Clean (L777/L780):** `$(TEST_BIN)` already covers nested binaries; add nested globs
  `tests/protocols/*/test_* tests/protocols/*/*.exe tests/protocols/*/smoke_*` + `*.dSYM` to the clean rules.
- **Fuzz:** `fuzz/` stays flat (not `tests/*.c`); the fuzzers link `libkeel_fuzz.a` symbols + `fuzz/corpus_*`,
  none of which reference test paths → **no fuzz reference changes** from the test move. (The R2a–e source
  moves already repointed fuzz inputs.)
- **Bench:** the wrk bench server is not a `tests/*.c` → unaffected.
- **CI (`.github/workflows/…`):** any job invoking a suite by path (e.g. `./tests/test_*`) or a curated
  target must accept the nested paths; jobs that use the `make test`/`test-iouring`/`test-win*` targets are
  path-agnostic (the Makefile resolves paths) and need no edit. Enumerate + repoint the few path-literal
  invocations in the increment that moves the relevant family.
- **Docs / gates:** living docs that cite a moved test path; the boundary gates (§6) — see §9.6.

### 9.5 Discovery, naming, clean (frozen mechanics)

- **Recursive discovery:** one extra wildcard level (`tests/protocols/*/test_*.c`) — the tree is exactly one
  family level deep, so no `find`/`**` is needed; make-3.81-safe.
- **Collision-free executables:** every test basename (`test_<suite>` / `smoke_<name>`) is **globally unique**
  across `tests/` (the §9.2 splits are assigned family-prefixed unique basenames precisely to preserve this),
  so `test_src_for` returns exactly one source per suite and the flat CI suite-name lists stay valid. The
  §9.4 fail-loud uniqueness check + the §9.6 ownership gate enforce uniqueness so a future duplicate can't make
  the resolver ambiguous.
- **Clean:** nested object/binary/`.dSYM` globs added (above); `TEST_BIN`-derived removal already recursive.

### 9.6 Permanent gates (extend §6)

- **G-test-ownership — EXACT basename→expected-path map (reviewer P1).** A stale-*root* list alone only stops
  a suite returning to `tests/`; it would NOT catch `test_http2.c` misfiled under `tests/protocols/http/`, or
  an unknown family dir. So the gate freezes an **exact map of every test basename → its one canonical path**
  (the group-A–H homes above, including the split basenames) and scans the actual tree, rejecting:
  (a) any file whose real path ≠ its mapped path (mismatch — wrong family or wrong dir),
  (b) any basename found at two paths (duplicate / leftover copy),
  (c) any `tests/**/test_*.c` / `smoke_*.c` not in the map (unknown/unclassified placement),
  (d) any file under `tests/protocols/<x>/` where `<x>` is not one of the five frozen families.
  Mirrors `check-no-httplegacy`'s exact-name mechanism (self-canary included). This makes a protocol test
  unable to drift to the wrong home *and* a new/renamed test unable to land unclassified.
- **G-test-stale-path:** the retired `tests/test_<moved>.c` / `tests/smoke_<moved>.c` paths (and the pre-split
  `test_overflow.c` / `test_tls_set_hostname_fail.c`) must not reappear — a moved/split test cannot silently
  resurrect at its old path (no duplicate/second copy). Subsumed by (b)+(c) above but kept as an explicit
  moved-away list for clarity.
- Each gate lands/extends **in the same increment that performs the corresponding move** (§6.1 gate-timing
  rule); the full exact map is finalized in R4.

### 9.7 Increment sequencing (adapts §7; does NOT touch accepted R2a–R2f)

1. **This freeze revision — docs-only. Pause for review.** *(current)*
2. **T-split — the §9.2 splits, as ATOMIC transactions (runs BEFORE the family moves; reviewer P1).** A
   cross-family split cannot move one family at a time and stay green: leaving the original duplicates the
   already-moved cases; removing only one family's cases strands protocol-owned tests at the root (ownership-
   gate violation); retiring the original early temporarily drops the remaining cases. So each split is **one
   commit** that creates ALL its destination files AND deletes/retires the original together, updating
   discovery + the curated-suite resolution + the exact §9.6 ownership map in that same commit:
   - `test_overflow.c` → `test_http_overflow.c` (+http2/+websocket), **delete the original in the same commit.**
   - `test_tls_set_hostname_fail.c` → the three client-family files, **delete the original in the same commit.**
   These two CROSS-family splits are the dedicated **T-split** increment (cleanest before any family move). The
   two SINGLE-family splits stay atomic but are naturally carried by **T-http** (§9.2 remainder edited in place
   + the new HTTP file added in one commit): `test_async.c` → root remainder + `test_http_async.c`;
   `test_tls.c` → root remainder + `test_http_tls.c`. No case is ever duplicated, stranded, or dropped in any
   intermediate commit.
3. Then dedicated reviewable **per-family test-layout increments** (the impls already moved in R2a–e, so tests
   move on their own): **T-http, T-http2, T-ws, T-dns, T-proxy** — each moves its group-B–F TUs + the group-G
   smokes it owns, repoints discovery/pattern-rule/curated-set/smoke/CI refs, adds its slice of the §9.6 gates,
   and passes the full validation matrix. Behavior-neutral; white-box relative includes repointed to the new
   impl paths; no test weakened/skipped/duplicated. (T-http additionally performs the two single-family splits
   above.)
4. **Integration tests** move during **R3** with their owning integration (§9.3), under
   `integrations/<role>/<backend>/tests/`.
5. **R4** finalizes recursive discovery, the ownership + stale-path gates, clean rules, and doc reconciliation.

### 9.8 Classification rulings (RESOLVED by reviewer — no open decisions)

- **`test_read_flow_control`, `test_timeout`, `test_alpn` → `tests/protocols/http/`.**
- **`smoke_tcp` → `tests/protocols/http/`.**
- **`smoke_tls`, `smoke_tls_completion` → `integrations/tls/mbedtls/tests/`** — both explicitly validate the
  mbedTLS backend (move in R3 with the TLS integration).
- **Completion-backend smokes STAY in `tests/`**, including the IOCP/pollcomp **TLS** and **async** variants —
  HTTP + identity/mock TLS is their vehicle, the completion/event backend axis is their contract (rule 5).
- **`smoke_iouring_client`, `smoke_pollcomp_client` STAY in `tests/`** — their documented contract is the
  **neutral completion-connect path**, despite using `KlHttpClient` as the driver.
- **Freestanding harness set + shared support (group H) stay flat in `tests/`.** Do **not** introduce a
  `tests/support/` dir.
