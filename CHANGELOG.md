# Changelog

All notable, user-visible changes to Keel are recorded here. The format follows Keep a Changelog, and
Keel follows Semantic Versioning (the compatibility contract is in `docs/contracts/compatibility.md`).

## [Unreleased]

No changes yet.

## [3.0.0-rc.2]

Release candidate; no release date (the tag and prerelease are a separately authorized step).
Supersedes 3.0.0-rc.1 and restarts the release-candidate window.

### Fixed

- Streaming-async routes are usable from outside the tree again. `kl_http_server_route_streaming_async`
  / `kl_http_router_add_streaming_async` require the handler to "yield on NEED_DATA," but the only
  mechanism was a direct `conn->state = KL_HTTP_CONN_READING_BODY` write, which the F2 `KlHttpConn`
  opacity made internal with no public replacement (the streaming dispatch otherwise defaults a
  returning handler to sending a response, ending the stream). Added `kl_http_request_await_body(req)`
  (`keel/http_request.h`) as the public yield signal, so a streaming-async handler can park for more
  body without reaching into the now-private connection header.

### Added

- `kl_http_request_await_body(const KlHttpRequest *req)` - park a streaming-async handler for more
  request body. Orthogonal to `kl_http_request_pause_body` / `_resume_body` (flow control within the
  body-reading state); this selects that state.

## [3.0.0-rc.1]

Release candidate; no release date (the tag and prerelease are a separately authorized step). 3.0.0 is
a major version: it renames most of the public HTTP surface and moves backend-specific headers out of
the installed set. The step-by-step source migration is in `docs/migrations/2.x-to-3.0.md`. Names below
were verified against the v2.9.0 tree (old) and the current tree (new).

### Breaking

- Public HTTP headers were re-prefixed: `keel/server.h` -> `keel/http_server.h`,
  `keel/client.h` -> `keel/http_client.h`, `keel/connection.h` -> `keel/http_connection.h`,
  `keel/router.h` -> `keel/http_router.h`, `keel/request.h` -> `keel/http_request.h`,
  `keel/response.h` -> `keel/http_response.h`, `keel/cors.h` -> `keel/http_cors.h`,
  `keel/sse.h` -> `keel/http_sse.h`, `keel/redirect.h` -> `keel/http_redirect.h`,
  `keel/client_pool.h` -> `keel/http_client_pool.h`, `keel/body_reader.h` -> `keel/http_body_reader.h`,
  `keel/body_reader_multipart.h` -> `keel/http_body_reader_multipart.h`,
  `keel/chunked.h` -> `keel/http1_chunked.h`, `keel/parser.h` -> `keel/http1_parser.h`,
  `keel/h2.h` -> `keel/http2.h`, `keel/h2_client.h` -> `keel/http2_client.h`,
  `keel/h2_server.h` -> `keel/http2_server.h`. The `keel/keel.h` umbrella still covers the full surface.
- Public HTTP types and functions were renamed to the `KlHttp*` / `KlHttp1*` / `KlHttp2*` and
  `kl_http_*` taxonomy (for example `KlServer` -> `KlHttpServer`, `KlConfig` -> `KlHttpServerConfig`,
  `KlClient` -> `KlHttpClient`, `KlConn` -> `KlHttpConn`, `KlResponse` -> `KlHttpResponse`,
  `kl_server_init` -> `kl_http_server_init`, `kl_response_json` -> `kl_http_response_json`). A gate
  keeps any 2.x name from surviving in the tree.
- `KlEventLoop.fd` was removed; the descriptor is backend-owned and the event ops take a pointer-width
  `KlSocketHandle` (`keel/handle.h`) instead of `int`.
- Several types are now opaque on the public surface (`KlHttpConn`, `KlWatcher`, `KlTimerEntry`,
  `KlHttpClientPoolEntry`, `KlHttpMiddlewareEntry`, `KlWsServerConn`); use the accessors and management
  APIs instead of reaching into fields (for example `kl_http_conn_response(conn)`).
- Caller-supplied allocators, providers, and vtables are now validated at their public boundary and
  rejected with `-1` / `KL_ERR_INVALID_ARG` when malformed, instead of being accepted silently or
  crashing later.

### Removed

- Backend-specific headers are no longer installed: `keel/tls_mbedtls.h`, `keel/compress_miniz.h`,
  `keel/decompress_miniz.h`. The backend-neutral vtables live in `keel/tls.h`, `keel/http_compress.h`,
  and `keel/decompress.h`; the concrete adapters live under `integrations/` and are added to your build
  explicitly.

### Added

- Tier-1 transport axis: `keel/stream.h` (`KlStream`), `keel/datagram.h` (`KlDatagram`),
  `keel/listener.h` (`KlListener`), with opt-in, unstable layouts in matching `*_detail.h` headers.
- Provider/socket surface: `keel/socket.h` (`KlSocketProvider`), `keel/sockaddr.h`
  (address-ABI-neutral `KlSockAddr`), `keel/handle.h` (`KlSocketHandle`), `keel/net.h`, `keel/clock.h`,
  `keel/connect_op.h`.
- A built-in async DNS resolver over the datagram primitive (`keel/dns_resolver.h`).
- A freestanding client/protocol header subset (`keel/freestanding.h`).
- Additional backends and platforms alongside the existing Linux/macOS readiness engines: an io_uring
  completion backend, a portable poll()-based completion double, Windows (Winsock, WSAPoll, IOCP,
  MinGW), UEFI (EFI_TCP4/EFI_UDP4), lwIP (BSD and raw NO_SYS), and Cosmopolitan.
- A single-source version mechanism: a root `VERSION` file feeds `keel/version.h`, `keel.pc`, and the
  SBOM through a generator, enforced by a default-deny version-drift gate.

### Changed

- Every installed `keel/*.h` header now compiles standalone under C11 and C++11 with strict flags and
  guards its declarations with `extern "C"`, so the C API is directly consumable from C++11 (there is
  no separate C++ API). This is enforced in CI.
- Installed headers are exactly a reviewed manifest (not a wildcard); internal headers are never
  installed. The `keel` pkg-config module name is unchanged and its `Version` is now single-sourced.
- `KL_VERSION_STRING` is derived from the single source; `KL_VERSION_NUMBER` stays numeric
  (`major*10000 + minor*100 + patch`) and a prerelease appears in `KL_VERSION_STRING` and
  `KL_VERSION_PRERELEASE`.

### Security

- The W^X posture is explicit and gated: `keel/keel.h` forbids `dlopen`, JIT, and runtime dynamic code;
  no backend uses dynamic loading or global mutable registration.
- Untrusted-input parsers are covered by libFuzzer targets (HTTP request/response, chunked, multipart,
  WebSocket frames, DNS responses, PROXY protocol, URL), and the malformed-vtable/allocator rejections
  above turn previously-undefined inputs into clean, fail-closed errors.

### Deprecated

- None.

### Fixed

- This candidate consolidates the fixes made across the development series since v2.9.0; individual
  fixes are recorded in the Git history. No behavioral regression relative to v2.9.0 is intended beyond
  the breaking changes listed above.

## Prior releases

Detailed per-release notes were not maintained before 3.0; the release history is the Git tags below
(dates are the tag commit dates). This changelog begins tracking notable changes at 3.0.0-rc.1.

| Tag | Date |
|-----|------|
| v2.9.0 | 2026-07-18 |
| v2.8.0 | 2026-07-17 |
| v2.7.1 | 2026-06-21 |
| v2.7.0 | 2026-06-21 |
| v2.6.3 | 2026-06-21 |
| v2.6.2 | 2026-06-20 |
| v2.6.1 | 2026-06-19 |
| v2.6.0 | 2026-06-19 |
| v2.5.1 | 2026-06-19 |
| v2.5.0 | 2026-06-19 |
| v2.4.0 | 2026-06-19 |
| v2.3.3 | 2026-06-19 |
| v2.3.2 | 2026-06-19 |
| v2.3.1 | 2026-06-19 |
| v2.3.0 | 2026-06-19 |
| v2.2.1 | 2026-06-18 |
| v2.2.0 | 2026-06-05 |
| v2.1.2 | 2026-06-04 |
| v2.1.1 | 2026-06-02 |
| v2.1.0 | 2026-06-02 |
| v2.0.0 | 2026-06-01 |
