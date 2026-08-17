# KlUdp → KlDatagram consolidation — design freeze (inventory + decisions)

**Status:** design freeze. **Inventory and decisions only — no production code.** Each migration below
is a separately reviewable increment, gated by its own review before code. Positioning context:
[datagram_vs_udp.md](datagram_vs_udp.md). Reframed direction (R2): `KlUdp` is required today;
consolidation is a recognized future objective, and this document is its design.

## 0. Goal + the de-risking finding

Reduce the two public datagram APIs to one canonical Tier-1 transport (`KlDatagram`) plus an explicit
**extended-UDP layer**, migrate the two consumers (DNS, `KlUdpServer`), and reduce `KlUdp` to a
compatibility wrapper (optionally deprecated later) — **without** rebuilding a monolithic `KlUdp`
inside the `KlDatagram` facade.

**Key finding that de-risks this:** the internal provider seam **already does most of the work.** The
`KlDatagramOps` vtable (`include/keel/socket_dgram.h`) already declares `send`/`recv`/`send_gso`/
`configure`/`set_tos`/`mcast_membership`/`rx_batch_new`/`recv_batch`/`send_batch`, and the POSIX
provider (`socket_dgram_posix.c`) **implements all of them** (Linux offload gated). Source-pin and
per-packet TOS are already carried end-to-end (`KlDatagramMessage.local` / `.tos`, `KlDgramTxDesc`,
caps `KL_DGRAM_CAP_SOURCE_PIN` / `KL_DGRAM_CAP_TOS`). So consolidation is mostly **wiring existing
provider capability up through the facade + adding a queue-policy option**, not new backend work.

The target layering (consumers never see backend internals — invariant I10):

```
DNS  /  KlUdpServer  /  future protocols (mDNS, CoAP, QUIC base)
                         │
        extended-UDP policy + optional capabilities
        (byte/both queue policy, multicast join/leave, batching/GSO/GRO opt-in)
                         │
        KlDatagram  — canonical Tier-1 message + lifetime transport
                         │
        KlDatagramOps providers  (POSIX / lwIP / EFI)
```

## 1. Feature inventory — every KlUdp feature × consumer need × provider support × KlDatagram status

Legend — **KlDatagram status:** `core` = already in the facade/message; `provider` = the
`KlDatagramOps` provider implements it but the facade does not expose it; `queue` = a queue-policy
concern; `—` = absent. **Consumers:** D = DNS resolver, S = `KlUdpServer`.

| KlUdp feature | DNS needs | UdpServer needs | Provider support (POSIX / lwIP / EFI) | KlDatagram status |
|---|---|---|---|---|
| unconnected `send_to(dest)` | **yes** | **yes** (reply) | ✓ / ✓ / — | **core** (`msg.peer`) |
| connected send | no | no | ✓ / ✓ / — | **core** (`KL_DGRAM_CAP_CONNECTED`) |
| per-datagram source (peer) on recv | **yes** (anti-spoof) | **yes** (reply dest) | ✓ / ✓ / — | **core** (recv `peer`) |
| local/dest addr capture (`recv_pktinfo`) | no | **yes** (multi-homed reply) | ✓ / ✗ / — | **core** (recv `local` + `KL_DGRAM_HAS_LOCAL`) |
| source-pinned send (`send_to_from`) | no | **yes** (reply pin) | ✓ / ✗ / — | **core** (`msg.local` + `KL_DGRAM_CAP_SOURCE_PIN`) |
| per-packet TOS on send | no | no | ✓ / ✗ / — | **core** (`msg.tos` + `KL_DGRAM_CAP_TOS`) |
| socket-default TOS (`tos`) | no | passthrough | ✓ / ✗ / — | **provider** (`set_tos`; fd-prep) |
| recv TOS delivery (`recv_tos`) | no | passthrough (unread) | ✓ / ✗ / — | **provider** (captured to slot, **not delivered** by `KlDatagramRecvFn`) |
| multicast join/leave + `IP_MULTICAST_*` | no | **yes** (public API) | ✓ / ✓(cond) / — | **provider** (`mcast_membership`; no facade API) |
| broadcast (`SO_BROADCAST`) | no | passthrough | ✓ / ? / — | **provider** (fd-prep) |
| `recvmmsg`/`sendmmsg` batching | no | passthrough (perf only) | ✓(Linux) / ✗ / — | **provider** (`recv_batch`/`send_batch`; facade single-flight) |
| GSO/GRO segmentation offload | no | passthrough (perf only) | ✓(Linux) / ✗ / — | **provider** (`send_gso`; GRO captured, not exposed) |
| byte-budget send queue (`max_send_queue`) | tolerated | passthrough | n/a (queue policy) | **queue** (facade is slot/COUNT only) |
| `reuse_addr/port`, `so_rcvbuf/sndbuf`, bind | yes (basic) | yes | fd-prep (`configure`) | **fd-prep** (caller/helper preps the fd before `kl_datagram_init`) |

**Reading it:** every feature DNS or `KlUdpServer` *load-bears* is already `core` or `provider`. The
only genuinely-missing pieces are: (a) a **byte/both queue policy**, (b) a **facade multicast
join/leave API**, and (c) **provider→caps derivation** so a consumer can discover what a provider
actually supports. Batching/GSO/GRO/broadcast/recv-TOS are passthrough perf/config knobs no consumer
reads through its public API, so they can be provider-level opt-ins without new facade surface.

## 2. DNS behavior-parity + failure-path requirements (the clean first migration)

DNS uses only `kl_udp_init` / `kl_udp_recv_start` / `kl_udp_send_to` / `kl_udp_free`, config
`{ctx, family, alloc}`, **zero extensions**. Parity requirements for a `KlDatagram` port:

- **Datagram boundaries** preserved (one `send` = one query; one recv = one response). `KlDatagram`
  core guarantees this.
- **Multiple concurrent legs on one socket** — dual-family A+AAAA and per-nameserver rotation issue
  several sends before replies arrive; replies are matched by txn-id, not order. `KlDatagram`'s
  fixed-slot send queue must be sized to the **max concurrent in-flight legs** (small, e.g. ≥ 2×
  families × retry fan-out). **Decision D-DNS-1:** size `send_slots` to the resolver's max concurrent
  legs; handle `KL_DATAGRAM_WOULD_BLOCK` (today's byte budget effectively never blocks tiny queries;
  the slot budget must not be tighter than the leg count).
- **Source validation** — `dns_on_recv` rejects any datagram whose `peer` is not a configured
  nameserver. `KlDatagram` recv delivers `peer` — parity holds.
- **No local/TOS/UDP-truncation needs** — DNS uses the payload's TC bit, not the UDP `MSG_TRUNC`
  flag; it never reads `local` or recv-TOS. No caps required (`want_caps = 0`).
- **Per-query timeout/retransmit** are DNS-level timers, transport-agnostic — unchanged.
- **Send failure** → map `KlDatagramSendStatus` (`ERROR`/`WOULD_BLOCK`) to the existing
  send-failure/next-nameserver path.
- **TCP fallback** is already a **separate** `(fd, KlTls)` path (not the UDP socket) — untouched by
  this migration.

Failure-path tests to add with the DNS migration: WOULD_BLOCK under many concurrent legs; send error →
NS rotation; spoofed-source drop; truncation → TCP fallback still triggers; dual-family concurrency;
close/reopen.

## 3. Optional-capability seam (decisions)

`KlDatagram` already has the *negotiation* half: `KlDatagramConfig.want_caps` (consumer requires),
`kl_datagram_caps()` (reports), `KL_DATAGRAM_UNSUPPORTED` (send refused when a message uses an
un-granted cap). What is missing is **provider→caps derivation** and coverage of the extended features.

- **Decision D-CAP-1 (provider reports, facade derives).** Add a capability-report to the provider
  seam (either a new `KlDatagramOps.caps(ctx, fd, family)` or fold it into `configure`'s return) so
  `kl_datagram_caps()` reflects what the provider **actually supports**, not just what the caller
  declared. `want_caps` then becomes a *request* that fails init (not just send) if unsupported —
  fail-loud, no silent emulation. **Unsupported providers report absence; they never emulate
  incorrectly** (lwIP already ignores source-pin/TOS — that must surface as a missing cap, not a
  silent no-op).
- **Decision D-CAP-2 (extended features live above the core, not in it).** Keep the Tier-1 core
  message minimal (peer/local/tos/flags — already there). Batching, GSO/GRO, multicast join/leave,
  and broadcast go in the **extended-UDP layer** (a thin wrapper over `KlDatagram` that talks to the
  provider's already-existing `send_gso`/`recv_batch`/`send_batch`/`mcast_membership`). This avoids
  the monolith trap — the core facade does not grow a knob per UDP feature.
- **Decision D-CAP-3 (multicast is a runtime API).** Join/leave are dynamic (mDNS/SSDP), so expose
  them as runtime calls in the extended layer (e.g. `kl_dgram_ext_multicast_join/leave`) routed to
  the provider's `mcast_membership`, gated by a `KL_DGRAM_CAP_MULTICAST` capability.
- **Open question O-CAP-1 (recv-TOS delivery).** Recv TOS is captured to the inbound slot but not
  delivered by `KlDatagramRecvFn`. No current consumer reads it through a public API (`KlUdpServer`
  sets `recv_tos` but never surfaces it). Options: (a) leave it captured-not-delivered; (b) add a
  post-delivery accessor (like `kl_udp_get_recv_tos`); (c) extend the recv callback (ABI). Recommend
  (a)/(b) — do **not** grow the Tier-1 recv callback for an unread knob. Decide at implementation.

## 4. Preallocated queue policy — slot / byte / both (decisions)

Today `KlDatagram` is **count-only**: a fixed `send_slots × send_slot_cap` array, atomic admission
(`WOULD_BLOCK` when no free slot, `TOO_LARGE` when `len > slot_cap`), single-flight pump, **all
preallocated** at init (no hot-path malloc; overflow-checked sizing). `KlUdp` is **byte-only**
(`max_send_queue`).

- **Decision D-Q-1 (policy enum, preallocated, default unchanged).** Add a bounded-queue *policy* to
  `KlDatagramConfig`: `SLOT` (count — today's default, unchanged), `BYTE` (total queued bytes),
  `BOTH` (admit only if a free slot **and** free bytes). Storage stays fully preallocated
  (`send_slots × send_slot_cap`); the byte cap is an **accounting gate**, not a growing buffer. This
  preserves I8 (allocation-free hot path).
- **Decision D-Q-2 (hook points, already located).** `kl_dgram_slots_init` gains a `byte_budget`;
  `kl_dgram_slots_acquire` gates on `free_slots > 0 && (policy≠BYTE-inclusive || bytes_used + len ≤
  byte_budget)`; `send_retire_head` decrements `bytes_used`. Overflow rules: reject at admission
  (`TOO_LARGE`) if a single datagram exceeds `slot_cap` (unchanged) or, under BYTE/BOTH, exceeds
  `byte_budget`; `WOULD_BLOCK` on transient fullness by either dimension.
- **Decision D-Q-3 (consumer mapping).** DNS: `SLOT` (default) is fine. `KlUdpServer`: `BYTE` or
  `BOTH` to preserve `max_send_queue` semantics — its compat wrapper sets the byte budget from the
  passed-through `max_send_queue`. **No silent reinterpretation** of one budget as the other.

## 5. KlUdpServer migration + compatibility-wrapper requirements

`KlUdpServer` public surface to preserve **exactly** (source + ABI): 8 functions
(`kl_udp_server_{init,reply,multicast_join,multicast_leave,free,local_port,fd,last_error}`), the
17-field `KlUdpServerConfig`, and the handler `KlUdpHandlerFn(KlUdpServer*, data, len,
const KlSockAddr *src, void*)`.

- **Load-bearing behaviors:** source-pinned reply (`kl_udp_send_to_from` → `msg.local` +
  `KL_DGRAM_CAP_SOURCE_PIN`, already in core) with auto-`recv_pktinfo` on wildcard bind; multicast
  join/leave (→ extended-layer `mcast_membership`). Everything else (batching/GRO/TOS/broadcast/byte
  budget) is **config passthrough** — the wrapper forwards it to fd-prep + queue policy + extended
  layer; none is read in handler/reply logic.
- **Migration shape:** re-implement `KlUdpServer` on `{KlDatagram core + extended-UDP layer}`,
  preserving the handler/reply/multicast semantics. The `reply` path uses `kl_datagram_send` with
  `msg.peer = src`, `msg.local = captured local` (source-pin), queue policy `BYTE/BOTH`.
- **Compatibility wrapper for `KlUdp` itself:** after the consumers migrate, re-implement the public
  `KlUdp` API as a thin wrapper over the same `{core + extended layer}`, preserving its full public
  surface + byte-budget semantics, so out-of-tree `KlUdp` users are unaffected. Deprecation (if ever)
  is a later, explicitly-versioned step — not part of consolidation.
- **Note (separate cleanup):** the traced `KlUdpServer` handler uses `KlSockAddr *src`, not
  `struct sockaddr` — so the I10 note that lists `udp_server.c` as a `struct sockaddr` *address
  exception* is **stale** (that description fits `dns_resolver.c`, not `udp_server.c`). Worth a
  one-line I10 correction as its own trivial docs fix, independent of this consolidation.

## 6. Separately reviewable implementation increments

Each is its own increment with its own design-freeze-then-code review; **none is authorized by this
document** — it only scopes them.

1. **M1 — queue policy (additive, no consumer change).** Add SLOT/BYTE/BOTH to `KlDatagram` per §4;
   default stays SLOT; preallocated; overflow-safe. Tests: byte/both admission + retirement, no
   hot-path alloc (ASan/LSan), no change to existing slot-only consumers.
2. **M2 — capability derivation + extended-UDP layer (additive).** Provider→caps report (§3 D-CAP-1);
   a thin extended layer exposing multicast join/leave (D-CAP-3) and, if justified, batching/GSO/GRO
   opt-in over the existing provider ops. No consumer change yet.
3. **M3 — migrate DNS → KlDatagram (first production consumer).** Behavior-parity + failure-path
   tests per §2; `send_slots` sized to max legs; TCP fallback untouched. Removes DNS's `KlUdp`
   dependency.
4. **M4 — migrate KlUdpServer → {core + extended layer}.** Preserve the public API/ABI + handler +
   multicast + source-pinned reply + byte budget per §5. Full parity + every backend gate.
5. **M5 — reduce KlUdp to a compatibility wrapper.** Re-express the public `KlUdp` API over the same
   substrate, preserving semantics/ABI; conformance across every backend. Optional deprecation is a
   later, versioned decision.

**Order/dependencies:** M1 and M2 are additive and unblock M3/M4; M3 (DNS) is the low-risk proof; M4
(server) needs M1+M2; M5 needs M3+M4 complete and green on every backend. Whether *full* consolidation
is worthwhile is re-decided after M3+M4 land — if the extended layer or wrapper proves ugly, stopping
after DNS (with `KlUdp` retained for the server) remains a valid end state.

## 7. Non-goals of this freeze

- No production code, no ABI change, no consumer migration here.
- Do **not** grow the Tier-1 `KlDatagram` core into a second monolithic `KlUdp`; extended UDP lives
  in a distinct layer above it.
- No silent budget reinterpretation (byte↔slot) and no silent capability emulation (unsupported
  providers report absence).
- Consolidation is a *goal*, not a commitment to remove `KlUdp` — M5/deprecation stay optional and
  gated on the earlier increments proving clean.
