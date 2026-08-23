# KEEL capability matrix

**The single authoritative reference for what runs where and how the readiness
and completion axes compare.** It consolidates the matrix fragments previously
scattered across `docs/keel_axis_audit.md` (running audit log) and
`docs/core_completion_plan.md` (Phase 0 gap analysis). Those remain the source of
*rationale*; this file is the source of *status*. Keep it current when a backend,
feature, or integration changes.

The design intent (see `docs/keel_axis_audit.md`): the **event axis** (readiness
vs completion) and the **socket axis** (POSIX vs Winsock) are orthogonal; the
**protocol layer** sits above both and observes identical Keel-level semantics
regardless of which backend is selected. This document records how fully that
holds today.

---

## 1. Backend combinations (socket × event)

Legend: ✅ yes · ⚠️ partial/conditional · ➖ n/a. "Production-ready" is asserted
only where the combination is exercised by CI or a validated smoke — not merely
because code exists.

| Combination | Implemented | Buildable | Tested | Production-ready |
|---|---|---|---|---|
| POSIX + epoll (Linux) | ✅ | ✅ | ✅ full suite + smoke (Linux CI) | ✅ default on Linux ≤ io_uring fallback |
| POSIX + io_uring (Linux, completion) | ✅ | ✅ | ✅ 50-suite gate + smokes + LSan (container) | ✅ default on Linux (8f-5) |
| POSIX + kqueue (macOS) | ✅ | ✅ | ✅ full suite (macOS CI) | ✅ default on macOS |
| POSIX + poll (universal fallback) | ✅ | ✅ | ✅ full suite | ✅ |
| Winsock + WSAPoll (Windows) | ✅ | ✅ | ✅ 47-suite subset (Windows CI) | ✅ default on Windows |
| Winsock + IOCP (Windows, completion) | ✅ | ✅ | ✅ lifecycle + smokes: plaintext, TLS-via-mock, PROXY, chunked TransmitFile, UDP-local | ⚠️ plaintext + PROXY prod-ready; real-mbedTLS-over-IOCP is BYO/out-of-CI |
| pollcomp (portable completion double) | ✅ | ✅ | ✅ smoke + tls/ws/async/proxy/bigstream, ASan/LSan | ➖ test double, not a shipping backend |

**Event↔socket negotiation** (`kl_event_ctx_sockets_compatible`, `async.c`): a
completion loop requires an overlapped socket provider; a readiness loop requires
a native-fd provider. An incompatible pairing is rejected, never silently
downgraded (see `docs/keel_axis_audit.md` §feature-detection).

---

## 2. Per-feature readiness ↔ completion parity

Each behavior below is observably identical at the Keel level across both axes;
the mechanism differs. "Evidence" names the test/smoke that proves it on the
completion axis (the readiness axis is covered by the default `make test`).

| Behavior | Readiness mechanism | Completion mechanism | Parity evidence |
|---|---|---|---|
| Accept | `EPOLLIN`/readable → `accept` loop | `comp_on_accept` on accept completion | `test_server_integration`, smokes (all axes) |
| Recv / request read | readable → `recv` → EAGAIN re-arm | posted recv → `KL_COMP_READ` completion → re-post | `test_connection`, `test_request`, smokes |
| Read-side flow control (pause/resume) | `kl_event_mod(fd,0)` drops READ interest | `comp_start_body_read` skips next post; resume re-posts (`kl_io_engine_post_read`) | `test_read_flow_control` (both axes) |
| Send + backpressure | writable → `writev`; buffer tail in `KlDrain` | `kl_comp_post_send` (copies iovec); ≤1 in-flight; re-pump on `KL_COMP_WRITE` | `test_drain`, `smoke-*` bigstream |
| Streaming (SSE/chunked/response) | flush on writability | `comp_stream_pump` over `KlDrain`, `stream_inflight` ≤1 | `smoke-pollcomp`/`-iouring`/`-iocp` bigstream; `docs/streaming_contract.md` |
| Partial I/O (short read/write) | EAGAIN / short `writev` retried | completion reports `< requested`; driver re-posts remainder | `test_client_stream`, `multipart_stream` |
| TLS (handshake + app data) | `read`/`write` vtable on the socket | memory-BIO `feed_input`/`drain_output` | `tls`, `tls_integration`, `peer_cert` over completion; `smoke-*-tls` |
| PROXY protocol header | `kl_http_conn_read_proxy_header` (peek+consume) | `comp_drive_proxy` + `kl_http_conn_ingest_proxy` from `read_buf` | `test_peer_addr` 6/6 over completion; `smoke-*` proxy |
| Close with outstanding work | stale-event guard after close | single terminal completion; op/buffer freed once | ASan/LSan smokes; `test_read_flow_control.shutdown_while_paused` |
| Timeout | idle sweep + timer heap | same timer heap; deadline on the loop | `test_timeout`, `test_timer` |
| UDP datagram (source + local addr) | `recvmsg`/`IP_PKTINFO` | `kl_comp_post_udp_recv`/`send` (WSARecvMsg on IOCP) | `udp`, `udp_server`, `udp_batching/offload/tos` over completion |

**One documented asymmetry (by design, not a gap):** a broadcast send without
`SO_BROADCAST` surfaces `EACCES` *synchronously* on readiness but on the *send
completion* on the completion axis (queued async). `udp_multicast`'s
`broadcast_flag_gates_send` asserts the synchronous form and is readiness-only.

---

## 3. Test coverage by axis

- **Readiness:** the full `make test` suite (57 suites) runs natively on the
  default backend (epoll/kqueue) and under `make BACKEND=poll`.
- **Completion:** `IOURING_TEST_SUITES` (50 suites) runs the model-independent
  suites over the real io_uring backend (`make BACKEND=iouring test-iouring`, in
  the Apple Linux container) and over the `pollcomp` double on macOS/Linux under
  ASan/LSan. IOCP is exercised by the Windows CI smokes.

**Readiness-only by design (7 suites, excluded from the completion gate):**

| Suite(s) | Why readiness-only |
|---|---|
| `async`, `event`, `event_ctx`, `event_caps` | Drive raw `kl_event_wait` / assert readiness capability + provider negotiation; a completion loop has no `kl_event_wait`, only `kl_comp_run`. |
| `socket_provider` | Asserts native-fd provider semantics (readiness contract). |
| `iocp_engine` | Windows/IOCP-specific engine unit test (not a cross-axis suite). |
| `udp_multicast` (`broadcast_flag_gates_send` case) | Asserts a synchronous `EACCES` — only holds on readiness (see §2). |

The async-over-completion *path* is covered despite `test_async` being excluded:
`smoke-pollcomp-async` and the io_uring `/astream` case drive
`kl_async_suspend`/resume + timer over a completion loop.

---

## 4. Integrations (bring-your-own; out of core CI)

See `integrations/README.md`. Validated outside CI (BYO libraries):

| Adapter | Vtable | Validation |
|---|---|---|
| mbedTLS (`integrations/tls/mbedtls`) | `KlTls` | Real loopback HTTPS handshake + roundtrip over **both** axes (`smoke-tls`, `smoke-tls-completion-e2e`); `tls`/`tls_integration`/`peer_cert` suites. |
| nghttp2 client (`integrations/http2/nghttp2`) | `KlHttp2ClientSession` | In-memory roundtrip + **real-socket e2e** via `kl_http2_client_connect` (h2c); ASan+UBSan+LSan on nghttp2 1.64 + 1.59. |
| nghttp2 server (`integrations/http2/nghttp2`) | `KlHttp2ServerSession` | In-memory roundtrip + **real-socket e2e** via `KlHttpServerConfig.h2` (h2c prior-knowledge) + **third-party interop** (`curl --http2-prior-knowledge`); same sanitizer coverage. |

---

## 5. Known limitations

- **real-mbedTLS-over-IOCP**: exercised only via the completion-mode mock TLS in
  CI (`smoke-iocp-tls`); real mbedTLS over IOCP is a BYO/out-of-CI concern,
  consistent with the mbedTLS policy.
- **HTTP/2 over completion**: the h2 server/client run over the completion axis
  through the same session vtable; nghttp2-backed HTTP/2 has been validated over
  a real socket on readiness (see §4) — completion-axis nghttp2 e2e is not yet a
  standing test.
- **nghttp2 client vs third-party server** (`nghttpd`) and **h2spec conformance**
  need the `nghttp2` CLI-tools formula; documented as optional follow-ups in
  `integrations/http2/nghttp2/README.md`.

---

## 6. Where the rationale lives

- `docs/keel_axis_audit.md` — running architectural audit (orthogonality litmus,
  per-pass findings, why each combination is sound).
- `docs/core_completion_plan.md` — Phase 0 gap analysis and design decisions.
- `docs/streaming_contract.md` — the authoritative streaming / body-read contract
  (write-side ownership, backpressure outcomes, read-side flow control,
  termination taxonomy) shared by both axes.
