# Changelog

All notable, user-visible changes to Keel are recorded here. The format follows Keep a Changelog, and
Keel follows Semantic Versioning (the compatibility contract is in `docs/contracts/compatibility.md`).

## [Unreleased]

### Security

- **An HTTP/2 stream closed by the flush that sends its response is released once.** The HTTP/2
  server flushed a stream's response while it still held the stream and released it afterwards. A
  session that really sends on that flush closes the stream once its END_STREAM is out and reports
  the close from inside the flush, which released the stream first; the stream was then released
  again, its count went to -1, and the next stream was written before the stream table (heap
  corruption). The nghttp2 adapter sends on that flush for stream 1 of an h2c `Upgrade`, so any
  plaintext client could trigger it with an upgrade followed by a second request. Each answer now
  releases the stream before it flushes (the session copies the response at submit), and releasing
  a slot that is not live is refused (and asserts where asserts are on).
  **Behavior change for `KlHttp2ServerSession` implementations:** the header name/value and body
  pointers passed to `submit_response` are borrowed for that call only and are freed before the
  following `flush`, so a session must copy whatever it sends later (the bundled nghttp2 adapter
  already does). `flush` may report a stream it closes while sending (`on_stream_reset`) from
  inside the call. Both rules are now stated in `<keel/http2_server.h>`.

- **A protocol-relative redirect against a `+unix` base is refused (behavior change).**
  `kl_url_resolve` copied the base scheme onto a `//host...` Location, so against an
  `http+unix://` or `https+unix://` base the host became a socket path (`//name` resolved to
  `http+unix://name`, a relative path), letting a redirect send the next request to a different
  local socket. A protocol-relative Location against a `+unix` base now returns -1, which the
  redirect client reports as a redirect failure, like any Location it cannot resolve.
- **The chunked request decoder rejects bare LF and control bytes in extensions and trailers, and
  caps their length (behavior change).** It skipped a chunk extension up to the next CR and a trailer
  line up to its CRLF, accepting any byte on the way, with no length limit (those bytes reach no
  body reader and count toward no body limit, so only the body deadline bounded them). A front end
  that ends a chunk-size line at a bare LF frames the body differently, the chunk-extension request
  smuggling class. A bare LF or a control byte other than HTAB in an extension or a
  trailer line is now a malformed chunk (413, as before for malformed framing), as is an extension
  list longer than `KL_HTTP1_CHUNK_EXT_MAX` (4096 bytes) or a trailer section longer than
  `KL_HTTP1_CHUNK_TRAILER_MAX` (8192 bytes). **ABI change:** the public `KlHttp1ChunkedDecoder`
  struct grew a field (`meta_len`), so its size changed: any external code that embeds the struct
  or allocates it by size must be rebuilt against the new header. The HTTP client parses responses with llhttp, not this
  decoder, and is unchanged.
- Async HTTP client errors remain deferred under allocation failure, so completion callbacks may
  safely free the client. Requests fail to start if their deadline timer cannot be reserved.
- HTTP readiness reads preserve connections on would-block and retry interrupted reads instead of
  reporting a terminal header/body failure. EOF and transport errors still close normally.
- Protocol capability hooks are published atomically across independent server threads; completion
  installation no longer uses an unsynchronized process-wide sentinel.
- Runtime completion providers missing their completion table or required drain operation are
  rejected at installation, with their initialized backend closed once.

- Plaintext completion WebSockets now flush large outbound drains in bounded pieces. A frame or
  accumulated backlog larger than the 1 MiB transport-queue allowance previously stalled after
  its header, even when the peer was reading. The queue allowance remains enforced.

- HTTP/2 client requests and nghttp2 adapter submissions reject invalid header counts and missing
  header/body pointers before allocation, preventing signed overflow in header-array sizing.

- Thread-pool destruction from a `done_fn` now waits until the callback returns, preventing a
  use-after-free in callback dispatch. Teardown callbacks may repeat the free request safely,
  and submissions after destruction is requested are refused.
- **Completion server: a posted send is never moved before it completes.** The output queue
  compacted and reallocated its buffer while a send posted from it was in flight. A backend that
  reads a posted send in place until it completes (the lwIP raw integration, which `completion.h`
  allows) then sent moved or freed memory. The posted bytes now stay where they are until their
  completion. The queue also never posts more than a backend takes: on the EFI integration, which
  refuses any send larger than its 16 KiB buffer, TLS, streamed and WebSocket output now goes out
  in pieces it accepts (a buffered plaintext response is still one send there, as documented).

- **The URL parser no longer takes a host from outside the authority or from userinfo (behavior
  change).** The closing `]` of an IPv6 literal was searched for in the whole URL, so
  `http://[a/b?c]` parsed with the host `a/b?c`, and anything could sit between the brackets. An `@`
  was kept as part of the host, so `http://user@example.com/` asked DNS for `user@example.com`, and a
  check of the parsed host saw a different name than a reader of the URL. The `]` is now looked for
  inside the authority only (before the first `/`, `?` or `#`), and only an address may sit between
  the brackets: hex digits, `:` and `.`, with an optional zone after `%`. Userinfo is not supported
  in a request URL, so `kl_url_parse` now rejects an `@` in the authority; an `@` in the path or
  query is unchanged. Proxy credentials are configured on `KlHttpProxyConfig`, not in the URL.
- **mbedTLS: a client context with no CA bundle no longer skips certificate verification
  (behavior change).** `kl_tls_mbedtls_client_ctx_create(NULL, alloc)` built a client that accepted
  any certificate, where the OpenSSL adapter's NULL means the system trust store. A caller that
  forgot the CA path got a silently unauthenticated client, open to impersonation. mbedTLS has no
  system store to fall back on, so a NULL path now returns NULL. A client that verifies nothing (for
  tests against self-signed loopback servers, the equivalent of `curl -k`) must be asked for by name:
  `kl_tls_mbedtls_client_ctx_create_insecure(alloc)`. Every in-tree caller that relied on NULL
  (examples/tls_client.c, the nghttp2, lwIP and UEFI tests, the smoke tests) now uses it, and the
  example verifies against a CA bundle when one is given.
- **A 32-bit build truncated a large Content-Length, so a request body could be read as the next
  request.** The HTTP/1 parser counts Content-Length in 64 bits and stored it in the request's
  `size_t` field: on a 32-bit build `Content-Length: 4294967296` read as 0, so the server took the
  body for a pipelined request (a smuggling vector behind a 64-bit proxy that framed the message
  correctly). A Content-Length that does not fit `size_t` is now a parse error, and the connection is
  closed as for any malformed request. 64-bit builds are unchanged.
- **A multipart parameter could be taken from inside a quoted value.** The multipart reader found
  `name=`, `filename=` and `boundary=` anywhere after a `;` or a space, including inside a quoted
  string, so `filename="x; name=evil"; name="real"` gave the field the name `evil` instead of `real`,
  letting one part pose as another form field. Parameters are now recognised only outside quoted
  strings, and a quoted string honours backslash escapes (`\"` does not end it), both when looking
  for a parameter and when reading its value. A quoted value with no closing quote is malformed.
- **Entropy fell back to a guessable generator when `/dev/urandom` could not be opened.** On Linux
  the random source opened `/dev/urandom` on every fill, and when that failed (a process out of
  descriptors, a chroot without `/dev`) it filled the buffer from the clock and process id instead.
  Those bytes become DNS transaction ids, 0x20 case patterns and cookies, and WebSocket mask and
  handshake keys, so an off-path attacker who could exhaust the server's descriptors could predict
  them. Linux now reads `getrandom(2)` first, which needs no descriptor, then `/dev/urandom`; if
  neither yields entropy the fill fails (Windows likewise, if `BCryptGenRandom` fails) and the
  caller refuses the operation: the DNS query is not sent and the lookup fails, and the WebSocket
  connect or frame send returns an error. There is no weak fallback any more.
- **Binding an AF_UNIX server socket with a mode changed the process umask.** To create the socket
  node with `unix_socket_mode`, the bind set the process-wide umask and restored it afterwards, so a
  file another thread created in that window got the wrong permissions. The umask is no longer
  touched: the node is created under the process umask and set to the exact mode with `fchmodat`
  before the socket listens, so nothing can connect while its mode is still the default.
- **PROXY protocol v1: the destination address and ports were not validated.** Only the source
  address and port were checked, the port with `strtol`, so a header with a bad destination
  address, a destination of the wrong family, a destination port of 70000, a signed (`+80`, `-0`)
  or overlong (`000080`) port, or a seventh field was accepted and its source trusted. Every field
  is now validated: both addresses must be literals of the header's family, and both ports one to
  five digits in 0-65535, with exactly six fields.

- **WebSocket server: a dead peer held its connection forever.** With `ping_interval_ms` set, the
  server pinged but never expected an answer, so a peer that vanished without a reset (behind a NAT,
  or a client that lost power) kept its slot for good. A peer that sends nothing at all, PONG or any
  other frame, for a whole interval after a ping is now failed: the server sends Close 1001 and
  closes the connection at once. `ping_interval_ms = 0` (the default) still means no pings and no
  liveness check.
- **WebSocket server: pings from a client that never reads grew server memory without bound.** With
  the drain enabled and no `max_size` (unlimited), every ping queued a PONG behind the output the
  client was not reading. While output is backed up, the server now keeps only the latest ping and
  answers it once the drain empties (RFC 6455 allows answering only the most recent ping).
- **HTTP/2: peer-driven resource limits, idle connections and graceful shutdown.** The nghttp2
  server adapter sent empty SETTINGS, so a peer could open streams without limit, and it copied every
  request header with no count or size cap: one HPACK table entry referenced again and again (an
  HPACK bomb) cost hundreds of MB per stream. It now advertises MAX_CONCURRENT_STREAMS and
  MAX_HEADER_LIST_SIZE (64 KiB) and resets a stream past `KL_MAX_HEADERS` fields or that size; the
  client adapter caps response headers the same way and refuses server push. An HTTP/2 connection
  with no stream open and nothing to send is now timed out by the idle sweep (it held its slot
  forever). A graceful shutdown now lets responses in flight finish (it cut them once its GOAWAY was
  out), and a response that cannot be submitted resets its stream instead of leaving it open.
  `KlHttp2ServerCallbacks` gains `max_concurrent_streams`, `initial_window_size` and
  `max_header_list_size`, which KEEL fills from `KlHttp2ServerConfig` (whose `initial_window_size`
  was never used).
- **TLS adapters: large completion-mode writes, drain retries, truncation and mTLS resumption.**
  With the OpenSSL adapter (and BoringSSL, LibreSSL) on io_uring, IOCP, pollcomp, lwIP-raw or UEFI,
  any write past the 256 KiB completion ring failed instead of waiting, so a buffered HTTPS response
  body over about 250 KiB was dropped. A write retried the way the drain buffer retries it (from its
  own copy, with more appended) failed with `BAD_WRITE_RETRY` on OpenSSL and, on mbedTLS, was
  acknowledged for bytes never encrypted, silently corrupting a wss frame stream or an HTTPS SSE
  stream. mbedTLS reported a bare TCP close as a clean TLS shutdown, so a truncated close-delimited
  response was accepted as complete. The OpenSSL mTLS server rejected resumed sessions. All four are
  fixed, and both adapters' end-to-end suites now run in CI.
- **The blocking HTTP client could wait forever on a server that stopped answering.** The sync client
  (`kl_http_client_request[_s]`, `kl_http_client_request_pooled`) put the socket back into blocking
  mode after connecting and then ran the TLS handshake, TLS reads and sends on it, so `timeout_ms`
  bounded only the waits between those calls. A server that accepted and never sent a ServerHello,
  sent part of a TLS record, or stopped reading a large upload blocked the caller with no limit. The
  socket now stays non-blocking for the whole request, and every step (connect, handshake, send,
  receive) waits only for the time left before the request deadline; running out of it fails the
  request with `KL_ERR_TIMEOUT`.
- **Completion loops: plaintext output never blocks the loop.** On io_uring, IOCP and the other
  completion engines, plaintext output did not use the ordered output queue that TLS output goes
  through. A streamed response (chunked, SSE, compressed), WebSocket frames and HTTP/2 frames
  written outside a feed (an h2c upgrade's first response, a drain's GOAWAY) were sent with a
  synchronous send on the loop thread. On io_uring, where accepted sockets are blocking, one client
  that stopped reading stalled every connection on the loop. On pollcomp such a send could overtake a
  send already posted and reorder the output. All of it now goes through the connection's output
  queue: one ordered overlapped send at a time, never a send on the loop thread. A plaintext HTTP/2
  connection that the session ends now sends its GOAWAY before closing (it was dropped), and over TLS
  a `100 Continue` is sent before the body is read (it stayed in the engine until the final
  response, so a client that waits for it before sending the body waited for nothing).
- **DNS: an answer is accepted only from the nameserver its query was sent to.** With several
  nameservers configured, a reply was matched to its query by transaction id and checked against
  whichever nameserver its source address named, not the one the query went to. An off-path
  spoofer could forge an answer from a second nameserver's address, one that had never sent a DNS
  cookie, and so get past the cookie check that would have rejected it from the primary. A reply
  from any other nameserver is now ignored, and the cookie check uses the state of the server the
  query was sent to.

- **A TLS connection on a completion loop could be released twice, corrupting the connection
  pool.** The TLS output queue lets a connection hold a receive and a send at once, and each op's
  completion closed the connection: a peer reset with both in flight (a TLS WebSocket client that
  stops reading, then resets) put the slot on the free list twice, so two later accepts could share
  one connection. Every op posted on a connection is now counted, and a connection with ops in flight
  is released by the last completion, after the others are cancelled. Related: a connection closing
  once its queued TLS output was out was never timed out, so a client that never read held its slot
  for good; a TLS WebSocket connection kept reading while its output queued, so a client that never
  read grew server memory without bound (it is now read only once its output is out); a body or a
  pipelined request that came with the headers was left in the TLS engine until the body timeout; a
  rejected TLS upload was drained from the wrong buffer (stale bytes, an over-read for a large
  receive); and a timed-out TLS request's 408 was never sent.
- **Post-body middleware and the access log read request headers the body had overwritten.** The
  body read reuses the connection's read buffer from offset 0, where `method`, `path` and the header
  pointers point. Post-body middleware (and the `access_log` callback) then saw body bytes in place of
  the headers: a CSRF check could read attacker body bytes as `Cookie` or `Origin`, a header value
  with no terminator left could be read out of bounds, and access-log lines could be forged. The
  request head is now copied before the body is read, and the request's pointers are moved to the
  copy; if the copy cannot be made, the request is answered 500.
- **A small compressed response could make the client inflate hundreds of megabytes.** For a buffered
  (non-streaming) response, `max_response_size` was checked only after the whole body had been
  decompressed, up to the decompressor's own 256 MB cap, synchronously on the loop thread, before the
  request failed with `KL_ERR_TOO_LARGE`. The buffered path now inflates through the streaming
  decompressor into a bounded buffer and stops as soon as the limit is passed, as the streaming path
  already did.
- **A redirect could take the client anywhere its caller would not have connected.** A caller that
  limits which hosts it talks to could check only the first URL: `kl_http_redirect_*` then followed
  every `Location` with no way to ask, so an allowed host could send the client to a cloud metadata
  endpoint or an internal service. `KlHttpRedirectConfig` gains `on_redirect` / `on_redirect_data`
  (appended; zero keeps today's behaviour). Keel calls it with each hop's resolved absolute URL
  before requesting it, in the sync, async and pooled paths; a non-zero return ends the request with
  the new `KL_ERR_REDIRECT_REFUSED` (appended to `KlError`) and no response.
- **An async redirect chain read the caller's config after it was gone.** The async redirect
  client kept the `KlHttpClientConfig *` it was given and read it again to start each later hop,
  on a later event-loop turn - by which time a config the caller had kept on its stack (the
  natural way to pass one) was dead memory, including its `tls` pointer. The client now copies
  the config when it starts.
- **HTTP/1 post-body middleware was skipped when the body arrived in a later read.** A body read
  reuses the connection's read buffer from offset 0, where `req->method` and `req->path` point, and
  the server matched post-body middleware against them only after the body was in. Once the body
  had overwritten the request line the match failed, so the middleware did not run, but the handler
  still did. Post-body middleware is where a CSRF check on a form body belongs, so a client could
  bypass it by sending the body after the headers. Every event model was affected. The server now
  matches post-body middleware at header time, while the request line is intact, and runs the
  matched set once the body is in. **Behavior change:** `kl_http_router_use_post` (and
  `kl_http_server_use_post`) now refuse more than `KL_HTTP_ROUTER_MAX_POST_MIDDLEWARE` (64) entries
  per router, returning -1, because the match is recorded in a 64-bit set. The handler-side limit is
  unchanged and documented: header pointers, including `req->method` and `req->path`, may be
  overwritten once body reading starts, for post-body middleware as for handlers.
- **`KlThreadPool` could lose work items and run others twice.** Submission admits up to
  `queue_capacity + num_workers` items in flight, but the work queue held only `queue_capacity`. When
  more items than that were submitted before idle workers woke to take them, the queue wrapped over
  items not yet taken. Those were lost, never running and never getting `done_fn` or `cancel_fn`, so a
  connection suspended on one hung. The items that overwrote them ran twice, with `done_fn` called
  twice: a double `kl_async_complete`, or a double free of the work context. The work queue now holds
  every item admission allows. The backpressure limit is unchanged, and the `queue_capacity` doc now
  states it. A new test submits bursts of items with distinct contexts before 8 workers wake, over 200
  rounds, and checks each one runs once. Without the fix it fails in the first rounds.
- **An empty HTTP/1 request header hid the header after it.** llhttp reports an empty value
  (`X-Empty:` with nothing, or only whitespace, before the line end) as a zero-length span at the
  first byte of the next line. The server recorded that pointer, then NUL-terminated every value in
  place, which blanked the first byte of the next header's name. That header became invisible to
  `kl_http_request_header`. A request with `Content-Type` after an empty header was then rejected by
  the multipart reader, and any header a proxy inserted after an empty one was lost. Every event model
  was affected. An empty value now points at its own header's name terminator, reads as `""`, and
  touches nothing outside its line.
- **The async HTTP client could call `on_done` twice when the loop ran late.** A connect-phase failure
  (DNS, a refused connect, or a TLS setup failure) is completed on the next loop tick, so that
  `on_done` may free the client. That deferral left the request deadline armed. When the loop ran late
  enough for both to be due in the same timer pass, the deadline fired first and completed the request,
  and the deferred completion then ran again. `on_done` was called twice. If the first call freed the
  client, as `examples/async_client.c` does, the second used freed memory. Deferring an error now
  cancels the deadline. Every completion, cancel and free drops a pending deferred completion, and the
  deferred completion does nothing once the request is done.
- **An `https://` or `wss://` request could be sent in plaintext.** The async, pooled, WebSocket and
  HTTP/2 clients refused a secure URL only when no TLS config was set. A config whose `factory` was
  NULL passed that check, and the connection then took the plaintext path. Headers, cookies and the
  body went out unencrypted, and the pooled client filed the plain connection as a TLS one, so later
  TLS requests could reuse it. The sync client already failed closed. Each of the four entry points
  now refuses a secure URL unless the TLS config has a factory.
- **Middleware on an exact path did not run for that path with a trailing slash.** Route matching
  tolerates one trailing slash on either side: the route `/admin` serves `GET /admin/`, and the route
  `/admin/` serves `GET /admin`. Exact middleware patterns were compared byte for byte, so an auth
  middleware registered on `/admin` did not run for `GET /admin/`, which the `/admin` handler then
  served. Pre-body and post-body middleware, HTTP/1 and HTTP/2 alike. An exact middleware pattern is
  now matched by the route matcher itself, so it covers every path a route with that pattern serves.
  **Behavior change:** this includes `:name` segments, which now match any value in an exact
  middleware pattern, as they do in a route.
- **Freeing the HTTP/2 client from its response or error callback was a use-after-free.**
  `on_resp` runs inside the session's receive, inside the client's read handler. If it called
  `kl_http2_client_free`, the client and its session were freed while both frames were still running,
  and the handler then removed the stream from, and flushed the session of, freed memory. The
  WebSocket client got the same protection in this release. `kl_http2_client_free` from a callback now
  closes the connection and finishes the free when the read handler unwinds, and no further `on_resp`
  is delivered after it.
- **A pooled TLS connection could be reused under a different TLS config.** The client pool keyed
  connections by host, port and "is TLS" only. A request whose config verified the server strictly,
  or presented no client certificate, could therefore get an idle connection made under a config with
  verification off, or under another client identity. The pool now keys a TLS connection by the
  config it was made with, compared by `ctx` and `factory`, and the pooled async and sync clients use
  it. New `kl_http_client_pool_acquire_tls` / `kl_http_client_pool_release_tls` take the config. The
  existing `kl_http_client_pool_acquire` / `_release` keep their signatures and match only connections
  released through them.
- **HTTP client request-side fixes.**
  - A TLS backend that leaves `set_hostname` NULL used to be accepted, and hostname verification was
    silently skipped. The async, sync, WebSocket and HTTP/2 clients now fail closed instead.
  - Proxy credentials containing CR or LF were written into the request, injecting header lines. They
    are now refused with `KL_ERR_INVALID_ARG`. Separately, a plain-HTTP request through an
    authenticating forward proxy now carries `Proxy-Authorization`; only the CONNECT tunnel sent it
    before.
  - An idle pooled connection with unsolicited bytes waiting is no longer reused, since the next
    request would read those bytes as its own response. Examples are a stray response, or a 408 sent
    before the server closed.
  - A streaming request whose `body_read` returned more than its buffer made the client send past the
    buffer. That now fails the request.
  - The request deadline now also covers name resolution. A resolver that never answered left the
    request pending forever.
  - **Behavior change:** a caller-supplied `KlResolver` must provide `cancel`, as `resolver.h`
    already states. Without it, a request freed or timed out while resolving could be called back
    after it was freed.
- **DNS: a cookie-less answer is refused from a server known to send cookies.** Once the resolver
  had learned a nameserver's server cookie, a later answer carrying no COOKIE option was still
  accepted. An off-path spoofer, who cannot see the client cookie, could simply leave the option
  out. Such answers are now dropped (RFC 7873 5.3); a server that has never sent a cookie is still
  accepted.
- **Last-resort entropy no longer repeats.** If the OS random source failed, `kl_plat_random` fell
  back to a fill derived from the buffer's address alone. A resolver refilling its pool then drew
  the same DNS transaction ids each time. The fallback now mixes a high-resolution clock, the
  process id and the address.

- **An HTTP/1 request split across reads was dropped or misparsed by the server.** When a request's line
  or headers arrived in more than one read (large headers, a slow link, TLS records, or a client
  that simply writes in pieces), the server fed the whole accumulated buffer to the request parser
  again from byte 0. The parser keeps its place between calls, so it parsed the start of the request
  twice. Depending on where the split fell, that:
  - rejected the request (the connection closed with no response);
  - produced a different request (phantom headers built from the re-fed request line);
  - or derived lengths from stale pointers into the buffer: the path length is computed by pointer
    arithmetic against a pointer from the earlier feed, and the path is then scanned up to that
    length, an out-of-bounds read. Split requests were observed
    coming out with a Content-Length of 2^64 - 28.

  Every event model was affected: readiness (epoll, kqueue, poll, WSAPoll) and completion (io_uring,
  IOCP, pollcomp). Any remote client could trigger it. Loopback tests missed it because a small
  request arrives in one segment. The parser is now given only the bytes it has not seen, through one
  helper both models share, and a buffer that grows (and may move) restarts the header parse on the
  completion path too, as it already did on the readiness path. The new `test_http_split_request`
  suite sends requests split at every byte offset, one byte at a time, a header block larger than the
  base read buffer, and a split request on a kept-alive connection, checking that the handler sees
  exactly the request that was sent. Without the fix 4 of its 5 cases fail, and none of the 109
  single split points yields the request that was sent: 64 get no response and 45 are parsed as a
  different request (phantom headers, or that 2^64 - 28 Content-Length). With it, all pass.

- **Freeing the async HTTP client inside `on_done` after a connect-phase failure was a
  use-after-free.** A DNS failure, a refused connect, or a TLS setup failure called `on_done` from
  inside the connect op's terminal dispatch. Freeing the client there, as `examples/async_client.c`
  does, freed the `KlConnectOp` embedded in it while that frame was still running, which then read
  and wrote freed memory; an owned resolver was also destroyed inside its own callback. Such a
  completion is now deferred to the next loop tick. `on_done` may free the client on every path, and
  is never called from inside `kl_http_client_start` (now documented on `KlHttpClientDoneFn`).
  **Behavior change:** a failure found while starting (for example a resolver that fails
  synchronously) used to call `on_done` before `kl_http_client_start` returned; it now arrives on
  the next loop tick. A consumer that freed the client in that callback was handed a dangling
  pointer by `start`. The
  new `test_http_client_free_in_done` frees inside `on_done` for an inline DNS failure, an
  asynchronous one, a refused connect and a TLS setup failure, using an allocator that poisons and
  quarantines freed blocks so any later read or write is caught; without the fix it crashes.

- **TLS was skipped when a client's connect completed at once.** The async HTTP, WebSocket and
  HTTP/2 clients treated a non-blocking connect that succeeded immediately as "connected, start
  talking" and bypassed the post-connect path that sets up TLS (and, for HTTP, a proxy CONNECT
  tunnel). An AF_UNIX connect usually completes at once, so `https+unix://` and `wss+unix://`
  requests went out in plaintext, with no certificate check, including headers such as
  `Authorization`; `h2` over a TLS-configured unix socket spoke plaintext too. The HTTP client now
  always takes the connecting path (the writable event arrives immediately); the WebSocket and
  HTTP/2 clients keep their immediate start only when no TLS is configured (so plaintext `h2c`
  can still issue a request right after connect). New `test_unix_socket` cases drive async
  `https+unix` and `wss+unix` against a TLS server and require that the client created a TLS
  session.

- **Credentials followed a cross-origin redirect.** `KlHttpRedirect*` stripped only
  `Authorization` when a 3xx pointed at another origin. The caller's `Cookie` and
  `Proxy-Authorization` went to whatever host the redirect named. All three are now dropped
  cross-origin, and all three are still kept on a same-origin redirect.
  - `test_http_redirect` adds `sync_cross_origin_drops_all_credentials` and
    `sync_same_origin_keeps_credentials`.

- **Freeing the WebSocket client from a callback was a use-after-free.** `on_close` ran before the
  connection was closed, then the client closed it; the frame loop kept using the connection after
  `on_message`; and code after `on_open` read it too. Calling `kl_ws_client_free` from any of
  these, the natural pattern, touched freed memory.
  - A free from inside a callback is now deferred: the connection is closed at once, and the memory
    is released when the event handler unwinds. The frame loop stops after a callback that freed or
    closed the connection.
  - `on_close` now fires after the connection is closed, as the last callback.
  - The WebSocket client also now:
    - fails the connection with close code 1002 on a masked server frame (RFC 6455 5.1);
    - fails on a close frame whose status may not appear on the wire (1002) or whose reason is
      not UTF-8 (1007), instead of echoing and reporting it;
    - caps the upgrade response at 16 KiB instead of growing the buffer without bound;
    - frees the upgrade request buffer with the size it was allocated with.
  - New suite `test_websocket_client_peer` uses a raw peer and a poisoning allocator. Without the
    fix, the three free-in-callback cases crash, and the masked-frame, two close-validation and
    handshake-cap cases fail.

### Added

- **`KL_IO_RESOURCE_EXHAUSTED` in `KlIoStatus`.** Appended (existing values unchanged, a minor,
  additive change): a provider reports an operation that failed for lack of descriptors or memory.
  The built-in POSIX and Winsock fallback maps `EMFILE`, `ENFILE`, `ENOBUFS` and `ENOMEM` (Winsock's
  `WSAEMFILE` / `WSAENOBUFS` arrive as those), the EFI provider maps `EFI_OUT_OF_RESOURCES` (it
  reported `KL_IO_FATAL` before). A provider that never returns it is unaffected. Code that switches
  over `KlIoStatus` without a `default` sees one more value.

- **Anonymous pipe pairs on POSIX.** `kl_anon_pipe_create` now works on every POSIX engine that
  watches native descriptors: epoll, kqueue, poll, and io_uring / pollcomp through their watcher relay.
  - The pair is an ordinary pipe: `pipe2(O_CLOEXEC)`, or `pipe` + `FD_CLOEXEC` on macOS. Only the
    parent end is non-blocking; the child's end (`kl_anon_pipe_end_fd`) stays blocking.
  - The parent end is the same `KlPipeStream`, in readiness mode over the generic watcher
    registration, never `KlSocketProvider`. The directional facets, lifetime and callbacks are
    unchanged.
  - **No SIGPIPE, no process-global change.** A write whose reader has gone returns
    `KL_STREAM_ERROR`. Keel uses `F_SETNOSIGPIPE` where it exists; elsewhere it blocks SIGPIPE in the
    writing thread and consumes only the signal its own write raised. An embedder's already-pending
    SIGPIPE is left alone.
  - **A vanished reader wakes a blocked writer.** Engines may report it only as HUP / ERR, delivered
    as READ, so a pending write is attempted on any readiness. `EPIPE` then fires the writable-again
    edge once, the retry returns `KL_STREAM_ERROR`, and a graceful close still detaches.
  - **Known defect, tracked, not worked around:** Cosmopolitan on a Windows host reports a full pipe
    as writable, so a blocked writer there busy-wakes instead of sleeping. There is no timer backoff
    and no message-size rule; `test_anon_pipe` records it as a known defect that fails once fixed.
  - `check-pipe-seam` adds three rules: POSIX pipe creation stays in its platform layer; no
    process-global SIGPIPE disposition outside the HTTP server's existing site; and the pipe transport
    never calls the socket seam.
  - `test_anon_pipe` now asserts support by platform and engine capability instead of
    completion-vs-readiness. It runs a POSIX section on every POSIX engine, io_uring and pollcomp
    included: data both ways with EOF, the blocked-writer / vanished-reader path, no SIGPIPE under the
    default disposition, an embedder's pending SIGPIPE left pending, pause, cancel, free from a
    callback, 200 close-order races, and allocation failure at every point, each checked against the
    allocator and the open descriptor count.

- **Anonymous pipe pairs on Windows: `kl_anon_pipe_create`.** One call makes a one-directional pipe
  whose parent end is a `KlPipeStream` and whose child end is a `KlAnonPipeEnd` for the embedder's
  process spawner (`<keel/anon_pipe.h>`; the native `HANDLE` via `<keel/anon_pipe_native.h>`).
  - The stream is directional: a `KL_ANON_PIPE_READS` pair's stream is read-only and a
    `KL_ANON_PIPE_WRITES` pair's is write-only. The other facet is never installed, so the stream
    contract refuses it.
  - It is the named-pipe transport reused: the parent end is the overlapped server end of a private
    named pipe, with a random 128-bit name, one instance, a current-user + SYSTEM DACL, remote
    clients rejected, and the connected client verified to be this process. The child end is
    synchronous, as a child's C runtime expects. Neither end is inheritable; the spawner opts the
    child end in.
  - Windows: IOCP only; WSAPoll returns `KL_PIPE_UNSUPPORTED`.
  - Keel still spawns nothing: `check-pipe-seam` now forbids process-management calls anywhere in the
    library, and stdio-role or protocol names in the pipe transport.
  - Tested by `test_anon_pipe` on IOCP: privacy, direction and inheritance, 1 MiB reads and 4 MiB
    edge-driven writes with EOF both ways, broken pipe, pause, cancel, free from a callback, 200
    close-order races, and allocation failure at every point, each checked against both the
    allocator and the process handle count.

- **`kl_stream_on_writable`: a writable-again edge on `KlStream`.** Before, a producer told
  `KL_STREAM_WOULD_BLOCK` had no way to learn when to retry. After a `WOULD_BLOCK`, the callback
  fires at most once, when the producer should retry because the blocked condition has ended:
  - capacity came back: the stream's total pending bytes, in-flight bytes included, dropped below
    its write capacity. The retry is accepted.
  - the write side failed terminally, so capacity never will. The retry returns `KL_STREAM_ERROR`.

  The retry's status is the single source of truth; the callback carries none. An accepted write
  disarms it, drain progress alone never fires it, it fires only from `kl_stream_flush` or
  `kl_stream_on_write_complete` (never from inside `kl_stream_write`), and a close in progress
  suppresses it. It is generic, so sockets, named pipes and future pipe transports share it.
  Contract in `docs/contracts/stream.md`; tested by `test_stream_writable` (every rule, plus a
  producer driven only by the edge over a real socket) and over a named pipe on IOCP.

### Fixed

- **UEFI server: a client that stops reading no longer stalls the whole firmware event loop.**
  The EFI completion drain sent server responses through the synchronous socket send, which pumps
  a Transmit token for up to about 60 s. A peer with a zero window (its send buffer full, the
  stack holding the Transmit queued) therefore froze every other connection for up to a minute per
  attempt, and then the send failed. The drain now submits one Transmit fragment and polls it
  (`kl_uefi_socket_send_step`), leaving the send pending while the token is queued; a later drain
  finishes it once the firmware completes the Transmit, counting each completed fragment as send
  progress so the idle sweep does not reap a slow but moving reader. While it is pending the
  connection's tx buffer belongs to it (a synchronous send on that connection is would-block).
  Closing a connection whose Transmit is still queued (the idle sweep reaping a peer that stopped
  reading) cancels it and then closes abortively (RST): a graceful close would wait behind the
  unsent data and was pumped for up to the same 60 s. (behavior change) Other closes stay
  graceful, and the client's synchronous send is unchanged.
- **UEFI server: posted ops of a quarantined connection complete.** A connection whose token a
  cancel could not retire is marked dead but keeps its generation. The drain dropped its posted
  recv/send as stale and a cancel freed them without a completion, so the server, which releases a
  connection only after its last op completes, leaked it. They now complete as failures; only an
  op of an earlier connection on a reused handle is dropped.
- **lwIP raw: a client half-close no longer truncates the response.** A FIN from the peer while a
  response was being sent failed the send, so a client that shut down its write side after the
  request (`shutdown(SHUT_WR)`) got a cut-off response. A FIN now ends only the read side (the next
  read sees EOF); the send fails only on an error delivery or a reset. (behavior change)
- **An HTTP/2 client closed from its response callback gets no `on_error` from a failed receive.**
  When the session completed a stream inside its receive (the response callback closed the client)
  and then failed on a later frame of the same batch, the receive-failure path checked only for a
  freed client, so a closed one was still reported to `on_error`. It now checks for a close too, as
  the flush paths do.

- **A body sent after the request head reaches a resumed handler alone (completion loops).** A
  streaming-async handler that suspended at dispatch before any body byte arrived (the head came on
  its own, as from a client waiting for 100 Continue), and whose resume awaited the body and
  un-paused the read, had the receive land behind the request head still counted in `read_buf`; the
  request line and headers then went to the body reader as body (Content-Length echoed the head,
  chunked failed with 413). The body window now starts empty whenever nothing is kept across the
  suspend.
- **TLS (readiness): a body the engine already holds after the header read is read on.** The header
  read takes at most the free read buffer, so an engine (OpenSSL, mbedTLS) keeps the rest of a
  record that carried headers and body decrypted (`pending() > 0`), which the socket never reports.
  After the dispatch moved to the body phase, or a resumed async handler awaited the body, nothing
  read it and the request ended in 408. Both now read on while the engine holds input, also when
  the body read is paused (by `on_data` during the leftover feed, or before an async suspend): as
  documented, a pause takes hold at the record boundary, so the rest of the decrypted record is
  delivered and the read stops there.
- **A paused body read stays paused after a nested `kl_async_complete` inside `on_resume`
  (kqueue).** The outer complete registered the paused read with an add for no interest, an empty
  change list on kqueue, so the READ filter the nested complete had enabled stayed on and the body
  was read while paused. The registration is now set explicitly.
- **The HTTP server refuses a completion loop that lacks the stream-server operations (behavior
  change).** `prime_accepts`, `post_accept`, `post_recv`, `post_send` and `cancel` are optional in a
  runtime completion provider (a client-only or datagram-only one leaves them out), but the server
  drives every connection through them: it called a NULL `post_recv` at the first accept, and without
  `cancel` a connection released with a receive posted was never returned to the pool.
  `kl_http_server_init` now fails with `KL_ERR_UNSUPPORTED` on such a loop. Every in-tree completion
  backend (IOCP, io_uring, pollcomp, lwIP raw, EFI) implements all five.
- **WebSocket client, HTTP/2 client, the DNS resolver's TCP fallback and the readiness accept loop
  classify a failed socket call through the provider (`kl_sock_io_status`), not `errno`.** A provider
  that reports by status and leaves `errno` alone (the documented contract for a freestanding one)
  had every nonblocking connect of the WebSocket and HTTP/2 clients refused, a would-block read or
  write treated as a hard error, and the accept loop never backing off when out of descriptors (it
  retried on every wake of the listen socket). The accept loop backs off on
  `KL_IO_RESOURCE_EXHAUSTED`; with the built-in providers the result is the same as before.
- **A buffered plaintext response larger than the completion backend's `send_max` goes out whole.**
  It was posted in one send with no regard to the cap, so on EFI (a 16 KiB send buffer, a hard
  limit) any buffered response above it failed and closed the connection. It now goes through the
  output queue, which posts it in pieces the backend accepts. The `send_max` contract is restated:
  a hard limit where the backend sets it as one (EFI), a hint on io_uring.

- **The async client's socket provider is per client; the shared `KlEventCtx` is never modified
  (behavior change).** `kl_http_client_start` and `kl_http_client_start_pooled` wrote the chosen
  provider (`cfg->sockets`, or a completion backend's native one) into the caller's ctx, also when
  the start failed after that point (DNS, allocation, connect, deadline). Every client and server
  I/O call reads the provider at call time, so a handler that started a client with its own provider
  on `kl_http_server_event_ctx(srv)` moved the server's I/O onto it, a client started earlier with
  provider A did its later I/O (its close among it) through provider B, and the pool closed its
  connections through whatever provider its ctx had last. The client now keeps the provider it
  chose (the configured one, else the ctx's, else the backend's native one on a completion loop) and
  uses it for all of its own socket calls, including those of the built-in DNS resolver it creates
  when none is configured (its UDP socket and TCP fallback); the ctx is only read. A pooled
  connection records the provider it was made through: it is closed through it and reused only by
  a request on the same provider (the public pool acquire/release/discard use the pool ctx's
  provider). Code that relied
  on a client start setting `ctx.sockets` for other users of the ctx must set it itself. A
  caller-owned provider must outlive every pooled connection made through it.
- **A datagram-capable socket provider no longer reads as an overlapped (completion) one.** The
  internal `KL_SOCK_CAP_OVERLAPPED` and the public `KL_SOCK_CAP_DATAGRAM` were the same bit
  (`1ull << 3`), so every provider that advertises datagram support (the built-in POSIX and Winsock
  providers, the lwIP BSD provider, any custom provider with a datagram vtable) also read as
  overlapped. Effects: a streamed (chunked) response whose server or ctx named such a provider
  explicitly (`KlHttpServerConfig.sockets = kl_socket_provider_posix()` / `_winsock()`) on a
  readiness loop handed its bytes to the completion output queue, which refused them, so
  `kl_http_response_begin_stream` and every SSE / streamed response failed (and in a
  `KEEL_NO_COMPLETION` build, where that queue is the `abort()` stub in `completion_http_absent.c`,
  the process aborted); every overlapped provider read as datagram-capable, including a TCP-only
  EFI provider built without `KEEL_UEFI_DATAGRAM`; and on a completion
  loop (IOCP, io_uring, pollcomp, lwIP raw) the capability negotiation accepted such a provider
  instead of adopting the backend's own overlapped one. `KL_SOCK_CAP_OVERLAPPED` now sits at bit 63
  (internal bits are allocated from the top, public ones from bit 0), and a compile-time check keeps
  the two sets disjoint. The IOCP overlapped provider, which carries the Winsock datagram ops but
  had relied on the shared bit to advertise them, now sets `KL_SOCK_CAP_DATAGRAM` itself.
  (behavior change) On a completion loop, an explicitly configured built-in
  or other datagram-capable readiness provider is now replaced by the backend's overlapped provider
  (server and async client), or refused where the backend offers none, as the negotiation contract
  already documented; it no longer negotiates by accident.
- **A datagram send refused for one datagram no longer ends the send side (behavior change).** Any
  send failure other than would-block set the `KlDatagram`'s sticky send error, so every later
  `kl_datagram_send` returned `KL_DATAGRAM_ERROR` for the life of the object. Many failures concern
  only the datagram they hit: no route to its destination (`ENETUNREACH`, `EHOSTUNREACH`,
  `ENETDOWN`, `EHOSTDOWN`), the ICMP report about an earlier datagram that Linux and the BSDs return
  from the next send on a connected socket (`ECONNREFUSED`), a full queue (`ENOBUFS`, `ENOMEM`), a
  firewall or broadcast refusal (`EPERM`, `EACCES`), a path MTU (`EMSGSIZE`), a pinned source that is
  not local (`EADDRNOTAVAIL`), a peer the socket cannot use (`EINVAL` for an unscoped IPv6 link-local
  peer, `EAFNOSUPPORT`, `EISCONN`, `EDESTADDRREQ`), and their Winsock counterparts. One query sent
  while the host had no route killed the built-in DNS resolver's socket for good. Such a failure now
  fails only its datagram: a direct send returns `KL_DATAGRAM_ERROR` for that call with
  `kl_datagram_last_error` = `KL_ERR_IO`, and a datagram already accepted (queued, or posted on a
  completion backend, including an IOCP send refused at issue) is dropped and counted by
  `kl_datagram_dropped` with `kl_datagram_last_error` = `KL_ERR_IO`; the next send goes out. Any
  other failure (a closed or invalid socket, an unknown error) is still sticky, and now reports
  `KL_ERR_SOCKET` (a bad message reports `KL_ERR_INVALID_ARG`). Every backend classifies its own
  error: the hosted errno mapping and the Winsock provider (which now keeps the error's identity
  instead of reporting `EIO`, and maps `WSAEHOSTDOWN`), io_uring, pollcomp, IOCP, the EFI_UDP4
  provider (a Transmit call or token ending `EFI_NOT_FOUND` (no route) / `EFI_ICMP_ERROR` /
  `EFI_*_UNREACHABLE` / `EFI_BAD_BUFFER_SIZE` / `EFI_NO_MAPPING` / `EFI_OUT_OF_RESOURCES`, or an IPv6
  peer) and the lwIP raw provider (`udp_sendto` `ERR_RTE` / `ERR_MEM` / `ERR_BUF`, or an IPv6 peer).
  Keel's own refusals inside the POSIX provider send (a TOS family it cannot determine, a control
  message it cannot build) stay sticky. An interrupted direct send (`EINTR`) is retried instead of failed. The DNS resolver
  moves a query whose send fails straight to the next nameserver.
- **A custom socket provider's `io_status` result `KL_IO_RESET` after a datagram send now means "drop
  this datagram" (behavior change).** It latched the datagram's send error like any other failure;
  it is now read as the ICMP report a UDP send can return, and only that datagram is dropped.
  `KL_IO_INTERRUPTED` retries the send. `KL_IO_FATAL` still latches.
- **IOCP: a datagram receive past many queued ICMP reports keeps going.** A receive that met more than
  16 ICMP reports in a row at issue (a socket adopted through `kl_datagram_init`, which keeps the
  Winsock reports on) failed, and the datagram stopped receiving for good. It now queues its own
  completion and is issued again from the next drain, as the readiness receive yields would-block at
  the same bound.
- **io_uring: a submit refused with `-EAGAIN` no longer spins the loop.** When the kernel was short of
  memory, `io_uring_enter` returned `-EAGAIN` at once and, with no completion to reap, the drain
  returned straight away, so the run loop called it again at 100% CPU for as long as the shortage
  lasted. The drain now waits for one completion or its timeout first.
- **A refused async client start leaves the shared `KlEventCtx` on its own socket provider.**
  `kl_http_client_start` and `kl_http_client_start_pooled` wrote `cfg->sockets` to the caller's ctx
  before checking that the ctx's loop could drive it. When the check refused the start, the ctx kept
  the incompatible provider: every later start on it without a provider of its own was refused, and
  every other user of the ctx (server, watchers, datagrams, WebSocket client) went through the wrong
  provider. The provider is now put back on a refusal; a start that passes still sets it.
- **HTTP/2 client: no `on_error` after the client is closed or freed from `on_resp` during a
  send.** A send that closes a stream (an END_STREAM or RST_STREAM going out) runs `on_resp`; when
  that closed or freed the client, the session went on sending its queued frames to the closed
  socket, the flush failed, and `on_error` ran on a client the user had already closed or freed. The
  send callback now fails at once on a closed client, and a failed flush reports nothing once the
  client was closed or freed, as the receive path already did.
- **A timer added from a timer callback waits for the next tick.** `kl_timer_fire` re-read the
  clock after every callback, so a 0 ms timer added from a callback was already due and fired in the
  same call; one that re-added itself (a retry through the 0 ms deferred-error timers of the HTTP
  client, WebSocket client or DNS resolver) kept the call from returning and starved all I/O. It now
  fires only the timers that existed when it was entered, and timers due at the same millisecond fire
  in the order they were added.
- **io_uring: a send of 4 GiB or more no longer loops forever.** A send SQE's length is 32 bits, so
  a remainder of exactly 4 GiB was prepared as 0 bytes, the kernel sent nothing, and the send was
  prepared again without end (reachable through the copy fallback for a file of 4 GiB or more on a
  kernel without splice). Each send SQE now carries at most 0x7ffff000 bytes and the rest follows as
  partial sends, the backend reports that as its `send_max`, and a send that completes with 0 bytes
  of a non-empty remainder fails the write. IOCP's WSASend lengths are capped the same way.
- **io_uring: -EBUSY from submit no longer stops the loop.** On kernels 5.5 to 5.18 a CQ overflow
  backlog makes `io_uring_enter` return -EBUSY (or -EAGAIN) until completions are reaped; the drain
  treated that as a fatal loop error. It now reaps, like a timeout or an interrupted wait, and the
  waiting submissions go in on the next drain.
- **Readiness: a WebSocket frame sent from outside its connection's event goes out.** With the drain
  enabled, a frame the socket would not take went into the drain, but WRITE interest was set only in
  the transition after that connection's own event. A frame sent from another connection's
  `on_message`, a timer or a thread-pool `done_fn` had no such event behind it, so the backlog sat
  until the peer sent something, and with `ping_interval_ms` set a healthy receive-only subscriber
  was failed with Close 1001. A send that leaves the drain pending now arms READ|WRITE itself, and a
  send that finds a backlog moves it first (counting it as the peer's progress, and answering a
  PONG owed meanwhile). Completion loops already flushed the drain from their send completions.
- **Readiness: a stream written while its connection is suspended keeps moving.** Once the drain
  held bytes, `kl_drain_write` only appended, never trying the socket, and a suspended connection has
  no WRITE interest, so a stream written from a timer (an SSE or event feed) stopped after one
  would-block until the resume, or until the 1 MiB cap failed the stream. `kl_drain_write` now first
  writes what is pending until the socket would block, then appends behind what is left, firing no
  callback.
- **A dual-stack server trusts PROXY headers from an IPv4 load balancer.** A server bound to `::`
  (dual-stack) sees an IPv4 peer as `::ffff:a.b.c.d`, and the `proxy_trusted_cidrs` match compared
  families strictly, so an IPv4 trust list (`10.0.0.0/8`) never matched: every request from behind
  the load balancer was refused with 400. `kl_cidr_match` now matches a v4-mapped peer as its IPv4
  address against the IPv4 entries, and a v4-mapped CIDR (`::ffff:10.0.0.0/104`) is stored as the
  IPv4 CIDR it names. Any other IPv6 peer still needs an IPv6 entry.
- **miniz: a streamed gzip body whose optional header fields arrive in pieces decodes.** The
  streaming decoder required FEXTRA, FNAME, FCOMMENT and FHCRC to arrive in the same `dfeed` call as
  the fixed 10 header bytes, so a valid response split there (FNAME is what gzip writes by default)
  failed with a decode error. The header is now parsed by a small state machine carried across
  feeds: the name and comment are skipped whatever their length, the extra field by its XLEN, the
  header CRC as two bytes, and a body that ends inside them still fails at end of input.
- **Windows DNS: a link-local IPv6 resolver listed first no longer breaks resolution.** The system
  nameserver list was formatted without scope ids and kept `fe80::` entries (only `fec0::`
  placeholders were dropped), and the resolver locked its server list to the first entry's family.
  A router-advertised `fe80::` resolver listed first therefore made every query go to scope 0 and
  fail, although IPv4 servers were configured. Windows discovery now skips link-local servers, and
  the resolver picks its family from the first usable server, skipping any unscoped `fe80::/10`
  entry (falling back to `127.0.0.1` when discovery leaves none usable, as when it finds none).
- **A streaming-async handler that suspends at dispatch and then awaits the body gets the whole
  body.** The handler ran before any body byte was fed, and when it suspended the body bytes read
  with the headers were left behind: a resume that asked for the body
  (`kl_http_request_await_body`) lost them on the completion engines and, with chunked framing, fed
  the next bytes to a decoder still holding the previous request's state. On the readiness engines
  nothing read the body at all (the resume registered no interest for that state) and the request
  ended in 408. The decoder and the body deadline now start before the handler runs; the bytes read
  with the headers are kept across the suspend and fed before the next read; the readiness resume
  registers the connection for reading (no interest while the body read is paused). On the
  completion engines a resume that also un-pauses the body read (`kl_http_request_resume_body`)
  no longer posts a receive behind the kept bytes before they are fed, which delivered them twice
  (or, with TLS, read over them). The kept bytes are delivered even while a pause is in effect
  (they are already off the connection); if the resume answers without awaiting the body, a later
  rejection drain runs them through the chunked decoder first, so its framing check starts in the
  right place. A reused
  connection slot no longer carries the previous request's body start time. The absolute body
  deadline (`body_timeout_ms`) no longer counts time spent suspended in a `KlAsyncOp`, which is the
  server's own work, not the client's upload.
- **A connection closed right after a nested `kl_async_complete` leaves the event loop.** A resume
  that suspended on a second op and completed it at once registered the connection's socket again;
  when the response then closed the connection, it was released while still registered. poll and
  WSAPoll kept the closed socket's entry (poll reports it on every wait, so the idle server could
  spin), and a reused descriptor number could reach the released slot. It is now taken out of the
  loop before the release.
- **A streaming handler that suspends from its body reader's `on_error` is not answered over.** When
  a body error resumed the handler and it started an async op, the server still wrote its own 413 or
  500 while the op was pending. That state is now honoured like a response the handler sent.
- **Completion server: TLS WebSocket output to a client that stops reading is bounded.** Every TLS
  write's ciphertext went from the engine's bounded ring onto the uncapped output queue at once, so
  a TLS WebSocket send never saw a full buffer: a client that stopped reading grew server memory
  without bound, and a single frame larger than the queue's bound was taken whole. A TLS WebSocket
  now has the bound a plaintext one has, checked before the frame is encrypted.
- **`kl_async_complete` inside the handler that suspended sends one response.** A handler that
  suspended and then completed before returning (the work could not be started, say) was driven
  twice: a second, empty response followed the real one on a keep-alive connection, and with
  `Connection: close` the slot was released twice, so two later connections shared it. The same
  holds inside a body reader's `on_data` and inside another op's `on_resume`.
- **`kl_async_cancel` closes the connection (behavior change).** Cancel, the documented way to fail
  a deadline, only retired the op: the connection stayed suspended, outside the loop and exempt from
  the idle sweep, until the server was freed, so each cancelled request leaked a slot and a socket
  until the server stopped accepting. A cancelled op's connection is now closed without a response,
  once, also when the cancel comes from inside the handler, `on_data` or another op's `on_resume`.
- **Windows: an ICMP unreachable no longer stops a datagram receiver.** Winsock reports an ICMP
  port-unreachable caused by an earlier send as `WSAECONNRESET` on the next receive (and
  network-unreachable as `WSAENETRESET`), even on an unconnected UDP socket. Keel took that as a
  fatal receive error and stopped the `KlDatagram` for good, on WSAPoll and IOCP alike. One
  unreachable nameserver (for example `127.0.0.1` with no local resolver) silenced the built-in DNS
  resolver, and any datagram server could be stopped by a spoofed datagram that made it reply to a
  closed port. A socket Keel prepares (`kl_datagram_socket_init`, and the resolver's own socket)
  now has these reports turned off (`SIO_UDP_CONNRESET` / `SIO_UDP_NETRESET`), and a receive that still meets one, on a socket adopted
  through `kl_datagram_init`, skips it and takes the next datagram (IOCP re-posts the receive). Any
  other receive error is still terminal.
- **POSIX: a connected datagram keeps receiving after an ICMP unreachable.** Linux and the BSDs queue
  an ICMP port, host or network unreachable caused by a send on a connected UDP socket as the socket
  error, and the next receive returns it (`ECONNREFUSED`, `EHOSTUNREACH`, `ENETUNREACH`). The datagram
  receiver took that as fatal on every POSIX backend (readiness, io_uring and pollcomp) and stopped for
  good, so a client connected to a peer that restarted, or a resolver connected to a nameserver that was
  briefly down, never heard from it again. The receive now consumes such a report and takes the next
  datagram (`recvmsg` and `recvmmsg` read again, yielding would-block after a bounded number of
  reports; io_uring re-posts the receive; pollcomp polls again). Any other receive error is still
  terminal.
- **A completion loop without datagram support refuses a `KlDatagram` instead of crashing.** The
  datagram slots of a completion provider (`post_dgram_recv`, `post_dgram_send`, `cancel_dgram`,
  `retire_dgram`) are optional, and a stream-only provider (the EFI integration built without
  datagrams, a custom provider) leaves them NULL. `kl_datagram_init` accepted such a loop anyway and
  called the NULL slot at the first receive. `kl_datagram_init`, `kl_datagram_init_ex` and
  `kl_datagram_socket_init` now fail with `KL_ERR_UNSUPPORTED` before taking the descriptor (and
  `kl_datagram_socket_init` before creating one), and the completion routers no longer call a missing
  slot: a post fails, a cancel does nothing, and the async client's connect fails cleanly on a
  provider without `post_connect`.
- **A UDP GSO group the kernel refuses is sent per segment instead of dropped.** Linux refuses a GSO
  send of more than 64 segments or a segment the path cannot carry with `EINVAL`, and one over the
  65507-byte UDP payload with `EMSGSIZE`. `kl_datagram_send_gso` fell back to per-segment sends only on
  `EOPNOTSUPP`; for these errors it dropped the whole group, so a large `kl_datagram_send_gso` call
  could lose every segment while reporting it accepted. A group over 64 segments or 65507 bytes now
  goes straight to per-segment sends, and any other refusal of one group sends that group per segment
  without turning GSO off for the next.
- **EFI integration: a server connection closed with a receive or send posted is released.** The
  EFI completion backend's cancel freed a connection's posted receive or send without completing it,
  but the HTTP server releases a connection only from the completion of the last operation it
  posted. Every connection closed while waiting for input (an idle keep-alive client timed out, a
  TLS handshake timeout) therefore kept its connection and its EFI socket slot for good, and with
  eight slots about seven idle clients left the firmware server unable to accept. A cancel now marks
  the posted operation, and the next drain delivers it once as a failed read or write; an operation
  left behind by an earlier connection on a reused handle is still dropped undelivered.
- **lwIP raw: an abort inside the send callback no longer leaves lwIP using the freed pcb.** When
  a file response's file came up short mid-transfer (a truncated file), or `tcp_write` failed hard,
  the send pump aborted the connection from inside lwIP's `tcp_sent` callback and the callback
  then returned success. lwIP skips its post-callback work only on `ERR_ABRT`, so it went on to
  touch the pcb it had just freed; lwIP's pools keep that memory mapped, so sanitizers did not
  see it. The callback now returns `ERR_ABRT` whenever the pump aborted, and the client connect
  callback that reported `ERR_ABRT` without aborting now aborts first.
- **lwIP raw: a connection with a send and a receive posted is released when it dies.** A peer
  reset (or a cancel) produced one failed completion per connection, reported as the send's, and
  suppressed the receive that was also posted, as during a TLS handshake, on WebSocket and HTTP/2,
  or with `Expect: 100-continue`. The server releases a connection only after its last posted op
  completes, so such a connection and its backend slot were never freed. Every posted op on a dead
  connection now completes on its own: the send with a failed write, the receive with a failed
  read, also after a cancel of a connection that is already dead. Nothing completes for an op
  that was not posted.
- **lwIP raw: closing a dead connection no longer tears down a new one on the same pcb.** An
  accepted connection's socket handle was its `tcp_pcb` pointer, and lwIP's pools give a freed
  pcb's address to the next accept. Closing a reset connection then found the new connection by
  that address and closed it, and the dead slot was never cleared. Accepted connections now get a
  slot handle (index plus generation) as their socket handle, which a later connection can never
  match. The raw backend's connection limit is 65535 as a result.
- **Completion server: TLS output written outside a request is sent at once.** On a completion
  loop a TLS write only reached the engine's output ring, which was flushed when the connection was
  next driven by input. A WebSocket auto-ping, a frame sent from a timer, or the drain's Close or
  GOAWAY then waited for the client to speak, so a client that only answered pings never got one and
  was closed as dead. Every TLS write on a completion loop now queues and starts sending its output.
- **Completion server: a graceful drain accepts no new connection.** A completion loop kept posting
  accepts during a drain, and served a client that connected after the stop (keeping the drain
  going). It now stops accepting when the drain begins, as readiness does, and closes a connection
  whose accept was already posted. This includes the lwIP raw and EFI integrations, whose backends
  accept on their own.
- **Completion server: a TLS stream written while its connection is suspended goes out.** A handler
  that started a stream, suspended, and wrote chunks from a timer (an event feed) had them held in
  the TLS engine until the connection resumed; readiness and plaintext send them as written, with
  the same bound on what a client that stops reading can hold.
- **A suspended connection that dies is cancelled.** On a completion loop a suspended connection
  whose send failed (its client reset) was released with its async op still registered: the op's
  `on_cancel` never ran, and the later `kl_async_complete` resumed a slot already back in the pool,
  or a new client's connection. Releasing a suspended connection now cancels its op.
- **WebSocket server: a frame sent from `on_close(1006)` fails (behavior change).** When a client
  went without a Close, or the server was freed with the WebSocket still open, `on_close` runs as the
  connection is released, and a frame sent from it was written to the connection being torn down. On a completion loop it was posted as a send whose
  completion arrived for a slot already back in the pool, and a failed one released that slot a
  second time. The connection is gone, so such a send now returns -1, and nothing is posted on a
  connection while it is released.
- **A failed `100 Continue` tells the body reader.** When the interim response could not be written
  the connection closed without calling the reader's `on_error`, unlike every other failure after
  the reader was created.
- **HTTP server: `100 Continue` is written in full, or the connection closes.** The interim response
  was written best-effort and its result ignored, so a write the socket or TLS engine refused dropped
  it. The client then waited for it, and with a real TLS engine the held record went out ahead of the
  response.
- **`kl_http_response_reset` keeps a pooled response's ownership mark.** A handler that reset its
  response and then streamed got a failed stream on a completion loop.
- **WebSocket server: the auto-ping no longer closes a slow but live client on a completion loop.**
  Without the drain, a completion-driven WebSocket refuses output once 1 MiB of it is unposted.
  The auto-ping queued its PING behind that backlog anyway, the write failed, and the failed send
  closed the connection at once, before the stall bound (the read timeout) could apply. No PING is
  now queued behind pending output: the interval stands in for one, the backlog moving answers it,
  and a backlog that does not move for the stall bound fails the connection as before. A PING goes
  out only with nothing queued ahead of it, and only bytes received answer it.
- **WebSocket client: `kl_ws_client_close` calls no callback.** When its Close frame could not be
  sent, close called `on_error` from inside the public call. An `on_error` that freed the
  connection freed it under the caller, and a caller doing `kl_ws_client_close(ws, ...)` then
  `kl_ws_client_free(ws)` freed it twice. The connection is still closed at once, but the error is
  now reported from the event loop (a 0 ms timer), which a free before it fires cancels.
- **HTTP/2 server: a request one field past the header cap is refused, not truncated.** When a
  request carried as many fields as KEEL keeps, an `:authority` and no `host` field, the last field
  the client sent was dropped silently to make room for the synthetic `host`. Such a stream, and
  one a session hands over with more fields than the cap, is now refused with 431, as an over-cap
  request is.
- **mbedTLS: every write fails after a refused shorter retry.** After a blocked write's retry with
  a shorter length was refused, mbedTLS still held the record built from the blocked write, so a
  later write flushed it and was acknowledged for bytes that never went out. The adapter now fails
  every write after that refusal, until `reset`.
- **DNS resolver: the internal entropy test hook checks the resolver's type.**
  `kl_dns_resolver_set_random` cast any `KlResolver` to the DNS resolver and wrote past the end of
  one that was not (a cache wrapper, a custom vtable). It now changes only a DNS resolver.
- **A handler that set a 1xx status sent it as a final response.** Since the status table was
  widened to every code, `kl_http_response_status(res, 103)` (or any 100-199) went out as a final
  response with `Content-Length` and a body. A client takes a 1xx as interim, so it waited for a
  final response that never came, or read the body as the next status line. A handler-set 1xx is
  sent as 500 again; `100 Continue` and the WebSocket `101` are written by their own paths.
- **PROXY protocol v1: fields must be separated by single spaces.** The header was split on runs of
  spaces, so doubled, leading and trailing separators were accepted; a NUL inside the line ended
  the parse early and whatever followed it up to the CRLF was consumed unchecked; and a port could
  have leading zeros. Each of these is now refused.
- **WebSocket client: a close that cannot be sent fails the connection.** When no Close frame could
  be written (no entropy for the frame mask, or a write error), `kl_ws_client_close` still left the
  client waiting in the closing state until the peer acted. It now fails the connection with an
  error at once.
- **Build: `getrandom` is not used on Android below API level 28,** where `<sys/random.h>` exists
  but does not declare it (the build failed under `-Werror`).
- **A redirect to an absolute path from a URL with no path kept the base's query or fragment.**
  `kl_url_resolve` ended the base URL's authority only at a `/`, so `/login` resolved against
  `http://example.com#top` gave `http://example.com#top/login`, which parses as the path `/`. The
  authority now ends at the first `/`, `?` or `#`, giving `http://example.com/login`.
- **The sync HTTP client no longer fails a request when a signal interrupts its wait.** Its
  readiness wait is a bare `poll()`, which returns EINTR when a signal lands (SIGCHLD, a profiler's
  SIGPROF), even for a handler installed with SA_RESTART on Linux. The request failed. An
  interrupted wait now counts as not ready yet: the client checks its deadline and waits again.
  Windows (WSAPoll) is unchanged.
- **DNS: a nameserver listed twice no longer drops its own answers.** A reply is matched to the
  nameserver it came from by address, and the first entry with that address was taken. With
  `nameserver 10.0.0.1` listed twice in resolv.conf, a retry sent to the second entry had its answer
  dropped as coming from the wrong server, so each such try cost a full timeout. A repeated
  nameserver is now kept once (a retry to the same server is no failover), and the attempt budget
  counts distinct servers.
- **DNS: a query that cannot get entropy ends the request instead of moving on to another name.**
  A query whose id, case pattern or cookie could not be drawn from the OS RNG is refused, and that
  refusal was taken like a search candidate that cannot be encoded: the resolver moved on to the
  next candidate and could answer for a different name than the one asked. A refusal for want of
  entropy now fails the request (`resolve()` returns NULL, or the callback gets `KL_ERR_DNS`); only
  an unencodable candidate is skipped.
- **mbedTLS: a write retried with a shorter length is refused.** After a write would block, mbedTLS
  holds a record built from the length it was given, and a retry flushes that record and reports the
  length it is called with. A retry with less would have only that much acknowledged, and the caller
  would send the rest again, duplicating bytes in the stream. Such a retry now fails the write (-1).
  A retry must pass at least the length just attempted, as KlDrain and every in-tree caller do;
  equal and longer retries are unchanged. The contract is documented in `keel_tls_mbedtls.h`.
- **Completion loops: a streamed response that is not a pooled connection's own is refused (memory
  safety).** A streamed response's writer took any response bound to a completion engine for the one
  embedded in a pooled connection and found "its" connection from the response's address. An HTTP/2
  stream's response is not inside a connection, so a streamed HTTP/2 response (SSE, `begin_stream`)
  on IOCP, io_uring or pollcomp read and wrote unrelated memory. The server now marks the response it
  embeds (the otherwise unused `KlHttpResponse.stream_inflight` field), and any other response is
  refused.
- **Completion loops: long transfers that keep moving are no longer cut off.** A streamed response
  closes once its queued output is out, and that close was timed against `read_timeout_ms` before
  send progress was counted, so a long streamed download was released mid-transfer. The output queue
  now posts at most 256 KiB per send (each part completes, so progress shows on every backend, and the
  backend's copy stays small), and pollcomp and IOCP count file bytes sent zero-copy as progress.
- **Completion loops: WebSocket output to a client that stops reading is bounded.** Every frame went
  onto the connection's output queue, which took everything, so a client that never read its frames
  grew server memory without bound. Past 1 MiB queued, a send is refused as would-block, as a full
  socket is on readiness: the WebSocket drain keeps the frame within its own limit, or the send fails.
- **Completion loops: a failed send post no longer leaves its bytes queued, a graceful drain no
  longer resumes accepting, and an accept back-off wakes the loop on time.**
- **WebSocket auto-ping: a client taking a large backlog is no longer failed as dead.** A ping
  queued behind a backlog of frames cannot be answered until the client has read that far, and on
  a completion loop the server posts no receive while the connection's output is queued, so even a
  PONG it sent is not read. The auto-ping counted only bytes received as an answer: a live client
  slowly reading a backlog longer than one interval was sent Close 1001 and closed. A ping sent
  behind queued output now counts that output moving (bytes a completion engine's sends moved, or
  the drain flushed onto a readiness socket) as the answer, and pings again. A backlog that does not
  move is given the server's read timeout before the peer is taken for dead, since a slow reader can
  spend seconds on what a full socket send buffer already holds, none of which shows as progress.
  A ping sent with nothing ahead of it is still answered only by bytes received, as before (its own
  send is never an answer). On IOCP an overlapped send completes whole, so its progress shows only
  when it completes.
- **HTTP/2 server: a long download is no longer timed out while its response is going out.** KEEL
  forgets a stream once its response is submitted, so with no stream left and nothing the session
  still wanted to write, the idle sweep took the connection for idle, and the idle clock moved only
  on reads. A download that took longer than the read timeout was closed once its last bytes
  reached the kernel (readiness), or as soon as the session had handed all its DATA to the output
  queue (completion). Output the session moves is now activity: each send that moves bytes restarts
  the idle clock, and on a completion loop the sweep counts send progress for HTTP/2 connections as
  it does for HTTP/1.1 ones.
- **HTTP/2 server: an idle connection is closed with a GOAWAY.** The idle sweep closed an idle
  HTTP/2 connection without one, so the client could not tell the close from a failure (RFC 9113
  6.8 says an endpoint SHOULD send GOAWAY before closing). The sweep now submits a graceful GOAWAY
  and flushes it first; on a completion loop the connection closes once the GOAWAY is out.
- **HTTP/2 server: a graceful shutdown closes each connection as soon as its session is done.**
  After the drain's GOAWAY, a session with no stream left wants neither read nor write, and the
  connection should close. That was checked only after a read, so a connection whose last response
  finished on write readiness, or that was already idle, stayed open until the idle timeout or the
  drain deadline, and the shutdown waited out its whole deadline. The check now also runs after a
  write-readiness flush and in the sweep, which closes a done session at once.
- **HTTP/2 server: the Host field made from `:authority` is no longer dropped at the field cap.** A
  request with `KL_MAX_HEADERS` regular fields, an `:authority` and no `host` field lost the
  synthetic `host` field the server adds for handlers, silently. It now keeps a slot of its own: at
  the cap, the last regular field gives way.
- **nghttp2 server adapter: request trailers are a header list of their own.** The header-list
  budget (`SETTINGS_MAX_HEADER_LIST_SIZE`) and the field cap carried over from the request's
  headers into its trailers, so a request whose headers and trailers each fit was reset. Each
  header block now starts its own budget (RFC 9113 6.5.2); trailer fields are not stored (KEEL
  takes no trailers), so the field cap stays the request block's.
- **nghttp2 client adapter: pseudo-header fields count toward the response header-list budget.**
  `:status` returned before it was counted, so a response could exceed the client's header-list
  limit by its pseudo-header fields. Every field now counts (RFC 9113 6.5.2).
- **Completion server: a failed accept post could crash the server.** When the next accept could not
  be posted, the listener returned its credit through the pool's release hook. The HTTP server's
  release hook announces the free slot, and that re-entered the listener while it was still
  listening: it posted again, failed again, and recursed until the stack overflowed. This happened
  on a server whose accept posts kept failing, for example one out of descriptors. Returning a failed
  post's credit no longer re-enters the listener; it posts again on the next sweep.
- **Completion server: a failed accept post no longer stops all accepts.** On a completion loop
  (IOCP, io_uring, pollcomp), posting the next accept can fail for a moment: no memory for the op,
  a full submission queue, a `WSASocketW` or AcceptEx call that fails, for example because a queued
  connection was reset before AcceptEx took it. The listener treated any such failure as a broken
  listen socket and closed. The server kept running and never accepted another connection. An arm
  hook can now report a transient failure (`KL_LISTENER_ARM_RETRY`). The listener then returns the
  credit and pauses, and the server's sweep posts again on its next tick.
- **Status codes missing from the server's table were sent as 500.** The HTTP/1 status line came
  from a fixed table of common codes, and any other code (412, 416, 426, 451, 501, ...) went out as
  `500 Internal Server Error`. Every registered code now has its reason phrase, and any other code
  from 100 to 599 is sent as itself with an empty reason phrase, which HTTP/1.1 allows. Only a value
  outside 100-599 is still sent as 500.
- **A route registered without a handler crashed the server on its first request.**
  `kl_http_router_add` (and `kl_http_server_route` and the streaming variants built on it) accepted
  a NULL handler, which the first matching request called. It now returns -1. WebSocket routes,
  which carried a NULL handler, now carry one that answers 404 when the request is not upgraded (an
  HTTP/2 stream on the route, as before, or a synthetic router dispatch, which crashed).
- **A failed compression header append left Content-Encoding on an uncompressed body.**
  `kl_http_response_body_compress` and `kl_http_compress_stream_begin` added `Content-Encoding` and
  then `Vary`; when the `Vary` append failed (out of memory) they returned -1 with `Content-Encoding`
  already in the header block, so a caller that then sent the body as is labelled plain bytes as
  compressed. The two headers are now added as a pair: on failure the header block is left as it was
  before the call.
- **Completion loops: a long download that keeps moving is no longer cut off at the read timeout.**
  A buffered or file response is one send op until all of it is out: the engine re-posts the rest
  of a partial send itself and reports nothing until the end. The idle clock only moved when the
  whole response completed, so on io_uring and pollcomp a download that took longer than
  `read_timeout_ms` (30 s by default) was cut off however steadily the client read. The engines now
  count the bytes each posted send moves (`KlStream.send_progress`), and the idle sweep treats any
  movement as activity. A client that stops reading is still timed out.
- **IOCP: a failed or cancelled send is no longer re-posted.** A send that completed with an error,
  or was aborted by the cancel at close after moving some bytes, was re-posted for the rest. To a
  client that had stopped reading, that op never completed, so the closing connection never gave
  back its slot. The send's own status is now checked: a failed send is reported as failed and never
  re-posted.
- **A signal ended `kl_event_ctx_run` loops on epoll, kqueue and poll.** A signal that arrived
  while the loop waited (SIGCHLD, a profiler's SIGPROF) made the wait fail with EINTR, which
  `kl_event_wait` and `kl_event_ctx_run` returned as -1, so a caller's
  `while (kl_event_ctx_run(...) >= 0)` loop stopped. An interrupted wait now returns 0, a tick with
  no events, as io_uring already did; due timers still fire.
- **A server out of file descriptors spun at full CPU.** When `accept()` failed with EMFILE,
  ENFILE, ENOBUFS or ENOMEM, the connection stayed queued and the listen socket stayed ready, so
  the loop retried at once and failed again without pause: readiness backends woke on the listen
  socket every tick (logging each failure), io_uring re-posted the accept immediately, and pollcomp
  kept the accept op and completed it on every poll. Accepting now pauses for 100 ms and then
  resumes, so a server at its descriptor limit idles until one frees and then serves the queued
  connection. Other accept failures (a peer that reset) still retry at once.
- **IOCP: the TransmitFile chunk size was read from the environment.** The library read
  `KEEL_IOCP_TF_CHUNK`, a test seam, in every build, so the environment of a production process
  could shrink each TransmitFile call to a single byte. Only a test build of the IOCP backend
  (`-DKEEL_IOCP_TEST_HOOKS`, linked into `smoke-iocp`) reads it now.
- **WebSocket server: NULL arguments to the public API crashed.** `kl_ws_server_config_init(NULL)`
  wrote through the pointer, `kl_http_server_ws_upgrade` crashed on a NULL server and registered a
  dead route for a NULL config, and the send functions read through NULL data given a nonzero length.
  `kl_ws_server_config_init(NULL)` is now a no-op, and the others return -1.
- **Internal: write-only fields removed.** The WebSocket server's recorded close code, and the HTTP/2
  server stream's `headers_done` and `body_done` flags, were set and never read. They are gone; both
  structs are internal, so the public API is unchanged.
- **miniz: a streamed gzip response arrived corrupt once its last block passed 4 KiB.** The
  streaming compressor's finishing call ran the deflater once with a 4 KiB output buffer, so the rest
  of a larger final block stayed inside it and the gzip trailer followed an incomplete stream: any
  streamed compressed response of roughly 15 KiB of text or more could not be decoded. It now drains
  the deflater until the stream is done. Also, the decompressor no longer accepts bytes after the gzip
  trailer (trailing garbage, or a second member it does not read) as part of a verified body.
- **The sync client's `timeout_ms` restarted with every read.** It was a per-wait timeout, so interim
  1xx responses or a server trickling its response a byte at a time held the call open for as long
  as the server kept it up. `timeout_ms` is now one deadline over the whole request, measured from
  the call, as it already was for the async client. **Behavior change:** a sync download that keeps
  moving but takes longer than `timeout_ms` in total now fails with `KL_ERR_TIMEOUT`; raise
  `timeout_ms` for long transfers.
- **The HTTP client tried only the first address a name resolved to.** The sync client always, and
  the async client with `system_dns` (and on freestanding builds without a resolver), connected to
  the first address alone, so a host whose first address refused or did not answer (often `::1` for
  `localhost`, or a broken IPv6 route) failed with `KL_ERR_CONNECT` though another address would
  have worked. The sync client now tries each address in turn, giving each an equal share of the
  time left; the async client races the list with Happy Eyeballs, as it does for a resolver's.
- **A custom resolver's socket type became the HTTP connection's.** The async client opened its
  connection with the `ai_socktype` / `ai_protocol` from the resolver's result, so a resolver that
  reported a datagram type got a UDP socket and a request that never completed. The connection is
  now always TCP.
- **The redirect API checked its arguments less than the client does.** `kl_http_redirect_request`
  and `_pooled` accepted a negative header count or a NULL header array (and walked it), silently
  dropped headers past `KL_HTTP_CLIENT_MAX_REQ_HEADERS`, and returned -1 for a bad allocator, URL,
  method or pool without touching `*resp`, so its `error` was whatever the caller left there. The
  header arguments are now checked as the client checks them, and every refusal leaves `*resp` zeroed
  with `error` set (`KL_ERR_INVALID_ARG`, or `KL_ERR_URL` for a URL too long). `kl_http_redirect_start`
  and `_start_pooled` return NULL for the same bad header arguments.
- **URL parsing let a `?`, `#`, space or control byte through.** `kl_url_parse` ended the authority
  only at `:` or `/`, so in `http://host?q` or `http://host#f` the query or fragment became part of
  the host name; a fragment went out in the request target; a space, tab or other control byte in
  the path reached the request line (only CR and LF were refused); and `HTTP://` was an unsupported
  scheme. The authority now ends at `/`, `?` or `#`; the path stops at the fragment; a space or
  control byte in the host or path is refused; and schemes match in any case (also in
  `kl_url_resolve`). A query with no path before it (`http://host?q`) is refused, since its target
  `/?q` is not a span of the caller's string. Relative references (`../x`) are still not resolved.
- **The proxy CONNECT reply was checked by three digits.** Any reply whose bytes 9 to 11 were `200`
  after `HTTP/1.` opened the tunnel, so `HTTP/1.1 2000` or `HTTP/1.1X200` passed, and a `2xx` other
  than 200 failed. The reply now needs a whole status line (`HTTP/1.` digit, space, three-digit code,
  then a space or the end of the line), and any 2xx opens the tunnel (RFC 9110 9.3.6).
- **`kl_http_client_pool_free` left the pool's `capacity` over a freed table.** A later call on the
  freed pool walked `capacity` slots of a NULL table. `capacity` is now 0 after a free.
- **DNS: SERVFAIL, NOTIMP and REFUSED now fail over to the next nameserver.** Such an answer ended
  the query for that address family at once, as if the name had no address, so one broken or
  misconfigured nameserver failed every lookup even when the next one in `resolv.conf` would have
  answered. These rcodes now move the query on to the next nameserver, as a timeout does, and the
  lookup fails only once every try is spent. NXDOMAIN still ends the lookup immediately.
- **DNS: a search-list candidate that cannot be queried is skipped.** A candidate name that does not
  encode as a query (for example, a search domain with a label over 63 bytes) stopped the whole
  lookup: `resolve()` returned NULL when it came first, and the lookup failed when it was reached
  later. The resolver now moves on to the next candidate and fails only when none can be queried.
- **Completion loops: a finished rejection drain held its slot, and a graceful stop could cut queued
  TLS output.** A rejected client that sent the rest of its declared body and kept the connection
  open held its slot until the drain deadline on io_uring, IOCP and pollcomp; the drain now ends once
  the framing is complete and a receive comes back short (nothing more queued), as on readiness. And
  the shutdown drain counted a connection closing once its queued TLS output was out (a response, a
  WebSocket Close, an HTTP/2 GOAWAY) as idle, so a graceful stop could end under that send; a
  connection with operations in flight now counts as active.
- **A streaming gzip response with no body failed as a decompression error.** The miniz backend
  (since the truncated-header check) failed the final call of a gzip stream that had received
  nothing at all, which a streaming client makes at the end of every response with
  `Content-Encoding: gzip`: a HEAD, a 204, a 304 or an empty 200 then failed with `KL_ERR_COMPRESS`.
  A stream with no bytes now ends cleanly; one that ends inside its header still fails.
- **A rejected upload whose client went quiet could hang a completion-loop server.** After a
  rejection (a 413), the idle sweep read the connection's socket synchronously to drain the rest of
  the upload. On io_uring the accepted sockets are blocking, so a client that then sent nothing more
  but kept the connection open parked the whole server in that read; with TLS the read also took the
  client's ciphertext away from the engine, so the 413 itself never arrived. On a completion loop the
  posted receive now does the draining and the sweep only enforces the deadline. Related, on completion
  loops: a receive completing on a connection the sweep had only cancelled (an IOCP race) no longer
  leaks the slot; a response queued after a long async suspension is no longer cancelled at once by a
  stale idle clock; and a client that half-closes while its final TLS response is still queued gets it.
- **Edge cases found re-auditing the previous round.** An HTTP/2 response no longer carries
  `Upgrade`, `Proxy-Connection` or `TE` (only `Connection`, `Transfer-Encoding` and `Keep-Alive` were
  dropped): clients reset a stream that has them, and stream 1 of an h2c upgrade now carries the
  HTTP/1.1 middleware's headers. A gzip body that ends inside its own header is rejected at end of
  input instead of accepted as empty. When a legacy streaming handler's response came from the body
  reader's `on_error` (a failed head copy), a 500 no longer follows it. `kl_http_server_run` stopped by
  an error (a failed event wait) now returns -1 and clears `running`. A file `sendfile` interrupted
  before sending anything is retried instead of cutting the response. The IOCP watcher retry count
  saturates. The lwIP NO_SYS engine sleeps at most 10 ms per idle drain, so its timers keep running.
  Documented: an HTTP/2 session's `upgrade` runs before the 101 and must not send, and a file response's
  descriptor must be binary on Windows.
- **gzip responses over a few KB failed to decompress (miniz streaming decompressor).** The miniz
  backend's streaming `dfeed` decompressed into a 4 KiB buffer used as a wrapping dictionary that
  restarted at every call, but deflate matches reach 32 KiB back: any match more than 4 KiB back read
  garbage and the CRC check failed. Ordinary HTML and JSON repeat at such distances. Since the
  buffered client path started decompressing through `dfeed` (to bound inflation while it happens),
  this hit every buffered gzip response, not only streaming ones. The decompressor now keeps a 32 KiB
  dictionary ring across calls, and also drains output still held when its input runs out.
- **The WebSocket and HTTP/2 clients wrote the authority wrongly, and AF_UNIX requests sent
  `Host: localhost:0`.** The WebSocket upgrade's `Host` dropped a non-default port and the brackets of
  an IPv6 literal (`ws://[::1]:9000/` sent `Host: ::1`), and the HTTP/2 client's `:authority` did not
  bracket IPv6 (`::1:8443`). The HTTP client's own fix appended the URL's port to an `http+unix`
  request, whose port is 0. All three clients now share one builder: IPv6 bracketed, the port only
  when it is not the scheme's default, and no port for a socket path.
- **An h2c upgrade dropped what the HTTP/1.1 middleware set, and a WebSocket server could pong after
  its Close.** The HTTP/2 stream that answers an `Upgrade: h2c` request was built fresh, so the
  pre-body middleware's `req->ctx` and the response headers it had added (CORS, for one) were lost on
  the upgraded response; both are now carried over to stream 1. And a Ping that arrived after the
  server had sent its Close (or stopped sending after a cut frame) was still answered with a Pong,
  a data frame after the Close (RFC 6455 §5.5.1) or a frame after a broken one; it is no longer
  answered once the server has stopped sending.
- **On a completion loop, TLS output could stall every connection, and a rejection over TLS lost
  its response.** TLS ciphertext was pushed with a synchronous send on the loop thread, so a client
  that stopped reading a large TLS response (a stream, WebSocket or HTTP/2 output, the handshake)
  blocked every connection on the loop: indefinitely on IOCP and io_uring, whose accepted sockets
  are blocking, and up to 30 s per stall on pollcomp. And a request rejected on a TLS connection
  (an over-limit body, a refused reader, a malformed chunk) had its 413 written into the TLS
  engine's buffer and the socket half-closed before that buffer was sent, so the client saw only
  FIN. All TLS output on a completion loop now leaves through one per-connection queue of
  overlapped sends, in order, one at a time; a response completes, the drain half-closes and a
  connection closes only once its queued output is out.
- **HTTP client decompression and HEAD reuse.** The bounded decompression path (any
  `max_response_size`) passed the whole body to `dfeed` with `flush=1`, where `decompress.h` has the
  final call carry no data, so a backend that holds the contract refused the response. It now feeds
  the body with `flush=0` and finishes with an empty `flush=1` call. And a keep-alive HEAD response
  with neither Content-Length nor chunked encoding was dropped from the pool: the llhttp response
  parser asked whether to keep the connection before llhttp knew the response had no body.
- **Server start-up and body-read edge cases.** `kl_http_server_run` sets `running` before the bind
  (so a stop during start-up is not lost), but a start-up that then failed returned -1 with
  `running` still set; it is cleared again. A streaming-async handler that suspended before its
  body was read could later read the body over the request head its `req` still pointed at; the
  head is now kept before the handler runs. When keeping the head could not allocate, the body
  reader was dropped with only `destroy`; it now gets `on_error` first, as on any other failure
  mid-body. `kl_http_request_pause_body` now documents that over TLS on a readiness backend the rest
  of the record already decrypted is still delivered (at most one TLS record).
- **An HTTP/2 client request issued from `on_resp` re-entered the session.** `on_resp` runs inside
  the session's receive (nghttp2's stream-close callback during `mem_recv`), and
  `kl_http2_client_request` flushed the session right there, re-entering its send from inside its
  receive, which nghttp2 does not support. Issuing the next request from `on_resp` (the common
  pattern) could mis-process later frames in the same batch. A request issued inside a client
  callback is now flushed once the session call has returned.
- **HTTP/2 and WebSocket edge cases.** An HTTP/2 HEAD whose response fell back to a 500 (a
  streaming body, an oversized or unreadable file) was sent the error text as a body; it now gets
  headers only. An HTTP/2 file response whose file was shorter than its declared size (it shrank
  after the handler sized it) went out as a complete 200 with the short body, which the client
  could not tell from a whole one (no content-length); it is now a 500. A WebSocket server frame the
  drain cut short (over `max_size`, or a socket error) left the connection open with no close
  scheduled, though every later send failed; it now closes, as when the direct path cuts a frame.
  And the WebSocket client's drain writer took a TLS write error for a full socket when an earlier
  call had left `EAGAIN` in `errno`, so the client stalled instead of failing; would-block is now
  asked of the socket provider, and only for a plaintext write.
- **Engine edge cases.** `kl_timer_next_timeout` with `max_ms = -1` (no cap) returned a negative
  timeout for a timer more than about 24.8 days out; it now clamps to `INT_MAX`. A datagram receive
  in batch mode (`recvmmsg`) refilled from the socket until the kernel had nothing, so one readable
  event under a flood never returned to timers and other sockets; it is now bounded like the serial
  path (64 datagrams, then only what the batch already holds). And on macOS a `sendfile` interrupted
  by a signal (EINTR) after sending part of the file returned -1, so a caller that retried sent
  those bytes again; the partial count is now returned.
- **HTTP/2 h2c upgrade refusals and uploads after an early response.** A session that refused an
  h2c upgrade's HTTP2-Settings (a value out of range, more settings than the nghttp2 adapter takes)
  was asked only after the 101 was written, so the connection then closed with no response at all;
  the session is now asked first, and a refusal answers the request over HTTP/1.1. And after a
  response that ended a stream the client was still uploading (a 413 for an over-limit body), the
  nghttp2 adapter never reset the stream, and nghttp2 kept opening the flow-control windows, so the
  server received and discarded the client's whole upload; it now sends RST_STREAM(NO_ERROR)
  (RFC 9113 §8.1), and the upload stops at the window.
- **A stop during server start-up was lost, and the server ran on.** `kl_http_server_run` bound the
  socket, published the port and logged "listening" before it marked the server running, so a
  `kl_http_server_stop` from another thread (or the SIGTERM/SIGINT handler) in that window was
  overwritten: the loop kept going, and a caller joining the server thread waited forever. The
  server is now marked running before anything else can see it start.
- **A full send buffer ended a plaintext HTTP/2 server connection.** The server's socket writer
  returned the socket's -1 when a send would block, and the session treats any -1 from its send
  callback as fatal, so a slow reader of a large response lost the whole connection (every stream on
  it). A would-block on plaintext is now reported as "nothing sent yet", as TLS already reports
  WANT_WRITE: the session keeps the bytes and WRITE interest sends them once the socket drains.
- **macOS: a large file response was cut short and reported sent.** When the socket's send buffer was
  full, the Darwin `sendfile` wrapper returned 0 for "nothing sent, would block", and every caller
  reads 0 as end of file: the response stopped part way and the connection carried on as if it had
  completed. A would-block with nothing sent is now reported as one, so the send resumes when the
  socket drains.
- **The nghttp2 adapters leaked the state of streams still open at teardown.** Destroying a client
  or server session freed nghttp2's own session but not the per-stream state the adapter had
  allocated for streams that had not closed (a connection dropped mid-request, a queued response).
  Each adapter now tracks its live streams and frees them when the session is destroyed.
- **pollcomp: a large TLS stream to a slow reader was cut short.** Flushing TLS output on the
  completion server gave up when the socket would block, so a streamed response larger than the
  socket buffers lost its tail when the client read slowly. The flush now waits for the socket to
  become writable (bounded) and carries on.
- **A `wss://` handshake could hang on a large 101.** A 101 response larger than the first read but
  inside one TLS record left the rest of it held in the TLS engine, and the client waited for socket
  readiness that never came (there is no handshake timeout). The handshake now drains what TLS holds,
  and frames that arrive in the same record as the 101 are processed at once.
- **A WebSocket server frame cut short by a full socket desynced the stream.** Without the drain
  (`kl_ws_server_enable_drain`), a send that the socket took only part of returned an error but left
  the connection open; the next send, once the client had read and the socket had room, started a
  frame header inside the cut frame's payload, and the client parsed garbage. After a cut frame the
  connection now only closes: further sends fail, and it is closed at once.
- **The HTTP/2 client could stall a large request body, and could be freed under its own flush.**
  When the socket took only part of the session's output, the rest stayed buffered in the session
  while the client watched only for READ, so a body larger than the send buffer could stall against
  a peer that sends nothing. The client now asks for WRITE while output is held back and flushes when
  the socket drains. Separately, `kl_http2_client_request` flushed outside the client's event guard,
  so an `on_resp` that called `kl_http2_client_free` during that flush destroyed the session under
  it; the free is now deferred until the flush returns.
- **The HTTP/2 client reported a 1xx in place of the final response.** nghttp2 delivers the final
  response that follows a `103 Early Hints` as a further HEADERS block, which the nghttp2 client
  adapter did not report, while it kept appending headers across blocks: after `103` then `200`, the
  client reported status 103 with the 200's body. The adapter now reports the first final (2xx and
  up) response, with that response's headers only.
- **One HTTP/2 stream's body limit ended the whole connection.** A request body over
  `max_body_size`, or a body reader refusing data, was reported to the session as a fatal error: no
  413 was sent, and every other stream multiplexed on the connection died with it. The stream is now
  answered 413 and closed on its own; the connection and its other streams carry on.
- **Substrate Lows (seventeenth audit).**
  - **A redirect from one AF_UNIX socket to another kept the caller's credentials.** Both URLs have
    no host or port, which the origin check compared as equal; the socket path is now the origin, so
    `Authorization` and `Cookie` are dropped between different sockets.
  - **A timer delay near `UINT64_MAX` wrapped and fired at once.** The deadline now saturates.
  - **Multicast calls on a closed datagram** handed the provider an invalid descriptor and reported
    `KL_ERR_IO`; they now report `KL_ERR_INVALID_ARG` with no provider call, as the other calls do.
  - **An AF_UNIX peer path could include stale bytes** past the length the kernel returned.
  - **One readable datagram socket could hold the loop.** A readiness receive now delivers at most
    64 datagrams per event and returns to the loop; the rest follow on the next ticks.
  - **A Windows named-pipe name with `.` or `..` segments passed the locality check.** Win32
    collapses them before the open, so `\.\pipe\..\UNC\host\pipe\x` reached a remote pipe. Such
    names (and `/`) are now refused.
  - **The Windows wakeup pair kept Nagle on**, so a signal could wait for a delayed ACK (up to
    ~200 ms). It now sets `TCP_NODELAY`.
- **An early rejection of a keep-alive request could be cut off by a client that over-sent.** When
  pre-body middleware rejected a request (401, 413, 415), the server counted the bytes already read
  past the headers only up to Content-Length, so a client that sent more than it declared was not
  seen to have done so: the connection then closed on unread bytes and the client could get a reset
  instead of the response. The bytes are now counted as they are, and the over-send is drained.
- **Completion server body reads (seventeenth audit).**
  - **A slow but steady upload was timed out.** On a completion loop a connection's activity time
    was refreshed only when a request began, so an upload that kept sending, but took longer than the
    idle timeout in all, was closed mid-body. Every completed read and write now counts as activity.
  - **Pausing and resuming in `on_data` could corrupt the body.** A resume from inside the body
    drive posted a second receive while the drive was still delivering the first, so bytes could
    arrive out of order or twice. The drive now finishes what it holds before a new receive is posted,
    and only one receive is ever in flight.
  - **A paused TLS body did not stay paused.** The TLS body loop kept decrypting and delivering
    records already held by the TLS engine after the reader paused. It now stops at the pause.
  - **A finished plaintext stream on a completion loop could hold its slot.** The timeout sweep now
    releases a sending, drain-enabled stream that has nothing left in flight.
- **HTTP server Lows (seventeenth audit).**
  - **A body rejected inside the bytes read with the headers closed the connection with no
    response.** A reader refusing data, an over-limit body or a malformed chunk in that leftover got
    no 413 and no drain, unlike the same rejection a read later. It is now answered 413, the same way.
  - **CORS: `Vary: Origin` was sent only when the origin was allowed.** Responses to a missing or
    disallowed Origin vary by Origin too, so a cache could serve an allowed origin's response to
    another. With an allowlist configured, every response now carries `Vary: Origin`.
  - **A HEAD stream never finished.** `kl_http_response_end_stream` returned early for HEAD without
    marking the stream ended, so a streamed HEAD response stayed in SENDING.
  - **`kl_http_server_init` leaked the PROXY-protocol trust list** on every failure after building
    it.
- **HTTP client Lows (seventeenth audit).**
  - **The Host header and request target lost the port and the IPv6 brackets.** A URL with a
    non-default port sent `Host:` without it, and an IPv6 literal went out unbracketed in `Host`,
    the absolute-form target sent to a proxy, and `CONNECT` (`[::1]:8080` became `::1`). All of them
    now carry the authority as RFC 9110 7.2 writes it.
  - **An HTTP/1.0 response without keep-alive was pooled.** The pool decided reuse from the
    `Connection` header alone, so a 1.0 server that closes after each response had its closed
    connection handed to the next request. `KlHttpClientResponse` gains `closes` (appended), set
    when the server will close the connection after this response (llhttp's keep-alive verdict, which
    covers the version), and the pool keeps no such connection.
  - **An async request with an error already waiting kept acting on socket events.** Between an
    error being recorded and its deferred report, the request's socket watcher stayed live, and an
    event could run the state machine again (starting a second TLS session, leaking the first). The
    watcher now ignores events while a completion is pending.
  - **An async redirect hop that completed inside its own start wrote to a freed client.** When the
    hop's client could not defer an error (no memory for its timer), it completed inline, before the
    redirect client recorded it; an `on_done` that freed the redirect client was followed by a write
    into it. The completion is now held until the hop is recorded. On the first hop the start fails
    instead (`NULL`, no callback), as `kl_http_client_start` does.
- **WebSocket and HTTP/2 Lows (seventeenth audit).**
  - **An HTTP/2 HEAD response carried a body.** The server sent the handler's body in DATA frames,
    which a strict client treats as a protocol error. HEAD now sends the headers only, as HTTP/1.1
    does.
  - **An h2c Upgrade ran the pre-body middleware twice** (once for the HTTP/1.1 request, again for
    its stream 1), so a rate limiter or audit log counted the request twice. Stream 1 of an upgrade
    no longer runs it again.
  - **An h2c Upgrade was honoured over TLS.** h2c is cleartext only (RFC 7540 3.2); over TLS the
    `Upgrade` is now ignored and the request served as HTTP/1.1.
  - **A malformed `HTTP2-Settings` was found only after the 101.** Its shape (base64url of whole
    settings) is now checked first, so a bad one is declined over HTTP/1.1.
  - **A bodiless HTTP/2 request met a body reader that needed a body.** With no content-length the
    route's reader was made at HEADERS time even when the request ended there, and a reader that
    needs a body (a multipart reader without a Content-Type) answered 415. Without a content-length
    the reader is now made on the first DATA frame.
  - **The HTTP/2 server leaked live streams when setting up a connection failed part way** (the
    prior-knowledge path), and the nghttp2 server adapter left a stream it could not take with
    neither a response nor a reset (it now resets it with INTERNAL_ERROR). A partial out-of-memory
    failure growing its header arrays freed them at the wrong size.
  - **The nghttp2 client adapter sent `:scheme https` on cleartext h2c**, and dropped a response
    header it could not store without saying so (the stream is now reset). `KlHttp2ClientSession`
    gains `keel_cleartext` (appended, Keel-managed) so an adapter can tell.
  - **The WebSocket client left a close echo in TLS until the next socket event**, and both clients
    called `tls->pending` without checking the backend provides it. The server's buffered WebSocket
    writer read a stale `errno` after a TLS write error.
- **Engine and DNS Lows (seventeenth audit).**
  - **A file response shorter than its declared length was reported sent** on readiness, pollcomp
    and io_uring (the file shrank after the handler sized it), leaving a keep-alive connection open
    with the body short of its Content-Length. The connection now closes, as on IOCP.
  - **IOCP: a transient watcher re-post failure marked the socket dead**, and a healthy socket was
    then reported ready on every drain (a busy loop). Out-of-buffers and similar errors are now
    retried on each drain, with the loop's wait capped at 10 ms while a retry is pending, for as long
    as the pressure lasts; only a permanent error (a reset or closed socket) marks the watcher dead.
  - **DNS: a cookie-less truncated reply from a server known to send cookies forced the TCP
    fallback.** The cookie check now runs before the truncation branch.
- **A paused request body poisoned, or leaked, its connection slot.** `kl_http_request_pause_body`
  set a flag on the connection that nothing cleared, so after that client left, the next connection
  given the same pool slot started paused, and its upload stalled until the body timeout answered
  408. On a completion loop (io_uring, IOCP, pollcomp) it was worse: nothing is posted while a body
  read is paused, so when the timeout sweep tried to reclaim the connection by cancelling its pending
  op, there was none, and the slot, socket and accept credit were never released. A client could
  exhaust the pool one slot at a time. The pause is now cleared with the rest of the per-request
  state, and the sweep releases a timed-out completion connection that has nothing posted directly.
- **`kl_datagram_send` used freed memory after a teardown from `on_drain`.** On a readiness loop, a
  send that flushes queued datagrams can empty the queue, which fires `on_drain` inside the call. A
  teardown (or a close whose `on_close` frees the datagram) requested there ran when the send core let
  go, and the facade then updated the datagram's WRITE interest through the freed core. The batch and
  GSO sends already held the datagram across the whole call; the single send now does too, so the
  teardown runs after it, once.
- **HTTPS, WSS and HTTP/2-over-TLS servers dropped connections when a TLS record arrived in
  pieces.** On a real network a TLS record often spans several TCP segments, so a readable event can
  carry part of one. The TLS engine buffers it and `read()` returns 0, which the `KlTls` contract
  defines as WANT_READ (-1 is error or close). On readiness loops (epoll, kqueue, poll, WSAPoll), the
  HTTP/1 server's header and body reads, the WebSocket server and the HTTP/2 server read that 0 as end
  of stream and closed the connection, mid-request or mid-message. Loopback tests rarely split a
  record. A 0 on a TLS connection now means wait for the rest. The WebSocket server also drains
  plaintext the engine already holds (`tls->pending()`), as the HTTP/1 and HTTP/2 servers do: a record
  larger than its 8 KiB read buffer used to wait for the peer's next send. Completion loops take a
  separate TLS path and were not affected.

- **Keel's TLS clients failed or stalled when a record did not arrive, or leave, in one piece.**
  The same `KlTls` contract (0 = WANT_READ / WANT_WRITE, -1 = error or close) was misread by the
  clients.
  - The async HTTP client failed a request with `KL_ERR_IO` when a write found the socket's send buffer
    full, so a TLS upload larger than that buffer failed outright.
  - The WebSocket client closed when part of a record arrived, during the handshake ("connection closed
    during handshake") or after it.
  - The HTTP/2 client did the same ("connection closed").
  - The sync client polled the socket while the TLS engine still held plaintext, so a response record
    larger than its 8 KiB read buffer waited until `timeout_ms` and failed. A connection it took from
    the pool came back non-blocking, so a split record read as end of stream, failing or truncating
    the response.
  - On TLS, the WebSocket and HTTP/2 clients also checked a -1 against a stale `errno`, so a peer's
    clean close could look like would-block and leave them waiting.

  A 0 on TLS now means wait or poll again in every client, a TLS -1 is a close or an error (with
  `at_eof` telling which), the WebSocket and HTTP/2 clients drain plaintext the engine holds, and the
  sync client reads buffered plaintext before polling and returns a pooled connection to blocking
  mode.
- **An abortive stream close could keep sending.** `kl_stream_cancel` drops the write queue and
  cancels the in-flight send, but that send can still complete successfully, the cancel having lost
  the race. On a completion loop the completion then submitted the next queued batch, a new send with
  no cancel requested. With a peer that had stopped reading, that send never completed and the stream
  never reached `on_close`; for a pipe stream the handle and memory stayed pinned. An abortive close
  now submits nothing more.
- **Every miniz-compressed response was freed with the wrong size.** The miniz backend's single-shot
  `compress` allocated room for the worst case but reported the actual compressed length. The server
  frees the buffer, and records it as `body_owned_size`, with that length, so a sized or tracking
  allocator was given a size that did not match the allocation. The output is now trimmed to exactly
  its reported length, as the decompressor's already was.
- **The WebSocket server did not validate close frames, and a route without `on_message` failed its
  second message.** The server echoed whatever close it received. An empty close was answered with
  status 1005 on the wire, a code RFC 6455 forbids sending. A 1-byte payload, a code outside the
  allowed ranges, or a reason that is not UTF-8 was accepted and passed to `on_close` instead of failing
  the connection with 1002 or 1007, as the client already did. Separately, a route with no
  `on_message` (send-only) never finished a received message, so the next one failed the connection
  with 1002. The server now applies the client's close validation (one shared check), echoes an empty
  close empty, and finishes every message, validating text as UTF-8, whether or not `on_message` is
  set.
- **An HTTP/1.1 `Upgrade: h2c` request was never answered.** The server sent 101 Switching Protocols
  and started a fresh HTTP/2 session, but the request that asked for the upgrade, which RFC 7540
  answers on stream 1, was dropped, so `curl --http2` and other upgrading clients waited forever.
  The upgrade is now done properly. `KlHttp2ServerSession` gains an optional `upgrade` op, appended
  per the compatibility contract, and the nghttp2 adapter implements it with
  `nghttp2_session_upgrade2`. The op applies the client's `HTTP2-Settings` and opens stream 1, and
  the request is then answered on stream 1 through the same route, middleware and handler path as any
  HTTP/2 request. When an upgrade cannot be done properly, the request is answered over HTTP/1.1,
  which RFC 9113 allows: a request with a body, a missing or repeated `HTTP2-Settings`, or a session
  without `upgrade`. Prior-knowledge h2c and ALPN `h2` are unchanged.
- **One stream's problem could take down an HTTP/2 server connection.**
  - DATA or END_STREAM for a stream the server had already answered was treated as an error, and
    the nghttp2 adapter makes a callback error fatal to the session. A client still sending a body
    the server had rejected early (for example a pre-body middleware's 401) aborted every other
    stream on the connection. Such frames are now ignored.
  - A stream over the configured `max_concurrent_streams` failed the same way. It is now refused on
    its own with a 503, and the connection carries on.
  - A client that reset a stream with NO_ERROR left its slot occupied, because the adapter reported
    only resets with an error code. Enough of them stopped the connection serving requests. Every
    close is now reported.
  - A request body sent without `content-length`, which HTTP/2 permits, never reached the route's
    body reader.
  - An HTTP/2 config with no session factory is now rejected by `kl_http_server_init`, rather than
    crashing on the first HTTP/2 connection.
- **HTTP/2 client fixes.**
  - After a connect that completed at once (a local AF_UNIX socket), the client kept write interest
    on an idle connection, which is always writable, so it spun the event loop at full CPU.
  - A request refused by the client's own stream limit returned -1 but had already been submitted, so
    the server ran a request the caller was told had failed.
  - A second response HEADERS on a stream (an interim 1xx, then the final response) leaked the first
    set of header strings.
  - A header copy that could not be allocated produced a response delivered as complete but missing
    headers. It now fails the stream with `KL_ERR_ALLOC`.
- **HTTP server fixes.**
  - SSE: `kl_http_sse_event` split data only on LF, but an SSE parser also ends a line at a bare CR,
    so `data` containing one could inject `event:` or `id:` fields. Data is now split on CR, LF and
    CRLF. An event name or id containing a line break is refused, and a multi-line comment stays a
    comment.
  - HTTP/2 prior knowledge on a completion loop (io_uring, IOCP, pollcomp) handed the session the
    connection preface with its magic stripped, so nghttp2 rejected the connection. It now receives
    the whole preface, as on readiness loops. Also, when HTTP/2 was configured but its server hooks
    were absent, the preface check compared past the end of its 25-byte constant.
  - Body bytes that arrived in the same read as the headers were not counted. A request whose whole
    body came with its headers looked unfinished when the connection was about to close, and the
    server held the connection until the drain deadline (500 ms by default) instead of closing.
  - The CORS middleware now sends `Vary: Origin` when it echoes a specific origin, so a shared cache
    cannot serve one origin's `Access-Control-Allow-Origin` to another.
- **HTTP client response-handling fixes.**
  - `max_response_size` now bounds the decompressed body, buffered or streamed, not only the bytes on
    the wire. A small compressed response could inflate to the decompressor's own cap.
  - The async client no longer ignores a failed decompression. It used to deliver the still-encoded
    body as a success; the sync client already failed.
  - A streaming response whose decompression fails at the final flush (a truncated stream) now fails
    the request.
  - `Connection` is now read as a token list, so `Connection: keep-alive, close` is not pooled.
  - A 101 Switching Protocols response is not pooled either.
  - After an interim 1xx with headers, a final response without any no longer frees the header
    array with size 0.
  - A failure to copy the response headers no longer leaks the body.
  - A streaming response parser reused after `reset` no longer counts the previous response against
    its size limit.
- **WebSocket fixes.**
  - Fragmented messages (RFC 6455 5.4), server and client:
    - An empty first fragment was not recorded as the start of a message, so its continuation was
      refused.
    - A final continuation with no message open was delivered as a message.
    - A new data frame inside a fragmented message silently replaced it.

    The first now starts the message and the other two fail the connection with 1002.
  - The client accepted any 101 with a correct accept value. It now also requires an upgrade to
    `websocket` over a `Connection` that carries `upgrade`, refuses an extension it never offered, and
    refuses a subprotocol other than one it requested.
  - When the client could not allocate room for frames that arrived with the 101, it dropped them
    and parsed the stream from mid-frame. It now fails the connection.
  - A second `kl_ws_server_enable_drain` no longer drops and leaks queued frames; it only updates the
    cap.
  - The client's upgrade buffer is freed at the size it was allocated with, even when an earlier
    shrink did not happen.
- **`kl_free(NULL, ...)` no longer reaches the allocator.** The contract never asked an allocator's
  `free` hook to accept NULL, yet several unwind paths passed it NULL with a nonzero size; the
  POSIX datagram batch teardown after a partial allocation failure is one. A tracking or arena
  allocator then miscounted. `kl_free` now skips NULL.
- **A closed datagram no longer acts on its old descriptor.** After close, `kl_datagram_fd`
  returned the closed descriptor's number, and `kl_datagram_set_tos`, the multicast calls and
  `kl_datagram_local_port` used it, though by then it could belong to another socket. They now report
  a closed datagram.
- **Event engine edge cases.**
  - IOCP: a watcher whose probe could not be re-posted (typically after the peer reset the
    connection) was retired silently, so its owner was never called again. It is now reported ready
    on every tick until it is removed, as a readiness backend reports a dead socket.
  - io_uring: `kl_event_del` on a watcher, with no submission entry free, dropped the poll removal.
    The poll stayed in the kernel holding the socket's file, so closing the descriptor did not close
    the connection. The removal is now retried at the next tick.
  - IOCP: a zero-length file response (`kl_http_response_file` with size 0) sent the whole file
    after its `Content-Length: 0` head, because TransmitFile reads a count of 0 as "the whole file".
    Only the head is sent now.
  - IOCP: a file that ended before its declared length (it shrank after the response was sized)
    was reported as fully sent. The write now fails and the connection closes.
  - pollcomp: accepted sockets were made blocking, so one large buffered body to a client that
    stopped reading blocked the whole loop. They are non-blocking now.
- **Keel's file reads are close-on-exec.** The resolver's reads of the hosts file and
  `resolv.conf`, and the POSIX `/dev/urandom` read, used descriptors a child process inherited. They
  now open with `O_CLOEXEC` (`_O_NOINHERIT` on Windows), and `check-cloexec` covers file opens.
- **Windows: the default hosts path is built per resolver.** A function-local static, filled on
  first use, was a data race when resolvers were created on two threads at once.

- **An interim 1xx response was reported as the response.** A server may send `100 Continue`,
  `102 Processing` or `103 Early Hints` before the final response (RFC 9110 15.2). The HTTP/1.1
  response parser stopped at that interim message, so the sync and async clients returned `103`
  (or `100`) with no body and never read the real response. A streaming caller's `on_headers` fired
  for the interim message too.
  - The llhttp response parser now discards an interim response (status, headers and all) and keeps
    parsing to the final one, in the same read or a later one. `101 Switching Protocols` stays
    final.
  - An interim response followed by end of stream has no final response, so it is an error, not a
    success.
  - `test_http1_response_parser` adds six cases (100, 103 then 102 then 200 with no header leak,
    interim and final in separate reads, interim then EOF, 101 still final, streaming `on_headers`
    once). `test_http_client_eof` adds three client cases over a real peer. Without the fix five
    parser cases and all three client cases fail.

- **The HTTP/2 client reported a reset stream as a complete response, and its bodies were
  unbounded.**
  - A stream the peer reset (`RST_STREAM`) was handed to `on_resp` like a finished one.
  - Body data grew without limit, and an allocation failure silently dropped data.
  - `KlHttp2ClientResponse` gains a trailing `error` field: `KL_ERR_IO` (reset), `KL_ERR_TOO_LARGE`
    or `KL_ERR_ALLOC`, with 0 meaning the stream completed.
  - `KlHttp2ClientConfig` gains a trailing `max_response_size` (0 = 16 MiB,
    `KL_HTTP2_CLIENT_DEFAULT_MAX_RESPONSE`). Both are additive, zero-default trailing fields.
  - `test_http2_client` adds three cases on a live connection driven through the mock session.
    Without the fix, the reset and over-cap cases fail.

- **HTTP server and client hygiene** (fifteenth-audit Lows L2, L8, L9, L10).
  - **Stale body accounting skipped the rejection drain (L2).** `request_body_received`,
    `request_body_complete` and `drain_framing_usable` were never reset between requests. After a
    request whose body was read in full on a kept-alive connection, an early rejection of the next
    request found the stale "complete" flag, skipped the drain, and closed with the body unread: the
    abortive close #278 exists to prevent. They are now reset on acquire and on keep-alive. The new
    `test_reject_drain` case passes before the fix on every platform tried (loopback delivers the 413
    first); the recorded trace shows the skipped drain.
  - **A connection with bytes after the response was pooled (L8).** The sync and async pooled
    clients dropped bytes that followed a complete response and returned the connection to the
    pool, so the next request read them as the start of its own response. Such a connection, and
    one whose response ended at end of stream, is now discarded instead.
  - **Pooled requests bypassed the proxy (L9).** `kl_http_client_request_pooled` and
    `kl_http_client_start_pooled` keyed and connected by the target and ignored `cfg->proxy`, so
    traffic meant for the proxy went direct. A proxied request now takes the non-pooled path, which
    honours the proxy.
  - **Wrong-size frees (L10).** `kl_http_client_remove_header` shrank the header count without
    resizing the array, which was later freed at the smaller size. A decompressed body was adopted
    as is: allocated at `out_len`, freed at `body_len + 1`, and not NUL-terminated. The miniz
    decompressor returned an untrimmed buffer when its trim allocation failed. All three now free
    each block with the size it was allocated with.
  - New tests: four pooled cases and a proxy case in `test_http_client_eof`, and a new
    `test_http_client_alloc_sizes` suite using a size-checking allocator. Without the fixes, both
    bytes-after-response cases, the proxy case and both size cases fail.

- **A duplicated truncated DNS reply dropped the TCP answer** (fifteenth-audit Low L1). Once a leg
  started recovering over TCP (RFC 7766), a later UDP reply for it (a duplicated or retransmitted
  truncated reply, or a spoof) fell through to the normal parse and settled the leg from the
  truncated reply, usually with no addresses. The real TCP answer was then dropped, and a later TCP
  failure could settle the same leg a second time.
  - A leg recovering over TCP now ignores UDP.
  - `dns_leg_settle` refuses an already-settled leg and clears the leg's TCP interest.
  - `test_dns_resolver` adds `duplicate_truncated_reply_does_not_preempt_the_tcp_answer`, with the
    UDP mock sending every reply twice.

- **Engine and platform hygiene** (fifteenth-audit Lows L3 to L7).
  - **IOCP: a zero-length datagram poisoned the send path (L3).** The send completion reported
    success as `bytes > 0`, so a successful empty datagram counted as failed and later sends were
    refused. Success is now the operation's own result. `test_datagram_socket` adds
    `zero_length_datagram_then_normal_send`, which fails on IOCP without the fix.
  - **io_uring: an op could be stranded when no SQE was free (L4).**
    - A continuation (a short send's tail, a splice step) that found no SQE marked the op aborted
      with nothing queued, so it never completed. It now fails the write at once.
    - A cancel that found no SQE was skipped. It is now recorded and re-posted at the next drain.
    - `test_iouring_sqe_fail` adds both cases, using the forced-SQE-failure seam.
  - **Thread pool: the wrong size was freed after a partial start (L5).** The thread array was
    freed at the started count rather than the allocated one; the capacity is now kept.
  - **Windows wakeup: the pair accepted whoever connected first (L6).** The loopback listener now
    checks that the accepted peer is its own client and drops anything else.
  - **Named pipes: a failed listen fired `on_close` (L7).** A `kl_pipe_listen` whose first arm
    failed called the owner's `on_close` for a listener it never handed out. That callback is now
    suppressed while the listen is starting. `test_pipe_stream` adds an allocation-failure sweep
    through `kl_pipe_listen`, which fails without the fix.

- **A WebSocket message larger than one read broke.** Both the server and the client decided
  "is this a new message?" from the frame's opcode alone, so the second chunk of the same TEXT or
  BINARY frame looked like the start of another message. The server closed the connection with 1002
  (protocol error), so any client message larger than one read, or one that straddled a TCP read,
  was rejected; the client kept only the tail of such a message. A control frame (ping, close) whose
  payload arrived over several reads was acted on from its last chunk only, so a pong or close
  reason carried a fragment. A new message now starts only at the first chunk of a non-continuation
  frame, and control payloads are gathered (up to their 125-byte limit) until the frame completes.
  The new `test_websocket_split_frames` round-trips a 60,000-byte message through a real server and
  client, and sends a raw frame in pieces plus a ping whose payload is split across writes; without
  the fix both cases fail.

- **Destroying the DNS resolver from `done()` during a TCP fallback was a use-after-free.** When
  the completing answer (or a connection failure) came through the TCP fallback, `done()` ran from
  the TCP watcher, outside any datagram frame; a `destroy()` there (which the resolver contract
  allows) freed the resolver while the TCP handler was still reading its state. The TCP handler now
  holds the resolver's deferred-destroy sentinel for the whole event and stops once a destroy was
  requested. New `test_dns_resolver` cases destroy from `done()` after a TCP answer and after a TCP
  drop.
- **Every resolver-cache hit leaked a request under `KlHttpClient`.** A synchronous completion (a
  hit, or an inner resolver that answered inline) returned a live handle that only `cancel()`
  freed, and the client drops the handle of a request that already completed. It now frees the
  handle and returns NULL; `resolver.h` states that a handle is dead once `done_fn` has run.
  **Behavior change:** `resolve()` on the cache returns NULL after a synchronous completion, and
  such a handle must not be cancelled (the built-in DNS resolver already behaved this way).

- **IOCP: a watcher interest change from a callback leaked an operation, and a failed re-arm could
  hang shutdown.** Between a dispatched watcher completion and its re-arm the probe owns no kernel
  I/O. An interest change in that window (the usual case: `kl_watcher_mod` from inside the watcher's
  own callback, which the async HTTP and WebSocket clients do on every request) allocated a new
  probe and "cancelled" the idle one, which could never complete, so one operation leaked per change
  until the loop closed. A re-arm that failed (for example on a socket closed from the callback) left
  its operation in a state neither `kl_watcher_del` nor the loop's close freed, and
  `kl_event_ctx_free` then waited forever. An idle probe is now retargeted in place, and a failed
  re-arm is retired so both existing paths free it. New `test_iocp_engine` cases count allocations
  across 200 callback-driven interest changes and free a loop after a failed re-arm under a watchdog.

- **Client sockets and completion-engine accepts leaked into child processes.** The sockets a
  client creates, and the connections the completion engines accept, were inherited by every child
  an embedder spawned.
  - Affected clients: sync HTTP, async HTTP (including each Happy Eyeballs attempt), WebSocket and
    HTTP/2, over both TCP and Unix sockets.
  - Affected accepts: io_uring (`io_uring_prep_accept` with no flags), pollcomp (a bare `accept`)
    and IOCP (`WSASocketW` with no `WSA_FLAG_NO_HANDLE_INHERIT`).
  - Each client now marks its socket through the provider seam (`kl_sock_set_cloexec`), so a custom
    provider's socket is covered too. Each engine accept is close-on-exec or non-inheritable at
    creation.
  - The built-in `socket` op now creates close-on-exec (`SOCK_CLOEXEC`) or non-inheritable
    (Winsock), which also closes the window where a concurrent `fork` could inherit a new socket.
  - `check-cloexec` now also flags `io_uring_prep_accept` / `WSASocketW` without the flag, a bare
    `socket` / `accept` outside the provider TUs, and a `kl_sock_socket` caller that never calls
    `kl_sock_set_cloexec`. `test_cloexec` adds a probe provider that hands out an inheritable socket
    and checks it has been marked by connect time.

- **`kl_wakeup_signal` could block its caller.** The wakeup channel's write end was blocking, so
  once enough signals went undrained to fill the pipe (POSIX) or loopback pair (Windows), the next
  signal blocked. That stalled a worker thread, or deadlocked the loop thread when it signalled its
  own channel.
  - The same channel carries `kl_http_server_stop`'s wakeup, which is documented as safe from a
    signal handler.
  - Both ends are now non-blocking. A full channel already holds a pending wakeup, so the dropped
    byte is not needed, as `wakeup.h` already promised ("a signal lost to a full channel").
  - `kl_wakeup_drain` now empties the channel (bounded at 4 MiB) instead of reading 64 bytes, so a
    burst of signals costs one wakeup instead of one per 64 bytes.
  - `test_wakeup` adds `signal_never_blocks_on_a_full_channel`: a thread floods 2^20 signals with
    nobody draining. Without the fix it blocked on the full channel.

- **A truncated HTTP/1.1 response was reported as success, and a close-delimited one lost its
  headers and body.** At end of stream the sync and async clients counted any response whose status
  line had arrived as complete. The parser hands headers and body over only at message-complete, so:
  - a close-delimited response (no `Content-Length`, not chunked) came back with no headers and no
    body;
  - a response cut off inside its headers or its `Content-Length` / chunked body succeeded as a
    partial response.

  `KlHttp1ResponseParser` gains an optional `finish` op, appended after `destroy` per the
  append-only vtable contract. The llhttp parser implements it with `llhttp_finish`. At EOF the
  clients now succeed only when `finish` reports a complete message; a truncated response fails with
  `KL_ERR_PARSE`. A third-party parser without `finish` keeps the old rule.
  - **HEAD.** A second optional op, `expect_no_body`, tells the parser that the request was `HEAD`,
    so the response has no body whatever its `Content-Length` says (RFC 9110 9.3.2).
    - The clients call it for every HEAD request, and the response is complete at the end of its
      headers.
    - Previously a HEAD response carrying `Content-Length` waited for a body that never came: on a
      kept-alive connection until the request timed out, and on a closed one it "succeeded" with no
      headers.
    - With a parser that has `finish` but not `expect_no_body`, the clients use the old status rule
      at end of stream for HEAD, so a HEAD response is never mistaken for a truncation.
  - New suite `test_http_client_eof` covers close-delimited, truncated-body and truncated-header
    responses over the sync and async clients.
  - `test_http1_response_parser` adds five `finish` cases, and `fuzz_response_parser` now also
    drives `finish`.
- **Pooled async requests cancelled someone else's timer and never timed out.**
  `kl_http_client_start_pooled` left the timer ids at 0 and never copied `timeout_ms`, so:
  - every completion called `kl_timer_cancel(ev, 0)`, cancelling whichever timer held id 0;
  - a pooled request to a silent server waited forever.

  The pooled path now initializes the timers and timeout as `kl_http_client_start_s` does, and arms
  the request deadline on a pool hit and on the direct-connect path.
  - `test_http_client_eof` adds `pooled.completion_leaves_unrelated_timer_alone` and
    `pooled.silent_server_times_out`.

- **A completion-mode `KlStream` stalled if its submit hook completed inline.** The stream contract
  (`docs/contracts/stream.md`) lets a submit hook complete synchronously, and the read side honours
  that. The write pump, however, marked the send as in flight only after the hook returned, so an
  inline `kl_stream_on_write_complete` was dropped as spurious. The send never retired, and the
  queue stalled with no error.
  - The pump now records the send before calling the hook. It notes an inline completion and
    retires it after the hook returns, then loops to submit the next batch, so a backend that
    always completes inline drains the queue without recursion.
  - An inline delivery failure is the same sticky error as an asynchronous one. `KlStreamSubmitFn`
    now documents inline delivery.
  - No shipped backend completes inline (the IOCP pipe path self-queues even synchronous results),
    so this matters to custom providers and wrappers.
  - `test_stream` adds five cases: referencing and copying backends, a backend that writes more from
    inside the hook, an asynchronous completion whose re-submit completes inline, and an inline
    failure. All five fail without the fix.

- **`kl_anon_pipe_create` accepted a runtime loop that cannot watch host pipes.** On POSIX the
  readiness path took `KL_EVENT_CAP_NATIVE_FD` to mean "watches OS descriptors". The lwIP BSD loop,
  installed at run time, advertises that bit (the socket-provider pairing needs it) but polls lwIP
  socket numbers, so create returned `KL_PIPE_OK` for a pipe whose readiness would never be reported.
  - Every runtime-installed loop is now refused with `KL_PIPE_UNSUPPORTED`, as the IOCP completion
    path already refuses one.
  - `docs/architecture/process_pipe_streams.md` lists the lwIP BSD loop among the refused engines.
  - `test_anon_pipe` adds `runtime_loop_is_refused_even_with_native_fd`, with a stub provider that
    advertises `READINESS | NATIVE_FD`.

- **Keel's own descriptors leaked into an embedder's child processes.** Four descriptors were
  inherited by every child an embedder spawned:
  - the epoll instance (`epoll_create1(0)`);
  - the run-loop / thread-pool wakeup pipe (`pipe()`), both ends;
  - io_uring's splice pipe (`pipe2(O_NONBLOCK)`);
  - on Windows, the wakeup loopback socket pair (Winsock sockets are inheritable by default).

  A leaked pipe end is more than clutter: it can keep a pipe open in a child that should see EOF.
  All four are now close-on-exec (`EPOLL_CLOEXEC`, `FD_CLOEXEC`, `O_CLOEXEC`), or non-inheritable on
  Windows. The kqueue descriptor, which `fork` never inherits, is marked too, so the rule has no
  exceptions: every descriptor or handle Keel creates is close-on-exec. The new `test_cloexec` checks
  it generically: every descriptor that appears across creating a loop, a wakeup, a thread pool and
  a datagram socket must be close-on-exec, and on POSIX an exec'd shell must be unable to use any of
  them. It runs on every engine set.

- **A graceful close could hang forever after a write failure.** `kl_stream_close_begin` waited for
  the write queue to drain, but after a terminal write failure queued bytes can never go out. Two
  cases:
  - readiness: the writer returned -1 and the queue kept its bytes;
  - completion: a send failed with more bytes queued behind it.

  In either case `on_close` never fired. This affected any `KlStream` (socket or named pipe), and a
  peer that disconnects while output is still queued makes it the common case. Now a terminal write
  failure makes the queue undeliverable, so it stops holding the close, which progresses exactly
  once, and still only after any outstanding operation has physically retired. A readiness writer
  failure is now sticky like a completion failure: later `kl_stream_write` calls report
  `KL_STREAM_ERROR`. Regressions cover the mock state machine in both modes, a real loopback socket
  whose peer resets, and a named pipe whose server disconnects under a pending write.

### Documentation

- **The event-loop teardown rule is now stated** on `kl_event_ctx_free` (`<keel/event_ctx.h>`). Freeing
  a loop delivers no further callback to anything still attached:
  - pending timers are discarded, even one already due;
  - watchers are removed, even one whose handle is already ready;
  - on a completion engine, posted operations are cancelled and reclaimed without a terminal event.

  Callers release every attached object first, and drive confirmed-close objects (`KlDatagram`,
  `KlStream`-based transports, listeners) to their close callback, because detachment needs the loop.
  After the free, only `kl_timer_cancel` (-1) and `kl_watcher_del` / `kl_watcher_mod` are defined.
  On completion engines, several objects reach the released loop if the order is broken, and the new
  documentation names them. Behaviour is unchanged. What changed is that the rule is written down,
  based on a per-object audit. The stream, datagram and async-lifecycle contracts cross-reference it.
  The last of these now says its "no silent loss" guarantee holds for `KlAsyncOp` only because
  `kl_http_server_free` cancels those operations before freeing its loop.

### Testing

- `test_loop_teardown` pins the rule on every engine:
  - pending and due timers are discarded;
  - a ready watcher gets no callback;
  - the context can be re-initialised;
  - `kl_timer_cancel` and the watcher calls are inert after the free;
  - a `KlDatagram` with a receive posted, closed to `on_close` before the loop is freed, leaves the
    allocator balanced.

## [3.2.0]

Minor release. No release date here (the tag and publish are a separately authorized step).

Headline: **Windows Named Pipes, client and listener, as first-class `KlStream` / `KlListener`
transports over IOCP.** This is the first stream transport that is not a socket. A consumer connects
or listens with a pipe-specific call, and from then on drives an ordinary `KlStream`: nothing above
the stream is pipe-specific. The pipe `HANDLE` never becomes a `KlSocketHandle` or reaches
`KlSocketProvider`. That makes Windows local IPC the counterpart of `AF_UNIX` on POSIX, and it
tested the Tier-1 transport abstraction against something that is not a network socket. `KlStream`
held unchanged. `KlListener` needed one append-only extension, because its accept handoff was typed
as a socket.

New public functions and an appended `KlListenerHooks` field pair are why this is a minor release.
Everything is source-compatible. Code that fills `KlListenerHooks` with designated initializers needs
no edit, only the recompile every 3.x update already requires (`docs/contracts/compatibility.md`).

### Added

- **Windows Named Pipe client**: `kl_pipe_connect(ctx, "\\\\.\\pipe\\name", &cfg, &p)` in the new public header
  `<keel/pipe.h>` returns a `KlPipeStream`; `kl_pipe_stream(p)` is its `KlStream`.
  - **Connect never waits.** An absent server (`KL_PIPE_ABSENT`) and every instance busy
    (`KL_PIPE_BUSY`) are reported at once, and retry policy is the caller's.
  - **Local names only.** A remote `\\host\pipe\` name is refused (`KL_PIPE_INVALID`).
  - **Identification only.** The client connects at identification level, so a server can learn who
    connected but cannot impersonate them.
  - **Free is legal at any time**, including from inside a callback. Memory and the handle are
    reclaimed only after the last overlapped operation has physically retired. (#336)
- **Windows Named Pipe listener**: `kl_pipe_listen(ctx, path, &cfg, &pl)`. Each connecting client
  arrives as a `KlPipeStream`, the same object a client gets. The owner attaches its callbacks with
  `kl_pipe_bind` and drives the `KlStream`. `kl_pipe_listener_close` / `kl_pipe_listener_free` follow
  `KlListener`'s confirmed-detachment rules.
  - **Security is fixed, not configurable.** Server instances carry a DACL for the current user and
    LocalSystem only (the Windows default also grants Everyone read). Remote clients are rejected.
  - **The name is claimed at listen time.** The first instance claims it, so a name another process
    already created, whether a second server or a squatter, fails with the new `KL_PIPE_IN_USE`
    instead of being shared.
  - **Instances are never recycled.** Each one is closed only after its operation has retired. (#338)
- **`KlListener` object handoff family**, for a transport whose accepted connection is not a socket.
  - **API.** `KlListenerHooks` gains two trailing fields, `on_accept_obj` / `dispose_obj`, and the new
    entry point is `kl_listener_on_accepted_obj(l, conn)`, where `conn` is the adapter's own
    connection object, passed through untouched.
  - **Exactly one family per listener,** checked at init. The listener state machine (window, credit,
    cancel-once, detachment) is shared by both families.
  - **Ownership is stated in `listener.h`.** Once an object is accepted, the listener releases it
    exactly once, to the owner or to `dispose_obj`.
  - **Unchanged:** the socket (fd) family and every existing listener user, including the HTTP server.
    (#338)
- **`KEEL_VENDOR_OPT`: an optimization level for the vendored TUs, separate from Keel's own.** The: an optimization level for the vendored TUs, separate from Keel's own.** The
  embedder hooks added in 3.1.0 made `KEEL_OPT` reach every TU at once, which is what an embedder
  wanting its own flags asked for. It had a consequence nobody asked for. The toolchain wedge that
  motivated the hook (cosmocc's GCC hanging indefinitely at `-O2` on Windows) is in the **vendored**
  TUs, llhttp and the miniz adapter, not in Keel's own; but the only way to get a build at all was to
  lower the level everywhere, so Keel's entire library was then compiled unoptimized. At `-O0` Keel's
  `.text` is about a third larger (333 KB to 447 KB on MinGW-w64) and none of it is optimized, a cost
  paid on every call and, through reduced code density, on process start.

  `KEEL_VENDOR_OPT` defaults to `KEEL_OPT`, so every existing invocation is byte-for-byte unchanged:
  verified by diffing `make -Bn` output against the previous Makefile for the default build on all six
  platform/backend branches (POSIX, cosmocc, Windows MinGW, Windows IOCP, poll, pollcomp) and for
  `KEEL_OPT=-O0`, `KEEL_OPT=-O0 CC=cosmocc`, `KEEL_OPT=-O1` and `KEEL_EXTRA_CFLAGS=-flto`. A build that
  needs the split now spells it `make KEEL_OPT=-O2 KEEL_VENDOR_OPT=-O0`, which recovers 72% of the
  `-O0` code-size regression (447 KB to 365 KB) and leaves every Keel TU optimized. (#328)

### Engine support

- **Named pipes run on the IOCP engine only** (`BACKEND=iocp`, MinGW and MSVC). On WSAPoll, the POSIX
  engines, io_uring and pollcomp, and on any non-Windows platform, `kl_pipe_connect` and
  `kl_pipe_listen` return `KL_PIPE_UNSUPPORTED` before touching the OS. There is no readiness
  emulation: WSAPoll cannot watch a `HANDLE`, and nothing pretends it can.

### Changed

- **Internal: the completion-lifetime token is now `KlCompLife`** (`src/completion_life.{h,c}`, the
  functions are now `kl_comp_life_*`). It was named for datagrams, and pipe streams became its second
  owner under the same ownership rules. No behaviour changed, and nothing in `include/keel/` names it.
  It does affect out-of-tree code that includes the internal completion seam: the lwIP and UEFI
  integrations in this tree were updated. The new `make check-no-dgram-life` gate rejects the old
  names outside `docs/archive/`. (#337)

### Testing

- `test_pipe_stream` runs the client and listener on IOCP under MinGW and MSVC, and asserts the refusal
  on every other engine.
  - **Connect outcomes:** absent, busy, a DACL-denied pipe, non-local names, and
    identification-only impersonation.
  - **Data:** a 1 MiB fragmented transfer with bounded-queue backpressure, orderly and abortive
    disconnects, strict pause/resume, and a u32be-framed exchange written against `KlStream` only.
  - **Close and free:** cancel with a read or a write pending, free with operations outstanding or
    from inside a callback, and loop teardown.
  - **Listener:** echo with Keel on both ends, sequential clients, eight concurrent clients through a
    window of two, streams outliving their listener, `KL_PIPE_IN_USE` for a squatter, the exact
    instance DACL, a deterministic `ERROR_PIPE_CONNECTED` race, and 200 client plus 100 listener
    close races.

  Every lifetime case checks allocator balance.
- `test_listener` adds ten object-family cases: handoff, teardown disposal, cross-family refusal, a
  completion window, and the ownership edges (synchronous inline accepts, an owner closing inside the
  accept callback, a refill failure after a handoff, and a close inside dispose).
- The new `make check-pipe-seam` gate keeps Win32 pipe I/O inside its two mechanics TUs and overlapped
  only, keeps pipe symbols off the socket axis and out of protocol TUs, and keeps consumer-protocol
  names out of the transport. The Tier-1 boundary gate now also covers the pipe seam headers.

## [3.1.1]

Patch release. No release date here (the tag and publish are a separately authorized step).

One packaging fix. No file under `src/` or `include/` changed since 3.1.0, so the compiled library
behaves exactly as it did; what is fixed is the `keel.pc` that an installed Keel hands to
`pkg-config`. An embedder who builds Keel in-tree, or who passes link flags by hand, is unaffected.

### Fixed

- **A consumer could not statically link an installed Keel on Windows.** `keel.pc` hardcoded
  `Libs.private: -lpthread`, which is wrong twice over there: the PAL uses Win32 threads, so pthreads
  is not what Keel links, and the libraries it does need (`ws2_32`, `mswsock`, `bcrypt`, `iphlpapi`,
  `advapi32`, `shell32`) were absent. The link failed as `ld returned 5` with no diagnostic, which is
  why it read as a toolchain quirk rather than a packaging bug. `Libs.private` is now derived from
  what the Makefile actually links, per platform and per toolchain: `LD_THREAD` off Windows,
  `LD_WIN_PLATFORM` on it, and `-luring` under `BACKEND=iouring`. A `.pc` generated from an MSVC
  build names `.lib` files; one from MinGW names `-l` flags. Linux consumers building the io_uring
  backend were affected by the missing `-luring` for the same reason. (#314)

### Testing

- The installed-artifact gates (`check-install`, `check-installed-consumer`, `check-public-headers`)
  now run on Windows, which is where the packaging bug above lived. Five of them had invoked a literal
  `make` and exited 127 before their first assertion, so they had never once executed on the platform
  whose packaging they describe. `tools/msys_native_paths.sh` gives them the two path treatments MSYS
  argument conversion requires: a real staging root has to reach a native tool in a spelling it
  understands, while a logical prefix recorded verbatim in `keel.pc` must not be rewritten.
- Three suites died under MSVC with exit `0xC0000409` and no output: the UCRT terminates the process
  on a CRT call against an invalid file descriptor, where glibc and MinGW return `-1`/`EBADF`. The
  Windows test prelude now installs an invalid-parameter handler so those deliberately fabricated
  descriptors reach the error paths the tests were written against. Keel's own behaviour is untouched;
  the library never installs a handler. (#316)
- The MSVC suite set is derived from the Windows set minus a documented exclusion list, and
  `check-msvc-parity` gates it so it cannot drift into a hand-curated list of whatever passes.
- `dgram_batch.recv_pause_resume_held_cursor` no longer depends on how many of a three-datagram
  loopback blast the kernel has queued when readiness first fires. It asserted that one `recv_batch`
  refill carries all three; on Darwin, loopback input is deferred, so a loaded runner could present
  two and then one, and the test failed on macOS CI while the machine under test was behaving
  correctly.

## [3.1.0]

Minor release. No release date here (the tag and publish are a separately authorized step).

Headline: **Keel builds natively with MSVC.** `cl.exe` and `lib.exe` produce a native static library
from the same source tree, the same Make graph and the same PAL implementations as every other
toolchain, so the compiler choice does not change runtime architecture. Supporting that took a
constructor-free Winsock lifecycle in the PAL, and the work surfaced several 64-bit portability
defects that MinGW had never diagnosed.

One source-compatible signature change (`kl_http_response_file`) is why this is a minor release
rather than a patch.

### Added

- **Native MSVC build path.** The supported workflow is two commands:

  ```sh
  source scripts/msvc-env.sh
  make CC=cl
  ```

  `scripts/msvc-env.sh` locates the Build Tools through `vswhere` and handles the two MSYS2
  behaviours that otherwise break a native build: its own `link.exe` shadowing the real one, and
  argument/environment path conversion mangling `/Fo`, `/std:c11` and `INCLUDE`/`LIB`. The resulting
  binaries import only `WS2_32.dll` and `KERNEL32.dll`, with no MinGW runtime dependency. Both Windows
  event backends build (`BACKEND=iocp` as well as the WSAPoll default). See
  [docs/msvc_build.md](docs/msvc_build.md).

- **`mk/toolchain.mk`**, a toolchain mapping layer. Build intent (`CC_STD`, `CC_WARN`, `OBJ_OUT`,
  `AR_CMD`, ...) is expressed once and mapped to GCC/Clang or MSVC; nothing else in the build tests
  which compiler is in use. Not a second build system: there is no Visual Studio project and no CMake.

- **PAL socket-runtime seam** (`src/platform_socket.h` + per-OS TUs). Winsock is started through
  `InitOnceExecuteOnce`, once per process, with no `WSACleanup`, replacing the GCC/Clang load-time
  constructor that MSVC has no equivalent of. One mechanism for every Windows toolchain.

- **Sanitized completion coverage for the unit suites.** Jobs `Sanitized completion (pollcomp)` and
  `Sanitized completion (io_uring)` run the completion-axis unit suites under ASan+UBSan on every PR.
  Sanitized completion *smoke* roundtrips already existed; the unit suites, which abort, cancel, reset
  and tear down mid-flight, did not.

- **Completion suite lists are derived, not curated.** The portable double now runs the unit suites it
  is eligible for, derived from the suites already passing on both existing completion backends, with
  `check-pollcomp-suite-list` and `check-completion-lane-parity` keeping the derivation and the
  sanitized/unsanitized set relationship machine-checked. There are currently **no exclusions**.

- **`ISOLATED_SUITES`**: suites that need a fresh process per test, via `tools/run_suites.sh`.

### Changed


- `kl_http_response_file()` takes `int fd` again, instead of `KlSocketHandle fd`. The parameter is a
  FILE descriptor, and every other file descriptor on the public surface is already an `int`:
  `KlSocketOps.sendfile` takes `int in_fd` beside `KlSocketHandle out_fd`, `KlFileIO.submit` takes
  `int file_fd` beside `KlSocketHandle sock_fd`, and `KlHttpResponse.file_fd` stores it as an `int`.
  It reaches `sendfile(2)` on POSIX and `_get_osfhandle()` on Windows, both of which take an `int`.

  This restores the original declaration. It was `int fd` from the first commit until the 3.0 work
  retyped the socket-handle surface to `KlSocketHandle` (ledger item BLK-4), which swept up this one
  file descriptor by mistake and left it the single place on the API where a file descriptor was typed
  as a socket handle. On 64-bit Windows that is a real narrowing, which native MSVC diagnoses.

  SOURCE-COMPATIBLE, which is what 3.x promises: C applies the integer conversion implicitly, so every
  call that compiled before still compiles, and the value has always been a small `int`. It is not
  binary-compatible on 64-bit Windows, where the parameter changes width; 3.x does not promise a
  cross-version ABI (see `docs/contracts/compatibility.md`). Code that takes the ADDRESS of this
  function and stores it in a `KlSocketHandle`-typed function pointer must update that pointer type.

  Because it changes a public declaration, the next release carrying it is a MINOR bump, not a patch.

### Fixed

- **`strtok_r` was an implicit declaration under MSVC**, which truncates the returned pointer on a
  64-bit build: a crash, not a warning. Mapped to the CRT `strtok_s` in the build layer so the sources
  stay POSIX-spelled (`dns_resolver.c`, `proxy_protocol.c`).

- **Pointer-width socket handles narrowed into `int`** in three static helpers in `http_response.c`
  (`seam_writev`, `try_writev`, `stream_writev_all`), and the keep-alive reinit path round-tripped the
  connection handle through an `int` local. Harmless where a socket handle is 32-bit; a real
  truncation on 64-bit Windows.

- **The Makefile's `LIB` variable collided with MSVC's linker search path.** `LIB` is an environment
  variable the toolchain reads, and make re-exports a variable it reassigns that also came from the
  environment, so `LIB = libkeel.a` silently replaced the linker's search path in every recipe.
  Renamed to `KEEL_LIB`.

- **The C11 atomics lock-free contract** asserted `ATOMIC_INT_LOCK_FREE == 2`, which reads as "require
  lock-free atomics" but means "refuse to build wherever the implementation declines to promise it in
  a macro". MSVC reports 1 while `atomic_is_lock_free()` returns true for the object Keel uses. The
  policy now tests the requirement: 0 fails the build, 2 is a compile-time guarantee, and 1 triggers a
  runtime query on the actual flag during `kl_http_server_init`, before any signal handler can be
  installed, refusing the platform with `KL_ERR_UNSUPPORTED` if it fails.

- **Object trees are per-toolchain.** An MSVC object and a MinGW object of the same TU are not
  interchangeable and the archive path is shared, so a `make CC=cl` after a `make` could mix them.

- **`test_reject_drain` intermittently crashed the whole suite** on the completion backends. Two
  defects: utest `ASSERT_*` returns from the test body, so a failing assertion skipped the fixture
  teardown and left a server thread running against a static that the next test then re-initialised;
  and the residual flakiness was accumulated process state, not a drain defect. Measured: the failure
  rate rose with the number of server lifecycles already run in the same process (0/100 alone, 23/100
  after eleven) and fell to 0/100 in a fresh process after identical work. Fixed by running that suite
  one process per test. No assertion was weakened.

### Upgrade notes

- `kl_http_response_file()` now takes `int fd`. **Source-compatible**: C applies the integer conversion
  implicitly, so every call that compiled before still compiles. **Not binary-compatible on 64-bit
  Windows**, where the parameter changes width; 3.x does not promise a cross-version ABI. The one
  construct needing an edit is taking the function's ADDRESS into a `KlSocketHandle`-typed function
  pointer.

- MSVC builds do **not** track header dependencies (MSVC has no usable `-MMD` equivalent), so run
  `make clean` after editing a header. CI always builds clean.

- No behaviour change for existing GCC/Clang/MinGW/Cosmopolitan builds: their compile and link command
  lines are byte-for-byte unchanged, verified by diffing `make -n` output.
## [3.0.1]

Patch release. No release date here (the tag and publish are a separately authorized step).

One public configuration-semantics fix, one contract clarification, and a large expansion of the
Windows test subsets. No behaviour change for an embedder who leaves the drain configuration alone.

### Added

- `KlHttpServerConfig.reject_drain_disable`. Setting it to 1 turns the post-rejection drain off; it is
  now the only way to do that, and it overrides both numeric fields.

### Changed

- `docs/contracts/early_rejection_drain.md` now states the one case the drain deliberately does not
  protect: if HTTP framing is complete and the transport reports nothing currently readable, Keel may
  close immediately, so a peer that resumes transmitting after that observation can still cause an
  abortive close. Keel does not delay every early rejection to guard against future protocol-invalid
  excess input. Accepted behaviour for 3.x, with the alternatives and their costs recorded.
### Fixed

- **The documented way to disable the post-rejection drain was the configuration that enabled it.**
  3.0.0's header and contract both said that zeroing `reject_drain_max_bytes` and
  `reject_drain_timeout_ms` disabled the drain. `kl_http_server_init` applied the defaults exactly when
  both were zero, so zeroing both ENABLED it at 64 KiB / 500 ms, while zeroing exactly one disabled it
  as an undocumented side effect. The two fields are now normalised independently, so a zero field
  selects that field's default as `KlHttpServerConfig` promises for every member, and disabling is
  explicit. Embedders who leave the fields alone, including anyone zero-initialising the struct, are
  unaffected: they got the defaults before and still do.

### Testing

- The Windows test subsets grew from 71 to 98 suites on the readiness axis (WSAPoll) and from 66 to 93
  on the completion axis (IOCP). Most of those suites already built and passed there and had simply
  never been listed; one needed a port off the POSIX-only `kl_socket_provider_posix()`. Enrolling them
  found two defects that were invisible while the suites were absent: a watcher probe re-armed before
  its completion had been dispatched, and a test fixture whose assertion failure became a SIGSEGV
  because utest's `ASSERT_*` returns before the teardown that joins a running server thread.
- `make release` now refuses to build an archive containing CRLF, and `.gitattributes` keeps the
  worktree LF on every platform, so the source archive is byte-reproducible regardless of who builds
  it. A Windows-built 3.0.0 archive differed from the canonical one by 174080 bytes.

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
