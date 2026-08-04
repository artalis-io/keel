# Phase 10 — UEFI network provider — Feasibility + design (spike-gated go decision)

Status: **feasibility / design (2026-08-05). No code, no build, no commit in this document.**
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
  **stock `libkeel.a`** (protocol layer + `completion_driver.c` + `completion_dispatch.c`
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
| **Freestanding (no libc)** | No `errno`/`malloc`/`fd`/`stdio`/threads/`time()`. | `KlAllocator` already funnels *all* Keel allocation (→ `AllocatePool`/`FreePool`). Error handling is normalized at the socket seam to Keel categories (not raw `errno`). Single-loop, no threads is already the model. Remaining libc gaps are a small `string.h`/`stdint` subset — a freestanding shim, not a core change. |

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
| EFI_TCP4 + EFI completion loop (client, plaintext) | *design; proven by U-0/U-3* |
| EFI_TCP4 + EFI completion loop (client, HTTPS) | *design; U-4* |
| EFI_UDP4 + EFI completion loop (DNS) | *design; U-5* |
| EFI_TCP4 + EFI completion loop (server) | *stretch; U-6* |
| Host EFI_TCP4 **mock** + completion driver (ASan gate) | *design; U-0/U-2* |
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
- **No public API change** expected — additive `event_provider`/`sockets` injection, as every
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

*Feasibility + design only. No code, no build, no commit. Doc path:
`docs/phase10_uefi_feasibility_design.md`. This is the roadmap's Phase 10; the lwIP-raw client
work lives in `docs/phase10_lwip_raw_client_design.md` (roadmap Phase 9). Cross-referenced from
`docs/pal_transformation_design.md`.*
