# Post-M5 — KlUdp disposition (inventory + decision freeze, rev 8 — RULED: remove, don't wrap)

**Status:** DECISION FREEZE (docs-only), **revision 8 — RULED**. No code this turn. The ruled direction
is **§11–§18**; the historical inventory/options (§0–§6) are retained as context; the former
**modified-Option-B KlUdp-wrapper plan (§7) and the M6.0a/M6.0b/M6.1 wrapper sequence (§8–§10) are
SUPERSEDED** (kept for history, banner-marked). The prior "modified Option B" is withdrawn.

### Rev 8 — GRO-activation + reconnect contracts (the last two D1 P1s)

Rev 7 closed four gaps but left two mechanically/semantically unspecified; rev 8 freezes them (§12):

- **P1-A (GRO activation — reviewer design #3).** Rev 7's "drop `recv_gro`, attach post-init" was
  IMPOSSIBLE: `kl_datagram_batch_create`/`_attach_batch` only provide storage + splitting; they do NOT
  enable `UDP_GRO` — that happens at `kl_datagram_open` via `recv_gro`, so without the knob
  `accepted_rx_caps & RX_GRO` can never be set and the two-part gate stays dead. **Fix:**
  `KlDatagramSocketConfig` **KEEPS `recv_gro`** (readiness-only GRO capture at configure), and
  **`kl_datagram_recv_start` is HARDENED to REFUSE** (`KL_ERR_UNSUPPORTED`) whenever `RX_GRO` was
  accepted but no actively-splitting batch is attached — the fail-loud net that makes the corruption
  unrepresentable, without the initializer owning `recv_start` (§12.5). `mmsg_batch` stays absent (batch
  size is a `kl_datagram_batch_create` argument).
- **P1-C revised (reconnect).** "Provider-specific behavior governs" is too ambiguous for a canonical
  facade, and the socket-provider connect seam does not report whether a failed UDP reconnect preserves
  the old association. **Fix: reconnect is UNSUPPORTED in D1** — `kl_datagram_connect` on an
  already-connected datagram is REFUSED (`KL_ERR_INVALID_ARG`) with no syscall. Connect is first-time
  only; first-connect failure is unambiguous (`connected` stays false, usable unconnected). A reconnect
  seam is a future increment (§12.4).
- **P2 fixes:** SLOT's `send_slots` default is the exact frozen value **8** (not "e.g. 8"); an unknown
  `queue_policy` → `KL_ERR_INVALID_ARG` (§12.3). A non-empty `multicast_group` is a **mandatory multicast
  request** — the initializer OR-s `CAP_MULTICAST` into required caps, so a capable provider is never
  rejected merely because the caller omitted the redundant bit (§12.1).

### Rev 7 — D1 contract reconciliation (four P1s + O-D6-2 ruling)

Rev 6's architectural direction stands; rev 7 makes D1 implementable by closing four contract gaps in
§12 (+ §17):

- **P1-A (GRO boundary safety):** `KlDatagramSocketConfig` **drops `recv_gro` + `mmsg_batch`**. Receive
  batching + GRO are a post-init, explicitly-attached concern (the M5.4 two-part gate + readiness-only +
  fail-loud teardown) — the initializer never enables GRO capture without a splitting batch (§12.5).
- **P1-B (connect cap consistency):** `kl_datagram_connect` gates on the **granted** `kl_datagram_caps()
  & CAP_CONNECTED`, and `kl_datagram_socket_init` **always requests CONNECTED opportunistically** — so
  capability, grant, and state agree; a reduced provider still does destination-addressed send (§12.4).
- **P1-C (failed reconnect):** frozen conservatively/portably — first-connect failure → `connected ==
  false` + usable unconnected; failed *reconnect* **preserves the prior connected state + peer**; a
  provider that can't guarantee that must refuse reconnect or make the datagram unusable — never claim
  un-connection (§12.4).
- **P1-D (required-RX seam):** `KlDatagramSocketConfig` gains **`want_rx_caps`** — a fail-loud required
  RX-capture mask (e.g. `RX_PKTINFO` for source-pinned replies, `RX_TOS`); init fails + closes once if
  the socket did not accept it. Plus a public inspector `kl_datagram_accepted_rx_caps` (§12.1/§12.2).
- **O-D6-2 (ruled):** explicit `queue_policy` selector (`DEFAULT | SLOT | BOTH`) on the config; a zeroed
  config = `DEFAULT` = **BOTH**; pure-SLOT is reachable from the convenience initializer, not hidden
  behind the low-level API (§12.3).

### Rev 6 — one canonical object per transport (supersedes the wrapper plan)

**New architectural ruling.** There is exactly ONE canonical object per transport abstraction.
`KlDatagram` is *the* generic connected-or-unconnected message transport; `KlStream` + `KlListener` +
`KlConnectOp` are *the* reliable byte-stream machinery. Therefore:

- `KlUdp` and `KlUdpServer` are **REMOVED**, not wrapped, renamed (`KlUdpSocket`), or retained as a
  usage-pattern shell. No `KlTcpServer`/`KlTcpClient`/`KlTcpConnection` are introduced. `KlHttp*` names
  are the HTTP protocol family and stay separate from transport objects.
- **Why:** `KlUdp`/`KlUdpServer` no longer carry unique transport semantics. DNS already rides
  `KlDatagram`; `KlUdpServer` is already a thin `KlDatagram`+M5 shell; source-pin, per-packet + default
  TOS, receive-TOS, multicast, connected-send capability, BOTH backpressure, byte-queue reporting,
  recv/send batching, GSO and GRO all now live on `KlDatagram`/M5 (the last four TOS/queue pieces landed
  in the accepted M6.0a). `KlUdp`'s only former unique semantic — the byte-only `malloc`-per-datagram
  queue — was explicitly rejected. `KlUdpServer` adds socket prep + callback adaptation + source-pinned
  reply *convenience*, not a distinct protocol or state machine. UDP has no fundamental client/server
  object split.
- **The ergonomic gap belongs directly on `KlDatagram`** (§12, D1): safe public socket
  creation/config/bind/adoption; provider-neutral optional connect; sensible send-slot/byte-budget/recv
  sizing defaults; exact ownership + cleanup on every failure — on top of the existing
  `KlDatagram` send/receive/close + M5 extensions.

**Ruled sequence (each its own freeze-then-code review):** **D1** add the public `KlDatagram` socket
convenience + connect (§12) → **D2** migrate every `KlUdp`/`KlUdpServer` test/smoke/integration/example
to direct `KlDatagram` use, proving replacement coverage (§13) → **D3** delete `KlUdp`, `KlUdpServer`,
`KlUdpTransport`/`KlUdpDatagram`/`udp_transport_detail.h`, the parallel `udp.c` data plane, and rename
`KlUdpConfig` to the datagram-neutral socket config; add a stale-name gate (§14). No permanent aliases,
forwarding headers, or compatibility macros — pre-1.0, this is an intentional API/ABI revision. The
**M6.1 KlUdp-wrapper cutover is CANCELLED.**

**M6.0b disposition (§16):** the landed M6.0b adapters + their test are **obsolete wrapper machinery
(category c)** — they translate onto `KlUdp` callbacks that D3 deletes. They changed zero production
behavior (unwired non-static functions). They are removed in D3 through a normal reviewed forward
commit — **not** reverted, reset, or amended (its commit stands, corrupted message and all).

---

### Rev 5 — dependency-first sequence (SUPERSEDED by rev 6; wrapper cutover cancelled)

A rewrite of the M6.1 boundary would have left an **invalid intermediate commit** — callable feature
functions with degraded semantics and their tests disabled — because retiring `KlUdpTransport` forces
rewriting the feature functions, yet their `KlDatagram` surface (recv-TOS accessor, socket-default TOS
op, GRO/recv-batch) was scoped to later increments. **Ruling:** neither gate-and-degrade nor a permanent
hybrid; instead land the missing `KlDatagram` capabilities FIRST, then cut KlUdp over atomically (§8):

- **M6.0a** — additive `KlDatagram` prerequisites (`optional_caps`, `send_queued_bytes`, receive-TOS
  capture/accessor, socket-default TOS facade op) + focused tests on readiness AND completion backends;
  **no KlUdp layout/behavior change**, every existing suite stays green.
- **M6.0b** — wrapper feature-adapter prep (recv delivery / GRO segmentation / M5 batch-ownership
  adapters onto the `KlUdp` callback contracts); if an adapter can't stand alone without the cutover,
  fold only its tests/helpers into the cutover — **never a second live socket owner**.
- **M6.1** — **atomic** KlUdp layout cutover: embed `KlDatagram`, retire `KlUdpTransport`, move plain
  send/recv **and all already-expressible features** at once; **preserve every public function's behavior
  except the revised BOTH backpressure; keep ALL suites green.**
- **M6.2** — GSO SEND-batch lifetime separable ONLY if `send_gso` stays correct through M6.1; else its
  adapter folds into the cutover and only the deeper optimization stays here (teardown ordering frozen).
- **Governing rule:** additive prerequisites land independently; once KlUdp changes layout the cutover is
  behaviorally complete — no public feature silently disappears between reviewed commits.

### Rev 4 — M6.1 P1 fix (opportunistic capability policy)

Requiring `KL_DGRAM_CAP_CONNECTED` in `want_caps` at `kl_udp_init` would break ordinary unconnected UDP:
`want_caps` is a hard fail-loud admission gate (`src/datagram.c:375`), so any provider lacking
connected-mode support (incl. reduced/freestanding lwIP/EFI providers reporting no optional caps) would
reject *every* `kl_udp_init`, even when the caller only uses `send_to`. Fixed (§7):

- **`kl_udp_init` succeeds for ordinary unconnected operation without requiring CONNECTED** — it passes
  `want_caps = 0` and grants KlUdp's optional caps opportunistically.
- **Additive seam:** new `KlDatagramConfig.optional_caps` (grant-if-supported), distinct from mandatory
  `want_caps`: `granted = want_caps | (optional_caps & provider_caps)`. Unsupported optional caps are
  silently dropped, not a failure; they degrade at the feature entry point.
- **Request EVERY cap the public API may use, unconditionally (rev-4 P1 fix).** `optional_caps =
  CONNECTED | SOURCE_PIN | TOS | MULTICAST` (+ M6.2b/c caps as they land) — NOT derived from
  `KlUdpConfig` knobs. The runtime feature functions (`send_to_from`, `send_to_tos`,
  `multicast_join`/`leave`, `send_gso`) are callable regardless of init options, so knob-derivation would
  regress them to `UNSUPPORTED` on a capable provider. The seam intersects with `provider_caps`, so the
  over-request is harmless. Socket-config knobs + accepted-RX capture stay separate (GRO keeps its
  two-part support+capture gate).
- **`kl_udp_connect` runs `kl_sock_connect` and succeeds only where connected sends are supported**
  (CONNECTED granted); it sets the wrapper's own `connected` state.
- **`kl_udp_send` (peerless) requires BOTH** the granted CONNECTED cap **AND** the wrapper's successful
  connect-state — capability support alone is not connection. Send-before-connect is refused with
  `KL_ERR_INVALID_ARG` (frozen — no new error enumerator).
- **New M6.1 regressions:** reduced-provider init (`caps` op NULL → `kl_udp_init` + `send_to` succeed;
  `kl_udp_connect` → `KL_ERR_UNSUPPORTED`, peerless send refused) and send-before-connect (refused, not UB).

### Rev 3 — ruling on O-KLUDP-2/3 + two freeze corrections

- **O-KLUDP-2 (ruled):** `kl_udp_send_queued` keeps reporting queued+in-flight payload BYTES via the
  additive `kl_datagram_send_queued_bytes()` accessor; slot-count would be an unnecessary extra semantic
  break.
- **O-KLUDP-3 (ruled):** M6.2 splits finer — **M6.2a** multicast + TOS (incl. receive-TOS); **M6.2b**
  receive batching + GRO + `recv_segments`; **M6.2c** GSO + SEND-batch lifetime; **M6.3** delete the
  obsolete parallel machinery + reconcile gates/docs. (§8.)
- **Correction 1 — `kl_udp_connect` (§7):** it must call the **provider-neutral socket connect seam**
  (`kl_sock_connect` on the adopted fd), exactly as the current implementation does.
  `KL_DGRAM_CAP_CONNECTED` only authorizes subsequent PEERLESS sends — it does NOT connect the socket.
- **Correction 2 — API count:** the public header declares **22** functions, not 19 (rev-2 miscount);
  the inventory (§1) now lists every declaration incl. `kl_udp_local_port`.
- **M6.2c teardown ordering (frozen, §8):** for the caller-owned lazy GSO batch, datagram close/discard
  must **retire the group and clear `gso_busy` BEFORE** the wrapper frees that batch. M6.2c is the
  highest-risk sub-increment and must NOT share a commit with receive batching (M6.2b).
- **Preserve `docs/claude_code_transport_taxonomy_prompt.md`** (present, untracked) across all M6 work —
  the M6.3 cleanup must not remove or `git clean` it.

## RULING (2026-08-19) — modified Option B  ⛔ SUPERSEDED by rev 6 (§11)

> **SUPERSEDED (rev 6):** the "keep + reimplement KlUdp as a wrapper" ruling below is WITHDRAWN. `KlUdp`
> and `KlUdpServer` are removed, not wrapped/renamed. The ergonomic surface moves directly onto
> `KlDatagram` (§12). The text is retained only for history.

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

**`KlUdp`'s unique surface — ONE genuine semantic.** The public API is **22 functions** (verified
against `include/keel/udp.h`; corrected in rev 3 from an earlier miscount of 19 — the migration
inventory must cover EVERY declaration): `kl_udp_init`, `_free`, `_connect`, `_recv_start`, `_recv_stop`,
`_send_to`, `_send_to_from`, `_send`, `_multicast_join`, `_multicast_leave`, `_send_gso`,
`_recv_segments`, `_set_tos`, `_send_to_tos`, `_recv_tos`, `_on_drain`, `_send_queued`, `_dropped`,
`_truncated`, `_fd`, `_local_port`, `_last_error`. Everything except the send queue now has a
`KlDatagram`/M5 equivalent:
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

## ⛔ §7–§10 — SUPERSEDED by rev 6 (wrapper plan withdrawn; see §11–§18)

> **SUPERSEDED (rev 6).** §7 (modified-Option-B mechanics), §8 (the M6.0a/M6.0b/M6.1 dependency-first
> wrapper sequence), §9 (rev-5 ruled decisions), and §10 (rev-5 non-goals) describe the CANCELLED
> KlUdp-wrapper cutover. Retained verbatim for history. The accepted parts already landed as the M6.0a
> additive `KlDatagram` prerequisites (optional_caps, send_queued_bytes, set_tos, receive-TOS, recv_tos)
> and the now-obsolete M6.0b adapters (§16). The live plan is **§11 onward**.

## 7. Frozen direction (modified Option B) — mechanics

`KlUdp` becomes a thin wrapper around an embedded `KlDatagram` (exactly the shape `KlUdpServer` took in
M4), plus the M5 batch/GSO/GRO extension for the feature knobs. The public 22-function API is unchanged;
the implementation, the struct layout, and the backpressure numbers change.

**Layout revision (accepted).** `struct KlUdp` stops embedding `KlUdpTransport` (the byte-queue
transport, `keel/udp_transport_detail.h`) + the `malloc`-per-node `KlUdpDatagram` FIFO, and instead
embeds a `KlDatagram` (like `KlUdpServer`), plus the facade-only callbacks + any batch handles. The
`KlUdpTransport`/`KlUdpDatagram` layouts and their opt-in detail header are RETIRED. Callers recompile.

**fd prep + lifecycle.** `kl_udp_init` prepares the fd via `kl_datagram_open` (M0) from the existing
`KlUdpConfig`, then `kl_datagram_init_ex` with the BOTH policy; `kl_udp_free` → `kl_datagram_teardown`
(synchronous outside a callback / deferred within, unchanged ergonomics).

**Opportunistic capability policy (rev 4 — P1 fix).** The existing `want_caps` is a **hard fail-loud
admission gate**: `kl_datagram_init_ex` rejects the fd if `want_caps & ~provider_caps`
(`src/datagram.c:375`), and a reduced/freestanding provider (a NULL `ops->caps`, or one reporting no
optional caps) derives `provider_caps == 0`. Therefore the wrapper **must NOT put `KL_DGRAM_CAP_CONNECTED`
(or any KlUdp feature cap) in `want_caps`** — that would reject `kl_udp_init` on every such provider even
when the caller only ever uses `send_to()`. `want_caps` is also, separately, what the send machine reads
to admit peerless sends (`s->caps & KL_DGRAM_CAP_CONNECTED`, `src/datagram_send.c:254`) — so the two
roles must be decoupled.

- **Additive `KlDatagram` init seam (frozen mechanism):** add `KlDatagramConfig.optional_caps` — a
  *grant-if-supported* mask, distinct from the *mandatory* `want_caps`. Init keeps `want_caps` fail-loud
  (unchanged), then computes `granted = want_caps | (optional_caps & provider_caps)` and sets
  `core->caps = granted` (so `optional_caps` a provider lacks are silently dropped, never a failure).
  This is a small additive Tier-1 config field, in the same class as M5.1's `accepted_rx_caps`; it keeps
  the single NULL-`caps`-op → 0 rule inside `datagram.c` (the alternative — the wrapper deriving
  `provider_caps` itself before adoption and intersecting — duplicates that rule across the seam and is
  rejected in favor of the additive field).
- **`kl_udp_init` grants, never demands — and requests EVERY cap the public API may use.** It passes
  `want_caps = 0` (ordinary unconnected `send_to` works on *every* provider, incl. reduced/freestanding)
  and requests `optional_caps` **unconditionally**, NOT "per configured knob":
  ```c
  optional_caps = KL_DGRAM_CAP_CONNECTED   /* kl_udp_connect + peerless send */
                | KL_DGRAM_CAP_SOURCE_PIN   /* kl_udp_send_to_from — needs NO recv_pktinfo config */
                | KL_DGRAM_CAP_TOS          /* kl_udp_send_to_tos  — needs NO socket-default TOS config */
                | KL_DGRAM_CAP_MULTICAST    /* runtime kl_udp_multicast_join/leave */
                /* + M6.2b/c caps (e.g. KL_DGRAM_CAP_GSO) as those feature increments land */;
  ```
  The runtime feature entry points (`send_to_from`, `send_to_tos`, `multicast_join`/`leave`, later
  `send_gso`) are callable irrespective of any init knob, so deriving these caps from `KlUdpConfig`
  options would wrongly deny them on a *capable* provider (e.g. source-pin does not require
  `recv_pktinfo`; per-packet TOS does not require a socket-default `tos`). The additive seam makes the
  over-request harmless: unsupported bits are intersected out (`optional_caps & provider_caps`).
  Unsupported optional caps then degrade at the **feature entry point** (e.g. `send_to_from` returns
  `-1`/`KL_ERR_UNSUPPORTED` when `SOURCE_PIN` was not granted), matching KlUdp's historical best-effort
  sockopt behavior — never at init.
- **Socket configuration and accepted-RX capture stay SEPARATE from `optional_caps`.** The `KlUdpConfig`
  sockopt knobs still drive `kl_datagram_open`/`configure()` (socket defaults), and RX capture
  (`accepted_rx_caps`) is unchanged — in particular **GRO** still needs its M5.3 two-part gate (provider
  `CAP_GRO` support **AND** `accepted_rx_caps & KL_DGRAM_RX_GRO`), which `optional_caps` neither grants
  nor bypasses.

**`kl_udp_connect` (corrected).** It calls the **provider-neutral socket connect seam** —
`kl_sock_connect(sockets, kl_datagram_fd(&udp->dg), peer)` — exactly as today (`udp.c` currently does
`kl_sock_connect(udp->dg.ctx->sockets, udp->dg.fd, peer)`). It **succeeds only where connected sends are
supported**: if `KL_DGRAM_CAP_CONNECTED` was not granted (`kl_datagram_caps(&udp->dg) & CAP_CONNECTED`
== 0), connect returns `-1`/`KL_ERR_UNSUPPORTED` before touching the socket (a provider that cannot admit
a peerless send has no use for the kernel association). On success it sets the wrapper's own
`connected` state flag; connect *syscall* failure maps to the existing `KL_ERR_CONNECT` surface.
`KL_DGRAM_CAP_CONNECTED` only **authorizes** subsequent peerless sends — it does NOT itself connect the
socket.

**`kl_udp_send` (peerless) requires BOTH cap AND connect-state.** A peerless `kl_udp_send` is admitted
only when (a) `CAP_CONNECTED` was granted (so the send machine accepts `peer == NULL`) **AND** (b) the
wrapper's own `connected` flag is set (i.e. `kl_udp_connect` actually ran `kl_sock_connect` successfully).
Capability support alone is NOT connection: a `kl_udp_send` issued **before** `kl_udp_connect` is refused
`-1` with `KL_ERR_INVALID_ARG` (frozen — no new enumerator; a `KL_ERR_NOT_CONNECTED` code would add no
architectural value), never passed to the kernel on an unconnected socket.

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
- `recv_tos` (read the current datagram's TOS in `on_recv`) + `set_tos` (socket-default TOS): need small
  `KlDatagram` additions — recv delivery must surface the datagram's TOS (a facade scratch mirroring
  M5.3's `recv_seg_size`, + a `kl_datagram_recv_tos` accessor), and a `kl_datagram_set_tos` facade op for
  the socket-wide default. Both land as **M6.0a prerequisites** (§8) BEFORE the cutover, so M6.1 can wire
  them without an invalid intermediate commit.

## 8. Increment breakdown (rev 5 — DEPENDENCY-FIRST; each its own freeze-then-code review)

**Governing rule (ruled rev 5):** additive `KlDatagram` prerequisites may land independently while KlUdp
stays untouched and fully green; **once KlUdp changes layout, the cutover is behaviorally complete** — no
public feature may silently disappear or ship with degraded semantics between reviewed commits. The
rev-3 finer M6.2a/b/c feature split is **superseded** (it implied an invalid intermediate commit where
callable functions regress and their tests are disabled). New order: **M6.0a → M6.0b → M6.1 (atomic
cutover) → M6.2 (optimization-only, optional) → M6.3 (reconcile)**.

1. **M6.0a — additive `KlDatagram` prerequisites (no KlUdp layout/behavior change).** Land the missing
   `KlDatagram` capabilities FIRST, so the M6.1 cutover has every surface it needs:
   - `KlDatagramConfig.optional_caps` grant-if-supported seam (`granted = want_caps | (optional_caps &
     provider_caps)`; §7).
   - `kl_datagram_send_queued_bytes` accessor (exposes the send machine's `bytes_used`; §7, O-KLUDP-2).
   - **Receive-TOS capture + accessor** — recv delivery surfaces the datagram's TOS (facade scratch
     mirroring M5.3's `recv_seg_size`) + `kl_datagram_recv_tos`.
   - **Socket-default TOS facade op** — `kl_datagram_set_tos` (the socket-wide `IP_TOS`/`IPV6_TCLASS`
     default, distinct from per-message `KlDatagramMessage.tos`).
   - Focused `KlDatagram` tests for each, on **both** a readiness and a completion backend. `KlUdp` is
     untouched and every existing suite stays green (these are purely additive to `KlDatagram`).
2. **M6.0b — wrapper feature-adapter preparation.** Define + test the internal callback adapters the
   cutover needs: mapping `KlDatagram` recv delivery, GRO segmentation, and M5 batch ownership onto the
   `KlUdp` callback contracts (`KlUdpRecvFn`/`KlUdpRecvSegmentsFn`/`KlUdpDrainFn`). **If an adapter cannot
   exist independently without a KlUdp cutover, fold only its tests/helpers into the M6.1 cutover design —
   do NOT introduce a second live socket owner** (no parallel data plane on a second fd).
3. **M6.1 — atomic KlUdp layout cutover (behaviorally complete).** Embed `KlDatagram`, retire
   `KlUdpTransport`/`KlUdpDatagram` + `udp_transport_detail.h`. Move plain send/receive **and all
   already-expressible feature functions** onto `KlDatagram`/M2/M5 in ONE reviewed increment:
   `init`/`free`/`connect`; `send_to`/`send_to_from`/`send`; `recv_start`/`recv_stop`/`on_drain`;
   `set_tos`/`send_to_tos`; `multicast_join`/`leave`; `recv_tos`; `recv_segments`/`mmsg_batch`;
   `fd`/`local_port`/`last_error`; the revised `send_queued`(bytes)/`dropped`/`truncated`. **Preserve
   every public function's behavior EXCEPT the explicitly-revised BOTH backpressure semantics; keep ALL
   suites enabled and green** (`test_udp`, `test_udp_tos`, `test_udp_multicast`, `test_udp_offload`,
   `test_udp_batching`).
   - **Opportunistic capability policy (§7, rev 4):** `kl_udp_init` passes `want_caps = 0` +
     `optional_caps = CONNECTED | SOURCE_PIN | TOS | MULTICAST` (+ GSO per §8.4), unconditionally
     (NOT knob-derived; the seam intersects `provider_caps`). Ordinary unconnected `send_to` succeeds on
     *every* provider incl. reduced/freestanding. `kl_udp_connect` calls `kl_sock_connect`, succeeds only
     where CONNECTED was granted, sets the wrapper `connected` flag; peerless `kl_udp_send` requires the
     granted CONNECTED cap AND `connected`; send-before-connect → `KL_ERR_INVALID_ARG`. Socket-config +
     accepted-RX capture (GRO two-part gate) stay separate.
   - **New regressions (added to the full parity re-run):** reduced-provider init (NULL `caps` op →
     `kl_udp_init` + `send_to` succeed; `kl_udp_connect` fails `KL_ERR_UNSUPPORTED`; peerless send refused)
     and send-before-connect (refused, no UB).
4. **M6.2 — GSO send-batch lifetime (separable ONLY if `send_gso` stays correct through M6.1).**
   `send_gso` MUST remain behaviorally correct after the M6.1 cutover. If its correct behavior can be
   preserved with the simple per-segment/`kl_datagram_send_gso` path during M6.1, the **deeper
   caller-owned SEND-batch optimization** may land here separately; **otherwise the `send_gso` adapter is
   folded INTO the M6.1 atomic cutover** and only the deeper optimization remains here. **Frozen teardown
   ordering (applies wherever the lazy SEND batch lands):** on datagram close/discard the group is
   retired and `gso_busy` cleared **BEFORE** the wrapper frees the caller-owned batch, so
   `kl_datagram_batch_free` never runs while `gso_busy` (returns -1) or with a live in-flight group
   referencing the batch's copy-once group buffer. This teardown-ordering step must NOT share a commit
   with unrelated receive batching.
5. **M6.3 — reconcile gates/docs + remove any residual dead code.** After the atomic cutover there is no
   parallel data plane to delete (M6.1 already retired `KlUdpTransport`); this step reconciles CI gates +
   docs (§10 matrices, source comments) to the reimplemented object and confirms the smoke/integration
   (`smoke_udp`, `lwip`, `uefi`) KlUdp-client usage. **Must preserve the untracked
   `docs/claude_code_transport_taxonomy_prompt.md` — no `git clean` that would remove it.**

**Order/dependencies:** M6.0a → M6.0b → M6.1 → (M6.2) → M6.3. Each is validated on the full matrix (macOS
kqueue, pollcomp, container epoll + io_uring under ASan/UBSan/LSan, MinGW, freestanding). The additive
prerequisites (M6.0a) are independently landable; the layout cutover (M6.1) is atomic and behaviorally
complete — no "stop with feature knobs on the old path" intermediate state.

## 9. Ruled decisions (rev 5)

- **O-KLUDP-2 (RULED):** `send_queued` reports queued+in-flight payload BYTES via the additive
  `kl_datagram_send_queued_bytes` accessor (§7). Slot-count would be an unnecessary extra semantic break.
- **O-KLUDP-3 (SUPERSEDED by the rev-5 dependency-first sequence):** the rev-3 finer M6.2a/b/c *feature*
  split is rejected — it would create an invalid intermediate commit (callable functions with degraded
  semantics + disabled tests). Feature presence is now delivered atomically in the M6.1 cutover after the
  additive prerequisites (M6.0a) land; only optimization-depth (GSO batch, §8.4) may remain separate.

## 10. Non-goals (rev 5)

- No code in this freeze; the layout + queue-semantics revisions are frozen-for-review, landing across
  M6.0a → M6.1 (→ M6.2).
- `KlUdpConfig` is retained (load-bearing). A datagram-neutral rename stays a separate cosmetic pass.
- No change to `KlDatagram`'s Tier-1 contract beyond **four** small additive changes (all M6.0a
  prerequisites, in the same additive class as M5.1's `accepted_rx_caps`): the `KlDatagramConfig.
  optional_caps` grant-if-supported field, and the `kl_datagram_send_queued_bytes`,
  `kl_datagram_recv_tos`, and `kl_datagram_set_tos` accessors/ops.
- Mode-B `recv_batch` (M5 §5.5) + native completion batched receive (M5 §10 O-1) remain independent.
- The untracked `docs/claude_code_transport_taxonomy_prompt.md` is preserved across all M6 work (M6.3
  cleanup must not remove it).

---

# Rev 6 — RULED direction: KlDatagram is the single canonical datagram object

## 11. The ruling + what is genuinely missing

One canonical object per transport abstraction (see the rev-6 banner). `KlUdp` + `KlUdpServer` are
removed; their ergonomics move onto `KlDatagram`. Comparing the entire `KlUdp`/`KlUdpServer` surface to
`KlDatagram`/M5 (§15.3), **only two behaviors are genuinely missing from `KlDatagram` today — both
ordinary construction/connect convenience, not unique transport semantics:**

1. **Public socket construction.** `kl_datagram_init`/`_init_ex` require an ALREADY-prepared fd
   (`socket()`'d + `configure()`'d + bound). Every current caller (DNS, `KlUdpServer`, tests) reaches
   the INTERNAL `kl_datagram_open` (`src/datagram_open.h`) + hand-builds a `KlDatagramConfig` with that
   fd. There is no public one-shot create-configure-bind-adopt. → **D1 `kl_datagram_socket_init`.**
2. **Connect + connected state.** `KlDatagram` has no connect and no "actually connected" state. The
   send machine admits a peerless send purely on the granted `KL_DGRAM_CAP_CONNECTED` capability
   (`src/datagram_send.c:254`), which does NOT mean the socket was ever `connect()`'d. → **D1
   `kl_datagram_connect` + a real connected-state gate.**

Everything else already exists: source-pin (`msg.local` + `CAP_SOURCE_PIN`), default TOS
(`kl_datagram_set_tos`, M6.0a), per-packet TOS (`msg.tos` + `CAP_TOS`), receive TOS
(`kl_datagram_recv_tos`, M6.0a), multicast (`kl_datagram_multicast_*`), BOTH backpressure
(`kl_datagram_init_ex`), queued-byte reporting (`kl_datagram_send_queued_bytes`, M6.0a), recv/send batch
+ GSO/GRO (M5 + `KlDatagramBatch`), pause/resume, confirmed-detachment close, reentrant teardown. The
`KlUdpServer` "source-pinned reply" is `on_recv`'s `local` (under `KL_DGRAM_HAS_LOCAL`) fed back as
`msg.local` on `kl_datagram_send` — a documented pattern/example, not new API (§15.3).

## 12. D1 — public `KlDatagram` socket convenience + connect (FROZEN shapes)

### 12.1 One-shot socket initializer

A single public entry that creates, configures, optionally binds, and adopts the socket into a
`KlDatagram` — reusing the accepted M0 preparation (`kl_datagram_open`) verbatim internally. **No
public two-step open/adopt is exposed** (the inventory shows no caller needs the intermediate prepared
fd exposed publicly; the wildcard source-pin gate `KlUdpServer` used the two-step for is done INSIDE the
initializer). The low-level `kl_datagram_init`/`_init_ex` (bring-your-own prepared fd) REMAIN for
advanced adoption; that is a distinct low/high-level pair, not an ambiguous two-step.

```c
/* Send-queue backpressure policy (O-D6-2). Zeroed config → DEFAULT → BOTH. */
typedef enum {
    KL_DATAGRAM_QUEUE_DEFAULT = 0,  /* == BOTH (the sensible default for a UDP socket) */
    KL_DATAGRAM_QUEUE_SLOT,         /* count-only backpressure; send_byte_budget IGNORED */
    KL_DATAGRAM_QUEUE_BOTH,         /* count + byte backpressure */
} KlDatagramQueuePolicy;

typedef struct {                    /* FROZEN public config — all pointers borrowed */
    struct KlEventCtx             *ctx;        /* event loop (selects completion vs readiness) */
    const struct KlSocketProvider *sockets;    /* datagram-capable provider; NULL = ctx default */
    KlAllocator                   *alloc;      /* NULL = ctx->alloc */

    /* ── socket creation + options (the former KlUdpConfig sockopt knobs, datagram-neutral) ── */
    int          family;            /* AF_INET / AF_INET6 / AF_UNSPEC (auto from bind_addr) */
    const char  *bind_addr;         /* numeric bind address, or NULL = unbound (pure client) */
    uint16_t     bind_port;         /* bind port; 0 = ephemeral (ignored when bind_addr == NULL) */
    int          reuse_addr, reuse_port;
    int          recv_pktinfo;      /* capture each datagram's local (dest) addr */
    int          recv_tos;          /* deliver each datagram's TOS (kl_datagram_recv_tos) */
    int          recv_gro;          /* enable UDP_GRO capture (readiness only; ignored on completion, per
                                     * M5.4). Enabling it OBLIGES the caller to attach a splitting RECV
                                     * batch before recv_start — else recv_start REFUSES (§12.5, P1-A). */
    int          broadcast;         /* SO_BROADCAST (IPv4) */
    int          multicast_ttl, multicast_disable_loop;
    unsigned     multicast_iface;   /* egress interface index; 0 = kernel default */
    const char  *multicast_group;   /* optional numeric group joined at init (after bind); NULL/"" = none.
                                     * A NON-EMPTY group is a MANDATORY multicast request: the initializer
                                     * OR-s KL_DGRAM_CAP_MULTICAST into required caps (fail-loud), so a
                                     * capable provider is never rejected for an omitted want_caps bit (P2). */
    int          so_rcvbuf, so_sndbuf;
    int          tos;               /* socket-default TOS/Traffic-Class (IP_TOS/IPV6_TCLASS) */
    /* NOTE: mmsg_batch is DELIBERATELY ABSENT — recv/send batch SIZE is a kl_datagram_batch_create
     * argument, and receive batching/GRO are attached post-init via the M5 APIs (§12.5). */

    /* ── KlDatagram sizing + policy — EXPLICIT units, no reinterpretation (§12.3 defaults) ── */
    KlDatagramQueuePolicy queue_policy; /* 0 = DEFAULT = BOTH (O-D6-2) */
    size_t       send_slots;        /* fixed outbound slot COUNT; 0 = derived default */
    size_t       send_slot_cap;     /* per-slot payload BYTES; 0 = default 65507 (full datagram) */
    size_t       send_byte_budget;  /* BOTH byte-gate BYTES; 0 = default 262144 (256 KiB); ignored under SLOT */
    size_t       recv_cap;          /* inbound slot payload BYTES; 0 = default 2048 (capped 65535) */

    /* ── capabilities ── */
    unsigned     want_caps;         /* required SEND-side caps (fail-loud, M2 exact-fd) */
    unsigned     optional_caps;     /* grant-if-supported (M6.0a). The initializer ALWAYS OR-s in
                                     * KL_DGRAM_CAP_CONNECTED so connect works wherever supported (§12.4). */
    unsigned     want_rx_caps;      /* required RX-CAPTURE mask (P1-D): KL_DGRAM_RX_PKTINFO / RX_TOS. Init
                                     * FAILS (fd closed once) if the socket did not ACCEPT every bit — the
                                     * receive-side analog of want_caps (e.g. source-pinned reply needs
                                     * RX_PKTINFO; want_caps=SOURCE_PIN proves only the send half). */
} KlDatagramSocketConfig;

/* Create + configure + (optionally) bind + adopt into `dg` (a memset-zero handle). Returns 0 (fd owned
 * by KlDatagram) or -1 (kl_datagram_last_error carries why; nothing for the caller to reclaim). */
int kl_datagram_socket_init(KlDatagram *dg, const KlDatagramSocketConfig *cfg);

/* Post-init inspector (P1-D complement): the KL_DGRAM_RX_* mask the socket actually accepted, so a
 * caller can validate capture instead of (or in addition to) the fail-loud want_rx_caps. */
unsigned kl_datagram_accepted_rx_caps(const KlDatagram *dg);
```

### 12.2 fd ownership (EXACT, single-close on every path)

- **Pre-adoption prep failure** (socket/configure/bind fails inside `kl_datagram_open`): `kl_datagram_open`
  already closes its own fd exactly once; the initializer returns -1 having created nothing else.
- **Required-RX-capture failure** (`want_rx_caps & ~prep.rx_caps` — the socket did not accept a mandated
  capture): checked on `prep` BEFORE adoption; the initializer closes the prepared fd exactly once
  (`KL_ERR_UNSUPPORTED`), returns -1 — the generic analog of `KlUdpServer`'s wildcard-pktinfo gate, now
  caller-driven instead of hard-coded.
- **Init failure before adoption** (`kl_datagram_init_ex` fails pre-adoption): the initializer closes the
  prepared fd exactly once (the M4 `KlUdpServer` ordering — copy `last_error`, then
  `kl_sock_close(prep.fd)`), returns -1.
- **Success:** fd ownership transfers to `KlDatagram`; the close machine closes it once at teardown.
- **Post-adoption failure** (e.g. a later multicast-join-at-init fails): tear down THROUGH `KlDatagram`
  (`kl_datagram_teardown`, which closes the fd once), return -1 — never a raw double close.

Allocator discipline preserved (all allocation via `KlAllocator`); no hot-path allocation added (the
slot/recv storage is the existing one-time init-time allocation). Readiness/completion/provider-neutral:
the initializer selects nothing itself — `kl_datagram_open` + `kl_datagram_init_ex` already negotiate
the loop model and go through the socket provider seam, so it works on POSIX, Winsock/IOCP, lwIP and EFI
(freestanding) exactly as `KlUdpServer`/DNS do today over `kl_datagram_open`.

### 12.3 Sizing defaults (no silent unit reinterpretation)

Each field keeps its own unit; `0` selects a documented default:
- `queue_policy == DEFAULT (0)` → **BOTH** (count + byte). `SLOT` → count-only (`send_byte_budget`
  ignored); `BOTH` → count + byte. Pure-SLOT is thus reachable from the convenience initializer (O-D6-2,
  ruled) — NOT hidden behind the low-level prepared-fd API. **Any `queue_policy` value outside
  `{DEFAULT, SLOT, BOTH}` → `KL_ERR_INVALID_ARG`** (init fails, nothing created).
- `send_slot_cap == 0` → **65507** (a full UDP payload — one datagram per slot).
- `send_byte_budget == 0` → **262144** (256 KiB, the legacy `max_send_queue` default); applied only under
  BOTH.
- `recv_cap == 0` → **2048**, hard-capped at 65535.
- `send_slots == 0` → under BOTH, derived **`max(1, send_byte_budget_eff / send_slot_cap_eff)`** (the M4
  `KlUdpServer` sizing; bytes÷bytes = a COUNT, a units-consistent documented derivation — not a
  reinterpretation of one value under two units); under SLOT (no budget to derive from), the exact frozen
  default **`8`**.

### 12.4 Provider-neutral connect + connected state (FROZEN)

```c
/* Provider-neutral connect: calls the socket provider's connect seam (kl_sock_connect) on the adopted
 * fd. On success, records the datagram's CONNECTED state so peerless sends (KlDatagramMessage.peer ==
 * NULL) are admitted. Returns 0, or -1 with kl_datagram_last_error(). */
int kl_datagram_connect(KlDatagram *dg, const KlSockAddr *peer);
```

- **One consistent capability rule (P1-B).** Both `kl_datagram_connect` AND the peerless-send gate read
  the **granted** `kl_datagram_caps() & KL_DGRAM_CAP_CONNECTED` — never `provider_caps` for one and
  granted for the other. To make the common case "just work," **`kl_datagram_socket_init` ALWAYS OR-s
  `KL_DGRAM_CAP_CONNECTED` into `optional_caps`** (opportunistic — granted iff the provider supports it,
  per M6.0a). So a socket-init'd datagram has CONNECTED granted wherever supported, and connect +
  peerless-send agree. A low-level `kl_datagram_init` caller that wants connect must itself put CONNECTED
  in `want_caps`/`optional_caps`. `kl_datagram_connect` on an ungranted datagram → `KL_ERR_UNSUPPORTED`,
  no syscall (a reduced provider still does destination-addressed send).
- **Capability vs state are distinct.** Granted CONNECTED = "peerless send is *permitted*." A NEW
  `connected` flag (core-visible), set only by a successful `kl_datagram_connect`, = "the socket is
  *actually* associated." **Peerless-send validation = granted CONNECTED AND `connected` set**
  (`src/datagram_send.c:254` gains the state check).
- **send-before-connect fails deterministically (O-D6-3):** a peerless `kl_datagram_send` before a
  successful connect returns `KL_DATAGRAM_UNSUPPORTED` — documented to cover BOTH "CONNECTED not granted"
  and "granted but not successfully connected." Never handed to the kernel on an unconnected socket;
  destination-addressed send (`msg.peer != NULL`) is unaffected.
- **Reconnect is UNSUPPORTED in D1 (P1-C — revised).** `kl_datagram_connect` connects ONCE, from the
  unconnected state. A second `kl_datagram_connect` on an already-`connected` datagram is **REFUSED with
  `KL_ERR_INVALID_ARG` and performs no syscall** — so D1 never has to reason about whether a *failed* UDP
  reconnect preserves the prior kernel association (a semantic the generic socket-provider connect seam
  does not currently report). This keeps the connected-state contract unambiguous and portable:
  - **First connect (from unconnected) succeeds** → `connected` true, peer recorded.
  - **First connect fails** → `connected` stays **false**; the datagram remains usable for
    destination-addressed sends (the socket was never associated). The socket is NOT closed.
  - **Any connect while already `connected`** → `KL_ERR_INVALID_ARG`, state + peer unchanged.
  Reconnect (re-point / disconnect-to-`AF_UNSPEC`) is deferred to a future increment that first
  strengthens the provider connect contract or adds an explicit reconnect result/state seam.

### 12.5 GRO activation + the recv_start fail-loud guard (P1-A — design #3)

GRO capture can ONLY be enabled at socket configure time (`recv_gro` → `UDP_GRO` sockopt → the provider
reports `RX_GRO` in `accepted_rx_caps`); `kl_datagram_batch_create`/`_attach_batch` provide only storage
+ splitting, never the sockopt. So the config KEEPS `recv_gro` (there is no other way to set
`accepted_rx_caps & RX_GRO`), enabled **readiness-only** (a completion loop cannot split a coalesced
buffer, so `recv_gro` is ignored there, per M5.4). The safety comes from a new fail-loud guard rather
than from hiding the knob:

- **`kl_datagram_recv_start` is HARDENED (D1 production change):** if the socket accepted `RX_GRO`
  (`accepted_rx_caps & KL_DGRAM_RX_GRO`) but **no actively-splitting batch is attached** (no `core->ext`,
  or the attached batch's two-part gate `gro_active` is false), `recv_start` **refuses with
  `KL_ERR_UNSUPPORTED`** — GRO-without-splitting (the M5.4 boundary-corruption bug, `src/udp_server.c:118`)
  is therefore *unrepresentable*: you cannot begin receiving on a GRO-enabled socket without a splitter.
- **Caller flow when `recv_gro` is set:** `kl_datagram_socket_init(recv_gro=1)` →
  `kl_datagram_batch_create(dg, KL_DGRAM_BATCH_RECV, n, bufsz)` → `kl_datagram_recv_attach_batch`
  (activates the §6.2 two-part gate: provider `CAP_GRO` AND accepted `RX_GRO`) → `kl_datagram_recv_start`
  (now succeeds). Omit the batch and `recv_start` fails loud. A caller that wants plain (unsplit) receive
  simply leaves `recv_gro` unset.
- The guard is backward-compatible: existing consumers either never set `recv_gro` (DNS) or attach a
  splitting batch before `recv_start` (`KlUdpServer`, `test_datagram_batch`), so none regress.
- **Batch sizing** is the `kl_datagram_batch_create(n, bufsz)` argument (not a config knob). Send
  batching + GSO are likewise post-init (`kl_datagram_batch_create(…, SEND, …)` + `kl_datagram_send_batch`
  / `kl_datagram_send_gso`). The initializer's job ends at a configured, adopted, optionally-connected
  socket; the batch/offload lifecycle stays the separate, already-safe M5 surface.

## 13. D2 — migrate internal usage + coverage (old → new)

Migrate every `KlUdp`/`KlUdpServer` **subject** test/smoke/integration/example to direct `KlDatagram`
use (via D1 `kl_datagram_socket_init` + `kl_datagram_connect`). Small test-only helpers are allowed; **no
private production wrapper** that recreates `KlUdp`/`KlUdpServer`. Preserve backend coverage for every
listed behavior across POSIX, Winsock/IOCP, lwIP and EFI. DNS stays on `KlDatagram`.

| Old (subject) | kl refs | Disposition | Covers-it-today / new home |
|---|---|---|---|
| `tests/test_udp.c` | 131 | **Migrate (unique live socket-init/connect/byte-queue)** | new `test_datagram_socket.c` (D1) + `test_datagram_live` additions; BOTH/geometry already in `test_datagram_public` |
| `tests/test_udp_server.c` | 108 | **Migrate unique (source-pinned reply, wildcard-pktinfo gate, multicast-at-init, reuse_port)** | bound-`KlDatagram` test/example; recv-local already in `test_datagram_live.completion_recv_captures_local` |
| `tests/test_udp_multicast.c` | 69 | **Migrate (LIVE join/leave + TTL/loop/iface)** | new live `KlDatagram` multicast test; routing/gating/errors already in `test_datagram_public.m2_multicast_*` (mock) |
| `tests/test_udp_offload.c` | 38 | **Migrate unique live GSO/GRO** | mostly DUP of `test_datagram_batch` (GRO split/gate/segments) + `send_gso` |
| `tests/test_udp_tos.c` | 36 | **Mostly DUP** | `test_datagram_live` (set_tos/per-pkt/recv-tos) + `test_datagram_public` gates (M6.0a) |
| `tests/test_udp_batching.c` | 20 | **Mostly DUP** | `test_datagram_batch` (recv batch) + `kl_datagram_send_batch` |
| `tests/test_udp_adapters.c` | 9 | **DELETE (obsolete M6.0b)** | adapters removed in D3 |
| `tests/smoke_udp.c` | 6 | **Migrate → `smoke_datagram.c`** (exists) | fold KlUdp roundtrip into the datagram smoke |
| `tests/smoke_{iocp,iouring,pollcomp}.c` | 5 ea | **Migrate KlUdp client leg → KlDatagram** | completion smokes |
| `tests/test_dns_resolver.c` | 67 + 62 svr | **Migrate the fake-nameserver harness (KlUdpServer) → bound `KlDatagram`** | DNS already on KlDatagram; only the test peer uses KlUdpServer |
| `tests/test_socket_provider.c` | 11 | **Migrate (ctx→sockets threading proof)** | prove threading with a `KlDatagram` instead of a `KlUdp` |
| `integrations/lwip/raw_udp_test.c` (48), `raw_caps_test.c`, `raw_dns_test.c`, `lwip_loopback_test.c` | — | **Migrate to `KlDatagram` over the lwIP provider** | preserves lwIP datagram coverage |
| `integrations/uefi/mock_efi_test.c` (11) | — | **Migrate to `KlDatagram` over the EFI provider** | preserves EFI datagram coverage |
| `tests/test_udp_cmsg.c` (20) | — | **KEEP (NOT a KlUdp subject)** | tests the shared `kl_udp_parse_*`/`build_control` cmsg helpers |

**KEEP (shared infra, not KlUdp object):** `src/udp_cmsg.{c,h}`, `src/udp_cmsg_win.{c,h}` and their
`kl_udp_*` cmsg helpers (used by the completion backends + `socket_dgram_*` providers), plus
`tests/test_udp_cmsg.c`. Rename of these `kl_udp_parse_*` symbols to `kl_dgram_*` is an OPTIONAL cosmetic
follow-up, out of D1–D3 scope (they are not the object API and the stale-name gate allowlists them).

**Examples:** none exist today (no `examples/*udp*`), so D2/D3 *adds* a canonical bound-`KlDatagram`
example (the D1 ergonomics showcase: init + recv + source-pinned reply + optional connect) rather than
replacing one.

## 14. D3 — remove the obsolete UDP object APIs

After D2 proves replacement coverage:
- **Delete** `KlUdp` (all 22 public functions), `KlUdpServer` (11 public functions), `KlUdpTransport`,
  `KlUdpDatagram`, `include/keel/udp_transport_detail.h`, the parallel `src/udp.c` data plane +
  `src/udp_internal.h`, `src/udp_server.c`, `include/keel/udp.h`, `include/keel/udp_server.h` — plus the
  M6.0b adapters + `tests/test_udp_adapters.c` (§16), the `KL_DGRAM_OWNER_UDP` token path and
  `kl_udp_comp_dispatch` (only KlUdp used it; `kl_datagram_comp_dispatch` is the survivor).
- **Rename** `KlUdpConfig` → `KlDatagramSocketConfig` at the provider `configure()` seam
  (`include/keel/socket_dgram.h` + `socket_dgram_posix.c`/`socket_dgram_win.c`/lwIP/EFI providers) and the
  M0 `kl_datagram_open`/`KlDatagramPrep` (`src/datagram_open.{c,h}`); reconcile every struct-tag
  forward-decl (`datagram.h`, `socket_dgram.h`). D1 introduces `KlDatagramSocketConfig` as the public
  config and maps its sockopt subset into the still-`KlUdpConfig`-typed M0 internally (see §17 O-D6-1);
  D3 collapses that mapping so a single config type remains.
- **Remove** obsolete files, `Makefile`/`CORE_SRC` entries, CI enrollment (the `test_udp_*` gate lines,
  `smoke_udp`), and documentation references (README/CLAUDE.md module list, matrices).
- **No permanent aliases / forwarding headers / compat macros / duplicate symbols** — intentional
  pre-1.0 API/ABI revision.
- **Add a mechanical stale-name gate** (a `check-no-kludp` target, sibling of `check-doc-refs`/
  `check-tier1-boundary`): fail if `KlUdp`/`KlUdpServer`/`KlUdpConfig`/`kl_udp_*` (object API) reappear in
  `src/`/`include/`/`tests/`/`integrations/`, with a NARROW allowlist for the retained cmsg helpers
  (`kl_udp_parse_*`, `kl_udp_build_control`, `kl_udp_send_family`, `test_udp_cmsg.c`) and for historical
  design docs (`docs/datagram_m6_kludp_decision_design.md`, `docs/claude_code_transport_taxonomy_prompt.md`).

## 15. Required inventory (verified against the tree, 2026-08-20)

**15.1 KlUdp/KlUdpServer references.** Production OBJECT (delete): `src/udp.c`, `src/udp_internal.h`,
`src/udp_server.c`, `include/keel/udp.h`, `include/keel/udp_server.h`,
`include/keel/udp_transport_detail.h`; the completion hook `kl_udp_comp_dispatch`
(`src/completion_core.c` registers it) + the `KL_DGRAM_OWNER_UDP` token (`src/datagram_life.*`). Tests/
smokes/integrations/examples/docs: the full D2 table (§13) — subject tests `test_udp*.c`
(`test_udp.c` 131, `test_udp_server.c` 108, `_multicast` 69, `_offload` 38, `_tos` 36, `_batching` 20,
`_adapters` 9), `smoke_udp.c` + the three completion smokes, `test_dns_resolver.c` (fake-server harness),
`test_socket_provider.c`, lwIP `raw_udp_test.c`/`raw_caps_test.c`/`raw_dns_test.c`/`lwip_loopback_test.c`,
EFI `mock_efi_test.c`; ~139 doc references (mostly historical design docs — allowlisted). NO examples.
**KEEP:** the `kl_udp_*` cmsg helpers + `test_udp_cmsg.c` (§13).

**15.2 KlUdpConfig references.** The socket config, RENAMED (not deleted) in D3. Load-bearing at the
provider seam — `configure(ctx, fd, family, const struct KlUdpConfig *cfg)` (`socket_dgram.h` vtable +
`socket_dgram_posix.c`/`socket_dgram_win.c` + lwIP/EFI providers) — and the M0 helper
`kl_datagram_open(sockets, const KlUdpConfig*, KlDatagramPrep*)` (`datagram_open.{c,h}`), plus
`dns_resolver.c` (builds one for `kl_datagram_open`) and struct-tag forward-decls in `datagram.h` /
`socket_dgram.h`, and ~20 test/integration constructors.

**15.3 KlUdpServer field-by-field vs KlDatagram/M5.** `init` = `kl_datagram_open` + wildcard source-pin
gate (`prep.rx_caps & RX_PKTINFO`) + `kl_datagram_init_ex` (BOTH) + optional recv `KlDatagramBatch`
(GRO/mmsg) + `kl_datagram_recv_start` + join-at-init → **all KlDatagram/M5 + the D1 initializer**;
`reply` = `kl_datagram_send` with `local` = the captured wildcard dest (source-pin) → **KlDatagram send +
the `on_recv` local pattern**; `multicast_join/leave` = `kl_datagram_multicast_*`; `free` =
`kl_datagram_teardown`; `local_port`/`fd`/`last_error` = `kl_datagram_local_port`/`_fd`/`_last_error`.
**No field is a distinct protocol or state machine** — only socket prep + reply convenience.

**15.4 Genuinely missing KlDatagram behavior.** Exactly the two in §11 (public socket construction;
connect + connected state). Nothing else — ordinary convenience is not counted as a unique semantic.

**15.5 KlDatagram construction audit.** Public `kl_datagram_init`/`_init_ex` demand a prepared fd; there
is no public creator. Callers therefore use the INTERNAL `kl_datagram_open` (`src/datagram_open.h`, not
installed) + a hand-built `KlDatagramConfig{.fd = prep.fd, ...}`. Consumers today: `dns_resolver.c`,
`udp_server.c`, and tests (`test_datagram_open.c`, `test_datagram_batch.c`, `test_dns_resolver.c`, EFI
`mock_efi_test.c`). D1 removes the need to reach the internal helper.

**15.6 KlDatagram connected-mode state audit.** Capability stored in `dg->provider_caps` (per-fd) and the
granted `core->caps` (= `want_caps | optional_caps&provider_caps`). **Actual successful-connect state is
NOT represented** — there is no `kl_datagram_connect` and no `connected` flag; the peerless-send gate
(`datagram_send.c:254`) checks only `s->caps & KL_DGRAM_CAP_CONNECTED`. No current KlDatagram consumer
requests CONNECTED, so the gate is presently dormant. D1 adds `kl_datagram_connect` + a `connected` flag
and tightens the peerless-send gate to require BOTH (§12.4).

**15.7 Old-test → new-test mapping.** The §13 table is the exact map (migrate-unique vs
duplicate-of-existing vs delete vs keep).

## 16. M6.0b audit + classification

- **Commit:** `23125f2` (`datagram(M6.0b) …`). **Diff:** +200 lines, all additions, zero deletions:
  `src/udp.c` +33 (three adapters `kl_udp_dg_on_recv`/`_on_segments`/`_on_drain`), `src/udp_internal.h`
  +27 (their declarations + doc block), `tests/test_udp_adapters.c` +140 (six unit tests). **The commit
  MESSAGE is corrupted** — zsh command-substituted the backticks in the `-m` string at commit time,
  injecting shell/env output into the body; the code diff is intact and correct.
- **Behavior:** changes NO production behavior. The three functions are non-static but **unwired** (the
  cancelled M6.1 was to register them); the recv path is untouched.
- **Classification (every changed symbol/file):**
  - a) canonical KlDatagram infrastructure to retain — **none.**
  - b) reusable migration coverage/helper — **none.** The adapters translate onto `KlUdpRecvFn`/
    `KlUdpRecvSegmentsFn`/`KlUdpDrainFn`, which D3 deletes; the `HAS_LOCAL`-gating logic they prove is
    already covered on `KlDatagram` (`dg_deliver` + `us_on_recv`).
  - c) **obsolete KlUdp-wrapper machinery to remove — ALL of it:** `kl_udp_dg_on_recv`,
    `kl_udp_dg_on_segments`, `kl_udp_dg_on_drain` (src/udp.c + udp_internal.h decls) and
    `tests/test_udp_adapters.c`.
- **Cleanup:** category (c) is removed as part of the **D3** deletion commit (they vanish with `udp.c`/
  `udp_internal.h`/the `test_udp_*` suite). **History is not rewritten** — `23125f2` stands; no revert,
  reset, or amend.

## 17. Decisions — RULED (rev 7)

- **O-D6-1 (RULED — accepted):** introduce `KlDatagramSocketConfig` in **D1** as the public config; keep
  the provider `configure()`/M0 `kl_datagram_open` on the existing `KlUdpConfig` during D1–D2 (the
  initializer copies the sockopt subset into a local `KlUdpConfig` — reuse M0 verbatim, zero provider
  churn mid-sequence); **collapse in D3** (switch `configure()`/M0 to `KlDatagramSocketConfig`, delete
  `KlUdpConfig`).
- **O-D6-2 (RULED — SLOT reachable via a policy selector, not the low-level API):** the config carries an
  explicit `KlDatagramQueuePolicy queue_policy` (`DEFAULT | SLOT | BOTH`); a zeroed config = `DEFAULT` =
  **BOTH**; `SLOT` gives pure count-only backpressure directly from the convenience initializer (§12.3).
- **O-D6-3 (RULED — accepted):** send-before-connect returns `KL_DATAGRAM_UNSUPPORTED` (send-status),
  **documented to cover both "CONNECTED not granted" and "granted but not successfully connected"**;
  `kl_datagram_connect` itself returns `KL_ERR_UNSUPPORTED` (ungranted cap) / `KL_ERR_CONNECT` (syscall).
- **P1-A (RULED — rev 8, reviewer design #3):** `recv_gro` STAYS in the config (the only way to set
  `accepted_rx_caps & RX_GRO`); `kl_datagram_recv_start` is hardened to refuse `RX_GRO`-accepted-without-
  active-splitter, making the M5.4 corruption unrepresentable (§12.5). `mmsg_batch` stays absent.
- **P1-B (RULED — rev 7):** connect + peerless-send both gate on GRANTED caps; the initializer always
  requests CONNECTED opportunistically (§12.4).
- **P1-C (RULED — rev 8):** reconnect is UNSUPPORTED in D1 (`kl_datagram_connect` refuses an already-
  connected datagram, no syscall) — sidesteps the unspecified failed-reconnect provider semantics (§12.4).
- **P1-D (RULED — rev 7):** `want_rx_caps` fail-loud required-RX mask + `kl_datagram_accepted_rx_caps`
  inspector (§12.1–2).
- **P2 (RULED — rev 8):** SLOT `send_slots` default = exact **8**; unknown `queue_policy` →
  `KL_ERR_INVALID_ARG` (§12.3); non-empty `multicast_group` = mandatory `CAP_MULTICAST` request (§12.1).
- **No true blockers.** D1 needs no new engine/provider capability; connect uses the existing
  `kl_sock_connect` seam (first-connect only); construction reuses M0; GRO/batching reuse the existing M5
  batch APIs + the hardened `recv_start`. Mode-B `recv_batch` and native completion batch-receive remain
  out of scope and are NOT required for D1–D3.

## 18. Non-goals (rev 8) + validation

**Non-goals:** no production code this turn; no HTTP-taxonomy rename; no `KlTcp*` APIs; no `KlUdpSocket`
wrapper; no private `KlUdpServer` replacement; no Mode-B explicit `recv_batch` API and no native
completion batch-receive expansion unless D1–D3 are genuinely blocked without them (they are not); no
push, no PR. The untracked `docs/claude_code_transport_taxonomy_prompt.md` is preserved (not staged,
committed, cleaned, or deleted).

**Validation for this docs-only freeze:** `make check-doc-refs`; `git diff --check`; confirm the
taxonomy prompt remains present + untracked; confirm no `src/`/`include/` changes. Commit only this
revised decision-freeze document locally; pause for review before any D1 implementation.
