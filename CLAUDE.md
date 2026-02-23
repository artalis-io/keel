# KEEL — Development Guide

## Build

```bash
make              # build libkeel.a (epoll on Linux, kqueue on macOS)
make test         # build and run all 68 unit tests
make examples     # build all 6 example programs
make debug        # debug build with ASan + UBSan (recompiles from clean)
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

10 orthogonal modules, each independently testable:

1. **allocator** — Bring-your-own allocator interface + default stdlib wrapper
2. **event** — epoll (Linux) / kqueue (macOS) / io_uring event loop abstraction
3. **request** — Parsed HTTP request struct (header-only, zero alloc)
4. **parser** — Pluggable HTTP parser vtable (ships with llhttp backend)
5. **response** — Response builder: buffered (writev), sendfile, or streaming chunked
6. **router** — Table-scan route matching with `:param` extraction
7. **connection** — Pre-allocated connection pool + state machine + timeout sweep
8. **server** — Top-level glue: init, bind, run loop, stop
9. **body_reader** — Pluggable body reader vtable + built-in buffer reader
10. **body_reader_multipart** — RFC 2046 multipart/form-data state machine parser

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
| `KlAllocator` | `allocator.h` | Vtable: malloc, realloc (with old_size), free (with size) |
| `KlParser` | `parser.h` | Vtable: parse (returns KlParseResult), reset, destroy |
| `KlConn` | `connection.h` | Connection: fd, state, read_buf, request, response, parser, route |
| `KlRouter` | `router.h` | Route table + match function |
| `KlRoute` | `router.h` | Single route: method, pattern, handler, user_data, body_reader |
| `KlEventLoop` | `event.h` | Platform event loop: init, add, mod, del, wait, close |

## Conventions

- C11, compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror`
- `-fstack-protector-strong` for buffer overflow detection
- No direct malloc/free — all allocation through `KlAllocator` interface
- All public functions prefixed with `kl_` (e.g. `kl_router_init`, `kl_response_json`)
- Header-only code in `request.h` uses `static inline`
- Vendor code compiled with `-w` (relaxed warnings, no `-Werror`)
- Integer overflow guards: check against `SIZE_MAX/2` or `INT_MAX/2` before arithmetic
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
