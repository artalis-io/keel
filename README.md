# KEEL — Kernel Event Engine, Lightweight

Minimal C11 HTTP server library built on raw epoll/kqueue/io_uring. Pluggable allocator, pluggable HTTP parser, pluggable body readers, streaming responses, multipart uploads, connection timeouts, zero forced buffering.

**101K req/s** on a single thread. **68 tests** with ASan/UBSan. **One vendored dependency** (llhttp).

## Build

```bash
make                    # build libkeel.a (epoll on Linux, kqueue on macOS)
make BACKEND=iouring    # build with io_uring backend (Linux 5.6+, requires liburing-dev)
make test               # run unit tests
make examples           # build all example programs
make debug              # debug build with ASan + UBSan
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

- **Three event loop backends** — epoll (edge-triggered), kqueue (edge-triggered), io_uring (POLL_ADD)
- **Pluggable HTTP parser** — ships with llhttp, swap via `KlConfig.parser`
- **Pluggable body readers** — vtable interface for request body processing
- **Multipart form-data** — RFC 2046 parser with configurable size limits
- **Three response modes** — buffered (writev), file (sendfile zero-copy), stream (chunked transfer encoding)
- **Route parameters** — `:param` capture, no allocation, pointers into read buffer
- **Connection timeouts** — monotonic clock sweep, automatic 408 responses, slow-loris protection
- **Pre-allocated connection pool** — no per-request malloc, no fragmentation under load
- **Pluggable allocator** — bring your own arena/pool/tracking allocator
- **pledge/unveil sandboxing** — init/run split makes syscall lockdown natural
- **Zero-copy techniques** — header pointers into read buffer, sendfile, writev batching

## Architecture

10 orthogonal modules, each independently testable:

| Module | Header | Description |
|--------|--------|-------------|
| **allocator** | `allocator.h` | Bring-your-own allocator interface |
| **event** | `event.h` | epoll / kqueue / io_uring abstraction |
| **request** | `request.h` | Parsed HTTP request struct (header-only, zero alloc) |
| **parser** | `parser.h` | Pluggable HTTP parser vtable |
| **response** | `response.h` | Response builder: buffered, sendfile, or streaming chunked |
| **router** | `router.h` | Table-scan route matching with `:param` capture |
| **connection** | `connection.h` | Pre-allocated connection pool + state machine |
| **server** | `server.h` | Top-level glue: init, bind, run loop, stop |
| **body_reader** | `body_reader.h` | Pluggable body reader vtable + buffer reader |
| **body_reader_multipart** | `body_reader_multipart.h` | RFC 2046 multipart/form-data parser |

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
    kl_response_body(res, br->data, br->len);
}

/* Register with size limit (1 MB) */
kl_server_route(&s, "POST", "/api/data", handle_post,
                (void *)(size_t)(1 << 20), kl_body_reader_buffer);
```

Pass `NULL` as the body reader factory for routes that don't accept a body. If a request with a body arrives on a route with no reader, KEEL discards the body. If the reader factory returns NULL, KEEL sends 415 Unsupported Media Type.

**Custom readers** — implement the `KlBodyReader` vtable (`on_data`, `on_complete`, `on_error`, `destroy`) and provide a factory function.

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

Implement the 3-function `KlParser` vtable (`parse`, `reset`, `destroy`) for any backend.

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

On Linux, use the [pledge polyfill](https://github.com/nicknisi/pledge) (seccomp-bpf + Landlock) for the same API. The key insight: KEEL's `init`/`run` split makes this natural — no library changes needed.

## Benchmark

```bash
./bench.sh              # automated: build, start server, warmup, wrk benchmark
```

Manual:

```bash
make examples
./examples/hello &
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

The io_uring backend uses `IORING_OP_POLL_ADD` for readiness notification — a drop-in replacement for epoll with io_uring's batched submission advantage. Requires `liburing-dev`.

## Testing

68 tests across 8 test suites, covering every module:

| Suite | Tests | Covers |
|-------|-------|--------|
| `test_allocator` | 4 | Default + custom tracking allocators |
| `test_router` | 8 | Exact match, params, 404, 405, wildcard |
| `test_response` | 8 | Status, headers, body, JSON, error, streaming, sendfile |
| `test_parser` | 7 | GET, POST, query strings, incomplete, reset |
| `test_connection` | 3 | Pool init, acquire/release, exhaustion |
| `test_body_reader` | 26 | Buffer + multipart: limits, spanning, binary, edge cases |
| `test_integration` | 6 | Full server: hello, POST, 413, keepalive, multipart |
| `test_timeout` | 4 | Idle, partial headers, partial body, active connections |

```bash
make test               # run all tests
make debug && make test  # run under ASan + UBSan
```

## Why C

**The attack surface is the network.** An HTTP server processes untrusted bytes from the internet through a parser (llhttp) into application handlers. We address this with defense-in-depth rather than language-level guarantees:

- `pledge()`/`unveil()` sandboxing — lock down syscalls and filesystem after binding the socket, before entering the event loop
- `-D_FORTIFY_SOURCE=2 -fstack-protector-strong` — compile-time and runtime buffer overflow detection
- AddressSanitizer + UBSan in debug builds
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

## CI

GitHub Actions runs on every push and PR against `main`:

- **Linux (epoll)** — build, test, smoke test
- **Linux (io_uring)** — build, test, smoke test
- **macOS (kqueue)** — build, test, smoke test

A separate benchmark workflow runs on push to `main` (informational, not gating).

## License

MIT
