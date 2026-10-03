# Changelog

All notable, user-visible changes to Keel are recorded here. The format follows Keep a Changelog, and
Keel follows Semantic Versioning (the compatibility contract is in `docs/contracts/compatibility.md`).

## [Unreleased]

### Security

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
