# `KlDatagram` vs `KlUdp`: which datagram API to use

**Status:** positioning + decision table (R2 of [keel_improvement_roadmap.md](../../roadmap/roadmap.md)).
Documentation only; **no source or ABI change**, and **no consumer migration**; migrations, if any,
are proposed and implemented separately (see the consumer inventory below).

**Frozen direction.** `KlDatagram` is the canonical Tier-1 API for new portable message protocols.
`KlUdp` remains supported and non-deprecated **because current consumers and extended UDP features
require it**. Any consolidation or deprecation of `KlUdp` requires a **separate design + migration
increment**; it is a recognized future objective, currently **deferred**, not precluded and not
promised as permanent. (Earlier phrasing that the two "coexist permanently" overstated the evidence;
the accurate statement is "required today, consolidation is a future increment".)

Keel ships two datagram APIs occupying different roles today:

- **`KlDatagram`** (`<keel/datagram.h>`); the **canonical Tier-1 bounded *message* transport.** A
  caller-owned, single-threaded, event-loop-driven datagram primitive, sibling to `KlStream` and
  `KlListener`, with a STABLE function+type contract validated live across every event backend
  (readiness + completion). Fixed-slot admission and confirmed-detachment close. Use it for new
  portable message protocols.
- **`KlUdp`** (`<keel/udp.h>`); the **compatibility + extended-UDP facility.** The original,
  full-featured UDP socket surface, required by `KlUdpServer` and by the advanced UDP features
  `KlDatagram` deliberately does not carry.

**True implementation relationship (not a public-API dependency).** `KlUdpServer` directly consumes the
**public `KlUdp`** API; the built-in DNS resolver has been **migrated to the public `KlDatagram`** (M3),
so it is `KlDatagram`'s first production consumer. The two public APIs are *not* layered on each other,
what they share is the **internal datagram substrate** (the `KlDatagramOps` provider seam, the
lifetime/liveness tokens, receive handling, and parts of the close/retirement machinery). So
`KlDatagram` is not the "foundation" of `KlUdpServer`; the shared internal substrate is.

## Decision table

| If you are… | Use | Because |
|---|---|---|
| Writing a **new portable message protocol** (mDNS, CoAP, a datagram QUIC/HTTP-3 base, …) | **`KlDatagram`** | It is the canonical Tier-1 message transport; one contract, every backend, bounded fixed-slot backpressure, confirmed-detachment close. |
| You need **advanced UDP features**; `recvmmsg`/`sendmmsg` batching, GSO/GRO segmentation offload, multicast join/leave + `IP_MULTICAST_*`, broadcast, per-packet TOS/DSCP, source-pinned sends, `IP_PKTINFO` dest capture | **`KlUdp`** | These live only on `KlUdp` (see "intentionally differ" below). `KlDatagram` is deliberately a minimal bounded-message core. |
| You are **maintaining or extending `KlUdpServer`** (the remaining `KlUdp` consumer) | **`KlUdp`** | It exposes `KlUdp`'s extended surface (multicast/broadcast/batching) + byte-budget semantics by design. |
| You need a **byte-budgeted** send queue (backpressure measured in bytes) | **`KlUdp`** | `KlDatagram` backpressure is a datagram **count** (fixed slots), not a byte budget; see below. |
| You want the **same event-model-agnostic lifetime/close contract** used by `KlStream`/`KlListener` | **`KlDatagram`** | It shares the Tier-1 confirmed-detachment / retirement model; `KlUdp` has its own legacy teardown. |

## Semantics that intentionally differ

These are **deliberate** differences, not gaps to be papered over. A protocol picks the API whose
semantics it wants; the two are not drop-in interchangeable.

| Aspect | `KlDatagram` (Tier-1) | `KlUdp` (extended UDP) |
|---|---|---|
| **Send backpressure** | **slot / count budget**, a fixed, preallocated number of outbound slots (`send_slots` × `send_slot_cap`); admission is atomic-or-refused | **byte budget**, `max_send_queue` bytes (default 256 KiB); whole-datagram queue |
| **Recv model** | one in-flight + one held, strict pause/resume; source + local addr + truncation flags | per-datagram callback + source/local addr; optional coalesced GRO / segment callbacks |
| **Batching / offload** | none (single-datagram core) | `recvmmsg`/`sendmmsg` (Linux), GSO/GRO (`kl_udp_send_gso`, `recv_gro`) |
| **Multicast / broadcast** | none | join/leave (any-source), `IP_MULTICAST_TTL/LOOP/IF`, `SO_BROADCAST` |
| **Per-packet TOS/DSCP, source-pinned send** | none | `kl_udp_send_to_tos`, `kl_udp_send_to_from`, `tos`/`recv_tos` |
| **Close** | confirmed-detachment (Tier-1): `on_close` after physical retirement | legacy teardown |
| **Layout ABI** | opt-in via `<keel/datagram_detail.h>` (recompile) | `KlUdpConfig`/`KlUdp` public layout |

**No promise of feature parity.** `KlDatagram` does **not** implement the `KlUdp` extensions above and
is not intended to. If a protocol needs one of them, it uses `KlUdp`. Do not migrate a `KlUdp`
consumer to `KlDatagram` if it relies on any extension, and never silently reinterpret a byte-budget
(`KlUdp`) as a slot-budget (`KlDatagram`); they are different backpressure contracts.

## Contributor rule

> **New portable message protocols use `KlDatagram`** (the canonical Tier-1 message transport) unless
> they require a documented `KlUdp` extension (batching, GSO/GRO, multicast/broadcast, per-packet
> TOS, source-pinned send, `recvmmsg`/`sendmmsg`). If a `KlUdp` extension is required, say which one
> and why in the design note. This mirrors invariant I10's dependency direction
> ([architecture_invariants.md](../../architecture/invariants.md)) at the datagram layer.

(Also recorded in [CONTRIBUTING.md](../../../CONTRIBUTING.md).)

## Consumer inventory (migrations proposed separately: NOT in this increment)

The production API consumers today:

| Consumer | Uses | Why `KlUdp` | Migration candidate? |
|---|---|---|---|
| `src/dns_resolver.c` (built-in DNS resolver) | **`KlDatagram`** | **Migrated (M3 of the consolidation).** The DNS query path is a fixed small-datagram exchange needing no `KlUdp` extension; it now embeds a fixed-slot `KlDatagram` (prepared via `kl_datagram_open`) with a transient-backpressure send machine (D-DNS-3). | Done; see [datagram_consolidation_design.md](datagram_consolidation_design.md) §2 (M3). |
| `src/udp_server.c` (`KlUdpServer`) | `KlUdp` | The dispatch surface deliberately passes through the `KlUdp` config knobs (multicast/broadcast/batching) and surfaces the source `struct sockaddr` in its handler API | Not a candidate; it exposes `KlUdp`'s extended surface by design. |

`KlDatagram`'s first production protocol consumer is the built-in DNS resolver (migrated in M3); each
further migration is a distinct increment, frozen and implemented one protocol at a time with behavior
parity and every applicable backend gate.

## Non-goals of this document

- No source or ABI change to `KlUdp` (or `KlDatagram`).
- No hidden change from byte-budget to slot-budget semantics.
- No claim that every `KlUdp` extension exists in `KlDatagram`.
- No consumer migration; only the positioning, the rule, and the inventory.
- **Not** a claim of *permanent* duplication: consolidation (migrate DNS + `KlUdpServer` onto
  `KlDatagram` behind an optional extended-capability / queue-policy layer, then reduce `KlUdp` to a
  compatibility wrapper and optionally deprecate it) is a **recognized future objective**. Its
  inventory + decisions are frozen in
  [datagram_consolidation_design.md](datagram_consolidation_design.md); the implementation is split
  into separately-reviewed increments there (none authorized yet).
