# Keel Core Completion & Integration Plan: gap analysis (Phase 0)

Status: **audit / gap analysis (2026-08-01).** No core rewrite proposed. This is the Phase 0
deliverable for the "architecturally complete + behaviorally validated networking core" roadmap.
It records what is already done, where readiness and completion still differ, the current
ownership/cancellation rules, the missing seams, and a per-phase increment list. Design decisions
that need the maintainer's call are collected in §7.

Cross-references (do not duplicate): `docs/keel_axis_audit.md` (orthogonality, 4 passes),
`docs/keel_audit.md` (safety, 6 passes), `docs/pal_transformation_design.md` (master roadmap),
`docs/phase8*_*.md` (completion migration).

---

## 1. Baseline: what is already complete

The PAL/completion migration (Phase 8) plus this session's follow-ups landed most of Phase 1 and
all of Phase 3's completion-side wiring:

- **Completion axis is real and platform-independent.** `src/completion.h` (abstract op/drain
  contract) + `src/completion_driver.c` (the model-blind connection driver) + `src/io_engine.h`
  (run-loop seam). Backends: `event_iouring.c` (Linux, SQE/CQE), `event_iocp.c` (Windows),
  `event_pollcomp.c` (portable `poll()` double). Readiness: epoll/kqueue/wsapoll/poll.
- **Protocol/axis separation holds** (axis-audit 4th pass, mechanical grep clean): no protocol TU
  includes a platform/event header or engine symbol; `completion.h`/`kl_comp_*` do not leak into
  the protocol layer.
- **Response streaming over completion (8g-0/8g-1, #126/#130):** `KlDrain` is the default outbound
  buffer for `KL_BODY_STREAM`; `comp_stream_pump` flushes it as overlapped sends (≤1 in flight via
  `KlResponse.stream_inflight`), no head-of-line spin. Producers (SSE / chunked / handler) write to
  the drain, never a socket.
- **Request-body streaming over completion:** `comp_start_body_read` / `comp_drive_body` /
  `kl_conn_ingest_body` feed `KlBufReader`/`KlMultipartReader` over the overlapped recv path.
- **TLS over completion:** memory-BIO `feed_input`/`drain_output` ops + `comp_tls_drive` /
  `comp_tls_send_response/file/stream`. Real mbedTLS proven end-to-end over the completion loop
  (`smoke-tls-completion-e2e`, #138) and in-memory (`smoke-tls-completion`).
- **HTTP/2, WebSocket, UDP, PROXY over completion:** `comp_h2_drive`, `comp_ws_drive`, UDP recv/send
  with local-addr (`WSARecvMsg`/`recvmsg` cmsg parity), PROXY-header phase (`comp_drive_proxy`, #136).
- **File responses:** zero-copy `splice` (io_uring), `TransmitFile` incl. >2 GiB chunking (IOCP, #133),
  pread+send (pollcomp).
- **Test parity so far:** `IOURING_TEST_SUITES` = 49 model-independent unit suites run over io_uring;
  the four TLS suites + `peer_addr` now run over completion (#135/#136). `smoke-pollcomp`,
  `smoke-iouring`, `smoke-iocp` cover the full protocol surface per backend. `make debug-test` (55
  suites, ASan+UBSan) is 0-failure/0-hit.

**No `integrations/` directory exists. No nghttp2 code exists anywhere** (the `KlH2*Session`
vtables are generic; all h2 tests use a mock echo session). **mbedTLS already exists in core** as
`src/tls_mbedtls.c` + `include/keel/tls_mbedtls.h` (a maintained adapter, `KEEL_TLS=mbedtls`).

---

## 2. Readiness vs completion: behavior deltas (the parity gap)

| Behavior | Readiness | Completion | Delta |
|---|---|---|---|
| Response streaming flush | `kl_conn_on_writable` non-blocking send + re-arm | `comp_stream_pump` overlapped, ≤1 in flight | **Equivalent** (both drain `KlDrain`) |
| Request-body read | recv loop → `kl_conn_ingest_body` | overlapped recv → same core | **Equivalent** |
| Read-side flow control | `on_data`→-1 aborts; **no pause** | same | **Gap both axes** (Phase 1) |
| Backpressure signal | `KlDrain.on_drain` (server streaming); client has its own | same drain | **Naming/contract not unified** (Phase 1/4) |
| PROXY header | `kl_conn_read_proxy_header` (socket peek) | `kl_conn_ingest_proxy` (read_buf) | Equivalent (shared `kl_proxy_parse`) |
| Cancellation of an in-flight op | interest removed + fd closed | `kl_comp_cancel` (CancelIoEx / ASYNC_CANCEL / poll_remove) → one aborting completion | Equivalent-by-design; **contract undocumented** (Phase 4) |
| `udp_multicast` sync-EACCES | synchronous | queued async (error on send completion) | **Genuinely different by model** (documented) |

Net: the *observable* protocol behavior is equivalent for everything that's implemented; the gaps
are (a) read-side pause/resume missing on **both** axes, and (b) the contracts (ownership,
backpressure outcomes, terminal results) are implemented but not *documented/normalized* as a
single spec.

---

## 3. Current ownership & lifetime rules (as implemented)

- **Completion send buffers are copied.** `kl_comp_post_send` copies the caller iovec into an
  op-owned buffer in all three backends → callers may pass transient/stack memory and free the
  source immediately. (`KlDrain` bytes are consumed after the copy; safe.) This already satisfies
  the roadmap's write-side "copied immediately" contract; it is **not yet documented as a promise**.
- **Recv buffers** are op-owned (TLS ciphertext) or the conn's `read_buf` (plaintext).
- **≤1 in-flight op per conn** (recv XOR send, posted only from a completion) → `kl_comp_cancel`
  yields exactly one aborting completion → one `comp_close`/release. No double-free (audit-verified).
- **`KlAsyncOp`** (`async.h`) is caller-owned, must outlive suspension; three callbacks
  (`on_resume`/`on_deadline`/`on_cancel`); exactly one fires per suspension in practice, but this
  is not stated as a formal "one terminal result" contract.

## 4. Current cancellation & terminal-result rules

- Idle/read timeouts and teardown call `kl_comp_cancel(ctx, fd)`; the cancellation completion is the
  single terminal event; late/duplicate CQEs are discarded via `aborted`/`removed` sentinels
  (io_uring/pollcomp) or CancelIoEx semantics (IOCP).
- Timers (`kl_timer_*`), DNS (`dns_resolver`), client (Happy-Eyeballs + deadline), connect, and
  thread-pool work each have their **own** cancel/deadline shape. There is no single documented
  "cancellation is synchronous-vs-requested / callback-ordering / completion-races-cancel" rule.

---

## 5. Gaps by roadmap phase, with proposed increments

**Phase 1: finish completion streaming (mostly done; two real gaps).**
- (a) **Read-side flow control:** add explicit `continue / pause / abort` to the body-read path.
  Today `on_data` returns 0 (continue) / -1 (abort); **pause** (stop posting recvs without
  aborting, resume explicitly + idempotently) is missing on both axes. Smallest change: a
  pause/resume on the reader (or a new return value) that stops `kl_comp_post_recv` / readiness
  re-arm and a `kl_conn_resume_read` that re-arms once. Bounded accumulation already holds
  (read_buf is fixed; the parser retains partial input).
- (b) **Document + test the streaming contract** (ownership = copied; backpressure outcomes =
  accepted/would-block/closed/error via the drain; termination = finish/abort/cancel/peer-close/
  timeout; stream-object validity during callbacks). Add the Phase-1 test matrix (single-byte
  fragmentation, split header/body completions, backpressure entry/exit, pause/resume, cancel while
  paused, cancel while write outstanding, shutdown-during-stream, peer reset, timeout, alloc
  failure, no-callback-after-destroy) as model-independent tests over pollcomp first.

**Phase 2: first-party integrations (net-new scaffolding; mbedTLS relocation decision).**
- Create `integrations/{Makefile,README.md,mbedtls/,nghttp2/}`; optional, own Makefiles, own
  tests, version matrix, `*_CFLAGS`/`*_LIBS` + optional `pkg-config`. Root convenience targets
  `integration-mbedtls|nghttp2|integrations|integration-test`. `make`/`make test` stay
  dependency-light.
- **mbedTLS:** an adapter already exists (`src/tls_mbedtls.c`). Decision in §7: relocate into
  `integrations/mbedtls/` vs. keep in `src/` and point the integration scaffolding at it. Either
  way it already implements the generic `KlTls` vtable with no mbedTLS types in core headers; the
  gaps vs. the roadmap's test list are mostly **test coverage** (SNI/ALPN/mTLS/WANT_*/cancel-during-
  handshake/streamed bodies over completion), not new code.
- **nghttp2:** net-new adapter implementing `KlH2ClientSession`/`KlH2ServerSession`; the demanding
  real consumer that will exercise the generic h2 + streaming + flow-control contracts.

**Phase 3: parity matrix (largely done).** Refactor test fixtures so model-independent suites take
a backend by fixture, not source edit; produce ONE authoritative capability matrix (extend the one
in `keel_axis_audit.md` §4 rather than adding a competing list). Most coverage exists; the delta is
fixture ergonomics + a single matrix.

**Phase 4: normalize async semantics (design + selective migration).** Define a minimal common
lifecycle (pending → exactly-one terminal: success/failure/cancel/timeout/peer-close; explicit
cancel; optional monotonic deadline; error retrieval; no futures). Document cancel sync-vs-requested,
callback ordering, reentrancy, destruction, parent/child shutdown, deadline-vs-cancel precedence,
completion-races-cancel. Migrate only where it removes real duplication (`KlAsyncOp` is the anchor).

**Phase 5: generic transport surface (genuine gap).** Today protocol authors have the socket
*provider* API (`keel/socket.h`, bring-your-own stack) and `KlEventCtx`/watchers, but **no
high-level listener / connected-byte-stream / datagram-endpoint API**; non-HTTP authors would reach
into `KlServer`/`KlConn`. Propose the smallest coherent public surface over existing primitives
(accept/connect/read/write/half-close/cancel/deadline/backpressure/TLS-wrap) + one framed-echo
test consumer proving no HTTP dependency. Design-first; no HTTP rewrite.

**Phase 6: API hardening.** Decide + document the compatibility promise (source / ABI / static-
relink). Add `struct_size`/`api_version` to integration-facing vtables/configs **only** if we
promise more than static-relink. Document ownership/thread-affinity/reentrancy on integration APIs.

**External conformance.** Add documented targets (not runtime deps) for h2spec, nghttp2 tools
(`nghttp`/`nghttpd`/`h2load`), OpenSSL `s_client`/`s_server`, Autobahn, and a smuggling corpus,
wired into CI where the tools are available.

---

## 6. Duplicated / feature-specific APIs to watch

- Backpressure/drain: server streaming uses `KlDrain.on_drain`; the client has its own streaming
  callbacks (`KlClientStreamCfg`). Normalize the *vocabulary* (accepted/would-block/closed/error)
  in Phase 1/4 without forcing a single callback where protocols legitimately differ.
- Proxy ingest: two axis-specific wrappers over one shared parser (`kl_proxy_parse`); correct
  split, not duplication.
- HTTP/2: `h2.h` + `h2_client.h` + `h2_server.h`; verify the nghttp2 adapter implements both
  client and server through these without a third surface.

## 7. Design decisions to confirm (maintainer's call)

1. **mbedTLS location.** Relocate the existing `src/tls_mbedtls.c` + `include/keel/tls_mbedtls.h`
   into `integrations/mbedtls/` (single canonical home, matches the roadmap; requires Makefile +
   `KEEL_TLS=mbedtls` → `integration-mbedtls` migration and updating the BYO smokes), **or** keep it
   in `src/` (no churn) and make `integrations/mbedtls/` the tests/version-matrix/README wrapper
   that builds it? Recommendation: **relocate**; it's what the roadmap intends and gives one home,
   but it touches the build + the smoke targets, so confirm before moving.
2. **Compatibility promise (Phase 6).** Given static-linking + W^X goals, recommend **source +
   static-relink compatibility only** (no ABI promise), and add `struct_size`/`api_version` only to
   the new integration vtables. Confirm.
3. **Pacing.** This is a 6-phase, multi-PR effort. Recommend: land Phase 1 (read-side flow control +
   streaming contract doc + tests) and the Phase 2 `integrations/` scaffolding + nghttp2 adapter as
   the first substantive PRs, checking in between phases; rather than one large branch.

## 8. Risks of changing public structures

- `KlBodyReader` (read-side pause); adding a return value or a pause/resume call is additive but
  touches a public vtable; prefer a new function + a new `on_data` return code over changing the
  signature.
- `KlAsyncOp`; widely used (server, Hull); normalize via docs + helpers, avoid struct-layout
  changes.
- `KlResponse`/`KlConn`; internal-ish but large; Phase 5's transport surface must not require
  exposing them. Keep additive.
