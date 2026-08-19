# Post-M5 — KlUdp disposition (inventory + decision freeze)

**Status:** DECISION FREEZE (docs-only). No code. A short inventory + options analysis + recommendation
for the deferred post-M5 `KlUdp` decision (the freeze
[`datagram_m5_batch_extension_design.md`](datagram_m5_batch_extension_design.md) §9 carried this
forward). Its purpose is to determine whether `KlUdp`'s remaining byte-only send-queue semantics justify
a compatibility wrapper or whether a versioned removal is cleaner — **the final call is a review ruling;
this document scopes the options, it authorizes no code.**

## 0. Context

The datagram consolidation (M0–M5) built a Tier-1 `KlDatagram` core + a capability-gated batch/GSO/GRO
extension, and migrated the two in-tree production consumers onto it: **M3** (`dns_resolver`) and **M4**
(`KlUdpServer`). M5 then moved every throughput feature `KlUdp` had (recvmmsg batching, sendmmsg, UDP
GSO, UDP GRO) into `KlDatagram` as an optional layer. `KlUdp` was deliberately left untouched; this is
the decision on its fate.

## 1. Inventory (verified against the tree, 2026-08-19)

**In-tree production consumers of the `KlUdp` OBJECT API: ZERO.** No `src/` module calls
`kl_udp_init`/`kl_udp_send_*`/`kl_udp_recv_*` except `udp.c` itself. DNS (M3) and `KlUdpServer` (M4)
embed a `KlDatagram`, not a `KlUdp`.

**`KlUdpConfig` is load-bearing and SURVIVES regardless of `KlUdp`'s fate.** It is the datagram
socket-option config — the argument to `kl_datagram_open` (M0) and the provider `configure()` op — and
is referenced *by struct tag* from `keel/datagram.h` (`struct KlUdpConfig;`). In-tree users:
`datagram_open.c`, `dns_resolver.c`, `udp_server.c`, `socket_dgram_posix.c`, `socket_dgram_win.c`,
`udp.c`. **Any option here treats `KlUdpConfig` as permanent** (possibly worth a rename to
`KlDatagramSockOpts` in a later cosmetic pass, out of scope here).

**`KlUdp`'s unique surface — ONE genuine semantic.** The public API is 19 functions
(`kl_udp_init/free/connect/recv_start/recv_stop/send_to/send_to_from/send/send_to_tos/send_gso/
set_tos/recv_tos/recv_segments/multicast_join/multicast_leave/on_drain/send_queued/dropped/truncated/
fd/last_error`). Everything except the send queue now has a `KlDatagram`/M5 equivalent:
- source-pin (`send_to_from`), TOS, multicast, GSO, GRO/`recv_segments`, recvmmsg batching → all in
  `KlDatagram` (M2/M5) or the provider vtable.
- **The byte-only, count-UNBOUNDED send queue** (`q_head`/`q_tail`/`q_bytes`/`max_send_queue`, a
  `malloc`-per-datagram FIFO; `kl_udp_send_queued` reports BYTES; `dropped` counts over-cap) is the
  **only** thing `KlDatagram` cannot express: M1's BOTH policy bounds by count AND bytes over a fixed
  preallocated slot array; it cannot offer *unbounded count* because a zero-length-datagram flood then
  has no bound (the reason the byte-only queue stayed OUT of the allocation-free core, M5 §7/§9).
  Tests exercising it: `test_udp.c` (`max_send_queue` backpressure, `send_queued`/`dropped`).

**`KlUdp` OBJECT consumers that remain are TESTS / SMOKES / INTEGRATIONS (our own):**
- Unit/feature suites: `test_udp.c`, `test_udp_tos.c`, `test_udp_multicast.c`, `test_udp_offload.c`,
  `test_udp_batching.c`.
- Used as a UDP CLIENT (not the subject): `test_udp_server.c`, `test_dns_resolver.c` (spoofer),
  `smoke_udp.c`, `smoke_iouring.c`/`smoke_pollcomp.c`/`smoke_iocp.c`.
- Integrations: `integrations/lwip/*` (raw_udp/raw_dns/loopback/caps tests), `integrations/uefi/
  mock_efi_test.c` — several use `KlUdp` to prove the lwIP/EFI provider serves UDP.

**Out-of-tree:** Hull (the sibling app) does NOT call `kl_udp_*` directly (its UDP/DNS is internal to
Keel via `http.fetch`/the resolver). The project is **pre-1.0 / pre-consumer** — ABI breaks were
explicitly accepted throughout M1–M5 (e.g. `KlDatagramConfig.accepted_rx_caps`, the `KlIoStatus`
append). So there is **no external-compat constraint** forcing `KlUdp` to persist.

## 2. What `KlUdp` costs today

`udp.c` (~950 lines) carries a full parallel datagram data-plane — send queue + drain, readiness/
completion recv machine (`KlUdpRx` sharing the Tier-1 `KlDgramRecv` + inbound slot + life token since
Phase A), recvmmsg/sendmmsg batching, GSO fallback, GRO split, multicast, TOS — **most of which now
duplicates `KlDatagram` + M5**. Two parallel implementations of the same features = ongoing maintenance
+ drift risk (every provider/seam change must be mirrored). The *only* part with no `KlDatagram`
equivalent is the byte-only queue (a few dozen lines).

## 3. Options

**Option A — Keep `KlUdp` as-is (deprecated compat, status quo).**
- *Pro:* zero work; preserves the ergonomic single-object UDP client + the byte-only queue; no test/
  integration churn.
- *Con:* the ~950-line parallel data-plane stays — two implementations of batch/GSO/GRO/multicast/TOS/
  recv to maintain in lockstep. The consolidation's stated goal (one datagram substrate) is not reached.

**Option B — Reduce `KlUdp` to a thin wrapper over the `KlDatagram` substrate, keeping ONLY its
byte-only queue as an above-Tier-1 adapter.**
- Re-express `kl_udp_send_*`/`recv_*`/lifetime/multicast/TOS/GSO/GRO over `KlDatagram` + the M5 batch
  extension; keep the `malloc`-per-node byte queue as a small allocator-backed adapter **above** the
  core (explicitly outside the allocation-free Tier-1 contract) so exact byte-budget backpressure +
  `send_queued`(bytes)/`dropped` are preserved. Delete the duplicated machinery from `udp.c`.
- *Pro:* preserves the full public API/ABI (19 fns) + the ergonomic client + the unique semantic; ONE
  data-plane implementation below the queue adapter; the goal is reached without breaking anyone.
- *Con:* real implementation work + risk (the recv path already shares Tier-1 machinery, but the send
  queue, GSO group, and GRO split would re-route through the M5 extension APIs — non-trivial); the byte
  queue stays a `malloc`-per-datagram legacy path.

**Option C — Versioned removal of `KlUdp` (keep `KlUdpConfig`).**
- Delete `udp.c` + `keel/udp.h`'s `KlUdp` object API; keep `KlUdpConfig`. Migrate the test/smoke/
  integration KlUdp-*client* usage to `KlDatagram` (or a tiny test helper); drop the KlUdp-*subject*
  feature suites whose coverage M5 now duplicates in `test_datagram_batch`/`test_dgram_*`.
- *Pro:* cleanest end state — a single datagram surface (`KlDatagram` low-level + `KlUdpServer` server);
  deletes ~950 lines + the byte-only queue entirely; pre-1.0 so no external breakage.
- *Con:* loses the ergonomic single-object UDP client API (a real usability regression for a bring-up/
  client use case `KlDatagram`'s manual fd-prep + facade does not match); the largest test/integration
  migration of the three; loses count-unbounded backpressure (only M1's count+byte BOTH remains).

## 4. Recommendation

The byte-only queue **alone does not justify a compatibility wrapper** — it is a legacy backpressure
model (not a throughput win), has no consumer, and M1's BOTH policy covers the realistic
count+byte-bounded case. So *if the question is strictly "does the byte-queue justify keeping KlUdp,"*
the answer is **no** → the byte-queue is not a reason to keep `KlUdp`.

But that reframes the decision to its real axis: **does the ergonomic single-object UDP client API have
standalone value worth maintaining as a public surface?** — independent of the (now-migrated) production
consumers. That is a product judgment, not a code fact:
- If **yes** (an easy `kl_udp_init`/`send_to`/`recv_start` client is a wanted public surface): **Option
  B** — reduce `KlUdp` to a thin wrapper over the substrate so the API/ergonomics survive with ONE
  implementation below, and the byte-queue rides an explicit above-Tier-1 adapter. This is the original
  M5 "compat wrapper" framing and reaches the consolidation goal.
- If **no** (the `KlDatagram` facade + `KlUdpServer` cover the real use cases, and an ergonomic client
  is not a committed surface): **Option C** — versioned removal, the cleanest architecture, viable
  because the project is pre-1.0 with no external `KlUdp` consumers.

**Provisional recommendation: Option B**, on the judgment that an ergonomic single-object UDP client is
a legitimate public surface (bring-up, discovery/mDNS/SSDP clients, simple send/recv) that the
lower-level `KlDatagram` facade — designed for embedding + manual fd prep + batch objects — does not
serve well; Option B keeps that value while eliminating the duplicated data-plane. If review judges the
ergonomic client surface unneeded, **Option C** is strictly cleaner. **Option A** is the do-nothing
fallback but leaves the parallel implementation the consolidation set out to remove.

## 5. Open decision for the reviewer

- **O-KLUDP-1 — the axis (§4):** does the ergonomic single-object UDP client API have standalone
  public-surface value? **Yes → Option B (thin wrapper); No → Option C (versioned removal).** (Option A
  = defer again.) This is the only judgment blocking a concrete increment plan; each option's mechanics
  are already scoped above.
- **O-KLUDP-2 (contingent):** if Option B, confirm the byte-queue adapter lives explicitly ABOVE Tier-1
  (allocator-backed, outside the allocation-free contract) and preserves the exact `max_send_queue`
  byte budget + `send_queued`(bytes)/`dropped` semantics + the full 19-fn API/ABI. If Option C, confirm
  `KlUdpConfig` is retained and the KlUdp-client test/integration usage migrates to `KlDatagram` (with
  the M5-duplicated feature suites dropped).

## 6. Non-goals

- No code, no API change, no test migration in this freeze — it is inventory + a decision to be ruled.
- `KlUdpConfig` is retained under every option (load-bearing socket-option type). Its possible rename to
  a datagram-neutral name is a separate cosmetic pass, not part of this decision.
- Mode-B explicit `kl_datagram_recv_batch` (M5 §5.5) and native completion-backend batched receive
  (M5 §10 O-1) remain independent follow-ups, unaffected by this decision.
