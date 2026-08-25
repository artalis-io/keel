---
name: axis-audit
description: Audit KEEL's networking architecture; verify the event axis (readiness/completion), the socket/platform axis, and the protocol layer stay orthogonal; produce a findings report and only narrowly-scoped low-risk fixes. Use to assess/harden the event↔socket↔protocol separation.
user-invocable: true
---

# Networking Architecture Axis Audit

Review the KEEL codebase and verify its networking architecture consistently preserves the
intended separation between **(1) the event model, (2) the socket/platform implementation,
(3) the protocol layer.**

**Target:** $ARGUMENTS (default: the whole repo; `src/` incl. `src/protocols/`, `include/keel/`, `tests/`).

**Do not begin with a large refactor.** First inspect the repo, identify the relevant modules
and abstractions, trace representative execution paths, and produce a concrete findings report.
Only make narrowly-scoped fixes where the intended design is clear and the change is low-risk.
**Do not modify code until you can explain the current architecture and trace at least one
readiness path and one completion path end to end.**

Write the report to `docs/archive/audits/keel_axis_audit.md` (prepend a new dated pass if the file exists,
mirroring `docs/archive/audits/keel_audit.md`).

---

## How to run it (operational)

1. **Map first.** Identify the event backends, socket providers, the abstract seams, and the
   protocol modules (see "Repo axis map" below; verify it against current code; it drifts).
2. **Trace before judging.** Trace one readiness path (epoll/kqueue) and one completion path
   (io_uring/pollcomp) for each of: accept, receive, send+backpressure, close-with-outstanding-work.
3. **Exercise both axes.** Readiness runs natively (`make` = epoll/kqueue; `make BACKEND=poll`).
   The completion axis has a **portable test double**; `BACKEND=pollcomp` (a `poll()` facade
   over `completion.h`) runs the completion driver on macOS/Linux under ASan. The production
   completion backend is `BACKEND=iouring` (Linux); run it in an **Apple `container` Linux VM**
   (see the `apple-container-for-linux` memory: kernel 6.18, unrestricted io_uring; Docker
   Desktop's shared VM blocks io_uring). IOCP (`BACKEND=iocp`) is Windows-only.
4. **Use the sanitizers.** The strongest checks for the completion axis:
   `make smoke-pollcomp-asan` (driver under ASan+LSan on POSIX) and, in the container,
   `make smoke-iouring-asan` (the io_uring op/buffer/splice/watcher lifecycle under LSan) +
   `make BACKEND=iouring test-iouring` (the curated completion unit-suite gate). A prior pass
   caught a real watch leak this way that the plain smokes missed. Also `make cppcheck`,
   `make analyze`, `make debug && make test`.
5. **Prove protocol independence mechanically**: grep protocol TUs for platform networking
   headers / event-engine symbols (see Goal 4). A passing grep is a real finding.
6. **Scope fixes tightly.** One coherent change at a time; compile + run the most relevant
   tests after each. Escalate anything needing a public-API change to a finding, not a fix.

---

## Repo axis map (verify against current code)

**Event axis**: `include/keel/event.h` (readiness interface) + `src/event_caps.h` (capability
negotiation, `KL_EVENT_CAP_READINESS | _NATIVE_FD | _COMPLETION`) + `src/completion.h` +
`src/completion_core.c` (the platform-independent completion axis + generic connection driver)
+ `src/completion_dispatch.c` / `src/completion_io.h` (the run-loop completion dispatch seam; the old io_engine has been retired). Backends, Makefile-selected via `BACKEND=`:
- **Readiness:** `event_epoll.c` (Linux default), `event_kqueue.c` (macOS default),
  `event_wsapoll.c` (Windows default), `event_poll.c` (universal fallback).
- **Completion:** `event_iouring.c` (Linux, `BACKEND=iouring`; completion-native SQE/CQE),
  `event_iocp.c` (Windows, `BACKEND=iocp`), `event_pollcomp.c` (portable `poll()` facade,
  `BACKEND=pollcomp`; the CI/ASan test double). Each backend implements `kl_event_caps` +
  `kl_event_native_provider` (auto-wires its overlapped provider) + the `completion.h` post/drain
  contract.

**Socket axis**: `src/socket.h` (the `KlSocketProvider` vtable: `KlSocketOps *ops; void *ctx;`
+ capability flags incl. `KL_SOCK_CAP_OVERLAPPED`) with providers `socket_posix.c` and
`socket_winsock.c`; the overlapped providers (`kl_socket_provider_iouring/iocp/pollcomp`) live in
their event TUs. Native handle is `KlSocketHandle` (`include/keel/handle.h`, pointer-width
`intptr_t`, `KL_INVALID_SOCKET` sentinel, `kl_handle_valid()`). Selected on `KlEventCtx.sockets`
+ public `KlHttpServerConfig.sockets`/`KlHttpClientConfig.sockets`.

**Protocol layer** (all under `src/protocols/http/` unless noted): `http_connection.c` (+
`http_conn_internal.h` model-blind core: `kl_http_conn_dispatch_request`/`kl_http_conn_ingest_body`/
`kl_http_conn_send_complete`), `http2_server.c`, `websocket.c`, the `http_client_*` TUs,
`http2_client.c`, `websocket_client.c`, `http_sse.c`, the `KlTls` vtable (`tls.h`), body readers,
`http_response.c`. These sit above both axes and go through `conn_read`/`conn_write`
(`http_internal.h`) + the socket seam.

The event/socket negotiation is `kl_event_ctx_sockets_compatible()` (`src/event_ctx.c`): a completion loop
requires an overlapped provider; a readiness loop requires a native-fd provider. See
`docs/archive/designs/pal_transformation_design.md` (master roadmap), the `docs/archive/phases/phase8*` design docs,
`docs/archive/phases/phase8f5_iouring_default_migration_design.md`.

---

## Intended architecture

Event handling and socket implementation are **separate, orthogonal axes**; protocols sit
above both and must not touch platform socket APIs or event engines.

```text
Application → Protocols → Abstract connection/socket ops
   ├── Event axis:   Readiness (epoll, kqueue, WSAPoll)  |  Completion (io_uring, IOCP)
   └── Socket axis:  Linux sockets | Darwin sockets | Winsock
```

Valid combinations (where supported): Linux sockets + {epoll, io_uring}; Winsock + {WSAPoll,
IOCP}; Darwin sockets + kqueue. The architecture must **not** assume every socket impl uses
readiness, or that a completion engine owns the socket impl.

---

## Primary audit goals

Assess each; cite files/symbols; distinguish necessary low-level integration from architectural
coupling that leaks upward.

1. **Event and socket axes genuinely independent.** Event engines operate through stable
   socket/handle abstractions, not concrete platform socket internals. Flag: epoll reaching into
   Linux socket internals; IOCP inseparable from the Winsock abstraction; io_uring bypassing the
   common socket lifecycle; platform socket code hard-coding one event engine; event types
   embedded in protocol state; `#ifdef` merging the socket + event abstractions into one backend.
2. **Readiness and completion semantics represented honestly.** Readiness = register interest →
   wait → perform op → handle EAGAIN → re-arm. Completion = construct → submit → track lifetime →
   receive completion → interpret → retire/cancel/resubmit. Readiness must NOT pretend an op
   completed because a descriptor is readable; completion must NOT emit synthetic readiness unless
   explicit + correct.
3. **The public abstraction does not leak one event model.** Flag APIs forcing protocols to think
   in `is_readable`/`is_writable`/epoll flags/kqueue filters/IOCP keys/`OVERLAPPED`/SQEs/CQEs/
   re-arming/native descriptor types. Also flag the opposite: an API claiming model-neutrality
   that can't express completion state, cancellation, partial completion, or op ownership.
   Readiness/completion MAY be explicit event-axis concepts; protocols just need a stable
   Keel-level contract, not platform mechanics.
4. **Protocols remain above both axes.** Protocol modules must: use common socket/stream + event
   abstractions; NOT include platform networking headers; NOT call epoll/kqueue/WSAPoll/IOCP/
   io_uring directly; NOT assume a socket is a Unix int fd; NOT contain Windows completion logic;
   NOT rely on readiness-only control flow unless the abstraction permits. (Mechanical: grep the
   protocol TUs for `<sys/epoll.h>`/`<sys/event.h>`/`io_uring`/`WSA`/`OVERLAPPED`/`epoll_`/`kevent`.)
5. **Socket abstraction platform-neutral where appropriate.** Check the contract + all impls for:
   create/bind/listen/accept/connect/recv/send/shutdown/close/nonblocking/addr-conversion/
   sockopts/error-translation/handle ownership. Flag POSIX assumptions that make Winsock second-
   class: treating `SOCKET` as `int`; negative = universal invalid handle; `close()` vs
   `closesocket()`; `errno` vs `WSAGetLastError()`; POSIX-only error mapping; sockets passed
   through fd-typed APIs; readiness-writeability connect semantics assumed identical. Use the
   existing `KlSocketHandle`; do not add a new handle type without assessing it.
6. **Operation ownership and lifetime correct** (completion makes these dangerous). Audit op
   allocation, buffer ownership, callback/continuation ownership, conn refs held by pending ops,
   close-while-outstanding, cancellation, timeout races, duplicate completion, stale completion
   after handle reuse, shutdown ordering, event-loop destruction, UAF/double-free. IOCP:
   `OVERLAPPED`-associated state. io_uring: submission state / `user_data` / buffers / cancel
   requests (+ the `LIBURING_UDATA_TIMEOUT` and cancel sentinels). Readiness: stale events after
   close, descriptor reuse, registration teardown.
7. **Partial I/O handled consistently** across backends + protocols: partial send/recv, short
   read/write, EAGAIN/EWOULDBLOCK, EINTR, zero-byte read, graceful shutdown, reset, async connect
   completion, accept exhaustion, backpressure. A completion event ≠ whole message transferred; a
   readiness event ≠ the next op consumes/produces all bytes. Send queues + recv state machines
   should behave equivalently across models.
8. **Backpressure model-independent.** Can protocols pause reads? Readiness interest reducible?
   Completion backends avoid unlimited submitted reads? Writes bounded-queued? High/low-water
   marks consistent? Parsers retain partial input safely? Close/cancel releases queued buffers?
   Different mechanisms are fine; Keel-level semantics must be equivalent.
9. **Error handling normalized.** Platform errors → stable Keel categories (would-block, pending,
   interrupted, reset, refused, timed-out, addr-in-use, unreachable, cancelled, peer-closed,
   invalid-state, resource-exhausted) while retaining native detail. Readiness + completion report
   equivalent errors for equivalent failures.
10. **Cancellation + timeout semantics explicit.** Stable semantics for cancelling accept/connect/
    recv/send + connection/idle timeout + loop shutdown. What happens when cancel races successful
    completion? The contract should specify exactly-one terminal result + how late events are
    discarded.
11. **Threading + event-loop affinity clear.** Is each socket/op/source bound to one loop / one
    thread / a worker / transferable? Audit cross-thread submission, wakeups, completion dispatch,
    socket migration, lock ownership, atomic state, callback ordering, close-from-another-thread,
    shutdown races. Both axes should conform to a documented common threading contract.
12. **Registration vs submission not conflated.** A readiness registration = persistent future
    interest (spans many ops); a completion submission = one in-flight op with state + buffers.
    If unified under a higher-level object, verify the internal representation preserves the
    distinction.
13. **Feature detection + backend selection correct.** Capability detection, io_uring
    kernel/runtime availability, IOCP availability, completion→readiness fallback, unsupported
    combos, feature flags, build-system source selection, runtime error messages. Never silently
    select an incompatible/partial combination; the selected config must be observable.
14. **Future provider compatibility (assess, don't build).** Could the abstractions host UEFI SNP
    + polling, lwIP + custom event provider, AF_XDP, DPDK, netmap, virtio-net, an embedded stack,
    a deterministic test transport? Report concrete blockers (mandatory POSIX fds / kernel sockets
    / edge-triggered readiness / async kernel ops; assuming event sources are always sockets;
    recv always copies into app buffers; universal scatter-gather/cancellation). Do NOT
    over-generalize for hypotheticals; recommend only abstractions justified by current code.

---

## Required code tracing (use actual function/type names)

- **Readiness receive:** protocol expects input → registered for read → engine reports readiness
  → recv executes → result normalized → protocol consumes → interest re-armed.
- **Completion receive:** runtime requests input → op + buffer created → submitted → completion
  returned → normalized → consumed → next recv submitted or paused.
- **Accept:** one readiness backend + one completion backend.
- **Send + backpressure:** a send larger than one writable op; how partial progress + backpressure
  work under both models.
- **Close with outstanding work:** receive pending, send queued, timeout armed, an event possibly
  already queued.

---

## Tests to inspect or add

Inspect existing coverage first; add focused, deterministic tests (prefer a mock socket/event
provider, the pollcomp double, or loopback over timing sleeps): equivalent protocol behavior under
readiness vs completion; partial send/recv; readiness→EAGAIN; completion of fewer bytes than
requested; peer graceful shutdown; reset during pending I/O; close with outstanding completions;
cancel racing completion; timeout racing I/O; handle reuse after close; loop shutdown with active
conns; backpressure limits; connect success/failure; accept bursts + exhaustion; backend
selection/fallback; Windows handles that don't fit POSIX assumptions; protocol TUs compiling with
no direct platform networking dependency.

## Security + robustness review

UAF, stale event delivery, native-handle reuse, OOB buffer access, integer truncation in byte
counts, signed/unsigned conversion, double completion, duplicate callbacks, double close, cancel
races, callbacks after owner destruction, unbounded queues, event-storm DoS, connection
starvation, malformed protocol input × partial I/O, platform error values treated as byte counts.
Use sanitizers + static analysis (`make debug`/`smoke-*-asan`/`cppcheck`/`analyze`).

## Architectural smells to flag (concrete instances)

Protocol code importing platform networking headers; event engines parsing protocol data; socket
providers knowing about HTTP; a "generic" abstraction that is actually POSIX-only; a "generic"
event object exposing backend-internal unions upward; duplicated connection state machines per
event model; readiness vs completion producing observably different protocol semantics; hidden
global event-loop state; backend conditionals scattered through protocol code; unsafe casts between
descriptors/pointers/integer IDs; completion ops stored inside protocol parser objects without
clear ownership; platform-specific error checks outside the platform layer.

## Avoid these refactoring mistakes

Do NOT: force readiness + completion into an artificially identical low-level API; rewrite all
engines around one model; emulate IOCP as readiness (or epoll as completion) for superficial
consistency; merge platform socket + event providers; add a C++-style hierarchy; add speculative
abstractions; replace working native mechanisms with a lowest-common-denominator; change public
APIs before documenting the contract + migration impact; do broad formatting/unrelated cleanup.
**The goal is semantic consistency ABOVE the event axis, not identical implementation below it.**

---

## Expected output (report sections)

1. **Current architecture map**: files, modules, types, vtables, factories, build flags; how the
   event axis, socket axis, and protocol layer connect.
2. **Execution-path traces**: the readiness + completion paths above, with real names.
3. **Findings**: per finding: severity (critical/high/medium/low/informational); affected
   files+symbols; the architectural principle; why correct/incorrect; a concrete failure scenario;
   the smallest reasonable fix.
4. **Compatibility matrix**: for `Linux sockets + epoll`, `Linux sockets + io_uring`,
   `Darwin sockets + kqueue`, `Winsock + WSAPoll`, `Winsock + IOCP` (+ pollcomp double): mark
   implemented / buildable / tested / production-ready / incomplete / unsupported-by-design. Do
   NOT infer production-readiness merely because code exists.
5. **Contract definition**: concise proposed internal contract for socket ownership, event-loop
   affinity, readiness notification, completion delivery, operation lifetime, cancellation,
   timeout races, error normalization, close semantics, backpressure. Base it on the existing
   design, not a new architecture.
6. **Recommended incremental roadmap**: immediate correctness fixes / test coverage / small
   architectural cleanup / deferred improvements.
7. **Changes made**: every modified file + why (if any). Keep minimal; compile after each
   coherent group; run the most relevant tests.

## Decision standard

Architecturally sound iff: socket + event providers remain separately replaceable; readiness +
completion preserve their native low-level semantics; higher layers get consistent Keel-level
behavior; protocols contain no platform-specific event/socket logic; op/buffer/connection
lifetimes are safe; cancellation/close/partial-I/O/errors/backpressure are explicitly defined;
supported backend combinations are **tested** rather than assumed.
