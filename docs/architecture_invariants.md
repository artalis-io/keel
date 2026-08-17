# KEEL — Architecture Invariants

**Status:** living document (R0). These are the *enforceable* invariants of Keel's networking
architecture — the rules every increment must preserve. Each is stated once, with why it matters
and an **anchor**: a header contract, a source seam, an executable gate, or a test that pins it to
real code. If an anchor and the code disagree, the code is authoritative and the anchor is a bug to
fix — not the other way round.

This document is deliberately about the **three-axis transport core**, not the whole HTTP feature
set. For the layered internals (parsing, routing, body readers, response modes) see
[architecture.md](architecture.md); for the historical audit trail see
[audits/README.md](audits/README.md).

## Vocabulary

Introductory reasoning uses three nouns (see [keel_improvement_roadmap.md](keel_improvement_roadmap.md)):

| Noun | Meaning | Canonical anchor |
|---|---|---|
| **Transport** | Semantic contract: `KlListener`, `KlStream`, `KlDatagram` | [stream.h](../include/keel/stream.h), [datagram.h](../include/keel/datagram.h), [listener.h](../include/keel/listener.h) |
| **Engine** | Execution model: readiness or completion | [event.h](../include/keel/event.h), [completion.h](../src/completion.h) |
| **Provider** | Network stack: POSIX, Winsock, lwIP, EFI | [socket.h](../src/socket.h), [socket_posix.c](../src/socket_posix.c) |

"Event backend", "completion backend", and provider names remain valid implementation terms.

---

## The invariants

### I1 — Semantic transports do not expose readiness/completion differences

A protocol built on `KlStream`/`KlDatagram`/`KlListener` sees the *same* Keel-level behavior
regardless of whether the loop underneath is readiness or completion. The engine model is an
implementation detail of the driver, never a branch in protocol code.

- **Why:** it is what makes the event axis replaceable. If protocols could observe the model, every
  backend swap would be a protocol regression.
- **Anchor:** the transport banners in [stream.h](../include/keel/stream.h) and
  [datagram.h](../include/keel/datagram.h) both declare a *model-agnostic* contract; the datagram
  facade proves it by feeding two capability-selected adapter tables into one state machine
  ([datagram.c](../src/datagram.c) — `dg_comp_*` vs `dg_rdy_*`).
- **Enforced by:** the same-facade live test runs over readiness and completion via
  `kl_event_ctx_init` (`tests/test_datagram_live.c`); the axis matrix in
  [keel_axis_audit.md](keel_axis_audit.md).

### I2 — Readiness registers interest; completion submits owned operations

The two engine models keep their native semantics. Readiness = register interest → wait → perform
op → handle `EAGAIN` → re-arm. Completion = construct owned op → submit → track lifetime → receive
completion → retire/cancel/resubmit. No **production** backend is emulated in terms of the other
(epoll/kqueue/WSAPoll/poll stay readiness; io_uring/IOCP stay completion). The one deliberate
exception is **pollcomp** — a portable *test double* that implements the completion contract over
`poll()` so the completion driver can be exercised under ASan on any POSIX host. It is explicitly a
CI/testing backend; the invariant this states is that production backends preserve their native
model, not that no adapter may ever bridge them for testing.

- **Why:** honest production models are correct and fast; a lowest-common-denominator emulation
  (shipping epoll-as-completion, or IOCP-as-readiness, in production) is both slower and a source of
  lifetime bugs. pollcomp is confined to CI precisely so production never pays that cost.
- **Anchor:** readiness interest is [event.h](../include/keel/event.h) `kl_event_add`/`_mod`/`_del`;
  completion submission is [completion.h](../src/completion.h) `kl_comp_post_*` with by-value op
  descriptors. Selection is [event_caps.h](../src/event_caps.h) (`KL_EVENT_CAP_COMPLETION`).
- **Enforced by:** [event_provider_design.md](event_provider_design.md); the capability negotiation
  `kl_event_ctx_sockets_compatible()`.

### I3 — Logical close is distinct from physical retirement

Calling close/cancel on a transport is a *logical* request. The underlying storage (op state,
buffers, the transport object) must not be reused or freed until the backend proves the operation
can no longer touch it. `on_close` fires exactly once, only after every posted op has physically
retired.

- **Why:** on a completion engine a "cancelled" op can still complete later; freeing on the logical
  close is a use-after-free.
- **Anchor:** the CLOSE facet in [stream.h](../include/keel/stream.h) ("CONFIRMED DETACHMENT — on_close
  fires once, only after both the receive and send ops are physically retired") and lifetime #1 in
  [listener.h](../include/keel/listener.h).
- **Enforced by:** [datagram_contract.md](datagram_contract.md) close section;
  [datagram_step7b9_efi_close_design.md](datagram_step7b9_efi_close_design.md) (the hardest close case).

### I4 — Callback reentrancy and synchronous completion are supported

An arm/submit may complete *synchronously*, before it returns, and a callback may run reentrantly
or earlier in the same drained completion batch than a related event. Transport state machines are
written to survive this (iterative arm trampolines, not recursive re-arm).

- **Why:** several providers (and the resolver contract) legitimately complete inline; code that
  assumes async-only delivery corrupts state under them.
- **Anchor:** the READ facet in [stream.h](../include/keel/stream.h) ("sync-completion-safe —
  iterative arm trampoline"); the resolver sync-completion contract in [CLAUDE.md](../CLAUDE.md).
- **Enforced by:** the pollcomp double drives synchronous/scripted completions
  (`make smoke-pollcomp-asan`); public-facade mock tests (`tests/test_datagram_public.c`).

### I5 — Completion-batch targets outlive every event that can reference them

Any object a completion event references (op, buffer, transport core, life token) stays valid until
*after* every event in the drained batch that could reach it has been dispatched. Ownership of a
life reference transfers into the op on submit and releases exactly once at its terminal event.

For classes that carry a raw `target` instead of a life token (stream, accept, connect, watcher),
the equivalent guarantee comes from **single-shot completion** — every backend emits exactly one
completion per submitted op, with no duplicate and no post-retirement completion — combined with the
class-specific guard (stream inflight-pin, accept force-reap, connect physical-abort). **Single-shot
is therefore a load-bearing contract:** a new completion backend must uphold it or supply its own
stale-completion guard (R3a inventory; R3b decision). **Covered** by `tests/test_stream_single_shot.c`
(R3b-T1) — a real-seam regression over pollcomp (deterministic oracle) + native io_uring / IOCP:
post one READ / one WRITE, drain the sole completion, then trigger more peer activity and drain again,
asserting no second completion.

The **watcher** guard is the `kl_event_dispatch` ctx-list scan **plus batch-bracketed deferred
reclamation** (R3b-W): the scan alone matched by pointer identity and had a pointer-reuse ABA hole
(a freed node's address reused within the same drained batch misdelivered the stale event; single-
shot did not prevent it — the stale event is the legitimate one completion). **Resolved** by
`kl_watcher_del` deferring a node's free while a dispatch bracket is open
(`kl_event_ctx_dispatch_begin`/`end` around all three internal loops), so the address cannot be
reused mid-batch. Regression: `tests/test_watcher_aba.c` (readiness / completion / server loops +
nested / overflow / depth-0), demonstrated to fail against the pre-fix behavior.

- **Why:** two related events in one batch, where the first frees the second's target, is the
  canonical completion UAF. A generation/lifetime token makes stale targets a safe no-op instead;
  for raw-`target` classes, single-shot + the class guard makes the freed-then-referenced sequence
  unreachable rather than merely survivable.
- **Anchor:** [completion.h](../src/completion.h) `KlCompletionEvent.life` + `retain_life` (the
  borrowed-vs-transferred rule), released *iff* `!retain_life` at all three sites —
  [completion_core.c](../src/completion_core.c), [datagram.c](../src/datagram.c), [udp.c](../src/udp.c).
- **Enforced by:** ASan/UBSan/LSan over the completion driver (`make smoke-pollcomp-asan`,
  container `make smoke-iouring-asan`); the R3 lifetime audit builds the full target table.

### I6 — Platform backends are transport-mechanical, not protocol-aware

Event/completion backends and socket providers move bytes and deliver ops. They contain no HTTP,
TLS, WebSocket, HTTP/2, or PROXY knowledge, and no host socket-address types leak up into protocol
TUs. The backend receives operation kind, opaque target/lifetime, buffers, lengths, and mechanical
flags — never protocol-state enums.

- **Why:** protocol knowledge in a backend duplicates decisions per engine and couples the axes.
- **Anchor:** completion ops are neutral by-value descriptors (`KlDgramSendOp`/`KlDgramRecvOp` in
  [completion.h](../src/completion.h)); the address seam is [socket.h](../src/socket.h) (`KlSockAddr`).
- **Enforced by:** `make check-sockaddr-neutral` ([Makefile](../Makefile)) + the protocol-header grep
  in [keel_axis_audit.md](keel_axis_audit.md) Goal 4. (R4 extends this to a protocol-state check.)

### I7 — Integration-owned types do not enter `include/keel/*.h`

Optional third-party/platform implementations (nghttp2, mbedtls, lwIP, EFI) live under
`integrations/` and implement Keel-owned seams. Their dependency headers and types never appear in
the installed public API.

- **Why:** the core `libkeel` must build and install without any integration's dependency present.
- **Anchor:** `include/keel/*.h` references no `nghttp2_*`/`mbedtls_*`/`lwip`/`efi` type;
  integrations sit in `integrations/` (see [event_provider_design.md](event_provider_design.md)).
- **Enforced by:** the freestanding-header gate `make freestanding-headers` ([Makefile](../Makefile)) —
  the public subset compiles with no hosted libc and no integration present.

### I8 — Hot transport paths allocate no memory

Steady-state send/recv/accept allocate nothing. Buffers and op slots are preallocated at init;
backpressure is a bounded, fixed-capacity queue, not a growing one.

- **Why:** allocation on the data path is a latency and failure-mode hazard; bounded queues are also
  the backpressure mechanism.
- **Anchor:** the datagram fixed-slot send queue ([datagram.h](../include/keel/datagram.h) —
  "packet-slot bounded send queue"); the pre-allocated connection pool and inline `read_buf`
  ([architecture.md](architecture.md), "No `malloc` during request handling"). All allocation goes
  through [allocator.h](../include/keel/allocator.h).
- **Enforced by:** ASan/LSan smokes show no per-request allocation churn; the fixed-slot admission
  tests in [datagram_contract.md](datagram_contract.md).

### I9 — Uncertain retirement quarantines storage rather than guessing

When a backend cannot prove an operation has retired (an abandoned firmware token that may still
write an inbound buffer), it *quarantines* the storage — retains the reference fail-closed — instead
of releasing and risking a UAF.

- **Why:** on exotic providers (EFI) physical retirement is genuinely unprovable at close; leaking a
  bounded amount of storage is strictly safer than a use-after-free.
- **Anchor:** the borrowed-ref path in [completion.h](../src/completion.h) (`retain_life=1`) and the
  `KL_DGRAM_RETIRE_QUARANTINED` classifier; design in
  [datagram_step7b9_efi_close_design.md](datagram_step7b9_efi_close_design.md).
- **Enforced by:** the EFI host-mock quarantine tests (`integrations/uefi/mock_efi_test.c`); the
  release invariant in I5 honors `retain_life` uniformly.

### I10 — Protocols depend downward only through the Tier-1 transports

`KlListener`, `KlStream`, and `KlDatagram` are the **canonical Tier-1 semantic transports**. A
protocol-layer TU (HTTP/1.1, HTTP/2, WebSocket, SSE, client, redirect, …) reaches the network *only*
through them plus the socket/event abstractions — the dependency direction is
**`protocol → transport → driver/adapter → engine + provider`, one way**. A protocol TU includes no
platform networking/event system header and no raw completion seam (`completion.h`); it uses a lower
seam only when a missing semantic is documented and reviewed.

- **Why:** a protocol that reached `epoll_ctl`/`WSARecv`/the completion vtable directly would be
  pinned to one engine — the whole point of the three-axis split is that a protocol is written once,
  above the transport, and runs over every backend unchanged.
- **Anchor:** `make check-tier1-boundary` ([Makefile](../Makefile)) — the complement of
  `make check-sockaddr-neutral` (host socket-*address* types, I6). It is **default-deny**: *every*
  `src/*.c` + `parsers/*.c` is governed (no platform networking/event header, no `completion.h`, no
  `io_engine.h`) **except** the allowlisted `TIER1_INFRA` — the engine/provider/bridge layer (event
  backends, socket providers, platform glue, the completion driver/adapters, the transport state
  machines, and the run-loop / async-connect drivers). This is the **mechanical classification rule**:
  a newly added protocol TU is governed automatically, so the whole protocol layer — including the
  pure-byte TUs `router.c`/`cors.c`/`chunked.c`/`body_reader*.c`/`parsers/*.c` — is covered, not just
  the network-facing subset. Include-based, so robust against the `WSA*`/overlapped mentions that
  appear only in explanatory comments (`connection.c`/`response.c`/`client_sync.c`).
- **`TIER1_INFRA` allowlist (with reason):** the bridge layer legitimately includes these headers.
  Notably `server.c` and `client_async.c` sit there because they drive the run loop / async connect
  via the Keel completion **tick** (`io_engine.h`: `kl_comp_run` / `kl_comp_post_connect`) — a Keel
  orchestration seam, not a backend internal. A new infrastructure TU that needs the headers is added
  to `TIER1_INFRA` with a reason; nothing else may include them.
- **Address exceptions (separate, I6):** `udp_server.c` (its datagram handler API surfaces the source
  `struct sockaddr`) and `dns_resolver.c` (the freestanding DNS-over-TCP fallback) carry deliberate,
  documented *address* exceptions — which is why they sit outside the address-neutral set — and are
  governed by the Tier-1 boundary (they include no platform/backend header).
- **Enforced by:** `make check-tier1-boundary` (found zero defects); the protocol-independence
  inventory in [keel_axis_audit.md](keel_axis_audit.md) (Goal 4).

---

## Enforcement summary

| Invariant | Primary mechanical gate |
|---|---|
| I1 model-agnostic transports | `tests/test_datagram_live.c` over both engines |
| I2 honest engine models | capability negotiation + `event_provider_design.md` |
| I3 close ≠ retirement | contract close sections + close-design note |
| I4 sync/reentrant completion | `make smoke-pollcomp-asan`, public mock |
| I5 target lifetime | ASan/UBSan/LSan smokes; R3 audit table |
| I6 transport-mechanical backends | `make check-sockaddr-neutral` + Goal-4 grep |
| I7 no integration types in public API | `make freestanding-headers` |
| I8 allocation-free hot path | fixed-slot design + sanitizer smokes |
| I9 quarantine on uncertain retirement | EFI host-mock quarantine tests |
| I10 protocols depend only through Tier-1 | `make check-tier1-boundary` |

Every markdown link above to an in-repo path is checked by `make check-doc-refs` — a claim that
points at a file which no longer exists fails the gate.
