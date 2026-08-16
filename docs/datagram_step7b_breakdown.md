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
| `close_transport` | provider `KlSocketOps.close` | provider `KlSocketOps.close` |

So `src/datagram.c` contains **two adapter builders** (`dgram_adapter_completion`,
`dgram_adapter_readiness`) that WRAP the existing seams — **no per-backend code in the facade**. The
per-backend behaviour comes from each backend's already-implemented `KlCompletionOps.post_dgram_*` /
`KlSocketProvider.dgram`, exactly as `KlUdp` gets it today.

**7B-7 design resolution — the completion fd↔loop registration (against this mapping).** The frozen
completion seam is `post/cancel/retire` (§2.5.1) — it has **no socket-association lifecycle**. But a
completion transport must register its fd with the loop before posting overlapped ops: on IOCP that is
`CreateIoCompletionPort(fd, port)`; on io_uring/pollcomp it is inert. This is NOT part of the datagram
seam — it is the **generic `kl_event_add`/`kl_event_del` event-loop registration** that `KlUdp` already
uses for completion loops (`kl_udp_recv_start`). Rather than fold association into the IOCP `post_dgram`
(which would add associate/detach state + a close-time detach the seam does not express, and would touch
`KlUdp`'s only CI-tested IOCP datagram path), the **completion adapter uses the same generic
registration IN ADDITION to post/cancel/retire** — backend-neutral (inert on io_uring/pollcomp), not
IOCP-specific facade logic. **Ordering (review High — the load-bearing correction): registration is the
LAST fallible step.** IOCP can NOT detach an ordinary socket from a completion port (`kl_event_del` is a
no-op there; only closing the socket drops the association), so an associate-then-fail path could not be
undone. The registration therefore runs via a `KlDgramCore` **pre-adoption hook** (`KlDgramCoreConfig.
on_prepared`), invoked ONCE after every core allocation has succeeded and immediately before the fd is
adopted: on failure the core unwinds all prepared allocations and returns -1 WITHOUT adopting the fd, so
`kl_event_add`/`CreateIoCompletionPort` (if it ran at all) is the only thing that could have failed — the
fd is NEVER left associated, and no allocation can fail after a successful association. At close the
coordinator retires every op, then `kl_event_del`, then the socket close — exactly once (`kl_event_del`
is symmetric bookkeeping; on IOCP the socket close does the de-association). `KlUdp`'s lifecycle is
unchanged. Runtime proof is the Windows IOCP CI (`smoke_datagram`) — PROVISIONAL until that job is green;
pollcomp/io_uring/readiness cannot exercise the association (their `kl_event_add` is inert). Mock
coverage: registration-failure, register-before-post + deregister-before-close ordering, AND
**allocation-failure-during-prep → fd never registered** (`tests/test_datagram_public.c`).

### 2.5.1 The completion-post seam must be neutralized (review High — the real blocker)

Today `kl_comp_post_dgram_send/recv` take `KlUdpTransport *dg` (post-rename) and **dereference the legacy
object** for the fd, the receive buffer, the capture flags, and the `KlDgramLife *`. A public
`KlDatagram`/`KlDgramCore` cannot be passed to them, so the completion adapter of §2.5 **cannot wrap the
existing seam as-is** — and adding only `cancel_dgram`/`retire_dgram` does not solve *posting*. The seam
must become **core-native / neutral** BEFORE the facade or mock encode it (else they encode an interface
the first live backend cannot implement). Frozen as a distinct increment (**7B-2**, below).

**Frozen boundary = operation DESCRIPTORS carrying everything the op needs — no transport deref.** Both
`KlUdpTransport` (KlUdp's path) and `KlDgramCore` (the facade) BUILD these from their own state:

**Header home (review Medium):** these descriptors carry the internal B.6 token + retirement-machine
concepts and are consumed by `KlCompletionOps` — they belong on the **completion-model AXIS**, in the
INTERNAL completion header (`src/io_engine.h` / the `KlCompletionOps` header), **NOT** the public
provider-facing `keel/socket_dgram.h`. `keel/socket_dgram.h` holds only the provider data-plane
(`KlDatagramOps` + `KlDgramRx*/TxDesc`); mixing the completion axis into the provider axis is exactly the
layering the axis-audit forbids. So:

```c
/* src/io_engine.h — the COMPLETION AXIS (internal), alongside KlCompletionOps — NOT a provider header */
typedef struct {
    KlSocketHandle fd;
    const void   *data; size_t len;         /* payload — COPIED into op storage before a successful post */
    const KlSockAddr *dest, *src; int tos;   /* dest UNSPEC=connected; src UNSPEC=no pin; tos -1=none —
                                              * ALSO copied before a successful post (address metadata is
                                              * captured with the payload, not aliased past return) */
    struct KlDgramLife *life;                /* token ref — see the ownership rule below */
} KlDgramSendOp;

typedef struct {
    KlSocketHandle fd;
    void         *buf; size_t cap;           /* the inbound slot — LENT (life-owned), backend writes it */
    unsigned      capture;                   /* KL_DGRAM_RX_* metadata-capture flags */
    struct KlDgramLife *life;
} KlDgramRecvOp;

/* added additively to KlCompletionOps (the completion axis) */
int  (*post_dgram_send)(struct KlEventCtx *ctx, const KlDgramSendOp *op);   /* was (…, KlUdpTransport*) */
int  (*post_dgram_recv)(struct KlEventCtx *ctx, const KlDgramRecvOp *op);
int  (*cancel_dgram)(struct KlEventCtx *ctx, struct KlDgramLife *life, KlDgramOpKind kind);
KlDgramRetireResult (*retire_dgram)(struct KlEventCtx *ctx, struct KlDgramLife *life,
                                    KlDgramOpKind kind, int *transport_err);
```

**Ownership-transfer rules (frozen — match the existing pollcomp/io_uring/IOCP/EFI/lwIP discipline the
7A-3-retained backend tests cover):**
- **Post — transfer ONLY on success (review High-1).** The caller holds one retained `life` ref across
  the post call. Ownership transfers into the op **iff `post_dgram_*` returns success** (op accepted /
  INFLIGHT). On **failure** (bad arg, allocation / SQE-full / `WSASendTo` error, etc.) the post returns
  -1 having taken NO ownership: the **caller** releases its retained ref, and the backend MUST NOT retain
  or release it. This is the single rule that prevents a leak (backend forgot) or double-release (both
  sides released) on a failed submit — the exact failure surface io_uring/IOCP add.
- **Payload AND address metadata:** a send op's `data` — **and its `dest`/`src`/`tos`** — are COPIED into
  backend op storage before a successful post returns (copy-before-accept). Nothing in the descriptor is
  aliased past a successful return, so the object-owned outbound pool + the caller's address structs are
  safe to reuse/free immediately.
- **Recv buffer:** a recv op's `buf` is LENT (the life-owned inbound slot); the backend writes into it
  and NEVER frees it. The completion delivers `len` + `peer` + `meta` (already how `KlCompletionEvent`
  carries a datagram RECV).
- **Completion + shared-ctx routing — frozen type-safe discriminator (review High-2).** The op's `life`
  ref rides the completion event (`ev->life`); the dispatch releases it after delivery. `KlDgramLife`
  MUST NOT be routed by guessing what `kl_dgram_life_target()`'s untyped pointer is. Frozen mechanism:
  **`KlDgramLife` carries, set at creation, an owner DISPATCH callback + a kind tag** —
  `KlDgramOwnerKind kind` (`KL_DGRAM_OWNER_UDP` / `KL_DGRAM_OWNER_DATAGRAM`, extensible) and
  `void (*dispatch)(void *target, const KlCompletionEvent *ev)`. The completion path calls
  `life->dispatch(kl_dgram_life_target(life), ev)` — each owner supplies its OWN typed handler
  (`kl_udp_comp_dispatch` for `KlUdpTransport`, `kl_datagram_comp_dispatch` for `KlDgramCore`), so
  routing is type-safe with no downcast-by-guess and KlUdp + KlDatagram coexist on one ctx. (This
  replaces today's single ctx-global `comp_udp_dispatch`.) `kl_dgram_life_create` gains the
  `kind`+`dispatch` params; landed in 7B-2.
- **Cancel:** `cancel_dgram` is idempotent and does NOT release the ref; the op's terminal completion
  (even when cancelled) releases it.
- **Retire:** `retire_dgram` is a pure query (no ownership effect); default RETIRED-on-terminal, EFI
  overrides QUARANTINED on an unconfirmed op.

`cancel_dgram`/`retire_dgram` and the descriptor-based `post_dgram_*` are added **additively to
`KlCompletionOps`** (the completion axis), NOT per-backend; a backend with no `retire_dgram` gets the
default. **7B-2 (below) neutralizes this seam across ALL completion backends at once** (each backend's
`post_dgram_*` refactored to descriptors; `KlUdp` rebuilds the descriptors from `KlUdpTransport`; full
suite green, zero behaviour change) — so the interface is proven real on every backend BEFORE the facade
(7B-3) or any live binding encodes it. Nothing about post/cancel/retire is deferred to a live increment.

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
- Relocate `KlDatagramOps` + its PROVIDER descriptors (`KlDgramRx*`/`KlDgramTxDesc`) + `KL_DGRAM_RX_*` →
  `keel/socket_dgram.h`. (The completion-axis op descriptors `KlDgramSendOp`/`KlDgramRecvOp` are a
  SEPARATE, internal `src/io_engine.h` concern introduced in 7B-2 — not part of this rename.)
- Sweep every reference: `udp.c`, `completion_*.c`, `event_{pollcomp,iouring,iocp,efi_*}.c`,
  `integrations/lwip`, `socket_*.c`, the freestanding manifest, tests.
- **Validation:** the WHOLE existing suite green unchanged on every gated backend (macOS, container
  ASan/UBSan/LSan, freestanding-dgram, lwIP loopback-raw-asan, MinGW, cosmo). The diff is a rename +
  header move; no logic changes. **This frees the `KlDatagram` / `keel/datagram.h` namespace.**

### 7B-2 — neutralize the completion post/cancel/retire seam (§2.5.1; zero behaviour change)
- Refactor `KlCompletionOps.post_dgram_send/recv` from `(…, KlUdpTransport *dg)` to the descriptor form
  (`KlDgramSendOp`/`KlDgramRecvOp`, in `src/io_engine.h`), and add `cancel_dgram` + `retire_dgram`, across
  ALL completion backends (`event_{pollcomp,iouring,iocp,efi_*}.c`, `integrations/lwip`, `completion_*.c`).
  Enforce transfer-only-on-success in every backend's post path (failure releases nothing).
- Add `{kind, dispatch}` to `KlDgramLife` (`kl_dgram_life_create` gains the params); replace the
  ctx-global `comp_udp_dispatch` with `life->dispatch(target, ev)` in the completion dispatch. `KlUdp`
  registers `kl_udp_comp_dispatch` on its tokens (target = `KlUdpTransport`); behaviour identical.
- Re-point `KlUdp`: `udp.c` builds the descriptors from `KlUdpTransport` at post time (was: passed `dg`).
- **Validation:** the WHOLE existing suite green unchanged on every gated backend — the seam is
  behaviour-preserving; only its shape changes. This is the increment that makes the seam CORE-NATIVE, so
  the facade and mock (7B-3) can encode an interface every live backend already implements. Kept separate
  from the 7B-1 rename so each review is a single concern.

### 7B-3 — public facade + headers + `test_datagram_public` (NO live backend)
- `keel/datagram.h` (public API + promoted types + the `keel/socket_dgram.h` compat re-export) +
  `keel/datagram_detail.h` (opaque-`KlDgramCore` layout, §1.1) + `src/datagram.c` (the `kl_datagram_*`
  facade forwarding to `KlDgramCore`, with the two adapter builders of §2.5 — exercised here by a MOCK
  completion/readiness seam).
- `tests/test_datagram_public.c` — drives the PUBLIC API over a scripted mock (the `test_dgram_core`
  adapters, one layer up): init/reuse/free-refusal, fixed-slot send geometry, strict pause/resume,
  confirmed-detachment close + terminal result, copy-before-accept, ownership (fd-on-success,
  free-after-close, alloc-failure pre-adoption), counters. Proves the **public surface + ABI + ownership**
  with **zero live-backend risk**. §10 matrix rows stay ⚙ (no live provider yet).
- **Validation:** macOS `make test`, container ASan/UBSan/LSan, freestanding-dgram (the facade must stay
  freestanding-clean; the compat re-export must not cycle), MinGW/cosmo header checks.

### 7B-4 … 7B-9 — LIVE backend bindings (the 7A-4b work), one increment each
Each binds the facade's adapter builders (over the now-neutral 7B-2 seam) to a REAL loop for that
backend, adds a live datagram round-trip test, flips that backend's `§10` rows (send-slot + strict-pause)
to ✅, and — per the 7A-3 reviewer note — **retains the existing backend tests** (post-failure unwind,
inline/early completion ordering, proven per backend). Order easiest-to-hardest:

- **7B-4 — pollcomp** (portable `poll()` completion double). The reference live binding; establishes the
  pattern the rest reuse.
- **7B-5 — io_uring** (Linux completion; container).
- **7B-6 — readiness (epoll / kqueue / poll)** — the readiness datagram seam (arm/disarm/pull). — the
  **hosted CHECKPOINT** (below).
- **7B-7 — IOCP** (Windows completion; MinGW cross-compile + CI).
- **7B-8 — lwIP-raw** (completion; Apple container). Reuses the 7A-5 one-held glue.
- **7B-9 — EFI** (freestanding; QEMU/OVMF). Reuses the EFI_UDP4 datagram provider. — **completion**.

**Backend scope (review Medium-4).** Every backend that already exposes the datagram provider capability
must get a working facade before the public surface is finalized/advertised STABLE — else a consumer sees
a capability with no usable API on IOCP/lwIP/EFI. So:

- **7B-6 is a CHECKPOINT, not completion.** After pollcomp + io_uring + readiness the public API is
  *usable + `§8`-validated on the primary hosted backends*, but it is NOT yet advertised STABLE and the
  IOCP/lwIP-raw/EFI `§10` rows stay ⚙.
- **7B-9 is completion** — every supported live backend (IOCP, lwIP-raw, EFI) has a working facade.
- **STABLE + advertise happens only at 7B-10**, after 7B-9. A hosted-only early adopter can use the API
  after the 7B-6 checkpoint (documented "usable, not yet STABLE"); the banner is not flipped until all
  supported backends are live.

### 7B-10 — matrix + banner finalization (only after 7B-9) — **DONE**
- All supported-backend `§10` rows → ✅; the STABLE banner is flipped (no "not-yet-wired" caveat
  remains); docs reconciliation (contract + this doc + the main design doc); README/site datagram entry.
- If any supported backend is deliberately deferred beyond 7B, it stays ⚙ with an explicit note and the
  banner keeps a matching caveat (no silent "covered").
- **Done:** the `<keel/datagram.h>` STABLE banner is finalized (STABLE function+type contract, live across
  pollcomp/io_uring/IOCP/lwIP-raw/EFI_UDP4 + POSIX & Winsock readiness); the §10 send-slot + strict-pause
  rows are ✅ for every supported backend (Winsock/WSAPoll flipped ✅ — the same backend-agnostic readiness
  adapter as POSIX, proven by the CI Windows `smoke-datagram`). The lone remaining ⚙ (lwIP-raw's 16-slot
  serial-recv ring → one-held-slot rework) is an explicitly-noted provider-internal buffering detail that
  does NOT weaken the STABLE API contract (the facade still delivers one datagram per op), NOT a deferred
  backend — so the banner carries no caveat. `<keel/datagram_detail.h>` keeps its OPT-IN/UNSTABLE-layout
  banner (the ABI split from 7B-3). Frozen validation matrix re-run green.

---

## 4. Per-increment validation matrix

| Increment | macOS | container ASan/UBSan/LSan | freestanding-dgram | lwIP raw-asan | MinGW | cosmo | QEMU/OVMF |
|---|---|---|---|---|---|---|---|
| 7B-1 rename | ✅ full suite | ✅ full | ✅ | ✅ | ✅ headers | ✅ | ✅ (EFI builds link) |
| 7B-2 neutralize seam | ✅ full suite | ✅ full | ✅ | ✅ | ✅ | ✅ | ✅ (EFI builds link) |
| 7B-3 facade+public test | ✅ | ✅ | ✅ facade clean | — | ✅ headers | ✅ | — |
| 7B-4 pollcomp | ✅ | ✅ live round-trip | — | — | — | — | — |
| 7B-5 io_uring | — | ✅ live round-trip | — | — | — | — | — |
| 7B-6 readiness *(checkpoint)* | ✅ (kqueue) | ✅ (epoll) | — | — | — | — | — |
| 7B-7 IOCP | — | — | — | — | ✅ + CI | — | — |
| 7B-8 lwIP-raw | — | — | — | ✅ live | — | — | — |
| 7B-9 EFI *(completion)* | — | — | ✅ | — | — | — | ✅ e2e |
| 7B-10 finalize | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

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
  Shape frozen now (§2.5.1); lands in 7B-2.

---

## 7. Open decisions to freeze in the 7B-0 review

1. **Namespace resolution** — accept §0's three-way split (transport→`KlUdpTransport`; provider vtable→
   `keel/socket_dgram.h` WITH the compat re-export from `keel/datagram.h`; public API takes
   `keel/datagram.h`), or an alternative (e.g. public API in a new `keel/datagram_api.h`, leaving the
   provider vtable in place)?
2. **`KlDgramCore` embedding — proposed RESOLVED to opaque pointer + explicit allocation** (§1.1,
   review High-1): confirm, or require the by-value installed core-layout header family instead.
3. **Backend scope (review Medium-4)** — confirm 7B-6 = hosted CHECKPOINT (usable, not STABLE) and
   7B-9 = completion (every supported live backend), with STABLE/advertise only at 7B-10; or a different
   line for what "7B done" requires.
4. **Completion-post seam neutralization (§2.5.1, review High-1/High-2/Medium)** — confirm: the
   descriptor `post_dgram_send/recv` (`KlDgramSendOp`/`KlDgramRecvOp`) + `cancel_dgram`/`retire_dgram` on
   `KlCompletionOps` live in the INTERNAL completion header (`src/io_engine.h`), not `keel/socket_dgram.h`;
   **transfer-only-on-success** (failure → caller releases, backend must not touch); payload AND
   `dest`/`src`/`tos` copied before a successful post; and the **type-safe dispatch discriminator** —
   `KlDgramLife` carries `{kind, dispatch}` set at creation, dispatch called as
   `life->dispatch(target, ev)`. Landed as 7B-2 (all backends) BEFORE the facade.
5. **Live adapter boundary (§2.5)** — confirm the facade-owns-two-builders model over existing seams and
   the pre-adoption init-failure contract for a loop lacking a datagram seam.
6. **`KlUdp` permanence** — confirm `KlUdp` stays on `KlUdpTransport` permanently (never re-based on the
   public `KlDatagram`), so the two surfaces coexist by design.
7. **fd-preparation contract** — confirm the caller prepares the fd via `sockets->dgram`
   (`socket`/`configure`/`bind`) before `kl_datagram_init`, and `KlDatagram` sets no sockopt (§2).

**No 7B code — not even the mechanical rename — begins until these are frozen.**
