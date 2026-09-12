# Changelog

All notable, user-visible changes to Keel are recorded here. The format follows Keep a Changelog, and
Keel follows Semantic Versioning (the compatibility contract is in `docs/contracts/compatibility.md`).

## [Unreleased]

### Added

- `KlHttpServerConfig.reject_drain_disable`. Setting it to 1 turns the post-rejection drain off; it is
  now the only way to do that, and it overrides both numeric fields.

### Fixed

- **The documented way to disable the post-rejection drain was the configuration that enabled it.**
  3.0.0's header and contract both said that zeroing `reject_drain_max_bytes` and
  `reject_drain_timeout_ms` disabled the drain. `kl_http_server_init` applied the defaults exactly when
  both were zero, so zeroing both ENABLED it at 64 KiB / 500 ms, while zeroing exactly one disabled it
  as an undocumented side effect. The two fields are now normalised independently, so a zero field
  selects that field's default as `KlHttpServerConfig` promises for every member, and disabling is
  explicit. Embedders who leave the fields alone, including anyone zero-initialising the struct, are
  unaffected: they got the defaults before and still do.

## [3.0.0]

First stable 3.0 release. No release date here (the tag and publish are a separately authorized step).
Supersedes 3.0.0-rc.1 through rc.3; everything in those entries is included.

The release-candidate window was spent on correctness rather than features, mostly on Windows, where
making previously-unrunnable suites runnable is what exposed the defects. The Windows readiness subset
went from 51 to 71 suites and the IOCP completion subset from 2 to 66.

### Added

- `KlWakeup` (`keel/wakeup.h`), the public cross-thread event-loop wakeup channel: a connected handle
  pair whose `.rd` registers as a `KlWatcher` and whose `.wr` any thread may signal, so a foreign
  thread can reach `kl_async_complete` (which is loop-thread-only) safely. A pipe where pipes are
  watchable, a loopback socket pair where they are not (WSAPoll and IOCP watch sockets only), so one
  piece of caller code runs on every backend. `KlThreadPool` uses it internally; callers use it
  directly when the completion signal comes from somewhere the pool does not own.
- `KlShutdownHow` (`KL_SHUT_RD` / `_WR` / `_RDWR`) and `KlSocketOps.shutdown`, appended to the socket
  provider vtable. A Keel spelling rather than `SHUT_WR` / `SD_SEND`, so the seam carries no platform
  constant and a provider with no half-close concept (lwIP raw, EFI_TCP4) can interpret or refuse it.
  A NULL slot selects the built-in native shutdown, as with every other op.
- `KlHttpServerConfig.reject_drain_max_bytes` and `.reject_drain_timeout_ms`, with
  `KL_HTTP_SERVER_DEFAULT_REJECT_DRAIN_BYTES` (64 KiB) and `..._MS` (500 ms). They bound the
  post-rejection drain described under Changed. Setting both to 0 disables it and restores the previous
  teardown.
- `docs/contracts/early_rejection_drain.md`, the authoritative contract for teardown after a final
  response when request-body data may still be arriving, including the explicit security boundary: if a
  peer keeps transmitting past the configured budget, Keel may terminate the connection even though
  that prevents reliable delivery of the response. Bounded resource consumption wins over delivery.
- `KEEL_OPT` and `KEEL_EXTRA_CFLAGS` / `KEEL_EXTRA_LDFLAGS` build hooks for embedders, applied with
  override appends so they survive the debug and coverage sub-makes.

### Changed

- **An early final response is no longer destroyed by its own teardown.** A server that answers early
  (413, 431, 415, 500, a 408 on a stalled upload, or a handler that simply stops reading) left the rest
  of the request body unread, and closing a socket with unread received data makes TCP send RST, which
  discards data the peer had already buffered. The client saw a reset instead of the response
  explaining why. Keel now flushes the response, half-closes its send side, and drains inbound bytes
  asynchronously (one bounded read per loop progression, never a blocking read-until-empty), bounded by
  both caps above. Entry is gated on unread input remaining, not on the call site, so ordinary
  keep-alive is untouched. Body completion comes from the real framing, not a byte count, so a chunked
  upload ends on its terminal chunk rather than sitting out the deadline.
- `max_align_t` no longer appears in a public header, so MSVC consumers can compile the installed
  headers.
- Objects are built under `build/<backend>/` and the archive records which backend produced it, so
  switching `BACKEND=` can no longer mix incompatible objects into one `libkeel.a`. Affects the build
  only, not the library.

### Fixed

- The post-rejection drain state was mis-handled at five completion-axis and async sites that treated
  anything other than keep-alive as "close now", which closed connections on top of unread bytes and
  destroyed responses that had already been written. `KlHttpConnState` dispatch is now exhaustive and
  compiler-enforced (no `default:`, so a new state fails to build until every dispatcher handles it),
  with a CI gate keeping the `default:` out.
- The drain is bounded by the configured cap alone. It was `min(declared Content-Length remainder,
  cap)`, which put an over-sending peer's excess outside the budget by construction, and it skipped
  the drain entirely when the declared framing was complete even with bytes still queued.
- On IOCP, a watcher probe was re-armed before its completion had been dispatched, so a still-readable
  socket delivered the same readiness twice; a readiness backend reports a level-triggered fd once per
  wait. Re-arming now happens after dispatch, and `kl_event_del` plus teardown understand the resulting
  state in which an op is tracked but owns no kernel I/O.
- On IOCP, `kl_watcher_mod` discarded the interest mask, so a watcher that switched to
  `KL_EVENT_WRITE` never fired again and the async HTTP client stalled on a completion loop.
- `kl_datagram_open` binds the wildcard on an ephemeral port when the caller supplies no bind address.
  POSIX lets a receive wait on an unbound UDP socket; Windows refuses a POSTED receive on one, so the
  built-in DNS resolver could not start on IOCP. Best effort: only a bind the caller asked for is
  fatal, so a provider with no bind concept is unaffected.
- A Winsock datagram socket reports its capabilities when UNBOUND instead of reporting none.
- A freed connection pool no longer claims its old capacity, which made a server that outlived its
  pool sweep freed slots.
- `kl_async_complete` sends the response when `on_resume` leaves the connection in the processing
  state, and arms READ when it leaves it draining.

## [3.0.0-rc.3]

Release candidate; no release date (the tag and prerelease are a separately authorized step).
Supersedes 3.0.0-rc.2 and restarts the release-candidate window.

### Fixed

- Streaming-async completion is usable from outside the tree. rc.2 restored the "keep reading" half of
  the streaming-async contract (`kl_http_request_await_body`), but the symmetric "handler done, send the
  response" half was still missing. A streaming-async handler resumed from the body reader's `on_data`
  callback (rather than via `kl_async_complete`) has no `KlAsyncOp` to complete and is not re-invoked by
  the dispatch, which returns the connection state as the handler left it (`streaming_async` returns
  `c->state` verbatim). With `KlHttpConn` opaque after F2, there was no public way to leave the
  connection in the sending state, so a completed handler stayed in the body-reading state and the
  request timed out (408) instead of sending its response (including error responses such as 413). Added
  `kl_http_request_send_response(req)` (`keel/http_request.h`) as the public send-side signal.

### Added

- `kl_http_request_send_response(const KlHttpRequest *req)` - the send-side partner to
  `kl_http_request_await_body`: a streaming-async handler resumed from `on_data` calls it after building
  its response on `kl_http_conn_response()` to transition the connection to sending. It does not itself
  flush (unlike `kl_http_response_send`); the existing write machinery performs the send, handling
  partial writes / would-block.

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
