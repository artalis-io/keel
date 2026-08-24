# Phase 8d, HTTP/2 over the completion loop, Design

**Status:** designed. The real h2-over-completion deferred from 8c-4 (which only made
the *refusal* clean). Builds on 8b (the platform-independent completion axis + driver)
and 8b-5/8c (TLS + its response paths over completion).

**Scope goal (per the request):** the design must **not percolate to the readiness
API** (h2's readiness entry points and the `KlH2ServerSession` vtable keep their
signatures and behavior), and **IOCP / platform specifics must not percolate to the
abstract completion axis** (`completion.h`/`completion_driver.c` stay Win32-free; IOCP
stays in `event_iocp.c`).

**And the overriding principle: the public h2 API masks the event axis.** An h2 user
configures a server + `KlH2ServerConfig` (session factory, callbacks) and it works;
they never see, choose, or name "readiness" vs "completion." The server internally
selects the readiness h2 drive (`on_readable`/`on_writable`) or the completion h2 drive
(`comp_h2_drive`) by `kl_event_caps()`, behind the *same* `kl_server_run`. This is
already exactly how HTTP/1.1 is masked (the `io_engine` seam:
`kl_io_engine_run_completion` vs the readiness loop, both behind one `kl_server_run`);
8d must extend that masking to h2, adding **nothing** event-axis-shaped to any
h2-user-facing type or call. If a change would make an h2 user aware of the loop model,
it is wrong.

---

## 1. What exists (investigation result)

The server h2 module (`src/h2.c`, `include/keel/h2_server.h`) is a **state machine
driving a pluggable `KlH2ServerSession` vtable** (`recv` / `submit_response` /
`want_write` / `flush` / `shutdown` / `destroy`). Its transport touchpoints are exactly
two, both **TLS-aware** (`src/internal.h`):

- **Input:** `kl_h2_server_on_readable(c)` → `conn_read(c, read_buf, cap)` →
  `session->recv(read_buf, nr)` (parses frames inline, invokes `on_request` etc.).
- **Output:** the session's `flush()` calls the `send` callback (`h2_cb_send`) →
  `conn_write(data, len)`. Short writes are the session's responsibility (it re-queues
  and retries on the next `flush`).

The readiness loop (`server.c`) calls `on_readable`/`on_writable` on FD readiness and
re-arms READ|WRITE based on `session->want_write()`.

**The pivotal fact:** in completion-TLS mode the memory BIO (8b-5) already inverts the
transport: `conn_read → tls->read` returns plaintext *decrypted from the fed input
ring* (the backend fed ciphertext via `feed_input`), and `conn_write → tls->write`
appends ciphertext *to the output ring* (drained by the driver). **So h2's existing
`conn_read`/`conn_write` already do the right thing over completion; the only missing
pieces are (a) owning the read loop and (b) draining the output ring; exactly what the
HTTP/1.1 completion driver already does.** No h2 session-vtable change; no new TLS op.

---

## 2. Design

### 2.1 The one seam: factor session-driving out of the transport read (8d-0)

`on_readable` couples "read from transport" with "drive the session." Split the driving
half into an **internal** helper (declared in `src/internal.h`/`conn_internal.h`, *not*
in `include/keel/`, zero public-API change):

```c
/* Drive the h2 session with already-received plaintext: parse frames + flush any
 * produced output. Returns the next KlConnState (HTTP2 / CLOSED). */
KlConnState kl_h2_server_feed(KlConn *c, const void *data, size_t len);
```

`kl_h2_server_on_readable` becomes `conn_read` + `kl_h2_server_feed(c, read_buf, nr)`:
**behavior identical**; the readiness event loop, the public `kl_h2_server_*` API, and
the `KlH2ServerSession` vtable are all unchanged. This is the additive, non-percolating
seam, and it is **POSIX-testable** (the existing readiness h2 tests exercise it).

### 2.2 The completion h2 driver (8d-1, TLS-ALPN): in `completion_driver.c`

- **Re-enable h2 on completion:** drop 8c-4's `nc->h2_config = NULL`. h2 ALPN may now
  negotiate; `kl_conn_on_handshake` performs the upgrade (allocates the session, and its
  initial SETTINGS are produced through `conn_write → tls->write` into the output ring).
- **Handshake → HTTP2:** `comp_tls_drive`, on `kl_conn_on_handshake` returning
  `KL_CONN_HTTP2`, drains the initial SETTINGS with `comp_tls_flush` and posts the first
  recv (instead of closing).
- **`comp_h2_drive(c)`**, the read loop, mirroring `comp_tls_drive`'s HTTP/1.1 loop:
  ```
  for (;;) {
      p = tls->read(read_buf, cap);         // plaintext from the fed input ring
      if (p < 0) close; if (p == 0) { post_recv; return; }   // WANT_READ
      st = kl_h2_server_feed(c, read_buf, p);// parse frames + flush output → ring
      if (comp_tls_flush(c) < 0) close;      // drain output ring → socket
      if (st != KL_CONN_HTTP2) { handle transition/close; return; }
      if (!tls->pending || tls->pending == 0) { post_recv; return; }
  }
  ```
- **Routing:** `comp_on_read` sends h2 conns (`c->state == KL_CONN_HTTP2`) to
  `comp_h2_drive`; `comp_after_state`/`comp_tls_drive` route the `KL_CONN_HTTP2`
  transition into the h2 setup (drain SETTINGS + post recv) rather than closing.

Output is **synchronous** (`comp_tls_flush` = drain ring + blocking send), matching the
streaming subset's documented head-of-line caveat; true per-stream overlapped output is
8d-3.

### 2.3 Plaintext h2c over completion (8d-2, optional)

The plaintext completion path (`comp_drive_reading`) can transition to `KL_CONN_HTTP2`
via prior-knowledge preface or an h2c `Upgrade`; route that to `comp_h2_drive` with the
plaintext transport (`conn_write → kl_sock_send`, blocking/synchronous). Lower priority
, h2 in practice is ALPN-over-TLS.

### 2.4 Overlapped h2 output (8d-3, optional)

Replace the synchronous `comp_tls_flush` with overlapped, sequenced sends of the drained
ciphertext (as 8c-1 did for HTTP/1.1 buffered responses), removing the head-of-line
caveat for h2. Deferred; the multiplexed output makes sequencing non-trivial.

---

## 3. Orthogonality litmus (both axes)

- **Axis 1: no readiness-API percolation.** `kl_h2_server_on_readable`/`on_writable`/
  `drain_shutdown`/`cleanup`/`upgrade*` keep their signatures **and behavior**;
  `kl_h2_server_feed` is a new **internal** helper (not in `include/keel/`). The
  `KlH2ServerSession` vtable is unchanged, a session backend written for readiness
  works over completion unmodified. **Zero `include/keel/*.h` change.**
- **Axis 2: no IOCP/platform percolation into the abstract axis.** `comp_h2_drive`
  lives in `completion_driver.c` and drives only `kl_h2_server_feed` + the `KlTls` vtable
  + `comp_tls_flush`/`kl_comp_post_recv`. **`event_iocp.c` is unchanged** (h2 rides the
  existing recv/send ops); `completion.h` is unchanged; no Win32 symbol enters the
  abstract driver.
- **Axis 3, the public API masks the event philosophy.** No h2-user-facing type or
  call gains anything event-axis-shaped. `KlH2ServerConfig`, the factory, the callbacks,
  and `kl_server_run` are identical whether the loop is epoll/kqueue/poll/WSAPoll
  (readiness) or IOCP (completion); the server picks the drive internally via
  `kl_event_caps()`, exactly as HTTP/1.1 already does behind the `io_engine` seam. An h2
  user cannot tell which loop they are on, and writes no axis-specific code.
- **POSIX byte-identical.** The completion TUs aren't compiled on POSIX; 8d-0 is a pure
  refactor that keeps `make test` green.

---

## 4. Honest validation gap (bigger than TLS, read before implementing)

TLS-over-completion is BYO-once: mbedTLS is bring-your-own but *available*, so the
runtime is locally validatable (the 8b-5a buffered-BIO smoke, and a local Windows run).

**h2-over-completion is doubly BYO.** The server h2 backend is a pluggable vtable with
**only stub sessions in the repo**: there is no built-in nghttp2 server wrapper. So
running h2-over-completion end-to-end needs **both** mbedTLS **and** a BYO nghttp2
`KlH2ServerSession`. Consequences:

- **8d-0** (the `feed` refactor) is genuinely validated, the readiness h2 tests (mock
  session) exercise it on POSIX/CI.
- **8d-1+** (the completion h2 driver) can only be **compile-gated** (MinGW `-Werror`) +
  proven POSIX-byte-identical. Its *runtime* is unvalidatable in this repo without first
  writing an nghttp2 server session, a larger gap than any prior increment.

**Mitigation option, a mock/loopback completion backend.** A small in-memory backend
implementing `completion.h` (post/drain over a loopback socketpair, no Win32) would let
`comp_h2_drive`, and the *entire* completion driver (HTTP/1.1, TLS, UDP), run under
`make test`/CI with the existing **mock** h2 session and a mock TLS. This would close the
runtime-validation gap for the whole completion axis, not just h2. It is arguably its own
increment (8d-0.5) and the highest-leverage way to make 8d, and everything in 8b/8c;
actually testable rather than compile-gated.

---

## 5. Staging

| Increment | Content | Validation |
|---|---|---|
| **8d-0** ✅ | `kl_h2_server_feed` internal seam; `on_readable` = `conn_read` + `feed` | POSIX: readiness h2 tests (mock session); real |
| **8d-0.5** ✅ | portable `poll()` completion backend (`event_pollcomp.c`, `BACKEND=pollcomp`) implementing `completion.h` | `smoke_pollcomp` runs the real completion driver on Linux/macOS under CI; 8b/8c now runtime-tested, not compile-gated |
| **8d-1** ✅ | `comp_h2_drive`; revert 8c-4 clear; route `KL_CONN_HTTP2` (ALPN + h2c) | POSIX via pollcomp (echo h2 session, h2c Upgrade roundtrip) + MinGW compile-gate; real |
| **8d-2** ✅ | h2 prior-knowledge preface over completion | POSIX via pollcomp (preface roundtrip): real |
| **8d-3** ✅ | overlapped h2 output (send-ordering: one send in flight); plaintext + TLS | POSIX via pollcomp (plaintext, ASan) + MinGW compile-gate (TLS) |

*8d-3 done:* h2 output is now overlapped, not synchronous. A single feed's produced frames
go out as **one ordered overlapped send**, and the next recv is deferred until that send
completes (`comp_on_write`), so **at most one h2 send is ever in flight** and frames
cannot reorder (the ordering guarantee the h2 wire requires). Plaintext captures the
session's output via an internal per-conn buffer (KEEL's `h2_cb_send` appends to it when
the driver sets `out_capture`, instead of `conn_write`); TLS drains the memory-BIO out
ring. Readiness is byte-identical. Plaintext is CI-tested over pollcomp (ASan-clean); TLS
is compile-gated.

*8d-4 (cleanup), the concession is resolved.* 8d-3 initially needed per-conn output
state, placed as additive fields on the KEEL-internal `KlH2ServerConn`; 8d-4 removes that
smell in two moves. (1) `KlH2ServerConn`/`KlH2ServerStream` are now **opaque**, their
bodies moved from the public `<keel/h2_server.h>` into `src/h2_internal.h` (only
`connection.c`/`server.c`/`h2.c` + the white-box h2 tests include it), so internal h2
state is **no longer a public-API change** and the public header actually *shrank*. (2)
The capture fields + the `h2_cb_send` branch are replaced by a generic **output-writer
seam** (`KlH2WriteFn out_write`/`out_ctx`, default = a `conn_write` wrapper), symmetric
with the WebSocket server's `kl_drain` boundary; the completion driver installs its own
buffering writer around a feed (`kl_h2_server_set_writer`) and owns the buffer + growth
in `completion_driver.c`. Result: **h2.c has zero completion state or logic**; just the
seam, and 8d makes **no `include/keel/` addition** at all. **Phase 8d complete.**

*8d-0.5 done:* rather than a throwaway mock, `event_pollcomp.c` is a genuine second
completion backend: it implements the full `completion.h` contract (accept / read /
TLS-recv / write / sendfile / udp-recv / udp-send + drain + prime) as a completion
facade over `poll()`, the mirror image of `event_iouring.c` adapting completion to the
readiness interface. `completion_driver.c` is reused **verbatim** (the proof the axis is
platform-independent, not IOCP-specific), and `smoke_pollcomp`, a full GET + POST body
+ file (sendfile) + chunked stream + UDP echo roundtrip; now runs it on Linux/macOS in
CI (the "Completion (poll)" job) and clean under ASan/UBSan. Every completion increment
since 8a is now genuinely runtime-validated on POSIX, not just compile-gated. 8d-1 (the
h2 completion driver) can now be tested the same way with the mock h2 session.
