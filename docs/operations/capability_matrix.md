# KEEL capability matrix

**The single authoritative reference for what runs where and how the readiness
and completion axes compare.** It consolidates the matrix fragments previously
scattered across `docs/archive/audits/keel_axis_audit.md` (running audit log) and
`docs/archive/designs/core_completion_plan.md` (Phase 0 gap analysis). Those remain the source of
*rationale*; this file is the source of *status*. Keep it current when a backend,
feature, or integration changes.

The design intent (see `docs/archive/audits/keel_axis_audit.md`): the **event axis** (readiness
vs completion) and the **socket axis** (POSIX vs Winsock) are orthogonal; the
**protocol layer** sits above both and observes identical Keel-level semantics
regardless of which backend is selected. This document records how fully that
holds today.

---

## 1. Backend combinations (socket × event)

Legend: ✅ yes · ⚠️ partial/conditional · ➖ n/a. The last column states **factual status** —
default selection, runtime validation, and known limitations. A backend-confidence /
production-readiness classification is deferred to the roadmap's Phase E3 and is deliberately **not**
asserted here.

| Combination | Implemented | Buildable | Tested | Default selection / notes |
|---|---|---|---|---|
| POSIX + epoll (Linux) | ✅ | ✅ | ✅ full suite + smoke (Linux CI) | Default on Linux when io_uring is unavailable |
| POSIX + io_uring (Linux, completion) | ✅ | ✅ | ✅ the `IOURING_TEST_SUITES` gate (`make BACKEND=iouring test-iouring`) + smokes + LSan (container) | Default on Linux (kernel 5.6+) |
| POSIX + kqueue (macOS) | ✅ | ✅ | ✅ full suite (macOS CI) | Default on macOS |
| POSIX + poll (universal fallback) | ✅ | ✅ | ✅ full suite | Universal fallback (`make BACKEND=poll`); auto-selected under Cosmopolitan |
| Winsock + WSAPoll (Windows) | ✅ | ✅ | ✅ the `WIN_TEST_SUITES` subset (Windows CI, `make test-win`) | Default on Windows |
| Winsock + IOCP (Windows, completion) | ✅ | ✅ | ✅ lifecycle + smokes: plaintext, TLS-via-mock, PROXY, chunked TransmitFile, UDP-local | `make BACKEND=iocp`; real-mbedTLS-over-IOCP is BYO / out-of-CI |
| pollcomp (portable completion double) | ✅ | ✅ | ✅ smoke + tls/ws/async/proxy/bigstream, ASan/LSan | Test double (`make BACKEND=pollcomp`), not a shipping backend |

**Event↔socket negotiation** (`kl_event_ctx_sockets_compatible`, `async.c`): a
completion loop requires an overlapped socket provider; a readiness loop requires
a native-fd provider. An incompatible pairing is rejected, never silently
downgraded (see `docs/archive/audits/keel_axis_audit.md` §feature-detection).

**Frontier completion providers (integration, `integrations/platform/`).** Two bring-your-own
providers extend the axes to environments with no OS sockets; both keep core `libkeel` unchanged
and are injected at runtime, and the datagram axis is STABLE over them as of Step 7:

- **lwIP raw (`NO_SYS`)** — `integrations/platform/lwip` (`event_lwip_raw.c`): the whole HTTP stack
  and `KlDatagram` over lwIP's raw callback API, with no OS sockets or threads. Validated by the
  `loopback-raw` / `loopback-raw-asan` gates (HTTP + datagram over loopback, ASan/UBSan/LSan).
- **UEFI EFI_TCP4/UDP4** — `integrations/platform/uefi` (`event_efi.c`): a stock async
  `KlHttpClient` and `KlDatagram` inside UEFI firmware, before any OS. Validated by the mock-EFI
  host harness (ASan/UBSan) and a QEMU/OVMF end-to-end boot (DNS over EFI_UDP4 → HTTPS GET; a
  public-`KlDatagram` send/recv roundtrip).

---

## 2. Per-feature readiness ↔ completion parity

Each behavior below is observably identical at the Keel level across both axes;
the mechanism differs. "Evidence" names the test/smoke that proves it on the
completion axis (the readiness axis is covered by the default `make test`).

| Behavior | Readiness mechanism | Completion mechanism | Parity evidence |
|---|---|---|---|
| Accept | `EPOLLIN`/readable → `accept` loop | `comp_on_accept` on accept completion | `test_server_integration`, smokes (all axes) |
| Recv / request read | readable → `recv` → EAGAIN re-arm | posted recv → `KL_COMP_READ` completion → re-post | `test_connection`, `test_request`, smokes |
| Read-side flow control (pause/resume) | `kl_event_mod(fd,0)` drops READ interest | `comp_start_body_read` skips next post; resume re-posts (`kl_http_comp_post_read`) | `test_read_flow_control` (both axes) |
| Send + backpressure | writable → `writev`; buffer tail in `KlDrain` | `kl_comp_post_send` (copies iovec); ≤1 in-flight; re-pump on `KL_COMP_WRITE` | `test_drain`, `smoke-*` bigstream |
| Streaming (SSE/chunked/response) | flush on writability | `comp_stream_pump` over `KlDrain`, `stream_inflight` ≤1 | `smoke-pollcomp`/`-iouring`/`-iocp` bigstream; `docs/contracts/streaming.md` |
| Partial I/O (short read/write) | EAGAIN / short `writev` retried | completion reports `< requested`; driver re-posts remainder | `test_client_stream`, `multipart_stream` |
| TLS (handshake + app data) | `read`/`write` vtable on the socket | memory-BIO `feed_input`/`drain_output` | `tls`, `tls_integration`, `peer_cert` over completion; `smoke-*-tls` |
| PROXY protocol header | `kl_http_conn_read_proxy_header` (peek+consume) | `comp_drive_proxy` + `kl_http_conn_ingest_proxy` from `read_buf` | `test_peer_addr` 6/6 over completion; `smoke-*` proxy |
| Close with outstanding work | stale-event guard after close | single terminal completion; op/buffer freed once | ASan/LSan smokes; `test_read_flow_control.shutdown_while_paused` |
| Timeout | idle sweep + timer heap | same timer heap; deadline on the loop | `test_timeout`, `test_timer` |
| UDP datagram (source + local addr) | `recvmsg`/`IP_PKTINFO` | `kl_comp_post_dgram_recv`/`_send` (WSARecvMsg on IOCP) | `datagram_socket`, `datagram_batch`, `datagram_multicast`, `udp_cmsg` over completion |

**One documented asymmetry (by design, not a gap):** a broadcast send without
`SO_BROADCAST` surfaces `EACCES` *synchronously* on readiness but on the *send
completion* on the completion axis (queued async). `datagram_multicast`'s
`broadcast_flag_gates_send` asserts the synchronous form and is readiness-only.

---

## 3. Test coverage by axis

- **Readiness:** the full `make test` suite (every `tests/test_*.c` +
  `tests/protocols/*/test_*.c`) runs natively on the default backend (epoll/kqueue) and
  under `make BACKEND=poll`.
- **Completion:** the curated `IOURING_TEST_SUITES` set runs the model-independent
  suites over the real io_uring backend (`make BACKEND=iouring test-iouring`, in
  the Apple Linux container) and over the `pollcomp` double on macOS/Linux under
  ASan/LSan. IOCP is exercised by the Windows CI smokes.

**Readiness-only by design (excluded from the completion gate):**

| Suite(s) | Why readiness-only |
|---|---|
| `async`, `event`, `event_ctx`, `event_caps` | Drive raw `kl_event_wait` / assert readiness capability + provider negotiation; a completion loop has no `kl_event_wait`, only `kl_comp_run`. |
| `socket_provider` | Asserts native-fd provider semantics (readiness contract). |
| `iocp_engine` | Windows/IOCP-specific engine unit test (not a cross-axis suite). |
| `datagram_multicast` (`broadcast_flag_gates_send` case) | Asserts a synchronous `EACCES` — only holds on readiness (see §2). |

The async-over-completion *path* is covered despite `test_async` being excluded:
`smoke-pollcomp-async` and the io_uring `/astream` case drive
`kl_async_suspend`/resume + timer over a completion loop.

---

## 4. Integrations

The mbedTLS TLS backend and the nghttp2 HTTP/2 session adapter are exercised by a **standing CI
job** ("Integrations (mbedTLS + nghttp2)", `.github/workflows/ci.yml`), while the ordinary core
build stays dependency-light. **"Bring-your-own" (BYO) means the dependency/provider is not vendored
— it does not mean out-of-CI.** Standing CI coverage per integration:

- **lwIP** — its own CI job ("Integration (lwIP)"): stock-`libkeel` loopback with the lwIP
  server / client / UDP-echo providers, HTTPS-over-lwIP via the mbedTLS BIO, and the raw-completion
  backend (`loopback-raw`) under ASan+UBSan+LSan (`loopback-raw-asan`).
- **UEFI (EFI_UDP4)** — the strict PE datagram-provider gate in the **Static Analysis** job
  (`make uefi-dgram-gate UEFI_GATE_STRICT=1`, both PE arches); the QEMU/OVMF boot e2e is a local
  harness, outside standing CI.
- **OpenSSL / BoringSSL / LibreSSL** TLS backends and the **miniz** codec — BYO with no standing job
  (see `integrations/README.md`).

| Adapter | Vtable | Validation |
|---|---|---|
| mbedTLS (`integrations/tls/mbedtls`) | `KlTls` | **CI-gated**: real mbedTLS 3.6.x built from source → `make KEEL_TLS=mbedtls smoke-tls`; also real loopback HTTPS handshake + roundtrip over **both** axes (`smoke-tls`, `smoke-tls-completion-e2e`) and the `tls`/`tls_integration`/`peer_cert` suites. |
| nghttp2 client (`integrations/http2/nghttp2`) | `KlHttp2ClientSession` | **CI-gated** (`make -C integrations/http2/nghttp2 test`): in-memory roundtrip + **real-socket e2e** via `kl_http2_client_connect` (h2c), plus `h2load` and the ALPN e2e (`alpn-interop`). ASan+UBSan+LSan on nghttp2 1.64 + 1.59. |
| nghttp2 server (`integrations/http2/nghttp2`) | `KlHttp2ServerSession` | **CI-gated**: real-socket e2e via `KlHttpServerConfig.h2` (h2c prior-knowledge), **h2spec** conformance (pass-count floor 130 of ~146), and **third-party interop** — `curl --http2-prior-knowledge` (`interop-curl`) + `nghttpd` (`interop-nghttpd`); same sanitizer coverage. |

---

## 5. Known limitations

- **real-mbedTLS-over-IOCP**: exercised only via the completion-mode mock TLS in
  CI (`smoke-iocp-tls`); real mbedTLS over IOCP is a BYO/out-of-CI concern,
  consistent with the mbedTLS policy.
- **HTTP/2 over completion**: the h2 server/client run over the completion axis
  through the same session vtable; nghttp2-backed HTTP/2 is validated over a real
  socket on readiness (§4) — completion-axis nghttp2 e2e is not yet a standing test.

---

## 6. Where the rationale lives

- `docs/archive/audits/keel_axis_audit.md` — running architectural audit (orthogonality litmus,
  per-pass findings, why each combination is sound).
- `docs/archive/designs/core_completion_plan.md` — Phase 0 gap analysis and design decisions.
- `docs/contracts/streaming.md` — the authoritative streaming / body-read contract
  (write-side ownership, backpressure outcomes, read-side flow control,
  termination taxonomy) shared by both axes.
