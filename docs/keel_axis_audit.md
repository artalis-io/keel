# KEEL Networking Architecture Axis Audit

## Third pass — completion test-coverage-gap triage (2026-08-01)

**Verdict: no hidden backend bug.** This pass triaged the low/informational test-coverage gaps
from §3 (F2/F3/F4) — the default-provider unit suites *excluded* from the completion test set
(`IOURING_TEST_SUITES`) — by running each over the completion backend (pollcomp, the shared
`completion_driver.c` double) and root-causing every failure. All but one are fixture limitations
or inherent axis-semantics differences, not backend defects; the one real functional gap (PROXY
protocol over a completion loop) is now made safe (fail-loud at init) rather than silently
mis-served.

**Per-suite triage (excluded default-provider suites, run over pollcomp):**

| Suite | Over completion | Root cause | Disposition |
|---|---|---|---|
| `tls_integration`, `peer_cert`, `cross_module`, `unix_socket` | TLS tests failed | Each defined its **own per-file passthrough mock TLS** that implemented only the *readiness* vtable (`read`/`write` on the socket), not the completion-mode `feed_input`/`drain_output`; `comp_on_accept` correctly rejects a TLS conn whose backend can't do memory-BIO mode. **Not a backend bug.** | **Now enabled (2026-08-01 follow-up):** ported all four to the shared completion-capable `tests/mock_tls.h` (the same mock `smoke-pollcomp-tls`/`smoke-iocp-tls` use; `peer_cert` installs its canned cert via the new `mock_tls_peer_cert_fn` hook). They now pass over readiness *and* completion (pollcomp + real io_uring) and are in `IOURING_TEST_SUITES` (44 → 48). Unit-level TLS-over-completion coverage, complementing the smokes. |
| `peer_addr` | 4/6 pass; `proxy_v1/v2_trusted` fail | **Real gap (now fixed):** PROXY-protocol header handling (`KL_CONN_PROXY_HEADER`) originally lived only in the readiness run loop; the completion driver had no PROXY-header phase, so a trusted-source PROXY header was misparsed as HTTP. | **Fully supported (2026-08-01 follow-up):** the completion driver grew a PROXY-header phase — `comp_on_accept` enters `KL_CONN_PROXY_HEADER` for a trusted peer, `comp_drive_proxy` parses the plaintext header from `read_buf` via the model-blind `kl_conn_ingest_proxy` and then enters the real initial state (TLS handshake feeding the buffered ClientHello, or HTTP read). The header recv is plaintext even for a TLS conn (`post_recv` skips the TLS branch while `state == KL_CONN_PROXY_HEADER`, all three backends). `peer_addr` now passes 6/6 over completion and is in `IOURING_TEST_SUITES`; a PROXY-v1 roundtrip is in `smoke-pollcomp` + `smoke-iocp`. The #134 fail-loud init guard was removed. |
| `udp_multicast` | 4/5 pass; `broadcast_flag_gates_send` fails | Inherent axis semantics: the test asserts a *synchronous* `EACCES` on a broadcast send without `SO_BROADCAST`, which only holds for readiness; completion sends are queued async, so the error surfaces on the send completion, not the post. **Not a backend bug.** | Excluded by design (already documented in the Makefile). |
| `async`, `event`, `event_ctx`, `event_caps`, `socket_provider` | n/a | Inherently readiness-axis: raw `kl_event_wait` drivers / readiness-cap + provider-negotiation assertions (a completion loop has no `kl_event_wait`, only `kl_comp_run`); `async` also hand-builds a `KlConn` with a NULL `ctx`. | Excluded by design (not applicable to a completion loop). |

**F3 (mbedTLS-over-IOCP, real TLS):** unchanged — BYO / out-of-CI. The IOCP TLS *code path* is
exercised by `smoke-iocp-tls` (the completion-mode `tests/mock_tls.h`); real mbedTLS-over-IOCP is
a bring-your-own concern consistent with the mbedTLS policy. Accepted, not a gap to close in CI.

**F4 (`test_async` over completion):** confirmed a test-harness artifact (a hand-built conn with
NULL `ctx` driven through the resume path — would equally hit any completion backend), not a
backend bug. The async-over-completion path itself *is* covered: `smoke-pollcomp-async` and the
io_uring `/astream` case (#127) drive `kl_async_suspend`/resume + timer over the completion loop.
`test_async` stays excluded (a completion-aware async fixture would be the enabling work).

**Net:** the completion backends are now at functional **and** test parity with readiness. The
TLS-touching suites were ported to the shared completion mock, and PROXY-over-completion was
implemented (`comp_drive_proxy` + `kl_conn_ingest_proxy`), so `IOURING_TEST_SUITES` grew 44 → 49
(the four TLS suites + `peer_addr`). The only remaining excludes are inherent axis-semantics or
readiness-axis tests: `udp_multicast`'s `broadcast_flag_gates_send` (asserts a synchronous EACCES;
completion sends are async) and the raw-`kl_event_wait` / provider-negotiation driver suites
(`async`, `event`, `event_ctx`, `event_caps`, `socket_provider`). The one accepted limitation is
BYO mbedTLS-over-IOCP (F3, out-of-CI by policy).

---

## Second pass — completion-axis parity + robustness follow-up (2026-07-31)

**Verdict: unchanged — architecturally sound.** No new architectural findings; the orthogonality
map, honest-model-representation, and grep litmus from the first pass all still hold. This pass
records two completion-axis items closed since 2026-07-30 and one Windows-IOCP robustness fix, and
re-confirms the remaining gaps are the same low/informational items (no criticals).

**Closed since the first pass:**

- **Async / long-lived streaming over the completion loop is proven, not assumed (#127, Phase
  8g-2).** `tests/smoke_iouring.c` gained an `/astream` handler that begins a chunked stream,
  writes a chunk, `kl_async_suspend`s on a `kl_timer_add` deadline, then resumes on the completion
  loop to write the tail + end. It exercises begin-stream → suspend → timer-fire → resume →
  end-stream entirely over io_uring, closing the open question from the 8g scoping (the outbound
  stream buffer was wired by default in #126/8g-0).

- **Streaming head-of-line blocking over the completion loop is fixed (#130, Phase 8g-1) — the
  last *functional* gap in the completion backends is now closed.** `comp_send_stream` used to
  flush a streaming response by busy-spinning `kl_drain_flush`; on a slow client whose socket send
  buffer is full, the non-blocking send returned would-block while `stream_ended`, spinning the
  loop thread and starving every other connection. Streaming now rides the same overlapped drive as
  buffered/file responses: `comp_stream_pump` posts the outbound buffer as one overlapped send
  (`kl_comp_post_send` copies it; the backend surfaces one `KL_COMP_WRITE` when it is fully out) and
  `comp_on_write` re-pumps — posting freshly-produced bytes or completing once the stream ended and
  the buffer drained. At most one send is in flight (`KlResponse.stream_inflight`), memory stays
  bounded by the drain cap, and the loop is free between sends. Purely completion-side
  (`completion_driver.c` + two `KlDrain` peek/consume accessors); readiness, protocols, and the
  public streaming API are untouched. A `/bigstream` slow-reader-plus-concurrent-fast-client HOL
  test on all three completion smokes (pollcomp/iouring/iocp) guards it; validated on pollcomp+ASan,
  io_uring+LSan (container), and the Windows-IOCP CI job.

- **UDP datagram local (dest) address parity on the completion backends is complete (#128).** IOCP
  now captures the datagram's local address via an overlapped `WSARecvMsg` + `IP_PKTINFO` control
  message (`kl_udp_win_get_recvmsg` / `kl_udp_win_parse_local` in the Windows-only, include-scoped
  `src/udp_cmsg_win.h`, shared byte-for-byte with the readiness recv), matching io_uring and
  pollcomp (#118/#119). Previously `kl_comp_post_udp_recv` used `WSARecvFrom` (source only), so
  `kl_udp_send_to_from` reply-from delivered a NULL local over IOCP. The Keel-level `KlUdpRxMeta`
  contract (source + local + gro_seg + truncated) is now honoured identically across all three
  completion backends — a parity win *above* the axis, with the platform mechanics staying in the
  Windows TUs + the include-scoped header (no `#ifdef` in cross-platform code, no public-API change).

**Robustness fix made this pass (unlike the report-only first pass):** the overlapped `WSARecvMsg`
posted for UDP recv over IOCP leaves an outstanding op that `closesocket` cancels at teardown; it
completes with `bytes == 0` and an *unfilled* (zeroed) `WSAMSG` control buffer. The completion path
parsed that buffer unconditionally, and the cmsg walk spun forever (`WSA_CMSG_NXTHDR` advances by
`WSA_CMSG_ALIGN(cmsg_len)`, so `cmsg_len == 0` yields the same pointer), wedging the loop thread so
`kl_server_run` never returned. `WSARecvFrom` never read control data, so this only surfaced once
`WSARecvMsg` landed. Fixed two ways: `kl_udp_win_parse_local`/`udp_parse_tos` now stop the walk on
a runt cmsg (`cmsg_len < sizeof(WSACMSGHDR)`) — a general loop-safety fix that also hardens the
readiness path — and the IOCP `KL_IOCP_UDP_RECV` handler only parses source + pktinfo metadata when
`bytes > 0` (a cancelled/failed op carries no valid name/control). This maps to Goal 6 (op
lifetime: close-while-outstanding) and Goal 7 (partial/zero-length completions handled honestly):
a completion event is not a received datagram. Validated by the Windows-IOCP `smoke-iocp` job,
which now prints `... + UDP + udp-local` after enabling `recv_pktinfo` and asserting the local
address is delivered.

**Matrix delta:** Winsock + IOCP (completion) UDP now has full datagram-metadata parity (source +
local/pktinfo + truncation), matching io_uring/pollcomp; the §4 row is otherwise unchanged
(plaintext prod-ready; real-mbedTLS TLS remains BYO/out-of-CI, F3).

**Also closed since the first pass:** IOCP file bodies larger than TransmitFile's ~2 GiB per-call
cap are now sent as several offset-advancing `TransmitFile` chunks re-posted from the
`KL_IOCP_SENDFILE` completion (`iocp_post_transmitfile_chunk`), transparent to the driver (one
`KL_COMP_WRITE` when the whole head+file is out; ≤1 op in flight). Previously such a body was
rejected. A `/bigfile` smoke case with a test-lowered per-call cap (`KEEL_IOCP_TF_CHUNK`) verifies
the multi-chunk path delivers every byte at the right offset without needing a >2 GiB fixture.

**Still open (all low/informational — see §3/§6):** F2 (per-suite triage so the default-provider
integration suites run over completion), F3 (a self-hosted mbedTLS Windows-IOCP TLS smoke, or
document the BYO gap), F4 (a completion-aware async test fixture). Non-functional, explicitly
deferred: io_uring multishot recv / registered buf-rings (benchmark showed no current need). (The
completion-streaming HOL and TransmitFile >2 GiB items are no longer open — closed by #130 / 8g-1
and the TransmitFile chunking above.)

---

## First pass — event / socket / protocol orthogonality (2026-07-30)

**Verdict: architecturally sound.** The event axis (readiness + completion), the socket/
platform axis, and the protocol layer are genuinely orthogonal. The mechanical litmus tests
pass, both event models are represented honestly, and every supported backend combination is
CI-tested (not merely present). No critical/high findings. The two real defects that a
lifetime/semantics audit would surface — an io_uring watch leak and a completion graceful-drain
hang — were already found + fixed earlier the same day (PRs #111, #113); they are recorded here
as resolved. Remaining items are test-coverage gaps (low) and one informational API note.

**Method:** repo map + mechanical grep litmus (protocol TUs vs platform headers/event symbols;
backend vs protocol symbols; public-header leak scan; Winsock-parity scan) + end-to-end path
traces over a readiness backend and a completion backend + review of ownership/lifetime,
partial-I/O, backpressure, cancellation, error-normalization, and CI backend coverage. Drew on
the just-completed 8d–8f completion migration + `/c-audit` fifth pass.

---

## 1. Current architecture map

**Event axis** (`BACKEND=` selects one at build time):
- Interface: `include/keel/event.h` (readiness: `kl_event_init/add/mod/del/wait/close` over
  `KlEventLoop` + `KlEvent{void *udata; KlEventMask ready;}`). Capability surface (internal):
  `src/event_caps.h` (`KL_EVENT_CAP_READINESS | _NATIVE_FD | _COMPLETION`, `kl_event_caps`,
  `kl_caps_compatible`, `kl_event_ctx_sockets_compatible`).
- Completion axis (peer to readiness): `src/completion.h` (abstract `KlCompletionEvent{target,
  kind, bytes, ok, accepted_fd, buf, peer}`, `kl_comp_post_recv/send/accept/sendfile/udp_*`,
  `kl_comp_drain`) + `src/completion_driver.c` (the platform-independent connection driver +
  `kl_io_engine_run_completion`) + `src/io_engine.h` (run-loop dispatch seam).
- Readiness backends: `event_epoll.c`, `event_kqueue.c`, `event_wsapoll.c`, `event_poll.c`.
- Completion backends: `event_iouring.c` (Linux, SQE/CQE), `event_iocp.c` (Windows), and
  `event_pollcomp.c` (portable `poll()` facade — the CI/ASan test double). Each implements
  `kl_event_caps` + `kl_event_native_provider` + the `completion.h` post/drain contract.

**Socket axis:** `src/socket.h` — `KlSocketProvider {const KlSocketOps *ops; void *context;
unsigned capabilities;}` (`KL_SOCK_CAP_NATIVE_FD | _OVERLAPPED`). Providers: `socket_posix.c`,
`socket_winsock.c`; overlapped providers (`kl_socket_provider_iouring/iocp/pollcomp`) live in
their event TUs. Native handle: `KlSocketHandle = intptr_t` (`include/keel/handle.h`),
`KL_INVALID_SOCKET = (KlSocketHandle)-1`, tested via `kl_handle_valid()` (never `< 0`). Selected
on `KlEventCtx.sockets` + public `KlConfig.sockets` / `KlClientConfig.sockets`. Error taxonomy:
`kl_sock_errno_to_error` (+ `kl_wsa_set_errno` on the Winsock seam).

**Protocol layer:** `connection.c` + `conn_internal.h` model-blind core (`kl_conn_dispatch_request`,
`kl_conn_ingest_body`, `kl_conn_send_complete`, `kl_conn_on_readable`), `h2.c`, `websocket.c`,
`response.c`, body readers, `chunked.c`, `cors.c`, `sse.c`, client transports (`client.c`,
`h2_client.c`, `websocket_client.c`), `KlTls` vtable. All server I/O flows through
`conn_read`/`conn_write` (`internal.h`) which branch TLS-or-plain and call the socket seam.

**How they connect:** the server run loop (`kl_server_run`) detects the axis once via
`kl_event_caps` → readiness branch (`kl_event_wait` + `kl_event_dispatch`) or completion branch
(`kl_io_engine_run_completion` → `kl_comp_run`). Both drive the *same* model-blind protocol core.
`kl_server_init` negotiates the loop against the provider (`kl_event_ctx_sockets_compatible`) and,
since 8f-5a, auto-adopts the backend's overlapped provider (`kl_event_native_provider`) when a
completion loop is paired with the default provider — so protocols/consumers never name the axis.

---

## 2. Execution-path traces

**Readiness receive (epoll/kqueue):** `kl_server_run` → `kl_event_wait` reports `KL_EVENT_READ`
→ `kl_event_dispatch` (untagged udata = conn) → `kl_conn_on_readable` → `conn_read` (`tls->read`
or `kl_sockdef_recv`) → `kl_conn_ingest_body`/`kl_conn_dispatch_request` → handler → response →
`kl_event_mod` re-arms interest / keep-alive. EAGAIN → return, wait again.

**Completion receive (io_uring/pollcomp):** driver posts `kl_comp_post_recv` (io_uring:
`io_uring_prep_recv` into `read_buf` slice with the op as `user_data`; TLS: into a cipher buffer)
→ `kl_comp_drain` (`io_uring_submit_and_wait_timeout`, reap CQE, skip the `LIBURING_UDATA_TIMEOUT`
+ cancel sentinels) → `iou_complete` maps `cqe->res` → `KL_COMP_READ{target=conn, bytes, ok}` →
`kl_comp_run` → `comp_on_read` → (TLS: `feed_input` then `comp_tls_drive`) → the *same*
`kl_conn_dispatch_request`/`ingest_body` core → `kl_comp_post_recv` (next) or pause. No synthetic
readiness; bytes are real kernel-delivered data.

**Accept:** readiness — `kl_event_wait` on the listen fd → `accept` via the seam →
`kl_conn_acquire`. Completion — `kl_comp_post_accept` (`io_uring_prep_accept` / `AcceptEx`) →
CQE → `KL_COMP_ACCEPT{accepted_fd, peer}` → `comp_on_accept` → `kl_conn_acquire` +
`kl_comp_post_recv` + `kl_comp_post_accept` (refill; single accept outstanding).

**Send + backpressure:** readiness — `kl_response_send`/`try_writev` with `send_offset` for
partial writes; on EAGAIN the remainder buffers in `KlDrain` (bounded, `on_drain` callback);
listen paused (`listen_paused`) when the pool is full. Completion — `kl_comp_post_send` copies the
iovec into an owned buffer (or a registered pool buffer, io_uring 8f-2); a short `cqe->res` re-preps
the tail (`iou_prep_send_tail`) so only a fully-completed send surfaces a `KL_COMP_WRITE`; UDP send
queue caps outstanding overlapped bytes. Equivalent Keel-level semantics, different mechanism.

**Close with outstanding work:** idle sweep `kl_server_sweep_conn_timeouts` → `kl_comp_cancel`
(io_uring `prep_cancel` by `user_data` + `aborted` flag) → the op completes `-ECANCELED`/error →
`comp_close` releases the conn through its normal completion (invariant: a conn is released only
from a completion — no dangling op, no double release). A removed readiness watch is kept LINKED in
`st->watches` until its poll-cancel CQE frees it, or `kl_event_close` frees it at shutdown
(the #111 fix — otherwise it leaked). Graceful drain runs in *both* run-loop branches via
`kl_server_drain_progress` (the #113 fix — the completion branch previously never exited drain).

---

## 3. Findings

| # | Sev | Files/symbols | Principle | Assessment | Smallest fix |
|---|-----|---------------|-----------|------------|--------------|
| F1 | **Informational** | `include/keel/event.h` `KlEventLoop.fd` (public `int`, "epoll_fd or kqueue_fd, -1 for io_uring") | G3 (no event-model leak) | Not a real leak: `KlEventLoop` is the loop object, not consumed by protocols; the field is documented backend-internal and frozen (PAL Appendix A). No protocol reads it. A future non-fd backend (lwIP raw) would want a portable handle here. | None now; revisit at Phase 9 (lwIP raw) with a `KlSocketHandle`-style widening if needed. |
| F2 | **Low** | `IOURING_TEST_SUITES` (Makefile) vs the ~14 default-provider suites (`client`, `client_stream`, `redirect`, `peer_*`, `tls_integration`, `udp*`, `unix_socket`, `dns_resolver`, `request`, `cross_module`) | Decision std: combos *tested*, not assumed | Test-coverage gap, not an architecture defect. These init over completion (5a) but have per-suite behavioural gaps; the completion backend is covered by 36 unit suites + full smokes + benchmark. | Incremental per-suite triage (deferred, documented in `phase8f5` §3). |
| F3 | **Low** | Windows + IOCP + **real mbedTLS** TLS runtime (BYO, out of CI) | Decision std: tested | The IOCP TLS *code path* is exercised via the identity mock-TLS (`smoke-iocp-tls`); real mbedTLS-over-IOCP is not CI-gated (mbedTLS is BYO everywhere). Consistent with the mbedTLS policy, but the combo's production-readiness is asserted, not proven. | A local/self-hosted `KEEL_TLS=mbedtls` Windows-IOCP smoke, or document the gap explicitly (already noted in `phase6_winsock_design.md`). |
| F4 | **Informational** | `tests/test_async` over completion (manual `KlConn` with NULL `ctx`) | G6 (lifetime) / testing | Crashes over completion because the test hand-builds a conn without `ctx` and drives the resume path — a test-harness artifact (would equally hit pollcomp/IOCP), not a backend bug. | A completion-aware async test fixture (or exclude, as now). |
| R1 | **Resolved (this session)** | `event_iouring.c` `kl_event_del` | G6 (lifetime) | A removed watch with an in-flight `POLL_ADD` was unlinked + its free deferred to a CQE dropped at `io_uring_queue_exit` → 48 B leak/server. Fixed (#111): keep linked, free in CQE or `kl_event_close`. LSan-clean. | Done. |
| R2 | **Resolved (this session)** | `server.c` completion run-loop branch | G2/G7 (semantics) | Graceful drain (`draining` → close-idle/deadline → stop) ran only in the readiness path, so a completion-loop server with `drain_timeout_ms` never exited drain → deadlock. Fixed (#113): shared `kl_server_drain_progress` in both branches. | Done. |

**Clean (verified, no finding):**
- **G1** — event backends reference zero protocol symbols; socket providers reference zero event
  engines/protocols (only a descriptive comment). No `#ifdef` merges the two axes (Makefile
  selects `EVENT_SRC` and `SOCKET_SRC` independently; POSIX socket TU builds under every POSIX
  event backend).
- **G4** — protocol TUs import **no** platform net/event headers, make **no** direct
  epoll/kqueue/io_uring/WSA/OVERLAPPED calls, and do **no** raw socket syscalls (all via
  `conn_read`/`conn_write` + the seam). Mechanically grep-clean.
- **G2** — completion delivers real kernel bytes (never synthetic readiness); readiness handles
  EAGAIN and never claims completion on mere readability. The event.h `kl_event_wait` on a
  completion loop is a documented no-op (the loop uses `kl_comp_run`).
- **G3** — no event-model mechanics in `include/keel/` consumable types; `KL_EVENT_CAP_COMPLETION`
  / `KL_SOCK_CAP_OVERLAPPED` are internal. Readiness/completion are explicit *internal* axis
  concepts; protocols consume the stable `KlConn`/`conn_read`/`conn_write` contract.
- **G5** — `KlSocketHandle = intptr_t`, `KL_INVALID_SOCKET`, `kl_handle_valid()` (never `<0`);
  `closesocket`/`WSAGetLastError` are confined to `event_iocp.c` + the `*_win` TUs; the seam has
  real `writev`/`sendfile` ops (POSIX + `WSASend`/`TransmitFile`). Winsock is first-class.
- **G6/G7** — single in-flight op per conn (driver invariant); partial send re-prep in every
  backend; sentinels handled; release-only-from-completion. (R1/R2 were the exceptions, now fixed.)
- **G8** — `KlDrain` (bounded write buffer + `on_drain`), `listen_paused` accept gating, capped
  UDP send queue, growable-but-capped read buffer (`max_header_size`) — model-independent.
- **G9** — `kl_sock_errno_to_error` maps to stable `KlError`; the Winsock seam translates
  `WSAGetLastError` → errno first. Equivalent errors across axes.
- **G10** — `kl_comp_cancel` (native `ASYNC_CANCEL` + `aborted` sentinel) vs readiness
  registration teardown; idle/read timeouts in both branches; release only from a terminal
  completion.
- **G11** — single-threaded event loop per worker (Node/Redis/Nginx model); cross-thread work via
  `KlThreadPool` + a `KlPlatWakeup` self-pipe relayed as `KL_COMP_WATCHER`/watcher; `kl_server_stop`
  wakes the loop from any thread/signal (async-signal-safe write).
- **G12** — registration vs submission preserved: readiness `kl_event_add` = persistent interest;
  completion `kl_comp_post_*` = one op with its own buffer/`user_data`; the `KL_COMP_WATCHER` relay
  bridges a readiness watch onto a completion loop without conflating the two.
- **G13** — `BACKEND=` selection; `kl_event_caps` observable; `io_uring_queue_init` failure → init
  `-1` (no silent bad combo); the negotiation rejects incompatible loop×provider pairings.

---

## 4. Compatibility matrix

| Combination | Implemented | Buildable | Tested (CI) | Production-ready |
|---|---|---|---|---|
| Linux sockets + epoll | ✅ | ✅ | ✅ full suite + smoke | ✅ (default Linux) |
| Linux sockets + poll (fallback) | ✅ | ✅ | ✅ full suite | ✅ |
| Linux sockets + io_uring (completion) | ✅ | ✅ | ✅ 36-suite gate + smokes + **LSan** | ✅ (default io_uring path, 8f-5) |
| Darwin sockets + kqueue | ✅ | ✅ | ✅ full suite (macOS CI) | ✅ (default macOS) |
| Winsock + WSAPoll | ✅ | ✅ | ✅ 47-suite subset (Windows CI) | ✅ |
| Winsock + IOCP (completion) | ✅ | ✅ | ✅ lifecycle + smokes (plaintext, TLS-via-mock) | ⚠️ prod-ready plaintext; real-mbedTLS TLS is BYO/out-of-CI (F3) |
| pollcomp (portable completion double) | ✅ | ✅ | ✅ smoke + tls/ws/async + ASan/LSan | n/a — **test double**, not a production backend |

Not built (by design / future): Winsock+io_uring (N/A), IOCP+non-Winsock (N/A), lwIP-raw
(Phase 9), UEFI (Phase 10).

---

## 5. Proposed internal contract (from the existing design)

- **Socket ownership** — a `KlConn`/`KlUdp` owns its `KlSocketHandle` for its lifetime; the
  `KlSocketProvider` is *borrowed* (must outlive its transports; owner calls
  `kl_socket_provider_destroy` after). Close routes through `kl_sock_close` (→ `closesocket` on
  Winsock).
- **Event-loop affinity** — one `KlEventCtx` per thread; a socket/op/watcher belongs to the loop
  it was registered/posted on. Cross-thread work enters via `KlThreadPool` + `KlPlatWakeup`; no
  op is submitted or completed off-loop.
- **Readiness notification** — level/edge interest via `kl_event_add/mod`; the consumer performs
  the op and handles EAGAIN; re-arm via `kl_watcher_rearm`/`kl_event_mod`.
- **Completion delivery** — one `kl_comp_post_*` = one op owning its buffer + `user_data`;
  `kl_comp_drain` surfaces exactly one `KlCompletionEvent` per finished op (partial sends re-prep
  internally; only whole-op completion surfaces).
- **Operation lifetime** — a conn holds ≤1 in-flight op; the op is freed when its completion is
  reaped; a conn is released **only** from a completion (cancel makes the op complete with error).
- **Cancellation** — `kl_comp_cancel` (native `ASYNC_CANCEL` / `poll_remove` + an `aborted`/
  `removed` sentinel); exactly one terminal result per op; late/duplicate CQEs are discarded via
  the sentinel; a removed watch is freed by its cancel CQE or by `kl_event_close`.
- **Timeout races** — idle/read timeouts cancel the pending op; the cancellation completion is the
  single terminal event (a datum arriving first just completes normally).
- **Error normalization** — platform error → `kl_sock_errno_to_error` → stable `KlError`, native
  detail retained; equivalent across axes.
- **Close semantics** — `comp_close`/`kl_conn_release` after the terminal completion; no I/O after
  close; `kl_event_close` tears down the loop + frees any watch whose cancel CQE never arrived.
- **Backpressure** — bounded write buffer (`KlDrain`), accept gating (`listen_paused`), capped
  read/send buffers; identical Keel-level limits regardless of axis.

---

## 6. Recommended incremental roadmap

- **Immediate correctness:** none open (R1/R2 fixed this session; nothing else surfaced).
- **Test coverage:** finish the F2 per-suite triage so the default-provider integration suites run
  over completion; add a completion-aware async fixture (F4); consider a self-hosted mbedTLS
  Windows-IOCP TLS smoke (F3).
- **Small architectural cleanup:** none warranted — the axes are clean; do not refactor.
- **Deferred:** Phase 9 (lwIP raw — a third event model; would revisit F1's `KlEventLoop.fd` as a
  portable handle), Phase 10 (UEFI). QUIC/HTTP-3 rides the existing UDP + completion groundwork.

---

## 7. Changes made

**None.** This pass is report-only — the decision standard is met and no clear, low-risk fix was
outstanding (the two real defects were already fixed under PRs #111 and #113 the same day). The
findings are a test-coverage gap (F2/F3), a test-harness artifact (F4), and one informational
API note (F1); none justify a code change under the "narrow, low-risk, design-clear" bar.

---

## Decision standard — met

Socket + event providers are separately replaceable ✅ · readiness + completion keep native
semantics ✅ · higher layers get consistent Keel-level behavior ✅ · protocols contain no
platform event/socket logic ✅ (grep-clean) · op/buffer/conn lifetimes safe ✅ (LSan-clean after
#111) · cancellation/close/partial-I/O/errors/backpressure explicitly defined ✅ · supported
combos tested, not assumed ✅ (matrix §4). **KEEL's networking architecture is orthogonal and
sound.**
