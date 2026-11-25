# Gateway readiness: what Keel still lacks for the Herding Sparks migration

Status: **assessment, read-only.** It changes no code and proposes a roadmap for approval. Every
claim cites source at the commits below; "ran" marks behavior a test or probe actually exercised,
"source" marks a reading of the code that nothing executed.

| Repository | Commit | Notes |
|---|---|---|
| Keel | `7035bce47302052f96fd5095e94809e828e9c0c7` | `main`, tag `v3.5.0` (published). 3.4.0 = `7fdc146` |
| Hull | `06099279e99f2a70e564ca9fcf54ea6ad1034681` | `main`, v0.24.1; `vendor/keel` at `7fdc146` (Keel 3.4.0) |
| Herding Sparks | `aded16501b06710cd06efc24264917066153420a` | branch `stack-hardening`, the only commit carrying `docs/stack-review.md` |

Merged and released: #499 client response-body backpressure (3.4.0), #506 suspended-connection
hang-up detection (3.5.0). Pending: #508 (site), #509 (CI probe retry). Proposed: everything in
section F. Hull's `origin/feat/http-stream-pause` is already squash-merged into Hull `main` (#1199).

Probes ran on macOS arm64 (kqueue, poll, pollcomp builds) and in a Linux container (epoll,
io_uring). IOCP, WSAPoll, Cosmopolitan, lwIP and UEFI were read, not run. Probe sources live outside
the tree (session scratch); the commands and results are summarized where they are used.


## Status (2026-11-24)

The body below is the assessment as written against 3.5.0 and is kept unchanged as the record. Since:

| Item | Where | State |
|---|---|---|
| P0-1 peer credentials only for AF_UNIX | #510 | merged |
| P0-2 deadline sweep survives a released op | #511 | merged |
| P0-3 gateway-grade graceful drain | #512 | merged |
| P0-4a suspended streams progress on readiness | #514 | merged |
| Windows readiness on `select()` (replaces WSAPoll, found while doing P0-4a) | #515 | merged |
| P0-4b-a legacy `on_drain` bypass, `end_stream` at the cap | #516 | merged |
| P0-4b-b bounded admission and the writable edge | #517 | merged; see [http_stream_backpressure.md](http_stream_backpressure.md) |
| Graceful drain over a backpressured relay (release acceptance) | #518 | merged: `stream_relay.a_drain_*`: resumes byte-exact; past its deadline, cancelled once |
| P0-4b-c compression-aware admission | this branch | implemented; flush per accepted write, see [http_stream_backpressure.md](http_stream_backpressure.md) section 8 |

Where section E names WSAPoll, read the Windows `select()` backend
([windows_readiness_backend.md](windows_readiness_backend.md)): write readiness now survives a
half-close, and the loop no longer advertises `KL_EVENT_CAP_HANGUP`.

## A. Executive summary

The gateway's request shape (authenticated JSON `POST`, upstream loopback HTTP/1.1, an SSE relay
of raw upstream bytes with keepalive comments, client-abandonment cancels the upstream) is
**functionally possible on Keel 3.5.0 today**, and most of what is missing is Hull binding work.
Keel itself has **three P0 gaps and one P0 defect**:

1. **Server streaming output has no usable backpressure (P0).** A streamed response's buffer is
   bounded on readiness (1 MiB) but reaching the bound kills the stream; on completion backends the
   bound is bypassed while the handler runs (64 MiB to a non-reading client grew RSS by 66 MiB,
   ran on pollcomp); and on readiness a suspended stream is never flushed and never fires
   `on_drain`, so a producer that pauses until the client drains deadlocks (ran on kqueue). This
   contradicts `docs/contracts/streaming.md:22-31`. The upstream half of the chain
   (`KL_HTTP_CLIENT_BODY_PAUSE` / `kl_http_client_resume`) already exists; the downstream half does
   not.
2. **Graceful drain is not gateway-grade (P0).** During `kl_http_server_stop`'s drain the listen
   socket stays open (new connects hang in the backlog instead of being refused, so the fleet cannot
   fail over), a connection arriving during the drain makes a readiness loop spin at 100% CPU (ran
   on kqueue and poll), idle HTTP/1.1 keep-alive connections hold the drain until its deadline and
   get no `Connection: close`, and `stop` publishes `draining` before the deadline it reads.
3. **Peer credentials are fabricated on non-UNIX sockets (P0 defect, security).**
   `kl_http_request_peer_cred` / `kl_peer_cred_fd` return success on a TCP connection: on macOS
   uid 0, gid 0 (ran, reproduced twice), on Linux the kernel's "no credentials" values with
   `has_pid = 1` (ran). The header promises -1. An application that authorizes `uid == 0` admits a
   remote TCP client on macOS. Hull does not bind peer credentials, so Hull is not exposed.
4. **A latent use-after-free in the async deadline sweep (P0 once confirmed).** Both sweeps save
   `next_aop` before calling `on_deadline`; an `on_deadline` that cancels the next op, whose
   `on_cancel` frees it, makes the sweep read freed memory (source; not yet reproduced).

What does **not** need Keel work: streamed responses to `POST` (any method, any status, ran), SSE
framing (generic streaming plus `http_sse.h` suffices), hang-up during an active relay (a relay is
written while suspended, so the 3.5.0 watch covers it, ran), async client `POST` bodies and
pause/resume (33 tests), release of a paused upstream on cancel (ran), HTTP and raw-socket use of
Unix-domain sockets on POSIX (31/31 `test_unix_socket`, ran), request-body pause (exists, tested on
both axes), and `LISTEN_FDS` activation.

Hull bindings carry most of the migration: bump to Keel 3.5.0, a streamed response on any method,
`req.signal`, backpressure exposure, the head-timeout and in-flight caps, an `onShutdown` hook, and
a raw AF_UNIX stream client (section G).

Out of scope for Keel: read grants, append writes, flock, gzip for applications, chown and group
lookup, statvfs/hostname/`if_nametoindex`, NVML, process spawning, setuid, pidfd and cgroups,
CLI-plus-server packaging, `sd_notify` as an API, and admission control.

**Minimum viable roadmap:** four independently reviewable P0 PRs (section F): the peer-credential
fix, the deadline-sweep fix, gateway-grade drain, and streaming output backpressure (two PRs).
Everything else is P2 or deferred.

## B. Capability matrix

| Capability | Existing implementation | Evidence | Backend coverage | Missing functionality | Owning layer | Priority |
|---|---|---|---|---|---|---|
| Streamed response, any method/status | **COMPLETE** | `kl_http_response_begin_stream` `http_response.h:183`; POST then SSE probe (ran) | readiness + completion; **HTTP/1.1 only** (`http2_server.c:181-183` answers 500) | none for HTTP/1.1 | HTTP server | n/a |
| SSE framing (events, comments) | **COMPLETE** | `http_sse.c:11,28-43`; `test_http_sse` 19/19 (ran) | both axes | `kl_http_sse_begin` forces 200; relay non-200 via `begin_stream` | HTTP server | n/a |
| Output bound for a streamed response | **PARTIAL** | `KL_STREAM_DRAIN_MAX` 1 MiB `http_response.c:24,829-830`; sticky error `drain.c:46-48`, `http_response.c:779-781`; completion bypass `completion_http_server.c:466-470,488-491` | readiness bounded but fatal; completion unbounded inside the handler (ran) | non-fatal would-block; one budget on both axes; headroom for `end_stream` | HTTP server response layer | **P0** |
| Writable / drain notification for a suspended stream | **PARTIAL** | readiness: `kl_async_suspend` drops the fd `async.c:163-165`, flush only on the next write `drain.c:104-117`; completion: `comp_tls_on_write` flushes `completion_http_server.c:1222-1234` | completion only (ran: pollcomp fires, kqueue never) | readiness write interest while suspended with pending output; a deferred writable edge | HTTP server response layer + async | **P0** |
| Producer capacity query | **MISSING** | only `kl_drain_buffered(&res->drain)` (public struct field), blind to `comp_tlsq` | readiness only, approximately | pending/capacity covering drain, connection queue, TLS ring | HTTP server response layer | **P0** |
| Hang-up while suspended (incl. active relay) | **COMPLETE** (3.5.0) | `async.c:59-74,101-156`; probe P2 (ran: kqueue ~0 ms, pollcomp via next send) | prompt with `KL_EVENT_CAP_HANGUP`; best-effort 1 s peek elsewhere | none; documented fallback limits stand | HTTP server | n/a |
| Exactly-once `on_cancel` / `on_resume` | **COMPLETE** | `async_retire` `async.c:186-201`; tests `resume_racing_hangup_ends_once`, `teardown_with_queued_pool_work_frees_each_context_once` | both axes | n/a | HTTP server | n/a |
| Deadline sweep re-entrancy | **PARTIAL** | `next_aop` saved before `on_deadline` `http_server.c:735-741`, `http_server_core.c:596-604` | both axes | revalidate the next op after a callback | HTTP server | **P0** (confirm first) |
| Write after cancel is harmless | **PARTIAL** | release frees the response and closes the fd, bumps `comp_gen` only `http_connection.c:300-318`; stale write falls back to a raw send `http_response.c:796` | both axes | a generation-guarded response handle | HTTP server (app contract today) | P2 |
| Async client POST body (fixed, streamed) | **COMPLETE** | `body`/`body_read`; `async_stream.request_stream_chunked_upload` | both axes | `body_read` has no "no data yet" | HTTP client | n/a (P2 for no-data) |
| Client response pause/resume | **COMPLETE** (3.4.0) | `http_client.h:150,158-162,363-411`; `test_http_client_backpressure` 33/33 (ran) | both axes, TLS, decompression | n/a | HTTP client | n/a |
| Cancel/free while paused releases upstream | **COMPLETE** | `http_client_async.c:1835-1849`; probe P5 (ran) | both axes | streaming requests are never pooled (by design) | HTTP client | n/a |
| Redirect method/body policy | **COMPLETE** | `http_redirect.c:63-80,123-134,566-575` | both axes | streaming client has no redirect follower | HTTP client | Deferred |
| Client inactivity timeout | **MISSING** | whole-request `timeout_ms` only (`http_client_backpressure.md:190-192`) | n/a | an idle timer | HTTP client (or app timer) | P2 |
| Long streamed response cap | **COMPLETE** (configurable) | `max_response_size` default 4 MiB `http_client.h:40` | both | raise per request | application / Hull | n/a |
| Request-body pause | **COMPLETE** | `kl_http_request_pause_body` `http_server_core.c:866-884`; `test_read_flow_control` (ran) | both axes | hang-up while paused found only at idle timeout | HTTP server | Deferred |
| Stop accepting during drain | **PARTIAL** | readiness re-arms the listen fd `http_server.c:486,541-546`; completion refuses to arm `completion_http_server.c:979,986` | listener stays open everywhere; busy spin on kqueue/poll (ran), epoll likely (source) | close or deregister the listener at drain start | HTTP server | **P0** |
| Drain-aware keep-alive | **MISSING** | keep-alive from `req.keep_alive` only `http_connection.c:1276`; idle READING counts active `http_server_core.c:820-821`; probe (ran: drain ended at the deadline) | all | `Connection: close` on the next response; close idle keep-alive at drain start | HTTP server | **P0** |
| Drain observability | **PARTIAL** | `KlHttpServerStats` `http_server.h:411-423` lacks `draining` and idle/busy; Hull reads `s->server.draining` (`hull/src/hull/serve.c:2970`) | all | a draining flag and idle/busy counts | HTTP server | **P0** (small) |
| Drain deadline precision | **PARTIAL** | per-tick check, 1 s wait `http_server_core.c:57`; pollcomp overshoot 516 ms (ran); `stop` writes `draining` before `drain_deadline_ms` `http_server.c:764-768` | all | write the deadline first; bound the wait by it | HTTP server | **P0** (small) |
| Cancel remaining at free | **COMPLETE** | `http_server_core.c:486-487`; probe (ran) | all | n/a | HTTP server | n/a |
| HTTP/2 GOAWAY on drain | **COMPLETE** | `http2_nghttp2_server.c:396-408` (single-phase) | n/a | two-phase GOAWAY | HTTP/2 adapter | Deferred |
| HTTP/1.1 server over AF_UNIX | **BINDING_ONLY** | `KL_HTTP_SERVER_TRANSPORT_UNIX` `http_server.h:62,140-169`; node lifecycle `unix_socket_node_posix.c`; `test_unix_socket` 31/31 kqueue, 30+1 skip io_uring (ran) | POSIX readiness + io_uring; IOCP UNVERIFIED (likely broken, `event_iocp.c:1085-1089,998-1004`); lwIP/UEFI absent | Hull binding | Hull | P1 (if a Unix server is ported) |
| HTTP client over AF_UNIX | **BINDING_ONLY** | `http+unix://` `url.c:123-176`, `http_client_async.c:119-164,1679-1692` | POSIX; IOCP UNVERIFIED | Hull allowlist denies a NULL host (`hull/src/hull/cap/http.c:35-39`) | Hull | P1 |
| Raw async AF_UNIX stream | **PARTIAL** | `KlConnectOp`/`KlStream`/`KlListener` are family-neutral hooks (`docs/contracts/stream.md:20-24,132-158`); no socket-backed adapter | POSIX | Hull extends `cap/net_stream.c` (AF_INET/6 only today) | Hull | P1 (gateway's `whole.sock` read) |
| Abstract AF_UNIX addresses | **MISSING** | `kl_sockaddr_from_unix` uses `strlen` `sockaddr.c:36-45`; `to_native` appends a NUL `sockaddr_native.h:48-54` | Linux only by nature | exact-length abstract addressing in `KlSockAddr` | substrate (sockaddr) | P2 |
| Peer credentials on AF_UNIX | **PARTIAL** | `kl_peer_cred_fd` `http_server_plat_posix.c:173-212`; Linux SO_PEERCRED, macOS getpeereid + LOCAL_PEERPID (ran) | Linux, macOS (ran); BSD (source); Windows -1 | AF_UNIX check (**P0 defect**); status taxonomy; provider seam; a transport-level header | substrate (today in the HTTP slice) | **P0** fix, P2 extension |
| UDP source / destination address | **COMPLETE** | `datagram.h:136-139,222`; `socket_dgram_posix.c:339-347` (ran: `local` = header destination) | per `datagram.md:474-477` | n/a | substrate (datagram) | n/a |
| UDP receiving interface index | **MISSING** | `ipi_ifindex`/`ipi6_ifindex` never read `socket_dgram_posix.c:90-112`, `udp_cmsg.c:30-55`, `udp_cmsg_win.c:43-60`; IPv6 `scope_id` zeroed (ran) | none | `ifindex` in receive metadata; link-local scope | substrate (datagram) + provider seam | P2 (tp-watch stays Python) |
| MSG_TRUNC / MSG_CTRUNC | **PARTIAL** | `KL_DGRAM_TRUNCATED` (ran on kqueue, epoll); MSG_CTRUNC absent (grep) | TRUNC on all; CTRUNC none | report ancillary truncation | substrate (datagram) | P2 |
| Egress interface selection (unicast) | **MISSING** | only `multicast_iface` `datagram.h:229`; source pinning by address `udp_cmsg.c:114-140` | n/a | bind-to-interface / `ipi_ifindex` on send | substrate (datagram) | Deferred |
| AF_UNIX datagram to a path | **PARTIAL** | adopt via `kl_datagram_init` delivered `READY=1` (ran on kqueue, epoll); `kl_datagram_socket_init` silently turns AF_UNIX into AF_INET `datagram_open.c:57-58` | readiness (ran); completion UNVERIFIED | refuse or support AF_UNIX explicitly | substrate (datagram) | P2 |
| `sd_notify` | **OUT_OF_SCOPE** (as API) | no NOTIFY_SOCKET handling (grep) | n/a | generic: abstract addressing (above) | Hull runtime | n/a |
| `LISTEN_FDS` activation | **COMPLETE** | `http_server_activation.c:19-90`; tested in `test_unix_socket` (ran) | POSIX | CLOEXEC and type check on inherited fds | HTTP server | P2 |
| HTTP/2 streaming and flow control | **MISSING** | 500 for streaming `http2_server.c:181-183`; auto WINDOW_UPDATE, whole-body buffering `http2_nghttp2_server.c:256-262,330-335` | n/a | per-stream backpressure | HTTP/2 adapter | Deferred |
| Filesystem grants, append, flock, gzip for apps, chown, statvfs, NVML, spawn, setuid, pidfd, cgroups, packaging, admission | **OUT_OF_SCOPE** | stack-review rows 1, 4, 5, 7, 8, 12-14 | n/a | n/a | Hull / application / native helper | n/a |

## C. HTTP streaming data flow

Chain: inference server -> Keel async HTTP client -> Hull application -> Keel HTTP server
streaming response -> downstream client.

| # | Boundary | Owner | Copy / borrow | Maximum | Backpressure propagates? |
|---|---|---|---|---|---|
| U1 | Upstream kernel receive buffer | OS | copy | SO_RCVBUF | yes: TCP flow control once the client stops reading |
| U2 | Client read buffer, unparsed rest of one read | `KlHttpClient` | copy | one 8 KiB read | yes: a paused client stops reading (`http_client.h:363-411`) |
| U3 | Client TLS plaintext held by the engine | TLS session | copy | one record set | yes: drained on resume, tested with mbedTLS |
| U4 | Streaming decoder output | `KlDecompress` | copy | no decoded output held with `dfeed_stoppable`; bounded fallback otherwise | yes |
| U5 | Client body callback fragment | application | **borrowed** for the callback only | one fragment | the application returns `KL_HTTP_CLIENT_BODY_PAUSE` |
| A1 | Application relay buffer (incomplete SSE event, a held fragment) | Hull / application | application-defined | application-defined (the Python gateway caps an event at 1 MiB) | application policy |
| D1 | Coalescing stack buffer | call frame | copy | 4 KiB (`http_response.c:723-748`) | transient |
| D2 | Response drain (`KlHttpResponse.drain`) | response | copy | 1 MiB default; `enable_drain(..., 0)` = **unbounded** | **only as a fatal -1**; `on_drain` fires on completion, never while suspended on readiness |
| D3 | Completion connection queue (`comp_tlsq`) | connection | copy; posted region pinned | 1 MiB for async producers; **unbounded while the handler runs or the driver flushes** (`completion_http_server.c:264-311,466-470`) | async producers: would-block to D2; inside the handler: none |
| D4 | Backend send operation | backend | copy or pinned borrow | 256 KiB per post (`completion_http_server.c:331-351`) | via the WRITE completion |
| D5 | Server TLS engine / completion out-ring | TLS session | copy (ciphertext) | about one record (readiness); absorbed into D3 (completion) | readiness: WANT_WRITE to D2; completion: as D3 |
| D6 | Compression codec window | `KlHttpCompressStream` | copy | codec-internal; one input can emit about its own length | no: sticky error only |
| D7 | Downstream kernel send buffer | OS | copy | SO_SNDBUF | yes: EAGAIN into D2 |
| D8 | No-allocator fallback | none | none | n/a | no: up to 256 EAGAIN spins on the loop thread, then -1 (`http_response.c:404-455`) |

**Unbounded queues:** D3 inside the handler on completion backends (default configuration, ran:
64 MiB accepted, RSS +66 MiB on pollcomp); D2 after an explicit `max_size = 0` on any backend (ran:
RSS +65.5 MiB on kqueue). Every other boundary is bounded.

**Where propagation breaks:** between D2/D3 and A1. The server never tells the application "full,
wait" without killing the stream, never tells it "drained" on a suspended readiness stream, and
does not let it read D3's occupancy. Upstream propagation (U1-U5) is complete. The final chunk
never overtakes queued data (`end_stream` goes through D2, D3 is FIFO), but `end_stream` is itself
subject to the D2 cap, so a stream filled to the cap cannot be terminated (ran).

The only workaround today is a polling hack: compare `kl_drain_buffered(&res->drain)` to a threshold
well under `max_size` and flush from a timer (`kl_drain_flush` / `kl_http_response_send`). It works
on readiness (ran), misses D3 on completion, and is not the contract `streaming.md` describes.

## D. Lifecycle and cancellation

The relay model is documented and tested: begin the stream, `kl_async_suspend`, write while
suspended, end from `on_resume` (`docs/contracts/streaming.md:37-47`; tests
`a_stream_written_while_suspended_keeps_moving`, `an_idle_suspended_event_stream_outlives_the_read_timeout`).
So active relay is the suspended state, and 3.5.0's hang-up watch applies to it.

| State | Downstream FIN / RST | Upstream failure | Deadline | Cancel (incl. from callbacks) | Server free | Limitations / owner |
|---|---|---|---|---|---|---|
| S1 Admission (handler running) | seen after the handler returns | n/a | `read_timeout_ms`, `header_timeout_ms`, `body_timeout_ms` (`http_server_core.c:778-796`) | complete/cancel inside the handler deferred to the frame (`async.c:278-283,386-389`) | n/a | HTTP server; admission policy is the application's |
| S2 Suspended, waiting for upstream headers | HANGUP watch (`async.c:59-74`), CANCEL gives one `on_cancel`; 1 s peek sweep without the capability | client `on_done` with an error; the application completes or cancels | `deadline_ms` optional, `on_deadline` is a trigger (`async.h:28-43`); client `timeout_ms` default 30 s | idempotent, exactly once (`async.c:186-201`) | every op cancelled before teardown (`http_server_core.c:486-487`) | FIN behind unread bytes invisible without `KL_EVENT_CAP_HANGUP` (`suspended_hangup_detection.md:353-380`) |
| S3 Active relay (suspended, stream begun) | as S2 (ran: kqueue ~0 ms); completion also cancels on the next failed send | application writes a final event, then completes or cancels | exempt from idle timeout (`http_server_core.c:691`): only `deadline_ms` | **no guard against writes after cancel** (`http_connection.c:300-318`, `http_response.c:796`) | as S2 | write-after-cancel is an application contract today |
| S4 Upstream paused (downstream full) | watch still armed | upstream close not seen until resume or `timeout_ms` (`http_client.h:399-402`) | client deadline keeps running (`http_client.h:387-389`) | cancel/free while paused closes the socket, never pooled (ran) | as S2 | `kl_http_client_free` is forbidden inside a client callback (`http_client.h:395-397`): defer it |
| S5 Waiting for downstream drain (suspended) | watched | n/a | only `deadline_ms` | as S3 | as S2 | **readiness: no flush, no `on_drain` (P0 gap)**; completion: flushes and fires |
| S6 Resumed, backlog flushing (SENDING) | no hang-up watch; a write error after a reset closes; half-close plus no reads reaped by `read_timeout_ms` (`http_connection.c:1426`) | n/a | idle timeout | op already retired | pool free closes | HTTP server |
| S7 Completed | `on_resume` ends the stream; a streamed async response is never kept alive (`async.c:265-271`) | | | | | |
| S8 Draining (stop) | as the state the connection is in | as above | `drain_timeout_ms`, overshoot up to one 1 s tick | second `stop` forces (`http_server.c:769-770`) | queued output discarded, TLS close_notify best effort (`http_connection.c:333-344`) | listener stays open; idle keep-alive holds the drain (P0) |

Verified properties: exactly-once terminals (tests above); hang-up watch removed before any
re-register or close (`async.c:76-89,198`); a completion slot returns to the pool only after its
last completion (`http_server_core.c:619-633`); cancel-in-push is deferred (probe P3, ran on both
axes). Unverified hazard: the deadline sweep's `next_aop` read after a callback that may free it.
Guaranteed cleanup is not claimed where the FIN can hide behind unread data on a loop without
`KL_EVENT_CAP_HANGUP`; there only a finite `deadline_ms` whose `on_deadline` ends the op bounds it.

## E. Backend matrix

"ran" = exercised by a test or probe in this assessment; "src" = from source only.

| Backend | Hang-up while suspended | Stream output bound | Suspended stream flushes | Drain busy spin | AF_UNIX HTTP server/client | Peer credentials | UDP local addr / ifindex |
|---|---|---|---|---|---|---|---|
| epoll | prompt, `KL_EVENT_CAP_HANGUP` (3.5.0 CI) | 1 MiB, fatal (src) | no (src) | likely (src, edge-triggered re-arm) | yes (src; suite not run here) | SO_PEERCRED; **TCP misreport** (ran) | addr yes (ran) / ifindex no |
| kqueue | prompt (ran) | 1 MiB, fatal (ran) | **no** (ran) | **yes** (ran, 1.0 s CPU/s) | yes (ran, 31/31) | getpeereid + LOCAL_PEERPID; **TCP: uid 0** (ran) | addr yes (ran) / ifindex no |
| poll | prompt on Linux; sweep elsewhere | 1 MiB, fatal (src) | no (src) | **yes** (ran on macOS) | yes (src) | as the OS | as the OS |
| WSAPoll | prompt (3.5.0 measurement) | 1 MiB, fatal (src) | no (src) | likely (src) | AF_UNIX node lifecycle only; suite excluded (`Makefile:595`) | -1 (`http_server_plat_win.c:146-153`) | addr yes (src) / ifindex no |
| io_uring | prompt (3.5.0 CI) | **unbounded in handler** (src, shares `completion_http_server.c`) | yes (src) | no listener arm (src) | yes (ran, 30+1 skip) | SO_PEERCRED (ran) | addr yes (src) / ifindex no |
| IOCP | 1 s peek sweep | unbounded in handler (src) | yes (src) | no (src) | **UNVERIFIED, likely broken** (AcceptEx `IPPROTO_TCP`, ConnectEx bind; `event_iocp.c:1085-1089,998-1004`) | -1 | addr yes (src) / ifindex no |
| pollcomp | prompt on Linux; sweep on macOS | **unbounded in handler** (ran) | **yes** (ran) | no (ran, 0.0 s CPU) | yes (src) | as the OS | as the OS |
| Cosmopolitan | sweep | as poll (src) | no (src) | as poll (src) | as poll (src) | UNVERIFIED | UNVERIFIED |
| lwIP | no detection (`recv_peek` fails) | provider-dependent (src) | provider-dependent | n/a | absent | provider bypassed: host `getsockopt` on an lwIP index (src) | lwIP-raw datagram per `datagram.md` |
| UEFI | no detection (`recv_peek` fails) | provider-dependent (src) | provider-dependent | n/a | absent | absent | EFI_UDP4 per `datagram.md` |

Capability negotiation already documents the hang-up column (`KL_EVENT_CAP_HANGUP`). The other
columns are not capability-gated today; the P0 work below makes the stream columns identical on
both axes rather than adding a capability.

## F. Proposed roadmap

Grouped into independently reviewable PRs. No release numbers are assigned.

### P0-1. Peer credentials only for AF_UNIX (defect, small)

- **Problem:** `kl_peer_cred_fd` (`http_server_plat_posix.c:173-212`) queries any fd; on TCP it
  returns 0 with uid 0 (macOS) or "no credentials" values (Linux). The contract says -1
  (`http_server.h:480-502`).
- **Consumer:** any application that authorizes by peer uid (hs-privd's rule, if it were ported).
- **Change:** check `getsockname` family is AF_UNIX (and a stream or seqpacket type) before the
  query; fix the `KlPeerCred` comment (macOS does fill the pid, `http_server.h:212-214`).
- **Compatibility:** source and ABI unchanged; behavior now matches the documentation. CHANGELOG
  under Fixed (security).
- **Tests:** TCP fd returns -1; AF_UNIX socketpair still returns the caller's uid, both on Linux and
  macOS.

### P0-2. Async deadline sweep survives a callback that frees another op (small)

- **Problem:** `next_aop` is read before `on_deadline` and used after it (`http_server.c:735-741`,
  `http_server_core.c:596-604`).
- **First:** an ASan reproducer (section H). If it does not reproduce, close as not-a-defect.
- **Change:** restart the scan after any callback, or mark the list dirty from `async_retire` and
  rescan (the pattern `async_hup_sweep` already uses).
- **Tests:** two ops with the same deadline, the first's `on_deadline` cancels the second, the
  second's `on_cancel` frees it; ASan clean on readiness and completion.

### P0-3. Gateway-grade graceful drain (small to medium)

- **Problem:** listener stays open and busy-spins readiness loops; keep-alive holds the drain;
  deadline ordering race; no public drain state.
- **Consumer:** hs-gateway: on SIGTERM new connects must be refused so the fleet fails over, held
  requests get up to 75 s (`gateway.py:2781-2815`).
- **Change (all in `http_server*.c`):**
  - at drain start, deregister and close the listen socket (an adopted `listen_fd` is closed too,
    documented), so new connects are refused; this also removes the busy spin;
  - while draining, every HTTP/1.1 response carries `Connection: close`, and idle keep-alive
    connections (READING with nothing buffered) are closed at drain start;
  - `stop` writes `drain_deadline_ms` before publishing `draining`; the wait timeout is bounded by
    the remaining drain time;
  - `KlHttpServerStats` gains appended `draining` and an idle/busy split (or a
    `kl_http_server_draining()` accessor) so Hull stops reading the internal field.
- **Axes:** readiness and completion share `drain_progress`; the listener close is per axis
  (`http_server.c:486,541-546`, `completion_http_server.c:979,986`).
- **Compatibility:** behavior change (listener closed at drain start, keep-alive ended while
  draining): minor version, upgrade note. Struct growth is append-only.
- **Tests:** connect during drain is refused; CPU during drain stays near zero; keep-alive client
  gets `Connection: close` and the drain ends without waiting for the deadline; suspended op and a
  suspended stream still complete during the drain; deadline honored within one wait of tolerance.

### P0-4a. Suspended streams make progress on readiness (medium)

- **Problem:** `kl_async_suspend` removes the fd (`async.c:163-165`); a suspended stream with
  pending output is never flushed and `on_drain` never fires. Completion already does both.
- **Change:** a suspended connection with pending stream output keeps WRITE interest (combined with
  the 3.5.0 HANGUP watch registration, which is classified by interest, not udata), flushes the
  drain on writability, and fires `on_drain` from that progress point, never from inside a write.
  WRITE interest is dropped again once the drain is empty.
- **Compatibility:** none at the API; fixes the documented contract (`streaming.md:27-31`).
- **Tests:** the paced-producer probe (pause at a threshold, resume from `on_drain`) completes on
  kqueue, epoll, poll and WSAPoll as it already does on pollcomp; hang-up still cancels a suspended
  stream with pending output.

### P0-4b. Non-fatal stream admission and one capacity on both axes (medium to large)

- **Problem:** a full stream is a sticky error; completion bypasses the bound inside the handler;
  no capacity query; `end_stream` can be refused.
- **Minimal API** (HTTP response layer, general-purpose, mirrors `KlStream`'s write facet and reuses
  the internal `drain_reserve.h` admission):

  ```c
  typedef enum {
      KL_HTTP_STREAM_ACCEPTED,     /* copied; queued or sent */
      KL_HTTP_STREAM_WOULD_BLOCK,  /* nothing taken; wait for on_writable; not sticky */
      KL_HTTP_STREAM_TOO_LARGE,    /* the framed chunk exceeds capacity */
      KL_HTTP_STREAM_CLOSED,       /* the connection or response is gone */
      KL_HTTP_STREAM_ERROR         /* allocation or transport failure */
  } KlHttpStreamStatus;
  KlHttpStreamStatus kl_http_response_stream_try_write(KlHttpResponse *res, const char *data, size_t len);
  KlHttpStreamStatus kl_http_response_stream_try_writev(KlHttpResponse *res, const KlIoVec *iov, int iovcnt);
  size_t kl_http_response_stream_pending(const KlHttpResponse *res);   /* drain + queue + in flight + TLS */
  size_t kl_http_response_stream_capacity(const KlHttpResponse *res);
  int    kl_http_response_stream_on_writable(KlHttpResponse *res, void (*fn)(void *ctx), void *ctx);
  ```

- **Semantics:** all-or-none per framed chunk; bytes copied before return (unchanged); WOULD_BLOCK
  takes nothing; capacity counts every server-side boundary (D2, D3, D5) on both axes and applies
  inside the handler; headroom is reserved so `end_stream` is never refused by the bound; the
  writable edge fires at most once per WOULD_BLOCK, deferred to a progress point (completion: after
  the WRITE completion retires, with the flushing flag cleared; readiness: from P0-4a), once more
  on terminal failure, and never after `on_cancel` or close. Queued output is freed on release, as
  today. TLS: admission on plaintext before encryption. Compression: a matching
  `kl_http_compress_stream_try_write` refuses input while pending exceeds capacity minus a worst
  case expansion. SSE helpers gain `try` variants only if a consumer needs them; raw bytes suffice
  for the gateway.
- **Compatibility:** additive; `kl_http_response_stream_write` keeps its current (fatal) semantics
  and `streaming.md` is corrected to describe both.
- **Application chain:** on WOULD_BLOCK the relay returns `KL_HTTP_CLIENT_BODY_PAUSE` upstream and
  keeps the fragment; in `on_writable` it retries and, on ACCEPTED, calls `kl_http_client_resume`.
- **Tests:** the reproducer table (section C) inverted: bounded RSS on both axes, WOULD_BLOCK then
  ACCEPTED after drain, `end_stream` always admitted, cancel while WOULD_BLOCK fires no writable,
  parity on readiness and completion, TLS with a real engine, a gateway-shaped end-to-end test
  (slow downstream, fast upstream: upstream server's send blocks, RSS flat).
- **Depends on:** P0-4a.

### P2 (general-purpose, no current blocker)

- **Peer identity on the socket axis:** a transport-level header with a status taxonomy (`OK`,
  `UNSUPPORTED`, `NOT_LOCAL`, `ERROR`), an appended provider op plus a cap bit so a provider without
  it (lwIP) never triggers a host syscall, and the existing functions as wrappers. Small to medium.
- **Datagram receive interface:** `ifindex` in the receive metadata (an in-callback query beside
  `kl_datagram_recv_tos`, so `KlDatagramRecvFn` is unchanged), link-local `scope_id`, and an
  ancillary-truncation flag. Medium. Consumer: tp-watch if it is ever ported.
- **Abstract AF_UNIX addresses** in `KlSockAddr` (exact length, no trailing NUL), and
  `kl_datagram_socket_init` either supporting AF_UNIX or refusing it. Small. Consumer: a runtime
  sending `sd_notify` to an abstract `NOTIFY_SOCKET`.
- **Generation-guarded response writes** so a write after cancel fails with CLOSED instead of
  reaching a closed or reused fd. Small, falls out of P0-4b's CLOSED status.
- **Client inactivity timeout** and **`body_read` "no data yet".** Small each. Consumer: an upstream
  silence timer without an application timer.
- **IOCP AF_UNIX** accept/connect (after the experiment in H confirms the failure).
- **`LISTEN_FDS`:** set CLOEXEC and check the socket type of inherited descriptors.

### Deferred (no consumer)

HTTP/2 streaming responses and per-stream flow control; two-phase GOAWAY; hang-up detection while a
request body is paused; egress interface selection; a socket-backed raw `KlStream` adapter (Hull can
compose `KlConnectOp` and `KlStream` itself); pooling and redirects for streaming client requests.

## G. Hull handoff (no Keel change needed)

1. Bump `vendor/keel` to v3.5.0; keep `suspended_hangup = CANCEL`. Wire `on_cancel` to a JS
   `req.signal` (or `onClose`) that also cancels any upstream `http.async.stream` the handler owns.
   Defer `kl_http_client_free` out of client callbacks (`http_client.h:395-397`).
2. A streamed response on any method: `res.stream({status, headers})` returning
   `{write, comment, close}` over `kl_http_response_begin_stream` / `end_stream`; write raw bytes
   (the gateway relays upstream events verbatim and must pass non-200 statuses, which
   `kl_http_sse_begin` cannot). Let `app.sse` accept `POST` with a buffered body
   (`runtime/js/routes.c:753` registers GET only; Keel does not require it).
3. Keep streaming routes on HTTP/1.1 (Keel answers 500 for streaming over HTTP/2).
4. Backpressure exposure in JS (`writableLength`, an awaitable drain). Until P0-4 lands, a timer
   that flushes and compares `kl_drain_buffered` to a threshold well under 1 MiB on readiness; on
   completion that misses the connection queue. Add a write-stall timeout
   (the gateway's `CLIENT_WRITE_TIMEOUT` 300 s) and the upstream silence timer (900 s).
5. Raise or make configurable the 10 min head-timeout clamp (`include/hull/limits/http.h:45`; the
   gateway needs 1800 s) and the 64 in-flight fetch cap; set `max_response_size` for long upstream
   streams (Keel default 4 MiB).
6. An `onShutdown(graceMs)` hook for server applications and `--drain-timeout 75000` for the
   gateway (default 5000, `serve.c:281`). Use the P0-3 accessor instead of `s->server.draining`.
7. A raw AF_UNIX stream client (extend `cap/net_stream.c`, AF_INET/6 only today) behind a manifest
   grant, for `whole.sock` (one JSON line, not HTTP), or change tp-watch to answer HTTP and use
   `http+unix://` (then let the allowlist accept a unix target, `cap/http.c:35-39`).
8. Optional: `sd_notify` READY/STOPPING from the runtime with `Type=notify` (pathname sockets work
   with plain `sendto`; abstract ones need the P2 addressing change or a direct syscall).
9. Corrections to `docs/stack-review.md` (Herding Sparks): row 2's function is
   `kl_http_response_begin_stream` (`http_response.h:183`); row 6's `http.fetch("unix:...")` does
   not fit a raw JSON-line socket; row 10 is a Keel gap (ifindex), not a binding; row 11 applies to
   neither tp-watch (not HTTP, no credential check) nor privd (inetd-style, exec as root).

## H. Remaining uncertainties

| Uncertainty | Smallest experiment |
|---|---|
| Deadline-sweep use-after-free is real | Two suspended ops, equal deadlines; op A's `on_deadline` calls `kl_async_cancel(B)`, B's `on_cancel` frees B; run under ASan on kqueue and pollcomp |
| epoll drain busy spin | The slice-D drain probe in the Linux container (epoll), measuring CPU during a 3 s drain with one client connected after stop |
| io_uring and IOCP share pollcomp's in-handler unbounded queue | The section-C reproducer under `BACKEND=iouring` (Linux container) and on a Windows runner (IOCP) |
| IOCP AF_UNIX accept/connect | Run `test_unix_socket`'s HTTP cases with `BACKEND=iocp` on the Windows CI runner |
| AF_UNIX datagram on completion backends | The `READY=1` adopt probe under pollcomp and io_uring |
| Peer-credential misreport on FreeBSD/OpenBSD/NetBSD | The TCP probe on a BSD runner (getpeereid on TCP) |
| Real-engine TLS for a suspended SSE stream | The P2 SSE probe with the mbedTLS integration instead of the identity mock |
| Whether closing an adopted `listen_fd` at drain start breaks socket-activated deployments | Run a `LISTEN_FDS` server under systemd, stop it, confirm systemd re-queues new connections to the next instance rather than refusing them; decide close versus deregister per that result |
| Hull's real gateway throughput with the polling workaround | A Hull prototype relay against a slow client, measuring RSS and latency before P0-4 |
