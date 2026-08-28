# F2-4 public-function coverage gaps (prioritized)

Genuine behavioral-coverage gaps found while classifying every public function in
`docs/f2/function_coverage.tsv`. These are the functions classified `intentional-untested`
(no behavioral coverage) or `example` (exercised only by an examples program that never runs under
test), plus a short secondary list of functions that ARE executed but whose own result is not directly
asserted (`indirect-execution`). This is the input to the later gap-filling increment; it is NOT a
to-do for this commit (the deliverable here is the classified manifest and the gate).

Conservatism note: a function classified `direct-assertion` / `indirect-execution` is genuinely
exercised; it is not listed here just to pad. Only real holes appear below.

## Priority 1 (high): zero coverage, real behavioral hole

- `kl_datagram_on_writable` (datagram.h) - the send-writable signal registration hook has ZERO test or
  example references anywhere. Missing: register an on_writable callback, drive the send queue to
  WOULD_BLOCK, then assert the callback fires when the socket becomes writable.

## Priority 2 (medium): public path with no asserted coverage

- `kl_http2_server_upgrade_from_h1` (http2_server.h) - the HTTP/1 -> HTTP/2 (h2c prior-knowledge /
  Upgrade) server path is untested; only the ALPN/direct upgrade is covered. A regression would
  silently break cleartext h2 upgrade.
- `kl_ws_server_close` (websocket_server.h) - server-initiated WebSocket close is entirely untested:
  close-frame emission, status codes, and server-side teardown are unverified.
- `kl_http_router_dispatch_synthetic` (http_router.h) - the public synthetic-dispatch entry point has
  no test driving a synthetic request through the router (assert the handler runs and a response is
  produced).
- CLOSED (F2-5) `kl_sockaddr_is_multicast` (sockaddr.h) - direct test added
  (tests/test_sockaddr.c is_multicast: IPv4 224.0.0.0/4 + IPv6 ff00::/8 positive/boundary/negative +
  NULL); manifest promoted to direct-assertion.

## Priority 3 (low): accessor/advisory with only transitive or example coverage

- `kl_http_request_peer_sockaddr` (http_server.h) - the raw KlSockAddr* peer accessor is untested (the
  string form `kl_http_request_peer_addr` is covered). Add a family + address-bytes assertion.
- `kl_http_conn_peer_addr` (http_connection.h) - connection-level peer-address accessor has no direct
  test (only the request-level wrapper). A one-line accessor assertion closes it.
- `kl_platform_caps` (http_server.h) - advisory capability bitmask is untested; assert
  KL_PLATCAP_PEER_CRED is set on POSIX (where peer-cred is proven) for self-consistency.
- `kl_ws_server_send_ping` (websocket_server.h) - server-originated ping (server-driven keepalive) has
  no caller; pair with a client that asserts the pong.
- `kl_ws_server_send_binary` (websocket_server.h) - only exercised by `examples/websocket_server.c`
  (never run under test) and only as an echo; no test asserts a server binary frame is received intact.
- `kl_http_sse_end` (http_sse.h) - only covered by `examples/sse.c`; the SSE unit test uses
  `kl_http_response_end_stream` instead, so the SSE-specific end path has no asserted coverage.
- `kl_watcher_rearm` (event_ctx.h) - the WOULD_BLOCK re-arm helper is referenced only in harness
  comments; add a watcher test that rearms after a would-block and confirms the next readiness fires.
- `kl_socket_provider_winsock` (socket.h) - reached only from a Windows-gated compile helper; add a
  smoke assertion (provider non-NULL, OVERLAPPED/native caps) in the Windows IOCP/WSAPoll gate.
- `kl_req_memeq` / `kl_req_strlen` (http_request.h) - header-inline primitives covered only through
  header/param lookups; cheap direct byte-compare/length cases (n=0, embedded NUL, mismatch) would lock
  the ASCII-exact contract independently.
- `kl_monotonic_ms` (clock.h) - used only as an opaque deadline source; a minimal
  "two reads are non-decreasing and advance across a short sleep" assertion would pin the clock
  contract.

## Secondary: executed but own result not directly asserted (indirect-execution hardening)

These are covered (their downstream effect is asserted) but a direct assertion on the function's own
result would harden them: `kl_datagram_recv_stop`, `kl_stream_read_close`, `kl_stream_pause`,
`kl_drain_free`, `kl_connect_op_cancel`. Low priority; listed for completeness, not a required fix.
