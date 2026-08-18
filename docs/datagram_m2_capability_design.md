# Datagram M2 — capability derivation + extended-UDP layer — design freeze

Status: **PROPOSED (docs-only)** — no code until reviewed and accepted, per the consolidation
workflow. Sibling of the accepted M0 / synchronous-teardown / M1 freezes. Authority is
`docs/datagram_consolidation_design.md` §3 (Decisions D-CAP-1..D-CAP-3, Open question O-CAP-1) and §5;
this freeze turns them into an implementable increment and pins the edges they leave open.

## 0. Scope

**M2 is additive and changes no consumer.** It gives the datagram facade two things the design (§3)
calls missing: (1) **provider→capability derivation** so `kl_datagram_caps()` reflects what the
provider *actually* supports and `want_caps` becomes a fail-loud **init** gate; and (2) a **thin
extended-UDP layer** exposing the one genuinely *runtime* UDP feature — **multicast join/leave** —
routed to the provider's existing `mcast_membership` op. It prepares M4 (`KlUdpServer`), which needs
source-pin caps + multicast; it does **not** touch the core send/recv/close machinery.

**In scope:**
- A provider capability report (`KlDatagramOps.caps`) + facade derivation (D-CAP-1).
- `want_caps` as an init-time gate: init fails loudly if the provider lacks a requested cap (D-CAP-1).
- `kl_datagram_caps()` re-specified to report **provider** support (not the `want_caps` echo).
- A new `KL_DGRAM_CAP_MULTICAST` (+ `KL_DGRAM_CAP_BROADCAST`) capability bit.
- Runtime `kl_datagram_multicast_join` / `_leave` gated on `KL_DGRAM_CAP_MULTICAST` (D-CAP-3).
- The per-provider caps table (each in-tree provider reports truthfully; no emulation).
- O-CAP-1 (recv-TOS delivery) resolved.

**Explicitly out of scope (deferred, with rationale):**
- **Batching (`recvmmsg`/`sendmmsg`) and GSO/GRO through the facade.** The Tier-1 core is *single-flight*
  (one send in flight, one recv posted), so a facade batching API has no core to drive and no current
  consumer to justify it (§6, O-M2-batching). The provider batching ops stay available for a future
  batched consumer; M2 adds no facade surface for them. This is a **known M4 perf note**: `KlUdpServer`
  on the single-flight core forgoes `recvmmsg`/`sendmmsg` coalescing (revisit only if a benchmark
  regresses materially — a separate increment, not M2/M4).
- **Broadcast / TOS / GRO / byte-budget as runtime APIs.** These are **configure-time** knobs on
  `KlUdpConfig` (applied by M0 `kl_datagram_open` → provider `configure`), not dynamic — they need no
  extended-layer runtime call. `KL_DGRAM_CAP_BROADCAST` is added only as a *reportable* cap (so a
  consumer can discover support), not a runtime toggle.

## 1. Provider→capability derivation (Decision D-CAP-1)

Today `KlDatagram` has only the negotiation half: `want_caps` (consumer requires), `kl_datagram_caps()`
(reports), `KL_DATAGRAM_UNSUPPORTED` (send refused for an un-granted cap). And `core->caps` is set to
`cfg->want_caps` — so `kl_datagram_caps()` echoes the *request*, and an unsupported `want_caps` is
caught only at **send** time. M2 closes that:

- **D-M2-1 — a provider capability report.** Append to the `KlDatagramOps` vtable
  (`include/keel/socket_dgram.h`):
  ```c
  /* KL_DGRAM_CAP_* the provider actually supports on this fd/family. Optional (NULL). */
  unsigned (*caps)(void *ctx, KlSocketHandle fd, int family);
  ```
  Appended at the **end** of the vtable (source-additive; in-tree providers add it, third-party
  providers that zero-init leave it NULL). A dedicated op — **not** folded into `configure`'s return,
  which already means a different bitmask (`KL_DGRAM_RX_*` receive-capture flags); conflating the two
  namespaces would be a latent bug.

- **D-M2-2 — the facade derives at init.** `kl_datagram_init_ex` (and thus `kl_datagram_init`) computes
  `provider_caps = ops->caps ? ops->caps(sp_ctx, fd, family) : CAPS_FALLBACK` (fallback in §1a), then:
  - **fail-loud gate:** if `want_caps & ~provider_caps` ≠ 0 → init **fails** (`-1`,
    `kl_datagram_last_error` = the unsupported error, §1b), fd not adopted (existing failure contract).
    No silent emulation, no deferral to send time.
  - store `provider_caps` on the core/facade for reporting; keep the **granted** set (`want_caps`,
    already `⊆ provider_caps` after the gate) as `core->caps`, which continues to drive the send-time
    `KL_DATAGRAM_UNSUPPORTED` check unchanged. Two fields, distinct meanings: *granted* (what the
    consumer opted into, enforced on send) vs *available* (what the provider supports, reported).

- **D-M2-3 — `kl_datagram_caps()` reports provider support.** Re-specified from "the `want_caps` echo"
  to "the `KL_DGRAM_CAP_*` the provider supports" (`provider_caps`). Safe: **no in-tree consumer calls
  `kl_datagram_caps()`** (verified), so the semantics change is observable only to new code, for which
  provider-support is the intended, more-useful meaning. Since `want_caps ⊆ provider_caps` post-gate,
  the value is a superset of the old echo — never fewer caps.

### 1a. `CAPS_FALLBACK` for a NULL `caps` op (Decision D-M2-4)

A provider that has not implemented `caps` (NULL) must not silently fail every `want_caps`. The facade
falls back to a **vtable-derived baseline**: the send-feature caps the core already exercises through
the message fields — `KL_DGRAM_CAP_SOURCE_PIN | KL_DGRAM_CAP_TOS | KL_DGRAM_CAP_CONNECTED` (every
provider's `send(dest, src, tos)` takes these) — **plus** `KL_DGRAM_CAP_MULTICAST` iff
`ops->mcast_membership != NULL` and `KL_DGRAM_CAP_BROADCAST` iff the provider set the broadcast RX/opt
path (conservatively: report it only when `configure` is present). This preserves today's behavior for
un-updated third-party providers (their `want_caps` is granted as before; a real gap still surfaces as
send-time `UNSUPPORTED`) while every in-tree provider reports precisely (§4). The fallback is
**documented as best-effort**; accurate reporting requires implementing `caps`.

### 1b. The unsupported-capability error (Decision D-M2-5)

`KlError` has no "unsupported" code today (only `KL_ERR_INVALID_ARG`). *Recommended:* add
`KL_ERR_UNSUPPORTED` (appended to the `KlError` enum — additive, existing values unchanged) for a
precise fail-loud diagnostic. *Alternative:* reuse `KL_ERR_INVALID_ARG`. Open decision O-M2-err (§11).

## 2. New capability constants (Decision D-M2-6)

Extend the `KL_DGRAM_CAP_*` bitfield (`include/keel/datagram.h`), appending bits (existing 1<<0..1<<2
unchanged):
```c
#define KL_DGRAM_CAP_MULTICAST (1u << 3)   /* runtime multicast join/leave (kl_datagram_multicast_*) */
#define KL_DGRAM_CAP_BROADCAST (1u << 4)   /* SO_BROADCAST datagrams (reportable; configured via KlUdpConfig) */
```
`MULTICAST` gates the §3 runtime API. `BROADCAST` is report-only (discoverability), configured at
`kl_datagram_open` time, no runtime toggle.

## 3. Multicast runtime API — the extended-UDP layer (Decision D-CAP-3 / D-M2-7)

Multicast join/leave are dynamic (mDNS/SSDP add/drop groups at runtime), so they are runtime calls, not
config. The **entire** extended-UDP runtime surface for M2 is two functions (public, `datagram.h`):
```c
int kl_datagram_multicast_join (KlDatagram *dg, const char *group, unsigned iface_index);
int kl_datagram_multicast_leave(KlDatagram *dg, const char *group, unsigned iface_index);
```
Symmetric with `kl_udp_multicast_join/leave` (`src/udp.c:928`). Semantics:
- Gated on `KL_DGRAM_CAP_MULTICAST`: if `!(provider_caps & MULTICAST)` (or `mcast_membership` is NULL)
  → return `-1` (`KL_ERR_UNSUPPORTED`), fail-loud, no emulation.
- Route to `dg_ops(dg)->mcast_membership(sp_ctx, dg->fd, family, group, iface_index, join)` — the same
  provider op `KlUdp` uses. Return its `0`/`-1`.
- **Family (Decision D-M2-8):** `mcast_membership` needs the socket family, but `KlDatagramConfig`
  carries none and is **frozen** (M1 — no ABI change). The family is **derived from the group literal**
  (`kl_sockaddr_parse`/family-detect: dotted-quad → `AF_INET`, colon → `AF_INET6`). A group whose family
  mismatches the socket is rejected by the kernel at the join syscall (fail-loud), so no stored socket
  family is needed. (Alternative — record family via `getsockname` at init — rejected: extra syscall,
  not portable to EFI/lwIP; the group literal is unambiguous.)

**Placement (Decision D-M2-9):** the two functions live in the facade TU `src/datagram.c` (they route
through the existing `dg_ops()` accessor, exactly like `kl_datagram_send` → provider `send`). No new
`datagram_ext.c`/`.h` — two provider-routing calls do not warrant a TU. If batching/GSO are ever
justified (§6), a dedicated `datagram_ext.*` can be introduced then. This keeps M2's "extended layer"
honest: a capability gate + two runtime calls, not a new subsystem.

## 4. Per-provider capability table (Decision D-M2-10)

Each in-tree provider implements `caps` reporting its **true** support — unsupported features report
**absent**, never emulate (the lwIP source-pin/TOS no-op must surface as a *missing* cap, D-CAP-1):

| Provider | SOURCE_PIN | TOS | CONNECTED | MULTICAST | BROADCAST |
|---|---|---|---|---|---|
| `socket_posix` (default) | ✓ | ✓ | ✓ | ✓ | ✓ |
| `socket_winsock` | ✓ | ✓ | ✓ | ✓ | ✓ |
| overlapped (iouring / iocp / pollcomp) | inherit posix/winsock dgram ops | | | | |
| lwIP (BSD + raw) | ✗ (ignored today) | ✗ | ✓ | ✓ (IGMP) | dep. |
| EFI_UDP4 | ✗ | ✗ | ✓ | dep. (EFI_UDP4 mcast) | ✗ |

Exact lwIP/EFI rows are finalized against each provider's real behavior at implementation; the
**principle is frozen**: report what is real, so the D-CAP-1 init gate turns a silent no-op into a loud
`want_caps` failure. (The overlapped providers delegate to the underlying posix/winsock dgram ops, so
they report that provider's set.)

## 5. Recv-TOS delivery — O-CAP-1 resolved (Decision D-M2-11)

Recv TOS is captured to the inbound slot (`KlDgramRxMeta.tos`) but not delivered by `KlDatagramRecvFn`.
**No consumer reads it** (`KlUdpServer` sets `recv_tos` but never surfaces it). **Resolution: (a) leave
it captured-not-delivered in M2** — do **not** grow the Tier-1 recv callback ABI for an unread knob.
If a future consumer needs it, add a post-delivery accessor `kl_datagram_recv_tos()` (option (b)) as a
trivial additive follow-up — explicitly **not** M2. (Growing `KlDatagramRecvFn` — option (c) — is
rejected: a recv-callback ABI change for a knob nobody reads.)

## 6. Batching / GSO deferral — O-M2-batching (Decision D-M2-12)

The roadmap lists "batching/GSO/GRO opt-in … **if justified**." It is **not** justified for M2/M4:
- The Tier-1 core is single-flight; a facade `send_batch`/`recv_batch` has no batched core to drive.
- M4's consumers are per-datagram: `KlUdpServer.reply` sends one datagram; DNS sends one query. Neither
  needs coalescing for correctness.
So M2 exposes **no** batching/GSO facade API. The provider ops (`recv_batch`/`send_batch`/`send_gso`)
remain available for a hypothetical future batched consumer. Recorded M4 perf note (§0): a migrated
`KlUdpServer` forgoes `recvmmsg`/`sendmmsg` coalescing vs today's `KlUdp`; if a benchmark shows a
material regression, a batched-recv extended layer is a **separate** future increment (not folded into
M2 or M4).

## 7. ABI / STABLE-contract treatment (Decision D-M2-13)

- **`KlDatagramConfig` and every existing `kl_datagram_*` signature: unchanged.** No struct-layout ABI
  break (the M1 lesson). M2 adds only **new** symbols (`kl_datagram_multicast_join`/`_leave`), **new**
  capability bits, and — additively — a new vtable member and (optionally) a new `KlError` value.
- **`KlDatagramOps` vtable: append `caps`** at the end. This is a provider-vtable extension, the same
  additive move by which `mcast_membership` / batching ops were added; NULL is tolerated (§1a). A
  provider that positionally-initializes must add the trailing member — in-tree providers all do (§4).
- **`kl_datagram_caps()` semantics clarification** (want_caps echo → provider support): a behavior
  refinement of a STABLE function, safe because no in-tree consumer calls it (§1). Documented in the
  banner as the D-CAP-1 clarification (report available caps, not the request).
- **`KlError` append** (if O-M2-err picks `KL_ERR_UNSUPPORTED`): enum extension, existing values fixed.

## 8. Consumer neutrality

- **DNS (M3):** `want_caps = 0` → the init gate is a no-op; unaffected. It never joins multicast.
- **`KlUdpServer` (M4):** will request `SOURCE_PIN` (source-pinned replies) — now validated at init
  against provider support — and call `kl_datagram_multicast_join/leave` for
  `kl_udp_server_multicast_join/leave`. M2 builds and tests the mechanism; it wires no consumer.
- The default POSIX build reports the full cap set, so nothing regresses on the common path.

## 9. Test matrix

Provider/caps + multicast over the scripted mock provider (`test_datagram_public.c`) and a
capability-reporting mock, plus a live loopback multicast case where the platform allows:

1. **caps derivation** — a mock provider reporting a subset; `kl_datagram_caps()` returns exactly the
   provider set (not the `want_caps` echo).
2. **want_caps init gate (fail-loud)** — `want_caps` requiring a cap the provider lacks → `init_ex`
   returns `-1` with the unsupported error and the fd is **not** adopted (retained by the caller); a
   `want_caps ⊆ provider_caps` request succeeds.
3. **NULL `caps` fallback** — a provider with `caps == NULL` grants the §1a baseline (init succeeds for
   `SOURCE_PIN|TOS|CONNECTED`, and `MULTICAST` iff `mcast_membership` set); a real gap still yields
   send-time `UNSUPPORTED`.
4. **multicast gated** — `kl_datagram_multicast_join/leave` on a provider **without** `MULTICAST` (or
   NULL `mcast_membership`) returns `-1`/`KL_ERR_UNSUPPORTED`, and does **not** call the provider.
5. **multicast routes** — on a `MULTICAST`-capable mock, join/leave call `mcast_membership` with the
   family **derived from the group literal** (IPv4 group → `AF_INET`, IPv6 group → `AF_INET6`), the
   right `iface_index`, and `join` 1/0.
6. **family mismatch** — a group/socket family mismatch surfaces the provider's `-1` (kernel/mock
   rejects), not a silent success.
7. **recv-TOS unchanged** — the recv callback signature and delivery are byte-identical (O-CAP-1 (a)).
8. **live loopback multicast** (where supported) — real POSIX provider join/leave on a loopback group
   returns 0 (skipped on platforms/CI without multicast).
9. **regression** — every existing datagram suite (send/close/public/live, DNS) passes verbatim.

## 10. Validation plan

- macOS default (kqueue readiness) `make test` — all suites + new caps/multicast cases, ASan/UBSan.
- `BACKEND=pollcomp` — the overlapped provider reports the delegated posix caps; multicast gating.
- Linux container `BACKEND=iouring` under ASan/UBSan/**LSan** — no leak from the caps/multicast path
  (no allocation added).
- Gates: `check-tier1-boundary` (the multicast calls route through the existing `dg_ops()` seam — no
  new boundary crossing), `check-sockaddr-neutral`, `check-doc-refs`, `cppcheck`; EFI host-mock +
  freestanding datagram build (new caps op + multicast funcs compile; EFI/lwIP report reduced sets).

## 11. Open decisions for the reviewer (before implementation)

- **O-M2-err — the unsupported error code.** *Recommended:* add `KL_ERR_UNSUPPORTED` (additive enum
  value) for a precise fail-loud diagnostic on the `want_caps` init gate and the multicast gate.
  *Alternative:* reuse `KL_ERR_INVALID_ARG`.
- **O-M2-caps-report — `kl_datagram_caps()` semantics.** *Recommended (D-CAP-1):* re-specify it to
  report **provider** support. *Alternative:* keep it as the granted (`want_caps`) echo and add a new
  `kl_datagram_provider_caps()` — avoids re-specifying a STABLE function but adds a second reporter.
- **O-M2-fallback — NULL `caps` baseline.** Confirm §1a (vtable-derived baseline preserving legacy
  grant) vs the stricter "NULL ⇒ report nothing ⇒ any `want_caps` fails init" (cleaner fail-loud, but
  breaks un-updated third-party providers). Recommended: §1a best-effort baseline.
- **O-M2-batching — defer batching/GSO from the facade.** Confirm §6 (defer; single-flight core, no
  justification) over building a batched-recv extended API in M2. Recommended: defer.
