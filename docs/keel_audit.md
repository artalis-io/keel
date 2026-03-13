# Keel HTTP Client/Server Library — C Code Audit Report

**Date:** 2026-03-13
**Previous Audit:** 2026-03-11
**Auditor:** Claude Opus 4.6 (automated)
**Scope:** All source files in `src/`, `parsers/`, `include/keel/`, and internal headers (`src/*.h`). Test files in `tests/` checked for coverage gaps.

## Summary

| Severity | Count |
|----------|-------|
| Critical | 0     |
| High     | 0     |
| Medium   | 5     |
| Low      | 10    |
| Info     | 9     |
| **Total**| **24**|

**Files scanned:** 56 (25 source `.c`, 31 headers `.h`)
**Test suites:** 25 (435+ UTEST cases)
**Lines of code (approx):** ~8,100 (src + parsers), ~2,600 (headers), ~10,000 (tests)

## Changes Since Last Audit (2026-03-11)

### New Module
- `src/sse.c` / `include/keel/sse.h` — Server-Sent Events helper: line framing over chunked streaming (zero alloc)

### New Test Suite
- `tests/test_sse.c` — 7 tests covering basic events, multiline data, empty data, comments, write errors, header setup

### New Example
- `examples/sse.c` — SSE streaming example with E2E smoke test

---

## Overall Assessment

The Keel codebase remains well-engineered with strong security discipline. The new SSE module follows the established zero-allocation pattern and correctly delegates to the existing streaming response API. Key strengths:

- **No unsafe string functions:** Zero uses of `strcpy`, `strcat`, `sprintf`, `gets`, `atoi`, `atol`, or `atof` across all 25 source files.
- **Consistent allocator discipline:** All core allocations go through `kl_malloc`/`kl_free`/`kl_realloc`. Only `tls_mbedtls.c` (startup-only) and `allocator.c` (stdlib backend) use raw `malloc`/`free`.
- **All 32+ kl_malloc calls NULL-checked** within 3 lines of allocation.
- **Integer overflow guards:** Systematic `SIZE_MAX/2`, `INT_MAX/2` checks before multiplications and capacity doublings (35+ guard sites).
- **CRLF injection prevention:** Three separate guards — server response headers, client request headers, URL components.
- **Build hardening:** `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -D_FORTIFY_SOURCE=2 -fstack-protector-strong`, debug builds with ASan+UBSan, static analysis targets, 4 fuzz targets.

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

### M-2: h2_client partial header copy leaks + UB on allocation failure

| Field | Value |
|-------|-------|
| **File** | `src/h2_client.c` |
| **Lines** | 192-208 |
| **Category** | Memory Safety / Resource Management |
| **Impact** | When allocation fails partway through `h2c_on_response` header copy loop, previously copied headers (iterations 0..i-1) are leaked. Additionally, `st->resp.num_headers = n` is set to the *intended* count, not the actually-copied count, leaving uninitialized entries that cause UB when `kl_h2_client_response_free` iterates them. |
| **Suggested Fix** | Track actual count: `st->resp.num_headers = i;` before the `return;`. On failure, free headers 0..i-1 before returning, or rely on `kl_h2_client_response_free` to clean up with the corrected count. |

### M-3: SSE `kl_sse_event` crashes on NULL data with data_len > 0

| Field | Value |
|-------|-------|
| **File** | `src/sse.c` |
| **Lines** | 37-39 |
| **Category** | Input Validation / Memory Safety |
| **Impact** | `data + data_len` is undefined behavior when `data == NULL`. `memchr(NULL, ...)` dereferences NULL. Crashes on any platform. |
| **Current Code** | `const char *end = data + data_len;` ... `memchr(p, '\n', (size_t)(end - p))` |
| **Suggested Fix** | Add guard: `if (data_len > 0 && !data) return -1;` at function entry. |

### M-4: Router `kl_router_add` / `kl_router_use` crash on NULL method/pattern

| Field | Value |
|-------|-------|
| **File** | `src/router.c` |
| **Lines** | 21, 137, 182 |
| **Category** | Input Validation |
| **Impact** | `strlen(method)` and `strlen(pattern)` crash on NULL. Called via `kl_server_route`, `kl_server_use`, `kl_server_use_post` which also don't validate. |
| **Suggested Fix** | Add `if (!method \|\| !pattern) return -1;` at entry. |

### M-5: io_uring kl_event_mod can leave fd un-monitored on double SQE failure

| Field | Value |
|-------|-------|
| **File** | `src/event_iouring.c` |
| **Lines** | 107-141 |
| **Category** | Resource Management / Event Loop |
| **Impact** | Unchanged from previous audit (was M-4). Extremely unlikely — requires RING_SIZE (256) concurrent mod operations. |
| **Suggested Fix** | Low priority. |

---

## Low Issues

### L-1: kl_request_header returns non-null-terminated pointer (API footgun)

| Field | Value |
|-------|-------|
| **File** | `include/keel/request.h` |
| **Lines** | 51-61 |
| **Category** | API Safety |
| **Impact** | Fixed in a45563d — header values are now null-terminated in-place. Retained for documentation. |

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

### L-4: Thread pool `done_cap` and queue allocations lack overflow guards

| Field | Value |
|-------|-------|
| **File** | `src/thread_pool.c` |
| **Lines** | 139, 154, 157, 181 |
| **Category** | Integer Overflow |
| **Impact** | `int done_cap = queue_cap + num_workers` has no `INT_MAX/2` guard. `(size_t)cap * sizeof(KlWorkItem)` has no `SIZE_MAX / sizeof(T)` guard. Unlike `connection.c:46` which checks before multiplication. Unreachable with realistic config values. |
| **Suggested Fix** | Add `if (queue_cap > INT_MAX/2 \|\| num_workers > INT_MAX/2) return NULL;` and `if ((size_t)queue_cap > SIZE_MAX / sizeof(KlWorkItem)) return NULL;` |

### L-5: h2_client header allocation lacks overflow guard

| Field | Value |
|-------|-------|
| **File** | `src/h2_client.c` |
| **Lines** | 183 |
| **Category** | Integer Overflow |
| **Impact** | `(size_t)n * sizeof(KlH2ClientHeader)` without `SIZE_MAX / sizeof(T)` guard. `n` comes from session vtable (potentially attacker-influenced). |
| **Suggested Fix** | Add `if ((size_t)n > SIZE_MAX / sizeof(KlH2ClientHeader)) return;` |

### L-6: h2_client `h2c_on_data` addition can wrap before SIZE_MAX guard

| Field | Value |
|-------|-------|
| **File** | `src/h2_client.c` |
| **Lines** | 220-221 |
| **Category** | Integer Overflow |
| **Impact** | `size_t needed = st->resp.body_len + len;` can wrap before `if (needed > SIZE_MAX / 2)` catches it. Correct pattern is `if (len > SIZE_MAX - body_len)`. Unreachable in practice (requires body_len near SIZE_MAX). |
| **Suggested Fix** | `if (len > SIZE_MAX - st->resp.body_len) return;` |

### L-7: SSE module has no NULL checks on public functions

| Field | Value |
|-------|-------|
| **File** | `src/sse.c` |
| **Lines** | 4, 24, 57, 61 |
| **Category** | Input Validation |
| **Impact** | `kl_sse_begin(NULL, sse)`, `kl_sse_comment(NULL, ...)`, `kl_sse_end(NULL)` all crash. Inconsistent with `kl_async_*` which check NULL. |
| **Suggested Fix** | Add `if (!sse) return -1;` to each function; add `if (!res) return -1;` to `kl_sse_begin`. |

### L-8: WebSocket server send API missing NULL ws check

| Field | Value |
|-------|-------|
| **File** | `src/websocket.c` |
| **Lines** | 217-233 |
| **Category** | Input Validation |
| **Impact** | `kl_ws_server_send_text/binary/ping`, `kl_ws_server_close` dereference `ws` without NULL check. The client equivalents (`kl_ws_client_send_*`) do check `!ws`. |
| **Suggested Fix** | Add `if (!ws) return -1;` to each function. |

### L-9: Client (int) cast truncates url path/host length for %.*s

| Field | Value |
|-------|-------|
| **File** | `src/client.c` |
| **Lines** | 210-211, 490-491 |
| **Category** | Signed/Unsigned |
| **Impact** | `(int)url->path_len` silently truncates values above `INT_MAX`. In practice, URL paths are small, and snprintf truncation checks catch oversized output. |

### L-10: TLS BIO `bio_send` uses plain `write()` without MSG_NOSIGNAL

| Field | Value |
|-------|-------|
| **File** | `src/tls_mbedtls.c` |
| **Lines** | 69-85 |
| **Category** | Network Safety |
| **Impact** | On Linux client-only usage (no server running to set `SIG_IGN`), SIGPIPE from a broken peer during TLS write could kill the process. In practice, macOS provides `SO_NOSIGPIPE` and the server sets `SIG_IGN`, so this path is rarely reached. |

---

## Informational Notes

### I-1: Thread pool backpressure correctly prevents done queue overflow

The `inflight` counter tracks items across all three stages. `submit()` rejects when `inflight >= done_cap`.

### I-2: Thread pool shutdown sequence is correct

Shutdown sets flag, broadcasts condvar, joins all workers, then drains remaining done items (`done_fn`) and work items (`cancel_fn`).

### I-3: Watcher re-arm is safe when callback removes watcher

`kl_watcher_rearm()` walks the watcher list. If the callback called `kl_watcher_del()`, rearm is a safe no-op.

### I-4: CORS origin_buf stack buffer matches KL_CORS_ORIGIN_SIZE (256 bytes)

### I-5: Debug build includes `-fno-omit-frame-pointer`

### I-6: Fuzz targets cover four primary attack surfaces

Parser, multipart, WebSocket, response parser.

### I-7: Async client uses blocking DNS (documented trade-off)

`KlResolver` vtable added for pluggable async DNS. NULL falls back to sync `getaddrinfo`.

### I-8: Tagged pointer alignment assumption undocumented

`watcher_tag()` sets LSB=1 on `KlWatcher*`. A `_Static_assert` would make the 2-byte alignment assumption explicit.

### I-9: thread_pool pipe write return value deliberately ignored

`(void)wr` on pipe write is intentional. If pipe buffer is full, done items are drained on next event loop tick.

---

## New Module Assessment: sse.c

| Check | Status |
|-------|--------|
| Zero allocation | Yes — all writes go through `write_fn` directly |
| write_fn return values checked | Yes — every call to `write_field` and `sse->write_fn` propagates -1 |
| Buffer overflow possible | No — no stack or heap buffers used |
| SSE format correct | Yes — `\n` line terminators, blank line event separator, `: ` comment prefix |
| Multiline data handling | Yes — splits on `\n`, each line gets `data: ` prefix |
| Empty data handling | Yes — `data_len == 0` produces `data: \n\n` |
| NULL parameter checks | **Missing** — see M-3, L-7 |
| NULL data with data_len == 0 | UB: `NULL + 0` pointer arithmetic (works in practice) |

**Issues found:** M-3 (NULL data crash), L-7 (no NULL checks on any public function).

---

## Test Coverage Assessment

| Module | Test File | Tests | Coverage |
|--------|-----------|-------|----------|
| allocator | test_allocator.c | 4 | Basic: custom, default, realloc |
| async | test_async.c | 14 | Good: watchers (KlEventCtx), suspend/resume, deadlines, cancel |
| body_reader | test_body_reader.c | 30 | Excellent: buffer + multipart, limits, spanning, binary |
| chunked | test_chunked.c | 17 | Excellent: sizes, extensions, trailers, overflow |
| client | test_client.c | 18 | Good: sync/async validation, response free, NULL safety |
| connection | test_connection.c | 9 | Basic: pool ops, acquire/release |
| cors | test_cors.c | 17 | Excellent: origins, preflight, credentials |
| event | test_event.c | 7 | Basic: init, add, wait, close |
| event_ctx | test_event_ctx.c | 8 | Good: watcher lifecycle |
| h2 | test_h2.c | 18 | Good: HPACK, frames, streams, settings |
| h2_client | test_h2_client.c | 29 | Good: mock session vtable, stream tracking, response free |
| integration | test_integration.c | 27 | Good: full request/response paths, middleware |
| overflow | test_overflow.c | 20 | Good: integer overflow guards across modules |
| parser | test_parser.c | 9 | Good: GET, POST, headers, body, chunked |
| request | test_request.c | 14 | Good: header access, params |
| response | test_response.c | 10 | Good: modes, streaming, CRLF injection, keep-alive |
| response_parser | test_response_parser.c | 24 | Good: 200, chunked, headers, limits, malformed |
| router | test_router.c | 27 | Excellent: params, methods, middleware, overlap |
| **sse** | **test_sse.c** | **7** | **Partial: see below** |
| thread_pool | test_thread_pool.c | 12 | Good: create/free, submit, FIFO, stress, cancel |
| timeout | test_timeout.c | 8 | Basic: idle, partial, body timeout |
| tls | test_tls.c | 20 | Good: vtable mocking, handshake, ALPN, shutdown |
| url | test_url.c | 20 | Good: HTTP/HTTPS, ws/wss, ports, IPv6, CRLF |
| websocket | test_websocket.c | 28 | Excellent: SHA-1, base64, frames, fragmentation, close |
| websocket_client | test_websocket_client.c | 42 | Good: frame encoding, mask XOR, handshake, parser |

### SSE Test Coverage Details

| Function | Tests | Verdict |
|----------|-------|---------|
| `kl_sse_begin` | 1 (`begin_sets_headers`) | Happy path only — no failure path tests |
| `kl_sse_event` | 5 (basic, data_only, multiline, empty, write_error) | Good for common cases |
| `kl_sse_comment` | 1 (`comment`) | Happy path only |
| `kl_sse_end` | 0 | **Not directly tested** (called via `kl_response_end_stream` in begin test) |

**Missing SSE edge cases:**
1. `kl_sse_end` — no direct test through the SSE API
2. NULL `data` with `data_len > 0` — crashes (M-3)
3. Data ending with `\n` — untested framing subtlety
4. Multiple consecutive events on same handle
5. Write failure at different points (mid-id, mid-data, terminal newline)
6. `kl_sse_begin` failure paths (header buffer full)

### General Coverage Gaps (Priority Order)

1. **Connection state machine** — `kl_conn_on_readable`, `kl_conn_on_writable` not directly unit-tested.
2. **WebSocket server send API** — `kl_ws_server_send_text/binary/ping/close` have no unit tests.
3. **No allocation failure injection** — No tests for malloc/realloc failure paths.
4. **`kl_sse_end`** — Not tested through the SSE API.

---

## Build Hardening Assessment

| Feature | Status |
|---------|--------|
| `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror` | Present |
| `-D_FORTIFY_SOURCE=2` | Present (non-cosmo, non-debug) |
| `-fstack-protector-strong` | Present (non-cosmo) |
| ASan + UBSan (debug) | Present (`make debug`) |
| `-fno-omit-frame-pointer` | Present (debug) |
| Clang static analysis | Present (`make analyze`) |
| cppcheck | Present (`make cppcheck`) |
| libFuzzer targets (4) | Present |
| Vendor code `-w` | Present |

**Missing/Recommended:** `-Wconversion` (catches implicit narrowing conversions).

---

## Recommendations

### Priority Fixes

1. **Fix M-3** — Add NULL data guard to `kl_sse_event`: `if (data_len > 0 && !data) return -1;`
2. **Fix M-2** — Fix h2_client partial header leak: set `st->resp.num_headers = i;` before early return.
3. **Fix L-7** — Add NULL checks to SSE public functions.
4. **Fix L-5** — Add overflow guard to h2_client header allocation.

### Test Improvements

1. Add direct `kl_sse_end` test through SSE API.
2. Add SSE NULL data edge case test (after fixing M-3).
3. Add connection state machine unit tests.
4. Add allocation failure injection via custom allocator.
