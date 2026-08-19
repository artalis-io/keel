# Post-M5 — KlUdp disposition (inventory + decision freeze, rev 2 — RULED)

**Status:** DECISION FREEZE (docs-only), **revision 2 — RULED**. No code. The inventory + options
(§1–§6) stand; rev 2 records the review ruling and turns it into a frozen migration plan (§7–§9). The
deferred post-M5 `KlUdp` decision (the freeze
[`datagram_m5_batch_extension_design.md`](datagram_m5_batch_extension_design.md) §9 carried this
forward) is now decided: **modified Option B** (§7). This document scopes the migration; each increment
lands as its own freeze-then-code review.

## RULING (2026-08-19) — modified Option B

Retain the ergonomic UDP client surface; do NOT retain the legacy byte-only queue:
- **Keep** `KlUdp` and its convenient `init`/`connect`/`send`/`recv`/`free` API (the *function surface*).
- **Reimplement** it entirely over `KlDatagram` + the M5 extension.
- **Backpressure** via the preallocated M1 **BOTH** policy (count + byte bounded); **remove** the
  `malloc`-per-datagram, count-UNBOUNDED FIFO queue (no hot-path allocation).
- **`send_queued` and related backpressure semantics** are an intentional **pre-1.0 behavior revision**.
- **Keep `KlUdpConfig`** (load-bearing).
- **Correction to Option B's original wording — NO ABI preservation.** `KlUdp` is *caller-embedded*
  (`struct KlUdp` is stack-/embed-allocatable via the public header), so re-basing it onto `KlDatagram`
  **changes its public struct layout**. The freeze therefore preserves the **function surface + client
  ergonomics**, with an **accepted struct-layout revision AND queue-semantics revision** (pre-1.0, no
  external consumers — consistent with the ABI changes accepted through M1–M5). Callers recompile; the
  API they call is unchanged.

*Why (reviewer):* removing `KlUdp` would leave external users without a convenient public
socket-creation/client API (`kl_datagram_open` is internal; raw `KlDatagram` needs a prepared fd), but
preserving an unused legacy queue would retain hot-path allocation + complexity with no real consumer.

## Original inventory + options (rev 1, retained)

**Status (rev 1):** DECISION FREEZE (docs-only). No code. A short inventory + options analysis +
recommendation for the deferred post-M5 `KlUdp` decision.

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

## 6. Non-goals (rev 1)

- No code, no API change, no test migration in this freeze — it is inventory + a decision to be ruled.
- `KlUdpConfig` is retained under every option (load-bearing socket-option type). Its possible rename to
  a datagram-neutral name is a separate cosmetic pass, not part of this decision.
- Mode-B explicit `kl_datagram_recv_batch` (M5 §5.5) and native completion-backend batched receive
  (M5 §10 O-1) remain independent follow-ups, unaffected by this decision.

## 7. Frozen direction (modified Option B) — mechanics

`KlUdp` becomes a thin wrapper around an embedded `KlDatagram` (exactly the shape `KlUdpServer` took in
M4), plus the M5 batch/GSO/GRO extension for the feature knobs. The public 19-function API is unchanged;
the implementation, the struct layout, and the backpressure numbers change.

**Layout revision (accepted).** `struct KlUdp` stops embedding `KlUdpTransport` (the byte-queue
transport, `keel/udp_transport_detail.h`) + the `malloc`-per-node `KlUdpDatagram` FIFO, and instead
embeds a `KlDatagram` (like `KlUdpServer`), plus the facade-only callbacks + any batch handles. The
`KlUdpTransport`/`KlUdpDatagram` layouts and their opt-in detail header are RETIRED. Callers recompile.

**fd prep + lifecycle.** `kl_udp_init` prepares the fd via `kl_datagram_open` (M0) from the existing
`KlUdpConfig`, then `kl_datagram_init_ex` with the BOTH policy; `kl_udp_free` → `kl_datagram_teardown`
(synchronous outside a callback / deferred within, unchanged ergonomics). `kl_udp_connect` →
`kl_datagram_send` connected-mode (want_caps `KL_DGRAM_CAP_CONNECTED`).

**Backpressure = BOTH (preallocated).** `max_send_queue` (bytes) → the M1 `send_byte_budget`;
`send_slots` derived from the budget (per the M4 `KlUdpServer` sizing: full-datagram `send_slot_cap`,
`slots = max(1, budget / 65507)`); `send_slot_cap` = a full UDP payload. No `malloc` on send. A
datagram that exceeds the byte budget OR finds no free slot is refused (the KlUdp `int -1` +
`KL_ERR_QUEUE_FULL` surface, preserved).

**Queue-semantics revision (accepted, pre-1.0).**
- `kl_udp_send_queued` continues to report **queued+in-flight payload BYTES** (closest to the legacy
  meaning) — via a new public `kl_datagram_send_queued_bytes` accessor exposing the send machine's
  `bytes_used` (a small additive `KlDatagram` accessor; `kl_datagram_send_queued` keeps returning the
  slot COUNT). `kl_udp_dropped`/`truncated` semantics are preserved.
- **The behavioral change:** admission is now count-AND-byte bounded (BOTH). A workload that previously
  queued an unbounded COUNT of small datagrams (byte-bounded only) can now be refused on the slot count
  — an accepted revision (the removed hazard: a zero-length-datagram flood was byte-unbounded).

**Feature knobs over M5/M2 (no duplication).**
- `send_to_from` (source-pin), multicast join/leave, `set_tos`/`send_to_tos` (per-packet TOS): existing
  `KlDatagram`/M2 send + `kl_datagram_multicast_*`; want_caps requested from `KlUdpConfig` as today.
- `send_gso`: `kl_datagram_send_gso` (M5.2b) — `KlUdp` lazily creates one SEND `KlDatagramBatch` (the
  GSO group buffer) on first use, torn down at free.
- `recv_segments` (GRO) + `mmsg_batch` (recvmmsg): a RECV `KlDatagramBatch` attached at `recv_start`
  when the knobs + provider caps warrant (the M5.4 `KlUdpServer` pattern — GRO-mandatory guard keyed to
  the accepted RX bit; `mmsg_batch` normalized 0→16/[1,64]).
- `recv_tos` (read the current datagram's TOS in `on_recv`): needs a small `KlDatagram` addition — the
  recv delivery must surface the datagram's TOS (a scratch on the facade, mirroring M5.3's
  `recv_seg_size`, + a `kl_datagram_recv_tos` accessor). Frozen as an M6.2 sub-item.

## 8. Increment breakdown (each its own freeze-then-code review; none authorized here)

1. **M6.1 — KlUdp core over `KlDatagram`.** The layout revision (embed `KlDatagram`; retire
   `KlUdpTransport`/`KlUdpDatagram` + `udp_transport_detail.h`); `init`/`free`/`connect`;
   `send_to`/`send_to_from`/`send` over `kl_datagram_send` with the BOTH policy (no queue);
   `recv_start`/`recv_stop`/`on_drain`; `fd`/`local_port`/`last_error`; the revised `send_queued`(bytes,
   via the new `KlDatagram` byte accessor)/`dropped`/`truncated`. The plain UDP client, end to end. Full
   parity re-run of the existing `test_udp` behavioral cases (adjusted for the bounded-queue revision).
2. **M6.2 — KlUdp feature parity over M5/M2.** Multicast join/leave; `set_tos`/`send_to_tos`;
   `recv_tos` (+ the small `KlDatagram` TOS-on-recv addition); `send_gso` (lazy SEND batch);
   `recv_segments` + `mmsg_batch` (RECV batch attach, M5.4 pattern). Re-runs `test_udp_tos` /
   `test_udp_multicast` / `test_udp_offload` / `test_udp_batching` against the reimplemented `KlUdp`.
3. **(after M6.1+M6.2)** Delete the now-dead `udp.c` byte-queue/parallel-data-plane code paths that the
   wrapper no longer uses; confirm the smoke/integration (`smoke_udp`, `lwip`, `uefi`) KlUdp-client
   usage still passes over the reimplemented object.

**Order/dependencies:** M6.1 → M6.2 → cleanup. Each is validated on the full matrix (macOS kqueue,
pollcomp, container epoll + io_uring under ASan/UBSan/LSan, MinGW, freestanding). Whether the migration
proves clean enough to finish (vs stopping after M6.1 with the feature knobs still on the old path) is
re-decided after M6.1 lands — the same "stop if ugly" escape hatch the consolidation used throughout.

## 9. Open decisions for the reviewer (rev 2)

- **O-KLUDP-2 (was contingent, now framed):** confirm `send_queued` reports queued+in-flight BYTES via a
  new `kl_datagram_send_queued_bytes` accessor (§7) — vs revising it to the slot COUNT (simpler, but a
  larger semantic change to the reported number). Recommendation: bytes (closest to legacy).
- **O-KLUDP-3:** confirm the M6.1 / M6.2 split (core client, then features) — or a finer split (e.g.
  GSO and recv-batch/GRO as separate increments) if the M6.2 surface proves too large for one review.

## 10. Non-goals (rev 2)

- No code in this freeze; the layout + queue-semantics revisions are frozen-for-review, landing in
  M6.1/M6.2.
- `KlUdpConfig` is retained (load-bearing). A datagram-neutral rename stays a separate cosmetic pass.
- No change to `KlDatagram`'s Tier-1 contract beyond the two small additive accessors (`send_queued_
  bytes`, `recv_tos`) the wrapper needs.
- Mode-B `recv_batch` (M5 §5.5) + native completion batched receive (M5 §10 O-1) remain independent.
