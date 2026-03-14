# KEEL — Agent Guidelines

Guidelines for AI agents working on keel: code review, auditing, testing, and contributing.

## Code Review Checklist

Every PR should be checked for:

- [ ] **Overflow guards** — All arithmetic on sizes/lengths checked against `SIZE_MAX/2` or `INT_MAX/2` before addition/multiplication
- [ ] **NULL checks** — Every pointer from allocation, lookup, or parameter is checked before dereference
- [ ] **Resource cleanup** — Every `_init` has a matching `_free`, every `open` has a `close`, every acquired connection is released
- [ ] **Bounds checking** — Array indices validated, buffer writes bounded by capacity, `snprintf` not `sprintf`
- [ ] **Return value checks** — `read`, `write`, `sendfile`, allocation functions all checked for errors
- [ ] **No allocation in hot path** — The event loop, connection state machine, and response send path must not call `malloc`
- [ ] **Allocator discipline** — All allocation goes through `KlAllocator`, never raw `malloc`/`free`
- [ ] **State machine correctness** — Connection state transitions are valid, no state reached without proper setup
- [ ] **Keep-alive safety** — Response reset clears all state, no stale pointers across requests
- [ ] **Header pointer lifetime** — After body reading starts, header pointers may be invalidated; don't access them in body reader callbacks
- [ ] **Middleware safety** — Middleware that short-circuits must write a complete response; short-circuit disables keep-alive (body unread)
- [ ] **Middleware order** — Registration order = execution order; global middleware (`/*`) before specific (`/api/*`) if intended
- [ ] **Request context cleanup** — `req->ctx` set by middleware is the application's responsibility to clean up in the handler
- [ ] **TLS orthogonality** — TLS changes must not affect middleware, router, body readers, or handlers
- [ ] **TLS shutdown** — Connections must call tls->shutdown() before close(fd)
- [ ] **Pending drain** — After TLS reads, tls->pending() must be checked to drain internal buffers
- [ ] **Async safety** — `kl_async_complete()` only called from event loop thread, never from workers
- [ ] **Watcher API** — All `kl_watcher_*` calls use `KlEventCtx *` (via `&server->ev` or standalone), never raw `KlServer *`
- [ ] **Thread pool callbacks** — `work_fn` must not touch connection/event loop state; `done_fn` runs on event loop thread
- [ ] **Thread pool creation** — `kl_thread_pool_create` takes `KlEventCtx *` (not `KlServer *`)
- [ ] **Thread pool shutdown** — `kl_thread_pool_free` called before `kl_server_free` or event loop teardown
- [ ] **Client response lifetime** — `KlClientResponse.alloc` is stored by value; response remains valid after allocator goes out of scope
- [ ] **Client async cleanup** — `kl_client_cancel()` + `kl_client_free()` called on error/deadline paths
- [ ] **URL injection** — URLs passed to `kl_url_parse()` / `kl_client_request()` are validated (CRLF rejection)

## Security Audit Patterns

### Unsafe functions to scan for

```bash
# These should NEVER appear in src/ or parsers/
grep -rn 'sprintf\b' src/ parsers/       # use snprintf
grep -rn 'strcpy\b' src/ parsers/        # use memcpy with length
grep -rn 'strcat\b' src/ parsers/        # use snprintf
grep -rn 'gets\b' src/ parsers/          # never
grep -rn 'atoi\b' src/ parsers/          # use strtol with validation
```

### Integer overflow patterns to check

```c
/* BAD — can overflow */
size_t new_cap = cap * 2;
char *buf = alloc(len + extra);

/* GOOD — overflow-safe */
if (cap > SIZE_MAX / 2) return -1;
size_t new_cap = cap * 2;

if (extra > SIZE_MAX - len) return -1;
char *buf = alloc(len + extra);
```

### Buffer boundary checks

- `read_buf` is heap-allocated, growable up to `max_header_size` — verify no write exceeds `read_cap`
- `hdr_buf` in multipart reader is 2048 — verify header overflow is caught
- Response header buffer grows — verify growth arithmetic is overflow-safe
- Body reader buffer grows — verify `max_size` is checked before growth

### Thread safety

KEEL's event loop is single-threaded. The `KlThreadPool` module introduces worker threads, but thread safety is maintained by construction:
- Workers only touch the work queue (mutex-protected) and a pipe fd — never the event loop or connection state
- `done_fn` callbacks run on the event loop thread (via pipe watcher) — safe to call `kl_async_complete`
- `kl_async_complete()` is NOT thread-safe — must only be called from the event loop thread
- No global mutable state (except `_Atomic int running`/`draining` for signal handling)
- Integration tests that use threads properly synchronize server start/stop
- No `static` mutable variables in any module (except test files)

## Testing Requirements

### Coverage expectations

- Every public function in `include/keel/*.h` should have at least one test
- Body reader tests should cover: normal operation, limits exceeded, boundary edge cases, empty input, binary data
- Integration tests should verify the full request→response cycle over real sockets
- Timeout tests should verify both triggering and non-triggering cases

### Edge cases to always cover

- Empty input (zero-length body, no headers, empty path)
- Maximum values (max headers=64, max params=16, max boundary=70)
- Boundary spanning (data split across multiple `on_data` calls)
- Allocation failure (if testing custom allocator that can fail)
- Connection pool exhaustion (all slots taken)
- Malformed input (bad HTTP, invalid multipart, truncated headers)

### Writing new tests

```c
#include <utest.h>
#include <keel/keel.h>

UTEST(suite_name, test_name) {
    /* Setup */
    KlAllocator alloc = kl_allocator_default();

    /* Action */
    int result = kl_some_function(&alloc, ...);

    /* Assert */
    ASSERT_EQ(0, result);
}

UTEST_MAIN();
```

Add the test file as `tests/test_<module>.c` — it's auto-discovered by the Makefile wildcard.

## Performance Considerations

- **No allocation in the hot path** — the event loop, state machine transitions, and response sending must work with pre-allocated buffers
- **writev batching** — response headers and body are sent in a single writev call, not separate writes
- **sendfile for files** — never read a file into a buffer to write it to a socket
- **TCP_CORK on Linux** — coalesce headers + file data into minimal packets
- **TCP_NODELAY** — no Nagle delay on response writes
- **Edge-triggered events** — fewer syscalls than level-triggered
- **Pre-built status lines** — "HTTP/1.1 200 OK\r\n" is a compile-time constant, not formatted per-request
- **Fast integer formatting** — Content-Length is formatted without snprintf

## Adding a New Module

1. Create header: `include/keel/<module>.h` with include guard `KEEL_<MODULE>_H`
2. Create source: `src/<module>.c`
3. Add to `CORE_SRC` in `Makefile`
4. Add `#include <keel/<module>.h>` to `include/keel/keel.h`
5. Prefix all public functions with `kl_<module>_`
6. Write tests: `tests/test_<module>.c`
7. Update module count (currently 29) in `README.md` and `CLAUDE.md`

## Adding a New Body Reader

1. Implement the `KlBodyReader` vtable (4 functions: `on_data`, `on_complete`, `on_error`, `destroy`)
2. Define a concrete struct embedding `KlBodyReader base` as the first field
3. Write a factory function: `KlBodyReader *kl_body_reader_<name>(KlAllocator *alloc, KlRequest *req, void *user_data)`
4. The factory inspects headers (Content-Type, Content-Length) to validate — return NULL to reject (triggers 415)
5. Register per-route: `kl_server_route(&s, method, pattern, handler, user_data, kl_body_reader_<name>)`
6. Create header in `include/keel/body_reader_<name>.h`, source in `src/body_reader_<name>.c`
7. Add to `CORE_SRC` in Makefile, include in `keel.h`

## Adding a New Event Backend

1. Implement all functions declared in `include/keel/event.h`:
   - `kl_event_init`, `kl_event_add`, `kl_event_mod`, `kl_event_del`, `kl_event_wait`, `kl_event_close`
2. Create `src/event_<backend>.c`
3. Add Makefile conditional:
   ```makefile
   ifeq ($(BACKEND),<name>)
     EVENT_SRC = src/event_<name>.c
     LDFLAGS += <any required libraries>
   endif
   ```
4. Test on the target platform — event backends are platform-specific

## Adding a New Middleware

Middleware uses the `KlMiddleware` function signature — return `0` to continue, non-zero to short-circuit:

```c
int my_middleware(KlRequest *req, KlResponse *res, void *user_data) {
    /* Inspect request, optionally modify response or req->ctx */
    /* Return 0 to continue, non-zero to short-circuit */
    return 0;
}
```

### Pass-through middleware (logging, metrics)

```c
int log_middleware(KlRequest *req, KlResponse *res, void *ctx) {
    (void)res; (void)ctx;
    fprintf(stderr, "[req] %.*s %.*s\n",
            (int)req->method_len, req->method,
            (int)req->path_len, req->path);
    return 0;  /* always continue */
}

kl_server_use(&s, "*", "/*", log_middleware, NULL);
```

### Short-circuit middleware (auth, rate limiting)

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
        return 1;  /* short-circuit — stop chain, send response */
    }
    return 0;
}

AuthConfig auth = {.api_key = "secret-key-123"};
kl_server_use(&s, "*", "/api/*", auth_middleware, &auth);
```

### Context-setting middleware (user lookup)

```c
int user_middleware(KlRequest *req, KlResponse *res, void *ctx) {
    (void)res; (void)ctx;
    req->ctx = my_user_lookup(req);  /* handler reads req->ctx */
    return 0;
}
```

### Pattern matching rules

- `"*"` method matches any HTTP method; `"GET"` also matches HEAD requests
- `/*` suffix = prefix match: `/api/*` matches `/api`, `/api/users`, `/api/users/123`
- No `/*` suffix = exact match: `/health` matches only `/health`
- No `:param` extraction in middleware patterns (inspect `req->path` directly)

### Registration

```c
kl_server_use(&s, method, pattern, fn, user_data);
// Or directly on the router:
kl_router_use(&r, method, pattern, fn, user_data);
```

Middleware runs in registration order, after route matching, before body reading.

## Adding a TLS Backend

1. Implement the 7 required `KlTls` vtable functions (`handshake`, `read`, `write`, `shutdown`, `pending`, `reset`, `destroy`) plus the optional `alpn_protocol` and `set_hostname` (NULL if not supported)
2. Create a `KlTlsCtx` struct for shared state (certificates, keys, ciphers)
3. Write a factory function: `KlTls *my_tls_factory(KlTlsCtx *ctx, KlAllocator *alloc)`
4. Register via `KlTlsConfig` in `KlConfig`:

```c
KlTlsConfig tls = {
    .ctx = my_ctx,
    .factory = my_tls_factory,
    .ctx_destroy = my_ctx_free,
};
KlConfig cfg = { .port = 8443, .tls = &tls };
```

The factory is called once per connection slot at server init. Each `KlTls` session is reused across keep-alive requests via `reset()`.

## Safety Tooling

All of these should pass cleanly before merging:

```bash
make test               # 613 unit + integration tests (35 suites)
make debug-test         # ASan + UBSan (catches memory errors, undefined behavior)
make analyze            # Clang static analyzer via scan-build
make cppcheck           # cppcheck static analysis
```

Fuzz testing covers the primary attack surface (untrusted network bytes):

```bash
make fuzz               # build libFuzzer targets (requires clang)
./fuzz/fuzz_parser fuzz/corpus_parser/
./fuzz/fuzz_multipart fuzz/corpus_multipart/
```

## Commit Conventions

- No `Co-Authored-By` trailers
- Concise commit messages focused on "why" not "what"
- One logical change per commit
