# HTTP/1.1 client response-body backpressure

Status: implemented for the async HTTP/1.1 client (`kl_http_client_start_s` streaming). This note
records what the code did before the change, the design that was chosen, the invariants it keeps, and
a follow-up design for the server and for uploads (not implemented).

## 1. Public contract

A `KlHttpClientBodyFn` keeps its `int` return type. The values are now named:

| Value | Meaning |
|-------|---------|
| `KL_HTTP_CLIENT_BODY_CONTINUE` (0) | Fragment consumed; delivery goes on. Unchanged. |
| `KL_HTTP_CLIENT_BODY_ABORT` (-1) | Fail the request. Unchanged; any value other than 0 or PAUSE also aborts. |
| `KL_HTTP_CLIENT_BODY_PAUSE` (1) | Fragment consumed; deliver nothing more until `kl_http_client_resume()`. |

`int kl_http_client_resume(KlHttpClient *)` schedules the continuation on a later loop tick and returns
0, or returns -1 with no effect when the client is not paused (never paused, running, finished,
cancelled), or when the continuation could not be scheduled (the client then stays paused and the call
may be retried). Repeated calls while a continuation is scheduled return 0 and do nothing more.

Guarantees: no body callback, no `on_complete` and no successful `on_done` while paused; delivery
resumes at the exact byte where it stopped (no loss, duplication, reordering or replay); resume never
calls back synchronously; a resumed client may pause again at once; cancellation and terminal errors
outrank a pause; resume after termination never revives the request; a callback may call
`kl_http_client_resume` (effective only if that callback returns PAUSE) or `kl_http_client_cancel`
(nothing more is delivered, no `on_done`), but must not call `kl_http_client_free`.

Existing callers return only 0 or -1 and see no change. The sync client has no resume, so a PAUSE there
fails the request, as any non-zero value always did. Fragments are never empty (the parser used to
pass llhttp's empty end-of-buffer span flushes through; it no longer does).

## 2. Reconnaissance: what the code did

- **Receive path** (`src/protocols/http/http_client_async.c`, `async_handle_receiving`): reads up to
  `KL_HTTP_CLIENT_RECV_BUF_SIZE` (8 KiB, not 16 KiB) into a stack buffer and hands the whole read to
  the parser; every callback fires synchronously inside one `parse()` call.
- **Parser** (`http1_response_parser_llhttp.c`): `resp_on_body` returned the user's value straight to
  llhttp. llhttp maps -1 to `HPE_USER`; any other non-zero value became an llhttp errno, and a value
  equal to `HPE_PAUSED` (21) was taken for message-complete (a latent bug, now an abort).
- **llhttp continuation** (verified against `vendor/llhttp` and with a probe program): a span callback
  returning `HPE_PAUSED` stops execution with `error_pos` at the end of that span and the next state
  saved; `llhttp_resume` + `llhttp_execute(error_pos, rest)` continues exactly there. Re-executing
  with zero bytes after a pause on the final body byte fires message-complete. Re-executing with zero
  bytes while a body span was open at the previous buffer end re-fires `on_body` with length 0 (the
  reason empty spans are now dropped). So the parser can stop exactly at a body-callback boundary.
- **Decompression** (`http_client_common.c`, `kl_http_client_decomp_on_body`): one `dfeed` call per
  body span, emitting through `decomp_emit_to_user`. The `KlDecompress.dfeed` vtable has no partial
  consumption: an emit that returns non-zero fails the stream (the miniz backend returns -1). So the
  decoder cannot stop mid-input without a vtable change. The shipped backend (miniz) decodes `gzip`
  only; there is no shipped `deflate` decoder.
- **TLS**: `tls->read` returns decrypted bytes; the engine keeps undelivered plaintext and reports it
  through `pending()`. Nothing outside it owns those bytes.
- **Transport, both axes**: the async client registers a `KlWatcher` on its socket and reads with
  `kl_sock_recv` / `tls->read`. On readiness engines that is a native watch. On io_uring and pollcomp the
  client's socket is a tagged watcher too, relayed as readiness (a single-shot `POLL_ADD`, or a `poll()`
  entry), and its reads are plain non-blocking `recv` through the POSIX control plane of the overlapped
  provider. On IOCP the watcher is a zero-byte `WSARecv` probe that consumes nothing. **The client never
  posts a payload receive on any engine**, so a pause can leave no receive outstanding that owns
  response bytes.
- **Dropping read interest**: a zero mask is not uniform. epoll, poll and io_uring still report
  hang-up and error; IOCP's probe still waits for READ when the mask lacks WRITE. `kl_watcher_del`
  is uniform on every engine (io_uring cancels the poll; IOCP cancels the probe and frees it in the
  drain; a stale event already in a dispatch batch is discarded by the liveness check in
  `keel__event_dispatch_watcher`).
- **Deferred dispatch**: the established mechanism is a zero-delay `kl_timer_add` (the client already
  uses one for deferred error completion, `done_timer`).
- **Timeouts**: `timeout_ms` is a whole-request deadline (connect, TLS, send and receive together), not
  an inactivity timeout.
- **Wrappers**: the redirect client and the pooled client take no streaming configuration
  (`kl_http_client_start` / `kl_http_client_start_pooled`), so no callback passes through them. The
  decompression wrapper is the only adapter between the parser and the user.
- **Server**: already has request-body flow control (`kl_http_request_pause_body` /
  `kl_http_request_resume_body`); see section 6.

Answers to the reconnaissance questions:

1. Pause/resume fits entirely in the protocol layer using existing primitives (`kl_watcher_del` /
   `kl_watcher_add`, `kl_timer_add`). **No lower-layer change was needed**: no socket, provider, event
   or TLS change.
2. The completion architecture permits no outstanding payload receive after a pause, because the
   client posts none (see above).
3. The parser can stop exactly at a body-callback boundary (llhttp `HPE_PAUSED` from a span callback).
4. The decoder cannot stop part-way through a `dfeed` call; it can be stopped between calls. Hence a
   new optional vtable op, `dfeed_stoppable`, which the shipped decoder implements, and a bounded
   fallback (slicing and a stash) for decoders without it. See below.
5. TLS pending bytes stay in the engine and are read first by the continuation, without waiting for
   readiness.

## 3. Design

Everything lives in the HTTP client TUs; each layer keeps what it owns.

**Parser.** `resp_on_body` maps CONTINUE to 0, PAUSE to `HPE_PAUSED` (setting `body_paused`), anything
else to an abort. `parse()` returns a new appended result, `KL_HTTP1_PARSE_PAUSED`, with `*consumed` at
the end of the accepted span, after `llhttp_resume`. Empty spans are never delivered.

**Client receive buffer.** A streaming client owns an 8 KiB heap `rbuf` and parses from
`rbuf[rbuf_off..rbuf_len)`. After a pause the unparsed rest of that read stays there (no copy, no
second buffer). The buffer is refilled only when it has been parsed completely.

**Pause.** `cli_pause` sets `paused` and `parse_pending`, and removes the watcher (`watch_parked`).
Nothing reads the connection, so the kernel buffer fills and TCP closes the window.

**Resume.** `kl_http_client_resume` schedules `resume_timer` (zero delay; one at a time). The
continuation `cli_on_resume` works in byte order:

1. the decompressor's stash (output decoded from earlier bytes; fallback path only), in fragments of
   at most 16 KiB;
2. the decoder's own unfinished work and the compressed body bytes it had not taken (`pend_in`,
   borrowed from `rbuf`, which is neither refilled nor moved until this is drained); a stopped
   final flush is continued here, and its trailer check runs when it finishes;
3. a deferred `on_complete` (the message ended while output was stashed), then success;
4. the parser residual in `rbuf`, parsed even when empty (a pending message-complete fires);
5. TLS plaintext and the socket, through the ordinary read loop, which re-registers the watcher only
   when it needs the network (`cli_wait_readable`).

Any step may pause again; the continuation then parks with nothing scheduled.

**Decompression: a decoder that can stop.** `KlDecompress` gains an optional op, appended after
`destroy` under the append-only, zero-default vtable policy (`docs/contracts/compatibility.md`):

```c
int (*dfeed_stoppable)(KlDecompress *self, const char *data, size_t len, int flush,
                       size_t *consumed, int (*emit)(void *, const char *, size_t), void *emit_ctx);
```

`emit` may return 1 ("stop"). The fragment it was given is accepted. The decoder then returns 1 without
emitting again, with `*consumed` the input bytes it took; output it has decoded but not emitted, and
its state, stay inside the session. The next call continues there with the rest of the input, possibly
zero bytes (a decoder still holding output must make progress on an empty call). A stop in the final
call (`flush = 1`) is continued by another flush call, which validates the trailer when it finishes.
Returning 0 means all input was taken and nothing is held.

The wrapper calls it with the whole body span and stops it at the fragment the user paused on; the
untaken input stays in `pend_in`. It holds no decoded output itself, so memory while paused does not
depend on the compression ratio. It checks every return: a decoder that stops unasked, emits after
being asked to stop, reports more input than it was given, or reports completion with input left
fails the request (`KL_ERR_PARSE` mid-body, `KL_ERR_COMPRESS` in the flush). So a resume that calls
the decoder again always follows a fragment the user accepted, and no input is fed twice. The miniz
backend implements it: its output already lives in its 32 KiB dictionary ring, and the only extra
state is whether tinfl still holds output (`more_out`).

ABI: Keel promises source and static-relink compatibility, not binary compatibility (no shared object,
no `dlopen`), so a decoder compiled against an older header never meets the newer library. A decoder
that zero-initializes its table (designated initializer, `memset`, or `= {0}`) leaves the new slot
NULL and keeps the fallback below. A table written with positional initializers still compiles to the
same thing (C zero-fills omitted members) but draws `-Wmissing-field-initializers` under `-Wextra`;
four in-tree test decoders were switched to designated initializers for that reason.

**Decompression: fallback for decoders without it.** A pausable wrapper over a decoder that has only
`dfeed` feeds compressed input in slices of at most 512 bytes and feeds no further slice once the user
paused. Output the decoder still produces for the slice in progress goes to a stash capped at
`512 * 1032 + 64 KiB` (one slice at deflate's maximum ratio, plus slack); a decoder that exceeds it
fails the request. The stash is freed once drained. The final flush may also be stashed; the user's
`on_complete` then waits for the resume. Checksum and trailer validation run unchanged (the final flush
is still the last call). The sync path does not slice (its wrapper is not pausable) and never uses
`dfeed_stoppable`.

**Callback trampolines.** The client reaches every user streaming callback through `cli_on_body`,
`cli_on_headers`, `cli_on_complete` (behind the decompressor when there is one). They make a
cancellation from any callback stop all further delivery, and scope `resume` calls made from inside a
callback.

**Memory bound while paused.** Client struct + `rbuf` (8 KiB) + whatever the TLS engine already holds
(one record) + the decoder session (miniz: about 43 KiB) + on the fallback path only, at most one stash
(bounded as above; a highly compressible body reaches about 516 KiB with a deflate decoder). Independent of the response
size. The stress test streams 48 MiB through under 32 KiB of client allocations.

## 4. Invariants

- At most one continuation is scheduled; every terminal path (success, error, cancel, free) cancels
  it (`cli_cancel_resume_timer`).
- While paused, the client has no watcher, no scheduled read and no posted receive. The only
  registration it may hold is the request deadline (and the scheduled continuation after a resume).
- `rbuf` bytes before `rbuf_off` are never read again, except the `pend_in` tail, which the
  continuation drains before touching `rbuf_off`.
- A paused response keeps its connection; streaming requests are never pooled, so no connection can
  be reused early.
- `on_done` is the only place the user may free the client; streaming callbacks may cancel.

## 5. Timeouts and known limitations

- The whole-request deadline keeps running while paused; a pause that outlasts `timeout_ms` fails with
  `KL_ERR_TIMEOUT`. Separating an inactivity timeout and a consumer-stall timeout from the
  whole-request deadline would need new configuration and is left for a follow-up.
- PAUSE is honoured by the async streaming client only. The sync client fails the request.
- Fallback path only (a decoder without `dfeed_stoppable`): the stash cap is sized for deflate (about
  1032:1). A decoder that produces more than the cap while the caller is paused fails the request, and
  whether that happens depends on when the caller pauses (and on read boundaries), not only on the
  response. Two conforming kinds can: one above about 1100:1 for a 512-byte slice (zstd, brotli on
  degenerate input), and one that holds output back and releases it at once (at the final flush, a
  body over about 580 KiB fails at any ratio). The error is `KL_ERR_PARSE` mid-body and
  `KL_ERR_COMPRESS` in the final flush. The failure is clean: bounded memory, nothing leaked, an
  in-order prefix delivered, no `on_complete`. Such decoders should implement `dfeed_stoppable`;
  removing the fallback would need a major version (the op would become required).
- A direct user of the public parser `kl_http1_response_parser_llhttp_s` whose body callback returns 1
  (formerly an abort) now gets `KL_HTTP1_PARSE_PAUSED` and must handle it.
- HTTP/2 is out of scope: per-stream backpressure there means withholding the stream's
  `WINDOW_UPDATE` (owned by the session vtable / nghttp2), not pausing the shared connection.

## 6. Follow-up design (not implemented)

**Server request bodies.** The server already streams bodies to `KlHttpBodyReader.on_data` (continue or
-1 abort) and already has flow control: `kl_http_request_pause_body` / `kl_http_request_resume_body`
(`include/keel/http_request.h`). On readiness it drops READ interest; on completion it stops posting the
next receive, with at most one receive in flight (`completion_http_server.c`); a paused connection stays
subject to the idle timeout. `on_data` always consumes a whole chunk, so no residual is kept. Nothing
further is required for parity. A `PAUSE` return value for `on_data` could be added later as sugar over
`kl_http_request_pause_body`; client and server need not share code (their read paths and buffers are
different, and coupling them would couple two state machines for one flag).

**Server responses.** Streaming output is unbuffered unless the handler enables the drain
(`kl_http_response_enable_drain`); with no drain a would-block spins a bounded number of times and then
fails the stream; with a capped drain a full drain is a hard error; with `max_size = 0` the drain is
unbounded, so a slow client can grow server memory. `KlDrain.on_drain` exists but is not wired to HTTP
responses or SSE, so a producer cannot wait for capacity. A follow-up would expose a writable /
on-drain notification on the response (backed by the existing `KlDrain` low-water hook) and a
would-block result from the streaming write, so a producer can stop and resume rather than fail.

**Client uploads.** Already bounded: the async upload pulls one 4 KiB chunk only when the previous one
is fully written, and waits for writability in between. What is missing is a way for `body_read` to say
"no data yet": it can return only >0, 0 (EOF) or -1. A future extension would add a would-block return
plus a `kl_http_client_upload_resume()` to re-arm the pull.
