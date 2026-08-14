# Phase 10 · Step 6.4b — EFI_UDP4 datagram provider (DESIGN FREEZE)

Status: **FROZEN design, pre-implementation.** This document freezes the architecture,
the **handle-discrimination** scheme, and the **cancellation / quarantine state machine**
for a generic EFI_UDP4 datagram provider, so the stock `src/dns_resolver.c` (already
freestanding-enabled in 6.4a-2) resolves over `KlUdp`-over-EFI_UDP4 on bare firmware.
No production code changes land with this document; implementation follows as 6.4b once
this freeze is reviewed.

Sibling references (verified against the tree):
- `integrations/uefi/socket_efi_tcp4.{h,c}` — the EFI_TCP4 `KlSocketProvider` (the handle /
  slot-pool / generation-guard / quarantine template we mirror).
- `integrations/uefi/event_efi.{h,c}` — the EFI completion backend (`KlCompletionOps`) we extend.
- `integrations/uefi/dns_uefi.c` (retired) — the bespoke one-shot EFI_UDP4 DNS client that SEEDED
  the concrete EFI_UDP4 API pattern (Configure / Receive+RecycleSignal / Transmit / `u5_pump_or_cancel` /
  quarantine). **6.4b generalized this into the persistent provider; `dns_uefi.c` has SINCE BEEN
  RETIRED (R-1 `91d50fa` / R-2 `81e3188`, 2026-08), and 6.4c is the realized provider.**
- `src/datagram_life.{h,c}` — `KlDgramLife`, the frozen B.6 backend-owned stable receive token.
- `src/completion.h` — `KlCompletionOps.post_dgram_recv/_send`, `KlCompletionEvent{.life,.buf,.peer,.local,.truncated,...}`.
- `src/udp.c` — `kl_udp_init` (init requires the socket provider + `dgram->configure`),
  `kl_udp_comp_dispatch` (owner recovered via `ev->life`), `kl_udp_free` (drops the owner token ref).
- `docs/datagram_contract.md` §6/§10 (token contract + backend matrix; EFI is the last ⚙ column),
  §11 ("Freestanding (UDP-only) DNS build" — the consumer this enables).

---

## 1. Goal & scope

**In scope (6.4b):**
1. Give the EFI socket provider a **datagram data-plane** (`KL_SOCK_CAP_DATAGRAM` + a `KlDatagramOps`
   whose `configure` maps to `EFI_UDP4.Configure`, plus socket/bind/close/get_local_addr over an
   EFI_UDP4 child).
2. Add **`post_dgram_recv` / `post_dgram_send`** to the EFI completion backend (`event_efi.c`),
   posting `EFI_UDP4.Receive` / `EFI_UDP4.Transmit` tokens and surfacing
   `KL_COMP_DGRAM_RECV` / `KL_COMP_DGRAM_SEND` events that carry the **B.6 stable token**.
3. A **cancellation / quarantine state machine** for outstanding Rx/Tx tokens that is correct with
   respect to the stable token (a quarantined op must never let the token's `on_final` free storage
   the firmware may still touch) and fail-closed at ExitBootServices (EBS).

**Out of scope (later steps):** 6.4c = QEMU/OVMF end-to-end (`dns_resolver.c` over EFI_UDP4 → A/AAAA →
HTTP GET 200); the `dns_uefi.c` retirement (since DONE — R-1 `91d50fa` / R-2 `81e3188`, 2026-08);
EFI_UDP6; multicast/GSO/GRO/TOS datagram knobs (EFI_UDP4
has no analogue for most — they stay `✖` capabilities, degraded per §9 of the contract).

**Non-goal:** 6.4b does NOT make legacy `KlUdp` implement the Step-7 `KlDgramClose` confirmed-detachment
contract. `kl_udp_free` retires the receive machine via the stable token exactly as on the other four
completion backends; the EFI-specific obligation is *token-retirement-or-quarantine* at socket close,
which is distinct from Step 7.

---

## 2. Why completion-native (not readiness)

The EFI event loop (`event_efi.c`) advertises **`KL_EVENT_CAP_COMPLETION`** (U-3: async connect via
`post_connect` + `drain`). `udp.c` therefore takes the **completion** path:
- receive: `kl_comp_post_dgram_recv(&udp->dg)` (never the readiness watcher/`dgram->recv` pull);
- send (unconnected, no source-pin, default TOS — exactly the DNS resolver's send): the completion
  branch `kl_comp_post_dgram_send(&udp->dg, data, len, dest)`.

This is the correct model: it uses the real EFI_UDP4 asynchronous **token** machine (as the seed
`dns_uefi.c` did — that file has since been retired) and slots directly into the B.6 stable-token
contract the other completion backends satisfy.

`kl_udp_init` still calls the **socket provider** for `socket()/set_cloexec/set_nonblocking/bind/
get_local_addr` and `dgram->configure` regardless of readiness/completion. So the datagram vtable's
**`configure` is mandatory**.

**`dgram->recv` and `dgram->send` (review-Medium resolution).** These are only reached on the non-
completion paths (a readiness loop, or a completion loop's source-pinned / TOS send which `udp.c` routes
around the `post_dgram_send` fast path). For a **completion-only** EFI provider:

- **`dgram->recv = NULL`, unconditionally.** A synchronous EFI `Receive` here would be a **second receive
  machine** racing the serial completion `post_dgram_recv` token → it would violate the one-receive-in-
  flight contract. There is no readiness receive on EFI. Hard NULL.
- **`dgram->send`**: provide a **synchronous single-`Transmit` fallback** ONLY to cover the source-pinned
  / TOS sends the completion fast path skips, and ONLY under an **explicit guarantee** that it uses an
  **independent Tx token** (never a receive token, never the serial recv machine) and **participates in
  the same §7 cancel/quarantine discipline** (`pump_or_cancel`; unconfirmed → quarantine + fail-close).
  The DNS resolver never source-pins or sets TOS, so this path is **not** on the DNS hot path — it exists
  only so a general source-pinned datagram send does not silently fail or NULL-deref. Alternatively, a
  source-pin/TOS send may be **rejected** as `KL_DATAGRAM_UNSUPPORTED` (contract §9) rather than
  supported; either is conformant, but a second *receive* machine is never introduced.

**FROZEN #1:** EFI datagram I/O is completion-native — `post_dgram_recv/_send` in `event_efi.c` over
EFI_UDP4 Rx/Tx tokens; `dgram->configure` mandatory; **`dgram->recv` is NULL** (no second receive
machine); **`dgram->send` is either a synchronous independent-Tx-token fallback under the §7
cancel/quarantine discipline OR a `KL_DATAGRAM_UNSUPPORTED` rejection** for source-pinned/TOS sends
(the DNS path uses neither — it rides `post_dgram_send`).

---

## 3. One unified provider (stream + datagram), one handle space

`KlEventCtx.sockets` is a **single** `KlSocketProvider`. A UEFI HTTP client that resolves via the stock
`dns_resolver.c` uses **UDP (DNS) and TCP (HTTP) on the same ctx** → the same provider must serve BOTH
`SOCK_STREAM` (EFI_TCP4 child) and `SOCK_DGRAM` (EFI_UDP4 child). So 6.4b **extends the existing EFI
provider into a unified stream+datagram provider**, not a second standalone provider:

- `capabilities |= KL_SOCK_CAP_DATAGRAM`, and `provider.dgram = &efi_udp4_dgram_ops`.
- `efi_sock_socket(domain, type, proto)` branches on `type`: `SOCK_STREAM` → EFI_TCP4 child (today);
  `SOCK_DGRAM` → EFI_UDP4 child (new).
- `bind/close/get_local_addr/set_cloexec/set_nonblocking` branch on the slot's transport kind.

**FROZEN #2:** the EFI provider is unified over one handle space; `socket()` dispatches child creation
by `type`; per-transport branches key off the slot's recorded transport kind (§4).

---

## 4. Handle discrimination (the primary design point)

### 4.1 The requirement
A pointer-width `KlSocketHandle` returned by the unified provider must be **self-identifying** as TCP vs
UDP, must survive close+slot-reuse without a UAF, and a late completion holding an old handle must be
**rejected** — never misinterpreted as the other transport nor as a re-used live socket.

### 4.2 Mechanism — tagged slot handle + magic + generation (mirrors EFI_TCP4, extended)
Keep the EFI_TCP4 fixed-slot model (`integrations/uefi/socket_efi_tcp4.c`: `g_conns[]`, `handle =
slot+1`, `magic == KL_EFI_CONN_MAGIC`, even/odd `generation`, `quarantined`). Extend it with an explicit
**transport tag in the handle** and a **per-slot transport kind**, so TCP and UDP handle values are
**disjoint** and each is validated against the correct pool:

```
KlSocketHandle layout (pointer-width, low bits carry the slot):

    [ TRANSPORT TAG bits ] [ slot_index + 1 ]

  TCP handle  = (KL_EFI_H_TCP << KL_EFI_H_SHIFT) | (tcp_slot + 1)
  UDP handle  = (KL_EFI_H_UDP << KL_EFI_H_SHIFT) | (udp_slot + 1)

  0 stays KL_INVALID_SOCKET (no tag, slot 0 unused).
```

- `KL_EFI_H_SHIFT` is chosen well above the max slot count (e.g. slots ≤ 4096 → shift ≥ 16); the tag
  occupies bits that a valid slot index can never reach. (Pointer-width handle → ample room; the exact
  shift/width is an implementation detail, not frozen here, but the *scheme* is.)
- Two **separate** slot pools with **distinct magics**: `g_tcp_conns[]` (`KL_EFI_CONN_MAGIC`, "TCP4KL")
  and `g_udp_conns[]` (`KL_EFI_UDP_MAGIC`, a distinct "UDP4KL" tag). Distinct storage means a TCP late
  completion can never index into UDP state and vice-versa, even before the tag check.
- Discrimination + validation (both directions read **stable** slot storage — never freed memory):

```c
/* Returns the UDP slot IFF the handle is UDP-tagged, in range, LIVE (magic set, not dead). */
static KlUefiUdp *udp_of(KlSocketHandle fd);
/* Stable-storage variant for the completion stale guard (magic+generation+dead check, no UAF). */
static KlUefiUdp *udp_slot_of(KlSocketHandle fd);
int  kl_uefi_udp_valid_h(KlSocketHandle fd, unsigned long long generation);
```

`udp_of` rejects a handle whose tag is not `KL_EFI_H_UDP` (so a TCP handle passed to a datagram op is a
hard NULL, not a misread).

**What the generation guard does and does NOT cover (review-High correction).** The generation lives in
**slot storage**, and the public handle encodes only `tag ‖ slot+1` — it does **not** carry the
generation. So a handle value is **reused** after close+reopen of the same slot: an arbitrary caller that
holds an *old, closed* handle and calls a datagram op will pass `udp_of()` against the **new** live
socket, because the handle values are identical. The generation guard therefore protects **only** a
**backend op that captured the generation at post time** (`op->generation = kl_uefi_udp_generation_h(fd)`)
— a late completion whose slot was since closed (generation bumped) or reused (different generation) is
rejected via `kl_uefi_udp_valid_h(op->fd, op->generation)`. It does **not**, and is not claimed to, make
an arbitrary stale *caller* handle detectable after reuse.

This is **exactly the existing EFI_TCP4 contract** (`socket_efi_tcp4.c`: `handle = slot+1`, generation in
slot storage only), and matches KEEL's single-threaded, caller-owns-lifetime model: **a caller must not
use a handle after it closed it** (invalid-by-contract, like a `close(2)`'d fd). We deliberately do NOT
widen the handle to embed the generation — it would diverge from the TCP model for no real gain in a
single-threaded pre-boot environment where the caller controls close ordering. If a future need arises,
embedding generation into the handle (`tag ‖ generation ‖ slot+1`) is the extension point, but it is
**out of scope and not frozen**.

### 4.3 Why the completion path does NOT rely on the handle to find the owner
For datagram completions the owner (`KlUdp`) is recovered through the **stable token** `ev->life`
(`kl_dgram_life_target`), never through the handle. The handle (`dg->fd`, captured into the op at post)
is used **only inside the EFI backend** to reach the EFI_UDP4 protocol for Receive/Transmit/Cancel and
to run the generation stale-guard on the **EFI slot** (is this child still the one we posted on?). So
there are two independent, complementary guards:
- **stable token** → is the KEEL owner (`KlUdp`) still alive? (transport-neutral, B.6)
- **tagged handle + generation** → is the EFI child/slot still the one we posted on? (EFI-local)

Both must pass before a completion touches owner state or the EFI child, respectively.

**FROZEN #3:** handle discrimination = tagged handle (transport tag ‖ slot+1) + separate per-transport
slot pools with distinct magics. The tag prevents cross-transport confusion. The even/odd generation
stale-guard (in slot storage, read from stable memory) protects **backend ops that captured the
generation at post** — NOT arbitrary reused caller handles (identical handle values after slot reuse;
same limitation and single-threaded caller-owns-close contract as EFI_TCP4). Datagram completions recover
the KEEL owner via `ev->life` (B.6) and validate the EFI child via the captured-generation guard — the
two guards are independent. Embedding generation into the handle is an out-of-scope extension, not frozen.

---

## 5. EFI_UDP4 lifecycle mapping

Per-datagram **source and destination are native** in EFI_UDP4 (`EFI_UDP4_SESSION_DATA` carries
`SourceAddress/SourcePort` + `DestinationAddress/DestinationPort`), so `ev->peer` (anti-spoof src for
the resolver) and `ev->local` (dest, pktinfo-equivalent) come for free.

| KEEL op | EFI_UDP4 mapping |
|---|---|
| `socket(SOCK_DGRAM)` | `ServiceBinding.CreateChild` → `OpenProtocol(EFI_UDP4_PROTOCOL)`; create the per-op token **events once** here (bare `EVT_TOKEN` type-0, CheckEvent-polled). |
| `dgram->configure` | `EFI_UDP4.Configure(EFI_UDP4_CONFIG_DATA)` **UNCONNECTED**: `UseDefaultAddress=TRUE` (DHCP station), `StationPort=0` (ephemeral), **no** `RemoteAddress` (multi-nameserver → per-datagram dest). Tolerate `EFI_NO_MAPPING` with a bounded Poll+Stall retry (DHCP settle), as `dns_uefi.c` did. |
| `bind(addr)` | `Configure` with an explicit `StationAddress/StationPort` (numeric; the resolver leaves it ephemeral). |
| `get_local_addr` | from the config / `GetModeData` station address+port. |
| `post_dgram_send(data,len,dest)` | `EFI_UDP4.Transmit(EFI_UDP4_IO_TOKEN)` with `EFI_UDP4_TRANSMIT_DATA{ FragmentTable→copied data }` and **`UdpSessionData.DestinationAddress:Port = dest`** (per-datagram dest on the unconnected socket). |
| `post_dgram_recv()` | `EFI_UDP4.Receive(EFI_UDP4_IO_TOKEN)`; firmware fills `Packet.RxData` (firmware-owned fragments + `UdpSession` + `RecycleSignal`). |
| completion detect | `drain`: `udp->Poll(udp)` then `CheckEvent(tok.Event)` for each outstanding Rx/Tx token (no notify callback). |
| recv copy-out (**signalled** token only) | coalesce `RxData.FragmentTable[]` into the **captured inbound buffer** (`op->buf == dg->recv_buf`, bounded by `op->buflen` → set `truncated` if the datagram exceeds it), read src/dest from `RxData.UdpSession`, then **`SignalEvent(RxData.RecycleSignal)`** to return the firmware buffer (do this on any signalled terminal where `Packet.RxData` is non-NULL, incl. aborted-but-signalled). An **unsignalled** token has no valid `Packet` — never inspected. |
| re-arm | `kl_dgram_recv_on_complete` re-posts → a fresh `EFI_UDP4.Receive` token (serial, one in flight). |

**FROZEN #4:** unconnected `Configure` (per-datagram dest via `TxData.UdpSessionData`; src+dest read
from `RxData.UdpSession`); a token's `Packet` union is valid **only after it signals** — on every
**signalled** Rx terminal (normal or aborted-and-signalled) with non-NULL `RxData`, copy out then
`SignalEvent(RxData.RecycleSignal)`; an **unsignalled** (quarantined) token's `Packet` is never inspected
and leaks with the op until EBS (see §7).

---

## 6. B.6 stable-token integration (post / transfer / release)

The EFI datagram ops obey the exact contract the other four backends do (`src/datagram_life.h`,
`docs/datagram_contract.md` §6). Per posted op (a fixed-slot `EfiDgramOp`, no heap in the hot path —
inline recv/send buffers like `EfiIoOp`):

1. **At post** — copy `dg->fd`, `dg->recv_buf`/`recv_buf_size` (recv) or the payload+dest (send), and
   capture `op->generation = kl_uefi_udp_generation_h(dg->fd)`. Then `op->life = (KlDgramLife*)dg->rx_life;
   kl_dgram_life_retain(op->life);` **after** the last fallible step (so a post-failure unwind that frees
   the op does not release a ref it never took). Never dereference `dg`/`KlUdp` again.
2. **On completion (drain)** — validate `kl_uefi_udp_valid_h(op->fd, op->generation)` (EFI child still
   ours); populate the event; **transfer** `ev->life = op->life; op->life = NULL;` then retire the op.
   `kl_udp_comp_dispatch` recovers the owner via `ev->life` (NULL owner ⇒ delivery dropped) and releases
   the ref after dispatch.
3. **On any drop-without-event** (post-failure unwind, stale-drop, loop teardown) — the op-free path
   `kl_dgram_life_release(op->life)` releases a still-held ref (op carries `life==NULL` after a transfer,
   so no double release).

Event fields for `KL_COMP_DGRAM_RECV`: `.life`(transferred) `.ok` `.bytes` `.buf`(op->buf)
`.peer`(RxData src→neutral) `.local`(RxData dest→neutral) `.truncated`(datagram>buflen) `.gro_seg=0`.
For `KL_COMP_DGRAM_SEND`: `.life` `.ok` `.bytes`.

**FROZEN #5:** the EFI datagram op is a fixed-slot, heap-free record that retains one token ref at post,
transfers it to the event on completion, and releases a still-held ref on every drop-without-event path —
identical to the pollcomp/io_uring/IOCP pattern.

---

## 7. Cancellation / quarantine state machine (the second design point)

This mirrors EFI_TCP4's `pump_or_cancel` + `quarantined`-slot discipline, with **one addition unique to
datagrams**: the interaction with the stable token and with firmware-owned `RxData`.

### 7.1 The rule the whole machine protects — and the two distinct lifetimes (review-Medium correction)
> A submitted EFI_UDP4 token has the firmware referencing the storage **directly reachable from the
> token** — the completion **token** itself (firmware writes `Status` + the `Packet` union), the
> **transmit descriptor + payload** (Tx), and, once the firmware *publishes a completion*, the
> firmware-owned **`RxData`** (Rx) — until the token reaches ONE terminal state (completed **or**
> cancelled-and-drained). That storage MUST NOT be freed or reused before then.

Two **distinct** lifetimes must not be conflated (the earlier draft wrongly said firmware writes the KEEL
inbound slot):

1. **Firmware-reachable EFI storage** — the token, its `EFI_EVENT`, the op record holding them, and any
   Tx descriptor/payload. This is what **quarantine pins** (leaked to EBS) when a cancel can't be
   confirmed. `EFI_UDP4.Receive` does **not** write the KEEL inbound slot (`dg->recv_buf`); it writes
   `Packet.RxData` into the token and owns the **fragment buffers**. KEEL copies fragments into
   `dg->recv_buf` **only after** a signalled completion. So **before completion the inbound slot is not
   firmware-reachable.**
2. **The B.6 receive stable-token reference** — retained on an unconfirmed-cancel recv op **because
   retirement is unconfirmed** (the op has not genuinely retired, so per B.6 its `life` ref must not be
   released), **not** because firmware writes `dg->recv_buf`. Keeping the ref (never letting the refcount
   reach zero) is the correct conservative rule: it prevents `on_final` from freeing the inbound
   slot/machine while an op the firmware might still complete is logically outstanding.

Corollary: an **unsignalled** token has **no valid `Packet` union** — do NOT inspect or recycle
`Packet.RxData` (or read `Packet.TxData`) of a token that never signalled (the quarantine path). `RxData`
+ `RecycleSignal` exist only once firmware has published them through a **signalled** completion.

### 7.1a Operation identity — the completion primitives select the EXACT posted op (review-High)
The completion backend polls an op that was posted EARLIER; between post and poll the slot may have been
**closed** (dead) or **closed-and-REUSED** (a new socket at the same handle value). Resolving the op
through the live handle alone (`udp_of(fd)`) is therefore **unsafe**: a dead slot skips the op (its
firmware buffer never returned), and a reused slot lets the poll touch the **new** socket's token
(cross-generation confusion). So the primitives take the **generation captured at post**
(`kl_uefi_udp_generation_h` after `post_recv`/`post_send`) and resolve the exact record via **stable slot
storage + a generation match** — never `udp_of(fd)`:
- **generation matches** → the live posted op → poll/copy/recycle as below;
- **generation differs** (closed / reused / quarantined) → the op is **stale**: it was already
  **reaped+recycled at close** (single-threaded — `kl_uefi_udp_close` runs its `Cancel`+drain, which
  recycles a signalled `RxData`, *before* any reuse; a quarantined slot is never reused), so the poll
  returns a **terminal-DROP without touching the (possibly reused) token**.

So for EFI the "signalled stale-child recycle" of §7.4 is realized **at close** (the synchronous reap
point), and a stale-generation poll performs no token access. This is the concrete EFI form of the B.6
"never resolve a stale completion through the object" rule.

**Result must distinguish retired-vs-quarantined (review-High #2).** A stale generation alone is not
enough: after an **unconfirmed** close the slot is `dead + quarantined + generation-bumped`, so a naive
"terminal-drop" would look identical to a **cleanly-retired** stale op — and the event layer would then
release the op's B.6 `KlDgramLife` ref, which a quarantined op must **retain forever** (retirement never
confirmed). So the primitives return an explicit `KlUefiUdpOpResult`:
`PENDING` · `DELIVERED` · `RETIRED` · `STALE_RETIRED` · `QUARANTINED` · `INVALID`. A quarantined slot is
never reused, so its one quarantined op is identified from **stable storage** by `slot.quarantined &&
captured_gen == slot.generation − 1` (close bumps the generation exactly once). The event-layer contract:
- `DELIVERED` → emit the completion, transfer `life`;
- `RETIRED` / `STALE_RETIRED` → drop the op, **release** `life` (→ `on_final` eventually runs);
- `QUARANTINED` → remove from active polling, **never release** `life` (→ `on_final` never runs);
- `INVALID` → fail safe (drop from polling; do NOT treat as a confirmed retirement);
- `PENDING` → keep polling.
`poll_recv`/`poll_send` return {PENDING, DELIVERED, STALE_RETIRED, QUARANTINED, INVALID}; `cancel_recv`/
`cancel_send` return {RETIRED, STALE_RETIRED, QUARANTINED, INVALID} (a quarantined op is **never** reported
as RETIRED); `kl_uefi_udp_op_state` is the side-effect-free query. Increment 3's on_final test asserts a
confirmed/stale drop eventually runs `on_final`, and a **quarantined Rx and Tx op do NOT**.

### 7.2 States (per datagram op)
- **POSTED** — token submitted, event not yet signalled; op holds one `life` ref.
- **COMPLETED** — CheckEvent fired (signalled). The op **always emits** the `KL_COMP_DGRAM_*` event and
  **transfers** `life` (`ev->life = op->life; op->life = NULL`); RxData recycled. The backend does **not**
  inspect owner liveness — generic `kl_udp_comp_dispatch` recovers the owner via `ev->life` and **drops
  the delivery if the owner is dead**, then releases the ref. (The backend's only *no-event* drops are the
  stale-EFI-child validation and teardown/quarantine below.)
- **CANCEL-DRAINING** — close/teardown called `Cancel(token)`; polling CheckEvent for the (EFI_ABORTED)
  signal within a bounded spin budget.
- **RETIRED** — cancel-drain confirmed the signal → recycle any RxData, release the op's `life` ref.
- **QUARANTINED** — cancel-drain could NOT confirm within budget → the token **never signalled**, so the
  firmware may still own the token / event / op record / Tx descriptor+payload **forever** (its `Packet`
  union is NOT valid — do not inspect it). The op (and its inline buffers) is **leaked**, its `life` ref
  is **never released** — for **both Rx and Tx** ops, because B.6 releases only on *genuine* retirement
  and this op's retirement is unconfirmed. For an **Rx** op that pinned ref also keeps `on_final` from
  freeing the inbound slot + receive machine (a token that might still complete must not have its machine
  freed). The EFI child/events are **never** CloseEvent'd / DestroyChild'd, and the provider
  **fail-closes** further datagram I/O on that socket. Leaked until EBS.

### 7.3 Transitions

| From | Trigger | Action | To |
|---|---|---|---|
| POSTED | `CheckEvent`=SUCCESS (drain), EFI child still valid | copy-out+recycle RxData; **always** emit event (`ev->life=op->life; op->life=NULL`); generic dispatch drops delivery if owner dead + re-arms | COMPLETED |
| POSTED | `CheckEvent`=SUCCESS but `kl_uefi_udp_valid_h` fails (stale child) | **signalled → `Packet` is valid**: for **Rx**, if `Packet.RxData` non-NULL `SignalEvent(RxData.RecycleSignal)` (do **not** copy into the stale owner buffer); (Tx has nothing to recycle); release `op->life`; free op — **no event** | RETIRED (no-event) |
| POSTED | socket close / owner `kl_udp_free` while token outstanding | `Cancel(token)` | CANCEL-DRAINING |
| CANCEL-DRAINING | `CheckEvent`=SUCCESS within budget | recycle RxData if present; `kl_dgram_life_release(op->life)`; free op slot | RETIRED |
| CANCEL-DRAINING | budget exhausted, no signal | mark op+slot `quarantined`; **do NOT** release `life`, CloseEvent, or DestroyChild; fail-close the socket | QUARANTINED |
| any | after ExitBootServices (`kl_uefi_after_ebs()`) | touch NO boot service; mark dead, leave storage | (EBS) |

### 7.4 Interaction points nailed down
- **`kl_udp_free` with an outstanding EFI recv token:** `udp.c` marks the token dead + drops the owner
  ref (it does **not** force-retire the physical op — B.6). The EFI provider's **socket close**
  (`efi_sock_close` for the UDP slot, invoked when `udp.c` calls `kl_sock_close`) is where `Cancel`+drain
  runs. If drain confirms retirement → the op releases its `life` ref (possibly the final release →
  `on_final` frees the inbound slot/machine — safe, the op is genuinely retired). If drain fails →
  **quarantine**: the op keeps its `life` ref forever, so `on_final` never runs and the inbound
  slot/machine stays pinned — **because retirement is unconfirmed** (a token that may still complete must
  not have its receive machine freed), and separately the firmware-reachable EFI storage
  (token/event/op/Tx payload) is leaked. Ordering: the socket-close cancel-drain reconciles the token
  **before** any buffer is eligible to be freed.
- **RecycleSignal vs signalling:** `RxData` (and thus `RecycleSignal`) exists **only after a signalled
  completion**. On a **signalled** Rx terminal — normal completion OR a cancel-drain that observed the
  `EFI_ABORTED` signal — read `Packet.RxData`; if non-NULL, copy out (on the clean path) and **always**
  `SignalEvent(RxData.RecycleSignal)` to return the firmware buffer (even on the aborted-but-signalled
  path, exactly as `dns_uefi.c` did). On the **quarantine** path the token **never signalled**, so there
  is no published `RxData` to recycle — do **not** touch `Packet` at all; it leaks with the op until EBS.
- **Stale-child but SIGNALLED (review correction):** "signalled" and "captured generation still valid"
  are **independent**. A drain may observe `CheckEvent`=SUCCESS (the token IS signalled, `Packet` valid)
  yet find `kl_uefi_udp_valid_h(op->fd, op->generation)` false because the EFI child was closed/reused
  since post. This is a **no-event** drop (the owner it targeted is gone), but the firmware buffer is
  real: for an **Rx** op, `SignalEvent(RxData.RecycleSignal)` when `Packet.RxData` is non-NULL (do NOT
  copy into the now-stale owner buffer), then release `op->life` and retire the op **without emitting**.
  Skipping the recycle here would leak the firmware buffer. (Tx has no firmware buffer to recycle.) This
  differs from quarantine, where the token is **un**signalled and `Packet` must not be touched.
  **EFI realization (§7.1a):** for this provider the reap+recycle happens synchronously at
  `kl_uefi_udp_close` (which runs before any slot reuse), and the completion primitives select the exact
  op by **captured generation over stable storage** — so a stale-generation drain poll performs **no token
  access at all** (the token was already reaped). The generic drain-time recycle above is the abstract
  form; §7.1a is how EFI satisfies it without ever resolving a stale op through `udp_of(fd)`.
- **Fail-close scope:** a quarantine fail-closes **that datagram socket** (`dead=1`), and — matching
  the seed `dns_uefi.c`'s `g_dns_quarantined` — a provider-level "a firmware that can't cancel is unusable" latch
  may fail-close further datagram sockets. (Whether the latch is per-socket or provider-wide is an
  implementation choice; **frozen**: at minimum per-socket fail-close + no reuse of a quarantined slot.)
- **EBS:** every datagram op (`socket/configure/send/recv/close`) checks `kl_uefi_after_ebs()` and is
  fail-closed; `kl_uefi_udp_provider_live_count()` (new, mirrors the TCP live-count) must be 0 before
  `kl_uefi_shutdown()`.

**FROZEN #6:** the cancel/quarantine state machine above, with the **token-ref rule**: a **quarantined op
of EITHER kind never releases its stable-token ref** because **retirement is unconfirmed** (B.6 releases
only on genuine retirement) — for an Rx op that also keeps `on_final` from freeing the receive machine
while a possibly-still-live op is outstanding; quarantine separately pins the firmware-reachable EFI
storage. An **unsignalled** token's `Packet` is never inspected. A **signalled** token's `Packet` IS valid
even if the captured child generation is now stale — the **stale-child no-event drop** must still
`SignalEvent(RxData.RecycleSignal)` for a non-NULL Rx buffer (no copy into the stale owner), then release
`life` and retire without an event. Confirmed cancel-drain releases normally; EBS is fail-closed
everywhere.

---

## 8. Poll / drive integration

`event_efi.c`'s `drain` gains a pass over outstanding datagram ops: `udp->Poll(udp)` on each live UDP
child, `CheckEvent` each POSTED Rx/Tx token, and surface `KL_COMP_DGRAM_RECV/_SEND` (bounded by the
`drain` `max`, interleaved with the existing connect/watcher/server-IO passes so timers and other ops are
not starved). Serial receive (one Rx token in flight) matches the recv machine's contract; re-arm posts
the next `Receive` from `kl_dgram_recv_on_complete`.

---

## 9. Testability & acceptance (before calling 6.4b done)

Two layers, matching the KlStream/UEFI coverage bar (host-mock ≠ real firmware, both required):

1. **Host mock-EFI unit test** (extend `integrations/uefi/mock_efi_test.c` / the U-2/U-3 mock harness):
   drive the datagram op state machine on the host under ASan/UBSan — post recv, inject a mock RxData
   (src+dest+payload), assert copy-out+RecycleSignal+event+token-transfer; post send, assert Transmit
   descriptor+dest; **cancel/quarantine**: force `CheckEvent` to never signal → assert `Cancel` called,
   budget exhausts, slot quarantined, `life` ref **retained** (inbound storage NOT freed — the mock
   allocator's live count proves it), fail-close. This is where the two emphasized mechanisms
   (discrimination + quarantine) get deterministic coverage the way `dgram_recv_classify` did for IOCP.
2. **QEMU/OVMF end-to-end (6.4c — DONE):** a U-5-derived harness (`run_dgram_dns.sh`: python DNS
   server on :53, SLIRP 10.0.2.2) with the **stock `dns_resolver.c` over `KlUdp`-over-EFI_UDP4** →
   A/AAAA → HTTP GET 200 + a truncation (TC) case;
   plus a truncated-response case exercising the freestanding TC branch (6.4a-2) on real firmware;
   assert `kl_uefi_udp_provider_live_count()==0` at teardown (clean, no quarantine on the happy path).

---

## 10. Open questions to resolve during implementation (not blocking the freeze)

- **Handle tag width / max UDP slots** — pick `KL_EFI_H_SHIFT` and `KL_EFI_MAX_UDP` (small, e.g. 8–16
  UDP children; DNS needs 1–2). Concrete values are an implementation detail.
- **Quarantine latch scope** — per-socket (frozen minimum) vs provider-wide `g_udp_quarantined` (as
  `dns_uefi.c` did). Lean provider-wide for datagrams (a firmware that can't cancel one UDP token is
  suspect for all), but confirm it doesn't wedge a mixed TCP+UDP process's TCP path (it must not — the
  latch is datagram-scoped).
- **`Configure` DHCP-settle budget** — reuse the seed `dns_uefi.c`'s `EFI_NO_MAPPING` Poll+Stall retry bounds.
- **AAAA over EFI_UDP4** — EFI_UDP4 is IPv4-only; AAAA queries still go out over UDP4 to the nameserver
  (the *transport* is v4, the *record* is v6) — no EFI_UDP6 needed for the resolver. `dns_is_literal`
  v6 shortcuts and v6 answers are unaffected (they never open a v6 socket).
- **`dgram->send` fallback shape** — FROZEN #1 fixes `dgram->recv = NULL` and requires `dgram->send` (if
  provided) to use an independent Tx token under §7. The only open choice is provide-vs-reject for
  source-pinned/TOS sends: implement the sync independent-Tx-token `Transmit` (cheap, mirrored the seed
  `dns_uefi.c`) **or** return `KL_DATAGRAM_UNSUPPORTED`. Recommendation: implement it (small, and keeps a
  general `KlUdp` source-pinned send working); revisit only if it complicates the quarantine latch.

---

## 11. Frozen decisions (summary)

1. **Completion-native** EFI datagram I/O; `post_dgram_recv/_send` in `event_efi.c`; `dgram->configure`
   mandatory; **`dgram->recv = NULL`** (no second receive machine); **`dgram->send`** = a sync
   independent-Tx-token fallback under the §7 discipline **or** `KL_DATAGRAM_UNSUPPORTED` for
   source-pin/TOS (DNS uses neither — it rides `post_dgram_send`).
2. **Unified** EFI provider (stream+datagram) on one handle space; `socket()` dispatches child by `type`.
3. **Handle discrimination** = tagged handle (transport tag ‖ slot+1) + separate per-transport pools with
   distinct magics. Tag prevents cross-transport confusion; the generation stale-guard (slot storage)
   protects **captured-at-post backend ops**, NOT arbitrary reused caller handles (same as EFI_TCP4;
   caller-owns-close contract). Owner recovered via `ev->life`; EFI child validated via the
   captured-generation guard (two independent guards). Generation-in-handle is out of scope.
4. **Unconnected `Configure`**; per-datagram dest via `TxData.UdpSessionData`; src+dest from
   `RxData.UdpSession`; a token's `Packet` is valid **only after it signals** — recycle `RxData` via
   `RecycleSignal` on every **signalled** Rx terminal (incl. aborted-and-signalled); an **unsignalled**
   (quarantined) token's `Packet` is never inspected (leaks until EBS).
5. **B.6 stable-token** obligations identical to the other four backends (retain-at-post,
   transfer-to-event, release-on-drop).
6. **Cancel/quarantine state machine** (§7) with the token-ref rule: a **quarantined op of EITHER kind
   never releases its stable-token ref** — because **retirement is unconfirmed** (B.6: release only on
   genuine retirement); for an Rx op that also keeps `on_final` from freeing the inbound slot/machine
   while a possibly-still-live op is outstanding. Quarantine pins the **firmware-reachable EFI storage**
   (token/event/op/Tx payload); the inbound slot is NOT firmware-written before completion. An
   **unsignalled** token's `Packet` is never inspected/recycled; a **signalled** token's `Packet` is valid
   even if the captured child generation is stale, so the **stale-child no-event drop still recycles**
   `RxData.RecycleSignal` (Rx, non-NULL) before releasing `life`. On a **signalled + valid-child**
   completion the backend always emits+transfers `life` (owner-dead drop is generic dispatch's job).
   Confirmed cancel-drain releases normally; **EBS fail-closed** everywhere; live-count 0 before shutdown.
7. **Acceptance** = host mock-EFI unit test (state machine incl. quarantine) **and** QEMU/OVMF e2e (6.4c);
   the `dns_uefi.c` retirement is DONE (R-1 `91d50fa` / R-2 `81e3188`, 2026-08); `KlDgramClose`/Step-7 is not claimed here.
