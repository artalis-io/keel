# C Audit Report: KEEL

> **Historical, append-only evidence log: newest pass first.** Each pass records what was verified
> on its date; verify any specific claim against current code before acting on it. Current-state
> docs: [architecture.md](../../architecture/overview.md), [architecture_invariants.md](../../architecture/invariants.md).
> Index: [audits/README.md](README.md).

## Nineteenth pass: final comprehensive audit after the re-audit loop (2026-10-06)

**Revision:** `main` `03cef23` (after #459 to #462). Read-only.
**Scope:** the whole `src/` tree, `include/keel/`, every integration (TLS backends, nghttp2, miniz,
lwIP raw, UEFI) and the build, in five parallel reviews:
- the HTTP/1 server, readiness and completion;
- the HTTP/1 client stack;
- HTTP/2 and WebSocket, server and client, with the nghttp2 adapter;
- the event and completion axis and the substrate;
- DNS, PROXY, the integrations and the build.

The axis review ran alongside (seventeenth pass in `keel_axis_audit.md`); its High is listed here
too, as N2. Every High below was re-checked against the code by hand. Confidence: **C** means the
path was traced end to end; **P** means plausible and needing a test. Items recorded earlier as
deliberately unfixed (S5, S9, E7, W11, X8 and the others listed in the eighteenth pass) were not
re-reported.

**Fixes since the eighteenth pass:** the re-audit fixes #452 to #459 and #461/#462 hold where they
were traced (completion queue pinning, `send_max`, the stream backpressure gate, `comp_closing` at
release, the async cancel at release, the WebSocket close and auto-ping changes, the mbedTLS sticky
failure, the DNS hook, deferred client errors, atomic hook registries, the completion-table check).
One of them introduced a High: N2, the TLS flush added to `conn_write` in #457.

**Result:** no Critical. **8 High, 15 Medium**, and a set of Lows. The Highs fall in three groups:
- **The async op lifecycle** (N3, N4): two call patterns the API allows corrupt or leak a
  connection.
- **Error and backpressure gaps on the main backends** (N1, N2).
- **The lwIP raw and EFI completion providers** (N5 to N8), which the core suites reach only through
  doubles: their cancel, terminal and handle semantics differ from what the HTTP driver relies on.

### High

| # | Area | Location | Defect | Failure scenario | Smallest fix |
|---|---|---|---|---|---|
| N1 | Substrate (C) | `src/socket_dgram_win.c:152-186`, `src/event_iocp.c:1267-1276`, `src/datagram_recv.c:210-247` | Windows reports an earlier `sendto`'s ICMP port-unreachable as WSAECONNRESET on the next receive, even on an unconnected UDP socket. It is mapped to EIO, and `recv_fail` stops the receiver for good. Nothing sets `SIO_UDP_CONNRESET`. | One unreachable nameserver (127.0.0.1 with no local resolver) and the built-in DNS resolver never receives again on Windows. A spoofed datagram that makes a `KlDatagram` server reply to a closed port stops it too. | `WSAIoctl(SIO_UDP_CONNRESET/NETRESET, FALSE)` in the configure path, and treat WSAECONNRESET/WSAENETRESET on a datagram receive as skip-and-continue (covers adopted fds). |
| N2 | Completion (C) | `http_internal.h` `conn_write` (TLS branch), `completion_http_server.c` `comp_tlsq_absorb_ring` | Since #457 every TLS write is moved from the engine's bounded ring into the uncapped output queue at once, so `tls->write` never refuses. TLS WebSocket output has no admission bound: the plaintext WebSocket bound and the stream gate (`kl_http_comp_stream_tls_full`) do not cover it. | A wss client that stops reading while the server broadcasts: every send succeeds and memory grows without bound, on io_uring, IOCP, pollcomp, lwIP raw and EFI. | Check the WebSocket admission bound before `tls->write` (an exported helper beside `kl_http_comp_stream_tls_full`), so the frame goes to the drain or fails; TLS twins of the two WebSocket bound tests. |
| N3 | Server (C) | `async.c:62-157`, `http_connection.c:415-443` `conn_process` | A handler may suspend and then complete in the same call (suspend, the thread-pool submit fails, write a 503, `kl_async_complete`). `kl_async_complete` drives the connection, then `conn_process` sets SENDING again. | Readiness with `Connection: close`: the slot is released twice and the free list self-loops (two accepts share one connection). Keep-alive: a phantom second response. Completion: the response is posted twice. | An `in_handler` flag set around the handler; `kl_async_complete` inside it runs `on_resume` and leaves the driving to the dispatch in progress. |
| N4 | Server (C) | `async.c:159-168`, `http_server_core.c:670-671` | `kl_async_cancel`, documented for deadline-as-failure, only retires the op. The connection stays SUSPENDED, out of the loop, with nothing posted, and the sweep skips every SUSPENDED connection. | Each upstream timeout resolved with cancel leaks a slot and an fd; after `max_connections` of them the server stops accepting. | Release a SUSPENDED connection whose op was cancelled (from the sweep, or from `kl_async_cancel` with a guard for the call from release itself). |
| N5 | lwIP raw (C) | `integrations/platform/lwip/lwip_raw_glue.c:752-780` | `lwr_srv_sent` runs the send pump, which can `tcp_abort` the pcb, and still returns `ERR_OK`; lwIP's `tcp_input` then keeps using the freed pcb. | A file response whose file is truncated mid-transfer: the next ACK aborts inside the sent callback, and lwIP writes to the freed `MEMP_TCP_PCB`. | The pump reports the abort and the callback returns `ERR_ABRT`. |
| N6 | lwIP raw (C) | `lwip_raw_glue.c:447-451, 1393-1394, 1459-1462`, `event_lwip_raw.c:640-655` | One terminal completion per connection, while a connection can hold a send and a recv. The second op never completes, and the cancel cannot find the dead slot. | A peer reset during a TLS handshake (or on WebSocket/h2) with both ops posted: the connection and its glue slot are never released; repetition exhausts the pool. | Emit one failed completion per outstanding op; surface READ failure for an armed dead slot. |
| N7 | EFI (C) | `integrations/platform/uefi/event_efi.c:297-301, 619-621` | `el_cancel` frees posted server recv/send ops without a completion. The driver releases a connection only at its last op's completion. | Every idle keep-alive connection the sweep times out leaks its slot; about seven idle clients take the firmware server down (`KL_EFI_MAX_CONNS` 8). | Mark cancelled I/O ops and have `el_drain` emit a failed READ/WRITE for each. |
| N8 | lwIP raw (C) | `lwip_raw_glue.c:389-397, 1355-1360`, `event_lwip_raw.c:655` | The `tcp_pcb*` doubles as the socket handle, and lookup prefers a live slot, so a dead connection's close finds a new connection that reused the pcb address. | A connection is aborted; the next accept reuses its pcb from lwIP's LIFO pool; the old connection's close tears down the new one and disarms it. | A slot handle (index plus generation) as the server `KlSocketHandle`, as the EFI provider does. |

### Medium

| # | Area | Location | Defect and scenario | Fix |
|---|---|---|---|---|
| M1 | Substrate (C) | `socket_dgram_posix.c:~205`, `datagram.c` `dg_rdy_pull` | A connected POSIX UDP socket stops receiving for good after one ECONNREFUSED/EHOSTUNREACH (the peer restarted). | Treat ICMP-induced receive errors as consumed; same on the completion paths. |
| M2 | Substrate (C) | `completion_dispatch.c`, `datagram.c` `kl_datagram_init_ex` | Optional `KlCompletionOps` slots are called without a NULL check; a provider without the datagram ops crashes at the first receive arm. | Refuse `kl_datagram_init_ex` with `KL_ERR_UNSUPPORTED` when a datagram slot is NULL (or make the routers fail on NULL). |
| M3 | Substrate (C mechanism, P reach) | `timer.c:501-526` | A 0 ms timer re-added from a timer callback fires again in the same `kl_timer_fire`, without end; a retry loop through the deferred-error timers starves all I/O. | Fire only timers that existed at entry (an id watermark). |
| M4 | Substrate (C) | `datagram.c` GSO path, `datagram_send.c:349` | A GSO group the kernel rejects (EINVAL above 64 segments, EMSGSIZE above 64 KiB) is dropped instead of sent per segment. | Force the per-segment path past the kernel limits; treat EINVAL/EMSGSIZE as unsupported for that group. |
| M5 | io_uring (C mechanism, P reach) | `event_iouring.c:497-507` | `sqe->len` is 32-bit: a 4 GiB remainder becomes 0, and the copy-sendfile fallback re-posts it forever. | Cap a post at 0x7ffff000 and set `send_max` to it; a 0 result on a non-empty send fails. |
| M6 | io_uring (P) | `event_iouring.c:1064-1066` | -EBUSY from submit (CQ overflow backlog on 5.5 to 5.18) is fatal to the loop. | Handle -EBUSY/-EAGAIN like -ETIME. |
| M7 | Client (C) | `http_client_async.c:1354-1368, 1752-1766` | A refused async start leaves the caller's shared `KlEventCtx` switched to the incompatible socket provider. | Check compatibility on a local, write `ev_ctx->sockets` only after it passes. |
| M8 | WebSocket (C) | `http_server_ws.c:158-169`, `http_server.c:650-659` | Readiness: a frame sent from outside the connection's own event lands in the drain with no WRITE interest and stalls; with auto-ping on, a healthy receive-only client is closed (1001). | Arm WRITE when the drain goes from empty to pending (or re-arm from the sweep). |
| M9 | HTTP/2 client (P) | `http2_client.c:471-475, 518-521, 161-184` | `on_error` fires on a client the user freed or closed from `on_resp` during a flush. | `h2c_on_send` fails at once when closed; skip `h2c_error` when freeing or closed. |
| M10 | Server (C) | `async.c:147-152`, `http_connection.c:795-811` | A streaming-async handler that suspends and then awaits the body: readiness never re-arms READ (408), and suspending at dispatch drops body bytes read with the headers and leaves the chunked decoder and body deadline unset. | A READING_BODY arm in the readiness resume; initialise body state before the handler; keep the leftover. |
| M11 | Server (C) | `drain.c:82-87` | Readiness: once the drain holds bytes it only appends, and a suspended connection has no WRITE interest, so a stream written while suspended stops after one would-block. | Try one write of the backlog before appending. |
| M12 | Server (P) | `http1_chunked.c:74-80, 157-166` | The chunked decoder accepts bare LF and control bytes in extensions and trailers, without a length cap: a framing mismatch with a front-end (the chunk-extension smuggling class). | Reject bare LF and control bytes; cap extension and trailer length. |
| M13 | PROXY (C) | `proxy_protocol.c:226-239`, `http_server.c:68-69` | On a dual-stack listener IPv4 peers arrive as `::ffff:a.b.c.d`, and the CIDR match compares families strictly: an IPv4 trust list never matches (fails closed, every request 400). | Unmap `::ffff:0:0/96` before matching. |
| M14 | miniz (C) | `decompress_miniz.c:316-340` | Streaming gzip needs the optional header fields in the same feed as the first 10 bytes; a split there fails a valid response (FNAME is gzip's default). | A small header state machine across feeds. |
| M15 | DNS (P) | `dns_sys_win.c:58-64`, `dns_resolver.c:1623-1644` | Windows: an `fe80::` resolver listed first is used without its scope and locks the list to IPv6; DNS fails although IPv4 servers exist. | Skip link-local entries (or keep the scope) and prefer a usable family. |

### Low (grouped)

- **Substrate:** the thread pool's nested-tick early return skips the wakeup drain (a nested loop
  spins); pollcomp leaks abort events' refs on an allocation failure (`return count ? count : -1`);
  an io_uring splice stage returning 0 reports success with a short body; an io_uring watcher whose
  poll fails goes silent; 32-bit length casts on Windows `writev`/IOCP posts; inheritable handles in
  the window after `socket()` on Windows and `pipe()` on POSIX; a named-pipe listener re-arm after
  every instance closed uses `first = 0`; datagram minor bounds (multicast `iface_index` ignored
  off Linux, unguarded batch products, an unchecked GSO total); the timer heap's doubling is
  signed-overflow-prone before its check.
- **Client:** sync redirect consults the policy for a hop past the limit; size-limit failures are
  reported as `KL_ERR_PARSE`; chunked trailers are merged into the response headers (a `Location`
  trailer is followed); freeing or cancelling inside a streaming callback is unsafe and
  undocumented; caller headers duplicating `Host`/`Content-Length`/`Transfer-Encoding` are sent and
  names are not checked as tokens; the proxy port is not range-checked; a failed redirect hop start
  always reports `KL_ERR_IO`; a stale pooled connection is not retried for idempotent methods.
- **HTTP/2 and WebSocket:** readiness WebSocket and h2 reads treat would-block as close; the Close
  echo is lost when the drain is pending; h2 cleanup destroys body readers without `on_error`; an
  nghttp2 header copy failure drops the header silently and response headers past 64 are dropped;
  the WebSocket client reports a Close-less EOF as 1001 (RFC: 1006) and fails some protocol errors
  without a Close; would-block is read from `errno` instead of `kl_sock_io_status` in the h2 and
  WebSocket clients; the documented nghttp2 floor (1.57) predates the CONTINUATION fix (1.61).
- **Server:** a partial PROXY header from a trusted source busy-loops readiness until the timeout;
  `max_header_size` below 8192 is not enforced; Windows listeners use `SO_REUSEADDR` rather than
  `SO_EXCLUSIVEADDRUSE`; handler headers can duplicate the server's framing headers, and 204/304
  get a Content-Length and body; a readiness stream drained but not ended spins on WRITE; graceful
  stop waits out idle keep-alive connections; the readiness TLS rejection drain counts ciphertext.
- **DNS, TLS, build:** a truncated (`KL_DGRAM_TRUNCATED`) DNS answer is used instead of falling back
  to TCP; a retransmit that hits would-block replaces the leg id; a learned server cookie is never
  forgotten; vendored llhttp builds without the stack protector and FORTIFY; OpenSSL ALPN
  reconfiguration can read past its array on a failed update; mbedTLS maps an out-of-range
  `client_auth` to none and accepts mTLS with no CA, `reset_failed` never clears, `write(0)`
  returns -1; `PEM_read_bio_PrivateKey` can prompt on a TTY; `efi_sock_close` can block the loop
  for a graceful close.

### Verified clean

Request parsing and CL/TE (llhttp strict, both rejected together), header limits, response header
injection guards, router, CORS, body readers, the completion output queue, connection reuse and the
drain teardown; the client URL parser, redirect method and credential rules, response parser caps,
pool keying and reuse, proxy CONNECT, the #462 deadline path, Happy Eyeballs, decompression caps,
`resolver_cache.c`; WebSocket framing, masking, fragmentation, UTF-8, size caps, close and liveness;
the h2 stream table, upgrade and session-done close; the readiness backends, provider validation,
watcher dispatch, listener credit, `KlCompLife`, backend op lifetimes on IOCP, io_uring and
pollcomp, datagram queues and close; DNS parsing (pointers never followed), spoof checks, TCP
framing; PROXY v1/v2 bounds; TLS verification defaults and adapters' rings; miniz bomb caps;
production build flags.

No source was changed by this pass. The fix order suggested: N1 to N4 first (main backends and the
public async API), then N5 to N8 (integrations), then the Mediums.

## Final C-audit sweep and targeted fix (2026-10-06)

**Revision:** `7878ef7` (merged #461; source-identical to local `5919644`).
**Scope:** mechanical scans of core C sources and public headers; direct Clang analysis of
applicable hosted POSIX core TUs; manual tracing of HTTP parsers, body readers, response framing,
router/middleware, client connect/error callbacks, completion output, timers, thread pool and
transport retirement. This is not an exhaustive proof of every backend or integration path.
The user redirected discovery to implementation after the finding below; no additional broad
review was pursued.

| Finding | Severity | Location | Proof and resolution |
|---|---|---|---|
| Deferred client error fell back to inline completion when a full timer heap could not grow | High (memory safety; allocation-failure dependent) | `src/protocols/http/http_client_async.c`, `async_complete_error` | A caller freeing the client in its documented `on_done` callback left `co_terminal` reading the freed embedded connect op. A temporary public-API ASan reproducer reported heap-use-after-free in `co_request_cancels`. Fixed by retiring the mandatory request deadline before scheduling its replacement: the deferred callback reuses a heap slot without allocating. Failure to reserve the initial deadline rejects startup without callbacks. |

Four new regressions cover full-heap allocation failure and initial deadline allocation failure
for direct and pooled clients. The original reproducer is sanitizer-clean after the fix. Focused
client free-in-done, deadline, Happy Eyeballs and pool suites passed under ASan/UBSan.
The full post-fix readiness sanitizer suite passed, as did pollcomp sanitizer runs of the
free-in-done and Happy Eyeballs suites (10 and 11 cases).

The pre-fix full readiness ASan/UBSan suite and structural gates passed. Seven separately
instrumented fuzz targets each completed 20,000 runs without a sanitizer finding. The local
fuzzer link required selecting GCC 13's installed C++ runtime explicitly. Direct Clang analysis
reported no diagnostics on the applicable core TUs or the changed client TU; this does not replace
the unavailable `scan-build` and `cppcheck` wrappers. No fresh Windows, native io_uring, or optional
integration execution is claimed. Generated fuzz corpus additions were moved to temporary storage.
No commit or push was performed in this follow-up.

## Eighteenth pass: comprehensive re-audit after the hardening round (2026-10-04)

**Scope:** the whole `src/` tree, `include/keel/`, every integration (TLS backends, nghttp2, miniz,
lwIP, UEFI) and the build, at `main` `d757d20`. That is after the hardening round (#415 to #436)
closed. Six parallel read-only reviews covered:
- the HTTP/1 server, readiness and completion;
- the HTTP/1 client stack;
- HTTP/2 and WebSocket, server and client, with the nghttp2 adapter;
- the engines and the completion axis;
- datagrams, DNS, PROXY, pipes and the platform layer;
- the integrations and the build.

Every High was then re-checked against the code by hand. Confidence: **C** means confirmed by
reading the path end to end. **P** means plausible and needing a test. Two Highs were found by two
reviews independently: S1 (also E1) and S2 (also E4).

**Fixes since the seventeenth pass:** every hardening-round fix was traced in its area and holds.
No Critical was found. The new findings fall in three groups:
- **The plaintext twin of the completion TLS work:** S1 and W4. The round moved TLS output on
  completion loops into an overlapped queue; plaintext streaming, WebSocket and the h2c upgrade
  response still `send()` on the loop thread, on sockets that block on io_uring and IOCP.
- **Peer-driven resource exhaustion on HTTP/2:** W1, W2, W3 and W6.
- **Correctness of the integrations themselves,** which the core tests reach only through mocks: I1,
  I2 and I3.

### Mechanical scans and gates

| Category | Result |
|---|---|
| Unsafe str/format, `atoi`/`atol`/`atof`, `alloca` | none in `src/` or `integrations/` (the UEFI libc shim defines `strcpy` for mbedTLS; a definition, not a use) |
| Direct `malloc`/`free` | only the PAL thread trampolines (I6, unchanged) and the default allocator |
| Unchecked allocations, VLAs | none found |
| Local gates | pass on `main` (the round's PRs ran all of them) |
| cppcheck, scan-build | green in CI at `d757d20` |

### High

| # | Location | Finding | Conf |
|---|---|---|---|
| S1 | `http_response.c:608-623` (`response_drain_writer`) via `drain.c:89-99`; `completion_http_server.c:218-237` (`comp_stream_pump`) | **Plaintext streaming on a completion loop sends synchronously on the loop thread** (also found as E1). `kl_drain_write` with an empty buffer calls `kl_sock_send` straight away, for the stream head and every chunk. io_uring and IOCP accepted sockets are blocking, so a client that stops reading an SSE, chunked or compressed stream blocks the whole server. On pollcomp (non-blocking) a direct send can overtake a queued pump op and reorder the chunked framing. | C (path) |
| W4 | `completion_ws.c:51-58` → `http_server_ws.c:170-171` (`conn_write_all`); `http2_server.c:795-808` → `h2_out_conn_write` | **Plaintext WebSocket, and the h2c upgrade's stream 1, send synchronously on a completion loop.** The same class as S1, on the paths the TLS queue does not cover: a client that sends pings and never reads blocks the loop on the auto-pong. | C (path) |
| E2 | `listener.c:150-155` (`l_pump`); `event_iocp.c:505-533` (`iocp_comp_post_accept`) | **One failed accept post closes the completion listener for good.** `arm_accept` returning -1 is treated as a broken listen socket. On IOCP that happens on a transient `WSASocketW` failure, or when a queued connection is reset before AcceptEx takes it. The server keeps running and never accepts again. | C (close path); P (Windows sync-failure codes) |
| W1 | `integrations/http2/nghttp2/http2_nghttp2_server.c:160-193` (`ng_on_header_cb`), `:103-134` | **HPACK bomb: request headers are stored with no limit.** Each header is copied into the per-stream record, and the arrays grow without a count or byte cap (Keel clamps only later). One header in the dynamic table, referenced by one-byte indices across HEADERS and CONTINUATION frames, costs hundreds of MB per stream. | C |
| W2 | `http2_nghttp2_server.c:484` (empty SETTINGS), `:310-315`; `http2_server.c:544-548` | **No limit on concurrent streams, or on response copies queued for a peer that does not read.** The adapter sends empty SETTINGS, so nghttp2's incoming stream limit stays unlimited; a Keel stream is freed once its response is submitted, while the adapter's copy of the body (up to 16 MiB) waits in nghttp2. `initial_window_size` is never read. | C |
| W3 | `http_server_core.c:684-686` | **HTTP/2 connections never time out.** The sweep skips the HTTP2 state, and the session has no idle handling. A client that completes ALPN h2, or sends only the preface, and goes silent holds its slot forever. | C |
| I1 | `integrations/codec/miniz/compress_miniz.c:146-165` (`miniz_feed`) | **The streaming gzip compressor truncates its final block.** On the finishing call `remaining` is 0, so one `tdefl_compress` runs; when the final block needs more than the 4 KiB output buffer, the trailer follows an incomplete deflate stream. Any streamed compressed response of roughly 15 KiB of text or more is corrupt. Reproduced against miniz. | C (run) |
| I2 | `integrations/tls/openssl/tls_openssl.c:145-150` (`kl_bio_write`, completion mode) | **OpenSSL fails any completion-mode write past ~256 KiB.** A full output ring returns -1 with no retry flag, so `SSL_write` fails and the connection closes; the core expects WANT_WRITE and retries after absorbing the ring. Any buffered HTTPS response body over ~250 KiB on io_uring, IOCP, pollcomp, lwIP-raw or UEFI with OpenSSL, BoringSSL or LibreSSL is dropped. | C |
| I3 | `tls_mbedtls.c:231-244`, `tls_openssl.c:354-371` via `drain.c:90-104` | **The drain breaks the TLS write-retry contract.** After WANT_WRITE the drain retries from its own buffer, with a different pointer and possibly a larger length. mbedTLS flushes the pending record and acknowledges the new length, so bytes are reported sent that were never encrypted (a wss frame stream or HTTPS SSE silently corrupts); OpenSSL fails with `BAD_WRITE_RETRY`. | C (code); library semantics cited |
| T1 | `http_client_sync.c:101`, `:157`, `:971`; handshake `:205-224` | **The sync client runs TLS and sends on a blocking socket, so `timeout_ms` is not enforced there.** The fd is put back to blocking after connect; the handshake, `SSL_read` of a partial record and a large `send` then block with no limit. A server that accepts and never sends a ServerHello blocks the caller forever. | C |

### Medium

| # | Location | Finding | Conf |
|---|---|---|---|
| S2 | `http_connection.c:746-754`; `completion_http_server.c:155-163` | On completion with TLS, `100 Continue` stays in the engine's ring until the final response (also found as E4): `curl -T` stalls a second per upload, and a client that waits for it hits the body timeout. | C |
| S3 | `http1_parser_llhttp.c:135`; `http_request.h:84` | On 32-bit builds Content-Length is truncated to `size_t`: `4294967296` reads as 0, so the body is parsed as the next request (smuggling behind a 64-bit proxy). | C (code); P (no 32-bit lane) |
| E3 | sweep `http_server_core.c:668-720`; backends re-post partial sends without an event | The S13 fix is incomplete: a plaintext buffered or file response is one op until it finishes, so a long download never refreshes `last_active_ms` and is cancelled at `read_timeout_ms` (30 s by default). | C |
| E5 | `event_iocp.c:1207-1226` | An IOCP WRITE completion ignores the operation status: a `CancelIoEx` abort that reports partial bytes is re-posted, so a closing connection whose client stopped reading can leak its slot. | P |
| W5 | `http2_nghttp2_server.c:367-371` | "Graceful" shutdown calls `nghttp2_session_terminate_session`, which stops all output after its GOAWAY: in-flight responses are cut. | C |
| W6 | `http2_nghttp2_client.c:92-116`, `:140-158` | The client side of W1: a malicious server can run the same HPACK amplification against the client. | C |
| I4 | `tls_mbedtls.c:165-166`, `:212-214` | mbedTLS maps a bare TCP EOF to close_notify, so a truncated close-delimited HTTPS response is accepted as complete. | C |
| I5 | `tls_openssl.c:1111-1122` | The OpenSSL mTLS server sets no session ID context, so resumption attempts are rejected with a fatal alert. | P |
| X1 | `dns_resolver.c:1144`, `:1172-1180` | The DNS cookie check is keyed on the reply's source, not on the server the query went to: with several nameservers, a spoofer forging from a secondary bypasses the cookie. | C |
| X2 | `dns_resolver.c:249`, `:1235-1241` | SERVFAIL, REFUSED and NOTIMP end the leg with no failover to the next nameserver. | C |
| T2 | `http_client_sync.c:565-602` | The sync client has no overall deadline: interim 1xx responses or a trickle hold it forever. | C |
| T3 | `http_client_sync.c:57`; `http_client_async.c:1515`, `:1858` | Only the first resolved address is tried (sync always; async on its non-Happy-Eyeballs paths). | C |

### Low

| # | Location | Finding |
|---|---|---|
| S4 | `http_response.c:31-60` | Status codes missing from the table (416, 412, 451, 501, ...) are sent as `500`. |
| S5 | `completion_http_server.c:428-441` | Completion with TLS can complete a stream response its producer has not ended. |
| S6 | `http_router.c:24-27` | A NULL route handler is accepted and crashes the first request. |
| S7 | `http_body_reader_multipart.c:112-128` | Multipart parameter scanning looks inside quoted strings (field-name confusion). |
| S8 | `http_compress.c:56-61` | A failed `Vary` append (OOM) leaves `Content-Encoding` on an uncompressed body. |
| S9 | `file_io.c`; `http_connection.c:1161-1269` | The async file-I/O state machine is unreachable (`kl_file_io_create` returns NULL). |
| T4 | `http_redirect.c:158-190`, `:350` | Redirect API argument checks are weaker than the client's; early returns leave `*resp` unset. |
| T5 | `src/url.c:155-190` | The authority does not end at `?`/`#`; SP/CTL and fragments reach the request line; schemes are case-sensitive; relative references are not resolved. |
| T6 | `http_client_async.c:248-251` | A custom resolver's `ai_socktype`/`ai_protocol` are used for the TCP socket. |
| T7 | `http_client_proxy.c:55-58` | The CONNECT status check reads only the three digits. |
| T8 | `http_client_pool.c:147-165` | `kl_http_client_pool_free` leaves `capacity` set over a NULL table. |
| T9 | `http_client_internal.h:137` | `conn_last_err` is dead. |
| E6 | `event_ctx.c:333-347` | `kl_event_ctx_run` returns -1 on EINTR on epoll, kqueue and poll. |
| E7 | `event_pollcomp.c:500-505`; `event_iouring.c:865` | An accept EMFILE/ENFILE spins. |
| E8 | `event_iocp.c:552-561` | The TransmitFile chunk test seam is read from the environment in production builds. |
| W7 | `http2_nghttp2_server.c:221-224` | A failed response submit gets no RST. |
| W8 | `completion_http2.c:79-83` | Plaintext h2 on completion drops the closing GOAWAY. |
| W9 | `http_server_ws.c:716-723` | No WebSocket liveness check: auto-ping never expects a PONG. |
| W10 | `http_server_ws.c:461-470` | With an unlimited drain, auto-pongs can grow it without bound. |
| W11 | `http2_nghttp2_client.c:328` | The HTTP/2 client never refuses server push. |
| W12 | `http_server_ws.c:36-38`, `:749-754` | Public WebSocket entry points lack NULL guards. |
| W13 | `http2_server.c:458-521`; `http_server_ws.c:206` | Write-only fields; `initial_window_size` is never read. |
| X3 | `dns_resolver.c:656-663` | One bad search candidate stops the whole resolve. |
| X4 | `platform_posix.c:47-62` | Linux entropy falls back to the weak generator when `/dev/urandom` cannot be opened. |
| X5 | `unix_socket_node_posix.c:212-218` | The process-wide `umask` is changed during an AF_UNIX bind. |
| X6 | `proxy_protocol.c:48-58` | PROXY v1 validates only the source address and port. |
| L1 | `tls_mbedtls.c:283` | `feed_input` lacks the overflow-safe length check (caller-only). |
| L2 | `tls_mbedtls.c:845-876` | `kl_tls_mbedtls_client_ctx_create(NULL)` disables verification, unlike OpenSSL. |
| L3 | `decompress_miniz.c:377-398` | Bytes after the gzip trailer are ignored. |
| L4 | `integrations/*/Makefile` | The standalone integration builds lack FORTIFY and PIE. |

Already recorded and still open: E7, W11, W18, X5, X6, X8, S10, S11 (seventeenth-pass IDs), and C4.

### Recommended order

1. **The integration Highs,** small and independent: I1 (one loop condition), I2 (a retry flag),
   I3 (a moving-buffer mode and a pending-length guard).
2. **HTTP/2 resource limits:** W1, W2, W6 (one adapter change: SETTINGS plus header caps), then W3
   and W5.
3. **Completion plaintext output:** S1 and W4, by routing plaintext output on completion loops
   through the same per-connection queue the TLS work uses.
4. **E2, T1,** then the Mediums by area, then the Lows.

## Seventeenth pass: re-audit after the sixteenth-pass fixes (2026-10-02)

**Scope:** the whole `src/` tree, `include/keel/` and the nghttp2 and miniz integrations, at `main`
`cf03091`. That is after #365 to #390 closed the sixteenth pass and #367 added the per-hop redirect
check. Five parallel read-only reviews covered:
- the HTTP/1 server;
- the HTTP/1 client stack;
- WebSocket and HTTP/2;
- the transport substrate;
- engines and DNS.

Each review read the `d686342..cf03091` diff first, to look for regressions in the new fixes. Every
High and Medium was then re-checked against the code by hand. Confidence: **C** means confirmed by
reading the path end to end. **P** means plausible and needing a test.

**Sixteenth-pass fixes:** each was traced and holds for what it targets. The new findings fall in
three groups:
- **Regressions caused by those fixes:**
  - E12: E5 made pollcomp sockets non-blocking, which TLS flushes on that engine do not expect.
  - S16: S9 caps the leftover count on keep-alive, and that hides an over-send on early rejections.
- **Incomplete fixes:**
  - T19: T8 checks the decompression limit only after full inflation.
  - S15: S1 runs post-body middleware, but its header pointers are stale.
  - W24: W1 covers only the event path.
  - E13: E4 is fixed on IOCP only.
  - X9: X4 missed `dg_multicast`.
- **Older defects next to the fixes:** the rest.

### Mechanical scans and gates

| Category | Result |
|---|---|
| Unsafe str/format, `atoi`/`atol`/`atof`, `alloca` | none in `src/` |
| Direct `malloc`/`free` | only the PAL thread trampolines (I6, unchanged) |
| Local gates | all 21 pass (incl. `check-public-headers`, `check-no-fsnode-in-protocols`, `check-tier1-boundary`) |
| `clang --analyze` (MinGW, Windows-branch view) | 0 warnings |
| cppcheck | not available locally; runs in CI (green at `cf03091`'s PRs) |

### High

| # | Location | Finding | Conf |
|---|---|---|---|
| W19 | `integrations/http2/nghttp2/http2_nghttp2_server.c:377-382` (`ng_server_destroy`); client twin `http2_nghttp2_client.c:243-248` | **Per-stream adapter records leak when a session is destroyed with streams open.** `NgServerStream` holds a copy of the response body of up to 16 MiB. It and `NgClientStream` are freed only in `ng_on_stream_close_cb`, and `nghttp2_session_del` frees its streams without calling that callback. A client that requests a large response and drops TCP leaks the copy. This is remote, unauthenticated and repeatable. | C (adapter); P (nghttp2's no-callback-on-delete, per upstream `free_streams`) |
| W20 | `src/protocols/http2/http2_server.c:490-493` (`h2_out_conn_write`) via `ng_send_cb` | **Plaintext readiness HTTP/2 treats a would-block send as fatal.** `conn_write` returns -1 with EAGAIN on the non-blocking socket, and `ng_send_cb` maps -1 to `NGHTTP2_ERR_CALLBACK_FAILURE`. Any h2c response larger than the free send buffer (prior knowledge, or the new Upgrade stream 1) kills the connection and every stream on it. TLS (WANT_WRITE = 0) and completion (capture writer) are unaffected. | C |
| E11 | `src/socket_posix.c:217-222` (`__APPLE__` `kl_sockdef_sendfile`); callers `http_response.c:587-593`, `event_pollcomp.c:555-563` | **macOS: a file response stops at the first full send buffer and is reported complete.** On a non-blocking socket `sendfile` returns -1/EAGAIN with `len == 0`. The wrapper returns 0 for EAGAIN, and both callers read 0 as end of file. A file larger than the send buffer is cut short with Content-Length unmet, and keep-alive desyncs. kqueue (the macOS default) has always had this; since E5 pollcomp on macOS has it too. | C (code); P (not run on macOS) |

### Medium

| # | Location | Finding | Conf |
|---|---|---|---|
| S13 | `completion_http_server.c:813-893` (`comp_on_read`/`comp_on_write`); sweep `http_server_core.c:647` | **On completion loops, active transfers time out.** `last_active_ms` is never refreshed by a completed receive or send; it is set only at accept, at the TLS handshake and at keep-alive reset. A download or upload still running `read_timeout_ms` after the last reset is cancelled mid-transfer on IOCP, io_uring and pollcomp. epoll refreshes the clock, so this breaks the documented "active transfers are never timed out" rule only on completion. | C |
| S14 | `http_server_core.c:752-760` (`kl_http_request_resume_body`); `completion_http_server.c:942-945` | **Resuming a body read on completion can post a second receive.** The resume posts unconditionally, and `comp_recv_posted` is a single yes/no flag. Pausing then resuming in the same dispatch puts two receives into `read_buf`, and the body bytes overlap. Resuming while SENDING can leave a receive in flight when `conn_keepalive_reset` shrinks `read_buf`, so the kernel writes into freed heap. | C (path); P (app pattern) |
| S15 | `http_connection.c:357-369`, `:287-296`, `:1041` | **S1 is only half fixed.** Post-body middleware now runs, but it and the `access_log` callback still get `method`, `path` and header pointers into a `read_buf` that the body read has overwritten from offset 0, with the NUL terminators gone. A CSRF check reads body bytes as `Cookie`/`Origin`. A body that fills `read_cap` with no NUL can make a `strlen` on a header value read out of bounds. Access-log lines can be forged. | C (stale pointers); P (exploit) |
| T19 | `http_client_common.c:316-328`; `integrations/codec/miniz/decompress_miniz.c:132-190` | **T8 is incomplete for buffered bodies.** `max_response_size` is compared only after `decompress()` has inflated the whole body, up to miniz's 256 MB cap. A small gzip body still costs hundreds of MB, allocated synchronously on the loop thread, before `KL_ERR_TOO_LARGE`. The streaming path is bounded. | C |
| E12 | `completion_http_server.c:226-243` (`kl_comp_tls_flush`) vs `event_pollcomp.c:486-506` | **Regression from E5.** `kl_comp_tls_flush` treats any send ≤ 0 as fatal, written for a blocking socket. Since pollcomp accepted sockets became non-blocking, a full send buffer drops TLS streaming responses, SSE, WebSocket-over-TLS and upgrade flushes on pollcomp. io_uring and IOCP sockets are still blocking. | C |
| W21 | `integrations/http2/nghttp2/http2_nghttp2_client.c:114-135` | **HTTP/2 client: the final response after a 1xx is never reported.** nghttp2 delivers it as `HCAT_HEADERS`, and the adapter reports only `HCAT_RESPONSE` while appending headers across blocks. After a `103` then a `200`, the client reports status 103 with the 200's body. | C |
| W22 | `src/protocols/websocket/websocket_client.c:609-639`, `:677-695` | **The WebSocket client handshake does not drain TLS-held plaintext.** A `wss://` 101 response over 511 bytes in one record is read partially, and the connection then waits for socket readiness that never comes. It hangs, and there is no handshake timeout (W11). | C (code); P (trigger) |
| W23 | `src/protocols/http2/http2_client.c:161-169`, `:472-481`, `:714-715` | **The HTTP/2 client never asks for WRITE readiness.** A short `on_send` leaves output buffered in nghttp2 while only READ is armed. A request body larger than the send buffer can deadlock against a peer that sends nothing. TLS WANT_WRITE now reaches this. | P |
| W24 | `src/protocols/http2/http2_client.c:714-715`, `:271-290` | **W1 covers only the event path.** `kl_http2_client_request` flushes outside `in_event`. A stream close emitted there runs `on_resp`, and a `kl_http2_client_free` from that callback destroys the session under `nghttp2_session_send`. | P |
| W25 | `http2_server.c:425-433`; adapter `:195-196` | **One stream's body limit kills the whole HTTP/2 connection.** An over-limit body, or a reader returning -1, is mapped to a fatal session error. No 413 is sent, and every multiplexed stream dies (W6's sibling). | C |
| W26 | `http_server_ws.c:156-160` with `http_internal.h:56-71` | **WebSocket server sends without the drain can leave a truncated frame on the wire.** `conn_write_all` gives up mid-frame on EAGAIN, or after the TLS spin limit, and the connection stays open. The next frame starts inside the old payload, and framing desyncs. | C |

### Low

| # | Location | Finding | Conf |
|---|---|---|---|
| S16 | `http_connection.c:579-585` | **Regression from S9:** the leftover count is capped at Content-Length on keep-alive. `kl_http_conn_begin_drain` runs only after keep-alive is cleared, typically on an early rejection (401/413/415 from pre-body middleware), and the cap hides an over-send there. The connection then closes on unread bytes and the response is reset. | C |
| S17 | `http_connection.c:716-733`, `:751-770`, `:822-827` | A body-reader rejection or a malformed chunk inside the leftover bytes closes the connection with no 413 and no drain; the body-phase path sends a 413. | C |
| S18 | `completion_http_server.c:580-597` | Completion with TLS ignores `read_paused`: `comp_tls_drive` always posts the next receive. | C |
| S19 | `http_server_core.c:272-409` | `proxy_cidrs` leaks on every `kl_http_server_init` failure after it is allocated, including the new NULL-h2-factory path. | C |
| S20 | `http_cors.c:92-116` | `Vary: Origin` is sent only when the origin is allowed; responses to a missing or disallowed Origin also vary by Origin. | C |
| S21 | `http_response.c:719-730` | `kl_http_response_end_stream` returns for HEAD before setting `stream_ended`, so a HEAD stream that reaches SENDING never completes. | P |
| S22 | `http_server_core.c:678-679`; `completion_http_server.c:205-206` | S4 covers READING_BODY only. A completion SENDING stream with nothing posted, waiting on a quiet producer, is never released by the sweep. | P |
| T20 | `http_client_async.c:1118-1127` | The deferred-error window leaves the winning fd's watcher live. An inline deferral (sync resolver, immediate connect) can re-enter CONNECTING and call the TLS factory again, leaking the first `KlTls`. | P |
| T21 | `http_client_common.c:273-281` | An HTTP/1.0 response without `keep-alive` is pooled (version not consulted). Was T18's HTTP/1.0 part, still open. | P |
| T22 | `src/url.c:145-151`; request builders | IPv6 literal hosts lose their brackets in Host, absolute-form and CONNECT, and Host drops a non-default port. | C |
| T23 | `http_redirect.c:428-453` | Async redirect: when `kl_timer_add` fails, the deferred error completes inline before `rc->inner` is assigned. If `on_done` frees `rc`, there is a use-after-free write (OOM only). | P |
| W27 | adapter `http2_nghttp2_server.c:161-170` | `on_request` failure is ignored: the stream gets neither a response nor RST_STREAM. | C |
| W28 | `http2_server.c:124-126` | The HTTP/2 server sends a DATA body for HEAD. | C |
| W29 | `http_connection.c:604`, `http2_server.c:379` | On h2c Upgrade, pre-body middleware runs twice (HTTP/1 phase, then stream 1). | C |
| W30 | `http_connection.c:631-639` | `Upgrade: h2c` is honoured on TLS connections, which is cleartext-only per RFC 7540 3.2. | C |
| W31 | `http2_server.c:661-672` | A malformed `HTTP2-Settings` is detected only after the 101 has been sent. | C |
| W32 | `http2_server.c:583-588`, `:521-527` | `h2_conn_abandon` frees the stream table without destroying live streams (prior-knowledge failure path). | C |
| W33 | `websocket_client.c:934` | The TLS pending drain runs only in OPEN, so a close echo held by TLS in CLOSING waits for the next socket event. | C |
| W34 | `websocket_client.c:934`, `http2_client.c:479` | Both clients call `tls->pending` without validating the factory's vtable; a backend without it crashes. | P |
| W35 | `http2_server.c:390-402` | A reader is created even for bodiless requests (END_STREAM on HEADERS), so a factory that requires a body type answers 415. | C |
| W36 | adapter `http2_nghttp2_client.c:122` | A header-copy failure in the client adapter is ignored (the client twin of I2). | C |
| W37 | `http_server_ws.c:83-92` | `ws_drain_writer` reads errno after a TLS -1 (the stale-errno class). | C |
| W38 | adapter `http2_nghttp2_client.c:219` | `:scheme` is hard-coded to `https`, including for h2c over `http://`. | C |
| W39 | adapter `http2_nghttp2_server.c:80-98` | A partial `ng_sstream_grow` failure frees arrays at the wrong size (OOM only). | C |
| X9 | `src/datagram.c:861-876` | X4 follow-on: `dg_multicast` does not check for a cleared fd. It passes `KL_INVALID_SOCKET` to the provider and reports `KL_ERR_IO`. | C |
| X10 | `src/url.c:236-249`; `http_redirect.c:77-93` | A redirect between two `http+unix` sockets counts as same-origin (host and port are empty), so `Authorization` and `Cookie` follow it to another local socket. | C |
| X11 | `src/platform_wakeup_win.c:130-187` | The Windows wakeup pair keeps Nagle on, so a 1-byte signal can wait up to the delayed-ACK timer (~200 ms). | C (code); P (latency) |
| X12 | `src/sockaddr_native.h:93-96` | The AF_UNIX path length is bounded by `sizeof sun_path`, not the returned `len`, so stale bytes can appear in a peer path (in bounds). | P |
| X13 | `src/datagram_recv.c:226-244` | No per-event receive budget: a UDP flood keeps one readable callback running and starves timers. | P |
| X14 | `src/platform_pipe_win.c:39-56` | The pipe-name locality check is textual only. Whether `..` after `\\.\pipe\` can reach a remote pipe is untested. | P |
| X15 | `src/timer.c:68` | `now + delay_ms` wraps for "never" values near `UINT64_MAX`, so the timer fires at once. | C |
| E13 | `event_pollcomp.c:562`; `event_iouring.c:608-611`, `:925-928`; `http_response.c:593` | E4 is fixed on IOCP only. A file that shrinks after sizing is still reported fully sent on pollcomp, io_uring and readiness. | C |
| E14 | `src/event_iocp.c:1082-1101` | Any watcher re-post failure, including a transient one, marks the watcher dead. A healthy socket is then reported every tick, and the loop spins. | P |
| E15 | `dns_resolver.c:1189` | The cookie drop is permanent per nameserver. A mixed fleet behind one address (some members without cookies) loses valid answers to timeouts. | P |
| E16 | `dns_resolver.c:1167-1181` | The TC branch runs before the cookie check, so a cookie-less spoofed TC answer can still force TCP fallback (no poisoning). | C |

Already recorded and still open: E7, W11, W18, X5, X6, X8, S10, S11.

### Recommended order

1. **The three Highs:**
   - W20: map a plaintext would-block to 0 in `h2_out_conn_write`.
   - E11: return -1/EAGAIN from the Apple `sendfile` wrapper when nothing was sent.
   - W19: track live per-stream records in each nghttp2 adapter and free them on destroy.
2. **The two regressions from the sixteenth-pass fixes:** E12 and S16.
3. **Completion server correctness:** S13, S14, S18 and S22, with S15 as its own change (keep the header region valid through the body read).
4. **The rest of the Mediums** (T19, W21 to W26), then the Lows by area.

## Sixteenth pass: re-audit after the fifteenth-pass fixes (2026-10-01)

**Scope:** the whole `src/` tree plus `include/keel/` at `main` `d686342`, after #348 to #364 closed
every fifteenth-pass finding. Five parallel read-only reviews covered:
- the HTTP/1 server;
- the HTTP/1 client stack;
- WebSocket and HTTP/2;
- the transport substrate;
- engines, sockets and DNS.

The significant findings were then re-checked against the code by hand. Confidence: **C** means
confirmed by reading the path end to end, and **C, run** means reproduced. **P** means plausible and
needing a test.

**Fifteenth-pass fixes:** all verified complete, apart from two follow-ons recorded below:
- T1: the H1 deferral leaves the request deadline armed.
- W-L: L12 missed one `upgrade_cap` free.

I7 (`h2c_on_response` leaking on a second call for a stream) is still open.

### Mechanical scans and gates

| Category | Result |
|---|---|
| Unsafe str/format, `atoi`/`atol`/`atof` | none in `src/` |
| Direct `malloc`/`free` | only the default-allocator seam and the PAL thread trampolines (I6, unchanged) |
| Dead code, VLAs, stack arrays | none; every macro-sized stack buffer is at most 16 KiB |
| Build hardening | unchanged (`-fstack-protector-strong`, `_FORTIFY_SOURCE=3`, PIE, RELRO/now, noexecstack) |
| Local gates | all 18 pass |
| `clang --analyze` (MinGW, Windows-branch view) | 87 TUs analyzed, 0 failed, 0 warnings |

### High

| # | Location | Finding | Conf |
|---|---|---|---|
| S1 | `http_connection.c:766,826,962`; `http_router.c:248-258`; completion twin in `completion_http_server.c` | **Post-body middleware is matched against overwritten bytes.** Body reads land at `read_buf[0]`, where `req->method`/`req->path` point. The documented limit covers handlers: header pointers die once body reading starts (`overview.md:222`). But Keel's own `kl_http_router_run_post_middleware` then matches on those pointers. A POST whose body arrives in a later read skips its post-body middleware, the documented home of CSRF checks, while the handler still runs. | C |
| X3 | `thread_pool.c:225-232` (with `:140`, `:158-159`) | **Work-queue ring overflow.** The ring holds `queue_cap` items, but admission only checks `inflight < queue_cap + num_workers`. A burst of submits before idle workers dequeue wraps the tail over unconsumed items. Those items are lost, so a suspended connection hangs. The items that overwrote them run twice: a double `done_fn`, double `kl_async_complete` or double free. | C |

### Medium

| # | Location | Finding | Conf |
|---|---|---|---|
| S2 | `http1_parser_llhttp.c:85-101`; `http_connection.c:543-552` | llhttp reports an empty header value (`X:\r\n`) at the first byte of the NEXT line, with length 0. NUL-termination then blanks that header's name, so it vanishes from lookups. With `Content-Type` gone, for example, multipart returns 415. | C, run |
| S3 | `http_server_core.c:729-740`; HTTP conns never re-run the read-facet init | `read_paused` survives release. A client that disconnects while its body is paused poisons the pool slot, and every later upload on that slot stalls (408 on readiness). | C |
| S4 | `http_server_core.c:609-610,663-664` | On a completion loop, the sweep reclaims a timed-out connection only by cancelling an in-flight op. A connection with nothing in flight (a paused body read, S3) is never released, leaking the slot, fd and credit. | C (logic) |
| S6 | `http_router.c:129-130` vs `:166-167` | Route matching tolerates a trailing `/`, but exact middleware matching does not. `GET /admin/` reaches the `/admin` handler and skips the `/admin` middleware (auth bypass). | C |
| T1 | `http_client_async.c:1087-1093`, `:418-427`, `:1073-1078`, `:1558` | H1 follow-on: the deferred-error path leaves `deadline_timer` armed. If both timers are due in one pass, `he_on_deadline` completes inline (the connect op is now DETACHED), and `async_deferred_error` completes again because it has no DONE check. The result is a double `on_done`, or a UAF if the first call freed the client: free skips `done_timer` once the state is DONE. | C |
| T2 | `http_client_async.c:536-561` | `https://` with `cfg->tls` set but `factory == NULL` falls to the plaintext branch (fail-open). The pooled client then files the plain connection under `is_tls=1`. The sync and tunnel paths fail closed. | C |
| T3 | `http_client_async.c:733-737` and the other send sites; TLS backends return 0 for WANT_WRITE | A TLS write that hits backpressure is treated as a fatal I/O error. Async HTTPS uploads larger than the socket buffer fail with `KL_ERR_IO`. | C |
| T4 | `http_client_pool.c:27-42` | Pooled connections are keyed by host/port/is_tls/proxy only, not by TLS config. A strict-verify request can reuse a connection made with verification off, or under another mTLS identity. The keying is documented, but it is a security hazard. | C |
| T5 | `http_client_sync.c:546-550`; `http_client_pool.c:173` | Sync pooled HTTPS polls the socket while TLS holds buffered plaintext, stalling until `timeout_ms`. Reused connections also stay non-blocking, so TLS "retry" zeros are read as EOF. | C / P |
| X1 | `stream_write.c:229-230`, `:123` | An abortive close (`kl_stream_cancel`) still pumps the next queued batch after a write completion, and never cancels it. A peer that stops reading then pins the stream, and `on_close` never fires. | C (path) |
| X2 | `datagram.c:557-565` | `kl_datagram_send` has no `dispatch_begin/end` bracket, unlike the batch and GSO paths. `on_drain` can call `close_cancel`, whose `on_close` can call `kl_datagram_free` inside the send; `dg_reconcile_write` then touches freed state. | C |
| W1 | `http2_client.c:255-258` | The HTTP/2 client has no free-from-callback protection. `kl_http2_client_free` in `on_resp` frees `c` and the session, then `h2c_stream_remove` and nghttp2 run on freed memory. The WebSocket client got this protection in #361. | C |
| W2 | `http_server_ws.c:600-604`; `http2_server.c:624-625`; `websocket_client.c:563-576,833-841`; `http2_client.c:416-419` | The readiness WS and H2 paths treat a TLS `read() == 0` (WANT_READ) as EOF and `write() == 0` as fatal. A TLS record split across TCP segments drops the connection. The completion twins are correct. | C (code) / P (trigger) |
| W3 | `http_server_ws.c:596-607`; `websocket_client.c:820-846` | The WS server and client read one 8 KiB buffer per event without draining `tls->pending()`. The rest of a 16 KiB record waits for the peer's next send. | P |
| W4 | `http_server_ws.c:401-419` | The WS server does not validate close frames (the client does since #361), and echoes 1005 on the wire for an empty close. | C |
| W5 | `http_server_ws.c:352-353` | With no `on_message` handler, `ws_deliver_message` returns before resetting message state, so the second message fails the connection with 1002. | C |
| W6 | `http2_server.c:403-404` | DATA for a stream Keel already finished (a pre-body rejection) returns -1. The nghttp2 adapter maps that to a fatal session error, aborting every multiplexed stream. | P |
| W7 | `http2_server.c:579-586` | h2c Upgrade sends 101 but never serves the upgrading request on stream 1 (RFC 7540 3.2), so `curl --http2` waits forever. | C |
| W8 | `http2_client.c:596-617` | After an immediate connect, the watcher keeps WRITE interest and every rearm fires again: a busy loop on `http+unix`. | P |
| S5 | `integrations/codec/miniz/compress_miniz.c:62-95`; `http_compress.c:48-68` | miniz allocates `10 + bound + 8` bytes but reports the smaller `total`, and core frees with the reported size. Every compressed response has an allocator size mismatch. | C |

### Low

| # | Location | Finding | Conf |
|---|---|---|---|
| S7 | `http_sse.c:26-48` | SSE field injection: a bare `\r` in `data`, or CR/LF in `event`/`id`, injects fields. Line `:46` also advances the pointer past `end + 1`. | C |
| S8 | `completion_http_server.c:456-462` | h2c prior-knowledge on completion passes the preface without its magic, while readiness passes it whole. With h2 configured but no hooks, a `memcmp` over-reads the 25-byte preface array. | C / P |
| S9 | `http_connection.c:687-754` | Body bytes that arrive with the headers are not counted. A rejected or `Connection: close` request lingers in DRAINING until EOF or 500 ms. | C |
| S10 | `http1_chunked.c:74-80,157-166` | Chunk extensions and trailers accept bare LF and CTLs, with no length bound, outside `max_body_size`. | P |
| S11 | `http_connection.c:462,476-478` | A partial PROXY header from a trusted peer busy-loops on level-triggered readiness. | P |
| T6 | `http1_response_parser_llhttp.c:219,323`; `http_client_common.c:411` | After a 1xx with headers and then a final response with none, the 16-slot header array is freed with size 0. | C |
| T7 | `http_client_async.c:1155-1157` | A user resolver without `cancel` is accepted, though the contract requires it. A timeout during RESOLVING then leads to a callback into freed memory. | C |
| T8 | `http_client_common.c:254-316`; miniz 256 MB cap | `max_response_size` does not bound decompressed output, and the async client ignores a decompression failure. | C |
| T9 | `http_client_async.c:432-464` | The request deadline starts after resolution, so a stuck resolver hangs the request. | C |
| T10-T18 | client (see review notes) | <ul><li>Pool peek treats stray bytes as a healthy connection.</li><li>`set_hostname` is optional, so a backend without it skips the hostname check.</li><li>Proxy auth is not checked for CR/LF.</li><li>A 101 ending at a read boundary is pooled.</li><li>The final decompress flush result is ignored.</li><li>A `body_read` that returns too much is not rejected.</li><li>An OOM during transfer leaks the body.</li><li>`reset` keeps `body_streamed`.</li><li>`Connection: close, x` is pooled.</li></ul> | C / P |
| W9-W18 | WS/H2 (see review notes) | <ul><li>A full stream table, or RST(NO_ERROR), leaks a slot.</li><li>The request is submitted before its stream is created.</li><li>The client `timeout_ms` is never used.</li><li>Fragmentation edge cases.</li><li>An OOM while copying leftover bytes drops them.</li><li>The handshake checks too few headers.</li><li>Calling `enable_drain` twice.</li><li>An H2 body without content-length gets no reader.</li><li>A NULL h2 factory is not rejected.</li><li>Swap-remove moves `req`.</li><li>W-L: `websocket_client.c:1110` still frees `upgrade_buf` with `upgrade_len`.</li></ul> | C / P |
| X4-X8 | substrate | <ul><li>Datagram calls reuse a stale `dg->fd` after close.</li><li>Freeing the thread pool from `done_fn`.</li><li>A 0 ms timer re-armed in its own callback starves I/O.</li><li>`kl_free(NULL, size)` in a batch unwind.</li><li>A wrong-family accept leaks the accepted value.</li></ul> | C / P |
| E1-E10 | engines/DNS | <ul><li>E1: a failed IOCP re-arm retires the watcher silently.</li><li>E2: io_uring `kl_event_del` drops POLL_REMOVE when there is no SQE.</li><li>E3: IOCP TransmitFile with count 0 sends the whole file.</li><li>E4: a truncated file is reported as fully sent.</li><li>E5: pollcomp makes accepted sockets blocking.</li><li>E6: DNS cookies are bypassed by omitting them.</li><li>E7: the UDP source port is fixed.</li><li>E8: the `/dev/urandom` fallback is constant.</li><li>E9: `open`/`fopen` are outside the cloexec gate.</li><li>E10: Windows hosts-path init is not thread-safe.</li></ul> | C / P |

### Recommended order

1. **The two Highs.**
   - S1: match post-body middleware at header time (a per-request bitmask) rather than after the body.
   - X3: bound admission on `work_count < work_cap`.
2. **The client and server correctness Mediums:** T1, T2, S2, S3+S4, S6, X2, W1.
3. **The TLS readiness group** (T3, T5, W2, W3) as one change, with a split-record test.
4. **The rest**, grouped by family as in the fifteenth pass.

## Fifteenth pass: whole-tree re-audit after the transport and pipe work (2026-09-30)

**Scope:** whole `src/` (103 `.c`, 33,411 lines) including `src/protocols/`, the internal and public
headers, and the `Makefile`, at `main` `f0d9085`. Trigger: everything merged since the fourteenth pass
(2026-08-26): the `KlStream` facets and writable-again edge, `KlListener`, Windows Named Pipes,
anonymous pipe pairs (Windows IOCP and POSIX readiness), close-on-exec on Keel's own descriptors, and
the MSVC/allocator-validation work. Five read-only reviewers swept the tree by subsystem (transport
substrate + readiness core; completion engines + socket axis; datagram + DNS; HTTP server + parsers;
clients + HTTP/2 + WebSocket); every Critical, High and Medium below was then re-traced against the
code, and C1 was reproduced end to end.

**Verdict: NOT CLEAN.** 1 Critical / 3 High / 9 Medium / 12 Low / 7 Informational. The Critical is
a long-standing HTTP/1 server defect that the suite never exercised: a request whose request line or
headers arrive in more than one read is rejected on both event models. None of the findings is in the
new transport or pipe code; those modules came out clean.

**Verification key:** **R** = re-traced in code by the auditor (and **E2E** = reproduced by running
it); **C** = cited from a slice reviewer, read but not independently re-traced.

### Mechanical scans (whole tree)

| Category | Result |
|---|---|
| Unsafe str/format (`strcpy`/`strcat`/`sprintf`/`vsprintf`/`gets`) | none in `src/` |
| Unsafe int parse (`atoi`/`atol`/`atof`) | none |
| Direct `malloc`/`free` | only the PAL thread trampolines (`platform_thread_posix.c:17,24,28`, `platform_thread_win.c:28,35,39`), deliberate and commented (the trampoline state is the thread's own and freed by it); see I6 |
| Dead code (`#if 0` / `if (0)`) | none |
| VLAs / large stack arrays | none found |
| Build hardening | `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, PIE, `-z relro -z now -z noexecstack` in production flags; `make debug` = ASan+UBSan |
| CI (PR #346 head `d8b7bb4`, tree identical to `f0d9085`) | 26/26 green incl. ASan+UBSan, sanitized io_uring and pollcomp, cppcheck, scan-build, all gates |

### Critical

| # | File:Line | Issue | Evidence | Fix |
|---|---|---|---|---|
| C1 | `src/protocols/http/http_connection.c:886` (readiness), `src/protocols/http/completion_http_server.c:468` (completion); parser `http1_parser_llhttp.c:161` | **A request split across reads is re-fed from byte 0 to a stateful parser.** After `KL_HTTP1_PARSE_INCOMPLETE` the connection returns to READING; the next read appends to `read_buf` and `parse()` is called again on `read_buf[0..read_len)`. `llhttp_execute` continues from where it stopped, so it parses the already-seen bytes a second time. The parser is reset only on keep-alive and when the header buffer grows. | **E2E, R.** Parser probe, request `GET /x HTTP/1.1\r\nHost: a\r\nX-Y: zz\r\n\r\n` split at every one of 36 points: re-feeding the whole buffer (as the connection does) is wrong at **36/36** (most `PARSE_ERROR`; splits 22-24 and 30-33 return `HEADERS_OK` with phantom headers; splits 5-6 leave `path_len = 4272` for a 2-byte path, an out-of-bounds length that `on_url_complete` then scans); feeding only the new bytes is correct at 36/36. A real `KlHttpServer` on loopback: the request in one write gets `200 OK`, the same request in two writes 200 ms apart is closed without a response at every split tried, on **WSAPoll and IOCP** alike. | Track a parsed offset per connection and pass only `read_buf + parsed_off .. read_len` to `parse`, advancing it by `consumed` on INCOMPLETE; reset it wherever the parser is reset. On the completion grow path also reset the parser and `c->req` (as readiness does), since a realloc moves `read_buf`. Add a split-request test at many split points on both event models. |

Why it survived: loopback clients (the test suite's own client, curl) deliver a small request in one
segment, and the existing partial-header tests (`test_timeout.c` `partial_headers`, the slowloris
smokes) never complete the request. Real traffic splits: large cookies, slow links, TLS records.

### High

| # | File:Line | Issue | Evidence | Fix |
|---|---|---|---|---|
| H1 | `src/connect_op.c:81-84` `co_terminal`; `src/protocols/http/http_client_async.c:331-344` `cli_co_on_done`, `:1491` `kl_http_client_free` | **Use-after-free when the client is freed inside `on_done` after a DNS or connect failure.** `cli_co_on_done` calls `async_complete_error`, which runs the user's `on_done`; `kl_http_client_free` frees the client unconditionally, and the `KlConnectOp` embedded in it is still mid-dispatch (`co_request_cancels`, `in_dispatch--`, `co_finalize` follow). With an owned resolver, the resolver is also destroyed inside its own callback. The shipped `examples/async_client.c:39` frees in `on_done`; the redirect wrapper does the equivalent. | **R.** NXDOMAIN, or every address refusing, reaches it. Tests free after the loop returns, so ASan never saw it. | Complete the request from `cli_co_on_detach` (the point where freeing is legal) instead of from inside the terminal dispatch, and document on `KlHttpClientDoneFn` whether freeing inside it is allowed. |
| H2 | `src/protocols/http/http_client_async.c:150` `start_connect`; same pattern `websocket_client.c:985`, `http2_client.c:570` | **TLS (and a proxy CONNECT tunnel) is skipped when a non-blocking connect completes at once.** `c->state = (rc == 0) ? KL_HTTP_CLIENT_SENDING : ...` bypasses `he_proceed_after_connect`, where the TLS session and the tunnel are set up. AF_UNIX connects complete at once on Linux, so `https+unix://` / `wss+unix://` send in plaintext with no certificate check: fails open. | **R.** `tests/test_unix_socket.c` uses a pass-through mock TLS and the sync client, so the plaintext send is invisible to it. | On `rc == 0`, go through `he_proceed_after_connect` (and the WebSocket/HTTP/2 equivalents) exactly as the asynchronous path does. |
| H3 | `src/protocols/websocket/http_server_ws.c:505-513`; `src/protocols/websocket/websocket_client.c:690-693` | **A WebSocket data frame larger than one read breaks.** `is_first` is derived from the opcode alone, so the second payload chunk of the same TEXT/BINARY frame looks like a new message. Server: `msg_len > 0` closes with 1002, so any message over about one read buffer, or one straddling a TCP read, is rejected. Client: the earlier chunks are discarded and `on_message` sees only the tail. Control-frame payloads split across reads are handled from their tail only. | **R** (server path); client path **C**. | Treat a frame as the start of a message only when its payload offset (`payload_before`) is 0; same for control frames. Add a test that sends one frame in several writes. |

### Medium

| # | File:Line | Issue | Evidence | Fix |
|---|---|---|---|---|
| M1 | `src/protocols/dns/dns_resolver.c:1051-1066` `dns_tcp_on_event`, `:969-1007` `dns_tcp_deliver`, `:920-935` `dns_tcp_fail`; `:453-456` | **Use-after-free when the resolver is destroyed from `done()` during a DNS-over-TCP completion.** `dns_complete`'s tail runs `dns_teardown`, which frees `r` synchronously when no datagram frame is active; the TCP path runs from a plain watcher, so after `dns_leg_settle` the deliver loop (`t->rneed`, `t->rlen`), `dns_tcp_fail`'s rescan and `dns_tcp_arm_idle` use freed memory. A nameserver that answers TC=1 drives the path. | **R.** | Bracket `dns_tcp_on_event` with `r->in_done++ ... --` and run the deferred teardown at its tail; return early after any settle once `destroy_requested` is set. |
| M2 | `src/resolver_cache.c:246-263` `cache_resolve`; `src/protocols/http/http_client_async.c:204-209` | **Every resolver-cache hit leaks the request handle under `KlHttpClient`.** A hit completes synchronously and returns a live `KlResCacheReq` that only `cache_cancel` frees; the client drops the handle whenever resolution completed inline. The resolver contract is also inconsistent: the DNS resolver frees its request inside `done`. | **R.** | Free the handle on synchronous completion and return NULL; state in `resolver.h` that a handle is dead once `done_fn` has run. |
| M3 | `src/event_iocp.c:259-279` `iocp_watch_reinterest` | **Each IOCP watcher interest change made from a callback leaks an op until loop close.** Between a dispatched completion and its re-arm the op owns no kernel I/O (`watch_rearm = 1`); reinterest marks it removed and calls `CancelIoEx`, which cancels nothing. The delete path handles this state (`:304-309`), reinterest does not. The async HTTP and WebSocket clients do exactly this (WRITE to READ after connect), so it is at least one op per request. | **R.** | If `old->watch_rearm`, just update its mask and udata and return; the next drain re-posts it. |
| M4 | `src/event_iocp.c:1060-1066` `iocp_watch_rearm_dispatched` | **A failed watcher re-arm leaves an op nothing frees, and `kl_event_close` then waits forever.** On failure `watch_rearm` is cleared and `watcher_removed` set; the delete path takes the `CancelIoEx` branch (nothing pending) and the quiesce pre-pass frees only `watch_rearm` ops, so the drain loop blocks with `INFINITE`. | **R.** | On failure keep `watch_rearm = 1` (with `watcher_removed = 1`) so both existing retirement paths free it. |
| M5 | `src/socket_posix.c:102-104` `kl_sockdef_socket`; `src/event_iouring.c:662`; `src/event_pollcomp.c:497`; `src/event_iocp.c:481`; `socket_winsock.c` | **Sockets can leak into child processes.** The default POSIX/Winsock socket creators and the completion-engine accepts (io_uring flags 0, pollcomp plain `accept`, IOCP `WSASocketW` without `WSA_FLAG_NO_HANDLE_INHERIT`) create inheritable sockets; only the readiness server's accept and the listener set close-on-exec. `check-cloexec` does not list socket creators. An embedder that spawns children (Hull) hands them open client and accepted connections, so peers never see EOF. | **R.** | `SOCK_CLOEXEC` / `accept4` / `WSA_FLAG_NO_HANDLE_INHERIT` at each creator; extend `check-cloexec` to `socket(`, `accept(`, `WSASocketW`, `io_uring_prep_accept`. |
| M6 | `src/platform_wakeup_posix.c:20-54`; `src/platform_wakeup_win.c:68-87`; `src/thread_pool.c:82,97` | **The wakeup write end is blocking and the drain reads 64 bytes, so a saturated pool can deadlock.** One byte per completed item; one 64-byte read per watcher callback. Under sustained completion rates above 64 per tick the pipe fills, workers block in `write`, and `kl_thread_pool_free` then joins a worker that nobody will unblock. `wakeup.h` promises that a full channel only loses a coalesced signal. | **R.** | Make the write end non-blocking and ignore EAGAIN; optionally drain until EAGAIN. |
| M7 | `src/protocols/http/http_client_async.c:1611-1640` `kl_http_client_start_pooled` | **Pooled requests cancel timer id 0 and have no deadline.** After `memset` the timer ids stay 0 (timer ids start at 0) and `timeout_ms` is never copied, so every completion or cancel calls `kl_timer_cancel(ev, 0)`, killing whichever timer holds id 0, and a pooled request to a silent server never times out. | **R** (no `-1` init or `timeout_ms` copy on this path; `start_s` has both at `:1248-1250`). | Mirror `start_s`'s initialisation and arm the deadline on the pool-hit path. |
| M8 | `src/protocols/http/http_redirect.c:103` `should_drop_header` | **Credentials follow a cross-origin redirect.** Only `Authorization` is stripped; caller-supplied `Cookie` and `Proxy-Authorization` go to whatever host a 3xx names. | **R.** | Also drop `Cookie` and `Proxy-Authorization` when `cross_origin`. |
| M9 | `src/protocols/http/http_client_async.c:915`; `http_client_sync.c:526-532` | **A truncated response is reported as success.** `status` is set in `on_status` but headers and body move into `resp` only at message-complete, and `llhttp_finish` is never called; EOF partway through headers or a Content-Length body returns success with no headers. | **C.** | Add a `finish` step to the response-parser vtable; at EOF succeed only if the parser reports complete. |

### Low

| # | File:Line | Issue | Evidence |
|---|---|---|---|
| L1 | `dns_resolver.c:1136` `dns_on_recv` | A duplicated TC=1 UDP answer, arriving after TCP recovery started, falls through and settles the leg (usually empty), so the real TCP answer is dropped; a later TCP failure can re-settle the done leg. Fix: ignore UDP for a leg once `tcp_pending`. | R |
| L2 | `http_conn_internal.h:100-103`, `http_connection.c` | `request_body_received`, `request_body_complete`, `drain_framing_usable` are never reset (only `c->req` is zeroed at `:142,845,944`), so a later request on the same pool slot can skip the #278 drain and RST. Fix: zero them on acquire and keep-alive reset. | R |
| L3 | `event_iocp.c:1211` | A successful zero-length datagram send completes with `bytes == 0`, is reported `ok = 0`, and poisons the send path; io_uring/pollcomp report `res >= 0`. | C |
| L4 | `event_iouring.c` re-prep paths (`iou_prep_send_tail`, `iou_prep_splice_*`), `iou_comp_cancel*` | Residual of the previous pass's stranded-op item: a re-prep or cancel that gets no SQE marks the op aborted with nothing queued, so it never completes. Only reachable if `io_uring_submit` cannot free a slot. | C |
| L5 | `thread_pool.c:182,194,282` | After a partial worker start the thread array is freed with the started count, not the allocated count (wrong size to a sized `KlAllocator.free`). | C |
| L6 | `platform_wakeup_win.c:32-39` | The loopback wakeup pair accepts whoever connects first; a local racer could take the `rd` end. Fix: verify the accepted peer against the client's local address. | C |
| L7 | `pipe_stream.c:561-566` via `listener.c:150-154` | A `kl_pipe_listen` whose first arm fails fires the owner's `on_close` for a listener it never received, then returns an error. | C |
| L8 | `http1_response_parser_llhttp.c`; `http_client_async.c:927` | Response framing: a 1xx completes as the final response; a HEAD response with Content-Length waits for a body; bytes after a complete response are dropped yet the connection is pooled. | C |
| L9 | `http_client_async.c` pooled path | Pooled requests ignore `cfg->proxy` (always direct). | C |
| L10 | `http_client_common.c:280-281`; `kl_http_client_remove_header`; `websocket_client.c` `wsc_build_upgrade` | Allocator size mismatches on three free paths. | C |
| L11 | `http2_client.c:210,248` | No response-size cap on HTTP/2 client data; an `RST_STREAM` is delivered as a normal response. | C |
| L12 | `websocket_client.c` | Masked server frames accepted; close code/reason unvalidated; handshake buffer uncapped; `on_close` runs before the connection is closed (freeing in it is a UAF). | C |

### Informational

- I1: pipelined HTTP/1 requests in the same read are discarded (`conn_keepalive_reset` sets `read_len = 0`); safe, but the client waits for the read timeout. Decide and document. (C)
- I2: a `max_header_size` below the 8192-byte base read buffer is not enforced (the 431 check runs only when the buffer is full). (C)
- I3: `event_iocp.c:766-769` treats `ERROR_MORE_DATA` as "no packet queued"; correct for byte read mode, which every Keel pipe uses, but a message-mode pipe would get two completions. Hardening only. (C)
- I4: WSABUF lengths are cast to `ULONG` without a clamp (`event_iocp.c:468,1145`, `socket_winsock.c:250`); not reachable at 4 GiB from the response writer. (C)
- I5: `pipe_stream.c` `rd_write` returns -1 after a short write when the watcher cannot be registered (ENOMEM); the stream is failed either way. (C)
- I6: the PAL thread trampolines allocate with `malloc`/`free` directly, contrary to the fourteenth pass's "none outside the default-allocator seam"; deliberate and commented. (R)
- I7: the fourteenth pass's L4 (`h2c_on_response` leak on a second call) is still open. (C)

### Checked clean (highlights)

- **New transport and pipe code:** `KlStream` facets (arm trampoline, `len > read_cap` fail-closed, writable edge with `in_writable` deferral, sticky `wq_err` releasing graceful close, exactly-once detach); `pipe_stream.c` `KlCompLife` retain/release on every post, failed post and dispatch, final release after every op, readiness mode holding a ref across callbacks and removing the watcher before the fd closes; `platform_pipe_posix.c` (`pipe2`/`FD_CLOEXEC`, non-blocking parent end only, per-thread SIGPIPE handling that leaves a pending SIGPIPE alone, fd+1 encoding); `platform_pipe_win.c` (bounded path conversion, length-checked SDDL, identification SQOS, anonymous pair client-PID check, every handle closed on failure); `listener.c`; `drain.c`; `event_ctx.c` watcher deletion during dispatch.
- **Named-pipe ops on IOCP:** exactly one completion per issue, life-token transfer, quiesce frees without re-issue, byte counts clamped.
- **DNS parser:** compression pointers skipped, never followed; label cap; `size_t` bounds on every field; exact 0x20 question echo; EDNS0/cookie option walk bounded; client-cookie mismatch rejected before learning; BADCOOKIE retries bounded; TCP framing capped at 65537 bytes.
- **Datagram layer:** cmsg length checks, overflow-safe control buffers, GRO split clamps, batch unwind, teardown deferral.
- **HTTP parsing limits:** `KL_MAX_HEADERS`, header-buffer growth capped with a `SIZE_MAX/2` guard, Content-Length zeroed under chunked, early 413, bounded drain; PROXY header peek bounded.
- **Clients:** WebSocket frame length/opcode/control limits and unmask offset; CONNECT build and status parse; HTTP/2 server header storage and `max_body_size`; Happy Eyeballs loser/straggler teardown (apart from H1); pool stale-connection peek.

### Coverage note

Read-only static audit on a Windows host, plus two local reproductions for C1 (a parser-level probe
and an end-to-end `KlHttpServer` run on WSAPoll and IOCP). Linux, macOS, io_uring and pollcomp
execution evidence is CI's. Slices not re-derived in depth because they are unchanged since the
fourteenth pass: `timer.c`, `connect_op.c` (apart from H1's path), `completion_core/dispatch.c`,
`event_poll.c`, `event_wsapoll.c`, `sockaddr.c`, `unix_socket_node_*.c`. No code was changed.

### Recommended order

1. **C1** first: it breaks ordinary HTTP/1 traffic on every engine and carries an out-of-bounds length.
2. **H1-H3**: a use-after-free reachable by following the shipped example, TLS failing open on an immediate connect, and WebSocket messages larger than one read.
3. The Mediums, grouped: DNS/resolver lifetime (M1, M2), IOCP watcher lifecycle (M3, M4), close-on-exec for sockets (M5, which matters to Hull), wakeup back-pressure (M6), client request hygiene (M7-M9).
4. Each fix with a test that fails first, per this repo's practice.

## Fourteenth pass: AF_UNIX node-cleanup security increment (#250/#251) + whole-tree re-audit (2026-08-26)

**Scope:** whole `src/` (93 `.c`) + `src/protocols/` + `integrations/` (67 `.c`) + headers +
`Makefile`. Trigger: the AF_UNIX node-lifecycle hardening merged at `4a5ac7e` (PR #251, issue #250 /
CodeQL #43): `src/unix_socket_node_posix.c` + the identity-anchored `src/unix_socket_node_win.c`. A
fresh six-subsystem sweep re-audited the rest of the tree (substrate core, datagram, event/completion
backends, HTTP server core + body/parse, client/http2/websocket/dns, integrations).

**Verdict: CLEAN.** 0 Critical / 0 High / 0 Medium / 4 Low + 2 Informational. The build is
`-Werror`-clean (0 warnings under `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2`), `cppcheck` exits 0,
`scan-build` reports no bugs (verified on `unix_socket_node_posix.c` this pass), and the full suite is
ASan/UBSan-clean (`make debug-test`, 90 suites, 0 sanitizer hits). None of the Low/Info items is a
reachable memory-safety defect; the security-critical node-cleanup modules are clean.

### Mechanical scans (whole tree)

| Category | Result |
|---|---|
| Unsafe str/format (`strcpy`/`strcat`/`sprintf`/`gets`/`vsprintf`) | none in `src/`; grep hits are the word "gets" in comments. One freestanding `strcpy` *definition* in `integrations/platform/uefi/mbedtls_platform_uefi.c` (a libc shim, bounded, expected) |
| Unsafe int parse (`atoi`/`atol`/`atof`) | none |
| Direct `malloc`/`free`/`calloc`/`realloc` in `src/` | none outside the default-allocator seam (`allocator_default_stdlib.c`); all app allocation via `kl_malloc`/`kl_free` (with size) |
| Overflow guards (`SIZE_MAX`/`INT_MAX` before size arithmetic) | present across substrate, datagram slots/batch, HTTP header/body growth, websocket/DNS/proxy length fields |
| Dead code (`#if 0` / `if (0)`) | none |
| VLAs / large stack arrays | none |

### Findings

| # | Severity | file:line | Issue | Suggested fix |
|---|----------|-----------|-------|---------------|
| L1 | Low (security: mTLS authz) | `integrations/tls/mbedtls/tls_mbedtls.c:338-373` (`x509_extract_cn`/`x509_extract_san`) | The mbedTLS adapter copies raw ASN.1 CommonName / DNS-SAN bytes verbatim with no canonicalization. An embedded NUL in a client-cert CN (`"admin\0evil"`) yields `subject_cn = "admin\0evil"`, so an app doing `strcmp(subject_cn, "admin")` for authorization matches. The OpenSSL sibling explicitly guards this (`identity_bytes_safe` rejects `<0x20`/`0x7f`; `dns_san_bytes_safe` allows only LDH+`.`/`_`/`*`, rejecting NUL/control and the `,` that would make the joined SAN list ambiguous). Bounds are correct: this is an identity-spoofing asymmetry between the two TLS backends, not memory unsafety. | Mirror the OpenSSL policy in the mbedTLS extractor: reject/skip a CN or DNS-SAN whose bytes contain NUL, a C0 control, DEL, or (SAN) a comma. |
| L2 | Low (liveness) | `src/event_iouring.c:476` (also 502, 513; callers 545/578/617) | On submission-queue exhaustion at the *initial* post, `iou_prep_send_tail` sets `op->aborted=1` and returns, but no SQE was queued and the post function still returns 0 (success). The drain (`io_uring_for_each_cqe`) only processes CQEs and `->aborted` is consumed only inside `iou_fill_event` when a CQE is dequeued, so no completion is ever delivered; the idle-timeout sweep skips already-aborted ops, so the WRITE/SENDFILE is stranded until the connection idle-closes. Not a UAF (kernel never owned the buffer). Requires SQ-depth (1024) exhaustion, so hard to trigger. The "surface as error next drain" comment is aspirational; there is no aborted-op sweep. | Treat an initial `!sqe` as a hard post failure: unlink + `iou_op_free` the op and return -1 (as the other `!sqe` post paths do), so the driver closes the connection instead of waiting on a completion that never arrives. |
| L3 | Low (integrity) | `integrations/codec/miniz/decompress_miniz.c:338-367` | The streaming gzip CRC32 + ISIZE verification runs only in the `flush && trail_len>=8` branch. A stream whose final block plus full 8-byte trailer arrives without a subsequent `flush`, or that ends with `<8` trailer bytes, is accepted with no integrity check; corrupted payload passes as valid. Contained by the 256 MB anti-bomb cap; ISIZE compares 32-bit `total_out` (wraps >4 GB). Not memory-unsafe. | Verify the trailer whenever `done` and 8 bytes are accumulated (independent of `flush`); treat a DONE stream with `<8` trailer bytes at flush as an error. |
| L4 | Low (not attacker-reachable) | `src/protocols/http2/http2_client.c:175-207` (`h2c_on_response`) | If invoked twice for one stream while `n <= headers_cap`, the realloc block is skipped and the previous `name`/`value` heap strings are overwritten in place without `kl_free` (leak). Only reachable if a peer delivers two `HCAT_RESPONSE` HEADERS frames on one stream, which nghttp2 rejects as a protocol error and RSTs before a second delivery. | Hardening: free existing `st->resp.headers[*]` and reset `num_headers` at the top of the copy block before re-filling. |
| I1 | Info | `integrations/platform/uefi/mbedtls_platform_uefi.c:234-266` | The freestanding `snprintf` skips `l`/`z`/`h` length modifiers but always `va_arg`s a 32-bit `int`/`unsigned` for `%x`/`%u`/`%d`. A `%zx`/`%lx` with a 64-bit arg would read the wrong vararg slot and desync the rest. Latent only: current mbedTLS call sites (ERROR_C/DEBUG_C off) use 32-bit conversions. | Honor the length modifier if a 64-bit/`z` conversion is ever introduced; otherwise acceptable under the documented "not a general printf" contract. |
| I2 | Info | `integrations/http2/nghttp2/http2_nghttp2_server.c:147` (`ng_on_header_cb`) | On `ng_sstream_grow` realloc failure mid-headers the callback returns 0, so the request is later delivered missing that header (graceful degradation). An app that authorizes/routes on the full header set could be bypassed by a truncated set. Not memory-unsafe. | Return `NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE` to RST the stream on allocation failure. |

**Update (same session, 2026-08-26): L1 FIXED.** `integrations/tls/mbedtls/tls_mbedtls.c` now mirrors
the OpenSSL backend's identity canonicalization: added `identity_bytes_safe` (rejects `<0x20`/`0x7f`)
and `dns_san_bytes_safe` (LDH+`.`/`_`/`*` only, rejecting NUL/control/comma); `x509_extract_cn` fails
closed on an invalid-byte CN, a would-truncate CN, or more than one CN attribute; the DNS-SAN branch
skips an invalid-byte or over-long entry instead of truncating. The adapter compiles clean under
`-Werror` and the mbedTLS `e2e` peer-cert suite passes (valid-cert extraction unregressed). The other
Low/Info items (L2/L3/L4, I1/I2) remain as recorded.

### Coverage note

Six independent subsystem sweeps read the real code (not pattern matching) across substrate core
(allocator/kl_cstr/error/drain/timer/thread_pool/url/sockaddr/file_io/resolve_sync/stream/listener/
connect_op/completion_core/completion_dispatch/event_ctx/event_dispatch), the datagram data plane
(`datagram*.c`, `udp_cmsg.c`, `socket_dgram_posix.c`), all event/completion + socket backends
(epoll/kqueue/poll/WSAPoll/io_uring/IOCP/pollcomp + posix/winsock providers), the HTTP server core +
untrusted-input body/parse path (connection/server/response/router/body readers/chunked/multipart/
cors/sse/compress/async), the client + secondary protocols (http_client_*/http2/websocket/dns/
proxy_protocol/decompress/resolver_cache), and the `integrations/` adapter glue (lwIP raw + EFI
TCP4/UDP4 token/quarantine lifetime, TLS mbedtls/openssl, nghttp2, miniz, UEFI freestanding shims).
Verified clean at scrutiny: the substrate size/index overflow guards and the synchronous-inline-
completion re-entrancy discipline (set-flag-before-hook + depth counter + deferred finalize) in
stream/listener/connect_op; the datagram life-token refcount (created at 1, retain-per-post,
release-exactly-once per completion, QUARANTINE borrowed-ref intentional leak) and fixed-slot/ring/
batch overflow guards; the completion-backend op/buffer ownership (io_uring queue_exit-before-free,
IOCP deliberate leak-on-port-error to avoid UAF, token transfer-and-NULL discipline); CL/TE
resolution (llhttp zeroes Content-Length when chunked), the chunked 16-hex-digit overflow guard and
multipart underflow-safe loop guards, response-header CRLF-injection rejection, and the Happy-Eyeballs
terminal-once teardown; the DNS/WebSocket/PROXY length-field bounds and decompression output cap; and
the two highest-risk integration lifetime domains (lwIP pcb/pbuf/token, EFI generation+quarantine).

The two security-critical modules from the merged increment were covered by the concurrent #250/#251
review this session rather than re-audited redundantly: `unix_socket_node_posix.c` (component-walked
`O_NOFOLLOW`/`O_DIRECTORY` trust validation on every path component, dev/ino identity captured at bind
and re-verified at teardown, `out_fd`-required NULL guard) is `scan-build`-clean; `unix_socket_node_win.c`
(per-component no-follow walk rejecting reparse points, ACL default-deny that skips inherit-only ACEs,
held/pinned parent, `FILE_ID_INFO`-verified delete-by-handle via `FileDispositionInfoEx` only) is
`cppcheck`-clean and contains no `DeleteFileA`/pathname-deletion path. Both fail closed on an untrusted
component or an identity mismatch.

## Thirteenth pass: datagram Phase B (public `KlDatagram` + `KlUdp`/dns) whole-tree re-audit (2026-08-17)

**Scope:** whole `src/` (69 `.c`) + `parsers/` + headers + `Makefile`, focused on the code added
since the twelfth pass; the datagram consolidation arc (`src/udp.c`, `src/socket_dgram_posix.c`,
`src/datagram.c`, `src/datagram_core.*`, `src/completion_core.c`, `src/dns_resolver.c`,
`src/udp_server.c`, `integrations/uefi/event_efi.c`) plus the IOCP association-at-create fix.

**Verdict: CLEAN.** 0 Critical / 0 High / 0 Medium / 1 Low (informational). The build is
`-Werror`-clean (**0 warnings** under `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2`), `cppcheck`
exits 0, and the completion axis is ASan/UBSan/LSan-clean natively (`smoke-pollcomp-asan`) and in
the container (`smoke-iouring-asan`).

### Mechanical scans (whole tree)

| Category | Result |
|---|---|
| Unsafe str/format (`strcpy`/`strcat`/`sprintf`/`gets`/`vsprintf`) | **none** (grep hits are the word "gets" in comments) |
| Unsafe int parse (`atoi`/`atol`/`atof`/`atoll`) | **none** |
| Direct `malloc`/`free`/`calloc`/`realloc` | **none**: all via `kl_malloc`/`kl_free` (with size) |
| Unchecked `kl_malloc` (spot-checked incl. dgram batch, websocket, decompress) | all NULL-checked; multi-alloc uses batch-then-check + free-on-error |
| Overflow guards (`SIZE_MAX`/`INT_MAX` before size arithmetic) | present in 39 `.c` files |
| Dead code (`#if 0` / `if (0)`) | **none** |
| VLAs / large stack arrays | **none** |
| SIGPIPE | ignored at server init; `SO_NOSIGPIPE` + `MSG_NOSIGNAL` (POSIX); no-op on Windows |
| `-Werror` build warnings | **0** |

### Automated safety net (cross-referenced, not re-derived)

83 test suites · 8 fuzz targets (parser/multipart/websocket/response/dns/proxy/url + decompress)
· `make analyze` (scan-build) · `make cppcheck` (**rc=0**) · `make debug` ASan+UBSan · the
completion-axis smokes above.

### Low (informational)

| # | File:line | Note |
|---|-----------|------|
| L1 | `src/socket_dgram_posix.c:500-504` | `pdg_rx_batch_new` computes `(size_t)n * sizeof(...)` for the `recvmmsg` batch without an explicit overflow guard on `n`. Not exploitable: `n` is the app-set `mmsg_batch` config knob (not network input) and is 64-bit-safe; on 32-bit a pathological value could overflow. Defense-in-depth: cap `mmsg_batch` or guard the multiply. |

**Conclusion:** no actionable memory-safety, resource, or hardening issues in the datagram arc or
the wider tree; the sole Low item is a config-controlled, 64-bit-safe robustness nit. No fixes
applied (audit-only pass).

---

## Twelfth pass: Phase 10 UEFI server (S-1..S-7) + client/server orchestration refactors (2026-08-08)

**Scope:** the code added/changed since the eleventh pass; the freestanding UEFI HTTP(S)
**server** (S-1..S-7) and the two review-driven orchestration refactors: `src/server_core.c`
(freestanding `kl_server_init`/`kl_server_free` carves), `src/client_proxy.c` (shared proxy
CONNECT), `src/server_activation.c` (systemd socket-activation split), the readiness→hook
dispatch unification (`src/server.c`, `src/server_ws.c`, `src/server_h2.c`, `src/proto_hooks.h`),
and the EFI provider data-plane (`integrations/uefi/event_efi.c`, `socket_efi_tcp4.c`).

**Verdict: CLEAN.** No new Critical/High/Medium findings. This pass is corroborated by the
strongest available checks rather than inspection alone:

| Check | Result |
|-------|--------|
| Unsafe fns (`strcpy`/`strcat`/`sprintf`/`gets`/`atoi`/`atof`) in `src/` + `parsers/` | none |
| Direct `malloc`/`free`/`realloc` in `src/` (allocator discipline) | none; all via `kl_*` |
| `kl_malloc` NULL-check discipline (new code) | the one new call site (server_core.c proxy_cidrs) is checked; client_proxy.c / server_activation.c are alloc-free |
| Full test suite under **ASan + UBSan** (`make debug-test`) | **65/65 suites, 0 leaks, 0 UB, 0 errors** |
| mock-EFI failure-path harness (ASan/UBSan) | PASS incl. the new `t_accept_backpressure` |
| Build hardening (prod CFLAGS) | `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 -fstack-protector-strong` |
| Dead code (`-Wunused` via `-Werror`) | clean build |
| Cross-compile: gcc-14 (all touched TUs) + MinGW (server_activation/server_plat_win/server.c) | clean |
| Freestanding archives (client + server) symbol-closure gate, x86_64 + aarch64 | gated OK |

**Bounds/injection spot-checks (session's new attack surface):**
- EFI server send copies into a fixed inline `sndbuf` with a `total > KL_EFI_SNDBUF` guard
  **and** a per-iovec `off + iov[i].len > KL_EFI_SNDBUF` guard (fail-closed → conn closed).
- EFI server recv is bounded by `read_cap - read_len`; the TLS-recv scratch is a fixed
  `KL_EFI_TLS_CIPHER` buffer consumed by `feed_input` each drain.
- Response header CRLF-injection guard (`contains_crlf`) unchanged in `response.c`.
- Accept backpressure bounds armed EFI tokens to free Keel pool slots (no accept-into-drop).

**Informational (not a finding):** `integrations/uefi/mbedtls_platform_uefi.c` *defines*
`strcpy` (and other libc primitives), it is the freestanding libc backing the vendored
mbedTLS build needs on bare firmware, not an unsafe *call* on untrusted input in KEEL code.
A grep for unsafe-function *names* flags the definition; it is benign by construction.

**Prior findings:** all eleventh-pass EFI lifetime bugs remain fixed (verified live on the
branch, commit `71573dd`; see the axis-audit tenth pass and the S-7 review round).

---

## Eleventh pass: Phase 10 UEFI provider failure-path hardening (2026-08-06)

**Scope:** the EFI network provider under `integrations/uefi/` + `spikes/uefi/` (F-8): the
EFI_TCP4 socket provider (`socket_efi_tcp4.c`), the built-in DNS resolver over EFI_UDP4
(`dns_uefi.c`), the completion backend (`event_efi.c`), the mbedTLS platform backing
(`mbedtls_platform_uefi.c`), the lifecycle teardown (`lifecycle_uefi.c`), and the U-4/U-7
self-tests.

**This is NOT a "clean" pass.** Two rounds of adversarial *lifetime* review found **real,
release-blocking EFI completion-token bugs** that QEMU happy-path testing (and the happy-path-focused
earlier passes) never exercised. Every finding below was FOUND, FIXED, and (the load-bearing part)
**covered by a host mock-EFI harness** (`mock_efi_test.c`, 18 scenarios) that compiles the *real*
provider TUs against a scriptable fake `EFI_BOOT_SERVICES`/`EFI_TCP4`/`EFI_UDP4` under ASan+UBSan.
The lesson recorded: **a QEMU happy-path GO ≠ correct**; completion-token providers demand
adversarial timeout/close/cancel/post-EBS host tests before any "production" claim.

**Method.** Host mock-EFI harness (18 scenarios, ASan+UBSan+LSan (`detect_leaks=1` on Linux; `=0` on Darwin)), the deliberate
quarantine "leak" is clean because backing storage is the static `g_conns`/`g_dns_*` pools
(reachable, not leaked). All touched freestanding TUs cross-compiled `--target=x86_64-unknown-windows
-ffreestanding -Wall -Wextra -Werror`. Core `make test` green. QEMU U-7 (plaintext send/recv/close
over real EFI_TCP4) re-run as a happy-path regression guard.

### Findings (all fixed + test-covered)

| # | Severity | File:symbol | Bug | Fix | Test |
|---|----------|-------------|-----|-----|------|
| U1 | **Critical** | `socket_efi_tcp4.c` `efi_sock_send`/`efi_sock_close`; `dns_uefi.c` `kl_uefi_dns_resolve` | A **failed** `Cancel`+drain (token never reaches a terminal state) was still followed by ordinary cleanup (`CloseEvent`/`DestroyChild`/`kl_free`) releasing storage the firmware may still write into (UAF / firmware-write-after-free). | **Quarantine model.** On a drain that does not confirm the token retired, set `c->quarantined` (DNS: `g_dns_quarantined`): never `CloseEvent`/`DestroyChild`/reclaim the slot; its **stable, provider-owned** storage (`static KlUefiConn g_conns[]`; DNS `g_dns_op`) is deliberately leaked until ExitBootServices. DNS fails closed on every subsequent `resolve`. | `t_cancel_fails_quarantine`, `t_dns_cancel_fails_quarantine` (child NOT destroyed; slot not reused; resolve fails-closed) |
| U1b | **Critical** | `dns_uefi.c` `kl_uefi_dns_resolve` (2nd review) | The DNS quarantine preserved the query buffer + descriptor but the **completion tokens themselves** (`tx_tok`/`rx_tok`) were still **stack-local**: on `goto quarantine` the frame died while firmware held their addresses (and would later write `Status`/`Packet.RxData` into freed stack). | Encapsulate the entire one-shot DNS op (tokens + descriptor + query buffer) in **one stable file-scope struct** `g_dns_op`. Invariant: nothing reachable from a submitted token lives on the stack. | `t_dns_cancel_fails_quarantine` extended to **model a delayed firmware write into the quarantined token after `dns_resolve()` returns**; negative control (tokens forced back to stack) reproduces the ASan **stack-use-after-return**, confirming the test bites |
| U2 | **High** | `socket_efi_tcp4.c` `efi_sock_close` | Close used `configured && !connected` as a proxy for "connect token outstanding"; because `CheckEvent` **consumes** the signal, close could wait ~60 s on an already-consumed event. | **Explicit token-state**: `conn_posted`/`tx_posted`/`close_posted` (+ existing `rx_posted`), set only after a successful submit, cleared only after the terminal event is observed. Close drains **only** flagged tokens. | `t_close_no_spin_on_consumed_connect` (`g_tcp_poll_calls` bounded; the mock models `CheckEvent` de-signalling) |
| U3 | **High** | `socket_efi_tcp4.c` `efi_sock_close` | Close ignored failed receive/connect cancellation drains (same root as U1 for the non-tx tokens). | Close tracks `drained_ok` across every posted token; `!drained_ok` → quarantine + early return (no teardown). | covered by U1 close paths + `receive pending during close` |
| U4 | **High** | `socket_efi_tcp4.c` `kl_uefi_socket_recv_ready` | Received `rx_data.DataLength` (and `FragmentTable[0].FragmentLength`) trusted without validating `<= KL_EFI_RXBUF` → OOB read on a malicious/buggy stack. | Validate both against `KL_EFI_RXBUF`; on violation latch `rx_err = EFI_DEVICE_ERROR` and surface a fatal `recv` -1. | `t_bad_rx_length` (recv -1, no OOB under ASan) |
| U5 | **Medium** | `socket_efi_tcp4.c` `kl_uefi_socket_configure`/`_connect_post`; `mbedtls_platform_uefi.c` heap | Post-EBS coverage incomplete: two data-path entries unguarded; mbedTLS heap retained the Boot Services pointer with no shutdown hook (TLS teardown after EBS could `FreePool` torn-down firmware). | `kl_uefi_after_ebs()` guard on the two entries; `uefi_mbed_calloc`/`_free` fail closed post-EBS; new `kl_uefi_mbedtls_platform_shutdown()` drops `g_bs`; U-4 calls it after destroying every TLS object, before EBS. | `calls after simulated EBS` (zero firmware calls across all entries) |
| U6 | **Medium** | `socket_efi_tcp4.c` `kl_uefi_socket_provider_reset` | Reset assumed all conns closed and blanket-zeroed the pool; corrupting quarantined/live firmware-owned slots. | Reset no longer zeroes the pool (preserves quarantined + live slots; only already-free slots are reused). New `kl_uefi_socket_provider_live_count()` exposes the "all-closed-before-shutdown" contract; U-7 asserts it observably. | `stale-guard no-UAF` + U-7 live-count line |
| U7 | **Medium** (load-bearing) | `mock_efi_test.c` (new) | The adversarial host harness itself was missing; the earlier passes could not have caught U1–U6. | Added the 18-scenario mock-EFI harness (cancel succeeds/fails/races, consumed connect event during close, receive outstanding during close, impossible receive length, post-EBS, slot reuse with stale generation, delayed firmware write into a quarantined DNS token). | itself |
| U9 | **Medium** | `build_mock_efi_test.sh` / `mbedtls_platform_uefi.c` (2nd review) | The advertised 13th (entropy fail-closed) test was gated behind `MOCK_WITH_MBEDTLS`, which the build never set; the harness silently ran 12, and the entropy path was "verified by inspection" only. Also: script not executable (0644); `detect_leaks=1` aborts under Apple ASan on Darwin. | Split `mbedtls_hardware_poll` into a mbedTLS-free `entropy_uefi.c` so the harness links + runs the fail-closed test **unconditionally** (genuinely 13). `chmod +x`; select `detect_leaks=0` on Darwin (Linux container keeps the real leak check). | `t_entropy_fail_closed` (now always runs: `r!=0, olen=0` without the insecure macro) |
| U8 | Low (tooling) | `build_u{3,4,5,7}.sh` | `declare -A` fails silently on macOS Bash 3. | Explicit `BASH_VERSINFO < 4` guard with a clear error. | n/a |
| U10 | **Medium** | `mbedtls_config_uefi.h`; new `civil_time`, `wallclock_uefi`, `clock_snapshot`, `time_uefi`; `platform_uefi.c`; `spikes/uefi/efi_min.h` | Certificate **validity-time was not enforced** (`MBEDTLS_HAVE_TIME` off), TLS accepted expired / not-yet-valid certs (CA + hostname only). | Enable `HAVE_TIME`/`HAVE_TIME_DATE`; bind mbedTLS's clock to Runtime Services **GetTime** → pure `civil_time` math. Trust boundary (3rd–5th review): **one TLS-platform-lifetime SNAPSHOT** (`clock_snapshot.c`) captured once **inside `kl_uefi_mbedtls_platform_init()`** (mbedTLS's time callback is global, no session context, so per-session would let concurrent sessions clobber each other's basis), shared by all sessions + advanced by the monotonic clock, cleared at platform shutdown; `mbedtls_time` never calls GetTime mid-handshake (closes the "GetTime fails after setup → epoch 0 → 1970-cert" loophole); **structural fail-closed gate in the mandatory initializer** (no app choreography), untrustworthy clock ⇒ init fails ⇒ no `KlTlsCtx`; **`EFI_TIME` field validation** rejects malformed firmware pre-conversion; **unspecified-TZ rejected by default** (UEFI §8.3 local-time; app offset or `KL_UEFI_ASSUME_UNSPECIFIED_UTC` test flag to opt in), sign `UTC = local + TimeZone`. | `t_civil_time` + `t_wallclock_decode` (unspecified-TZ policy, malformed fields) + `t_clock_snapshot` (snapshot read-not-GetTime; **post-snapshot GetTime failure does not reopen epoch-0**; failed refresh → epoch 0). + t_clock_snapshot platform-not-ready case (frozen monotonic timer → snapshot refused → TLS init fails). **QEMU U-4 prod: valid+good-clock → 200; valid+bad-clock (RTC 2000) → TLS refused, no 200; expired (2020) → `status -1`/`KL_ERR_TLS_HANDSHAKE`/no 200.** |

**No open findings from this pass.** Certificate validation is now complete (CA chain + hostname +
validity-time); the previously-documented cert-time gap (U10) is closed. Remaining items are
operational (real-firmware RTC quality), not missing checks.

> **Retirement note (2026-08):** `dns_uefi.c` (the U1/U1b findings above, and the
> `t_dns_cancel_fails_quarantine` test) has since been **retired**: the stock async
> `src/dns_resolver.c` now runs over the EFI_UDP4 datagram provider (`socket_efi_udp4.c`, 6.4b/6.4c).
> The cancel / quarantine / stable-token disciplines the U1/U1b findings hardened now live in
> `socket_efi_udp4.c`, whose 6.4b mock cases cover them; `t_dns_cancel_fails_quarantine` was removed
> with the retired engine. The findings above are preserved as a point-in-time record.

**Status:** the happy path is proven end-to-end in QEMU; the failure paths are hardened and
host-test-covered under ASan+UBSan; cert-time enforcement is proven with a valid/expired QEMU pair.
The two originally-Critical token-lifetime findings (and the 2nd-review DNS-token + entropy-test
gaps) are closed.

## Tenth pass: freestanding portability phase (2026-08-05)

**Scope:** the freestanding phase merged since the ninth pass (PRs #199–#210): the allocator split
(A1), `completion.h`→`KlSockAddr` (A2), freestanding public headers + `kl_ssize_t`/off_t (A3/F0),
`errno`→`KlIoStatus` (B1) + sync-proxy neutrality, the `completion_driver.c`→core/server/h2/ws and
`client.c`→common/sync/async **TU splits** (B2a/B2b) behind `KlEventCtx` hooks + `src/event_ctx.c`,
F-4 formatted-I/O elimination (`src/kl_cstr.c`) + the optional reference mem*/strlen
(`src/kl_cstr_builtin.c`), and the multi-arch + PE-link gates (B1+B2). The lwip-raw accept-window
UAF (M1, #208) was the ninth pass's M1; now **fixed**.

**Method:** the TU splits are pure code movement (nm-proven client_async.o/completion_core.o have
no cross-stack deps; full suite + harness green), not re-audited. A focused reviewer took the
genuinely-NEW hand-written logic; `kl_cstr.c` (bounded parsers/formatters on **untrusted** URL/
response input), `kl_cstr_builtin.c` (reference mem*), `event_ctx.c` (the split watcher API), plus
mechanical sweeps and the automated gate in the Apple container.

**Automated tools + mechanical: CLEAN.** cppcheck **0 err/warn**; scan-build **"No bugs found"**;
**64 suites** under ASan+UBSan (incl the new `test_kl_cstr` 6/6 + `test_kl_cstr_builtin` 5/5); the
url parser (which now routes through `kl_strchr`/`kl_strstr`/`kl_parse_u16_decimal`) is fuzzed; no
unsafe libc funcs, no raw malloc/free, no VLAs in the new TUs; hardening unchanged.

### Findings

| # | File:Line | Sev | Issue | Fix |
|---|-----------|-----|-------|-----|
| L1 | `include/keel/client.h:42` | Low (cosmetic) | `KL_CLIENT_CHUNK_HDR_SIZE 16` commented "Fits `FFFFFFFFFFFFFFFF\r\n`"; that line is 18B+NUL, doesn't fit 16. **No overflow**: `chunk_buf`=4096 → real chunk sizes ≤3 hex digits, and `kl_buf_append_hex` is bounded (returns -1→error) even adversarially. **Fixed**: bumped to 24 + accurate comment. |

### Verdict (verified clean)
- **`kl_cstr.c`**: the append builders never compute `off+len` directly (guard order `o>cap` →
  `len>cap-o`), so no additive overflow and no NUL off-by-one; `kl_u64_to_dec/hex` bound the
  `tmp[20]`/`tmp[16]` reversal + reject `n>cap`; `kl_parse_u16_decimal` rejects at the exact u16
  boundary before any uint32 overflow; `kl_strstr`/`kl_strchr`/`kl_streq`/`kl_str_startswith` match
  libc (empty needle, NUL, short strings). Every caller (sockaddr/url/client_common/client_async)
  passes `sizeof(buf)` as cap and aborts on append failure; an adversarial oversized host/path/
  header yields a clean failure, not an overflow. `kl_u64_to_*` don't NUL-terminate; the one raw
  caller (`sockaddr.c` v6 formatter) memcpys exactly `r` + terminates itself; all others go through
  `kl_buf_append_*` which write the NUL. **Clean.**
- **`kl_cstr_builtin.c`**: memmove both directions + `d==s`/`n==0` early-out; unsigned memcmp
  ordering; the self-referential-lowering foot-gun is disarmed (`-fno-builtin` base +
  `-fno-tree-loop-distribute-patterns` on the self-contained target, probe-gated); out of CORE_SRC
  so no duplicate-def on hosted/EDK2. **Clean.**
- **`event_ctx.c`**: the split preserved the watcher-list invariants + the **H1 liveness guard**
  (which lives in the `event_ctx.h` inline, so both `kl_event_ctx_run` and the completion driver
  see it); add-failure unlinks+frees; `kl_event_ctx_run` heap path SIZE_MAX-guarded. **Clean.**

Overall: **Low** risk; one cosmetic comment (fixed), no Critical/High/Medium. The new
freestanding logic is correctly bounded, overflow-guarded, and NUL-termination-correct.

---

## Ninth pass: lwIP-raw client axis (LC-0..LC-5) + full re-sweep (2026-08-05)

**Scope:** the completion-native lwIP-raw **client** work merged since the eighth pass;
LC-0 completion-CONNECT contract (`KL_COMP_CONNECT` + `post_connect` across `completion.h`/
`completion_dispatch.c`/`completion_driver.c`/`event_pollcomp.c`/`event_iouring.c`/
`event_iocp.c`/`src/client.c`/`src/async.c`), LC-1/LC-2 plaintext + Happy-Eyeballs raw client,
LC-3a `KlUdp` over lwip-raw, LC-3 DNS via KEEL's `dns_resolver.c` on `KlUdp`-over-raw, LC-4
HTTPS (client socket-BIO through the raw provider + server memory-BIO completion-TLS leg, two
backend fixes), LC-5 caps/docs (PRs #191–#197). Files centered on `integrations/lwip/`
(`event_lwip_raw.c`, `lwip_raw_glue.c/.h`, `lwipopts_raw.h`) + the cross-backend LC-0 seam.

**Method:** two parallel deep-review agents (the lwip-raw backend/glue lifetime + the LC-0
cross-backend connect primitive), each tracing recv/send/connect/close-with-outstanding paths;
plus mechanical sweeps (no `strcpy`/`sprintf`/`atoi`, no raw `malloc`/`free` outside test
harnesses) and the automated gate in the Apple container: **cppcheck 0 errors/warnings,
scan-build "No bugs found," 60 suites passing under ASan+UBSan** (`make debug-test`,
`detect_leaks=1`), and the loopback-raw + raw-tls ASan runs. Build hardening confirmed
unchanged (`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -O2 -fstack-protector-strong
-fPIE -D_FORTIFY_SOURCE=3`; SIGPIPE fully handled).

**Automated tools: CLEAN.** cppcheck (0), scan-build (No bugs), no unsafe libc calls in
`src/`+`parsers/` (only `allocator.c`'s stdlib wrapper legitimately wraps `realloc`), no VLAs,
allocator discipline intact in the new backend (all Keel-owned memory via `KlAllocator`, no
hot-path allocation; TX window pre-allocated, only lwIP's own `pbuf_alloc` for a UDP send).

### High

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| H1 | `include/keel/event_ctx.h:140` (`kl_event_dispatch`) + `src/completion_driver.c` (`kl_comp_run` batch) + `src/client.c` (`he_close_attempts`/`client_drop_connect_fd`) | **Same-batch Happy-Eyeballs connect use-after-free.** `kl_comp_run` drains multiple completions into one batch and dispatches them sequentially; `kl_event_dispatch` derefs the tagged `KlWatcher*` with **no liveness check**. When two racing connect attempts (dual-stack A+AAAA) both complete in one drain, dispatching the winner's `KL_COMP_CONNECT` runs `he_win → he_close_attempts → client_drop_connect_fd → kl_watcher_del`, which **synchronously frees the loser's watcher node**; dispatching the loser's still-batched `KL_COMP_CONNECT` then reads freed memory. The default `connect_attempt_delay_ms=250` shields the loopback happy path (only one attempt in flight), so tests pass; a genuinely-racing dual-stack connect (both in flight, both completing in one wait) hits it. Confirmed by direct trace on pollcomp + io_uring. **Fix:** in the `KL_COMP_CONNECT`/`KL_COMP_WATCHER` dispatch, validate the tagged watcher is still linked in `ctx->watchers` before deref (small, backend-agnostic guard); skip if it was freed earlier in the same batch. |

### Medium

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| M1 | `integrations/lwip/lwip_raw_glue.c:766-804` (`lwr_srv_accept`/`lwr_srv_err`) | **Dangling-pcb UAF in the accept→post_recv window.** `lwr_srv_err` keys on `c->owner == arg`, but `owner` is set only later (first `post_recv`), and `lwr_srv_accept` never `tcp_arg`s the new pcb (it inherits the listener's arg). If lwIP aborts the accepted pcb (peer RST / OOM) before the driver's `post_recv`, the err is dropped, the slot keeps `dead=0` + a dangling `->pcb`, and the pending ACCEPT still surfaces → the driver adopts a freed pcb (UAF). Narrow timing (needs a stack-initiated abort in that window; rare on loopback). **Fix:** `tcp_arg(newpcb, slot)` at accept and resolve `lwr_srv_err` by the slot pointer, so a pre-owner abort still marks its slot `dead`. |

### Low

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| L1 | `src/redirect.c:77` (`is_cross_origin`) | **UBSan: NULL passed to `strncasecmp` (declared nonnull).** When both URLs have `host_len==0`, the equal-length guard falls through to `strncasecmp(a->host, b->host, 0)` with NULL hosts; defined-behavior-pedantic UB (0 length, no actual read), but it trips `UBSAN_OPTIONS=halt_on_error=1`. CI stays green only because its UBSan job runs recover-mode (prints, doesn't fail). **Fix:** `if (a->host_len == 0) return 0;` (or guard `a->host && b->host`) before the `strncasecmp`. |
| L2 | `integrations/lwip/event_lwip_raw.c:672-677` | **False EOF vs explicit overflow on a full non-TLS `read_buf`.** When `space==0` with `rx_queued>0`, the drain surfaces `bytes=0` → the driver reads it as peer-close (EOF), where the readiness backend (`event_pollcomp.c:248`) deliberately signals a header-overflow failure. Converges to a close either way; cosmetic (wrong reason). **Fix:** surface a failed READ when `space==0`. |
| L3 | `src/completion_driver.c:768`, `event_pollcomp.c:358`, `event_iouring.c:695` | **Stale comments** claim the client re-reads `SO_ERROR` for `KL_COMP_CONNECT`; the actual path (`he_on_connect_result`, `client.c`) trusts the mask-carried win/fail and does **not** read `SO_ERROR` (correct for io_uring, which drops it). Comment-only. **Fix:** correct the comments to reference `he_on_connect_result` + the mask. |

### Verdict
The new code is disciplined on overflow, allocation, backpressure, and the neutral seam
(`event_lwip_raw.c` stays lwIP-free; all `tcp_*`/`udp_*` in the glue), and the automated tools
are clean. Two real use-after-frees (H1 same-batch HE connect race; M1 accept-window pcb) are
edge-timing bugs a happy-path sanitizer run misses; both warrant fixes; H1 is the priority as
it sits on the outbound connect path for any completion backend. L1 is a trivial UBSan fix. The
underlying feature (raw client + HE + DNS + HTTPS) is functionally sound and end-to-end verified.

---

## Eighth pass: datagram data-plane folded onto the socket provider (2026-08-03)

**Scope:** the four-stage "datagram provider" refactor (#168–#172) that resolves
axis-audit **A2**: the UDP datagram data-plane moves from the compile/link
`udp_io_*` seam onto an optional `KlDatagramOps` vtable on `KlSocketProvider`, so
one runtime provider owns stream **and** datagram I/O. Files: `include/keel/datagram.h`
(the new vtable + `KlDgramRxSlot`/`KlDgramTxDesc`/`KlDgramRxMeta`), `src/socket_dgram_posix.c`
+ `src/socket_dgram_win.c` (the POSIX/Winsock datagram primitives, ~1000 lines),
`src/udp_cmsg.c` + `src/udp_cmsg_win.c` (shared cmsg parsers the completion
backends reuse), `src/udp.c` (the send-queue flush + recv drain **machine loops
moved up here**, provider dispatch, `udp_group_ok` mcast validation, batch
lifecycle, dgram-required init), the `socket.h`/`socket_posix.c`/`socket_winsock.c`
provider wiring + `kl_sockdef_dgram()`, the completion backends' `.dgram`
inheritance, and `integrations/lwip/socket_lwip.c`'s datagram ops. `udp_io_posix.c`,
`udp_io_win.c`, `udp_io.h` were deleted.

**Method:** three parallel deep-review agents (POSIX dgram + udp_cmsg; Winsock dgram
+ udp_cmsg_win; udp.c machine rework + lwIP + provider wiring) tracing cmsg
build/parse bounds, mmsg-batch alloc/index/free symmetry, the data-oriented
recv_batch/send_batch slot/descriptor arrays, address marshalling on untrusted
recv addresses, callback re-entrancy in the machine loops, and the dgram-default
resolution; mechanical sweeps (no `strcpy`/`sprintf`/`atoi`, no raw `malloc`/`free`
in the new TUs); and the Stage-4 gate; **cppcheck + scan-build clean, the full unit
suite under ASan+UBSan (891 tests, 0 failures, 0 sanitizer hits)**, gcc-14 +
cosmocc + MinGW (iocp/wsapoll), and the Apple container: epoll (0 fails, UDP
batching 5/5 via real `recvmmsg`/`sendmmsg`), io_uring (`smoke-iouring-asan`
UDP-over-completion), and the lwIP loopback + HTTPS.

**Verdict: clean.** No Critical or High findings. The memory-safety surface of the
refactor (cmsg bounds, the mmsg batch blocks, the data-oriented slot/descriptor
arrays, address marshalling, and machine-loop callback re-entrancy) is sound. One
**Low** correctness item was **fixed this pass**; the rest are Low/Informational,
pre-existing or unreachable via the public API.

### Low: fixed this pass

| # | File(s) | Issue | Fix |
|---|---------|-------|-----|
| L1 | `src/socket_dgram_posix.c` / `src/socket_dgram_win.c` (`pdg_send`/`wdg_send`) | On a source-pinned/TOS send with **no destination** (a connected socket), the TOS control-message's IP level was guessed `AF_INET`, so a TOS mark on a connected **IPv6** socket would build an `IP_TOS` (v4) cmsg → kernel ignores/rejects it. Unreachable via the public API today (`kl_udp_send_to_tos`/`_from` always pass a dest; `kl_udp_send` uses tos −1), so defense-in-depth. | Derive the family from the dest, else the **source-pin** address, else v4. |

### Reported, not changed (accepted / by design)

| # | Area | Note |
|---|------|------|
| R1 | `src/udp.c` completion-loop src-pin/TOS send | A source-pinned or per-packet-TOS send on a **completion** loop skips the overlapped `kl_comp_post_udp_send` branch (which is plain-send only) and takes the synchronous provider `send()`; on `EAGAIN` it `udp_enqueue`s, which arms a **readiness** WRITE watcher that a completion loop never drives → the datagram stalls. **Pre-existing** (the pre-refactor path did the same via `kl_udp_io_raw_send` + `udp_enqueue`); IOCP-only, and only under transient send-buffer pressure on a source-pinned/marked datagram. Fix would be an overlapped `WSASendMsg` path for src/TOS; deferred as its own change. |
| R2 | `src/udp.c` batch machine loops | `udp_recv_dgram`/`udp_flush_dgram` put a `KlDgramRxSlot[64]`/`KlDgramTxDesc[64]` (~17 KB) on the stack **inside the batch branch only** (mmsg batching is Linux + opt-in `mmsg_batch>1`); the per-datagram path (lwIP/embedded) never allocates them. Fine on host stacks; noted for tiny-stack targets. Optionally cap `UDP_MMSG_MAX` or heap the arrays. |
| R3 | `src/socket_dgram_posix.c` `configure` (macOS IPv4) | Sets `IP_PKTINFO`/`IP_RECVPKTINFO` and reports `KL_DGRAM_RX_PKTINFO`, but macOS delivers the IPv4 local address via `IP_RECVDSTADDR` (not `IP_PKTINFO`), so `meta.has_local` stays 0 there; graceful degradation, but the cap bit overstates. Pre-existing (matches the old `setup_recv_opts`). |
| R4 | Winsock length casts (`wdg_send`/`wdg_recv`) | `size_t len`/`buflen` cast to `ULONG`/`int` without a guard; unreachable for UDP (≤65507 B). |
| R5 | `cfg->tos == 0` in `configure` | `if (cfg->tos)` treats 0 as "OS default / unset" (documented in `udp.h`), so 0 can't be an explicit clear; by design. |

### Areas audited clean (no findings)

- **cmsg build/parse** (posix + win): every parse `memcpy` is length-gated
  (`CMSG_LEN`/`WSA_CMSG_LEN`), the Winsock parsers stop on a runt cmsg
  (`cmsg_len < sizeof(WSACMSGHDR)`), and `dgram_build_control` writes at most the
  `DGRAM_TX_CMSG_SPACE` the buffers are sized to.
- **mmsg batch** (`socket_dgram_posix.c`): `rx/tx_batch_new`/`free` use matching
  `(size_t)n * sizeof/bufsz/ctrl_sz` for every member, sizes set before the
  sub-allocs so a mid-alloc NULL frees cleanly (no partial leak); `recv_batch`/
  `send_batch` clamp to `min(b->n, max/n)`; all per-slot indexing is `< n <= b->n`.
- **data-oriented slots**: `KlDgramRxSlot.data` points into the batch payload and is
  delivered synchronously before the next `recv_batch`; lifetime honored by
  `udp_recv_dgram`.
- **machine loops** (`udp.c`): batch fill double-bounded (`cnt < mmsg_batch && cnt <
  UDP_MMSG_MAX`); `udp_drop_front(sent)` drops exactly what was sent; EAGAIN vs
  hard-error-drop correct; recv loops re-check `recv_active`/`kl_handle_valid` after
  every deliver (no callback-reentrancy UAF); no uninitialised `KlSockAddr` reaches
  `on_recv` (the `family != UNSPEC ? &src : NULL` guard).
- **dgram resolution + lifetime**: `udp_dg()` maps NULL sockets → `kl_sockdef_dgram()`;
  `kl_udp_init` rejects a provider without `.dgram` before opening the fd (no leak);
  the completion providers inherit the underlying `.dgram` for config/opts only (the
  data-plane stays on `kl_comp_post_udp_*`, gated by `KL_EVENT_CAP_COMPLETION`).
- **`udp_group_ok`**: `kl_sockaddr_parse` + first-octet multicast range check
  (IPv4 224/4, IPv6 ff00::/8), family cross-checked; a bad group string rejects
  cleanly (`KL_ERR_INVALID_ARG`).
- **lwIP datagram ops**: send/recv/gso/configure/set_tos/mcast bounds + family
  guards correct; batch NULL → per-datagram; no leak.
- scan-build: clean. cppcheck: clean. ASan+UBSan unit suite: 891 tests, 0 failures.

## Seventh pass: KlSockAddr address-ABI neutralization + lwIP platform (2026-08-02)

**Scope:** everything added/changed since the sixth pass; the runtime event-provider seam
(#150/#151), the **KlSockAddr address-ABI neutralization** series (#153–#159: canonical type +
pure helpers, socket-vtable currency, resolver, udp public API, accept + proxy_protocol + peer
addr, protocol-TU purge + grep-gate, lwIP payoff), and the **lwIP platform** (#152, #160–#165:
socket + event providers, client axis via `resolve_sync_lwip`, responsive stop via
`platform_wakeup_lwip`, `udp_io_lwip` + the `udp_io` seam→KlSockAddr flip, TLS-over-lwIP via the
provider-routed mbedTLS BIO, production `lwipopts.h`). Files: `sockaddr.{h,c}`,
`sockaddr_native.h`, `socket.h` + `socket_{posix,winsock}.c`, `udp.c` +
`udp_io_{posix,win}.c` + `udp_internal.h`, `resolver_cache.c`, `dns_resolver.c`,
`dns_sys_{posix,win}.c`, `completion_driver.c` + `event_{pollcomp,iouring,iocp}.c` (UDP
marshalling), `integrations/lwip/*`, and the `integrations/mbedtls` socket-provider routing.

**Method:** five parallel deep-review agents (sockaddr core; udp seam + I/O; lwIP integration;
completion-backend UDP marshalling; resolver + socket providers) tracing marshalling bounds,
op/buffer/node lifetime, integer math, and untrusted-input parsing; mechanical sweeps
(`src/`+`parsers/`: no `strcpy`/`sprintf`/`gets`, no `atoi`/`atol`/`atof`, raw `malloc`/`free`
only in the allocator wrapper); **scan-build: "No bugs found"**; **cppcheck: clean**; the
**full unit suite under ASan+UBSan (`make debug` + `make test`), 891 tests, 0 failures, 0
sanitizer hits**; gcc-14 + cosmocc + MinGW (IOCP/WSAPoll) compile gates; and the Apple container
for Linux epoll/io_uring + the lwIP loopback/HTTPS runtime tests.

**Verdict: clean after fixes.** No Critical or High findings. Four **Medium** memory-safety
defects on untrusted-input (network) paths were found **and fixed this pass**, plus one Low
hardening fix and the cppcheck gate made version-robust. The KlSockAddr marshalling boundary is
the right place to have caught these; the neutralization concentrated all host↔neutral address
conversion into one reviewed seam.

### Medium: fixed this pass

| # | File(s) | Issue | Concrete risk | Fix |
|---|---------|-------|---------------|-----|
| M1 | `src/sockaddr_native.h` `kl_sockaddr_from_native` | AF_INET/AF_INET6 cases cast an **untrusted** `struct sockaddr` (from `accept`/`recvfrom`) to `sockaddr_in`/`sockaddr_in6` and `memcpy`'d the address **without a lower-bound `len` check** (only AF_UNIX checked `len`). | A short/garbage `socklen` from the kernel or a foreign provider → up to 16-byte out-of-bounds read past the caller's address buffer. | Added `if (len < sizeof(struct sockaddr_in{,6})) return -1;` before each cast/`memcpy`. |
| M2 | `src/udp_io_posix.c` (batched + single recv), `src/udp_io_win.c`, `src/completion_driver.c` (`KL_COMP_UDP_RECV`) | The four datagram-recv paths passed `&ksrc` to the `on_recv` callback **unconditionally**. When `kl_sockaddr_from_native` fails (unrecognised family, or `peer_len == 0` on a connected socket) it leaves the scratch **untouched** → an **uninitialised `KlSockAddr` (stack garbage) delivered as the datagram source** to application code. | Info-leak of stack contents into the app's source-address logic / reply targeting, reachable from network input. | Honor the return value: pass `NULL` (unknown source) instead of uninitialised stack; guard the local (pktinfo) address the same way. |
| M3 | `integrations/lwip/event_lwip.c` `lwev_mod` / `lwev_del` | Negative-fd guard used `!kl_handle_valid(fd)`, which only rejects `-1`; an `fd <= -2` passed and indexed `fd_to_idx[(int)fd]`; a **negative-index OOB** read (and an OOB write in `del`). `src/event_poll.c` guards the full negative range. | Memory corruption from a bogus/underflowed lwIP descriptor reaching mod/del. | `(int)fd < 0 || (int)fd >= cap`, matching `event_poll.c`. |

(M2 is one defect across four sites. Its readiness-posix instance predates this series but shares
the marshalling pattern introduced here and was fixed for completeness.)

### Low / tooling: fixed this pass

| # | File(s) | Issue | Fix |
|---|---------|-------|-----|
| L1 | `src/resolver_cache.c` `cache_insert` | Fixed `host[]` buffer filled via `memcpy(strlen+1)` with no self-check (callers bound it, but the function wasn't self-defending). | `strlen(host) >= KL_CLIENT_HOSTNAME_MAX` early return. |
| L2 | `Makefile` `cppcheck` | A newer cppcheck than CI's failed `make cppcheck` on `staticFunction` **false-positives** (public-API `kl_*` functions flagged should-be-static because cppcheck can't see the header consumers) and `normalCheckLevelMaxBranches` **informational** notes. | Added `--suppress=staticFunction --suppress=normalCheckLevelMaxBranches`; keeps the gate green across cppcheck versions without hiding real defects. |
| L3 | `src/async.c`, `src/dns_resolver.c`, `src/server.c` (×2), `src/body_reader_buffer.c` | Newer-cppcheck `constParameterPointer`/`constVariablePointer` on read-only pointers/params. | Added `const` (4 sites). The `kl_body_reader_buffer` factory param stays `void*` to match the `KlBodyReaderFactory` typedef; inline-suppressed with justification. |

### Reported, not changed (accepted / by design)

| # | File | Note |
|---|------|------|
| R1 | `src/socket_winsock.c` `kl_wsa_set_errno` | On Windows `EAGAIN != EWOULDBLOCK`; the mapping emits `EWOULDBLOCK`. Safe because every would-block test in-tree ORs **both** codes (verified across `dns_resolver.c` + the socket-seam callers). A contract, not a bug; keep the OR convention (a `KL_EWOULDBLOCK()` helper would make it self-enforcing). |
| R2 | `src/dns_resolver.c` `dns_resolve` | An over-long hostname (`> DNS_NAME_MAX`) is `snprintf`-truncated before `dns_build_candidates` rejects it, so a truncated name could be queried rather than hard-failed. No memory-safety impact. Optional: reject `strlen(host) >= DNS_NAME_MAX` up front. |
| R3 | `src/event_iocp.c` | A UDP send with an UNSPEC destination on an *unconnected* socket fails silently at completion (`ok=0`) rather than up-front; unreachable in practice because `kl_udp_send_to_from` validates `dest` at the public API. |
| R4 | `integrations/lwip/{socket_lwip,platform_wakeup_lwip}.c` | `set_reuseport`/`set_cork` return `-1` without setting `errno` (cosmetic diagnostic); the wakeup ignores `lwip_send`'s return (idempotent wakeup, recovered on the next poll tick). |

### Areas audited clean (no findings)

- **DNS response parser** (`kl_dns_parse_response`, `dns_skip_name`, `dns_extract_opt`,
  `dns_question_matches`): no compression-pointer following loop (names are terminated in place),
  label / RDATA / RR-header / address-size (`rdlen==4/16`) / multi-address-array bounds all tight.
  Also fuzzed (`fuzz_dns`).
- **`sockaddr.c`** parsers/formatters (`parse_ipv4`/`parse_ipv6`, `kl_sockaddr_format*`): bounded,
  no overflow; every builder `memset`s the union (no uninitialised-field leak in the round-trip).
- **UDP queue node** (flexible array): alloc-size overflow guard, matching free size on every
  path (send / EAGAIN-requeue / hard-error-drop / free); send-queue byte-cap underflow-guarded;
  GRO-split loop + cmsg build/parse bounds (runt-cmsg underflow guarded); recvmmsg/sendmmsg batch
  alloc/index/partial-free.
- **Completion backends** (pollcomp / io_uring / IOCP): op sockaddr-buffer sizes vs the socklen
  used in send/recv, recv name/control-buffer bounds, and op alloc/free lifetime; including the
  immediate-failure early-return paths and cancelled / zero-byte completions (no stale-buffer
  parse, no double-free/leak).
- **mbedTLS socket-provider routing**: NULL `sp` is byte-for-byte the prior behaviour
  (`kl_sockdef_*` fallback); per-session inheritance from the ctx; borrowed (not owned) provider
  pointer; no leak, no dangling copy; the completion memory-BIO path is unaffected.
- **socket_posix/winsock accept marshalling**: untrusted-peer `socklen` bounded before
  `kl_sockaddr_from_native`; `writev` iovcnt bounded before the stack `iovec[]`.

## Sixth pass: completion-axis feature work (PROXY / streaming / TransmitFile / TLS) (2026-08-01)

**Scope:** everything added/changed since the fifth pass; the completion-backend feature run
(PRs #128, #130, #133, #134, #135, #136): the completion driver's PROXY-header phase
(`comp_drive_proxy` + `kl_conn_ingest_proxy`) and overlapped streaming flush (`comp_stream_pump`,
8g-1); `event_iocp.c` (overlapped `WSARecvMsg` UDP local-addr, chunked `TransmitFile`, the
`post_recv` plaintext-during-PROXY guard); the three backends' `post_recv` guard; `drain.c`'s new
`kl_drain_data`/`kl_drain_consume`; `response.c` streaming-drain wiring; the TLS unit suites ported
to the shared completion-capable `tests/mock_tls.h`; and verification of the (already-landed)
platform-neutral mbedTLS backend on Windows.

**Method:** two parallel deep-review agents (completion axis; protocol/support + broad re-scan)
tracing op/buffer/connection lifetime, integer/offset math, and bounds; mechanical sweeps across
`src/`+`parsers/` (no `strcpy`/`sprintf`/`gets`, no `atoi`/`atol`/`atof`, raw `malloc`/`free` only
in the allocator wrapper, `kl_malloc` NULL-check + free-size discipline); `cppcheck` (clean on the
changed TUs); the **full 55-suite unit test under ASan+UBSan (`make debug-test`), 0 failures, 0
sanitizer hits**; and a MinGW compile-gate on the touched Windows TUs.

**Verdict: clean.** No Critical/High/Medium findings. Two **Low** allocator-discipline defects
(free-size mismatch on a zero-length completion send, latent under a bring-your-own *sized*
allocator; harmless under the default stdlib allocator) were found **and fixed this pass**.

### Low: fixed this pass

| # | File | Issue | Fix |
|---|------|-------|-----|
| L1 | `src/event_pollcomp.c` `pc_op_free` | A zero-length WRITE/SENDFILE/UDP op allocs `sendbuf` as `total ? total : 1` (1 byte) but `pc_op_free` freed it as `send_total ? send_total : KL_PC_CIPHER_SIZE` → frees a 1-byte block as **17408 bytes**. Reachable via a 0-length TLS-ciphertext send. A size-classed/arena `KlAllocator` mis-buckets the free. | Fall back to `1`, not `KL_PC_CIPHER_SIZE` (mirrors the alloc; `send_total` is set to the real size on every alloc path). |
| L2 | `src/event_iocp.c` `iocp_op_free` | Same shape: a zero-length WRITE frees a 1-byte `sendbuf` with `send_total == 0`. | Free `send_total ? send_total : 1`. |

### Informational (no action required)

- **I1** `event_iocp.c` `KL_IOCP_SENDFILE` completion advances `file_done` by the *requested*
  `file_chunk`, not bytes actually transferred. Correct because overlapped `TransmitFile` is
  all-or-nothing per chunk; a short success (never observed) would garble (not OOB) the body.
- **I2** `event_iouring.c` `kl_comp_cancel` leaves an op in-flight if the SQ is full at cancel
  time (liveness edge under SQ pressure, not a safety bug).
- **I3** `dns_resolver.c:976` write-buffer growth omits the project's `SIZE_MAX/2` doubling idiom;
  safe today (values bounded to a few KB), add the guard for uniformity.

### Verified correct (explicitly not findings)

Drain peek/consume is safe because `kl_comp_post_send` **copies** the buffer synchronously in all
three backends (so `kl_drain_consume` after posting is not a UAF); the PROXY `memmove` never
underflows (`kl_proxy_parse`'s `consumed ∈ [0,len]`, and `read_cap` 8192 ≫ `KL_PROXY_HEADER_MAX`
536, bounded by the `-1` guard); the ≤1-in-flight invariant (recv XOR send per conn, posted only
from a completion) means `kl_comp_cancel` yields exactly one aborting completion → one release, no
double-free; every `kl_comp_post_*` frees its op on all error paths; the Windows cmsg walks
(`kl_udp_win_parse_local`/`udp_parse_tos`) stop on a runt cmsg; `response.c` fixed stack buffers
(`cl_buf[48]`, `hdr[24]`) are correctly sized; CRLF header-injection + CL/TE smuggling (llhttp)
guards intact.

---

## Fifth pass: io_uring completion backend + provider auto-wire + stop-wakeup (2026-07-30)

**Scope:** the code added/changed across the io_uring-completion migration (PRs #101–#110):
`src/event_iouring.c` (the new ~750-line completion backend, hand-written op/registered-
buffer/splice/watcher lifecycle, the highest-risk new C), the 5a provider auto-wire
(`kl_event_native_provider` in every event backend + `kl_server_init`/client), the
`kl_server_stop` self-pipe wakeup (`src/server.c`), and the `iouringcomp`→`iouring` rename.

**Method:** mechanical sweeps (unsafe string/parse funcs, raw `malloc`/`free`, VLAs, `kl_malloc`
NULL-check discipline, integer-overflow guards) across `src/`; `cppcheck` on the new TUs; and,
the decisive step, an **ASan + UBSan + LeakSanitizer** run of the io_uring smokes on Linux
(Apple `container` VM, kernel 6.18), which the plain CI smokes don't provide.

**Issues found: 1** (Critical: 0, **High: 1**, Medium: 0, Low: 0), **fixed + verified.**

### High

| # | File | Issue | Fix |
|---|------|-------|-----|
| H1 | `src/event_iouring.c` (`kl_event_del`) | **Memory leak**: a readiness watch (`KlIouWatch`) removed while its `IORING_OP_POLL_ADD` was in flight was *unlinked* from `st->watches` with its free *deferred* to the poll-cancel CQE. At shutdown `kl_event_close`→`io_uring_queue_exit` drops that CQE, so the unlinked watch was never freed (and `kl_event_close`, freeing `st->watches`, no longer saw it). Surfaced as the `kl_server_stop` self-pipe watch leaking 48 B per server (2× in the async smoke). LeakSanitizer-confirmed; missed by CI because the io_uring backend was never run under LSan (only pollcomp was). | Keep the removed watch **linked** (skipped by `add`/`mod` via `!w->removed`); free it in the drain CQE (unlink+free) on the normal path, or in `kl_event_close` if the CQE never arrives (shutdown). Re-verified: both io_uring smokes pass under ASan+UBSan+LSan with **zero leaks**. |

**Systemic fix:** added `make smoke-iouring-asan` + a CI step in the *Completion (io_uring)*
job, so the io_uring op/buffer/splice/watcher lifecycle is now under LeakSanitizer in CI (the
gap that let H1 reach `main`).

### Clean (verified)

- **No unsafe functions** (`strcpy`/`strcat`/`sprintf`/`gets`/`atoi`/`atol`/`atof`) anywhere in
  `src/`+`parsers/`; raw `malloc`/`free` only in the default allocator wrapper (`allocator.c`).
- **`event_iouring.c` allocations**: all `kl_malloc` sites NULL-checked (registered pool is
  best-effort with malloc+SEND fallback; the rest fail cleanly via `iou_op_free`+`-1`).
- **Integer overflow**: `kl_comp_post_sendfile` guards `count`/`head_total` against `SIZE_MAX/2`;
  `sendcap` tracks the exact allocation for a correctly-sized free.
- **`cppcheck`** clean on the new TUs. **Hardening** intact (`-Werror -Wall -Wextra -Wpedantic
  -Wshadow -Wformat=2 -fstack-protector-strong -D_FORTIFY_SOURCE=3`; ASan/UBSan debug build).
- `LIBURING_UDATA_TIMEOUT` / cancel sentinels handled in the drain (no `(void*)-1` deref);
  single in-flight op per conn (driver invariant); idle-timeout cancel via `ASYNC_CANCEL` + abort.

## Fourth pass: mbedTLS backend + test shim + parser re-audit (2026-07-26)

**Scope:** the surface that changed since the third pass; `src/tls_mbedtls.c`
(significantly refactored: BIO callbacks routed through the socket seam, fds
retyped to `KlSocketHandle`, a shared `server_ctx_from_mem` + new
`kl_tls_mbedtls_ctx_create_from_buf`), the new test-network shim
(`tests/net_compat.{h,c}` posix/win + `tests/smoke_tls.c`), and a regression
re-audit of the untrusted-input parsers after the Winsock seam sweep + socket-
handle retype.
**Method:** three parallel source-level auditors; (1) deep `tls_mbedtls.c`
memory-safety (peer-cert extraction on untrusted mTLS input, error-path free
discipline, allocator/key-material handling), (2) the `net_compat` shim +
`smoke_tls` resource safety, (3) a regression re-check of `dns_resolver`,
`proxy_protocol`, `url`, `websocket`, `chunked`, `connection`; plus a tooling
sweep (dangerous functions, VLAs, raw alloc, cppcheck, and an ASan+UBSan real-
handshake run of the mbedTLS backend).

**Issues found: 3** (Critical: 0, High: 0, Medium: 0, **Low: 2, Doc: 1**),
**all fixed.**

The TLS backend came through clean. The auditors confirmed the hard parts sound:
the peer-cert CN/SAN extraction (`x509_extract_cn`/`x509_extract_san`) is
bounds-safe on attacker-controlled certs; every `memcpy` into the fixed `subject_cn`/
`issuer_cn`(256)/`san`(512) buffers is length-clamped before the copy, the
comma-separated SAN accumulator checks `off + ilen (+1) >= outlen` *before* every
write and always NUL-terminates, and the SHA-256 fingerprint fits its `char[65]`
exactly; the refactored ctx-creation error paths free each mbedTLS structure
exactly once and scrub key material (`kl_secure_zero`) on every path that owns the
key buffer (success and every early return); `read_file`'s length handling
(negative `ftell`, 1 MB cap, `len+1`) is safe; the BIO `kl_sockdef_send`/`recv`
errno→mbedTLS mapping is correct on both platforms; allocator create/destroy is
paired with the right sizes and the shared ctx is never double-freed. The
untrusted-input parsers showed **no regression** from the seam/handle changes;
the seam preserves the POSIX `ssize_t` contract and every `fd < 0` check migrated
to `kl_handle_valid`. Tooling: no dangerous functions, no VLAs, no unsanctioned
allocation, cppcheck clean, and the mbedTLS backend runs a real loopback handshake
clean under ASan+UBSan.

---

### Low

**L1: `x509_extract_cn` / `x509_extract_san`: defensive `outlen == 0` guard**
**`src/tls_mbedtls.c`**: *fixed.* Both peer-cert extractors wrote `out[0] = '\0'`
unconditionally and (CN) computed `outlen - 1`; safe with today's callers (fixed
256/512 buffers), but a future `outlen == 0` caller would write OOB / underflow.
Added `if (outlen == 0) return;` at the top of each; defense-in-depth on
untrusted-input functions.

**L2: `tests/smoke_tls.c`: client URL hard-coded the port instead of `SMOKE_PORT`**
**`tests/smoke_tls.c`**: *fixed.* The HTTPS URL literal duplicated `18443`; if
`SMOKE_PORT` changed, the client would silently target the old port and the test
would fail confusingly. Now built with `snprintf` from `SMOKE_PORT`.

### Documentation

**D1: `tests/net_compat_win.c`: undocumented WSAStartup precondition**
**`tests/net_compat_win.c`**: *fixed.* The loopback-pair helper needs Winsock
initialized; the library's load-time `WSAStartup` (socket_winsock.c) covers every
test, but that was implicit. Added a precondition note to the file comment.

### Areas audited clean (no findings)
- **`tls_mbedtls.c` peer-cert extraction**: bounds-safe on untrusted mTLS certs
  (clamped CN/SAN copies, pre-write accumulator bound, exact-fit fingerprint).
- **`tls_mbedtls.c` ctx error paths**: each mbedTLS struct freed once; key
  material scrubbed on all owning paths; no double-free/leak/UAF; correct free
  sizes; shared ctx not freed per-connection.
- **`tls_mbedtls.c` BIO + fd**: seam-routed I/O, correct errno mapping,
  `KlSocketHandle`/`KL_INVALID_SOCKET` throughout, transport owns the fd.
- **`net_compat_win.c` `kl_test_socketpair`**: each of listener/client/server
  closed exactly once on every `goto fail`, no double-close on success, no SOCKET
  leak, `addrlen` initialized before `getsockname`.
- **`smoke_tls.c` lifetimes**: server ctx freed on all paths (direct on init
  fail; via `kl_server_free` otherwise), per-iteration client ctx destroyed once,
  server stopped+joined before ctx destroy.
- **Untrusted-input parsers** (`dns_resolver`, `proxy_protocol`, `url`,
  `websocket`, `chunked`, `connection`), no regression; all bounds checks,
  length math, anti-spoof, and smuggling guards intact after the seam/handle sweep.
- **Tooling**: no `strcpy`/`sprintf`/`atoi`/`alloca`/…; no VLAs; no libc alloc
  outside `allocator.c`; `make cppcheck` clean; mbedTLS backend real-handshake
  ASan+UBSan green; full ASan+UBSan gauntlet green in CI on `main`.

## Recommendation
All three findings fixed. The mbedTLS backend, including the untrusted-input
peer-cert parsers and the refactored error paths, is well-bounded and now
platform-neutral. No further action.

---

## Third pass: Windows/Winsock PAL surface (2026-07-25)

**Scope:** the Windows platform TUs that landed with PAL Phase 6 (the Winsock
port), `src/socket_winsock.c`, `src/dns_sys_win.c`, `src/udp_io_win.c`,
`src/event_wsapoll.c`, `src/platform_win.c`, `src/server_plat_win.c`, and the
compatibility-boundary headers `src/sockcompat.h` / `src/socket.h` /
`src/dns_sys.h` / `src/platform.h` (Windows branches).
**Method:** three parallel source-level auditors (these TUs are Windows-only and
cannot be compiled on the macOS host; no MinGW), each cross-checking the Windows
implementation against its POSIX sibling's contract: (1) `udp_io_win.c` cmsg /
WSARecvMsg / batching; (2) `event_wsapoll.c` + `platform_win.c` +
`server_plat_win.c`; (3) the header shims + re-verification of the two fixes that
were already in the working tree.

**Issues found: 3** (Critical: 0, High: 0, **Medium: 3**, Low: several/Doc),
**all three Medium fixed.** The two fixes already present in the working tree
(`socket_winsock.c` errno translation on the seam ops + writev fail-loud;
`dns_sys_win.c` hosts-path truncation) were re-verified **correct and complete.**

> ⚠️ These fixes touch Windows-only TUs and were **not compiled locally** (no
> MinGW on the audit host). They must go green on the Windows CI job before merge.
> The POSIX build + full test suite remain green (the only shared header edit,
> `sockcompat.h`, is inside the `#if defined(_WIN32)` branch).

### M1: `event_wsapoll.c`: `kl_event_wait` returned `-1` without setting `errno`
**`src/event_wsapoll.c` (`kl_event_wait`)**: *fixed.*

On `WSAPoll` → `SOCKET_ERROR` the backend returned `-1` without touching `errno`
(Winsock reports via `WSAGetLastError()` and never sets the CRT `errno`). The
server accept loop (`server.c:582`) branches on `errno == EINTR` to decide
retry-vs-abort and then feeds `errno` to `kl_log_errno`; on a genuine poll error
it would read a **stale** `errno`; spuriously `continue`-looping if the stale
value happened to be `EINTR`, or logging a bogus reason otherwise. **Fix:**
translate via the now-shared `kl_wsa_set_errno()` before returning `-1`.

### M2: `platform_win.c`: `kl_plat_poll1` returned `-1` without setting `errno`
**`src/platform_win.c` (`kl_plat_poll1`)**: *fixed.*

Same root cause as M1 on the sync-client single-fd poll wrapper: returned
`WSAPoll`'s `-1` verbatim with no `errno`. **Fix:** `kl_wsa_set_errno()` on
`SOCKET_ERROR`, return `-1` (success value passed through unchanged).

*Shared-helper refactor for M1/M2:* `wsa_set_errno()` was file-static in
`socket_winsock.c`; promoted to a non-static `kl_wsa_set_errno()` declared in
`sockcompat.h` (Windows branch) so the event backend and `poll1` translate
identically to the socket seam ops. No behavior change on the existing 14 seam
call sites.

### M3: `udp_io_win.c`: truncated-datagram path parsed an indeterminate cmsg buffer
**`src/udp_io_win.c` (`kl_udp_io_recv_drain`)**: *fixed.*

On the `WSAEMSGSIZE` (truncated) return of `WSARecvMsg`, `msg.Control.buf/len`
are **not** reliably populated, yet the code fell through to
`udp_parse_local`/`udp_parse_tos` with `Control.len` still at the full buffer
size and the control buffer never zero-initialized. The additive
`cmsg_len >= WSA_CMSG_LEN(...)` checks prevent any out-of-bounds read, but the
delivered `recv_local` / `recv_tos_val` could be derived from indeterminate stack
bytes for a truncated datagram. **Fix:** a `have_control` flag set only on the
successful recv path now gates the cmsg parse, so truncated datagrams report no
local-addr/TOS (correct, that control data is unreliable).

### Low / Doc (noted, not fixed)
- **`event_wsapoll.c`**: empty pollset with an *infinite* timeout (`count==0 &&
  timeout_ms<0`) returns 0 immediately rather than blocking (POSIX `poll(...,-1)`
  blocks). Not reachable on the server path (the listen socket is always
  registered and the server passes a finite computed timeout). Low.
- **`platform_win.c`**: `kl_plat_random` casts `len` to `ULONG` for
  `BCryptGenRandom`; a `len > 4 GiB` would truncate. Callers use tiny buffers
  (mask keys, DNS txn-ids); not exploitable. The RNG return **is** checked and
  the buffer is never left uninitialized. Doc/Low.
- **Best-effort non-block failures**: `ioctlsocket(FIONBIO)` on the wakeup
  socket-pair read end ignores failure, matching the POSIX sibling's lenient
  `fcntl(O_NONBLOCK)` contract. Low, symmetry only.
- **`sockcompat.h`**: the `WSAE* → E*` mapping depends on the socket `E*`
  constants existing in `<errno.h>`; MinGW-w64 (the Makefile target) provides
  them but MSVC's `<errno.h>` historically does not. Worth a one-line "MinGW-w64
  required" note. Doc.
- **`kl_sockdef_close`**: returns `closesocket()`'s value without
  `kl_wsa_set_errno()`; no caller branches on errno after close. Doc.

### Verified sound (Windows PAL)
- **`socket_winsock.c`**: every seam op a caller tests for
  `EWOULDBLOCK`/`EINPROGRESS`/`EINTR` sets `errno` on `-1`; `connect` overrides
  `WSAEWOULDBLOCK→EINPROGRESS`; the tuning knobs that intentionally skip errno
  have no errno-inspecting caller; the writev `EINVAL`-on-`iovcnt>16` guard fails
  loud and can never overrun `stackbufs[16]` (real callers use `iov[7]`); the
  translation switch covers all tested codes.
- **`dns_sys_win.c`**: the hosts-path fix's `memcpy`s are bounds-safe (full
  default is 40 B into a `MAX_PATH`=260 buffer); build-once `static` buffer
  lifetime sound under the single-threaded loop.
- **`udp_io_win.c`**: TX/RX cmsg buffer sizing exact (`WSA_CMSG_SPACE`
  additive, no subtraction/underflow); all received-cmsg lengths additively
  checked; no dynamic alloc in the TU; matched `kl_free` size on the send queue;
  `kl_handle_valid` (not `<0`) re-check; `WSAEMSGSIZE` translated. The GSO-unsup
  `EIO` return correctly trips `udp.c:434`'s `!= EAGAIN/EWOULDBLOCK` predicate
  (no retry loop).
- **`platform_win.c`**: `kl_monotonic_ms` sec/rem decomposition avoids
  `ctr*1000` overflow, divide-by-zero guarded; socket-pair emulation closes all
  fds on every error path with no double-close; `kl_plat_file_pread` clamps
  `count > INT_MAX`.
- **`server_plat_win.c`**: `kl_srv_bind_unix` bounds `sun_path` before the
  `+1`-NUL `memcpy`; bind-failure closes + resets the fd; atomic console-ctrl
  handler registration sound.
- **`event_wsapoll.c`**: `grow_arrays` `INT_MAX/2` overflow guard + `size_t`
  math + fds-shrink-back on udata realloc failure; full unwind in init; matched
  free sizes in close.
- **Header shims** (`sockcompat.h`/`socket.h`/`dns_sys.h`/`platform.h`), `ssize_t`
  = `intptr_t` (pointer-width); all handle-carrying ops use `KlSocketHandle`
  (intptr_t) with `KL_INVALID_SOCKET`; no `SOCKET→int` truncation; no
  identifier-hijacking macros; every inline wrapper NULL-guards + falls back to
  `kl_sockdef_*`.

---

## Second pass: UDP feature surface (2026-07-19)

**Date:** 2026-07-19 (second pass, UDP feature surface)
**Scope:** the UDP stack that landed since the first pass; `src/udp.c` (roughly
doubled: multicast, `recvmmsg`/`sendmmsg` batching, GSO/GRO offload, ECN/TOS/DSCP
marking, and a send-path cmsg refactor), `src/udp_server.c`, a re-audit of the
`src/dns_resolver.c` untrusted-input parsers, and a full tooling/hardening sweep.
**Method:** three parallel auditors; (1) deep `udp.c` memory-safety/bounds,
(2) `udp_server.c` + DNS parser re-check, (3) automated tooling.

**Issues found: 2** (Critical: 0, High: 0, Medium: 0, Low: 1, Doc: 1), **both fixed.**

The UDP surface came through clean. The auditors confirmed the hard parts are
sound: RX/TX control-message buffer sizing (`UDP_RX_CMSG_SPACE` fits pktinfo +
UDP_GRO + TOS simultaneously; `UDP_TX_CMSG_SPACE` fits pktinfo + TOS exactly),
the `udp_build_control` CMSG_NXTHDR construction walk, batch alloc/free integer
safety (n∈[1,64], bufsz∈[1,65535], products well under SIZE_MAX; matched
free-sizes; partial-alloc cleanup), the recvmmsg/sendmmsg lifecycle and the
mid-batch use-after-free rechecks, `udp_drop_front`'s self-bounding dequeue (the
prior scan-build fix is genuinely sound), GSO/GRO split bounds, per-packet
`node->tos` initialization, and fd-leak/double-free paths. Tooling: no dangerous
functions, no VLAs, no unsanctioned allocation, `-Werror`-clean production build,
ASan+UBSan green, cppcheck only benign false positives.

---

## Low

### L1: `udp_parse_tos` computed a length by subtraction (size_t underflow risk)
**`src/udp.c` (`udp_parse_tos`)**: *fixed.*

```c
size_t dl = cm->cmsg_len - CMSG_LEN(0);   /* wraps if cmsg_len < CMSG_LEN(0) */
if (dl >= sizeof(int)) { ... }
```

A control message matching the TOS level/type but with `cmsg_len < CMSG_LEN(0)`
would underflow `dl` to a huge value and read `CMSG_DATA(cm)` past a runt cmsg.
Not reachable in practice (these cmsgs originate from the kernel, which always
sets a valid `cmsg_len`, and `CMSG_NXTHDR` won't advance into a runt trailer),
but it was the only one of the three cmsg parsers to subtract rather than use the
safe additive comparison. **Fix:** mirror `udp_parse_local`/`udp_parse_gro`;
`if (cm->cmsg_len >= CMSG_LEN(sizeof(int)))` / `>= CMSG_LEN(1)`. No subtraction,
matches the codebase idiom.

## Documentation

### D1: `kl_dns_parse_all` referenced but never implemented
**`CLAUDE.md`, `docs/dns_parity_design.md`**: *fixed.*

The Phase 2a design proposed a separate `kl_dns_parse_all()`; the implementation
instead folded multi-address collection directly into `kl_dns_parse_response`
(which fills `out->addrs[]` up to `KL_RESOLVE_MAX_ADDRS`). No such function
exists, yet CLAUDE.md claimed it was an implemented, fuzzed parser. **Fix:**
corrected CLAUDE.md to reference only `kl_dns_parse_response`, and noted the
plan-vs-implementation deviation in the design doc.

---

## Areas audited clean (no findings)

- **`udp.c` cmsg buffer sizing**: RX (64B) and TX (48B) buffers exactly cover the
  worst-case simultaneous cmsg sets; `udp_build_control` sets `msg_controllen`
  before the CMSG_NXTHDR walk, NULL-guards every write, and `used ≤ bufsz` always.
- **`udp.c` batch lifecycle**: no integer overflow (clamped n/bufsz), matched
  alloc/free sizes, correct partial-failure cleanup, mid-batch UAF rechecks after
  every callback (recv), self-bounding `udp_drop_front` (send).
- **`udp.c` GSO/GRO**: arg validation + fallback loop bounds; GRO split rechecks
  `recv_active` between segments.
- **`udp.c` public API**: NULL/`fd<0` guards on every entry; idempotent
  `kl_udp_free`; no double-close; fd freed on all `kl_udp_init` error paths.
- **`udp_server.c`**: all public entries NULL-checked; reply source-pinning
  bounds-checked and reset per datagram; all config fields forwarded with correct
  types; no fd leak on partial init.
- **`dns_resolver.c` response parser**: `dns_skip_name` /
  `kl_dns_parse_response` / `dns_question_matches`: every packet read length-checked
  before dereference; compression pointers skipped in place (no forward-follow, no
  loop); RDATA length validated before the fixed 4/16-byte memcpy; additive
  bounds math (no underflow); `naddrs` capped; anti-spoof (txn-id + source +
  question echo) intact. No regression.
- **Tooling**: no `strcpy`/`sprintf`/`atoi`/`alloca`/… ; no VLAs; no libc
  allocation outside `allocator.c`; `make` `-Werror` clean; `make debug-test`
  ASan+UBSan green; `make cppcheck` only benign `staticFunction`/const-suggestion
  false positives; production hardening flags all present.

## Recommendation

Both findings are fixed. The UDP feature surface, including the untrusted-input
cmsg parsers and the batched/offload paths, is well-bounded. No further action.
