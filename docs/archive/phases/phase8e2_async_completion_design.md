# Phase 8e-2: Async handlers over the completion loop -- Design

**Status:** designed. The deep half of 8e (8e-1 WebSocket shipped). Async/suspended
handlers (`KL_CONN_SUSPENDED`), the thread-pool-offload path for blocking work (SQLite,
crypto, file I/O), currently close on a completion loop. Bringing them onto it requires
turning the completion loop into a *full* event loop, done without coupling the async API
or the abstract completion axis to any platform.

**Non-negotiable orthogonality (per the request):**
1. **The async/watcher/thread-pool public API does not change** and gains nothing
   event-axis- or platform-shaped. A user writes identical async code on epoll, kqueue,
   IOCP, or pollcomp.
2. **The abstract completion axis (`completion.h`) does not couple to IOCP or pollcomp.**
   New axis concepts are expressed as impl-agnostic contracts each backend satisfies its
   own way; no `event_iocp.c`/`event_pollcomp.c` detail leaks into the axis or the
   generic driver.
3. The readiness-vs-completion distinction is the **abstract event axis**
   (`kl_event_caps() & KL_EVENT_CAP_COMPLETION`), already used by `server.c`. Reusing it is
   *not* impl coupling; it names the model, never the backend.

---

## 1. The gap (precise)

On a completion loop the server run loop runs only:

```
kl_io_engine_run_completion(s, timeout)   // prime accepts + kl_comp_run (drain conn ops)
kl_server_sweep_conn_timeouts(s, now, 1)  // idle sweep
```

It never services what the readiness branch does:

- **Watchers.** `kl_watcher_add` → `kl_event_add(&ctx->loop, fd, mask, tag)` (async.c). On
  the completion backends `kl_event_add` is a **no-op** (event_iocp.c/event_pollcomp.c:
  "completion model: no readiness arming"). So no generic FD watcher fires, and the
  **thread pool** signals completion via a `KlPlatWakeup` fd watched by exactly such a
  watcher (`thread_pool.c`: `kl_watcher_add(ctx, pool->wakeup.rd, …)`). Its `done_fn`
  (→ `kl_async_complete`) is therefore never called on a completion loop. This is the
  primary async path, and it is dead.
- **Timers** (`kl_timer_next_timeout` + the min-heap tick) and the **async-op deadline
  sweep**.

And `kl_async_complete` (async.c) resumes the readiness way: `kl_conn_on_writable` +
`kl_event_add(WRITE/READ)`.

---

## 2. Design

Three pieces. The load-bearing idea: **make the completion loop relay readiness watchers
through the same abstract axis it already uses for connection completions**, so the
generic thread-pool / watcher / timer machinery runs unmodified.

### A. Completion-aware async resume (8e-2a): small

`kl_async_complete`, after `on_resume` sets the response, must drive the send. Today it
does the readiness send. Make it branch on the abstract axis:

```c
if (kl_event_caps(&s->ev.loop) & KL_EVENT_CAP_COMPLETION)
    kl_io_engine_resume_completion(s, conn);   // seam → comp_after_state(s, conn, state)
else { /* existing: kl_conn_on_writable + kl_event_add(WRITE/READ) */ }
```

`kl_io_engine_resume_completion` is a new **io_engine seam** (declared in `io_engine.h`,
stubbed in `io_engine.c` on readiness builds, real in `completion_driver.c` where it calls
the existing `comp_after_state(s, conn, conn->state)`, the same send path a normal request
takes). `kl_async_suspend` needs no change: `kl_event_del` is already inert on completion,
and a conn suspended *during dispatch* holds no pending op (the recv that delivered the
request was consumed before dispatch). **The `KlAsyncOp`/suspend/complete API is
untouched;** the only addition is an internal caps-branch + an internal seam.

### B. Relay watchers through the completion axis (8e-2b): the crux

Two impl-agnostic additions to the abstract axis:

1. **`kl_event_add` on a completion backend registers a readiness watch** (instead of
   no-op): "surface when this fd is ready for `mask`." `kl_event_mod`/`kl_event_del` adjust
   /remove it. This is stated abstractly: *how* a backend waits on a readiness fd
   alongside its completions is the backend's business.
2. **`kl_comp_drain` surfaces a ready watch as a new event kind `KL_COMP_WATCHER`**
   (`target` = the udata tag registered with `kl_event_add`), alongside the connection-op
   events it already returns.

The **generic driver** then routes it with the machinery that already exists:

```c
case KL_COMP_WATCHER:  kl_event_dispatch(ctx, &ev[i]);  break;   // existing watcher callback
```

`kl_event_dispatch` (event_ctx.h) already unmasks the watcher tag and invokes its callback
(→ thread-pool `done_fn` → `kl_async_complete`). **No platform code enters
`completion_driver.c`;** it just gains one `case`.

**Per-backend implementation of "watch readiness fds + surface them"** (the contract; each
its own way, none in the axis):

- **pollcomp:** keep a small `(fd, mask, tag)` table; add those fds to the `poll()` set each
  `kl_comp_drain` alongside the op fds; surface ready ones as `KL_COMP_WATCHER`. The
  thread-pool wakeup write makes its fd readable → `poll` returns → watcher fires. **No
  separate wake primitive needed**: fd readiness already wakes `poll`. (A few lines; poll
  is already there.)
- **IOCP:** the completion port cannot readiness-watch an arbitrary fd. The `KlPlatWakeup`
  read end on Windows is a loopback **socket**, so post an overlapped 1-byte `WSARecv` on
  it; the worker's wakeup write **completes** that recv → `GetQueuedCompletionStatusEx`
  returns → surface `KL_COMP_WATCHER`, re-post. This covers the thread pool (the async
  path that matters). Arbitrary non-socket FD watchers on IOCP would need a side poller
  thread that `PostQueuedCompletionStatus`es; out of scope initially and documented as
  such (KEEL's completion consumers are the thread pool + timers, both covered).

No `kl_comp_wake` is required: every cross-thread signal already travels through a watcher
fd, and folding those fds into each backend's wait set makes the signal wake the drain
natively.

### C. Timers + async deadlines on the completion loop (8e-2c)

Factor the readiness branch's "nearest timer/deadline → wait timeout", the `kl_timer` tick,
and the async-op-deadline sweep into shared helpers, and call them from the completion
branch; bound the `kl_comp_drain` timeout by the nearest timer/deadline so the loop wakes
on time. Purely generic (time math + the existing timer/async structures); no platform.

---

## 3. Staging

| Increment | Content | Test |
|---|---|---|
| **8e-2a** | `kl_io_engine_resume_completion` seam; `kl_async_complete` caps-branch | POSIX byte-identical (readiness unchanged); compile-gate |
| **8e-2b** | `KL_COMP_WATCHER` + `kl_event_add`-registers-watch (pollcomp); driver routes via `kl_event_dispatch`; timers/deadlines on the completion loop | **pollcomp thread-pool-async smoke** (submit blocking work → `done_fn` → `kl_async_complete` → response), ASan CI |
| **8e-2c** | IOCP watcher relay (overlapped `WSARecv` on the socket wakeup) | MinGW compile-gate + a Windows IOCP thread-pool smoke |

8e-2b is the end-to-end unit (the resume seam only fires once a watcher can). 8e-2b is
CI-testable on pollcomp; 8e-2c is the Windows leg.

---

## 4. Orthogonality litmus

| Axis | How 8e-2 holds it |
|---|---|
| Async/watcher/thread-pool API unchanged | Only an internal caps-branch + internal seam; `KlAsyncOp`, `kl_async_suspend/complete`, `KlWatcher`, `KlThreadPool` signatures + behavior identical. **No `include/keel/` change.** |
| Abstract axis not coupled to impl | `completion.h` gains `KL_COMP_WATCHER` + the `kl_event_add`-registers-watch contract, impl-agnostic; pollcomp/IOCP satisfy them differently, neither leaks into the axis or the driver. |
| No platform in the generic driver | `completion_driver.c` gains one `KL_COMP_WATCHER` → `kl_event_dispatch` case; no Win32/poll symbol. |
| Public API masks the axis | Same `kl_thread_pool_*` / `kl_async_*` / handler code runs on readiness or completion; the server picks the drive. |

---

## 5. Honest scope

This is the **largest** completion increment: it generalizes the completion loop into a
full event loop (watchers + timers + deadlines), and the IOCP watcher relay (readiness fds
through a completion port) is genuinely fiddly; scoped initially to the socket-based
thread-pool wakeup, which is the async path that matters. It is a **convenience, not a
blocker**: multi-core scaling on Windows is horizontal via `SO_REUSEPORT` today, and
blocking work can run on a readiness (WSAPoll) worker process. Recommend landing **8e-2b
on pollcomp first** (CI-testable, proves the whole mechanism) before the IOCP leg.
