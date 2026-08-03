# Phase 8f step 5 — make io_uring-completion the default io_uring backend — Design

**Status:** designed (step 5 of the io_uring-completion migration). Steps 1–4 landed the
completion-native backend (`event_iouring_comp.c`), splice + registered buffers, the
unit-suite gate, and the benchmark. Step 4's data is decisive:

| backend | `GET /hello` | `POST /echo` |
|---|---|---|
| epoll (readiness) | 73,312 req/s | 72,479 |
| io_uring (readiness-adapted) | 64,951 req/s | 63,082 |
| **io_uring (completion-native)** | **95,418 req/s** | **90,872** |

Completion-native io_uring is ~30 % faster than epoll and ~47 % faster than the readiness-
adapted `event_iouring.c` — which is itself *slower than plain epoll* (a `poll_add`
multiplexer paying readiness overhead with no completion payoff). So `BACKEND=iouring`
should mean the **completion** backend, and the readiness `event_iouring.c` +
`file_io_iouring.c` should be retired.

---

## 1. The one real blocker: the overlapped provider

`event_iouring_comp.c` advertises `KL_EVENT_CAP_COMPLETION`, and the Phase-7 negotiation
(`kl_event_ctx_sockets_compatible`, enforced at `kl_server_init` server.c:474 and the two
client entry points client.c:2237/2760) requires an **overlapped** socket provider on a
completion loop. Every consumer — examples, the test suite, Hull — creates servers/clients
with the **default** POSIX provider. So naively flipping `BACKEND=iouring` to the completion
backend would make `kl_server_init` reject them all (exactly the step-3 crashes: init
returns −1, callers that ignore it fault). Today only the smokes work over completion
because they explicitly pass `kl_socket_provider_iouringcomp()`.

**This is the whole problem step 5 must solve.** The rest (removing files, CI, docs) is
mechanical once it is solved.

---

## 2. The enabler (5a): auto-select the backend's overlapped provider

The founding orthogonality principle is that *the public API masks the two event
philosophies* — a user configures the same server whether the drive is readiness or
completion; the server picks the mechanism from `kl_event_caps`. Provider selection is the
last place that isn't yet true. Make it true:

**A new backend hook** (internal, `event.h` / `event_caps.h`):

```c
/* The overlapped socket provider this loop needs, or NULL if it runs on the default
 * (readiness) provider. A completion backend returns its provider; readiness backends
 * return NULL. Lets the server/client auto-wire the matching provider without naming any
 * backend — the axis stays masked. */
const KlSocketProvider *kl_event_native_provider(const KlEventLoop *loop);
```

- `event_iouring_comp.c` (and `event_iocp.c`, `event_pollcomp.c`) return their overlapped
  provider (`kl_socket_provider_iouringcomp()` etc.); every readiness backend returns `NULL`
  (a one-line stub, like `kl_event_caps`).
- `kl_server_init` / the client, right before the compatibility check: **if no provider was
  configured (or the configured one is incompatible) and the loop offers a native provider,
  adopt it.** Only then run `kl_event_ctx_sockets_compatible`. Pseudocode at server.c:466:

  ```c
  s->ev.sockets = s->config.sockets;
  if (!s->ev.sockets || !kl_event_ctx_sockets_compatible(&s->ev)) {
      const KlSocketProvider *np = kl_event_native_provider(&s->ev.loop);
      if (np) s->ev.sockets = np;          /* completion loop → its overlapped provider */
  }
  if (!kl_event_ctx_sockets_compatible(&s->ev)) { /* still bad → reject as today */ }
  ```

**Effect:** a server/client built with `BACKEND=iouring` (completion) and the default
provider now *just works* — the loop auto-wires its overlapped provider. No consumer change,
no public API change, no `#ifdef` in shared code (the hook is backend-selected by the
Makefile, like `kl_event_caps`). An explicitly-configured, compatible provider is still
honoured; an explicitly-configured *incompatible* one is still rejected (a real
misconfiguration). This is the orthogonality principle finally complete: **the axis is
invisible above the build flag.**

Independently valuable + low-risk + mergeable on its own — and it retroactively fixes most
of the step-3 exclusions (the default-provider integration suites now init cleanly over
completion).

---

## 3. Full suite over completion (5b)

With 5a in place, `make BACKEND=iouring test` (completion) runs the whole suite, not just
the 29-suite gate. Expected remaining exclusions shrink to the *inherently* readiness ones:

- **Raw `kl_event_wait` drivers** — `test_event`, `test_event_ctx`, and the `kl_event_wait`
  cases of `test_async`. A completion loop has no readiness `kl_event_wait`; these assert the
  readiness API itself. Keep excluded (or split the readiness-API cases out).
- **`test_event_caps`** asserts `CAP_READINESS && !CAP_COMPLETION` for the backend — true for
  epoll (its job), false for a completion `BACKEND=iouring`. It is a per-backend cap suite;
  it stays in the readiness jobs, excluded from the completion run (already is).
- **`test_file_io_iouring`** tests the readiness io_uring file backend — removed with it (5d).

**Result (re-survey with 5a in place).** The gate grew 29 → **34**: 5a's auto-wire moved
`client_happy_eyeballs`, `client_pool`, `error`, `server_stats`, `timeout` from fail to pass
(a default-provider server/client now auto-adopts the overlapped provider instead of being
rejected at `kl_server_init`). The **inherently-readiness** suites stay excluded as expected.

**Post-flip triage (2026-07-30): gate grown to 36.** `test_integration` and
`test_server_integration` now pass over completion and joined the gate, once two real
completion-run-loop bugs were fixed:
1. **Prompt teardown** — `kl_server_stop` gained a self-pipe wakeup (the `KlPlatWakeup` the
   thread pool uses), so a cross-thread stop wakes the loop immediately instead of waiting out
   the ≤1 s tick. This alone turned `test_integration`'s >30 s timeout into 2 s.
2. **Graceful drain over completion** — the completion run-loop branch never ran the
   drain-progress step (`draining` → close idle conns / stop at deadline), so a server with
   `drain_timeout_ms` on a completion loop *never exited drain mode* — a genuine completion
   deadlock (not "slow teardown," as first suspected), hanging `test_server_integration`'s
   drain tests. Fixed by factoring `kl_server_drain_progress` and calling it from **both** the
   readiness and completion branches.

The remaining default-provider suites (`client`, `client_stream`, `redirect`, `peer_addr`,
`peer_cert`, `tls_integration`, `udp`, `udp_server`, `udp_multicast`, `udp_offload`,
`unix_socket`, `dns_resolver`, `request`, `cross_module`) init over completion (5a) but have
per-suite behavioural gaps left to triage incrementally — not a correctness prerequisite (the
smokes + 36 unit suites + the benchmark back the backend).

**Triage complete (2026-08-03): gate at 55.** The "~14 remaining" above were enrolled across
subsequent completion-parity fixes (timer firing in `kl_comp_run`, shared completion-capable
`mock_tls.h`, completion PROXY-header phase, UDP cmsg parity — each documented in the
`IOURING_TEST_SUITES` comment). A final coverage sweep then added the 5 backend-agnostic
unit/seam suites (`alpn`, `event_provider`, `sockaddr`, `stream_transport`, `version`), verified
green via `make BACKEND=iouring test-iouring` in the Apple container (kernel 6.18) on a real
ext4 checkout. The **final** exclusion set is now only:
- **Inherently readiness-axis** — `event`, `event_ctx` (raw `kl_event_wait` drivers; a completion
  loop has only `kl_comp_run`), `event_caps`, `socket_provider` (readiness cap / provider-
  negotiation assertions — the latter also holds the readiness-path mock `KlDatagramOps` tests).
- **`udp_multicast`** — `broadcast_flag_gates_send` asserts a *synchronous* `EACCES`, which only
  holds on readiness (completion sends are queued async; the error surfaces on the send completion).
- **`async`** — over io_uring its synthetic conn (built with no `ctx`) segfaults in
  `kl_comp_post_send` on the resume-posts-a-send path. An async-over-completion *test-harness* gap
  (the suspend/resume suite needs a completion-capable conn fixture), tracked as follow-up; the
  real async-over-completion server path is covered by the smokes + `integration` suites.
- **`iocp_engine`** — Windows-only (won't build on Linux).

---

## 4. Second-host benchmark confirmation (5c)

Step 4's numbers came from a shared x86 CI VM. A second run on different hardware — an Apple
M1 Max (ARM64) in a dedicated Apple `container` VM, kernel 6.18, `io_uring_disabled=0`,
`wrk -t4 -c100 -d8s`, loopback, **build-once + 5 sample rounds per backend** (median below) —
gives a **materially different picture**, which is exactly why this gate exists:

| backend | `GET /hello` (x86 CI) | `GET /hello` (ARM M1, median of 5) |
|---|---|---|
| epoll | 73,312 | **~347K** (310–351K) |
| io_uring readiness | 64,951 | **~149K** (137–151K) |
| io_uring completion | 95,418 | **~310K** (297–341K) |

**The x86 ordering does not replicate on ARM.** On x86 CI, completion was +30 % over epoll;
on the M1 (5 rounds, tight spread), **epoll is ~10–12 % *ahead* of completion**. What *does*
hold on **both** arches — and strongly, on every one of the 5 ARM rounds — is that
**readiness-adapted io_uring is by far the worst**: below epoll on x86, and ~2.3× slower than
both epoll and completion on ARM (completion is ~2.1× faster than readiness).

**Implications for 5d (revised, honest):**

- **Retiring the readiness `event_iouring.c` is justified everywhere** — it is the weakest
  backend on every host measured. This is the solid, arch-independent conclusion.
- **"Completion is the fastest" is x86-specific, not universal.** So 5d should frame the flip
  as *"completion-native is the right io_uring backend (readiness-io_uring is retired)"*, not
  *"completion beats epoll."* On ARM, epoll is ~10–12 % ahead of completion; on x86 completion
  is +30 % ahead of epoll. The default Linux backend therefore **stays epoll** (io_uring is
  opt-in via `BACKEND=iouring`) — a safe choice on both arches — so the flip only changes
  which *io_uring* backend you get, and both hosts agree that should be the completion one
  (2–2.3× over readiness).
- A dedicated bare-metal box (not a laptop VM) would sharpen absolutes further, but two
  independent hosts, 5 rounds each, already agree on the decision-relevant ordering
  (readiness ≪ {epoll, completion}), so 5c is satisfied as a gate.

(Absolute M1 throughput is ~3–4× the x86 CI VM simply because the M1 Max in a clean dedicated
VM is a far faster host than a shared CI runner — the site's headline peak reflects this.)

---

## 5. The flip + retirement (5d) — **done**

- **Makefile:** `BACKEND=iouring` builds `src/event_iouring.c` (the completion TU — the old
  readiness TU's name was reclaimed) + `file_io.c` + `completion_driver.c` + `-luring`. The
  transitional `iouringcomp` alias was **dropped** in the same pass — `iouring` everywhere.
- **Removed** the readiness `event_iouring.c`, `src/file_io_iouring.c`, `src/iouring_internal.h`,
  and `tests/test_file_io_iouring.c`. File responses ride zero-copy `splice` (8f-2); the
  completion path never calls `KlFileIO.submit` (it uses `kl_comp_post_sendfile`), so the
  io_uring async-read backend was dead — `kl_file_io_create` is the POSIX NULL stub in
  `file_io.c`. The completion backend's provider is `kl_socket_provider_iouring()`.
- **CI:** the "Linux (io_uring)" *full-suite matrix entry was removed* — a completion backend
  can't run the whole `make test` (the readiness-shaped suites are excluded; see §3), and it
  is fully covered by the dedicated **Completion (io_uring)** smoke job + **Completion
  (io_uring) unit suite** (`make BACKEND=iouring test-iouring`). `test_file_io_iouring` is gone
  from `TEST_SRC`.
- **Docs:** README, `CLAUDE.md` module list, and this doc updated — `BACKEND=iouring` is
  completion-native (SQE/CQE, splice, registered buffers); the readiness POLL_ADD adapter is
  retired.

**Validation:** in an Apple `container` Linux VM, `make BACKEND=iouring` builds the completion
backend and `make BACKEND=iouring smoke-iouring` passes the full roundtrip surface (GET/POST/
sendfile-via-splice/stream/UDP/h2c/h2-pk/idle/keepalive/resilience/large). Native (macOS)
default build stays clean.

**Note (per §3 triage):** the `kl_server_stop` active-wakeup + the completion graceful-drain
fix landed (§3), bringing `integration` + `server_integration` into the gate (36 suites). ~14
default-provider suites still have per-suite behavioural gaps over completion and are not yet
gated; they now *init* (5a), so finishing that triage is follow-up work, independent of this flip.

---

## 6. Staging

| Increment | Content | Gate |
|---|---|---|
| **5a** | `kl_event_native_provider` hook + auto-wire in server/client | existing suites stay green; a new test: default-provider server over `iouringcomp` now inits + serves |
| **5b** | full suite over `BACKEND=iouringcomp`; fix/triage remainder | `make BACKEND=iouringcomp test` green bar the inherent-readiness few |
| **5c** | bare-metal `bench-compare` confirmation | ordering holds (gate, not code) |
| **5d** | flip `BACKEND=iouring` → completion; remove readiness io_uring TUs; CI/docs | all jobs green; `event_iouring.c`/`file_io_iouring.c` gone |

5a is the linchpin and lands first (independently useful — it makes IOCP and pollcomp
drop-in too). 5d is the irreversible one and lands last, behind 5c's confirmation.

**Status: 5a–5d all landed**; io_uring is completion-native, the readiness adapter retired.
Post-flip: the `kl_server_stop` wakeup + completion graceful-drain fix landed and grew the
unit-suite gate to 36 (`integration` + `server_integration` now pass over completion).
Remaining follow-up (independent of the flip): the per-suite triage of the ~14 remaining
default-provider suites over completion.

---

## 7. Orthogonality litmus

| Axis | How step 5 holds it |
|---|---|
| Public API masks the axis | 5a *completes* this — provider selection becomes automatic; a completion `BACKEND=iouring` server is written exactly like an epoll one. No public API change. |
| Abstract axis vs implementation | the new hook is on the abstract `event.h` seam; each backend answers it (`NULL` or its provider) — no backend named in shared code, no `#ifdef`. |
| No protocol percolation | unchanged — h2/WS/async already ride the reused driver. |
| No `include/keel/` change | the hook is internal (`event.h` is internal? — if public, additive-only: a new internal-use function, not part of the documented API). |

---

## 8. Risks

- **Flipping a default is user-visible.** `BACKEND=iouring` changes meaning. Mitigated by:
  the completion backend is a strict superset behaviourally (same protocols, faster), and 5a
  makes it source-compatible (no consumer change — a default-provider server just works). The
  transitional `iouringcomp` alias was dropped once the flip settled; the readiness backend
  remains in git history if ever needed.
- **Kernel-feature floor.** The completion backend needs io_uring recv/send/accept/cancel
  (5.5–5.6+), splice (5.7+), and — for full speed — is happiest on 5.13+/5.19+ (multishot).
  The readiness backend worked on any io_uring kernel. Document the floor; the graceful
  fallbacks (single-shot, pread+SEND, malloc+SEND) keep older kernels correct if slower.
- **`file_io_iouring` async-read parity.** Its true-async file reads are replaced by splice
  for file bodies; confirm no other consumer relied on `KlFileIO` async reads over the
  io_uring backend before deleting.
- **Memlock footprint.** Already addressed in 8f-3 (256 KiB pool); a completion loop is a
  touch heavier than an epoll fd but within normal limits.

**Recommendation:** implement **5a first** (the provider-auto-wire — small, high-leverage,
independently valuable), then 5b, gate on 5c, and only then 5d.
