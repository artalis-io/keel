# Keel HTTP Client/Server Library — C Code Audit Report

**Date:** 2026-07-16
**Previous Audit:** 2026-03-13
**Auditor:** Claude Opus 4.8 (automated, 6-agent parallel sweep + manual verification)
**Scope:** All source files in `src/` (36 `.c`), `parsers/` (2 `.c`), and `include/keel/` headers. Uncommitted UNIX-domain-socket work in `src/server.c` / `include/keel/server.h` reviewed with extra scrutiny. Test files checked for coverage.

## Summary

| Severity | Count | Status |
|----------|-------|--------|
| Critical | 0     | —      |
| High     | 0     | —      |
| Medium   | 4     | **all fixed** |
| Low      | 7     | **all fixed** (L1–L4, L6, L7, L10) |
| Low (withdrawn) | 3 | L5, L8, L9 — traced as non-issues |
| Info     | 5     | advisory (open) |
| **Total real** | **11** | 11 fixed |

**Files scanned:** 38 source `.c` + 2 parser `.c` (~15,159 LOC)
**Test suites:** 43 (`tests/test_*.c`), including new `tests/test_unix_socket.c` (6 cases)

No memory-safety vulnerabilities (buffer overflow, use-after-free, double-free, OOB read/write, missing NUL terminator) were found. All findings are resource leaks, missing resource caps, protocol-conformance gaps, or robustness hardening.

### Remediation status (2026-07-16)

All four Medium findings **fixed**, and the fixes independently **re-audited (adversarial second pass) with no regressions**. Full suite (42 test binaries) passes under `-Werror` on the kqueue **and** poll backends, and clean under ASan + UBSan (`make debug-test`):

- **M1** — `client.c`: `decomp_installed` flag added; `cleanup:` now calls `kl_decompress_stream_free(&decomp_wrap.ds)` (idempotent) on all exit paths. Re-audit confirmed: flag is initialized before any `goto` that skips the `decomp_wrap` declaration, so no uninitialized read; no double-free (idempotent free + parser destroyed before cleanup).
- **M2** — `decompress_miniz.c`: added `KL_DECOMP_MAX_OUTPUT` (256 MB, `-D`-overridable) ceiling. One-shot growth loop caps `new_size` and rejects at the ceiling; early-rejects when the untrusted ISIZE exceeds it. Streaming path tracks a non-wrapping `uint64_t out_full` and aborts before `emit` once the cap is exceeded. Re-audit confirmed: no no-progress/infinite-loop at the clamp boundary, cap-boundary output still succeeds, `out_full` reset on every reuse path, CRC/ISIZE checks intact.
- **M3** — `websocket_client.c`: outbound `KlDrain` buffer (4 MB cap) added. `wsc_send_frame` writes header+payload through the drain; `KL_EVENT_WRITE` interest is armed while data is pending and flushed on write-readiness. Fixes both plain-socket EAGAIN and TLS `WANT_*` backpressure without frame truncation or connection abort. `wsc_write_all` (which mis-folded would-block into a hard error) removed. Re-audit confirmed: interest arm/drop correct, no read starvation, no UAF in the `WSC_CLOSED` guard, no drain leak/double-free, frame ordering preserved.
- **M4** — `event_poll.c`: `kl_event_add` is now idempotent — a re-add updates the existing slot in place instead of orphaning it (matches epoll/kqueue upsert semantics). Re-audit confirmed correct for all states; does not corrupt the `kl_event_del` swap-compaction.

### Corrections to the 2026-07-16 first pass (from the re-audit)

- **L8 (decompress gzip trailer skipped when >8 trailing bytes) — WITHDRAWN, false positive.** The `break` on `TINFL_STATUS_DONE` sits *outside* the `remaining <= 8` capture block, so control still reaches the trailer-accumulation block (`if (s->done && remaining > 0 && s->trail_len < 8)`), which captures the full 8-byte trailer; CRC32 + ISIZE are verified normally on flush. (Residual: bytes beyond the 8-byte trailer are discarded rather than parsed as a concatenated gzip member — a multi-member-gzip gap, not a verification skip. Now tracked as **I5**.)
- **L9 (resolver_cache `count` drift) — WITHDRAWN, false positive.** When `cache_lookup` frees an expired slot (`occupied=0; count--`) it becomes a genuine free slot, which the next `cache_insert` reclaims in step 2 (`count++`) before steps 3/4 ever run. The counter is at most transiently off-by-one and self-corrects; it gates no logic (informational getter only).
- **L6 framing corrected:** a *full* CRLF cannot survive multipart line-splitting; the real gap is embedded single control bytes (lone `\r`/`\n`/`\t`/`\0`) in `name`/`filename`. Reworded below.

### Low-findings remediation (2026-07-16, second pass)

All seven real Low findings **fixed**; the three remaining were **withdrawn** after tracing (L5 not reachable under oneshot io_uring poll; L8/L9 false positives). Native fixes (L1, L2, L4, L6, L7, L10) build clean under `-Werror` on kqueue and poll and pass 42/42 suites under ASan + UBSan. The io_uring-only fix (L3) is a guarded bounds check, not compile-tested locally.

- **L1** — `server.c`: `umask(0777 & ~mode)` around `bind()` makes the socket node's mode atomic; chmod retained for exact bits.
- **L2** — `server.c`: `lstat`+`S_ISSOCK` guard before the teardown `unlink`.
- **L3** — `file_io_iouring.c`: reject `len > UINT_MAX` before splice/read SQE prep (guarded for 32-bit `size_t`).
- **L4** — `websocket_client.c`: handshake buffer keeps a spare byte and is NUL-terminated after each read.
- **L6** — `body_reader_multipart.c`: `mp_has_ctl` rejects control bytes (`<0x20`/`0x7F`) in `name`/`filename`.
- **L7** — `websocket.c`: shared frame parser rejects reserved opcodes (0x3–0x7, 0xB–0xF) — covers server + client.
- **L10** — `async.c`: `kl_watcher_add` is now idempotent (in-place update on duplicate fd).

The Info items (I1–I5) are advisory and remain open.

## Changes Since Last Audit

- **New: UNIX-domain-socket server transport** (`src/server.c`, `include/keel/server.h`) — `KlTransport` enum, `unix_socket_path`/`unix_socket_unlink`/`unix_socket_mode` config, and bind/listen/teardown refactor (`kl_server_bind_tcp`, `kl_server_bind_unix`, `kl_server_unlink_stale_unix_socket`, `kl_server_close_listener`). New `tests/test_unix_socket.c` (6 cases).
- The new code is well-constructed: `sun_path` bounds check leaves room for the NUL, `addr_len` includes the terminator, fds are closed and reset to `-1` on every error path, `lstat`+`S_ISSOCK` guards the pre-bind unlink (won't follow a symlink or delete a regular file), and chmod-failure cleanup is correct. Only two minor TOCTOU/atomicity windows remain (M-nil; see L1/L2).

---

## Medium Findings

### M1 — `client.c`: streaming decompressor session leaked on sync error / EOF-success paths
**File:** `src/client.c` — setup at `858-877`, activation in `decomp_on_headers` (~`690-698`), leak at the `cleanup:` label `917-927`.
**Verified.** In the sync API, `decomp_wrap.ds.decomp` (a heap `KlDecompress` created by the factory) is freed **only** inside `decomp_on_complete()` → `kl_decompress_stream_free()`, which the parser calls solely on message-complete (`KL_PARSE_OK`). If `recv_response_sync()` exits via poll timeout, I/O error, `KL_PARSE_ERROR`, or a reset **after headers parsed but before body completion**, the session leaks. It also leaks on a *successful* EOF-terminated response (returns `ret=0` without firing `on_complete`). The async path handles this correctly in `kl_client_free` (checks `decomp_wrap->active`); the sync path has no equivalent guard.
**Fix:** At `cleanup:`, if the wrapper was installed (`actual_stream == &wrapped_stream && decomp_wrap.active`), call `kl_decompress_stream_free(&decomp_wrap.ds)`. It is idempotent (`decompress.c` nulls `ds.decomp`), so freeing after a normal `KL_PARSE_OK` is safe.

### M2 — `decompress_miniz.c`: no output-size cap → decompression-bomb exposure
**File:** `src/decompress_miniz.c` — one-shot growth loop `158-174`; streaming `miniz_dfeed_fn` `284-320`.
**Verified.** The one-shot growth loop doubles `buf_size` with no ceiling other than `SIZE_MAX/2`, and the CRC/ISIZE check runs only *after* the full output is materialized — a ~1 KB crafted `Content-Encoding: gzip` body can force unbounded allocation on the client before any integrity check. The streaming path is worse: it emits through `emit` in an unbounded loop with no cumulative cap, and its only guard (`s->total_out`, a `uint32_t`) silently wraps at 4 GB, so the flush-time `total_out != expected_isize` check can be defeated. `expected_isize` is used only as an initial size *hint*, never as an enforced limit.
**Fix:** Add a configurable `max_output` ceiling. In the one-shot loop, reject when the next `buf_size`/`out_pos` would exceed it (and reject early when `expected_isize > max_output`). In the streaming path, track cumulative output in `size_t`/`uint64_t` and abort in the loop before `emit` once the cap is exceeded.

### M3 — `websocket_client.c`: no outbound backpressure; partial write on non-blocking socket tears down the connection
**File:** `src/websocket_client.c` — `wsc_write_all` `117-132`; callers `354`, `367`.
**Verified.** `wsc_write_all` returns `-2` on `EAGAIN/EWOULDBLOCK`, but every caller checks only `< 0`, folding would-block into a hard `-1`. There is no outbound drain buffer (unlike the server's `KlDrain`), so under real backpressure (slow peer, TLS `WANT_WRITE`) a frame whose header or payload chunk was partially sent cannot be resumed — the send fails and the connection is aborted. This is a *clean abort* (not silent frame corruption, since the caller returns error and tears down), so it is Medium rather than High, but the client has no working partial-write story for masked frames.
**Fix:** Give the WS client an outbound drain buffer (mirror `KlDrain`), or at minimum handle `-2` distinctly — buffer the unsent tail and retry on write-readiness rather than dropping the connection on the first would-block.

### M4 — `event_poll.c`: re-adding an already-registered fd orphans a slot and keeps polling a stale fd
**File:** `src/event_poll.c` — `kl_event_add` `122-140`.
**Verified.** `kl_event_add` never checks `fd_to_idx[fd] >= 0`; a second add for the same fd appends a new slot and overwrites `fd_to_idx[fd]`, orphaning the first slot. The `kl_event_del` swap-compaction (`172`) then only fixes the index for one duplicate, leaving a stale slot that keeps the fd in `poll()` — a potential poll-on-closed-fd if the fd is later closed. The epoll (`EPOLL_CTL_ADD` → `EEXIST`) and kqueue (`EV_ADD` upsert) backends tolerate re-add; poll does not. Reachability is low (server does del-before-add), so this is a latent robustness bug.
**Fix:** In `kl_event_add`, if the fd is already registered, update in place (delegate to `kl_event_mod`) instead of appending.

---

## Low Findings

### L1 — `server.c`: `unix_socket_mode` chmod is not atomic (permission window after bind) — **FIXED**
**FIXED (2026-07-16):** `kl_server_bind_unix` now sets `umask(0777 & ~mode)` around `bind()` (saved/restored immediately after), so the socket node is created atomically with the requested mode; the existing `chmod` is kept to guarantee the exact bits regardless of the platform's socket-creation base. Closes the bind→chmod window in which a restrictive `unix_socket_mode` (e.g. `0600`) was not enforced.

### L2 — `server.c`: teardown `unlink` has no `S_ISSOCK` re-check — **FIXED**
**FIXED:** `kl_server_close_listener` now does `lstat` (not `stat`, so it won't follow a symlink) + `S_ISSOCK` before the teardown `unlink`, so a regular file or a different process's socket that replaced the path is not removed.

### L3 — `file_io_iouring.c`: `size_t len` truncated to 32-bit for splice/read — **FIXED**
**FIXED:** `iouring_fio_submit` rejects `len > UINT_MAX` (guarded `#if SIZE_MAX > UINT_MAX` to avoid a tautological comparison where `size_t == unsigned`) before any SQE prep; this covers both the splice-in and read casts, and the phase-2 splice length is derived from already-bounded bytes. *Not compile-tested locally (Linux/liburing only) — change is a trivial guarded bounds check.*

### L4 — `websocket_client.c`: handshake response buffer not NUL-terminated — **FIXED**
**FIXED:** the grow trigger is now `handshake_len + 1 >= handshake_cap` (always keeps a spare byte), the read reserves the last byte (`handshake_cap - handshake_len - 1`), and `handshake_buf[handshake_len] = '\0'` is written after each read — so `strstr`/`strncmp`/`strncasecmp` can no longer over-read past the buffer.

### L5 — WITHDRAWN (not reachable under oneshot poll; was: io_uring wakeup loss)
Re-traced against the actual arming model: KEEL uses **oneshot** `io_uring_prep_poll_add` (not multishot). In `kl_event_wait` the `count >= max` break happens *before* `seen++` and *before* the slot-clear, so an unreportable CQE is left in the ring (reprocessed next tick) with its slot intact; the slot-clear (`fd_udata[fd] = UDATA_UNUSED`) only runs for CQEs actually reported, and the caller's rearm restores the slot. No CQE is ever both cleared and dropped. Deliberately **not** changing concurrency-critical Linux-only code on a speculative basis. (If multishot poll is ever adopted, revisit the dedup-clear.)

### L6 — `body_reader_multipart.c`: extracted `name`/`filename` may contain single control bytes — **FIXED**
**FIXED:** new `mp_has_ctl` helper rejects any byte `< 0x20` or `== 0x7F` in the extracted `name` and `filename` (returns MALFORMED before `mp_strdup`). Blocks lone `\r`/`\n`/`\t`/`\0` (log-injection / NUL-truncation vectors) that survive quoting.

### L7 — `websocket.c` / `websocket_client.c`: reserved opcodes not rejected — **FIXED**
**FIXED:** `kl_ws_frame_parse` (the parser shared by server and client) now rejects reserved opcodes — `(opcode > 0x2 && opcode < 0x8) || opcode > 0xA` → `-1` — immediately after the existing RSV-bits check. Only `0x0-0x2` (data) and `0x8-0xA` (control) are accepted, per RFC 6455 §5.2. Verified the client's `wsc_process_frames` calls the same parser, so one fix covers both.

### L8 — WITHDRAWN (false positive; was: decompress gzip trailer skip)
The re-audit refuted this — the `break` is outside the `remaining <= 8` block and the trailer-accumulation block still captures the full 8-byte trailer; CRC/ISIZE are verified. See "Corrections" above. (Residual multi-member-gzip gap tracked as **I5**.)

### L9 — WITHDRAWN (false positive; was: resolver_cache `count` drift)
The re-audit refuted this — an expired slot freed by `cache_lookup` becomes a free slot that the next `cache_insert` reclaims via step 2 (`count++`) before steps 3/4 run; the counter self-corrects and gates no logic. See "Corrections" above.

### L10 — `async.c`: `kl_watcher_add` does not guard against a duplicate fd — **FIXED**
**FIXED:** `kl_watcher_add` now scans the watcher list first; if `fd` is already registered it updates the existing `KlWatcher` in place (mask/callback/user_data), sets `dispatch_dirty`, and calls `kl_event_mod` — mirroring the M4 event_poll upsert. No duplicate node, no orphan.

`src/async.c` (`kl_watcher_add` `56-79`). The function unconditionally prepends a new `KlWatcher` and calls `kl_event_add`. On a double-add of the same fd: the epoll backend rejects (`EPOLL_CTL_ADD` → `EEXIST` → the node is unlinked/freed and `-1` returned), but kqueue (`EV_ADD` upsert) and now poll (idempotent in-place update, per the M4 fix) accept it — the event-loop slot points at the *new* watcher while the *first* `KlWatcher` node is orphaned in the list (leaked until `kl_event_ctx_free`, never receives events, and a later `kl_watcher_del` removes the wrong node). Pre-existing and low-reachability (KEEL internally always `del`s before `add`), but the M4 fix makes the poll backend's tolerance of double-add consistent with kqueue, so the list-layer inconsistency is now the sole remaining gap. **Fix:** in `kl_watcher_add`, if `fd` is already registered, update the existing `KlWatcher` in place (mask/callback/user_data) and `kl_event_mod`, mirroring the event_poll idempotency.

---

## Informational

- **I1 — `parser_llhttp.c` continuation pointer arithmetic** (`39`, `68`, `94`): computes fragment length as `(at+len) - original_ptr`, valid only if llhttp delivers callback fragments from one contiguous buffer within a single `llhttp_execute`. Safe given the connection layer feeds one growable buffer capped by `max_header_size`, but the invariant is implicit — worth an assert/comment. Verify the connection layer never feeds incremental slices without re-basing pointers.
- **I2 — `server.c`: `config.port` not range-validated** before `snprintf` into `port_str[8]`. Truncation is memory-safe but yields a confusing `getaddrinfo` failure for out-of-range ports. Consider rejecting `port < 0 || port > 65535`.
- **I3 — `response.c`: `format_content_length` relies on implicit 64-bit `size_t` bound** for its 48-byte buffer (safe on all supported platforms). A `_Static_assert` documenting the 38-byte worst case would harden it.
- **I4 — `websocket_client.c`: masking-key PRNG fallback** to `srand(time)`/`rand()` when `/dev/urandom` is unavailable. Correct per RFC 6455 (masking is anti-cache-poisoning, not a security boundary) — no action.
- **I5 — `decompress_miniz.c`: streaming path ignores concatenated gzip members.** Bytes past the first 8-byte trailer (a valid RFC 1952 multi-member stream) are discarded rather than decoded. Integrity of the first member is still verified. Only relevant if a peer sends multi-member gzip; document as unsupported or handle a second member.

---

## Files Verified Clean

`connection.c`, `router.c`, `chunked.c`, `body_reader_buffer.c`, `cors.c`, `url.c` (correct `strtol` range validation + CRLF guards), `parser_llhttp.c` (CL/TE smuggling defense correct), `response_parser_llhttp.c`, `h2.c`, `h2_client.c` (all size arithmetic overflow-guarded, stream lifecycle/`_free` pairing intact), `redirect.c` (memset-before-error avoids UAF; cross-origin auth stripping correct; loop termination sound), `compress.c`, `compress_miniz.c`, `drain.c`, `sse.c`, `timer.c` (re-entrant-safe min-heap), `async.c` (`kl_async_complete` contract correct; watcher-list manipulation sound except the double-add gap L10), `thread_pool.c` (create/shutdown unwind correct; queues bounded), `event_epoll.c`, `event_kqueue.c`, `file_io.c`, `tls_mbedtls.c` (all `fail:` ladders free the six mbedTLS objects; key buffer `kl_secure_zero`'d), `allocator.c`, `error.c` (table matches enum, bounds-checked), `client_pool.c` (eviction/accounting consistent, no UAF/double-free).

**No unsafe libc** (`strcpy`/`strcat`/`sprintf`/`gets`/`atoi`/`atol`/`atof`) anywhere in the tree — only bounded `snprintf`/`memcpy` with checked returns.

---

## Recommendations (priority order)

1. ~~M1–M4~~ — **done and re-audited** (see Remediation status).
2. ~~L1–L4, L6, L7, L10~~ — **done** (see Low-findings remediation); L5/L8/L9 withdrawn.
3. **Add regression tests** for the two behavior-changing fixes: L6 (multipart part with a control byte in `filename` → rejected as MALFORMED) and L7 (WS frame with a reserved opcode → connection failed). Both are cheap and lock in the new validation.
4. **L3** — verify on a Linux/liburing build (the guard was not compile-tested locally).
5. Optional advisory: **I2** (port range check), **I1** (document the llhttp contiguous-buffer invariant), **I5** (multi-member gzip).
