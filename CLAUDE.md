# KEEL — Development Guide

## Build

```bash
make              # build libkeel.a (epoll on Linux, kqueue on macOS)
make test         # build and run all 160 unit tests
make examples     # build all 6 example programs
make debug        # debug build with ASan + UBSan (recompiles from clean)
make analyze      # Clang static analyzer (scan-build)
make cppcheck     # cppcheck static analysis
make fuzz         # build libFuzzer fuzz targets (requires clang)
make clean        # remove all build artifacts
```

## Project Structure

- `include/keel/` — Public headers. Each module is a `.h` file. `keel.h` is the umbrella.
- `src/` — Core source. Each module is a `.c` file.
- `parsers/` — Pluggable parser backends (`parser_llhttp.c`).
- `vendor/` — Vendored libraries (llhttp, utest.h). Do not modify.
- `tests/` — Unit tests using Sheredom's utest.h framework.
- `examples/` — Example programs (hello, rest_api, streaming_json, static_files, stream_body, multipart).
- `docs/` — Architecture and roadmap documentation.

## Architecture

13 orthogonal modules, each independently testable:

1. **allocator** — Bring-your-own allocator interface + default stdlib wrapper
2. **event** — epoll (Linux) / kqueue (macOS) / io_uring event loop abstraction
3. **request** — Parsed HTTP request struct (header-only, zero alloc)
4. **parser** — Pluggable HTTP parser vtable (ships with llhttp backend)
5. **response** — Response builder: buffered (writev), sendfile, or streaming chunked
6. **router** — Route matching with `:param` extraction + middleware chain
7. **connection** — Pre-allocated connection pool + state machine + timeout sweep
8. **server** — Top-level glue: init, bind, run loop, stop
9. **body_reader** — Pluggable body reader vtable + built-in buffer reader
10. **body_reader_multipart** — RFC 2046 multipart/form-data state machine parser
11. **chunked** — RFC 7230 parser-agnostic chunked transfer-encoding decoder
12. **cors** — Built-in CORS middleware with configurable origins/methods/headers
13. **tls** — Pluggable TLS transport vtable (bring-your-own backend)

## Key Types

| Type | Header | Purpose |
|------|--------|---------|
| `KlServer` | `server.h` | Server instance: config, router, pool, event loop |
| `KlConfig` | `server.h` | Configuration: port, bind_addr, max_connections, timeouts, allocator, parser |
| `KlRequest` | `request.h` | Parsed request: method, path, query, headers, content_length, body_reader |
| `KlResponse` | `response.h` | Response builder: status, headers, body/file/stream modes |
| `KlBodyReader` | `body_reader.h` | Vtable: on_data, on_complete, on_error, destroy |
| `KlBufReader` | `body_reader.h` | Buffer reader: growable buffer with max_size limit |
| `KlMultipartReader` | `body_reader_multipart.h` | Multipart parser: parts array, state machine, overlap buffer |
| `KlMultipartPart` | `body_reader_multipart.h` | Single part: name, filename, content_type, data, data_len |
| `KlMultipartConfig` | `body_reader_multipart.h` | Limits: max_part_size, max_total_size, max_parts |
| `KlChunkedDecoder` | `chunked.h` | Chunked TE decoder: state machine, no allocation |
| `KlAllocator` | `allocator.h` | Vtable: malloc, realloc (with old_size), free (with size) |
| `KlParser` | `parser.h` | Vtable: parse (returns KlParseResult), reset, destroy |
| `KlConn` | `connection.h` | Connection: fd, state, read_buf, request, response, parser, route |
| `KlRouter` | `router.h` | Route table + match function |
| `KlRoute` | `router.h` | Single route: method, pattern, handler, user_data, body_reader |
| `KlMiddleware` | `router.h` | Middleware function: `int (*)(KlRequest *, KlResponse *, void *)` |
| `KlMiddlewareEntry` | `router.h` | Registered middleware: method, pattern, fn, user_data |
| `KlCorsConfig` | `cors.h` | CORS config: allowed origins, methods, headers, credentials |
| `KlEventLoop` | `event.h` | Platform event loop: init, add, mod, del, wait, close |
| `KlTls` | `tls.h` | Vtable: handshake, read, write, shutdown, pending, reset, destroy |
| `KlTlsCtx` | `tls.h` | Opaque per-server TLS context (user-owned) |
| `KlTlsConfig` | `tls.h` | TLS config: ctx, factory, ctx_destroy |
| `KlTlsResult` | `tls.h` | Enum: OK, WANT_READ, WANT_WRITE, ERROR |
| `KlTlsFactory` | `tls.h` | Factory: creates per-connection KlTls from shared context |

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
- Error handling: return `-1` on failure, `0` on success (or positive value)
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
- Registered via `kl_router_use()` / `kl_server_use()` with method + pattern filter
- Patterns: `/*` = prefix match, `/exact` = exact match, `*` method = any method
- Middleware runs after route match, before body reading
- Short-circuit disables keep-alive (body may be unread)
- `req->ctx` (`void *`) enables middleware→handler data passing

Built-in: `kl_cors_middleware` (pass `KlCorsConfig *` as user_data).

To add a new middleware: implement the `KlMiddleware` signature, register with `kl_server_use()`.

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
- **Header access**: Use `kl_request_header(req, "Content-Type")` — case-insensitive, returns NULL if missing
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
