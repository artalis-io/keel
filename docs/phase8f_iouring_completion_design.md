# Phase 8f — io_uring as a completion backend — Design

**Status:** designed. The completion axis (`completion.h`) has two implementations — IOCP
(Windows) and pollcomp (a portable `poll()` facade for CI). It is **missing io_uring**,
which is the odd omission: io_uring is *natively* a completion engine (submit SQEs, reap
CQEs), so it is arguably the **most** natural fit for the axis — more than IOCP (needs
per-op overlapped bookkeeping) and far more than pollcomp (fakes completion over
readiness). This phase adds a completion-native io_uring backend.

**The codebase already anticipated it.** `src/event_iouring.c` is a *readiness adapter*:
it drives io_uring purely as a poll-multiplexer (`io_uring_prep_poll_add`, `loop->fd = -1`)
and advertises `KL_EVENT_CAP_READINESS`; the actual socket I/O still goes through the
readiness `conn_read`/`conn_write` seam. Its own comment says a variant that "would
advertise `KL_EVENT_CAP_COMPLETION` — that is Phase 8," and the `loop` parameter on
`kl_event_caps` exists for exactly this. 8f delivers it.

**Value.** (1) A **third** completion backend proves the axis is genuinely
platform-independent on a *real, completion-native* engine — `completion_driver.c` is
reused **verbatim** again. (2) It is a **production-grade Linux async backend** (pollcomp
is a test facade; this is the real thing — zero-copy `splice`, multishot accept, batched
submit). (3) It is **CI-testable on Linux** under ASan — so, unlike the Windows-only IOCP
mechanics, io_uring completion gets first-class sanitizer coverage.

**Orthogonality (same bar as 8b–8e):** the abstract axis does not couple to io_uring; the
generic driver stays io_uring-free; no public API or protocol changes; io_uring is *one
more implementation* of `completion.h`, a peer of IOCP and pollcomp.

---

## 1. io_uring → `completion.h`, one-to-one

io_uring maps to the existing axis with **no new abstract concepts** — everything
`completion.h` already declares (from 8b–8e) has a direct io_uring op:

| `completion.h` primitive | io_uring op | Notes |
|---|---|---|
| `kl_comp_post_accept` | `IORING_OP_ACCEPT` (multishot where available) | multishot re-arms itself; else re-post per accept |
| `kl_comp_post_recv` (plaintext / TLS-cipher) | `IORING_OP_RECV` | into `read_buf` / the cipher buffer |
| `kl_comp_post_send` | `IORING_OP_SEND` | contiguous copy owned by the op (as IOCP/pollcomp) |
| `kl_comp_post_sendfile` | head `SEND` then `IORING_OP_SPLICE` | true zero-copy file→socket (pollcomp uses `sendfile`, IOCP `TransmitFile`) |
| `kl_comp_post_udp_recv` / `_send` | `IORING_OP_RECVMSG` / `SENDMSG` | `msghdr` carries src/dest + `IP_PKTINFO`, as udp.c already builds |
| `kl_comp_cancel` (idle timeout) | `IORING_OP_ASYNC_CANCEL` | by `user_data`; the cancelled op reaps a `-ECANCELED` CQE → the driver's normal release |
| `KL_COMP_WATCHER` relay (8e-2) | `IORING_OP_POLL_ADD` (multishot) | **the cleanest of the three** — io_uring has a native "watch this fd" op; the wakeup/timer fd arms a poll, its CQE surfaces `KL_COMP_WATCHER`, the driver routes via `kl_event_dispatch` |

`kl_comp_drain` is `io_uring_submit_and_wait` (timeout via `io_uring_wait_cqe_timeout` or an
`IORING_OP_TIMEOUT` SQE) → for each CQE: recover the op from `io_uring_cqe_get_data(cqe)`,
map to a `KlCompletionEvent` (`kind` from `op->type`, `bytes`/`ok` from `cqe->res`), handle
partial sends by re-preparing the remainder, `io_uring_cqe_seen`. Same op-pool + `KlXxxOp`
shape as `KlIocpOp` / `KlPcOp`, with the SQE `user_data` pointing at the op — the exact
role `OVERLAPPED`+`CONTAINING_RECORD` plays on IOCP.

**Nothing in `completion.h` changes.** The `KL_COMP_WATCHER` + `kl_event_add`-registers-a-
watch contract from 8e-2 is satisfied here by `IORING_OP_POLL_ADD` — a third backend, a
third way to meet the same abstract contract.

---

## 2. The backend TU

`src/event_iouring_comp.c` (new), selected by `BACKEND=iouringcomp`:

- **`kl_event_*` lifecycle** over `io_uring_queue_init` (ring in `loop->_backend`).
  `kl_event_caps` returns `KL_EVENT_CAP_COMPLETION | KL_EVENT_CAP_NATIVE_FD` (the readiness
  adapter returns `READINESS`). `kl_event_add` for a **tagged** udata arms a multishot
  `POLL_ADD` (the watcher relay); for a connection it is inert (I/O is submitted, not
  armed) — mirroring pollcomp/IOCP.
- **the `completion.h` post/drain/prime/cancel contract** per the table above.
- **an overlapped socket provider** — reuse the POSIX control-plane ops (like pollcomp's
  `kl_socket_provider_pollcomp`) + the `KL_SOCK_CAP_OVERLAPPED` capability the Phase-7
  negotiation keys on. io_uring uses real fds and the POSIX control plane, so this is a
  thin wrapper.

**`completion_driver.c`, `server.c`, `async.c`, `udp.c`, the TLS/h2/WS logic — all
unchanged.** They already run over any `completion.h` backend. TLS, HTTP/2, WebSocket, and
async/thread-pool ride the io_uring backend the moment it exists, because they were written
against the axis, not the backend. (The third verbatim reuse of the driver.)

---

## 3. Relationship to the readiness `event_iouring.c`

Both stay. They are different *models* of the same engine:

- `BACKEND=iouring` → `event_iouring.c`: io_uring **adapted to the readiness interface**
  (poll-multiplexer + `conn_read`/`conn_write`). Battle-tested; the current default Linux
  async option.
- `BACKEND=iouringcomp` → `event_iouring_comp.c`: io_uring driving the **completion axis**
  (native SQE/CQE I/O + `completion_driver.c`). Architecturally the truer fit — io_uring
  *is* completion — and the strategic direction, but new.

Nothing is removed. If the completion-native backend proves out, it can later become the
default io_uring path, but that is a separate decision; 8f only *adds* it.

---

## 4. Staging

| Increment | Content | Test (Linux CI, ASan) | Status |
|---|---|---|---|
| **8f-1** | ring lifecycle + provider + accept/recv/send + drain → GET/POST; `RECVMSG`/`SENDMSG` UDP; `POLL_ADD` watcher relay; `ASYNC_CANCEL` idle timeout; TLS/h2/WS ride the reused driver | `smoke_iouring` + `smoke_iouring_async` | ✅ landed (#101) |
| **8f-2** | zero-copy `SPLICE` sendfile (file → pipe → socket) + registered send-buffer pool (`WRITE_FIXED` for small responses) | `smoke_iouring` `/bigfile` (multi-chunk splice) + small responses (registered path) | ✅ landed |

The original 8f-3/8f-4 rows (watcher relay, TLS/h2/WS) folded into 8f-1: because the driver
is reused verbatim, TLS/h2/WS/async all rode io_uring the moment the backend existed, so
they were validated in 8f-1's smokes rather than split out. Because the driver is reused
verbatim, the completion smokes could also be pointed at `BACKEND=iouringcomp` — the same
double-duty pollcomp served, now on a production engine.

**8f-2 notes.** Splice uses a per-op pipe (created lazily at the head→file transition) and
`flags=0` so io_uring waits asynchronously for socket writability rather than erroring on a
full buffer; short splice-outs re-submit the pipe remainder. The registered pool is a fixed
256 KiB block (16 × 16 KiB — kept modest since it is pinned memlock; see Step 3) registered
once at loop init; a response ≤ 16 KiB borrows a slot
and sends via `WRITE_FIXED` (no per-send malloc), returning the slot on completion; larger
responses or an exhausted pool fall back to malloc + `SEND`. Both degrade gracefully on old
kernels (no `SPLICE` → pread+`SEND`; failed registration → malloc+`SEND`). Whether these
win in practice is the benchmark's job (the next migration step); they are correctness-safe
regardless.

---

## Step 3 — the unit-test suite over `BACKEND=iouringcomp`

Toward eventually retiring the readiness `event_iouring.c`, the full unit-test suite was run
over the completion backend (`make BACKEND=iouringcomp test-iouringcomp`; a one-shot survey
selected the set). Result: **29 suites pass; the rest are coupled to the readiness model,
not blocked by backend defects — the survey found zero completion-driver/protocol bugs.**

**Pass (29, the permanent CI gate):** the backend-agnostic logic suites (allocator,
parser, router, url, chunked, overflow, response, response_parser, cors, drain,
multipart_stream, proxy, proxy_protocol, resolver_cache, body_reader, compress, decompress)
plus the protocol/loop suites that drive the loop through the backend-neutral
`kl_server_run` / `kl_event_ctx_run` path (connection, thread_pool, timer, h2, h2_client,
websocket, websocket_client, sse, tls, file_io, udp_batching, udp_tos).

**Excluded — readiness-model coupling (why each fails, verified by ASan):**

- **Raw `kl_event_wait` drivers** — `test_async`, `test_event`, `test_event_ctx` tick the
  loop with `kl_event_wait`, which a completion backend deliberately implements as a no-op
  (its analogue is `kl_comp_run`, reached via `kl_event_ctx_run`/`kl_server_run`). The
  smokes exercise the real async path (`smoke_iouring_async`), so async-over-completion *is*
  covered — just not through these readiness-shaped unit harnesses. (`test_async` also
  drives the completion resume path with a hand-built conn whose `ctx` is NULL — a test
  artifact that would equally crash IOCP/pollcomp.)
- **Default-provider integration harnesses** — `test_integration`, `test_client*`,
  `test_server_integration`, `test_redirect`, `test_peer_*`, `test_timeout`,
  `test_tls_integration`, `test_udp*`, `test_unix_socket`, `test_dns_resolver`,
  `test_request`, `test_error`, `test_cross_module`, `test_server_stats` build a server with
  the **default (non-overlapped) socket provider**, which a completion loop **correctly
  rejects** at `kl_server_init` (the Phase-7 `kl_event_ctx_sockets_compatible` negotiation).
  The suites ignore the documented `-1` return and then crash/assert — a pre-existing
  test-robustness gap, exposed by (not caused by) the completion backend. The smokes use the
  overlapped provider and pass the full surface.
- **Readiness-cap / provider assertions** — `test_event_caps` asserts `KL_EVENT_CAP_READINESS`;
  `test_socket_provider` asserts readiness negotiation. Inherently readiness-axis suites.
- **Backend-specific** — `test_file_io_iouring` (readiness io_uring file I/O),
  `test_iocp_engine` (Windows).

**Takeaway.** The completion backend's *logic* is validated (29 suites + the full smoke
surface). The excluded suites are readiness-shaped test harnesses; running them over
completion would mean opting each into the overlapped provider and the `kl_comp_run` drive
(and fixing the ignored-`init`-return crashes) — a test-refactor effort, **not** a backend
prerequisite. That effort belongs with step 5 (making `iouringcomp` the default), after the
step-4 benchmark justifies it.

## 5. Orthogonality litmus

| Axis | How 8f holds it |
|---|---|
| Abstract axis not coupled to impl | `completion.h` unchanged; io_uring maps onto the existing kinds/contract, incl. `KL_COMP_WATCHER` via `POLL_ADD`. |
| Generic driver stays impl-free | `completion_driver.c` reused **verbatim** — no `io_uring_*` symbol; a third proof after IOCP + pollcomp. |
| No public API / protocol change | `BACKEND=iouringcomp` selection only (like `iocp`/`pollcomp`); TLS/h2/WS/async APIs untouched. |
| Concept vs implementation separate | io_uring is one more `completion.h` implementation, peer to IOCP + pollcomp; each meets the contract its own way (overlapped / poll-set / SQE-CQE). |

---

## 6. Honest notes

- **Dependency + kernel features.** liburing is already a dependency (`event_iouring.c`,
  `file_io_iouring.c`, `-luring`). io_uring op availability is kernel-version-gated
  (multishot accept 5.19+, `SEND`/`RECV` 5.6+, `SPLICE` 5.7+, multishot poll 5.13+); the
  backend should probe (`io_uring_opcode_supported` / feature flags) and fall back
  (single-shot accept, `read`+`send` for sendfile) or fail init cleanly on too-old kernels.
- **Two io_uring backends** is intentional (readiness-adapted vs completion-native), not a
  duplication to collapse now — see §3.
- **Provisioned/registered buffers, `IOSQE_IO_LINK`, SQPOLL** are performance refinements
  deferred past the correctness-first staging above.

**Recommendation:** implement **8f-1 on Linux CI** first (the core roundtrip over real
io_uring completion, ASan) — it simultaneously proves the axis a third time and lands a
production Linux backend — then extend through 8f-2/3/4, reusing the completion smokes.
