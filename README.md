# KEEL — Kernel Event Engine, Lightweight

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Minimal C11 HTTP client/server library built on raw epoll/kqueue/io_uring/poll. Both the server and client support sync and async operation — sync handlers return immediately, async handlers suspend and resume via the event loop; the client offers both a blocking API and an event-driven API. Pluggable allocator, pluggable HTTP parser, pluggable TLS, pluggable body readers, per-route middleware, streaming responses, multipart uploads, connection timeouts, thread pool, zero forced buffering.

**101K req/s** on a single thread. **400+ tests** with ASan/UBSan. **One vendored dependency** (llhttp).

## Build

```bash
make                    # build libkeel.a (epoll on Linux, kqueue on macOS)
make BACKEND=iouring    # build with io_uring backend (Linux 5.6+, requires liburing-dev)
make BACKEND=poll       # build with poll() backend (universal POSIX fallback)
make CC=cosmocc         # build with Cosmopolitan C (Actually Portable Executable)
make test               # run unit tests
make examples           # build all example programs
make debug              # debug build with ASan + UBSan
make analyze            # Clang static analyzer (scan-build)
make cppcheck           # cppcheck static analysis
make fuzz               # build libFuzzer fuzz targets (requires clang)
make clean              # remove artifacts
```

## Hello World

```c
#include <keel/keel.h>

void handle_hello(KlRequest *req, KlResponse *res, void *ctx) {
    (void)req; (void)ctx;
    kl_response_json(res, 200, "{\"msg\":\"hello\"}", 15);
}

int main(void) {
    KlServer s;
    KlConfig cfg = {.port = 8080};
    kl_server_init(&s, &cfg);
    kl_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);
    kl_server_run(&s);
    kl_server_free(&s);
}
```

## Features

- **Four event loop backends** — epoll (edge-triggered), kqueue (edge-triggered), io_uring (POLL_ADD), poll (universal POSIX fallback)
- **Pluggable HTTP parser** — ships with llhttp, swap via `KlConfig.parser`
- **Pluggable TLS** — bring your own BearSSL/LibreSSL/OpenSSL via vtable, zero vendored TLS code
- **Pluggable body readers** — vtable interface for request body processing
- **Per-route middleware** — pattern-matched middleware chain with short-circuit support
- **Built-in CORS middleware** — configurable origins, methods, headers, preflight handling
- **Multipart form-data** — RFC 2046 parser with configurable size limits
- **Three response modes** — buffered (writev), file (sendfile zero-copy), stream (chunked transfer encoding)
- **WebSocket** — RFC 6455 server + client with frame encoding/decoding
- **Route parameters** — `:param` capture, no allocation, pointers into read buffer
- **Connection timeouts** — monotonic clock sweep, automatic 408 responses, slow-loris protection
- **Access logging** — pluggable callback after each response, zero overhead when disabled
- **Async server handlers** — suspend connections via `KlAsyncOp`, resume later from watchers or thread pool workers — no stalling the event loop
- **HTTP client (sync + async)** — blocking `kl_client_request()` for simple use cases, event-driven `kl_client_start()` for non-blocking I/O, both with TLS support
- **Composable event context** — `KlEventCtx` decouples the event loop from the server, enabling standalone clients and thread pools without a `KlServer`
- **Thread pool** — submit blocking work from event loop, execute on workers, resume via pipe wakeup
- **Generic FD watchers** — register any file descriptor for event-driven callbacks via `KlWatcher`
- **Pre-allocated connection pool** — no per-request malloc, no fragmentation under load
- **Pluggable allocator** — bring your own arena/pool/tracking allocator
- **pledge/unveil sandboxing** — init/run split makes syscall lockdown natural
- **Zero-copy techniques** — header pointers into read buffer, sendfile, writev batching

## Architecture

21 orthogonal modules, each independently testable:

| Module | Header | Description |
|--------|--------|-------------|
| **allocator** | `allocator.h` | Bring-your-own allocator interface |
| **event** | `event.h` | epoll / kqueue / io_uring / poll abstraction |
| **event_ctx** | `event_ctx.h` | Composable event loop context (watchers + allocator) |
| **request** | `request.h` | Parsed HTTP request struct (header-only, zero alloc) |
| **parser** | `parser.h` | Pluggable request/response parser vtables |
| **response** | `response.h` | Response builder: buffered, sendfile, or streaming chunked |
| **router** | `router.h` | Route matching with `:param` capture + middleware chain |
| **connection** | `connection.h` | Pre-allocated connection pool + state machine |
| **server** | `server.h` | Top-level glue: init, bind, async event loop, stop |
| **body_reader** | `body_reader.h` | Pluggable body reader vtable + buffer reader |
| **body_reader_multipart** | `body_reader_multipart.h` | RFC 2046 multipart/form-data parser |
| **chunked** | `chunked.h` | Parser-agnostic chunked transfer-encoding decoder |
| **cors** | `cors.h` | Built-in CORS middleware with configurable origins |
| **websocket** | `websocket.h` + `websocket_server.h` | RFC 6455 WebSocket server support (shared frame parser + server API) |
| **websocket_client** | `websocket_client.h` | RFC 6455 WebSocket client (masked frames, async handshake) |
| **tls** | `tls.h` | Pluggable TLS transport vtable (bring-your-own backend) |
| **h2** | `h2.h` + `h2_server.h` | HTTP/2 server (pluggable session vtable) |
| **h2_client** | `h2_client.h` | HTTP/2 client (multiplexed streams, pluggable session) |
| **async** | `async.h` | Connection suspension for async operations |
| **thread_pool** | `thread_pool.h` | Worker thread pool with pipe-based event loop wakeup |
| **url** | `url.h` | URL parser (http/https/ws/wss, IPv6, CRLF injection guard) |
| **client** | `client.h` | HTTP/1.1 client (sync blocking + async event-driven) |

**Deliberate design choices:**

- **Single-threaded event loop** — Same model as Node.js, Redis, Nginx (per-worker), and Python asyncio. No mutexes, no lock contention, no data races — the entire connection state machine is lock-free by construction. `KlThreadPool` offloads blocking work to workers; multi-core scaling is horizontal via `SO_REUSEPORT` with multiple processes.
- **O(n) router** — Linear scan over routes per request. A `memcmp` scan over even hundreds of routes costs nanoseconds, invisible next to network I/O syscalls. A trie or radix tree would add complexity to param extraction and middleware pattern matching for no measurable gain.
- **O(n) timeout sweep** — Iterates all connection slots once per event loop tick. At `max_connections` = 256 (default), this is a tight loop over a contiguous array well within L1 cache.

## Request Body Handling

KEEL uses a vtable-based body reader interface. Register a body reader factory per-route — the connection layer creates the reader after headers are parsed, feeds it data as it arrives, and makes the finished reader available in the handler via `req->body_reader`.

**Built-in buffer reader** — accumulates the body into a growable buffer:

```c
void handle_post(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlBufReader *br = (KlBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        kl_response_error(res, 400, "Request body required");
        return;
    }
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "application/octet-stream");
    kl_response_body_borrow(res, br->data, br->len);
}

/* Register with size limit (1 MB) */
kl_server_route(&s, "POST", "/api/data", handle_post,
                (void *)(size_t)(1 << 20), kl_body_reader_buffer);
```

Pass `NULL` as the body reader factory for routes that don't accept a body. If a request with a body arrives on a route with no reader, KEEL discards the body. If the reader factory returns NULL, KEEL sends 415 Unsupported Media Type.

**Custom readers** — implement the `KlBodyReader` vtable (`on_data`, `on_complete`, `on_error`, `destroy`) and provide a factory function.

## Middleware

Register middleware that runs before handlers. Middleware can inspect/modify the request and response, or short-circuit the chain by returning a non-zero value (e.g., to reject unauthenticated requests).

### Built-in CORS middleware

```c
KlCorsConfig cors;
kl_cors_init(&cors);
kl_cors_add_origin(&cors, "https://app.example.com");
kl_cors_add_origin(&cors, "https://staging.example.com");
// Or parse from an environment variable:
// kl_cors_parse_origins(&cors, getenv("ALLOWED_ORIGINS"));

kl_server_use(&s, "*", "/*", kl_cors_middleware, &cors);
```

Handles `Access-Control-Allow-Origin`, `Allow-Credentials`, and automatically responds to OPTIONS preflight requests with 204 + all required CORS headers.

### Writing custom middleware

Middleware uses the same `(KlRequest *, KlResponse *, void *)` signature. Return 0 to continue, non-zero to short-circuit:

**Logging middleware:**

```c
int log_middleware(KlRequest *req, KlResponse *res, void *ctx) {
    (void)res; (void)ctx;
    fprintf(stderr, "[req] %.*s %.*s\n",
            (int)req->method_len, req->method,
            (int)req->path_len, req->path);
    return 0;  /* continue to next middleware / handler */
}

kl_server_use(&s, "*", "/*", log_middleware, NULL);
```

**Auth middleware:**

```c
typedef struct { const char *api_key; } AuthConfig;

int auth_middleware(KlRequest *req, KlResponse *res, void *ctx) {
    AuthConfig *cfg = ctx;
    size_t key_len;
    const char *key = kl_request_header_len(req, "X-API-Key", &key_len);
    size_t expect_len = strlen(cfg->api_key);
    if (!key || key_len != expect_len ||
        memcmp(key, cfg->api_key, expect_len) != 0) {
        kl_response_error(res, 401, "Unauthorized");
        return 1;  /* short-circuit — response already written */
    }
    return 0;  /* continue */
}

AuthConfig auth = {.api_key = "secret-key-123"};
kl_server_use(&s, "*", "/api/*", auth_middleware, &auth);
```

**Request context passing (middleware → handler):**

```c
int user_middleware(KlRequest *req, KlResponse *res, void *ctx) {
    (void)res; (void)ctx;
    /* Validate token, look up user, set context for handler */
    req->ctx = my_user_lookup(req);
    return 0;
}

void handle_profile(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    User *user = req->ctx;  /* set by middleware */
    /* ... */
}
```

### Middleware patterns

- Patterns ending with `/*` are prefix matches: `/api/*` matches `/api`, `/api/users`, `/api/users/123`
- Patterns without `/*` are exact matches: `/health` matches only `/health`
- Method `"*"` matches any HTTP method; `"GET"` also matches HEAD requests
- Middleware runs in registration order, before body reading
- Short-circuiting disables keep-alive (unread body can't be drained)

## Multipart Uploads

```c
static KlMultipartConfig mp_config = {
    .max_part_size  = 4 << 20,   /* 4 MB per part */
    .max_total_size = 16 << 20,  /* 16 MB total */
    .max_parts      = 8,
};

void handle_upload(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    KlMultipartReader *mr = (KlMultipartReader *)req->body_reader;
    if (!mr || mr->num_parts == 0) {
        kl_response_error(res, 400, "No parts received");
        return;
    }
    for (int i = 0; i < mr->num_parts; i++) {
        KlMultipartPart *p = &mr->parts[i];
        printf("  %s: %zu bytes (filename=%s)\n",
               p->name, p->data_len, p->filename ? p->filename : "n/a");
    }
    kl_response_json(res, 200, "{\"ok\":true}", 11);
}

kl_server_route(&s, "POST", "/upload", handle_upload,
                &mp_config, kl_body_reader_multipart);
```

```bash
curl -F "name=Alice" -F "file=@photo.jpg" localhost:8080/upload
```

## WebSocket

Register a WebSocket endpoint and get bidirectional communication:

```c
static int ws_on_message(KlWsServerConn *ws, KlWsOpcode opcode,
                         const char *data, size_t len, void *ctx) {
    (void)ctx;
    if (opcode == KL_WS_TEXT) {
        kl_ws_server_send_text(ws, data, len);  /* echo back */
    }
    return 0;
}

static void ws_on_close(KlWsServerConn *ws, void *ctx) {
    (void)ws; (void)ctx;
    printf("WebSocket closed\n");
}

static KlWsServerConfig ws_config = {
    .on_message = ws_on_message,
    .on_close = ws_on_close,
};

int main(void) {
    KlServer s;
    KlConfig cfg = {.port = 8080};
    kl_server_init(&s, &cfg);
    kl_server_ws(&s, "/ws", &ws_config);
    kl_server_run(&s);
}
```

The WebSocket server module handles frame parsing, masking, and protocol details. The handler receives callbacks for each message — use `kl_ws_server_send_text()` or `kl_ws_server_send_binary()` to reply.

## Async Operations

Server handlers can be **sync** (return immediately with a response set) or **async** (suspend the connection for later resumption). KEEL provides two primitives for async handlers: **KlWatcher** (generic FD callbacks) and **KlAsyncOp** (connection suspension). Together they allow handlers to park a connection, perform work asynchronously, and resume when done — without stalling the event loop.

```c
void handle_async(KlRequest *req, KlResponse *res, void *user_data) {
    KlServer *srv = user_data;
    KlConn *conn = kl_request_conn(req);

    /* Allocate context for the async operation */
    MyCtx *ctx = ...;
    ctx->op.on_resume = my_resume_cb;
    ctx->op.on_cancel = my_cancel_cb;

    /* Create a pipe — watcher fires when the pipe is written to */
    socketpair(AF_UNIX, SOCK_STREAM, 0, ctx->pipe_fds);
    kl_watcher_add(&srv->ev, ctx->pipe_fds[0], KL_EVENT_READ, my_watcher, ctx);

    /* Suspend the connection (removes it from event loop, exempt from timeouts) */
    kl_async_suspend(srv, conn, &ctx->op);

    /* Later: write to pipe → watcher fires → kl_async_complete → connection resumes */
}
```

The watcher callback runs on the event loop thread, making it safe to call `kl_async_complete()` which re-registers the connection FD and drives the state machine forward.

## Thread Pool

`KlThreadPool` bridges blocking work (SQLite queries, file I/O, DNS, crypto) and the single-threaded event loop. Submit work items from the event loop, execute on worker threads, resume connections via pipe wakeup.

```c
/* Create pool (auto-detects CPU count, 64-item queue) */
KlThreadPool *pool = kl_thread_pool_create(&server.ev, NULL);

/* Work callbacks */
static void do_query(void *ud) {
    MyWork *w = ud;
    w->status = sqlite3_exec(w->db, w->sql, ...);  /* runs on worker thread */
}
static void query_done(void *ud) {
    MyWork *w = ud;
    kl_async_complete(w->server, &w->op);           /* runs on event loop thread */
}

/* In handler: suspend connection, submit blocking work */
kl_async_suspend(srv, conn, &work->op);
KlWorkItem item = { .work_fn = do_query, .done_fn = query_done, .user_data = work };
kl_thread_pool_submit(pool, &item);
```

Each `KlWorkItem` has three callbacks:

| Callback | Thread | Purpose |
|----------|--------|---------|
| `work_fn` | Worker | Execute blocking work |
| `done_fn` | Event loop | Resume connection (called via pipe watcher) |
| `cancel_fn` | Event loop | Cleanup for items still queued at shutdown (may be NULL) |

Thread safety is guaranteed by construction — workers never touch the event loop directly. They push completed items to a done queue and write a byte to a pipe; the pipe watcher fires on the event loop thread and calls `done_fn`. Backpressure: `submit()` returns `-1` when the queue is full.

## HTTP Client

KEEL includes both **sync** (blocking) and **async** (event-driven) HTTP/1.1 clients with TLS support. The sync client is a single function call for simple use cases. The async client uses `KlEventCtx` (not `KlServer`), so it works standalone — no server required.

**Sync (blocking):**

```c
KlAllocator alloc = kl_allocator_default();
KlClientResponse resp;
if (kl_client_request(&alloc, NULL, "GET", "http://api.example.com/data",
                       NULL, 0, NULL, 0, &resp) == 0) {
    printf("status=%d body=%.*s\n", resp.status, (int)resp.body_len, resp.body);
    kl_client_response_free(&resp);
}
```

**Async (event-driven):**

```c
static void on_done(KlClient *client, void *user_data) {
    if (kl_client_error(client) == 0) {
        const KlClientResponse *r = kl_client_response(client);
        printf("status=%d\n", r->status);
    }
    kl_client_free(client);
}

/* Works with standalone KlEventCtx — no KlServer needed */
KlAllocator alloc = kl_allocator_default();
KlEventCtx ev;
kl_event_ctx_init(&ev, &alloc);
kl_client_start(&ev, &alloc, NULL, "GET", "http://example.com/", NULL, 0, NULL, 0, on_done, NULL);
/* pump ev.loop ... */
kl_event_ctx_free(&ev);
```

The URL parser (`kl_url_parse`) handles `http://`, `https://`, `ws://`, `wss://`, IPv6 `[::1]:port`, default ports, and rejects CRLF injection.

## Static File Serving

Zero-copy file responses via `sendfile(2)`:

```c
void handle_static(KlRequest *req, KlResponse *res, void *ctx) {
    (void)ctx;
    if (memmem(req->path, req->path_len, "..", 2) != NULL) {
        kl_response_error(res, 403, "Forbidden");
        return;
    }
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "./public%.*s",
             (int)req->path_len, req->path);
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) { kl_response_error(res, 404, "Not Found"); return; }
    struct stat st;
    fstat(fd, &st);
    kl_response_status(res, 200);
    kl_response_header(res, "Content-Type", "text/html");
    kl_response_file(res, fd, st.st_size);  /* zero-copy sendfile */
}
```

Uses `sendfile(2)` on Linux and macOS, with TCP_CORK coalescing on Linux for optimal throughput.

## Streaming Responses

Write directly to the socket via chunked transfer encoding — zero intermediate buffering:

```c
void handle_stream(KlRequest *req, KlResponse *res, void *ctx) {
    kl_response_header(res, "Content-Type", "application/json");

    KlWriteFn write_fn;
    void *write_ctx;
    kl_response_begin_stream(res, 200, &write_fn, &write_ctx);

    write_fn(write_ctx, "{\"data\":", 8);
    // ... write as much as you want, each call becomes a chunk ...
    write_fn(write_ctx, "}", 1);

    kl_response_end_stream(res);
}
```

The `KlWriteFn` signature (`int (*)(void *ctx, const char *data, size_t len)`) is designed to be compatible with streaming JSON writers.

## Route Parameters

```c
kl_server_route(&s, "GET", "/users/:id/posts/:pid", handler, NULL, NULL);
// Params extracted from path — no allocation, pointers into read buffer
```

The router returns 200 (match), 405 (path matched, wrong method), or 404 (not found).

## Connection Timeouts

```c
KlConfig cfg = {
    .port = 8080,
    .read_timeout_ms = 15000,   /* 15 seconds (default: 30000) */
};
```

KEEL stamps each connection with a monotonic clock on every I/O event. A periodic sweep (every ~400ms) closes connections that have been idle longer than `read_timeout_ms` and sends a 408 Request Timeout response. This protects against slow-loris attacks and abandoned connections without affecting active transfers.

## Access Logging

```c
void my_logger(const KlRequest *req, int status,
               size_t body_bytes, double duration_ms, void *user_data) {
    fprintf(stderr, "%.*s %.*s %d %zu %.1fms\n",
            (int)req->method_len, req->method,
            (int)req->path_len, req->path,
            status, body_bytes, duration_ms);
}

KlConfig cfg = {
    .port = 8080,
    .access_log = my_logger,       /* NULL = disabled (default) */
    .access_log_data = NULL,       /* passed as user_data */
};
```

Set a callback in `KlConfig` and KEEL calls it after each response is fully sent. The callback receives the full request (method, path, headers), response status, body size, and wall-clock duration in milliseconds. Users implement their own formatting (JSON, CLF, custom). `NULL` = no logging, zero overhead.

## Custom Allocator

```c
KlAllocator arena = my_arena_allocator();
KlConfig cfg = {
    .port = 8080,
    .alloc = &arena,
};
```

The allocator interface passes `size` to `free` and `old_size` to `realloc` — enabling arena and pool allocators that don't store per-allocation metadata.

## Pluggable Parser

Ships with llhttp (default). Swap by setting `KlConfig.parser`:

```c
KlConfig cfg = {
    .port = 8080,
    .parser = kl_parser_pico,  // use picohttpparser instead
};
```

Implement the 3-function `KlRequestParser` vtable (`parse`, `reset`, `destroy`) for any backend. The response parser (`KlResponseParser`) uses the same pattern for the HTTP client.

## Pluggable TLS

KEEL doesn't vendor any TLS library. Bring your own backend (BearSSL, LibreSSL, OpenSSL, rustls-ffi) by implementing the 7-function `KlTls` vtable:

```c
KlTlsCtx *ctx = my_bearssl_ctx_new("cert.pem", "key.pem");
KlTlsConfig tls = {
    .ctx = ctx,
    .factory = my_bearssl_factory,
    .ctx_destroy = my_bearssl_ctx_free,
};
KlConfig cfg = {
    .port = 8443,
    .tls = &tls,
};
```

The vtable interface (`handshake`, `read`, `write`, `shutdown`, `pending`, `reset`, `destroy`) wraps the transport layer. Everything above it — parser, router, middleware, body readers, handlers — works identically on plaintext and TLS connections.

When TLS is active, `sendfile(2)` falls back to `pread` + TLS write (encryption requires userspace access to plaintext). All other response modes (buffered, streaming) work transparently.

## Sandboxing with pledge/unveil

KEEL deliberately does **not** own your sandbox policy — that's an application concern. The server separates initialization (bind/listen) from the event loop (accept/read/write), so you can lock down syscalls and filesystem access between the two:

```c
#include <keel/keel.h>

int main(void) {
    KlServer s;
    KlConfig cfg = {.port = 8080};
    kl_server_init(&s, &cfg);    // binds socket — needs inet, rpath
    kl_server_route(&s, "GET", "/hello", handle_hello, NULL, NULL);

    // --- Sandbox boundary ---
    unveil("/var/www", "r");     // only serve files from here
    unveil(NULL, NULL);          // lock it down
    pledge("stdio inet rpath", NULL);

    kl_server_run(&s);           // enters event loop — sandboxed
    kl_server_free(&s);
}
```

On Linux, use the [pledge polyfill](https://github.com/jart/pledge) (seccomp-bpf + Landlock) for the same API. The key insight: KEEL's `init`/`run` split makes this natural — no library changes needed.

## Benchmark

```bash
./bench.sh              # automated: build, start server, warmup, wrk benchmark
```

Manual:

```bash
make examples
./examples/hello_server &
wrk -t4 -c100 -d10s http://localhost:8080/hello
kill %1
```

Measured on a single thread, single core (Apple M-series):

| Metric | Value |
|--------|-------|
| Requests/sec | ~101,000 |
| Avg latency | ~0.98ms |
| Connections | 100 concurrent |
| Transfer | ~13 MB/s |

No GC pauses. No goroutine scheduling. No async runtime overhead. Just `epoll_wait` → `read` → `write`.

## Platform Support

| Platform | Backend | Build |
|----------|---------|-------|
| macOS / BSD | kqueue (edge-triggered) | `make` |
| Linux | epoll (edge-triggered) | `make` |
| Linux 5.6+ | io_uring (POLL_ADD) | `make BACKEND=iouring` |
| Any POSIX | poll (level-triggered) | `make BACKEND=poll` |
| Linux (musl/Alpine) | epoll (edge-triggered) | `make` |
| Cosmopolitan (APE) | poll (auto-selected) | `make CC=cosmocc` |

The io_uring backend uses `IORING_OP_POLL_ADD` for readiness notification — a drop-in replacement for epoll with io_uring's batched submission advantage. Requires `liburing-dev`.

The poll backend is a universal POSIX fallback that works on any platform with `poll(2)`. It enables Cosmopolitan C support (Actually Portable Executables that run on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from a single binary). When `CC=cosmocc` is detected, the Makefile automatically selects the poll backend.

## Testing

408 tests across 23 test suites, covering every module:

| Suite | Tests | Covers |
|-------|-------|--------|
| `test_allocator` | 4 | Default + custom tracking allocators |
| `test_router` | 27 | Exact match, params, 404, 405, wildcard, middleware chain |
| `test_response` | 14 | Status, headers, body, JSON, error, streaming, sendfile |
| `test_parser` | 10 | GET, POST, query strings, incomplete, reset, chunked TE |
| `test_response_parser` | 9 | HTTP response parsing, chunked, headers, body limits, malformed |
| `test_connection` | 9 | Pool init, acquire/release, exhaustion, active count, state machine, monotonic clock |
| `test_body_reader` | 30 | Buffer + multipart: limits, spanning, binary, edge cases |
| `test_chunked` | 17 | Chunked decoder: single/multi chunk, hex, extensions, trailers, errors |
| `test_cors` | 17 | Config, origin whitelist, wildcard, preflight, credentials, middleware |
| `test_websocket` | 42 | Frame parsing, masking, opcode, fragments, close, echo, send frame encoding |
| `test_websocket_client` | 28 | Client frame encoding, mask XOR, handshake, parser, API, config |
| `test_h2` | 29 | HTTP/2 sessions, streams, routing, ALPN, goaway |
| `test_h2_client` | 18 | Mock session vtable, stream tracking, response free, API validation |
| `test_integration` | 27 | Full server: hello, POST, keepalive, multipart, chunked, middleware |
| `test_timeout` | 8 | Idle, partial headers, partial body, active connections, body timeout, keepalive idle, concurrent |
| `test_tls` | 20 | TLS vtable, handshake FSM, response send/stream/file via mock, shutdown retry, pool teardown |
| `test_async` | 15 | Watchers (KlEventCtx), suspend/resume, deadlines, cancel, e2e async handler |
| `test_thread_pool` | 12 | Create/free, submit, backpressure, FIFO ordering, multi-worker, shutdown, stress |
| `test_url` | 20 | URL parsing, IPv6, CRLF rejection, default ports, ws/wss schemes |
| `test_client` | 20 | Sync/async client, response free, TLS config, error handling |
| `test_event_ctx` | 4 | Standalone event context init/free, watcher lifecycle |
| `test_event` | 8 | Event loop init/close, add/wait, del, multiple FDs, timeout, mod mask |
| `test_request` | 10 | Header case-insensitive lookup, params, query strings, empty/missing values |

```bash
make test               # run all tests
make debug && make test  # run under ASan + UBSan
make analyze            # Clang static analyzer
make cppcheck           # cppcheck static analysis
```

## Why C

**The attack surface is the network.** An HTTP server processes untrusted bytes from the internet through a parser (llhttp) into application handlers. We address this with defense-in-depth rather than language-level guarantees:

- `pledge()`/`unveil()` sandboxing — lock down syscalls and filesystem after binding the socket, before entering the event loop
- `-D_FORTIFY_SOURCE=2 -fstack-protector-strong` — compile-time and runtime buffer overflow detection
- AddressSanitizer + UBSan in debug builds
- Clang static analyzer (`make analyze`) and cppcheck (`make cppcheck`) for compile-time bug detection
- libFuzzer fuzz testing (`make fuzz`) on the HTTP parser and multipart parser — the primary attack surface
- Pre-allocated connection pool — no per-request `malloc`, no fragmentation, no OOM under load
- All inputs bounds-checked at system boundaries (read buffer limits, header count limits)
- Integer overflow guards (`SIZE_MAX/2`, `INT_MAX/2` checks) on all arithmetic
- Pluggable allocator — swap in an arena allocator per-request for deterministic cleanup

**Why not Rust?** Rust's safety guarantees are real, and for a large-team, high-churn codebase they pay off. For a small, focused library with 7 source files and one or two authors:

- *The hot path is FFI.* The HTTP parser is llhttp — a C library. Every request crosses an `unsafe` boundary. You get Rust's borrow checker overhead without Rust's safety guarantees where the actual parsing happens.
- *Self-referential request structs.* `KlRequest` holds pointers into the connection's read buffer. This is one line of C; in Rust it's a lifetime annotation project or a `Pin<Box<>>` adventure.
- *Zero-copy response streaming.* The write callback passes raw `(ctx, data, len)` through to `write(2)`. No intermediate `Vec<u8>`, no `String`, no `Arc<Mutex<>>`. The streaming interface is 3 lines of C. In Rust, safely sharing the socket fd between the response builder and the caller's streaming writer requires careful lifetime management that adds complexity without adding safety — the fd is valid for the connection's lifetime, period.
- *Cargo supply chain.* A Rust HTTP server pulls tokio, hyper, http, bytes, pin-project, mio — 100+ transitive crates. KEEL vendors exactly one dependency (llhttp, 4 files you can read in an afternoon).
- *Build time.* Clean build: under 2 seconds. A comparable Rust project: 30–90 seconds.

**Why not Go?** Go's goroutine-per-connection model is elegant but fundamentally different. KEEL's single-threaded event loop with edge-triggered epoll/kqueue gives predictable latency and zero GC pauses. Go's GC alone can exceed the per-request budget of a sub-microsecond JSON response. No manual memory layout control, no zero-copy sendfile, no pluggable allocator.

**Why not C++?** Everything C gives you here but with a language that fights simplicity. The connection state machine is a clean `enum` + `switch`. In C++ someone would reach for `std::variant<State1, State2, ...>` with `std::visit` or a template-based state machine library. The router is a flat array scan with `memcmp`. In C++ it becomes `std::unordered_map<std::string, std::function<void(...)>>` with heap allocations on every lookup. Both objectively worse for this domain.

**Why not Zig?** Zig's explicit allocators and `comptime` are genuinely appealing — the allocator interface in KEEL is essentially Zig's `std.mem.Allocator` in C. For a new project not needing battle-tested HTTP parsing, Zig would be a strong choice. But llhttp doesn't have a Zig-native equivalent yet, and Zig's ecosystem for production networking (TLS, HTTP/2) isn't there.

**Why not C# / Java / Python?** Managed runtimes are the wrong abstraction for a library that needs to control every byte and syscall. KEEL's connection pool is a flat array with zero per-request allocation. Its response path goes straight from a user buffer to `writev(2)` or `sendfile(2)` — no managed heap, no GC, no object headers. A C# `Span<byte>` or Java `ByteBuffer` gets close but still lives inside a runtime that owns your threads, your memory layout, and your syscall surface. Python is ~100x slower for raw I/O dispatch and adds a GIL. These languages are excellent for application code *on top* of an HTTP server — they're the wrong tool for building the server itself.

## Not in Scope

KEEL is a transport library — it handles sockets, parsing, routing, and response serialization. Everything above the HTTP layer is an application concern:

- **Authentication / authorization** — Token validation, session management, OAuth flows, RBAC. These are policy decisions that vary per application. KEEL's middleware interface makes auth trivial to implement (see [Auth middleware example](#writing-custom-middleware)) but deliberately doesn't ship one. *[Hull](https://github.com/artalis-io/hull) provides session, JWT, and RBAC middleware.*

- **Request logging** — Log format (JSON, CLF, custom), destination (stderr, file, syslog), and filtering are application choices. KEEL provides a pluggable `access_log` callback with method, path, status, body size, and duration — you bring the formatter. *Hull provides a structured JSON logger middleware.*

- **Request IDs / correlation IDs** — These are application-generated identifiers for tracing requests through your system. Use middleware to read/generate X-Request-ID headers and pass them to your logging/observability. *Hull generates and propagates request IDs automatically.*

- **Rate limiting** — Rate limits depend on your authentication layer, your billing tiers, your abuse patterns. Implement as middleware with whatever backing store (in-memory, Redis, database) fits your architecture. *Hull provides configurable rate limiting middleware.*

- **Request validation** — Schema validation, content-type negotiation, input sanitization. These are application-level concerns that depend on your data model. *Hull provides a validation module with schema-based input checking.*

- **ETag / Last-Modified** — These are application-specific (KEEL doesn't know when your data changes). Use existing `kl_request_header()` / `kl_response_header()` for the headers; your application handles 304 logic. *Hull handles conditional responses for static assets.*

- **Static file serving** — MIME types, path traversal protection, directory listing are application decisions. See `examples/static_files.c` for the pattern. *Hull auto-serves embedded or filesystem static assets with MIME detection.*

- **CSRF protection** — Cross-site request forgery tokens for form-based applications. *Hull provides `hull.middleware.csrf` with automatic token generation and validation.*

- **Idempotency keys** — Safe POST retry via `Idempotency-Key` header. *Hull provides `hull.middleware.idempotency` with configurable TTL and response caching.*

The general principle: if it requires policy decisions that vary between applications, it belongs in application code, not in the transport library. KEEL provides the hooks (middleware, body readers, access log callback) — you provide the policy.

## CI

GitHub Actions runs on every push and PR against `main`:

- **Linux (epoll)** — build, test, examples, smoke test
- **Linux (io_uring)** — build, test, examples, smoke test
- **Linux (poll fallback)** — build, test, examples, smoke test
- **macOS (kqueue)** — build, test, examples, smoke test
- **Linux (musl/Alpine)** — build, test, examples
- **Cosmopolitan (APE)** — build, examples, smoke test
- **ASan + UBSan** — build and test with sanitizers
- **Static Analysis** — scan-build + cppcheck
- **Fuzz Testing** — libFuzzer on HTTP parser + multipart (60s each)

A separate benchmark workflow runs on push to `main` (informational, not gating).

## License

MIT
