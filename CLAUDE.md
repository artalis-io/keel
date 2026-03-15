# KEEL — Development Guide

## Build

```bash
make              # build libkeel.a (epoll on Linux, kqueue on macOS)
make BACKEND=poll # build with poll() backend (universal POSIX fallback)
make CC=cosmocc   # build with Cosmopolitan C (APE, auto-selects poll backend)
make test         # build and run all 664 unit tests
make examples     # build all 22 example programs (24 with TLS, 26 with compression)
make debug        # debug build with ASan + UBSan (recompiles from clean)
make analyze      # Clang static analyzer (scan-build)
make cppcheck     # cppcheck static analysis
make fuzz         # build libFuzzer fuzz targets (requires clang)
make coverage     # code coverage report (Linux, requires lcov)
make install      # install lib + headers + pkg-config (PREFIX=/usr/local)
make uninstall    # remove installed files
make clean        # remove all build artifacts
```

## Project Structure

- `include/keel/` — Public headers. Each module is a `.h` file. `keel.h` is the umbrella.
- `src/` — Core source. Each module is a `.c` file.
- `parsers/` — Pluggable parser backends (`parser_llhttp.c`, `response_parser_llhttp.c`).
- `vendor/` — Vendored libraries (llhttp, utest.h). Do not modify.
- `tests/` — Unit tests using Sheredom's utest.h framework.
- `examples/` — Example programs (hello_server, rest_api_server, middleware, static_files, streaming, sse, body_readers, websocket_server, websocket_client, tls_server, tls_client, async, thread_pool, h2_server, h2_client, client, streaming_client, async_client, async_thread_pool, custom_allocator, connection_pool, url_parser, timer, redirect_client, proxy_client, compress_server, decompress_client).
  - Compression backend also provides decompression (`decompress_miniz.c`) for client-side use.
- `docs/` — Architecture and roadmap documentation.

## Architecture

31 orthogonal modules, each independently testable:

1. **allocator** — Bring-your-own allocator interface + default stdlib wrapper
2. **event** — epoll (Linux) / kqueue (macOS) / io_uring / poll (universal POSIX fallback) event loop abstraction
3. **event_ctx** — Composable event loop context (KlEventCtx: loop + allocator + watcher list)
4. **request** — Parsed HTTP request struct (header-only, zero alloc)
5. **parser** — Pluggable request/response parser vtables (ships with llhttp backend)
6. **response** — Response builder: buffered (writev), sendfile, or streaming chunked
7. **router** — Route matching with `:param` extraction + middleware chain
8. **connection** — Pre-allocated connection pool + state machine + timeout sweep
9. **server** — Top-level glue: init, bind, async event loop, stop (handlers can be sync or async)
10. **body_reader** — Pluggable body reader vtable + built-in buffer reader
11. **body_reader_multipart** — RFC 2046 multipart/form-data state machine parser
12. **chunked** — RFC 7230 parser-agnostic chunked transfer-encoding decoder
13. **cors** — Built-in CORS middleware with configurable origins/methods/headers
14. **tls** — Pluggable TLS transport vtable (bring-your-own backend)
15. **async** — Connection suspension for async operations (uses KlEventCtx)
16. **thread_pool** — Worker thread pool with pipe-based event loop wakeup
17. **url** — URL parser (http/https/ws/wss, IPv6, CRLF injection guard)
18. **client** — HTTP/1.1 client: sync (blocking) + async (event-driven via KlEventCtx), response streaming (push) + request streaming (chunked pull)
19. **websocket_client** — Async WebSocket client with masked frames (RFC 6455)
20. **h2_client** — HTTP/2 client with pluggable session vtable (multiplexed streams)
21. **resolver** — Pluggable async DNS resolver vtable (bring-your-own backend)
22. **sse** — Server-Sent Events helper: line framing over chunked streaming (zero alloc)
23. **error** — Diagnostic error codes (KlError enum) + kl_strerror()
24. **timer** — One-shot timer scheduling on KlEventCtx (min-heap, checked per event loop tick)
25. **client_pool** — HTTP client connection pool: caches idle TCP+TLS connections keyed by (host, port, is_tls) for keep-alive reuse
26. **redirect** — HTTP redirect following: automatic 3xx redirect with RFC 7231/7538 method transformation, cross-origin auth stripping, URL resolution
27. **compress** — Pluggable response compression vtable: single-shot buffer + streaming chunked, with KlCompressConfig on KlConfig for server-wide use
28. **decompress** — Pluggable response decompression vtable: single-shot buffer + streaming, with KlDecompressConfig on KlClientConfig for client-side use
29. **drain** — Backpressure write buffer: buffers unsent data on would-block, flushes on write-readiness, with on_drain callback and max_size cap
30. **file_io** — Pluggable async file I/O vtable: submit/cancel/tick lifecycle, io_uring backend for true async reads (non-TLS file responses)
31. **resolver_cache** — Caching DNS resolver decorator: wraps any KlResolver, caches successful results with configurable TTL/capacity, transparent to consumers

**Deliberate design choices:**

- **Single-threaded event loop** — Same model as Node.js, Redis, Nginx (per-worker). No mutexes, no data races. `KlThreadPool` offloads blocking work; multi-core scaling is horizontal via `SO_REUSEPORT`.
- **O(n) router** — Linear scan over routes. A `memcmp` scan over even hundreds of routes costs nanoseconds, invisible next to network I/O. A trie would add complexity for no measurable gain.
- **O(n) timeout sweep** — Iterates all connection slots once per tick. At default `max_connections=256`, this fits in L1 cache. Not worth optimizing.
- **No built-in 503 / load shedding** — `kl_server_stats()` exposes connection counts so users can implement load shedding as middleware. Policy decisions (thresholds, Retry-After values) belong in application code, not the framework.
- **No global memory monitoring** — The allocator is pluggable, so the framework can't reliably track total memory. OS-level OOM handling is the right layer. Existing per-resource caps (`max_body_size`, `max_header_size`, `KlDrain.max_size`) bound the main vectors.
- **Resolver sync-completion contract** — `KlResolver.resolve()` may call `done_fn` synchronously. Decorators handle this via an `in_resolve`/`completed` sentinel pattern (see `resolver_cache.c`). This is inherent to sync-completion-capable vtables and documented rather than architecturally changed.

## Key Types

| Type | Header | Purpose |
|------|--------|---------|
| `KlServer` | `server.h` | Server instance: config, router, pool, event loop |
| `KlConfig` | `server.h` | Configuration: port, bind_addr, max_connections, timeouts, max_body_size, max_header_size, allocator, parser |
| `KlRequest` | `request.h` | Parsed request: method, path, query, headers, content_length, body_reader |
| `KlResponse` | `response.h` | Response builder: status, headers, body/file/stream modes |
| `KlBodyReader` | `body_reader.h` | Vtable: on_data, on_complete, on_error, destroy |
| `KlBufReader` | `body_reader.h` | Buffer reader: growable buffer with max_size limit |
| `KlMultipartReader` | `body_reader_multipart.h` | Multipart parser: parts array, state machine, overlap buffer |
| `KlMultipartPart` | `body_reader_multipart.h` | Single part: name, filename, content_type, data, data_len |
| `KlMultipartConfig` | `body_reader_multipart.h` | Limits: max_part_size, max_total_size, max_parts |
| `KlChunkedDecoder` | `chunked.h` | Chunked TE decoder: state machine, no allocation |
| `KlAllocator` | `allocator.h` | Vtable: malloc, realloc (with old_size), free (with size) |
| `KlRequestParser` | `parser.h` | Vtable: parse (returns KlParseResult), reset, destroy |
| `KlResponseParser` | `parser.h` | Client-side response parser vtable |
| `KlConn` | `connection.h` | Connection: fd, state, read_buf, request, response, parser, route |
| `KlRouter` | `router.h` | Route table + match function |
| `KlRoute` | `router.h` | Single route: method, pattern, handler, user_data, body_reader |
| `KlMiddleware` | `router.h` | Middleware function: `int (*)(KlRequest *, KlResponse *, void *)` |
| `KlMiddlewareEntry` | `router.h` | Registered middleware: method, pattern, fn, user_data |
| `KlCorsConfig` | `cors.h` | CORS config: allowed origins, methods, headers, credentials |
| `KlEventLoop` | `event.h` | Platform event loop: init, add, mod, del, wait, close |
| `KlEventCtx` | `event_ctx.h` | Composable event context: loop + allocator + watcher list |
| `KlTls` | `tls.h` | Vtable: handshake, read, write, shutdown, pending, reset, set_hostname, destroy |
| `KlTlsCtx` | `tls.h` | Opaque per-server TLS context (user-owned) |
| `KlTlsConfig` | `tls.h` | TLS config: ctx, factory, ctx_destroy |
| `KlTlsResult` | `tls.h` | Enum: OK, WANT_READ, WANT_WRITE, ERROR |
| `KlTlsFactory` | `tls.h` | Factory: creates per-connection KlTls from shared context |
| `KlWatcher` | `event_ctx.h` | Registered FD watcher: fd, callback, user_data, ctx-owned list |
| `KlWatcherFn` | `event_ctx.h` | Watcher callback: `void (*)(int fd, KlEventMask ready, void *user_data)` |
| `KlAsyncOp` | `async.h` | In-flight async operation: conn, deadline, on_resume/deadline/cancel |
| `KlAsyncFn` | `async.h` | Async callback: `void (*)(KlAsyncOp *op, void *user_data)` |
| `KlThreadPool` | `thread_pool.h` | Opaque thread pool: workers, work/done queues, pipe watcher |
| `KlWorkItem` | `thread_pool.h` | Work item: work_fn (worker), done_fn (event loop), cancel_fn (shutdown) |
| `KlThreadPoolConfig` | `thread_pool.h` | Config: num_workers, queue_capacity, allocator |
| `KlUrl` | `url.h` | Parsed URL: is_https, host, port, path |
| `KlClientHeader` | `client.h` | Request/response header: name, value |
| `KlClientResponse` | `client.h` | Response: status, body, headers, allocator (by value) |
| `KlProxyConfig` | `client.h` | Proxy config: host, port, auth (borrowed pointers) |
| `KlClientConfig` | `client.h` | Client config: timeout_ms, max_response_size, TLS, proxy |
| `KlClient` | `client.h` | Opaque async client handle |
| `KlClientDoneFn` | `client.h` | Async completion callback |
| `KlClientStreamCfg` | `client.h` | Per-request streaming config: response push callbacks + request pull callback |
| `KlClientBodyFn` | `client.h` | Response body streaming callback (push-based) |
| `KlClientHeadersFn` | `client.h` | Response headers-complete callback |
| `KlClientReadFn` | `client.h` | Request body streaming callback (pull-based, like read()) |
| `KlResolver` | `resolver.h` | Pluggable async DNS resolver vtable: resolve, cancel, destroy |
| `KlResolveReq` | `resolver.h` | Opaque per-request handle (resolver-owned) |
| `KlResolveResult` | `resolver.h` | Resolved address: sockaddr_storage, addrlen, ai_family |
| `KlResolveDoneFn` | `resolver.h` | Resolution completion callback |
| `KlWsClientConn` | `websocket_client.h` | WebSocket client connection handle |
| `KlWsClientConfig` | `websocket_client.h` | Client config: timeout, max_frame_size, TLS, protocol, ping_interval_ms |
| `KlWsClientCallbacks` | `websocket_client.h` | Callbacks: on_open, on_message, on_close, on_error |
| `KlH2ClientConn` | `h2_client.h` | HTTP/2 client connection handle |
| `KlH2ClientConfig` | `h2_client.h` | Config: timeout, max_streams, TLS, session factory |
| `KlH2ClientSession` | `h2_client.h` | Pluggable session vtable (wraps nghttp2 etc.) |
| `KlH2ClientHeader` | `h2_client.h` | Request/response header: name, value |
| `KlH2ClientResponse` | `h2_client.h` | Accumulated stream response: status, headers, body |
| `KlSse` | `sse.h` | SSE stream handle: write_fn + write_ctx + response (zero alloc) |
| `KlError` | `error.h` | Diagnostic error enum: 25 codes (alloc, network, DNS, TLS, HTTP, redirect, proxy, event, thread, pipe) |
| `KlTimerFn` | `timer.h` | Timer callback: `void (*)(void *user_data)` |
| `KlTimerEntry` | `timer.h` | Timer heap entry: deadline_ms, cb, user_data, id |
| `KlClientPool` | `client_pool.h` | Connection pool: flat array, idle timers, per-host limits |
| `KlClientPoolConfig` | `client_pool.h` | Pool config: capacity, max_per_host, idle_ms |
| `KlClientPoolConn` | `client_pool.h` | Acquired connection handle: fd, tls, reused flag |
| `KlRedirectConfig` | `redirect.h` | Redirect config: max_redirects |
| `KlRedirectClient` | `redirect.h` | Opaque async redirect client handle |
| `KlRedirectDoneFn` | `redirect.h` | Async redirect completion callback |
| `KlCompress` | `compress.h` | Pluggable compression vtable: compress, feed, encoding, reset, destroy |
| `KlCompressCtx` | `compress.h` | Opaque per-server compression context (user-owned) |
| `KlCompressConfig` | `compress.h` | Compression config: ctx, factory, ctx_destroy |
| `KlCompressFactory` | `compress.h` | Factory: creates per-operation KlCompress from shared context |
| `KlCompressStream` | `compress.h` | Compressed streaming handle: comp, write_fn, write_ctx, res |
| `KlDecompress` | `decompress.h` | Pluggable decompression vtable: decompress, dfeed, encoding, reset, destroy |
| `KlDecompressConfig` | `decompress.h` | Decompression config: ctx, factory, ctx_destroy (shares KlCompressCtx) |
| `KlDecompressStream` | `decompress.h` | Decompression stream handle: decomp, alloc, error |
| `KlDrain` | `drain.h` | Backpressure write buffer: write_fn, buf, max_size, on_drain |
| `KlDrainWriteFn` | `drain.h` | Writer callback: `ssize_t (*)(const char *data, size_t len, void *ctx)` |
| `KlDrainCb` | `drain.h` | Drain callback: `void (*)(void *ctx)` |
| `KlFileIO` | `file_io.h` | Pluggable async file I/O vtable: submit, cancel, tick, destroy |
| `KlFileIOResult` | `file_io.h` | File I/O completion result: udata + bytes read |
| `KlResolverCacheConfig` | `resolver_cache.h` | Cache config: ttl_ms, capacity |
| `KlServerStats` | `server.h` | Read-only server load snapshot: active_connections, max_connections, async_suspended, listen_paused |

## Git

- When committing, do NOT add any Co-Authored-By trailers.
- Do NOT add "Generated with Claude Code" or similar attribution to PRs.

## Conventions

- C11, compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror`
- `-fstack-protector-strong` for buffer overflow detection
- No direct malloc/free — all allocation through `KlAllocator` interface
- All public functions prefixed with `kl_` (e.g. `kl_router_init`, `kl_response_json`)
- Header-only code in `request.h` uses `static inline`
- Vendor code compiled with `-w` (relaxed warnings, no `-Werror`)
- Integer overflow guards: check against `SIZE_MAX/2` or `INT_MAX/2` before arithmetic
- TLS wraps transport — all I/O goes through `conn_read`/`conn_write` helpers when TLS is active
- Error handling: return `-1` on failure, `0` on success (or positive value). Stateful structs store `KlError last_error` — set at the point of `return -1`, retrieve with `kl_strerror(err)`.
- Resource cleanup: every `_init` has a corresponding `_free`

## Body Reader Pattern

Body readers use a vtable interface:

```c
struct KlBodyReader {
    int  (*on_data)(KlBodyReader *self, const char *data, size_t len);
    void (*on_complete)(KlBodyReader *self);
    void (*on_error)(KlBodyReader *self);
    void (*destroy)(KlBodyReader *self);
};
```

Factory signature: `KlBodyReader *(*factory)(KlAllocator *alloc, KlRequest *req, void *user_data)`

- Factory is called after headers are fully parsed (header pointers still valid)
- `on_data` is called as body chunks arrive — return `-1` to abort (triggers 413)
- `on_complete` signals end of body — handler is invoked after this
- `on_error` is called on connection errors — clean up partial state
- `destroy` frees all reader resources

To add a new reader: implement the 4 vtable functions, write a factory, register per-route.

## Middleware Pattern

Middleware uses the `KlMiddleware` function signature:

```c
typedef int (*KlMiddleware)(KlRequest *req, KlResponse *res, void *user_data);
```

- Return `0` → continue to next middleware / handler
- Return non-zero → short-circuit (response must already be written)
- Patterns: `/*` = prefix match, `/exact` = exact match, `*` method = any method
- `req->ctx` (`void *`) enables middleware→handler data passing

### Two-Phase Middleware

Middleware runs in two phases:

```
headers → route match → init response → PRE-BODY middleware → ws/h2 → body reading → POST-BODY middleware → handler
```

**Pre-body** (`kl_server_use()` / `kl_router_use()`):
- Runs before body is read — ideal for rate limiting, CORS, auth via headers
- Short-circuit disables keep-alive (body may be unread)

**Post-body** (`kl_server_use_post()` / `kl_router_use_post()`):
- Runs after body is fully consumed — can access `req->body_reader` data
- Short-circuit preserves keep-alive (body already consumed)
- Use for middleware that needs form body access (e.g. CSRF token validation)

Built-in: `kl_cors_middleware` (pass `KlCorsConfig *` as user_data) — pre-body.

To add a new middleware: implement the `KlMiddleware` signature, register with `kl_server_use()` (pre-body) or `kl_server_use_post()` (post-body).

## Async Pattern

KEEL provides two async primitives for non-blocking operations without stalling the event loop.

### KlWatcher — Generic FD Callbacks

Register any file descriptor with the event loop. When the FD becomes ready, the callback fires on the event loop thread. Watchers operate on `KlEventCtx` (not `KlServer` directly).

```c
int  kl_watcher_add(KlEventCtx *ctx, int fd, KlEventMask mask, KlWatcherFn on_ready, void *user_data);
int  kl_watcher_mod(KlEventCtx *ctx, int fd, KlEventMask mask);
void kl_watcher_del(KlEventCtx *ctx, int fd);
```

Watchers are heap-allocated and ctx-owned. `KlServer` embeds `KlEventCtx ev` — use `&server->ev` when calling watcher functions from server context. Uses tagged pointers (LSB=1) to distinguish watcher events from connection events in the event loop dispatch.

### Dispatch Helpers

Two helpers eliminate the tagged-pointer boilerplate for client-side event loops:

```c
/* Dispatch a single event: returns 1 if watcher, 0 if not (static inline in event_ctx.h) */
int kl_event_dispatch(KlEventCtx *ctx, const KlEvent *event);

/* Run one tick: wait + dispatch all watchers. Stack-buffers ≤64 events (in async.c) */
int kl_event_ctx_run(KlEventCtx *ctx, int max_events, int timeout_ms);
```

**`kl_event_dispatch`** — unmasks the tagged pointer, calls `on_ready`, re-arms. Server uses this inline for mixed connection+watcher dispatch:
```c
for (int i = 0; i < n; i++) {
    if (kl_event_dispatch(&s->ev, &events[i])) continue;
    /* handle connection events ... */
}
```

**`kl_event_ctx_run`** — standalone event loop tick for clients and thread pools (all FDs are watcher-owned):
```c
while (!done) {
    if (kl_event_ctx_run(&ev, 16, 1000) < 0) break;
}
```

### KlAsyncOp — Connection Suspension

Suspend a connection for an async operation. The connection is removed from the event loop and exempt from idle timeouts while suspended.

```c
int  kl_async_suspend(KlServer *s, KlConn *conn, KlAsyncOp *op);
void kl_async_complete(KlServer *s, KlAsyncOp *op);
```

Three callbacks per op (separate because deadline semantics differ per use case):
- `on_resume` — called by `kl_async_complete`, handler sets response and state
- `on_deadline` — called when `deadline_ms` reached (sleep = success, HTTP = timeout)
- `on_cancel` — called if connection dies while suspended

**Important**: `kl_async_complete()` is NOT thread-safe — it manipulates the ops linked list and calls `kl_event_add`. Always call it from the event loop thread (e.g. from a watcher callback or the thread pool's `done_fn`).

### Typical async flow

```c
void handler(KlRequest *req, KlResponse *res, void *user_data) {
    KlServer *srv = user_data;
    KlConn *conn = kl_request_conn(req);

    /* Set up async op (caller-owned, must remain valid until completion) */
    MyCtx *ctx = ...;
    ctx->op.on_resume = my_resume;
    ctx->op.on_cancel = my_cancel;

    /* Create a pipe/socket for completion signal */
    socketpair(AF_UNIX, SOCK_STREAM, 0, ctx->pipe_fds);
    kl_watcher_add(&srv->ev, ctx->pipe_fds[0], KL_EVENT_READ, my_watcher, ctx);

    /* Suspend the connection */
    kl_async_suspend(srv, conn, &ctx->op);

    /* Trigger completion later (e.g. from another thread via pipe write) */
}
```

## Thread Pool Pattern

`KlThreadPool` bridges blocking work (SQLite, file I/O, DNS, crypto) and the event loop. Submit work from the event loop, execute on a worker thread, signal completion back via a pipe + `KlWatcher`.

### Architecture

```
Event loop thread                    Worker threads (N)
─────────────────                    ─────────────────
kl_thread_pool_submit()
  → push to work_queue               pthread_cond_wait
  → pthread_cond_signal ──────────→  wake, pop work item
                                     execute work_fn(user_data)
                                     push to done_queue
kl_event_wait fires                  write(pipe_wr, "1") ←──────┘
  → watcher on pipe_rd
  → drain done_queue
  → call done_fn(user_data) on event loop thread
```

### API

```c
KlThreadPool *kl_thread_pool_create(KlEventCtx *ctx, const KlThreadPoolConfig *cfg);
int            kl_thread_pool_submit(KlThreadPool *pool, const KlWorkItem *item);
void           kl_thread_pool_free(KlThreadPool *pool);
```

### Three Callbacks

| Callback | Thread | When | Purpose |
|----------|--------|------|---------|
| `work_fn` | Worker | Item dequeued | Execute blocking work (SQLite, I/O) |
| `done_fn` | Event loop | Pipe watcher fires | Resume connection via `kl_async_complete` |
| `cancel_fn` | Event loop | `kl_thread_pool_free` drains remaining items | Free resources for items that never ran (may be NULL) |

### Thread Safety

Thread safety is guaranteed by construction:
- `work_fn` runs on a worker thread with no lock held
- `done_fn` runs on the event loop thread (pipe watcher callback) — safe to call `kl_async_complete`
- Workers never call `kl_async_complete` directly — they push to the done queue and write a byte to the pipe
- All queue access is protected by a single mutex

### Backpressure

`submit()` returns `-1` when `inflight >= done_cap` (where `done_cap = queue_capacity + num_workers`). The `inflight` counter tracks items across all three stages (work queue + executing + done queue), preventing done queue overflow.

### Shutdown Sequence

`kl_thread_pool_free()`:
1. Set `shutdown = 1`, broadcast `work_avail`
2. `pthread_join` all workers (waits for in-flight work to complete)
3. Remove pipe watcher from event loop
4. Drain done queue → call `done_fn` for each
5. Drain work queue → call `cancel_fn` for each (if non-NULL)
6. Close pipe, destroy mutex/condvar, free all memory

### Usage with KlAsyncOp

```c
typedef struct {
    KlAsyncOp op;
    KlServer *server;
    /* ... blocking work context ... */
} MyWork;

static void my_work_fn(void *ud) {
    MyWork *w = ud;
    /* runs on worker thread — do blocking I/O here */
}

static void my_done_fn(void *ud) {
    MyWork *w = ud;
    /* runs on event loop thread — safe to resume connection */
    kl_async_complete(w->server, &w->op);
}

/* In handler: */
kl_async_suspend(srv, conn, &work->op);
KlWorkItem item = { .work_fn = my_work_fn, .done_fn = my_done_fn, .user_data = work };
kl_thread_pool_submit(pool, &item);
```

## Testing

Tests use Sheredom's utest.h (single-header, `vendor/utest.h`). Each `tests/test_*.c` file is compiled as a standalone executable.

```bash
make test                           # run all tests
make debug && make test             # run under ASan + UBSan
./tests/test_body_reader            # run a single test suite
```

Test files are auto-discovered via `$(wildcard tests/test_*.c)` in the Makefile.

Test naming: `UTEST(suite, test_name)` — e.g. `UTEST(mp, boundary_spanning)`.

## Common Patterns

- **Error handling**: Functions return `int` — negative on error, 0 or positive on success
- **Resource cleanup**: Always pair `_init`/`_free`. Response has `_reset` for keep-alive reuse.
- **Overflow guards**: Before `a + b`, check `a > SIZE_MAX/2 || b > SIZE_MAX/2`. Before `n * size`, check `n > SIZE_MAX / size`.
- **Header access**: Use `kl_request_header(req, "Content-Type")` — case-insensitive, returns null-terminated value or NULL if missing
- **Body access**: Cast `req->body_reader` to the concrete type (`KlBufReader *`, `KlMultipartReader *`)

## Debugging

```bash
make debug          # clean + rebuild with -fsanitize=address,undefined -g -O0
make test           # run tests under sanitizers
```

ASan catches: heap/stack buffer overflow, use-after-free, double-free, memory leaks.
UBSan catches: signed overflow, null dereference, misaligned access, shift overflow.

## Static Analysis

```bash
make analyze        # Clang static analyzer via scan-build (--status-bugs fails on warnings)
make cppcheck       # cppcheck with --enable=all (--error-exitcode=1 fails on issues)
```

Both targets should exit cleanly with no findings before merging.

## Fuzz Testing

Two libFuzzer targets cover the primary attack surface (untrusted network input):

```bash
# Requires clang with libFuzzer support
# Linux:  make fuzz CC=clang
# macOS:  make fuzz CC=/opt/homebrew/opt/llvm@18/bin/clang

# Run a fuzzer (Ctrl-C to stop):
./fuzz/fuzz_parser fuzz/corpus_parser/       # HTTP parser + chunked decoder
./fuzz/fuzz_multipart fuzz/corpus_multipart/ # multipart/form-data parser
```

Fuzz targets are built with ASan + UBSan enabled. Corpus files in `fuzz/corpus_*/` are seed inputs — crashes found by the fuzzer are saved to the corpus automatically.
