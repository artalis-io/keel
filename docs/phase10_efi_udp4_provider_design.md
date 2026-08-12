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
- `integrations/uefi/dns_uefi.c` — the bespoke one-shot EFI_UDP4 DNS client: the concrete
  EFI_UDP4 API pattern (Configure / Receive+RecycleSignal / Transmit / `u5_pump_or_cancel` /
  quarantine). **6.4b generalizes this; the separate `dns_uefi.c` retirement is a later step.**
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
HTTP GET 200); the `dns_uefi.c` retirement; EFI_UDP6; multicast/GSO/GRO/TOS datagram knobs (EFI_UDP4
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

This is the correct model: it uses the real EFI_UDP4 asynchronous **token** machine (as `dns_uefi.c`
already does) and slots directly into the B.6 stable-token contract the other completion backends satisfy.

`kl_udp_init` still calls the **socket provider** for `socket()/set_cloexec/set_nonblocking/bind/
get_local_addr` and `dgram->configure` regardless of readiness/completion. So the datagram vtable's
**`configure` is mandatory**; **`dgram->send`/`dgram->recv` are only reached on a readiness/source-pinned/
TOS send path** — provide them as **synchronous EFI fallbacks** (Transmit/Receive + `pump_or_cancel`,
like `dns_uefi.c`) for robustness and readiness-portability, but they are **not** the DNS hot path.

**FROZEN #1:** EFI datagram I/O is completion-native — `post_dgram_recv/_send` in `event_efi.c` over
EFI_UDP4 Rx/Tx tokens; `dgram->configure` mandatory; `dgram->send/recv` sync fallbacks only.

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
hard NULL, not a misread). The generation guard is identical in spirit to `kl_uefi_conn_valid_h`: even
generation = closed, different generation = slot reused → reject.

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
slot pools with distinct magics + even/odd generation stale-guard read from stable storage. Datagram
completions recover the KEEL owner via `ev->life`, and validate the EFI child via the tagged-handle
generation guard — the two guards are independent.

---

## 5. EFI_UDP4 lifecycle mapping

Per-datagram **source and destination are native** in EFI_UDP4 (`EFI_UDP4_SESSION_DATA` carries
`SourceAddress/SourcePort` + `DestinationAddress/DestinationPort`), so `ev->peer` (anti-spoof src for
the resolver) and `ev->local` (dest, pktinfo-equivalent) come for free.

| KEEL op | EFI_UDP4 mapping |
|---|---|
| `socket(SOCK_DGRAM)` | `ServiceBinding.CreateChild` → `OpenProtocol(EFI_UDP4_PROTOCOL)`; create the per-op token **events once** here (bare `EVT_TOKEN` type-0, CheckEvent-polled). |
| `dgram->configure` | `EFI_UDP4.Configure(EFI_UDP4_CONFIG_DATA)` **UNCONNECTED**: `UseDefaultAddress=TRUE` (DHCP station), `StationPort=0` (ephemeral), **no** `RemoteAddress` (multi-nameserver → per-datagram dest). Tolerate `EFI_NO_MAPPING` with a bounded Poll+Stall retry (DHCP settle), as `dns_uefi.c` does. |
| `bind(addr)` | `Configure` with an explicit `StationAddress/StationPort` (numeric; the resolver leaves it ephemeral). |
| `get_local_addr` | from the config / `GetModeData` station address+port. |
| `post_dgram_send(data,len,dest)` | `EFI_UDP4.Transmit(EFI_UDP4_IO_TOKEN)` with `EFI_UDP4_TRANSMIT_DATA{ FragmentTable→copied data }` and **`UdpSessionData.DestinationAddress:Port = dest`** (per-datagram dest on the unconnected socket). |
| `post_dgram_recv()` | `EFI_UDP4.Receive(EFI_UDP4_IO_TOKEN)`; firmware fills `Packet.RxData` (firmware-owned fragments + `UdpSession` + `RecycleSignal`). |
| completion detect | `drain`: `udp->Poll(udp)` then `CheckEvent(tok.Event)` for each outstanding Rx/Tx token (no notify callback). |
| recv copy-out | coalesce `RxData.FragmentTable[]` into the **captured inbound buffer** (`op->buf == dg->recv_buf`, bounded by `op->buflen` → set `truncated` if the datagram exceeds it), read src/dest from `RxData.UdpSession`, then **`SignalEvent(RxData.RecycleSignal)`** — always, even on the abort path (return the firmware buffer). |
| re-arm | `kl_dgram_recv_on_complete` re-posts → a fresh `EFI_UDP4.Receive` token (serial, one in flight). |

**FROZEN #4:** unconnected `Configure` (per-datagram dest via `TxData.UdpSessionData`; src+dest read
from `RxData.UdpSession`); **`RecycleSignal` is signalled on every Rx completion including the
aborted/cancelled path** (return the firmware-owned buffer or leak it under quarantine — see §7).

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

### 7.1 The rule the whole machine protects
> A submitted EFI_UDP4 token has the firmware referencing **everything reachable from it** — the token
> (Status/Packet written by firmware), the transmit descriptor+payload, and (Rx) the firmware-owned
> `RxData` — until the token reaches ONE terminal state (completed **or** cancelled-and-drained). Its
> backing storage MUST NOT be freed or reused before that. For a **receive** op, that backing storage
> includes the **inbound slot** (`dg->recv_buf`), which the **stable token's `on_final` owns**. Therefore
> an unconfirmed-cancel recv op MUST NOT allow the token refcount to reach zero — else `on_final` frees a
> buffer the firmware may still write.

### 7.2 States (per datagram op)
- **POSTED** — token submitted, event not yet signalled; op holds one `life` ref.
- **COMPLETED** — CheckEvent fired; event emitted (`life` transferred) or dropped (owner dead) — but in
  both cases the op releases/transfers its ref; RxData recycled.
- **CANCEL-DRAINING** — close/teardown called `Cancel(token)`; polling CheckEvent for the (EFI_ABORTED)
  signal within a bounded spin budget.
- **RETIRED** — cancel-drain confirmed the signal → recycle any RxData, release the op's `life` ref.
- **QUARANTINED** — cancel-drain could NOT confirm within budget → firmware may still own the token /
  descriptor / RxData / the inbound buffer **forever**. The op (and its inline buffers) is **leaked**,
  its `life` ref is **never released** (so the token never hits its final release → the inbound slot +
  receive machine are pinned, never freed — safe), the EFI child/events are **never** CloseEvent'd /
  DestroyChild'd, and the provider **fail-closes** further datagram I/O on that socket. Leaked until EBS.

### 7.3 Transitions

| From | Trigger | Action | To |
|---|---|---|---|
| POSTED | `CheckEvent`=SUCCESS (drain) | copy-out+recycle RxData; emit event (`ev->life=op->life; op->life=NULL`) or drop if owner dead; re-arm recv | COMPLETED |
| POSTED | socket close / owner `kl_udp_free` while token outstanding | `Cancel(token)` | CANCEL-DRAINING |
| CANCEL-DRAINING | `CheckEvent`=SUCCESS within budget | recycle RxData if present; `kl_dgram_life_release(op->life)`; free op slot | RETIRED |
| CANCEL-DRAINING | budget exhausted, no signal | mark op+slot `quarantined`; **do NOT** release `life`, CloseEvent, or DestroyChild; fail-close the socket | QUARANTINED |
| any | after ExitBootServices (`kl_uefi_after_ebs()`) | touch NO boot service; mark dead, leave storage | (EBS) |

### 7.4 Interaction points nailed down
- **`kl_udp_free` with an outstanding EFI recv token:** `udp.c` marks the token dead + drops the owner
  ref (it does **not** force-retire the physical op — B.6). The EFI provider's **socket close**
  (`efi_sock_close` for the UDP slot, invoked when `udp.c` calls `kl_sock_close`) is where `Cancel`+drain
  runs. If drain confirms → the op releases its `life` ref (possibly the final release → `on_final` frees
  the inbound slot, now safe). If drain fails → **quarantine**: the op keeps its `life` ref forever, so
  `on_final` never runs and the inbound slot is pinned (safe against firmware writes). Ordering: the
  socket-close cancel-drain reconciles the token **before** any buffer is eligible to be freed.
- **RecycleSignal under quarantine:** if a quarantined Rx op still holds a firmware `RxData` we can't
  prove is retired, we do **not** SignalEvent it (the firmware may already be mid-write) — it leaks with
  the rest until EBS. On the confirmed cancel-drain path, recycle it normally.
- **Fail-close scope:** a quarantine fail-closes **that datagram socket** (`dead=1`), and — matching
  `dns_uefi.c`'s `g_dns_quarantined` — a provider-level "a firmware that can't cancel is unusable" latch
  may fail-close further datagram sockets. (Whether the latch is per-socket or provider-wide is an
  implementation choice; **frozen**: at minimum per-socket fail-close + no reuse of a quarantined slot.)
- **EBS:** every datagram op (`socket/configure/send/recv/close`) checks `kl_uefi_after_ebs()` and is
  fail-closed; `kl_uefi_udp_provider_live_count()` (new, mirrors the TCP live-count) must be 0 before
  `kl_uefi_shutdown()`.

**FROZEN #6:** the cancel/quarantine state machine above, with the **token-ref rule**: a quarantined
receive op **never releases its stable-token ref** (pins the inbound storage, so `on_final` cannot free a
firmware-owned buffer); confirmed cancel-drain releases normally; EBS is fail-closed everywhere.

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
2. **QEMU/OVMF end-to-end (6.4c, next step):** the U-5 harness (python DNS server on :53, SLIRP
   10.0.2.2) with the **stock `dns_resolver.c` over `KlUdp`-over-EFI_UDP4** → A/AAAA → HTTPS GET 200;
   plus a truncated-response case exercising the freestanding TC branch (6.4a-2) on real firmware;
   assert `kl_uefi_udp_provider_live_count()==0` at teardown (clean, no quarantine on the happy path).

---

## 10. Open questions to resolve during implementation (not blocking the freeze)

- **Handle tag width / max UDP slots** — pick `KL_EFI_H_SHIFT` and `KL_EFI_MAX_UDP` (small, e.g. 8–16
  UDP children; DNS needs 1–2). Concrete values are an implementation detail.
- **Quarantine latch scope** — per-socket (frozen minimum) vs provider-wide `g_udp_quarantined` (as
  `dns_uefi.c` does). Lean provider-wide for datagrams (a firmware that can't cancel one UDP token is
  suspect for all), but confirm it doesn't wedge a mixed TCP+UDP process's TCP path (it must not — the
  latch is datagram-scoped).
- **`Configure` DHCP-settle budget** — reuse `dns_uefi.c`'s `EFI_NO_MAPPING` Poll+Stall retry bounds.
- **AAAA over EFI_UDP4** — EFI_UDP4 is IPv4-only; AAAA queries still go out over UDP4 to the nameserver
  (the *transport* is v4, the *record* is v6) — no EFI_UDP6 needed for the resolver. `dns_is_literal`
  v6 shortcuts and v6 answers are unaffected (they never open a v6 socket).
- **Sync `dgram->send/recv` fallbacks** — provide them (Transmit/Receive + `pump_or_cancel`) for the
  source-pinned/TOS send path, or leave NULL and document that EFI datagram send requires the completion
  path. Lean: provide the sync send fallback (cheap, mirrors `dns_uefi.c`); recv fallback optional.

---

## 11. Frozen decisions (summary)

1. **Completion-native** EFI datagram I/O; `post_dgram_recv/_send` in `event_efi.c`; `dgram->configure`
   mandatory, `dgram->send/recv` sync fallbacks only.
2. **Unified** EFI provider (stream+datagram) on one handle space; `socket()` dispatches child by `type`.
3. **Handle discrimination** = tagged handle (transport tag ‖ slot+1) + separate per-transport pools with
   distinct magics + even/odd generation stale-guard from stable storage; owner recovered via `ev->life`,
   EFI child validated via the tagged-handle generation guard (two independent guards).
4. **Unconnected `Configure`**; per-datagram dest via `TxData.UdpSessionData`; src+dest from
   `RxData.UdpSession`; **`RecycleSignal` on every Rx completion incl. the abort path** (or leaked under
   quarantine).
5. **B.6 stable-token** obligations identical to the other four backends (retain-at-post,
   transfer-to-event, release-on-drop).
6. **Cancel/quarantine state machine** (§7) with the token-ref rule: a **quarantined receive op never
   releases its token ref** (pins the inbound storage so `on_final` can't free a firmware-owned buffer);
   confirmed cancel-drain releases normally; **EBS fail-closed** everywhere; live-count 0 before shutdown.
7. **Acceptance** = host mock-EFI unit test (state machine incl. quarantine) **and** QEMU/OVMF e2e (6.4c);
   the `dns_uefi.c` retirement is a separate later step; `KlDgramClose`/Step-7 is not claimed here.
