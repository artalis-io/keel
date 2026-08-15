# Datagram Step 7B — public `KlDatagram` surface: design-freeze breakdown

**Status: DESIGN FREEZE (docs-only). No 7B implementation begins until this is reviewed and frozen.**

7A (7A-1…7A-5) built and unit-tested the internal `KlDgramCore` assembly + its machines
(slots/send/recv/close/life) and converted the lwIP-raw receive path to the one-held contract. `src/`
protocol code and `KlUdp` behaviour are unchanged; every backend still rides its existing transport.

7B turns `KlDgramCore` into the **public `KlDatagram` API** and wires it onto the live backend seams.
Per the review cadence this document freezes, **before any code**:

1. the **namespace resolution** (the public API name `KlDatagram` is currently taken by an internal
   object — this must be settled first);
2. the **public ABI** (types, functions, header split, STABLE banner);
3. the **ownership + lifetime contract**;

and then splits the work — especially the **live backend wiring** — into small, individually-reviewable
increments, each with its own validation.

This doc is the 7B counterpart of `docs/datagram_step7_public_api_design.md` (which froze the 8 API
topics + the 7A breakdown). It does not restate those; it references them (§1 surface, §4 close, §5
storage asymmetry, §6 D-COMPAT) and pins the remaining boundaries.

---

## 0. The blocker: the `KlDatagram` name is already taken

The public fixed-slot API wants the name `KlDatagram` and the header `<keel/datagram.h>`. Both are
**currently in use for different things**:

| Symbol / header (today) | What it is today | Role |
|---|---|---|
| `struct KlDatagram` (`keel/datagram_detail.h`) | the **legacy KlUdp-embedded transport** — byte-budget `q_head/q_tail` send FIFO, `recv_buf`, mmsg batch blocks, interest, `rx_life` | internal object `KlUdp` wraps (Phase-A carve) |
| `KlDatagramOps` (`keel/datagram.h`) | the **provider datagram data-plane vtable** — `send/recv/send_gso/configure/set_tos/mcast/rx_batch/tx_batch/…` | the `KlSocketProvider.dgram` seam |
| `KlDgramRxMeta` / `KlDgramRxSlot` / `KlDgramTxDesc`, `KL_DGRAM_RX_*` (`keel/datagram.h`) | provider recv/batch descriptor types | provider seam |
| `KlDatagramMessage` / `KlDatagramSendStatus` / `KlDatagramCloseResult` (`src/datagram_*.h`) | fixed-slot **message + status + terminal-result** types, already named "for the eventual public API" | the future public surface (internal today) |

So there are **three** distinct namespaces tangled under "datagram": the legacy transport OBJECT, the
provider data-plane VTABLE, and the future public fixed-slot API. 7B-0 must separate them. The frozen
end-state proposed here (open for review — see §7):

- **Legacy transport object** `struct KlDatagram` → rename **`KlUdpTransport`**; its layout header
  `keel/datagram_detail.h` → **`keel/udp_transport_detail.h`**. (Reviewer-endorsed direction in the
  7A-3 scope note: *"today's legacy KlUdp-embedded transport is renamed internally, e.g. KlUdpTransport."*)
  `KlUdp` keeps embedding it, unchanged (D-COMPAT §6 — byte-budget, NOT re-based).
- **Provider data-plane vtable** `KlDatagramOps` + its descriptor types + `KL_DGRAM_RX_*` → move to a
  provider-scoped header **`keel/socket_dgram.h`** (included by `keel/socket.h`). It is the
  `KlSocketProvider.dgram` seam and stays named `KlDatagramOps` (a provider concept, not the app API).
  Freeing `keel/datagram.h` of the provider vtable is what lets the public API take that header.
  **Source-compat (review Medium-3):** relocating `KlDatagramOps` out of `<keel/datagram.h>` would break
  existing provider authors who `#include <keel/datagram.h>` directly. To keep it source-compatible, the
  **new public `<keel/datagram.h>` re-includes `<keel/socket_dgram.h>`** (a compatibility re-export), so
  `KlDatagramOps` + the descriptor types stay visible through the old include path. The move is then a
  relocation, not a break; if a future major version wants to drop the re-export it is classified +
  documented then. (Guard against an include cycle: `socket_dgram.h` must not include `datagram.h`.)
- **Public fixed-slot API** takes the freed **`keel/datagram.h`** (functions + `KlDatagram` handle) and
  **`keel/datagram_detail.h`** (the public layout). `KlDatagramMessage/SendStatus/CloseResult` are
  promoted from `src/` into `keel/datagram.h` (their contracts are already frozen; only the header
  location changes).

This is the single most invasive part of 7B (it touches every completion backend, which references
`struct KlDatagram *dg` in `kl_comp_post_dgram_*`, plus `udp.c`, EFI, lwIP, and the socket providers).
It is deliberately isolated into **one mechanical rename increment (7B-1)** with **zero behaviour
change**, so its review is "diff is a pure rename + relocation; all suites green."

---

## 1. Frozen public ABI

The surface, `KlDatagramConfig`, and the STABLE banner are already frozen in
`docs/datagram_step7_public_api_design.md` §1 — pinned here verbatim by reference, not restated. 7B-0
additionally freezes the **layout** and **type homes**:

### 1.1 Handle + layout — resolved to an opaque-pointer core (review High-1)

A **by-value** `KlDgramCore core;` in `<keel/datagram_detail.h>` is **NOT installable as first sketched**:
the detail header would need `KlDgramCore`'s complete type, which lives in `src/datagram_core.h` and
recursively pulls `src/datagram_{slots,send,recv,close,life}.h` — none of which are installed. There is
no "src-only include" an installed consumer can satisfy. Two ways out; **frozen choice = the second**:

- *(A) by-value)* Relocate the whole machine layout — `KlDgramCore` + `KlDgramSlots` + `KlDgramSend` +
  `KlDgramClose` (embedded by value) — into an **installed unstable core-layout header family** under
  `include/keel/` (e.g. `keel/datagram_core_detail.h` including `keel/dgram_{slots,send,close}_detail.h`;
  recv/inbound/life stay behind the rx pointer). Preserves the value type but **installs four internal
  machine layouts** as public-unstable headers — a large, churny installed surface.
- **(B) opaque pointer + explicit allocation — CHOSEN.** `<keel/datagram_detail.h>` only forward-declares
  `KlDgramCore`; `kl_datagram_init` allocates the core from `cfg->alloc`, `kl_datagram_free` releases it.
  The installed detail surface stays tiny (one forward decl); no internal machine layout is exported.

```c
/* keel/datagram_detail.h (opt-in, UNSTABLE) */
struct KlDgramCore;   /* opaque — full type is src-only; the facade heap-allocates it */
struct KlDatagram {
    struct KlDgramCore *core;   /* heap, allocated in init from cfg->alloc, freed in free (after CLOSED) */
    /* facade-only state (NOT in the core): the user recv callback, last_error, and the borrowed handles
     * kept for the close/backend-retirement step + reuse. */
    KlDatagramRecvFn on_recv; void *recv_ud;
    KlError last_error;
    KlEventCtx *ctx; const KlSocketProvider *sockets;
};
```

`KlDatagram` stays a **caller-owned handle** (the struct lives wherever the caller puts it — stack,
embed, or heap); only its heavy machine state is heap-allocated behind `core`. Cost vs. by-value: **one
extra allocation at init** (a new init failure mode — handled without taking fd ownership, §2/§2.5) and
an indirection. Benefit: the installed ABI is a forward decl, and 7A's `KlDgramCore`/machine headers
stay purely internal. (§1 of the main design doc says "value type" of the HANDLE — an opaque-pointer
handle satisfies it; it does not require embedding the core by value.)

### 1.2 Type homes (frozen)

| Type | Home in 7B |
|---|---|
| `KlDatagram` (handle) | `keel/datagram.h` (public, STABLE contract) |
| `struct KlDatagram` (layout) | `keel/datagram_detail.h` (opt-in, UNSTABLE) |
| `KlDatagramMessage`, `KlDatagramSendStatus`, `KlDatagramCloseResult`, `KlDgramCloseState`, `KL_DGRAM_CAP_*` | promoted to `keel/datagram.h` (public) |
| `KlDatagramRecvFn`, `KlDatagramWritableFn`, `KlDatagramDrainFn`, `KlDatagramCloseFn` | `keel/datagram.h` (public callbacks) |
| `KlUdpTransport` (was `struct KlDatagram`) | `keel/udp_transport_detail.h` (internal) |
| `KlDatagramOps` + `KlDgramRx*`/`KlDgramTxDesc` + `KL_DGRAM_RX_*` | `keel/socket_dgram.h` (provider seam) |

### 1.3 STABLE banner (frozen wording — from §1)

`<keel/datagram.h>` carries the §1 banner verbatim: STABLE covers the `kl_datagram_*` function + type
CONTRACT; the struct LAYOUT is not ABI-stable; a layout-embedding consumer includes
`<keel/datagram_detail.h>` and must recompile on change; a `KlDatagram *`-only consumer is insulated.

---

## 2. Frozen ownership + lifetime contract

Inherited from `docs/datagram_step7_public_api_design.md` §1/§4/§5 and the 7A implementation; pinned:

- **fd ownership transfers on `kl_datagram_init` returning 0 ONLY.** On failure, nothing the caller must
  reclaim is touched; the caller retains the fd. (Enforced in `KlDgramCore` init already — 7A-3.)
- **Backend retirement + fd close happen in the close machine's retirement step, EXACTLY ONCE, not in
  `free`** — via the `close_transport` hook the facade binds to the provider's socket close (7A-3).
  `close_transport` is REQUIRED (7A-3 review 2).
- **`kl_datagram_free` is refused (-1) before terminal close (CLOSED)**; after CLOSED it releases object
  memory. The life-owned rx storage is freed by the B.6 token's final release; a QUARANTINED op pins it
  (fail-closed) — the object is still safe to free.
- **Uniform B.6 token**: every posted op (send AND recv) holds one stable-token ref (7A-3 review 2).
- **Reuse**: `kl_datagram_init` on a memset-zero (fully-detached) object (invariant 8).
- **Sockopts stay OUT of this surface** (§6): the caller prepares the fd (`socket()`/`configure()`/
  `bind`) through `sockets->dgram` before `init`. `KlDatagram` never sets a sockopt.
- **`KlUdp` is NOT re-based** onto `KlDatagram` (D-COMPAT §6). It keeps `KlUdpTransport` (byte-budget).
  The two surfaces coexist permanently; consumers choose. (Confirm in §7.)

---

## 2.5 Live adapter boundary (review High-2)

`KlDgramCore` takes NEUTRAL hooks (submit / arm / disarm / pull / cancel / retire / close_transport).
`KlDatagramConfig` does **not** carry them, and `src/datagram.c` cannot manufacture backend behaviour
generically. Frozen boundary — **the facade assembles the hooks from the EXISTING backend seams**, in
exactly two implementations, selected by the loop's capability (the same negotiation `KlUdp` already
does via `src/event_caps.h`):

| Core hook | Completion mode (loop has `KL_EVENT_CAP_COMPLETION`) | Readiness mode (`KL_EVENT_CAP_READINESS` + `sockets->dgram`) |
|---|---|---|
| `submit` (send) | `KlCompletionOps.post_dgram_send` | `sockets->dgram->send` (synchronous) |
| `arm` (recv) | `KlCompletionOps.post_dgram_recv` | add READ interest (a `KlWatcher` on the fd) |
| `disarm` | — (completion holds the posted op) | remove READ interest |
| `pull` | — | `sockets->dgram->recv` |
| `cancel_send/recv` | `KlCompletionOps.cancel_dgram` *(new, additive)* | drop interest / no-op |
| `retire` (§4.3 classify) | `KlCompletionOps.retire_dgram` *(new, additive; default RETIRED-on-terminal, EFI overrides QUARANTINED)* | synchronous → always RETIRED |
| `close_transport` | `sockets->stream`/provider socket close | provider socket close |

So `src/datagram.c` contains **two adapter builders** (`dgram_adapter_completion`,
`dgram_adapter_readiness`) that WRAP the existing seams — **no per-backend code in the facade**. The
per-backend behaviour comes from each backend's already-implemented `KlCompletionOps.post_dgram_*` /
`KlSocketProvider.dgram`, exactly as `KlUdp` gets it today.

**Where the new hooks live (frozen):** `cancel_dgram` + `retire_dgram` are added **additively to
`KlCompletionOps`** (the completion axis, `src/event_caps.h` / the completion-ops table), NOT to any
per-backend header — so a backend that doesn't implement them gets the documented default (RETIRED on
terminal completion). Their exact signatures are finalized in **7B-3** (the reference completion seam)
but their HOME (`KlCompletionOps`) and default semantics are frozen now.

**Init failure without fd ownership (frozen):** `kl_datagram_init` selects a mode BEFORE calling
`kl_dgram_core_init`. If the loop offers neither a datagram-capable completion seam
(`post_dgram_recv/send` present) NOR a datagram-capable readiness provider (`sockets->dgram != NULL`),
`init` returns -1 immediately — the fd is never adopted (it is only handed to `kl_dgram_core_init`, which
itself adopts on success only). A requested capability the selected mode can't supply is a
`KL_DATAGRAM_UNSUPPORTED`-class init failure, likewise pre-adoption.

---

## 3. Increment breakdown

Each increment is a single reviewed commit (or a tight cluster) with its own tests; each pauses for
review; none proceeds before the prior is approved — same cadence as 7A.

### 7B-0 — ABI + namespace + ownership freeze  *(THIS document; docs-only)*
Deliverable: this doc, reviewed and frozen. No code. Resolves §0 namespace, §1 ABI/type homes, §2
ownership, and the §7 open decisions. **Gate: reviewer sign-off on the frozen contract.**

### 7B-1 — the rename (pure mechanical, zero behaviour change)
- `struct KlDatagram` → `KlUdpTransport`; `keel/datagram_detail.h` → `keel/udp_transport_detail.h`.
- Relocate `KlDatagramOps` + descriptors + `KL_DGRAM_RX_*` → `keel/socket_dgram.h`.
- Sweep every reference: `udp.c`, `completion_*.c`, `event_{pollcomp,iouring,iocp,efi_*}.c`,
  `integrations/lwip`, `socket_*.c`, the freestanding manifest, tests.
- **Validation:** the WHOLE existing suite green unchanged on every gated backend (macOS, container
  ASan/UBSan/LSan, freestanding-dgram, lwIP loopback-raw-asan, MinGW, cosmo). The diff is a rename +
  header move; no logic changes. **This frees the `KlDatagram` / `keel/datagram.h` namespace.**

### 7B-2 — public facade + headers + `test_datagram_public` (NO live backend)
- `keel/datagram.h` (public API + promoted types) + `keel/datagram_detail.h` (layout embedding
  `KlDgramCore`) + `src/datagram.c` (the `kl_datagram_*` facade forwarding to `KlDgramCore`, binding the
  neutral adapter hooks to a provider+completion loop *interface* — but exercised here by a MOCK).
- `tests/test_datagram_public.c` — drives the PUBLIC API over a scripted provider/completion mock
  (the `test_dgram_core` adapters, one layer up): init/reuse/free-refusal, fixed-slot send geometry,
  strict pause/resume, confirmed-detachment close + terminal result, copy-before-accept, ownership
  (fd-on-success, free-after-close), counters. Proves the **public surface + ABI + ownership** with
  **zero live-backend risk**. §10 matrix rows stay ⚙ (no live provider yet).
- **Validation:** macOS `make test`, container ASan/UBSan/LSan, freestanding-dgram (the facade must stay
  freestanding-clean), MinGW/cosmo header checks.

### 7B-3 … 7B-8 — LIVE backend seams (the 7A-4b work), one increment each
Each binds `KlDgramCore`'s neutral adapters (submit / arm / disarm / pull / cancel / retire /
close_transport) to a REAL provider + completion (or readiness) loop for that backend, adds a live
datagram round-trip test, flips that backend's `§10` rows (send-slot + strict-pause) to ✅, and — per
the 7A-3 reviewer note — **retains the existing backend tests** (post-failure unwind, inline/early
completion ordering are adapter concerns proven per backend). Proposed order, easiest-to-hardest:

- **7B-3 — pollcomp** (portable `poll()` completion double). Testable on any POSIX host + CI/ASan; the
  reference completion seam. Establishes the facade↔provider binding pattern the rest reuse.
- **7B-4 — io_uring** (Linux completion; container).
- **7B-5 — readiness (epoll / kqueue / poll)** — the readiness datagram seam (arm/disarm/pull path).
  `KlDatagram` supports readiness because `KlDgramCore` does; this wires it.
- **7B-6 — IOCP** (Windows completion; MinGW cross-compile + CI).
- **7B-7 — lwIP-raw** (completion; Apple container). Reuses the 7A-5 one-held glue.
- **7B-8 — EFI** (freestanding; QEMU/OVMF). Reuses the EFI_UDP4 datagram provider.

**Backend scope (review Medium-4).** Every backend that already exposes the datagram provider capability
must get a working facade before the public surface is finalized/advertised STABLE — otherwise a
consumer sees a capability with no usable API on IOCP/lwIP/EFI. So:

- **7B-5 is a CHECKPOINT, not completion.** After pollcomp + io_uring + readiness the public API is
  *usable + `§8`-validated on the primary hosted backends*, but it is NOT yet advertised STABLE and the
  IOCP/lwIP-raw/EFI `§10` rows stay ⚙.
- **7B-8 is completion** — every supported live backend (IOCP, lwIP-raw, EFI) has a working facade.
- **STABLE + advertise happens only at 7B-9**, after 7B-8. A hosted-only early adopter can use the API
  after the 7B-5 checkpoint (documented "usable, not yet STABLE"); the banner is not flipped until all
  supported backends are live.

### 7B-9 — matrix + banner finalization (only after 7B-8)
- All supported-backend `§10` rows → ✅; the STABLE banner is flipped (no "not-yet-wired" caveat
  remains); docs reconciliation (contract + this doc + the main design doc); README/site datagram entry.
- If any supported backend is deliberately deferred beyond 7B, it stays ⚙ with an explicit note and the
  banner keeps a matching caveat (no silent "covered").

---

## 4. Per-increment validation matrix

| Increment | macOS | container ASan/UBSan/LSan | freestanding-dgram | lwIP raw-asan | MinGW | cosmo | QEMU/OVMF |
|---|---|---|---|---|---|---|---|
| 7B-1 rename | ✅ full suite | ✅ full | ✅ | ✅ | ✅ headers | ✅ | ✅ (EFI builds link) |
| 7B-2 facade+public test | ✅ | ✅ | ✅ facade clean | — | ✅ headers | ✅ | — |
| 7B-3 pollcomp | ✅ | ✅ live round-trip | — | — | — | — | — |
| 7B-4 io_uring | — | ✅ live round-trip | — | — | — | — | — |
| 7B-5 readiness | ✅ (kqueue) | ✅ (epoll) | — | — | — | — | — |
| 7B-6 IOCP | — | — | — | — | ✅ + CI | — | — |
| 7B-7 lwIP-raw | — | — | — | ✅ live | — | — | — |
| 7B-8 EFI | — | — | ✅ | — | — | — | ✅ e2e |
| 7B-9 finalize | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

Each live-backend increment ALSO keeps its existing per-backend suite green (KlUdp + protocol tests over
that backend are unchanged — `KlUdp` still rides `KlUdpTransport`).

---

## 5. What 7B does NOT change

- `KlUdp` behaviour or its byte-budget transport (now `KlUdpTransport`) — D-COMPAT §6.
- `KlDgramCore` and its machines — 7B consumes them as-is (any change found necessary is a scoped,
  separately-reviewed fix, not folded into a 7B increment).
- The provider datagram data-plane vtable's *contract* (`KlDatagramOps`) — only its header home moves.
- Protocol/`src/` code — the facade is additive.

---

## 6. Risks

- **7B-1 blast radius.** The rename touches every completion backend + freestanding manifest + Windows
  + cosmo. Mitigation: pure mechanical, no logic change, full multi-target gate before review; land it
  alone so any breakage is unambiguously "rename typo," not behaviour.
- **Header layering under freestanding.** `keel/datagram.h` becoming the public API must stay
  freestanding-clean (no host headers). The Medium-3 compat re-export (`datagram.h` → `socket_dgram.h`)
  must NOT create an include cycle (`socket_dgram.h` never includes `datagram.h`). Verified by the
  freestanding-dgram gate in 7B-2.
- **Opaque-pointer core adds an init allocation** (a new failure mode). Mitigation: `init` allocates the
  core BEFORE adopting the fd, so an allocation failure returns -1 with nothing adopted (§2.5); `free`
  releases it only after CLOSED. This is the cost of keeping the installed ABI a forward decl (§1.1).
- **New `KlCompletionOps` hooks (`cancel_dgram`/`retire_dgram`).** Added additively with defaults so
  every existing backend keeps compiling + behaving (default RETIRED-on-terminal); only EFI overrides.
  Shape finalized in 7B-3, home frozen now (§2.5).

---

## 7. Open decisions to freeze in the 7B-0 review

1. **Namespace resolution** — accept §0's three-way split (transport→`KlUdpTransport`; provider vtable→
   `keel/socket_dgram.h` WITH the compat re-export from `keel/datagram.h`; public API takes
   `keel/datagram.h`), or an alternative (e.g. public API in a new `keel/datagram_api.h`, leaving the
   provider vtable in place)?
2. **`KlDgramCore` embedding — proposed RESOLVED to opaque pointer + explicit allocation** (§1.1,
   review High-1): confirm, or require the by-value installed core-layout header family instead.
3. **Backend scope (review Medium-4)** — confirm 7B-5 = hosted CHECKPOINT (usable, not STABLE) and
   7B-8 = completion (every supported live backend), with STABLE/advertise only at 7B-9; or a different
   line for what "7B done" requires.
4. **Live adapter boundary (§2.5)** — confirm the facade-owns-two-builders model over existing seams,
   `cancel_dgram`/`retire_dgram` added additively to `KlCompletionOps`, and the pre-adoption init-failure
   contract for a loop lacking a datagram seam.
5. **`KlUdp` permanence** — confirm `KlUdp` stays on `KlUdpTransport` permanently (never re-based on the
   public `KlDatagram`), so the two surfaces coexist by design.
6. **fd-preparation contract** — confirm the caller prepares the fd via `sockets->dgram`
   (`socket`/`configure`/`bind`) before `kl_datagram_init`, and `KlDatagram` sets no sockopt (§2).

**No 7B code — not even the mechanical rename — begins until these are frozen.**
