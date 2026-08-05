# Phase 10 — UEFI network provider — Feasibility + design (spike-gated go decision)

Status: **feasibility / design (2026-08-05).**

> **IMPLEMENTATION STATUS — the freestanding portability phase is COMPLETE (2026-08-05).**
> The review's prerequisite — *"a client-only, completion-only, errno-free, POSIX-type-free,
> CRT-minimal `libkeel_freestanding.a`"* — is delivered (PRs #199–#208): allocator split (A1),
> `completion.h`→`KlSockAddr` (A2), freestanding public headers + `kl_ssize_t`/off_t + header gate
> (A3/#206), `completion_driver.c` + `client.c` splits behind `KlEventCtx` hooks (B2a/B2b),
> `errno`→`KlIoStatus` across the client family (B1/#205), F-0 archive + symbol gate (#206), F-4
> formatted-I/O/locale elimination (`kl_cstr.*`), F-5 platform hooks + F-7 host mock harness (#207).
> **The acceptance milestone is now FULLY MET:** B1 adds a multi-arch gate (x86_64 **and** aarch64
> PE targets), B2 adds a CRT-less **PE/COFF link** (`make freestanding-link` → `keel_freestanding.efi`),
> plus a `keel/freestanding.h` client-subset umbrella — see the **B1 / B2** sections below.
> Corrections to this doc's original optimistic prose are flagged inline below with **[resolved]**.
>
> **F-8 IS NOW UNDERWAY AND GOING — U-0..U-3 SHIPPED (2026-08-05).** The go/no-go gate returned
> **GO** and the client stack is built + proven on real firmware:
> - **U-0** (#211) — raw EFI_TCP4 `GET / → 200 OK` under QEMU + **stock Ubuntu OVMF** (the predicted
>   "OVMF lacks a TCP4 stack" blocker did not happen). Six lifecycle findings fed U-1..U-3.
> - **U-1** (#213) — `allocator_uefi.c` (AllocatePool/FreePool) + `platform_uefi.c`
>   (EVT_TIMER monotonic clock, EFI_RNG fail-closed) + `u1_link_stubs.c` (incl. a spec-correct asm
>   `__chkstk` for llhttp's >4 KiB frame).
> - **U-2** (#214) — `socket_efi_tcp4.c`: a `KlSocketProvider` over EFI_TCP4 (child/token/**generation**
>   lifecycle; no-errno `EFI_STATUS`→`KlIoStatus`), proven send/recv → 200 in QEMU.
> - **U-3** (#215) — **the culmination**: `event_efi.c` is a completion `KlEventProvider` injected on
>   the STOCK `libkeel_freestanding.a`; an unmodified async `KlClient` does
>   `GET http://<numeric>/ → 200` on bare firmware. Its `KlCompletionOps` (post_connect + drain
>   emitting `KL_COMP_CONNECT`/`KL_COMP_WATCHER` + cancel) is the 1:1 real-firmware analogue of the
>   F-7 mock harness. `KlClient` needed **zero** code changes (model-blind across io_uring/pollcomp/EFI).
>
> Remaining F-8: **U-4** TLS (mbedTLS adapter over the EFI socket-BIO), **U-5** DNS (real `KlResolver`
> over EFI_UDP4/EFI_DNS4, replacing the numeric `resolve_uefi.c`), **U-6** non-blocking token recv,
> **U-7** ExitBootServices lifetime.
This is the roadmap's real **Phase 10** (`docs/pal_transformation_design.md`, phase table) —
UEFI feasibility + an optional prototype. It is *not* the lwIP-raw client work in
`docs/phase10_lwip_raw_client_design.md` (that doc uses a local "Phase 10" label but is the
completion of the roadmap's **Phase 9**; see the doc-numbering note in the roadmap).

Unlike `phase9_lwip_raw_design.md` — which said **GO** because a loopback spike had already
proven the model — this doc says **SPIKE FIRST**: the PAL seams almost certainly *fit* UEFI (the
analysis below shows why), but a firmware/boot-services environment is different enough that a
`GET / → 200` over QEMU+OVMF must exist before any staged implementation begins. The deliverable
of this doc is the feasibility argument + the spike plan that produces the go/no-go.

---

## 1. Goal + why UEFI at all

Run a Keel HTTP client (and, as a stretch, a server) inside the **UEFI boot environment** —
before an OS kernel exists — over firmware-native networking. Concrete motivations:

- **HTTP(S) boot / provisioning agents.** Firmware that fetches a boot image, config, or
  attestation payload over HTTP/HTTPS from a numeric or DNS server address — the same shape as
  the lwIP-raw client (LC-1..LC-4), which was built partly with this target in mind.
- **The last provider shape the PAL refactor was aimed at.** `include/keel/handle.h` already
  names "UEFI protocol pointers" as a rationale for making `KlSocketHandle` `intptr_t`. Phases
  0–9 dissolved the POSIX assumptions (fd-as-int, `errno`, `close()`, readiness-only,
  kernel-socket lifecycle) precisely so a completion-native, socket-less, single-threaded,
  pointer-handle provider could drop in. UEFI is the acid test that the axes are genuinely
  independent.
- **Zero new core.** The success criterion mirrors Phase 9: a UEFI provider should ride a
  a **stock freestanding source profile** — `libkeel_freestanding.a`, a client-only,
  completion-only archive built from the `FREESTANDING_CLIENT_SRC` manifest **[resolved:
  not the same bytes as the POSIX `libkeel.a`; a reduced archive, per the review]** (protocol
  layer + `completion_driver.c` + `completion_dispatch.c`
  unchanged), injected via `KlEventCtx.event_provider` / `KlConfig.event_provider`.

Non-motivation: this is **not** a UEFI DXE network *driver* (we consume firmware's stack, not
implement one), and **not** an OS-runtime concern (boot-services only; see §10).

---

## 2. What UEFI networking actually is

UEFI (UEFI 2.x spec) exposes networking as a stack of **EFI protocols** obtained from the
firmware's handle database via `LocateProtocol` / `OpenProtocol`:

- **`EFI_TCP4_PROTOCOL`** / **`EFI_TCP6_PROTOCOL`** — connection-oriented TCP. The relevant
  surface: `Configure()`, `Connect()`, `Accept()`, `Transmit()`, `Receive()`, `Close()`,
  `Cancel()`, `Poll()`, `GetModeData()`. Each I/O call takes a **completion token**
  (`EFI_TCP4_..._TOKEN`) carrying an `EFI_EVENT` and a `Status`; the call returns immediately
  and the firmware **signals the event** when the op completes.
- **`EFI_UDP4/6_PROTOCOL`** — the datagram analog (for a future DNS/UDP leg).
- **`EFI_DNS4/6_PROTOCOL`** — firmware DNS (present on many but not all implementations).
- **`EFI_MANAGED_NETWORK_PROTOCOL` / `EFI_SIMPLE_NETWORK_PROTOCOL`** — lower layers; we do not
  touch these directly if TCP4 is available.
- **`EFI_BOOT_SERVICES`** — the runtime: `CreateEvent`/`CloseEvent`/`SignalEvent`,
  `WaitForEvent`/`CheckEvent`, `SetTimer` (periodic/relative EFI events for timeouts),
  `AllocatePool`/`FreePool` (the heap), `Stall`, `GetMemoryMap`, etc.

**Execution model:**

- **Single-threaded, cooperative, no preemption at TPL_APPLICATION.** There is exactly one
  thread of control. Progress on network I/O requires *pumping*: call `Poll()` on the protocol
  and/or `CheckEvent`/`WaitForEvent` on the tokens' events. This is precisely KEEL's
  single-loop model — the KEEL event loop **is** the firmware pump, exactly as it **is** the
  lwIP `NO_SYS=1` mainloop (Phase 9).
- **Completion-native.** You construct a token, submit (`Transmit`/`Receive`/`Connect`/
  `Accept`), track it, receive a completion (event signaled + `token->Status`), interpret, and
  retire/cancel/resubmit. This is the **completion axis** semantics verbatim (compare
  `completion.h`'s "construct → submit → track lifetime → receive completion → interpret →
  retire/cancel/resubmit").
- **Freestanding C.** No hosted libc: no `errno`, no `malloc`/`free`, no file descriptors, no
  `sockets`, no `stdio`, no threads, no `time()` (only EFI time/timer services). Code is
  compiled freestanding (`-ffreestanding`, PE/COFF, EFI calling convention) and linked against
  a UEFI application entry (`EFI_IMAGE_ENTRY_POINT` / `efi_main`).

---

## 3. The crux — three environmental deltas, each already anticipated by PAL

| Crux | UEFI reality | Why PAL already fits |
|------|--------------|----------------------|
| **No fd / no sockets** | I/O is via `EFI_TCP4_PROTOCOL*` + tokens, not descriptors. | `KlSocketHandle = intptr_t` (`handle.h`) carries an `EFI_TCP4_PROTOCOL*` (or a heap `KlUefiConn*`) exactly as lwip-raw carries `tcp_pcb*`. `KL_INVALID_SOCKET = (KlSocketHandle)-1`; `kl_handle_valid()` is the only test. **Solved in Phase 5.** |
| **Completion, not readiness** | Token event signaled on completion; no `POLLIN`. | The completion axis (`KlCompletionOps` + `completion_driver.c`) is model-blind; io_uring/IOCP/lwip-raw already inject it via `loop->ops->completion`. `KlEventLoop.fd = -1` for a loop with no pollable OS fd (lwip-raw sets this today). **Solved in Phases 8–9.** |
| **Freestanding (no libc)** | No `errno`/`malloc`/`fd`/`stdio`/threads/`time()`. | `KlAllocator` funnels *all* Keel allocation; the stdlib default was split out (A1) so the freestanding archive takes an **explicit** allocator (no `malloc`/`free` in-archive). **[resolved]** The `errno`-based classification the client actually read is gone — replaced by `KlIoStatus`/`kl_sock_io_status` (B1). **[resolved]** The libc surface was *not* "a small `string.h` subset": it required `snprintf`/`strtol`/`str*`; F-4 replaced those with bounded, locale-free `kl_cstr.*` helpers, shrinking the archive's C-runtime dependency to **`mem*` + `strlen`** (optionally provided in-archive by `kl_cstr_builtin.c` for a bare target). Single-loop, no threads is already the model. |

The residual novelty vs lwip-raw is **only the freestanding toolchain + the EFI event/token
lifecycle**. Everything above the socket seam is unchanged from Phase 9.

---

## 4. Mapping the completion contract onto EFI_TCP4

The whole port is: implement one `KlSocketProvider` (over `EFI_TCP4_PROTOCOL`) + one
`KlEventProvider` whose `->completion` points at a `KlCompletionOps` implemented over EFI
tokens/events. The generic `completion_driver.c` does the rest.

| `KlCompletionOps` primitive | EFI mechanism | Notes |
|-----------------------------|---------------|-------|
| `post_accept(server)` | `EFI_TCP4_PROTOCOL.Accept(&AcceptToken)` | Completion → new child `EFI_TCP4_PROTOCOL*` handle → `KL_COMP_ACCEPT` (target `KlConn*`). Server axis (stretch goal). |
| `post_connect(ctx, fd, addr, watcher_udata)` | `Configure()` (active) + `Connect(&ConnectToken)` | Completion → `KL_COMP_CONNECT` targeting the client's tagged watcher (LC-0 contract, reused verbatim). |
| `post_recv(conn)` | `Receive(&RxToken)` with an `EFI_TCP4_RECEIVE_DATA` fragment table | Completion → bytes in the fragment buffers → `KL_COMP_READ`. Ciphertext for TLS (feed to `feed_input` — the completion-TLS backend contract, see below). |
| `post_send(conn, iov, n, total)` | `Transmit(&TxToken)` with an `EFI_TCP4_TRANSMIT_DATA` fragment table | Fragment table maps directly from `KlIoVec[]` (scatter-gather is native). Completion → `KL_COMP_WRITE`. |
| `post_sendfile(...)` | (optional) `Transmit` chunks from the file | No zero-copy `splice` in UEFI; chunk through a bounded buffer, like the readiness fallback. |
| `post_udp_recv/send(udp)` | `EFI_UDP4_PROTOCOL` Receive/Transmit tokens | Datagram axis → enables KEEL's `dns_resolver.c` over EFI UDP (one DNS path, the LC-3 pattern). Deferred to a later stage. |
| `drain(ctx, out, max, timeout_ms)` | `WaitForEvent`/`CheckEvent` over the pending tokens' events (+ the protocol's `Poll()` to advance the stack) | The pump. Collect every signaled token, translate `token->Status` → Keel category, emit `KlCompletionEvent[]`. `timeout_ms` via an `EFI_TIMER` event added to the `WaitForEvent` set. |
| `cancel(ctx, fd)` | `EFI_TCP4_PROTOCOL.Cancel(token)` (or `Cancel(NULL)` for all on that handle) | Must reconcile the exactly-once terminal-result contract (§5). |

`KlEventProvider`: `caps()` returns `KL_EVENT_CAP_COMPLETION` (no `NATIVE_FD`);
`native_provider()` returns the EFI TCP4 socket provider (auto-wire, like lwip-raw); `->completion`
carries the `KlCompletionOps`. Injected on a stock libkeel via `kl_event_ctx_init_ex`. The
dual-role split (provider TU + optional builtin-glue) from RC-3 applies.

**Address ABI:** `KlSockAddr` ⇄ `EFI_IPv4_ADDRESS` + port in the provider's
`configure`/`connect`/`get_local_addr` (IPv4 first; IPv6/`EFI_TCP6` is a family switch). No
host `sockaddr_in` ever reaches EFI — the `KlSockAddr` neutralization (Phases A–G) already
guarantees this.

**TLS/HTTPS:** reuse the memory-BIO completion-TLS leg exactly as lwip-raw LC-4 did. mbedTLS is
pure crypto (no OS calls) once built freestanding; the backend only moves ciphertext bytes
through `Receive`/`Transmit` tokens. **The backend obligations from LC-4 apply** (feed received
ciphertext to `tls->feed_input`; provide a synchronous send for `comp_tls_flush`'s handshake
records) — a UEFI backend must satisfy the same cross-backend completion-TLS contract from day
one (see `docs/phase10_lwip_raw_client_design.md` §6 + the LC-4 fixes).

---

## 5. Operation lifetime + the EFI event/token lifecycle (the axis-audit risk)

Completion makes lifetime dangerous; EFI adds its own wrinkles the design must pin down:

- **Token ownership.** Each in-flight op owns exactly one `EFI_EVENT` + one token struct
  (Keel-allocated via `KlAllocator`). The token + its fragment buffers must outlive submission
  until the completion is drained — the same "op buffer stays put until `KL_COMP_WRITE`" rule
  lwip-raw follows. Pre-allocate a bounded per-conn token pool (no allocation in the
  submit/drain hot path — AGENTS.md rule).
- **Exactly-once terminal result.** `Cancel` races a natural completion: the spec allows the
  event to be signaled with `EFI_ABORTED` *or* to have already completed. The drain must
  treat "signaled" as the single terminal edge and discard a late/duplicate signal after the
  handle is retired — mirror the io_uring cancel-sentinel discipline and the "stale completion
  after handle reuse" guard.
- **Close ordering.** `EFI_TCP4.Close(&CloseToken)` is itself asynchronous. Teardown =
  `Cancel` outstanding tokens → `Close` (await its token) → `CloseEvent` every event →
  `FreePool` tokens/buffers. Exactly-once, no leak — validated the way lwip-raw's
  close-with-outstanding case is.
- **Backpressure.** No unbounded submitted `Receive`s; bound the in-flight recv/token count per
  conn (the completion-side backpressure rule). `Transmit` queue is bounded-buffered; high/low
  water marks map to the existing `KlDrain`/send-queue semantics — Keel-level behavior stays
  equivalent to the other backends.
- **Timeouts.** An `EFI_TIMER` event in the `WaitForEvent` set gives the loop tick + per-op
  deadlines; `kl_timer` (min-heap) rides on top unchanged.

---

## 6. Freestanding-libc blockers + mitigations (concrete)

| Blocker | Mitigation |
|---------|------------|
| No `malloc`/`free` | `KlAllocator` over `gBS->AllocatePool`/`FreePool`. Already the *only* allocation path in Keel — a UEFI allocator is ~20 lines. |
| No `errno` | Socket seam already normalizes to Keel error categories (`kl_strerror`/`last_error`); the EFI provider maps `EFI_STATUS` → those categories. No `errno` in the protocol layer to begin with. |
| No fd / `close()` vs `closesocket()` | `KlSocketHandle` + provider `close` op. Solved. |
| No `stdio` | Keel core has no `stdio` in the data path; tests/examples that `printf` are host-only. A UEFI app uses `EFI_SIMPLE_TEXT_OUTPUT` for its own logging (test harness only). |
| No `time()` | `kl_monotonic_ms` needs a UEFI backing (an `EFI_TIMER` tick counter or `GetTime`/`SetTime`); this is a tiny platform seam (already abstracted for the timer/deadline paths). Confirm it's cleanly injectable. |
| `string.h`/`memcpy`/`memset` | Freestanding provides these (or the toolchain intrinsics); edk2's `BaseLib` supplies the rest. A thin shim, no core change. |
| Integer/`stdint` | Freestanding `<stdint.h>` is available; C11 `-Wall -Wextra -Wpedantic` still applies. |
| Toolchain | Two options: (a) **edk2** build (`EDK II` package, `MdePkg`/`NetworkPkg`, GNU/clang toolchain, PE/COFF); (b) **gnu-efi** (lighter, host GCC + `-ffreestanding` + `elf_x86_64_efi.lds`). Pick in the spike (§7). |

None is a *core* blocker — every one is either already solved by an existing seam or a small
platform shim confined to the UEFI TU (the neutral-seam discipline: EFI headers stay behind the
UEFI provider TU, exactly as lwIP headers stay behind `lwip_raw_glue.c`).

---

## 7. Testability gate — the spike that produces the go/no-go

This doc does **not** claim GO. It claims: build the smallest thing that proves the model, then
decide. The spike (host-runnable, no hardware):

1. **QEMU + OVMF.** Boot `qemu-system-x86_64` with the OVMF (edk2) firmware + a virtio-net or
   e1000 NIC, user-mode networking (`-netdev user`), and a UEFI shell. QEMU's SLIRP provides a
   gateway + can forward to a host HTTP server. This is the UEFI analog of "Apple container for
   Linux" — a deterministic, in-CI-capable environment.
2. **Minimal EFI app** (`efi_main`): `LocateProtocol(EFI_TCP4_SERVICE_BINDING)` →
   `CreateChild` → `Configure` (active, DHCP or static) → `Connect` token → `Transmit` a raw
   `GET / HTTP/1.1` → `Receive` tokens → print status line. **No Keel yet** — this proves the
   EFI_TCP4 token/event/pump model end-to-end and shapes the `KlCompletionOps` mapping (exactly
   as the lwip-raw `raw-spike` preceded P9-1).
3. **Then** a `KlEventProvider`+`KlSocketProvider` over EFI_TCP4, injected on a freestanding
   build of stock libkeel, doing `KlClient GET → 200` in the same app. If this runs
   ASan-equivalent-clean (UEFI has no ASan; use edk2's pool-guard / `MemoryProtection` +
   careful review) → **GO** for the staged plan.
4. **Fallback / parallel: a host mock.** A deterministic `EFI_TCP4_PROTOCOL` **test double** (a
   vtable of function pointers over a loopback socket or an in-memory pipe) lets the completion
   mapping + lifetime be unit-tested on the host under real ASan/UBSan/LSan — the role
   `BACKEND=pollcomp` plays for the completion axis. This de-risks the lifetime/cancel/close
   logic *before* fighting the firmware toolchain, and keeps most of Phase 10 CI-gated on the
   host.

**Open question for review:** is the authoritative gate **OVMF-in-CI** (real firmware, slower,
flakier) or the **host EFI_TCP4 mock** (fast, ASan-clean, but not real firmware) with OVMF as a
manual/local confidence run? Recommendation: **mock is the CI gate; OVMF is the local/hull
confidence run** — same posture as BYO-mbedTLS TLS (LC-4) and the pollcomp-vs-io_uring split.

---

## 8. Staged plan (spike-gated; each stage independently tested)

Mirrors P9-1..P9-5 / LC-0..LC-5 discipline. **U-0 is a hard gate** — no U-1 without a GO.

- **U-0 — feasibility spike + go/no-go.** The §7 QEMU+OVMF raw-TCP4 app *and* the host
  EFI_TCP4 mock skeleton. Deliverable: `GET / → 200` from a raw EFI app + a decision.
- **U-1 — freestanding build + platform shims.** Compile stock libkeel freestanding (allocator
  over `AllocatePool`, `kl_monotonic_ms` over EFI time, string/stdint shims). Gate: libkeel
  links in the UEFI target; a trivial `KlAllocator` self-test runs in the app.
- **U-2 — EFI_TCP4 socket provider.** `KlSocketProvider` over EFI_TCP4 (socket/connect/send/
  recv/close/get_local_addr; `KlSockAddr` ⇄ `EFI_IPv4_ADDRESS`; `EFI_STATUS` → Keel errors).
  Tested against the **host mock** under ASan first, then OVMF.
- **U-3 — EFI completion backend (client).** `KlCompletionOps` over tokens/events +
  `KlEventProvider` (`caps=COMPLETION`, `native_provider`, `->completion`); `drain` over
  `WaitForEvent`/`CheckEvent`; `post_connect`/`post_recv`/`post_send`; lifetime + cancel +
  close-with-outstanding. Gate: `KlClient GET http://<addr>/ → 200` on stock libkeel (mock +
  OVMF). This is the U-analog of LC-1.
- **U-4 — HTTPS (client).** mbedTLS built freestanding; reuse the memory-BIO completion-TLS leg;
  satisfy the backend completion-TLS contract (feed ciphertext + sync handshake send). Gate:
  `GET https://<addr>/ → 200`. Out of standard CI (BYO mbedTLS), like LC-4.
- **U-5 — DNS + UDP (optional).** `EFI_UDP4` datagram ops → KEEL's `dns_resolver.c` over it
  (one DNS path, the LC-3 pattern), or `EFI_DNS4` where present. Gate: resolve + `GET → 200`.
- **U-6 — server (stretch, optional).** `Accept` tokens → `KL_COMP_ACCEPT`; a raw `KlServer`
  serves `GET / → 200` under OVMF. Only if a boot-time server has a real use case.
- **U-7 — caps + docs.** Capability matrix (what's Supported/Unsupported-by-design/
  Not-demonstrated), a `keel_uefi.h` API-facing statement, a caps test, README, and flip the
  roadmap Phase-10 row → done. Mirrors LC-5.

Client-first (U-2..U-4), server deferred — HTTP-boot is a client. IPv4 first; IPv6 is a family
switch (`EFI_TCP6`).

---

## 9. Compatibility matrix (target — to be filled by the spike)

| Combination | Status |
|-------------|--------|
| EFI_TCP4 + EFI completion loop (client, plaintext) | **PROVEN — `KlClient GET → 200` in QEMU/OVMF (U-3, #215)** |
| EFI_TCP4 + EFI completion loop (client, HTTPS) | *design; U-4* |
| EFI_UDP4 + EFI completion loop (DNS) | *design; U-5 (numeric `resolve_uefi.c` ships pre-DNS)* |
| EFI_TCP4 + EFI completion loop (server) | *stretch; deferred (client-first)* |
| Host EFI_TCP4 **mock** + completion driver (ASan gate) | **PROVEN — F-7 harness 57/57 (mock socket + completion provider)** |
| IPv6 (`EFI_TCP6`) | *out of first cut (family switch)* |

Nothing is marked production-ready by existence — only by a passing gate, per the axis-audit
decision standard.

---

## 10. Non-goals + scope

- **Not** a UEFI DXE/network **driver** — we consume firmware's EFI_TCP4/UDP4, not implement a
  stack (contrast the lwIP integration, where lwIP *is* the stack).
- **Boot-services only.** After `ExitBootServices()` the EFI networking protocols + boot
  services are gone; a runtime-services HTTP client is out of scope.
- **No preemption / no threads** — the single-loop model is mandatory (it already is Keel's
  model; `KlThreadPool` is unavailable in UEFI and simply not used).
- **Subset first** — buffered HTTP/1.1, IPv4, client. h2/WebSocket/ALPN-h2, TLS file/stream
  bodies inherit the LC-4 subset caveats.
- **Additive/evolutionary public API changes** — **[resolved: the original "no public API change"
  was wrong]** the freestanding work made small, source-compatible public changes: `ssize_t`→
  `kl_ssize_t` (pointer-width identical), `off_t`→`uint64_t` on the file-response API, and the
  additive `KlIoStatus` enum + optional `io_status` op. Still additive `event_provider`/`sockets`
  injection for the provider itself, as every
  prior PAL phase.

---

## 11. Recommendation

**SPIKE (U-0), then decide.** The architectural analysis is strongly favorable — every UEFI
delta (no fd, completion-native, freestanding) was already anticipated and dissolved by Phases
5/8/9, and the lwip-raw client (a NO_SYS, completion, pointer-handle, in-process provider) is a
near-exact precedent that shipped with **zero** `src/` changes for the transport. The
irreducible unknowns are (a) the freestanding toolchain + mbedTLS-freestanding build, and (b)
the EFI token/event/cancel lifetime under real firmware. U-0 (QEMU+OVMF raw app + host mock)
retires both cheaply and produces the go/no-go without committing to the full port.

If U-0 is GO, the staged plan (U-1..U-7) is small, host-CI-gated on the EFI_TCP4 mock, with
OVMF as the local/hull confidence run — the same discipline that carried Phases 8 and 9.

---

---

## F0 — freestanding archive: shipped

The freestanding phase's first acceptance milestone — a **freestanding client archive with a
documented undefined-symbol whitelist** — is now in the tree (ahead of the U-0 spike; it is the
host-side, no-firmware groundwork the spike builds on).

- **`off_t` neutralized.** `KlResponse.file_size`/`file_offset`, `kl_response_file(...)`, and the
  `KlFileIO` submit offset moved from the hosted `off_t` to `uint64_t` (non-negative domain, same
  neutral type as the internal sendfile seam). `response.h` + `file_io.h` dropped `<sys/types.h>`
  and joined the `make freestanding-headers` gate (zero POSIX deps).
- **`make freestanding-lib`** builds `libkeel_freestanding.a` from a client-only, completion-only
  manifest (`FREESTANDING_CLIENT_SRC`) with `clang -ffreestanding -fshort-wchar -mno-red-zone
  -fno-stack-protector -fno-builtin -DKEEL_FREESTANDING` and asserts, via
  `tests/freestanding_symbol_gate.sh` over `nm`, that every undefined symbol is whitelisted.
- **Two decoupling gaps found + fixed to keep the manifest server/udp/dns-free:**
  1. `async.c` bundled the client-usable `KlEventCtx`/`KlWatcher` API with the server-side
     `kl_async_suspend`/`complete`/`cancel` machinery (which pulls `kl_conn_on_writable` /
     `kl_server_conn_release` / `kl_io_engine_resume_completion`). Split the client half into
     `event_ctx.c`; the server-suspend half stays in `async.c` and is excluded.
  2. `client_async.c` unconditionally auto-created the built-in DNS-over-UDP resolver
     (`kl_dns_resolver_create`), dragging in the DNS + UDP stack. Gated out under
     `KEEL_FREESTANDING` (a freestanding client resolves via `cfg->resolver` or a numeric
     address — the §8 IPv4/numeric-first shape; DNS is U-5).

**The enforced whitelist** (the archive's full undefined-symbol closure):

| Class | Symbols |
|-------|---------|
| C-runtime memory/string | `memcpy` `memmove` `memset` `memcmp` `strlen` `strcmp` `strncmp` `strcasecmp` `strchr` `strstr` `strtol` `snprintf` (+ `__*_chk` FORTIFY wrappers) |
| C-runtime alloc + diagnostic | `malloc` `free` `realloc` `fprintf` `abort` `stderr`/`__stderrp` |
| KEEL platform + resolution hooks | `kl_monotonic_ms` `kl_resolve_sync` (`kl_plat_*` reserved) |
| Socket provider ops (vtable) | `kl_sockdef_socket/connect/recv/recv_peek/send/close/get_so_error/io_status/set_nonblocking/set_nosigpipe` |
| Event/completion backend hooks | `kl_event_{init,add,mod,del,wait,close,caps,native_provider}_builtin` `kl_comp_ops_builtin` |

The socket/event/completion entries are the **provider injection points** — a freestanding build
supplies its own (EFI_TCP4 socket provider + EFI-token completion backend), exactly the U-2/U-3
seams §4 maps. `kl_monotonic_ms`/`kl_resolve_sync` are the tiny platform seams §6 flags. No
server, thread_pool, `kl_udp_*`, `kl_dns_*`, file_io, or OS-syscall/errno symbol appears — the
gate FAILS loud if one ever does. Toolchain used: **clang** (falls back to `cc` where clang is
absent).

---

## F-5 — platform hooks: the freestanding embedder contract

The F-0 archive names the symbols it leaves undefined; **F-5** pins down what an embedder must
actually *implement* to satisfy that closure, and provides a **host reference implementation** for
the F-7 harness (`tests/freestanding_host_platform.c`).

**The minimal platform seam (plaintext HTTP/1.1 client):**

| Hook | Purpose | Freestanding note |
|------|---------|-------------------|
| `uint64_t kl_monotonic_ms(void)` | Monotonic millisecond clock. Backs `kl_timer` deadlines, the Happy-Eyeballs attempt delay, and the request deadline. | UEFI: an `EFI_TIMER` tick counter / `GetTime`. The harness uses a **mock, advanceable** clock (`fs_clock_set`/`fs_clock_advance`) so timeouts are deterministic with no sleeps. |
| `void kl_plat_random(void *, size_t)` | Randomness (DNS query-id / cookie / TLS entropy). | **Reserved** — the plaintext client archive does not pull it (no DNS/TLS in the manifest). A REAL freestanding build MUST fail-closed or use `EFI_RNG_PROTOCOL`; the harness's deterministic PRNG is **test-only** and cryptographically worthless. |
| `int kl_resolve_sync(...)` | Blocking name→`KlSockAddr`. | See below — a numeric-only reference, never blocking DNS. |
| a `KlSocketProvider` | socket/connect/send/recv/close/get_so_error/io_status/set_nonblocking/set_nosigpipe, advertising `KL_SOCK_CAP_OVERLAPPED` for a completion loop. | U-2 (EFI_TCP4). Fills the `kl_sockdef_*` fallbacks so the built-in defaults are never reached. |
| a `KlEventProvider` + `KlCompletionOps` | `drain` + `post_connect` + `cancel` (+ `add`/`mod`/`del`/`caps`/`native_provider`). | U-3 (EFI tokens/events). Injected via `kl_event_ctx_init_ex`, so the `kl_event_*_builtin`/`kl_comp_ops_builtin` fallbacks are never reached. |

Nothing more for a plaintext client. TLS/ws add a `KlTls` backend + its entropy (`kl_plat_random`
/ EFI_RNG); DNS (U-5) adds a `KlResolver` or a real `kl_resolve_sync` over EFI_UDP4/EFI_DNS4.

**`kl_resolve_sync`: MOCKED, not trimmed (finding).** The archive genuinely *references*
`kl_resolve_sync` — `client_async.c`'s sync-DNS *fallback* (reached when `client_pick_resolver`
returns NULL, which under `KEEL_FREESTANDING` it always does) still calls it, and numeric address
literals also flow through that fallback. Trimming would mean `#ifdef`-ing out both call sites
*and* their surrounding blocks (a real `src/` change) and would leave a freestanding client unable
to dial even a numeric literal without a resolver. The design already whitelists it as "the tiny
platform seam", so the correct move is to **provide a numeric-only reference** (dotted-quad IPv4
parse; any non-numeric host fails — a freestanding client with no resolver *cannot* resolve names).
That is exactly what a minimal embedder ships pre-DNS (U-5). The F-7 harness itself drives the
client via `cfg->resolver`, so it never *executes* `kl_resolve_sync` — but the symbol stays a link
dependency, hence the mock. **No `src/` change was required for F-5/F-7.**

## F-7 — host mock harness: the client RUNS on injected hooks + mocks

`tests/freestanding_harness.c` (+ `make freestanding-harness`) is the acceptance milestone: the
F-0 client archive's async state machine **runs an HTTP/1.1 GET end-to-end** on the host over a
**mock socket provider** + **mock completion event provider** + **mock resolver** + the F-5 host
platform hooks — **no real syscalls, no `errno`, no loopback socket** — under ASan+UBSan (+LSan on
Linux) with a per-scenario counting-allocator leak check.

**The emulated-readiness shape (the reusable model, and a UEFI-readiness finding).** A pure mock
*is* sufficient — but only because the client's completion path is **connect-over-completion,
send/recv-over-synchronous-ops**, identical to the shipped pollcomp/lwip-raw backends: the client
posts `kl_comp_post_connect` (→ `KL_COMP_CONNECT`), then for I/O it arms `KL_EVENT_WRITE`/`READ`
watchers and calls `kl_sock_send`/`recv`, re-arming on `KL_IO_WOULD_BLOCK`; the backend's `drain`
relays each armed watch as a `KL_COMP_WATCHER`. So a completion backend (EFI_TCP4 included) that
wants to drive KEEL's client needs **only** `post_connect` + `cancel` + a `drain` that emits
`KL_COMP_CONNECT` and relays armed watches — it does **not** need `post_recv`/`post_send` for the
client (those are the *server* driver's path). This is the concrete U-3 client contract, now
exercised on the host.

**Toolchain choice:** the harness compiles the `FREESTANDING_CLIENT_SRC` manifest with the **host
ASan/UBSan toolchain** + `-DKEEL_FREESTANDING` (rather than linking the clang-freestanding
*archive*, whose objects are not instrumented), so ASan/UBSan cover the **library** code — a
use-after-free inside the client's completion state machine is the whole point of F-7. The
archive's link closure is still enforced independently by `make freestanding-lib`.

**Scenarios (all deterministic; each asserts result AND zero leak):** 1 happy path (200 + body),
2 connect refused, 3 partial send + `WOULD_BLOCK` re-arm, 4 fragmented recv (3 chunks +
`WOULD_BLOCK`), 5 peer close mid-response, 6 timeout (advance mock clock past the deadline),
7 cancel mid-flight (no UAF), 8 response-size cap exceeded, 9 allocation failure (sweep early
malloc indices), 10 completion-after-cancel / stale completion (exercises the `kl_event_dispatch`
watcher-liveness guard). Result: **57/57 checks PASS**, ASan/UBSan clean, zero leaks. **No real
freestanding defect was exposed** — the archive's async client runs correctly over fully mocked,
non-hosted hooks, which retires the host-side risk before the U-0 QEMU/OVMF spike.

## F-4 — drop formatted-I/O + locale: the archive's C-runtime surface is now mem* + strlen

F-0 shipped the archive but whitelisted a broad C-runtime surface (`snprintf`, `strtol`,
`strcasecmp`, `strncmp`, `strstr`, `strchr` + the stdlib default allocator's `malloc`/`free`/
`realloc` + `fprintf`/`abort`). **F-4 removes that surface**: the formatted-I/O + locale calls in
the shared client/URL/sockaddr TUs are replaced by bounded, locale-free internal helpers, and the
stdlib default allocator is dropped from the manifest (a freestanding client always takes an
**explicit** `KlAllocator *` — verified: no freestanding TU calls `kl_allocator_default()`).

**The helpers** (`src/kl_cstr.{h,c}`, in the manifest) — every one ASCII-only, bounds-checked,
overflow-guarded, and behavior-identical to the libc call it displaces (the hosted url/sockaddr/
client/pool suites + the url/dns fuzzers are the guardrail, since these TUs are shared with the
hosted server + sync client):

| Helper | Replaces | Notes |
|--------|----------|-------|
| `kl_ascii_strcasecmp` / `kl_ascii_strncasecmp` | `strcasecmp` / `strncasecmp` | ASCII case-fold, no locale; header-name matching |
| `kl_parse_u16_decimal` | `strtol` (URL port) | bounded decimal → uint16, rejects overflow/non-digits/empty; port 0 still rejected |
| `kl_u64_to_dec` / `kl_u64_to_hex` + `kl_buf_append{,_n,_u64,_hex}` | `snprintf` request/CONNECT/chunk-header/abs-URL construction | offset-tracking bounded builders |
| `kl_fmt` path in `sockaddr.c` | `snprintf("%u.%u.%u.%u"/"%x"/"%.*s"/"%s:%u"/"[%s]:%u")` | **byte-identical** dotted-quad / RFC 5952 IPv6 / UNIX / port output |
| `kl_streq` / `kl_str_startswith` | `strcmp` (pool host key) / `strncmp` (URL scheme) | case-**sensitive** exact/prefix — preserves existing acceptance |
| `kl_strstr` / `kl_strchr` | `strstr` / `strchr` | bounded finds |

Fixed-length, both-non-NUL compares (the proxy `HTTP/1.` status check) became `memcmp`. The
`FORTIFY __*_chk` wrappers (`memcpy`/`memmove`/`memset`) stay — they resolve to the same mem*.

**The shrunk whitelist** (the archive's full undefined-symbol closure now):

| Class | Symbols |
|-------|---------|
| C-runtime memory/string (the minimal freestanding surface) | `memcpy` `memmove` `memset` `memcmp` `strlen` (+ `__*_chk` FORTIFY wrappers) |
| **Vendored-llhttp residual** (documented, not KEEL code) | `abort` (unreachable switch-defaults in `vendor/llhttp/api.c`) + `fprintf`/`stderr` (llhttp's `pretty_print` debug dumper, never called on the client path) |
| KEEL platform + resolution hooks | `kl_monotonic_ms` `kl_resolve_sync` |
| Socket provider ops (vtable) | `kl_sockdef_*` |
| Event/completion backend hooks | `kl_event_*_builtin` `kl_comp_ops_builtin` |

The gate (`tests/freestanding_symbol_gate.sh`) now **FAILS** if `snprintf`/`strtol`/`strcasecmp`/
`strncmp`/`strcmp`/`strstr`/`strchr` or `malloc`/`free`/`realloc` ever reappear. The only residual
beyond mem*/strlen + KEEL hooks is **vendored llhttp's own** `abort`/`fprintf`/`stderr` (we do not
modify vendored code, per AGENTS.md); a real UEFI build stubs these (`abort` → `CpuDeadLoop`, no
stdio). `make freestanding-harness` still passes **57/57** over the mocks with the always-explicit
allocator; hosted `make test` is byte-identical (no behavioral change).

---

## B1 — multi-arch gate: x86_64 AND aarch64 PE targets (F-6 "test AArch64 early")

`make freestanding-lib` (and `freestanding-lib-selfcontained`) now cross-compile + symbol-gate the
manifest for **both** `x86_64-unknown-windows` **and** `aarch64-unknown-windows` — UEFI ships on
ARM64 too, and the compiler-runtime helper closure differs by arch. Each triple is built with
`clang --target=<triple> -ffreestanding -fshort-wchar -fno-stack-protector -fno-builtin
-DKEEL_FREESTANDING` (`-mno-red-zone` **only** on x86_64 — it is the x86 UEFI interrupt-safety flag;
the aarch64 pass drops it), archived to `libkeel_freestanding_<arch>.a`, and run through the gate.
The canonical `libkeel_freestanding.a` is the x86_64 copy. A triple whose clang backend/headers are
unavailable is **SKIPPED** with a printed note (never a build failure); the recipe reports which
triples ran. With the `cc`/gcc fallback (no cross `--target`), the list collapses to a single native
pass — the pre-B1 behavior. `FREESTANDING_TARGETS` is override-able.

**Cross-target shim (`tests/freestanding/shim/`, `-isystem`).** Clang's `-ffreestanding` provides
`stddef.h`/`stdint.h`/`stdarg.h`/`limits.h`/`stdbool.h`/`stdatomic.h` but **not** `string.h`/
`stdlib.h`/`stdio.h`/`sys/types.h`/`errno.h`, and a `*-unknown-windows` triple defines `_WIN32`, so
the internal `src/sockcompat.h` `_WIN32` branch also reaches for `winsock2.h`/`ws2tcpip.h`. The shim
supplies **declaration-only** stand-ins for exactly those (the freestanding client passes addresses
as the neutral `KlSockAddr` and never touches a native `struct sockaddr`, so the Winsock shims only
need the `AF_*`/`SOCK_*` constants `client_async.c` maps to). A real UEFI/EDK2 build supplies the
equivalents (BaseMemoryLib, the EFI socket protocols). The shim carries no logic, so the archive's
undefined closure is unchanged. This is Makefile+shim only — **no `src/` change**; the native host
build (and the F-7 harness) still use the host libc headers unchanged.

**The aarch64 vs x86_64 closure — identical except one PE compiler-runtime helper.** Both arches'
undefined closures are byte-for-byte identical (mem*/strlen + the KEEL seams + the llhttp residual)
**plus `__chkstk`** — the Windows-ABI stack-probe helper the PE target emits for functions with
large stack frames. It appears on **both** arches (it is a PE-ABI thing, not arch-specific) and is
part of the PE **compiler runtime** (supplied by the CRT / EDK2 on a real target), **not** KEEL code
and **not** a hosted-CRT dependency; it never appears on the host ELF/Mach-O archive. It is added to
the whitelist as the documented PE compiler-runtime residual. **Notably absent:** no `__aarch64_*`
outline atomics and no integer-division helpers — so the multi-arch surface is exactly the same seam
on both. Any *other* `__*` compiler-runtime symbol is **not** whitelisted (a real finding).

## B2 — CRT-less PE/COFF link: the milestone's last literal

`make freestanding-link` **links** `libkeel_freestanding_selfcontained.a` (mem-family + strlen
in-archive) into a **PE/COFF EFI image with NO hosted CRT** — the last literal of the acceptance
milestone. `tests/freestanding_link_main.c` is a minimal `efi_main` that references the client
public API (pulling `client_async`/`client_common` + their transitive archive objects) and
**defines** every seam the archive leaves undefined — the platform hooks (`kl_monotonic_ms`/
`kl_plat_random`/`kl_resolve_sync`), the `kl_sockdef_*` fallbacks, the `kl_event_*_builtin`/
`kl_comp_ops_builtin` hooks, the llhttp `abort`/`fprintf`/`stderr` residual, and `__chkstk` — as
minimal fail-closed stubs. It is freestanding (`-DKEEL_FREESTANDING`, no libc includes, the shim
`-isystem`) and does **not run** — it must **link** with an empty undefined set under `-nostdlib`.

The link is:

```
clang --target=x86_64-unknown-windows -ffreestanding -fno-stack-protector -fno-builtin \
      -nostdlib -fuse-ld=lld -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
      -o keel_freestanding.efi <main.o> libkeel_freestanding_selfcontained_x86_64.a
```

Result: a `PE32+ executable (EFI application)` — `Magic 0x20B`, `Subsystem
IMAGE_SUBSYSTEM_EFI_APPLICATION (0xA)`, `Machine IMAGE_FILE_MACHINE_AMD64` — linked with **no hosted
CRT** (`-nostdlib`, empty undefined set). The **aarch64** variant links the same way
(`--target=aarch64-unknown-windows`, `Machine IMAGE_FILE_MACHINE_ARM64`, `Subsystem EFI_APPLICATION`)
→ `keel_freestanding_aarch64.efi`. The recipe prints the `llvm-readobj --file-headers` proof for
each. If lld's PE backend is unavailable in the toolchain, the recipe emits the strongest achievable
proof instead (COFF entry object for each arch + the empty self-contained undefined set from the
gate) and **documents the limitation** — it does not fake a PE image. `freestanding-link` requires
clang; the `cc` fallback SKIPs it.

## `keel/freestanding.h` — the freestanding client-subset umbrella

`include/keel/freestanding.h` is a small umbrella that `#include`s **exactly** the client + protocol
public headers proven freestanding-clean by the header gate (the same 19-header set as
`tests/freestanding_headers.c`), plus the linked-library version macros/accessors. A freestanding
consumer gets **one** entry point instead of the full `<keel/keel.h>` (which drags in the server /
UDP / DNS / thread-pool / native-socket surfaces a freestanding build excludes, and via `net.h` the
platform socket headers). `make freestanding-headers` now compiles the umbrella too and dep-proves it
pulls **zero** POSIX/system headers.

## Acceptance milestone — FULLY MET

The freestanding phase's first acceptance milestone — *"links as PE/COFF without a hosted CRT,
documented undefined-symbol whitelist, passes host sanitizer tests, performs an HTTP/1.1 GET over a
mock completion provider"* — is now met in full:

| Clause | Where |
|--------|-------|
| documented undefined-symbol whitelist | `tests/freestanding_symbol_gate.sh` (F-0/F-4) |
| passes host sanitizer tests | `make freestanding-harness` — 57/57 under ASan+UBSan+LSan (F-7) |
| performs an HTTP/1.1 GET over a mock completion provider | `tests/freestanding_harness.c` scenario 1 (F-7) |
| **links as PE/COFF without a hosted CRT** | `make freestanding-link` → `keel_freestanding.efi` (**B2**) |
| **multi-arch** (x86_64 + aarch64) | `make freestanding-lib` gates both; `freestanding-link` links both (**B1**) |

Note on the manifest: `src/decompress.c` is **kept** in `FREESTANDING_CLIENT_SRC` — the freestanding
client genuinely references it (`kl_client_decompress_response_body` + the streaming
`kl_decompress_stream_*` wrapper in `client_common.c`/`client_async.c`). It is **vtable dispatch
only** (no miniz/zlib backend in the archive); an embedder that wants actual gzip/deflate injects a
`KlDecompress` backend via `KlClientConfig.decompress`, exactly like the socket/event providers.

---

*Feasibility + design only for §§1–11. No firmware code/build/commit in this document. Doc path:
`docs/phase10_uefi_feasibility_design.md`. This is the roadmap's Phase 10; the lwIP-raw client
work lives in `docs/phase10_lwip_raw_client_design.md` (roadmap Phase 9). Cross-referenced from
`docs/pal_transformation_design.md`.*
