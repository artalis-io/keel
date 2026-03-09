# Keel HTTP Server Library — C Code Audit Report

**Date:** 2026-03-09
**Previous Audit:** 2026-03-05
**Auditor:** Claude Opus 4.6 (automated)
**Scope:** All source files in `src/`, `parsers/`, `include/keel/`, and internal headers (`src/*.h`). Test files in `tests/` checked for coverage gaps.

## Summary

| Severity | Count |
|----------|-------|
| Critical | 0     |
| High     | 0     |
| Medium   | 5     |
| Low      | 4     |
| Info     | 6     |
| **Total**| **15**|

**Files scanned:** 35 (18 source `.c`, 17 headers `.h`)
**Test suites:** 16 (286 UTEST cases)
**Lines of code (approx):** ~6,000 (src + parsers), ~2,000 (headers), ~8,000 (tests)

## Changes Since Last Audit (2026-03-05)

### New Modules
- `src/async.c` / `include/keel/async.h` — KlWatcher (FD callbacks) and KlAsyncOp (connection suspension)
- `src/thread_pool.c` / `include/keel/thread_pool.h` — Worker thread pool with pipe-based event loop wakeup

### Resolved Issues (9 of 23)
| ID | Issue | Resolution |
|----|-------|------------|
| H-1 | io_uring POLL_ADD one-shot not re-armed | **FIXED** — `kl_watcher_rearm()` added; called after every watcher callback in `server.c:305`. Watchers now store their event mask for re-arming. |
| H-2 | io_uring kl_event_mod SQE exhaustion | **MITIGATED** — Flush-before-mod (line 115-119) + retry-once on second SQE failure (lines 129-136). Downgraded to M-5. |
| M-2 | WebSocket Sec-WebSocket-Key not validated as base64 | **FIXED** — Base64 alphabet validation loop added after length check in `websocket.c:340-350`. |
| M-3/M-4 | writev_all/conn_write_all spin loop hardcoded | **FIXED** — Named constant `KL_CONN_WRITE_SPIN_MAX` defined in `internal.h:12`. Used by `conn_write_all`; `response.c` retains its own `KL_WRITE_SPIN_MAX` (same value). |
| M-7 | listen_fd not set to -1 on event_init failure | **FIXED** — `s->listen_fd = -1` added at `server.c:244`. |
| L-3 (old) | H2 route params not copied to req->params | **FIXED** — `memcpy` + `num_params` copy added at `h2.c:313-315`. |
| L-3 (new) | H2 file response limited to 16 MB without error | **FIXED** — Files > 16 MB now return 500 with "File too large for HTTP/2 response" error in `h2.c:143-151`. |
| L-7 | ws_msg_grow overflow check missing | **FIXED** — `if (additional > SIZE_MAX - ws->msg_len) return -1;` added at `websocket.c:430`. |

## Overall Assessment

The Keel codebase is well-engineered with strong security discipline. Key strengths:

- **No unsafe string functions:** Zero uses of `strcpy`, `strcat`, `sprintf`, `gets`, `atoi`, `atol`, or `atof` in the source tree (only `snprintf` with bounded buffers in CORS and WebSocket).
- **Consistent allocator discipline:** All core allocations go through `kl_malloc`/`kl_free`/`kl_realloc`. The only raw `malloc`/`free` calls are in `tls_mbedtls.c` (startup-only, clearly documented) and `allocator.c` (stdlib backend implementation).
- **Integer overflow guards:** Systematic `SIZE_MAX/2`, `INT_MAX/2` checks before multiplications and capacity doublings across all modules (30+ guard sites).
- **All 20+ kl_malloc calls NULL-checked** within 3 lines of allocation.
- **Header injection prevention:** `contains_crlf()` guard on all response header values.
- **Request smuggling mitigation:** CL/TE priority correctly enforced in `parser_llhttp.c` (line 126-127).
- **SIGPIPE handled:** `signal(SIGPIPE, SIG_IGN)` in `server.c`.
- **Three-layer body size limits:** Content-Length early reject, chunked accumulation tracking, per-route buffer limits.
- **Thread pool safety by construction:** Workers never touch event loop state; done_fn always runs on event loop thread via pipe wakeup.
- **Build hardening:** `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -D_FORTIFY_SOURCE=2 -fstack-protector-strong`, debug builds with ASan+UBSan, static analysis targets, and fuzz targets (parser, multipart, WebSocket).

---

## Critical Issues

None found.

---

## High Issues

None (both previous High issues resolved/mitigated).

---

## Medium Issues

### M-1: tls_mbedtls.c uses raw malloc/free for file I/O and context allocation

| Field | Value |
|-------|-------|
| **File** | `src/tls_mbedtls.c` |
| **Lines** | 291, 301, 328, 359, 372, 384, 428, 436, 468, 503, 523 |
| **Category** | Allocator Discipline |
| **Impact** | The `read_file()` helper and `ctx_create` functions use raw `malloc`/`free` instead of the `KlAllocator` interface. This is documented as intentional (startup-only, before allocator lifetime begins), but breaks the invariant that all allocations go through the allocator. A custom allocator that tracks allocations for leak detection will miss these. |
| **Suggested Fix** | Accept as a documented exception for startup-only code, or thread an optional `KlAllocator` through the context creation functions. |

### ~~M-2: WebSocket Sec-WebSocket-Key not validated as base64~~ — FIXED

| Field | Value |
|-------|-------|
| **File** | `src/websocket.c` |
| **Lines** | 334-350 |
| **Category** | Input Validation |
| **Status** | **FIXED** — Base64 alphabet validation loop added at lines 340-350. Rejects keys with characters outside `[A-Za-z0-9+/=]`. |

### ~~M-3/M-4: writev_all / conn_write_all spin loop hardcoded~~ — MITIGATED

| Field | Value |
|-------|-------|
| **File** | `src/internal.h`, `src/response.c` |
| **Category** | Network / Performance |
| **Status** | **MITIGATED** — Named constant `KL_CONN_WRITE_SPIN_MAX` (256) defined in `internal.h:12`, used by `conn_write_all`. `response.c` retains its own `KL_WRITE_SPIN_MAX` at the same value. The spin behavior is accepted as-is — EAGAIN yield would require significant refactoring of the write path. |

### M-5: io_uring kl_event_mod can leave fd un-monitored on double SQE failure

| Field | Value |
|-------|-------|
| **File** | `src/event_iouring.c` |
| **Lines** | 107-141 |
| **Category** | Resource Management / Event Loop |
| **Impact** | `kl_event_mod` flushes pending SQEs before the mod (good), and retries once if the second SQE fails (good). But if the retry also fails (line 135-136), the cancel SQE was already submitted and flushed, leaving the fd un-monitored. This is an edge case requiring extreme SQ pressure. |
| **Suggested Fix** | Low priority — the flush-before-mod pattern makes this extremely unlikely. Would require RING_SIZE (256) concurrent mod operations to exhaust the SQ. |

---

## Low Issues

### L-1: kl_request_header returns non-null-terminated pointer (API footgun)

| Field | Value |
|-------|-------|
| **File** | `include/keel/request.h` |
| **Lines** | 51-61 |
| **Category** | API Safety |
| **Impact** | `kl_request_header()` returns a `const char *` that is NOT null-terminated. The function signature looks like it returns a C string, but it doesn't. Users may pass it to `strlen`, `strcmp`, `printf("%s")`, etc. |
| **Suggested Fix** | Consider deprecating or removing in favor of `kl_request_header_len()`. |

### L-2: body_reader_buffer casts user_data to size_t (pointer-to-integer)

| Field | Value |
|-------|-------|
| **File** | `src/body_reader_buffer.c` |
| **Lines** | 58 |
| **Category** | Portability |
| **Impact** | `size_t max_size = (size_t)user_data;` relies on `sizeof(void*) >= sizeof(size_t)`. True on all modern platforms, but technically not guaranteed by C standard. |
| **Suggested Fix** | Accept as-is. Documented pattern. |

### ~~L-3: H2 file response limited to 16 MB without error feedback~~ — FIXED

| Field | Value |
|-------|-------|
| **File** | `src/h2.c` |
| **Lines** | 143-151 |
| **Category** | Resource Limits |
| **Status** | **FIXED** — Files > 16 MB now return HTTP 500 with "File too large for HTTP/2 response" error body, following the same pattern as the streaming-not-supported 500. |

### L-4: H2 stream linear search O(n) per lookup

| Field | Value |
|-------|-------|
| **File** | `src/h2.c` |
| **Lines** | 19-25 |
| **Category** | Performance |
| **Impact** | `h2_stream_find` performs linear scan over streams array. Acceptable for default max of 128 streams, but could bottleneck at high concurrency. |
| **Suggested Fix** | Low priority. Consider hash map if high stream counts are needed. |

---

## Informational Notes

### I-1: Thread pool backpressure correctly prevents done queue overflow

The `inflight` counter in `thread_pool.c` tracks items across all three stages (work queue + executing + done queue). `submit()` rejects when `inflight >= done_cap` where `done_cap = work_cap + num_workers`, guaranteeing the done queue can never overflow.

### I-2: Thread pool shutdown sequence is correct

Shutdown sets flag, broadcasts condvar, joins all workers, then drains remaining done items (calling `done_fn`) and work items (calling `cancel_fn`). Workers that are mid-execution complete normally before the join returns.

### I-3: Watcher re-arm is safe when callback removes watcher

`kl_watcher_rearm()` walks the watcher list to find the fd. If the callback called `kl_watcher_del()`, the watcher is removed from the list and `rearm` is a safe no-op.

### I-4: CORS origin_buf stack buffer matches KL_CORS_ORIGIN_SIZE

The `kl_cors_middleware` function uses `char origin_buf[KL_CORS_ORIGIN_SIZE]` (256 bytes) on the stack, matching the maximum origin length. This is safe.

### I-5: Debug build includes `-fno-omit-frame-pointer`

Good practice for ASan/UBSan diagnostics. The Makefile debug target correctly includes this flag.

### I-6: Fuzz targets cover the three primary attack surfaces

`fuzz_parser` (HTTP parser + chunked decoder), `fuzz_multipart` (multipart parser), and `fuzz_websocket` (WebSocket frame parser) cover all untrusted network input surfaces.

---

## New Module Assessment: async.c

| Check | Status |
|-------|--------|
| NULL parameter checks | `kl_watcher_add`: checks `!s`, `fd < 0`, `!on_ready`. `kl_async_suspend`: checks `!s`, `!conn`, `!op`. |
| kl_malloc NULL check | `kl_watcher_add:22` — checked immediately |
| Matching free | `kl_watcher_del:59` frees with correct size |
| Event mask stored | `w->mask = mask` in both `add` and `mod` |
| Tagged pointer alignment | `watcher_tag()` sets LSB=1; KlWatcher is heap-allocated so always aligned (LSB=0). Safe. |

**Verdict:** Clean. No issues.

## New Module Assessment: thread_pool.c

| Check | Status |
|-------|--------|
| NULL parameter checks | `create`: checks `!s`. `submit`: checks `!pool`, `!item`, `!item->work_fn`, `!item->done_fn`. |
| All kl_malloc NULL-checked | Lines 142, 155, 158, 182 — all checked within 2 lines |
| Matching kl_free | `kl_thread_pool_free:284-287` frees all 4 allocations with correct sizes |
| Pipe FDs closed on error | `fail_pipe` label closes both `pipe_rd` and `pipe_wr` |
| Goto cleanup cascade | Correct reverse-order: threads → watcher → cond → mutex → pipe → done_queue → work_queue → pool |
| Pipe read end non-blocking | Set via `fcntl(O_NONBLOCK)` at line 168-170 |
| Thread join on shutdown | All `num_workers` threads joined at lines 251-252 |
| Mutex held during queue ops | All work_queue and done_queue access under `pool->mutex` |
| Inflight backpressure | `submit` rejects when `inflight >= done_cap` — prevents done queue overflow |
| Partial thread spawn | If some `pthread_create` calls succeed and some fail, `pool->num_workers` is set to `started`. If `started == 0`, cleanup occurs. Only successfully created threads are joined on free. |
| write() return checked | `ssize_t wr = write(...); (void)wr;` — best-effort pipe signal, suppresses warn_unused_result |

**Verdict:** Clean. No issues. Well-structured with comprehensive error path handling.

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
| Clang static analysis | Present | `make analyze` |
| cppcheck | Present | `make cppcheck` |
| libFuzzer targets | Present | Parser + multipart + WebSocket |
| Vendor code `-w` | Present | Relaxed for llhttp |
| `-lpthread` (Linux) | Present | Required for thread pool |

**Missing/Recommended:**

- `-Wconversion` is not set. Would catch implicit narrowing conversions.

---

## Test Coverage Assessment

| Module | Test File | Tests | Coverage |
|--------|-----------|-------|----------|
| allocator | test_allocator.c | 4 | Basic: custom, default, realloc |
| async | test_async.c | 14 | Good: watchers, suspend/resume, deadlines, cancel |
| body_reader | test_body_reader.c | 30 | Excellent: limits, overflow, chunked, multipart |
| chunked | test_chunked.c | 17 | Excellent: sizes, extensions, trailers, overflow |
| connection | test_connection.c | 4 | Minimal: pool ops only |
| cors | test_cors.c | 17 | Excellent: origins, preflight, credentials |
| h2 | test_h2.c | 29 | Good: session, streams, callbacks, middleware |
| integration | test_integration.c | 27 | Good: full request/response paths |
| parser | test_parser.c | 9 | Good: GET, POST, headers, body, chunked |
| response | test_response.c | 14 | Good: modes, CRLF injection, keep-alive |
| router | test_router.c | 27 | Excellent: params, methods, middleware |
| thread_pool | test_thread_pool.c | 12 | Good: create/free, submit, FIFO, stress, cancel, shutdown |
| timeout | test_timeout.c | 4 | Basic: idle, partial, body timeout |
| tls | test_tls.c | 20 | Good: vtable mocking, handshake, ALPN |
| websocket | test_websocket.c | 38 | Excellent: SHA-1, base64, frames, fragmentation |
| overflow | test_overflow.c | 20 | Good: WS frame, conn pool, chunked, multipart, router, body reader |

### Coverage Gaps

1. **No allocation failure injection** — No tests for malloc/realloc failure paths in response header growth, router expansion, body reader realloc.

2. **No dedicated event loop backend tests** — Backends exercised indirectly via integration tests only.

3. **Connection state machine** — Complex state machine in `connection.c` only partially covered by integration tests. Missing: `kl_conn_on_readable`, `kl_conn_on_writable`, `kl_conn_on_handshake` direct tests.

---

## Recommendations

### Priority 1 (Done)

1. ~~**Add `-D_FORTIFY_SOURCE=2`**~~ — **DONE** (Makefile, non-cosmo builds).

2. ~~**Add a fuzz target for WebSocket frames**~~ — **DONE** (`fuzz/fuzz_websocket.c` + seed corpus).

3. ~~**Add overflow boundary tests**~~ — **DONE** (`tests/test_overflow.c`, 20 tests).

### Priority 2 (Low)

4. **Add allocation failure injection** — Custom allocator that fails on Nth call for testing error paths.

5. **Add event loop backend tests** for io_uring re-arm contract.

6. **Consider removing `kl_request_header()`** (L-1) in favor of length-aware `kl_request_header_len()`.
