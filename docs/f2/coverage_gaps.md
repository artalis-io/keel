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

- CLOSED (F2-5) `kl_datagram_on_writable` (datagram.h) - direct tests added
  (tests/test_datagram_public.c): the callback fires exactly once on the send-queue full->non-full edge
  with the WOULD_BLOCK->ACCEPTED state transition observable; no spurious notification when the queue
  never fills; a non-edge drain does not re-fire while a distinct edge fires again. Manifest promoted to
  direct-assertion.

## Priority 2 (medium): public path with no asserted coverage

- CLOSED (F2-5) `kl_http2_server_upgrade_from_h1` (http2_server.h) - direct tests added
  (tests/protocols/http2/test_http2.c): success emits the 101 Switching Protocols response and wires the
  session (HTTP2), and a malformed session vtable is rejected (CLOSED) with the session destroyed and
  conn.h2 left NULL (ownership cleanup, no dangling state). Manifest promoted to direct-assertion.
- CLOSED (F2-5) `kl_ws_server_close` (websocket_server.h) - direct tests added
  (tests/protocols/websocket/test_ws_server_close.c): the emitted CLOSE frame (opcode/length/status
  code/reason) is captured and asserted, plus idempotence (second close emits nothing), the code-0 empty
  payload, and invalid-state handling (NULL conn rejected; sends refused after close). Manifest promoted
  to direct-assertion.
- CLOSED (F2-5) `kl_http_router_dispatch_synthetic` (http_router.h) - direct tests added
  (tests/protocols/http/test_http_router.c router_synthetic: matched=200 + handler effect, unmatched=404,
  method-mismatch=405, pre-body middleware short-circuit, pre+post pass-through -> handler, invalid=-1);
  manifest promoted to direct-assertion.
- CLOSED (F2-5) `kl_sockaddr_is_multicast` (sockaddr.h) - direct test added
  (tests/test_sockaddr.c is_multicast: IPv4 224.0.0.0/4 + IPv6 ff00::/8 positive/boundary/negative +
  NULL); manifest promoted to direct-assertion.

## Priority 3 (low): closed cheaply, or rationale preserved

Closed in F2-5 (cheap, deterministic, direct assertions added):
- CLOSED `kl_http_conn_peer_addr` (http_connection.h) - tests/protocols/http/test_http_connection.c
  (connection.response_accessor): returns &stream.peer_addr and reads back family + port.
- CLOSED `kl_ws_server_send_ping` (websocket_server.h) - tests/protocols/websocket/test_ws_server_close.c
  (ws_server_send.emits_ping_frame): PING opcode + payload asserted; over-125 control payload rejected.
- CLOSED `kl_ws_server_send_binary` (websocket_server.h) - same file
  (ws_server_send.emits_binary_frame): BINARY opcode + payload asserted.
- CLOSED `kl_http_sse_end` (http_sse.h) - tests/protocols/http/test_http_sse.c
  (sse.end_terminates_stream): emits the zero-length chunked terminator; NULL rejected.

Rationale preserved (genuinely advisory, platform-only, or not cheap to test deterministically; left
intentional-untested / compile-only, not padded):
- `kl_http_request_peer_sockaddr` (http_server.h) - the raw KlSockAddr* peer accessor needs a live
  accepted connection carrying a peer address; not a cheap unit assertion. The string form
  `kl_http_request_peer_addr` is directly covered.
- `kl_platform_caps` (http_server.h) - advisory capability bitmask; its exact bits are platform-defined,
  so a direct assertion would only restate the platform, not verify behavior.
- `kl_watcher_rearm` (event_ctx.h) - the WOULD_BLOCK re-arm helper requires driving a real would-block
  edge through the event loop; not a cheap deterministic case, deferred.
- `kl_socket_provider_winsock` (socket.h) - Windows-only; reached from a Windows-gated compile helper
  and cannot run on the primary (POSIX) CI. Belongs to the Windows IOCP/WSAPoll gate.

## Secondary: executed but own result not directly asserted (indirect-execution hardening)

These are covered (their downstream effect is asserted) but a direct assertion on the function's own
result would harden them: `kl_datagram_recv_stop`, `kl_stream_read_close`, `kl_stream_pause`,
`kl_drain_free`, `kl_connect_op_cancel`, and the header-inline primitives `kl_req_memeq` /
`kl_req_strlen` / `kl_monotonic_ms` (exercised through the request/deadline paths). Low priority; listed
for completeness, not a required fix.
