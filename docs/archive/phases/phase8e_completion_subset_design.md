# Phase 8e: Extending the completion subset: WebSocket + async handlers, Design

**Status:** designed. The completion driver (8a–8d) drives HTTP/1.1, TLS, and h2, but
deliberately **excludes** two connection kinds; WebSocket (`KL_CONN_WEBSOCKET`) and
suspended/async handlers (`KL_CONN_SUSPENDED`); which currently fall through to close.
8e brings them onto the completion loop.

**The two are very different in scope**, and the design says so plainly:

- **8e-1 WebSocket** is tractable; it mirrors h2-over-completion almost exactly, reusing
  an existing transport-agnostic frame core. Testable over pollcomp in CI.
- **8e-2 async** is deeper; it requires the completion loop to become a *full* event
  loop (service `kl_watcher`s, timers, and async-op deadlines), which it is not today.

**Orthogonality litmus (unchanged from 8b–8d):** no readiness-API change (reuse the
existing WS/async entry points); no IOCP/platform symbol in the abstract driver
(`completion_driver.c`); the public API masks the event axis (a WS/async user configures
the same server and never names readiness vs completion). Additive-only if any header
must change.

---

## 8e-1: WebSocket over the completion loop

### What already fits

The WebSocket server is structured exactly like h2 for our purposes:

- **A transport-agnostic frame core already exists:** `kl_ws_server_on_readable_data(c,
  data, len)` (websocket.c) parses frames from bytes handed to it and fires callbacks,
  the direct analogue of `kl_h2_server_feed`. `kl_ws_server_on_readable` is just
  `conn_read` + `on_readable_data` (the readiness wrapper); the completion driver reads
  via its own path and calls `on_readable_data`.
- **I/O goes through `conn_read`/`conn_write`** (the TLS-aware seam), so over
  completion-mode TLS it uses the memory-BIO rings **identically to h2**: no
  socket-specific code. Output backpressure is an optional `KlDrain` (`ws_drain_writer →
  conn_write`), the same boundary h2 got a writer seam for in 8d-4.
- **The timeout/ping sweep already runs on the completion loop:**
  `kl_server_sweep_conn_timeouts` (added in the hardening pass) handles `KL_CONN_WEBSOCKET`
  `kl_ws_server_auto_ping` + `kl_ws_server_check_close_timeout` + `kl_comp_cancel`.

### Design

Mirror `comp_h2_drive` with a `comp_ws_drive`:

1. **Upgrade → WEBSOCKET.** The WS upgrade happens during dispatch (`connection.c`:
   `route->ws_config` → `kl_ws_server_upgrade`, which writes the 101 handshake via
   `conn_write_all`). `comp_after_state` gets `KL_CONN_WEBSOCKET`: reset `read_len`,
   flush any handshake output (TLS ring via `comp_tls_flush`; plaintext already sent),
   post the first recv. (Same shape as the h2c/HTTP2 transition.)
2. **`comp_on_read` routes `KL_CONN_WEBSOCKET` → `comp_ws_drive`.**
3. **`comp_ws_drive`** (mirrors `comp_h2_drive`):
   - *TLS:* decrypt loop (`tls->read`) → `kl_ws_server_on_readable_data(c, buf, n)` →
     drain the produced output ciphertext → send; loop on `tls->pending()` for coalesced
     records; post recv.
   - *Plaintext:* `kl_ws_server_on_readable_data(c, read_buf, read_len)` → output went out
     through `conn_write` during the frame callbacks; post recv.
   - A `KL_CONN_CLOSED` result (close frame / error) → flush any final output, close.

### Output model

Read-driven WS output (frames produced in response to a received frame, inside the
callbacks) is sent **synchronously** for the subset; plaintext via `conn_write`
(blocking), TLS via the ring flush; matching the streaming/h2 subset. Overlapped WS
output is a later refinement (the same writer-seam trick as h2 8d-3, and the WS server
already has a `KlDrain` boundary to hang it on).

### Scope

Read-driven WebSocket (echo / request-response, the common case) is fully in scope.
**Server-initiated async push**, a frame sent outside a read event, is the async case
(8e-2): it needs a way to wake the loop and send without a triggering completion.

### Litmus / testability

`kl_ws_server_on_readable_data` and the WS callbacks/config are unchanged (readiness
untouched); `comp_ws_drive` lives in `completion_driver.c` and drives only the WS core +
the `KlTls` vtable (no Win32); no `include/keel/` change. Runtime-testable over pollcomp
in CI with a real WS handshake + echo (KEEL ships `kl_ws_client`), under ASan; the same
harness that runruns h2c today. **This is the increment to do first.**

---

## 8e-2: Async handlers over the completion loop

### The real gap

Async is not "route one more state"; the completion loop is **not a full event loop
yet**. The server's completion branch runs only:

```
kl_io_engine_run_completion(s, timeout)   // prime accepts + kl_comp_run (drain conn ops)
kl_server_sweep_conn_timeouts(s, now, 1)  // idle sweep
```

It does **not** service, as the readiness branch does:
- **`kl_watcher`s**: generic FD callbacks (tagged-pointer dispatch). The **thread pool**
  signals completion by writing a pipe watched by a `kl_watcher`; on a completion loop
  that watcher never fires, so `done_fn` (→ `kl_async_complete`) is never called. This is
  the primary async path (SQLite/file/crypto offload) and it is dead on completion.
- **Timers** (`kl_timer_next_timeout` / the min-heap tick).
- **The async-op deadline sweep** (per-op `on_deadline`).

So async handlers, timers, and async deadlines simply don't run on the completion loop.

### Two things must change

1. **A completion-aware resume.** `kl_async_suspend` calls `kl_event_del` (inert on a
   completion backend; fine; a suspended conn holds no op). But `kl_async_complete`
   (async.c) resumes the readiness way: `kl_conn_on_writable` + `kl_event_add(WRITE/READ)`
   the readiness send path. On a completion loop the resume must instead drive the
   completion send. *Small, clean:* after `on_resume`, branch on `kl_event_caps` and, for
   a completion loop, call a new io_engine seam `kl_io_engine_resume_completion(s, conn)`
   that runs `comp_after_state(s, conn, conn->state)`; the same send path a normal
   request takes. Readiness path unchanged; the seam is stubbed on readiness builds. No
   public-API change (the `KlAsyncOp`/suspend/complete API is untouched).

2. **Service watchers + timers + async deadlines on the completion loop.** This is the
   substantial part; generalising the completion loop into a full event loop:
   - **Watchers (thread-pool wakeup):** the loop must notice a ready watcher fd.
     - *IOCP:* associate the wakeup with the port; `PostQueuedCompletionStatus` from the
       worker signals `kl_comp_drain`, surfaced as a "watcher-ready" completion the tick
       routes to the watcher callback. (Or run the watcher fds on a side thread that
       posts to the port.)
     - *pollcomp:* add the ctx's watcher fds to the `poll()` set alongside the op fds and
       dispatch ready ones; a few lines, since pollcomp already polls.
   - **Timers + async deadlines:** compute the completion `kl_comp_drain` timeout from the
     nearest timer/deadline (as the readiness branch computes its `wait_timeout`), and run
     `kl_timer` + the async-deadline sweep in the completion branch.

   The cleanest framing: factor the readiness branch's "watchers + timers + deadlines"
   servicing into shared helpers and invoke them from the completion branch too, with the
   backend providing a "wake me" primitive (`kl_comp_wake`; `PostQueuedCompletionStatus`
   / a self-pipe for pollcomp).

### Staging

- **8e-2a**: the completion-aware resume seam (`kl_io_engine_resume_completion`);
  `kl_async_complete` branches on caps. Small; unlocks resume-correctness once (2b) can
  deliver the wakeup.
- **8e-2b**: service watchers on the completion loop (thread-pool wakeup): `kl_comp_wake`
  (PQCS / self-pipe) + route watcher-ready through the tick. Enables thread-pool async.
- **8e-2c**: timers + async-op deadlines on the completion loop (shared servicing +
  drain-timeout from the nearest deadline).

8e-2 is materially larger than 8e-1 and touches the shared run-loop servicing; it is its
own mini-arc. Recommend landing **8e-1 (WebSocket)** first; self-contained, CI-testable,
high-value; then deciding whether the async/thread-pool-over-completion investment (8e-2)
is warranted now or deferred (multi-core scaling on Windows is horizontal via
`SO_REUSEPORT` today, so async offload on the completion loop is a convenience, not a
blocker).

---

## Litmus summary (both increments)

| Axis | 8e-1 WebSocket | 8e-2 async |
|---|---|---|
| Readiness API unchanged | ✅ reuse `on_readable_data` + WS config | ✅ `KlAsyncOp`/suspend/complete API unchanged |
| No IOCP in abstract driver | ✅ `comp_ws_drive` drives WS core + vtable | ✅ resume seam is generic; `kl_comp_wake` is per-backend |
| Public API masks the axis | ✅ same `kl_server_ws` | ✅ same async API; server picks the drive |
| `include/keel/` change | none | none (io_engine seams are internal) |
| CI-runtime-testable | ✅ pollcomp + `kl_ws_client`, ASan | partial; thread-pool wakeup needs 8e-2b to test end-to-end |
