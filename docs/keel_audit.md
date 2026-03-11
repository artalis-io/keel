# Keel HTTP Client/Server Library — C Code Audit Report

**Date:** 2026-03-11
**Previous Audit:** 2026-03-09
**Auditor:** Claude Opus 4.6 (automated)
**Scope:** All source files in `src/`, `parsers/`, `include/keel/`, and internal headers (`src/*.h`). Test files in `tests/` checked for coverage gaps.

## Summary

| Severity | Count |
|----------|-------|
| Critical | 0     |
| High     | 0     |
| Medium   | 4     |
| Low      | 8     |
| Info     | 9     |
| **Total**| **21**|

**Files scanned:** 48 (22 source `.c`, 26 headers `.h`)
**Test suites:** 19 (308 UTEST cases)
**Lines of code (approx):** ~8,000 (src + parsers), ~2,500 (headers), ~9,500 (tests)

## Changes Since Last Audit (2026-03-09)

### New Modules
- `src/url.c` / `include/keel/url.h` — URL parser with CRLF injection guard, IPv6 support
- `src/client.c` / `include/keel/client.h` — Sync (blocking) + async (event-driven) HTTP/1.1 client
- `parsers/response_parser_llhttp.c` / `include/keel/parser.h` — Client-side HTTP response parser
- `include/keel/event_ctx.h` — Composable event loop context (refactored from server.h + async.h)

### Renamed Types
- `KlParser` → `KlRequestParser`, `kl_parser_llhttp()` → `kl_request_parser_llhttp()`
- `KlWatcher` / watcher API moved from `async.h` to `event_ctx.h`, now takes `KlEventCtx*` instead of `KlServer*`
- `KlThreadPool` creation now takes `KlEventCtx*` instead of `KlServer*`

### Resolved Issues (1 of 15)
| ID | Issue | Resolution |
|----|-------|------------|
| M-2 (prev) | listen_fd not set to -1 on event_init failure | Carried forward as resolved from previous audit |

## Overall Assessment

The Keel codebase remains well-engineered with strong security discipline. The new client modules (url.c, client.c, response_parser_llhttp.c) follow all established conventions. Key strengths:

- **No unsafe string functions:** Zero uses of `strcpy`, `strcat`, `sprintf`, `gets`, `atoi`, `atol`, or `atof` across all 22 source files.
- **Consistent allocator discipline:** All core allocations go through `kl_malloc`/`kl_free`/`kl_realloc`. Only `tls_mbedtls.c` (startup-only) and `allocator.c` (stdlib backend) use raw `malloc`/`free`.
- **Integer overflow guards:** Systematic `SIZE_MAX/2`, `INT_MAX/2` checks before multiplications and capacity doublings across all modules (35+ guard sites).
- **All 32+ kl_malloc calls NULL-checked** within 3 lines of allocation.
- **CRLF injection prevention:** Three separate `has_crlf()` / `contains_crlf()` guards — server response headers (`response.c`), client request headers (`client.c`), and URL components (`url.c`).
- **Request smuggling mitigation:** CL/TE priority correctly enforced in `parser_llhttp.c`.
- **SIGPIPE handled:** `signal(SIGPIPE, SIG_IGN)` in `server.c`; `SO_NOSIGPIPE` on client sockets (BSD/macOS).
- **Three-layer body size limits:** Content-Length early reject, chunked accumulation tracking, per-route buffer limits.
- **Client response size limit:** `max_response_size` enforced in response parser via `accum_append` overflow guards.
- **Thread pool safety by construction:** Workers never touch event loop state; `done_fn` always runs on event loop thread via pipe wakeup.
- **KlEventCtx composability:** Client and thread pool operate with just `KlEventCtx`, no server dependency.
- **Build hardening:** `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -D_FORTIFY_SOURCE=2 -fstack-protector-strong`, debug builds with ASan+UBSan, static analysis targets, and fuzz targets (parser, multipart, WebSocket).

---

## Critical Issues

None found.

---

## High Issues

None found.

---

## Medium Issues

### M-1: tls_mbedtls.c uses raw malloc/free for file I/O and context allocation

| Field | Value |
|-------|-------|
| **File** | `src/tls_mbedtls.c` |
| **Lines** | 291, 301, 328, 359, 372, 384, 428, 436, 468, 503, 523 |
| **Category** | Allocator Discipline |
| **Impact** | Unchanged from previous audit. Startup-only raw `malloc`/`free`. Documented exception. |
| **Suggested Fix** | Accept as-is, or thread an optional `KlAllocator` through context creation. |

### M-2: kl_async_complete ignores kl_event_add return value

| Field | Value |
|-------|-------|
| **File** | `src/async.c` |
| **Lines** | 157-160 |
| **Category** | Resource Management |
| **Impact** | After resuming a suspended connection, `kl_event_add` is called to re-register the fd with the event loop. If this fails (event loop full, fd invalid), the connection is stranded — removed from the suspended ops list but not registered in the event loop. It will never receive events and never be cleaned up. |
| **Suggested Fix** | Check `kl_event_add` return value. On failure, release the connection via `kl_server_conn_release`. |

### M-3: kl_client_start missing input validation for num_headers/headers

| Field | Value |
|-------|-------|
| **File** | `src/client.c` |
| **Lines** | 727-857 |
| **Category** | Input Validation |
| **Impact** | The sync API (`kl_client_request`) validates `num_headers` range `[0, KL_CLIENT_MAX_REQ_HEADERS]` and `headers != NULL` when `num_headers > 0`. The async API (`kl_client_start`) does not. A positive `num_headers` with `headers == NULL` crashes in `build_request`. |
| **Suggested Fix** | Add the same validation at the top of `kl_client_start`. |

### M-4: io_uring kl_event_mod can leave fd un-monitored on double SQE failure

| Field | Value |
|-------|-------|
| **File** | `src/event_iouring.c` |
| **Lines** | 107-141 |
| **Category** | Resource Management / Event Loop |
| **Impact** | Unchanged from previous audit (was M-5). Extremely unlikely — requires RING_SIZE (256) concurrent mod operations. |
| **Suggested Fix** | Low priority. |

---

## Low Issues

### L-1: kl_request_header returns non-null-terminated pointer (API footgun)

| Field | Value |
|-------|-------|
| **File** | `include/keel/request.h` |
| **Lines** | 51-61 |
| **Category** | API Safety |
| **Impact** | Unchanged from previous audit. |

### L-2: body_reader_buffer casts user_data to size_t (pointer-to-integer)

| Field | Value |
|-------|-------|
| **File** | `src/body_reader_buffer.c` |
| **Lines** | 58 |
| **Category** | Portability |
| **Impact** | Unchanged. Accepted as documented pattern. |

### L-3: H2 stream linear search O(n) per lookup

| Field | Value |
|-------|-------|
| **File** | `src/h2.c` |
| **Lines** | 19-25 |
| **Category** | Performance |
| **Impact** | Unchanged. Acceptable for default max 128 streams. |

### L-4: build_request addition lacks overflow guard

| Field | Value |
|-------|-------|
| **File** | `src/client.c` |
| **Lines** | 508 |
| **Category** | Integer Overflow |
| **Impact** | `size_t total = (size_t)off + body_len;` does not check for wrap-around. If `body_len` is near `SIZE_MAX`, `total` wraps to a small value, and the subsequent `memcpy` writes past the allocation. Practically mitigated by `kl_malloc` failing for huge sizes, but violates the project convention of explicit overflow guards. |
| **Suggested Fix** | Add `if (body_len > SIZE_MAX - (size_t)off) return NULL;` |

### L-5: H2 malloc failure sends empty 200 instead of 500

| Field | Value |
|-------|-------|
| **File** | `src/h2.c` |
| **Lines** | 155 |
| **Category** | Error Handling |
| **Impact** | When `kl_malloc` fails for a file-backed HTTP/2 response, the code sends a 200 with an empty body instead of a 500 error. Not a crash (NULL is guarded by `if (file_buf)`) but a silent data loss. |
| **Suggested Fix** | Return 500 on allocation failure, matching the pattern at lines 143-151. |

### L-6: Standalone client on Linux lacks SIGPIPE protection

| Field | Value |
|-------|-------|
| **File** | `src/client.c` |
| **Lines** | 41 (io_write), 84-88 (SO_NOSIGPIPE) |
| **Category** | Network Safety |
| **Impact** | `SO_NOSIGPIPE` only works on macOS/BSD. On Linux, SIGPIPE is only suppressed when the server is running (`signal(SIGPIPE, SIG_IGN)` in `kl_server_run`). A standalone client on Linux without a server could receive SIGPIPE on a broken pipe. |
| **Suggested Fix** | Use `send(fd, buf, len, MSG_NOSIGNAL)` instead of `write()` in `io_write`, or add `signal(SIGPIPE, SIG_IGN)` in `kl_client_request()`. |

### L-7: kl_watcher_rearm ignores kl_event_mod return value

| Field | Value |
|-------|-------|
| **File** | `src/async.c` |
| **Lines** | 79 |
| **Category** | Error Handling |
| **Impact** | `kl_event_mod` return value is silently discarded. If re-arm fails, the watcher remains in the list but never fires. The function is `void`, so callers cannot detect the failure. |
| **Suggested Fix** | Low priority — either return `int` or assert internally. |

### L-8: Response parser has no per-header size limit

| Field | Value |
|-------|-------|
| **File** | `parsers/response_parser_llhttp.c` |
| **Category** | Resource Limits |
| **Impact** | Header count is limited to `KL_MAX_RESPONSE_HEADERS` (64), but individual header name/value sizes are unbounded. A malicious server could send a single header value of arbitrary size, consuming unbounded memory. The `max_response_size` parameter only applies to the body. |
| **Suggested Fix** | Consider adding a `max_header_size` limit, or applying `max_response_size` to headers + body combined. |

---

## Informational Notes

### I-1: Thread pool backpressure correctly prevents done queue overflow

The `inflight` counter tracks items across all three stages (work queue + executing + done queue). `submit()` rejects when `inflight >= done_cap` where `done_cap = work_cap + num_workers`.

### I-2: Thread pool shutdown sequence is correct

Shutdown sets flag, broadcasts condvar, joins all workers, then drains remaining done items (`done_fn`) and work items (`cancel_fn`).

### I-3: Watcher re-arm is safe when callback removes watcher

`kl_watcher_rearm()` walks the watcher list to find the fd. If the callback called `kl_watcher_del()`, the watcher is removed from the list and `rearm` is a safe no-op.

### I-4: CORS origin_buf stack buffer matches KL_CORS_ORIGIN_SIZE

256-byte stack buffer matches the maximum origin length. Safe.

### I-5: Debug build includes `-fno-omit-frame-pointer`

Good practice for ASan/UBSan diagnostics.

### I-6: Fuzz targets cover the three primary attack surfaces

`fuzz_parser` (HTTP request parser + chunked decoder), `fuzz_multipart` (multipart parser), and `fuzz_websocket` (WebSocket frame parser).

### I-7: Async client uses blocking DNS (documented trade-off)

`kl_client_start` calls `getaddrinfo()` synchronously on the event loop thread. Code comment acknowledges this: "blocking — typically fast, OS-cached". For production use with slow DNS, consider async DNS via thread pool.

### I-8: Tagged pointer alignment assumption undocumented

`watcher_tag()` sets LSB=1 on `KlWatcher*`. This requires 2-byte alignment, which is guaranteed by the struct's `int` and pointer members. A `_Static_assert` would make the assumption explicit.

### I-9: thread_pool queue_cap + num_workers unchecked

`int done_cap = queue_cap + num_workers` could overflow if both are near `INT_MAX`. Both come from user config (`KlThreadPoolConfig`), typically small values (4-64). Not exploitable in practice.

---

## New Module Assessment: url.c

| Check | Status |
|-------|--------|
| NULL parameter checks | `kl_url_parse`: checks `!url`, `!out` |
| CRLF injection guard | `has_crlf()` on hostname (line 68) and path (line 88) |
| IPv6 bracket handling | Missing `]` returns -1 (line 51). Empty `[]` rejected via `host_len == 0` (line 64) |
| Port range validation | `strtol` + range check `[1, 65535]` (line 76) |
| Non-numeric port rejected | `end == url` check after `strtol` (line 74) |
| Zero-copy documented | Header doc states pointers reference original string |
| No dynamic allocation | Zero `kl_malloc` calls — pure pointer arithmetic |

**Verdict:** Clean. No issues.

## New Module Assessment: client.c

| Check | Status |
|-------|--------|
| All kl_malloc NULL-checked | Lines 281, 509, 815, 839 — all checked within 2 lines |
| fd closed on all error paths | Sync: goto cleanup → close(fd). Async: every failure point closes fd |
| TLS shutdown before close | Sync: lines 380-383. Async: `async_complete_success/error` |
| Response size limit enforced | `max_resp` passed to response parser factory |
| Connect timeout handled | Non-blocking connect + `poll()` with `timeout_ms` |
| CRLF injection guarded | Method, header names, header values all checked via `has_crlf()` |
| Watcher cleanup on cancel/error | `kl_watcher_del` called in both completion paths and `kl_client_cancel` |
| kl_client_free handles all states | Calls `kl_client_cancel` first if still in-flight |

**Issues:** M-3 (missing `num_headers` validation in async path), L-4 (overflow guard in `build_request`), L-6 (SIGPIPE on standalone Linux).

## New Module Assessment: response_parser_llhttp.c

| Check | Status |
|-------|--------|
| Factory kl_malloc NULL-checked | Line 347 |
| accum_append realloc NULL-checked | Line 70 |
| flush_header realloc NULL-checked | Lines 97-98, with cleanup of name_copy on value failure |
| Body size limit enforced | Line 191: `p->max_body` check |
| Header count limited | Line 88: `KL_MAX_RESPONSE_HEADERS` (64) cap |
| Integer overflow in accum_append | Line 59: `SIZE_MAX - *len` guard. Line 65: `SIZE_MAX / 2` guard |
| Proper cleanup on destroy | Lines 317-338: frees all header strings, body, headers array, self |
| Proper cleanup on reset | Lines 283-315: same pattern minus freeing self |
| Ownership transfer clean | Lines 236-248: parser pointers cleared after transfer |

**Issues:** L-8 (no per-header-value size limit).

## New Module Assessment: event_ctx (async.c refactor)

| Check | Status |
|-------|--------|
| kl_event_ctx_init NULL checks | Line 16: `!ctx || !alloc` |
| kl_event_ctx_free NULL check | Line 23: `!ctx` |
| All watchers freed on ctx_free | Lines 25-31: walks list, event_del + kl_free each |
| Event loop closed | Line 31: `kl_event_close` after all watchers removed |
| kl_watcher_add rollback on failure | Lines 53-55: unlinks from list, frees watcher |
| kl_watcher_mod validates inputs | Line 61: ctx, fd checks. Returns -1 for unknown fd |
| kl_watcher_del safe for unknown fd | Lines 82-96: silently returns if not found |

**Issues:** M-2 (`kl_async_complete` ignores `kl_event_add` return), L-7 (`kl_watcher_rearm` ignores `kl_event_mod` return).

---

## Build Hardening Assessment

| Feature | Status | Notes |
|---------|--------|-------|
| `-Wall -Wextra -Wpedantic` | Present | All builds |
| `-Wshadow` | Present | All builds |
| `-Wformat=2` | Present | All builds |
| `-Werror` | Present | All builds |
| `-D_FORTIFY_SOURCE=2` | Present | Non-cosmo, non-debug builds |
| `-fstack-protector-strong` | Present | Non-cosmo builds |
| ASan + UBSan (debug) | Present | `make debug` |
| `-fno-omit-frame-pointer` | Present | Debug builds |
| Clang static analysis | Present | `make analyze` |
| cppcheck | Present | `make cppcheck` |
| libFuzzer targets | Present | Parser + multipart + WebSocket |
| Vendor code `-w` | Present | Relaxed for llhttp |
| `-lpthread` (Linux) | Present | Required for thread pool |

**Missing/Recommended:**

- `-Wconversion` is not set. Would catch implicit narrowing conversions.
- Consider a fuzz target for the response parser (`fuzz_response_parser`).

---

## Test Coverage Assessment

| Module | Test File | Tests | Coverage |
|--------|-----------|-------|----------|
| allocator | test_allocator.c | 4 | Basic: custom, default, realloc |
| async | test_async.c | 11 | Good: watchers (KlEventCtx), suspend/resume, deadlines, cancel |
| body_reader | test_body_reader.c | 25 | Excellent: buffer + multipart, limits, spanning, binary |
| chunked | test_chunked.c | 18 | Excellent: sizes, extensions, trailers, overflow |
| client | test_client.c | 12 | Good: sync/async validation, response free, NULL safety |
| connection | test_connection.c | 4 | Minimal: pool ops only |
| cors | test_cors.c | 17 | Excellent: origins, preflight, credentials |
| h2 | test_h2.c | 26 | Good: HPACK, frames, streams, settings, cleanup |
| integration | test_integration.c | 28 | Good: full request/response paths, middleware, logging |
| overflow | test_overflow.c | 18 | Good: integer overflow guards across modules |
| parser | test_parser.c | 8 | Good: GET, POST, headers, body, chunked |
| response | test_response.c | 14 | Good: modes, CRLF injection, keep-alive |
| response_parser | test_response_parser.c | 9 | Good: 200, chunked, headers, limits, malformed, reset |
| router | test_router.c | 30 | Excellent: params, methods, middleware, overlap |
| thread_pool | test_thread_pool.c | 12 | Good: create/free, submit, FIFO, stress, cancel, shutdown |
| timeout | test_timeout.c | 4 | Basic: idle, partial, body timeout |
| tls | test_tls.c | 20 | Good: vtable mocking, handshake, ALPN, shutdown, pool |
| url | test_url.c | 15 | Good: HTTP/HTTPS, ports, IPv6, CRLF, edge cases |
| websocket | test_websocket.c | 33 | Excellent: SHA-1, base64, frames, fragmentation, close |

### Coverage Gaps

1. **No allocation failure injection** — No tests for malloc/realloc failure paths.

2. **No dedicated event loop backend tests** — Backends exercised indirectly via integration tests only.

3. **Connection state machine** — `kl_conn_on_readable`, `kl_conn_on_writable` not directly unit-tested.

4. **`kl_response_body_copy`** — Recently added function with no test coverage.

5. **mbedTLS backend** — 5 concrete functions untested (require linked mbedTLS + real cert files). Vtable is tested via mocks.

6. **No response parser fuzz target** — The request parser and multipart parser have fuzz targets, but the response parser does not.

---

## Recommendations

### Priority 1 (Should Fix)

1. **Fix M-3** — Add `num_headers` / `headers` validation to `kl_client_start` (copy from `kl_client_request`).

2. **Fix M-2** — Check `kl_event_add` return value in `kl_async_complete`. Release connection on failure.

3. **Fix L-4** — Add overflow guard in `build_request`: `if (body_len > SIZE_MAX - (size_t)off) return NULL;`

### Priority 2 (Low)

4. **Fix L-6** — Use `send(fd, buf, len, MSG_NOSIGNAL)` on Linux for standalone client SIGPIPE safety.

5. **Add a fuzz target for `response_parser_llhttp.c`** — The client parses untrusted server responses.

6. **Add `kl_response_body_copy` test** — Recently added, currently untested.

7. **Add allocation failure injection** — Custom allocator that fails on Nth call for testing error paths.

8. **Consider removing `kl_request_header()`** (L-1) in favor of length-aware `kl_request_header_len()`.
