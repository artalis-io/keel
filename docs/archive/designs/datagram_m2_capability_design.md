# Datagram M2, capability derivation + extended-UDP layer, design freeze

Status: **PROPOSED (docs-only, revision 3)**; no code until reviewed and accepted, per the
consolidation workflow. Sibling of the accepted M0 / synchronous-teardown / M1 freezes. Authority is
`docs/datagram_consolidation_design.md` §3 (Decisions D-CAP-1..D-CAP-3, Open question O-CAP-1) and §5;
this freeze turns them into an implementable increment and pins the edges they leave open.

**Revision 2** applies the review rulings. (1) The `KlDatagramOps.caps` append is an **intentional
pre-consumer ABI revision**: documented as such (§7), not claimed ABI-safe merely because the member
is trailing/nullable; NULL-tolerance applies only *after* recompilation. (2) An unknown/NULL provider
report yields **no optional caps** (§1a), a function signature never proves source-pin/TOS/connected/
broadcast behavior. (3) The `caps` op takes no `family` **parameter** so it is callable at init (where
`KlDatagramConfig` carries no family); rev 3 sharpens this: it is fd-family-*aware* (§1). (4)
`kl_datagram_caps()` is **preserved** as the granted-caps report; provider support is exposed additively
via a **new** `kl_datagram_provider_caps()` (§1, P2 resolved without revising a STABLE function).
Rulings confirmed: add `KL_ERR_UNSUPPORTED`; unknown ⇒ no optional caps; defer batching/GSO/GRO.

**Revision 3** applies the rev-2 review: (P1) the `caps` report is **truthful for the specific fd's
family**, not provider-global, so the fail-loud gate cannot grant an IPv4-only cap to an IPv6 socket
(the provider inspects the fd, e.g. `getsockname`, or reports the cross-family intersection;
`KL_DGRAM_CAP_BROADCAST` is `AF_INET`-only), plus a family-limited-provider init-rejection test (§9.4);
(P2, doc) reconciled the last stale §0 sentence (`kl_datagram_caps()` stays the granted report); (P2,
multicast) specified deterministic `kl_datagram_last_error()` outcomes: `KL_ERR_UNSUPPORTED` (missing
capability), `KL_ERR_INVALID_ARG` (malformed group literal), `KL_ERR_IO` (provider/syscall failure).

## 0. Scope

**M2 is additive and changes no consumer.** It gives the datagram facade two things the design (§3)
calls missing: (1) **provider→capability derivation** so a new `kl_datagram_provider_caps()` reports
what the provider *actually* supports on the fd and `want_caps` becomes a fail-loud **init** gate
(`kl_datagram_caps()` itself is preserved as the granted-caps report); and (2) a **thin
extended-UDP layer** exposing the one genuinely *runtime* UDP feature; **multicast join/leave**:
routed to the provider's existing `mcast_membership` op. It prepares M4 (`KlUdpServer`), which needs
source-pin caps + multicast; it does **not** touch the core send/recv/close machinery.

**In scope:**
- A provider capability report (`KlDatagramOps.caps`) + facade derivation (D-CAP-1).
- `want_caps` as an init-time gate: init fails loudly if the provider lacks a requested cap (D-CAP-1).
- `kl_datagram_caps()` **preserved** (granted caps); provider support exposed via a new additive
  `kl_datagram_provider_caps()`.
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
  regresses materially, a separate increment, not M2/M4).
- **Broadcast / TOS / GRO / byte-budget as runtime APIs.** These are **configure-time** knobs on
  `KlUdpConfig` (applied by M0 `kl_datagram_open` → provider `configure`), not dynamic; they need no
  extended-layer runtime call. `KL_DGRAM_CAP_BROADCAST` is added only as a *reportable* cap (so a
  consumer can discover support), not a runtime toggle.

## 1. Provider→capability derivation (Decision D-CAP-1)

Today `KlDatagram` has only the negotiation half: `want_caps` (consumer requires), `kl_datagram_caps()`
(reports), `KL_DATAGRAM_UNSUPPORTED` (send refused for an un-granted cap). And `core->caps` is set to
`cfg->want_caps`, so `kl_datagram_caps()` echoes the *request*, and an unsupported `want_caps` is
caught only at **send** time. M2 closes that:

- **D-M2-1, a provider capability report, truthful for THIS fd (Decision D-M2-1, blocker P1).** Append
  to the `KlDatagramOps` vtable (`include/keel/socket_dgram.h`):
  ```c
  /* KL_DGRAM_CAP_* usable on THIS fd: accounting for the fd's actual address family. Optional
   * (NULL ⇒ no optional caps, §1a). The signature omits `family` (KlDatagramConfig carries none at
   * init); the provider determines the fd's family itself (e.g. getsockname) OR conservatively reports
   * only the cross-family intersection. It must NOT report a capability the fd's family cannot use. */
  unsigned (*caps)(void *ctx, KlSocketHandle fd);
  ```
  The report is **fd-specific, not provider-global.** Reporting a provider-wide set and deferring
  family limits to the operation's syscall would break the fail-loud `want_caps` gate, e.g. an
  IPv4-only capability granted to an IPv6 socket would pass init and fail later. The frozen contract is
  **"the caps the provider can honor on this exact fd."** A provider obtains the fd's family internally
  (`getsockname`) or, if it cannot, reports only the **cross-family intersection** (omitting any
  family-specific capability). Concrete instance: `KL_DGRAM_CAP_BROADCAST` is **IPv4-only** (IPv6 has no
  broadcast), the POSIX provider reports it only for an `AF_INET` fd, never for `AF_INET6`; so
  `want_caps = BROADCAST` on an IPv6 datagram fails init loudly, not at a later send.

  The op takes no `family` parameter: resolving that `KlDatagramConfig` carries no family to pass at
  init (P1-family); the provider inspects the fd. A dedicated op, **not** folded into `configure`'s
  return, which already means the `KL_DGRAM_RX_*` receive-capture bitmask.

  Appended at the **end** of the vtable, an **intentional pre-consumer ABI revision** (§7), not an
  "ABI-safe" trailing add: any provider that positionally-initializes `KlDatagramOps` MUST recompile;
  NULL-tolerance is a *post-recompile* convenience, not binary compatibility.

- **D-M2-2, the facade derives at init.** `kl_datagram_init_ex` (and thus `kl_datagram_init`) computes
  `provider_caps = ops->caps ? ops->caps(sp_ctx, fd) : 0` (NULL ⇒ **no optional caps**, §1a), then:
  - **fail-loud gate:** if `want_caps & ~provider_caps` ≠ 0 → init **fails** (`-1`,
    `kl_datagram_last_error` = `KL_ERR_UNSUPPORTED`, §1b), fd not adopted (existing failure contract).
    No silent emulation, no deferral to send time.
  - store `provider_caps` on the core/facade for `kl_datagram_provider_caps()`; keep the **granted** set
    (`want_caps`, already `⊆ provider_caps` after the gate) as `core->caps`, which continues to drive
    the send-time `KL_DATAGRAM_UNSUPPORTED` check unchanged and is what `kl_datagram_caps()` reports.
    Two fields, distinct meanings: *granted* (what the consumer opted into, enforced on send, reported
    by `kl_datagram_caps()`) vs *available* (what the provider supports, reported by
    `kl_datagram_provider_caps()`).

- **D-M2-3: `kl_datagram_caps()` preserved; add `kl_datagram_provider_caps()` (P2 resolved
  additively).** `kl_datagram_caps()` keeps its exact current meaning, the **granted** caps
  (`core->caps`, == `want_caps` post-gate), so the STABLE function is **not** revised. Provider support
  is exposed through a **new** additive symbol:
  ```c
  unsigned kl_datagram_provider_caps(const KlDatagram *dg);   /* KL_DGRAM_CAP_* the provider supports */
  ```
  Two crisp, non-overlapping meanings: `kl_datagram_caps()` = "what this datagram will accept on send"
  (granted/negotiated); `kl_datagram_provider_caps()` = "what the provider could support" (available).
  This is purely additive: no re-specification of a STABLE function (chosen over reopening
  `kl_datagram_caps()` pre-consumer, to minimize contract churn).

### 1a. NULL / unknown `caps` op ⇒ no optional caps (Decision D-M2-4, blocker P1)

A NULL `caps` op reports **no optional capabilities**: `provider_caps = 0`. A function signature never
proves behavior: `send(dest, src, tos)` takes a source and TOS argument whether or not the provider
*honors* them (lwIP already ignores source-pin/TOS, exactly the silent no-op D-CAP-1 forbids). So the
facade must **not** infer `SOURCE_PIN`/`TOS`/`CONNECTED`/`BROADCAST` (or `MULTICAST` from a non-NULL
`mcast_membership`) from the vtable shape. Consequences, both intended:
- Any non-zero `want_caps` on a NULL-`caps` provider **fails init** (fail-loud), correct: an
  unimplemented reporter cannot vouch for a capability. `want_caps == 0` (e.g. DNS) is unaffected.
- Every in-tree provider therefore MUST implement `caps` to grant any optional capability (§4); this
  ships as part of M2. Since there are no external providers yet, no third-party build regresses.

### 1b. The unsupported-capability error (Decision D-M2-5, RULED)

Add **`KL_ERR_UNSUPPORTED`** to the `KlError` enum (appended, existing values unchanged) for a precise
fail-loud diagnostic on both the `want_caps` init gate (§1) and the multicast gate (§3). (Reviewer
ruling; the reuse-`KL_ERR_INVALID_ARG` alternative is dropped.)

## 2. New capability constants (Decision D-M2-6)

Extend the `KL_DGRAM_CAP_*` bitfield (`include/keel/datagram.h`), appending bits (existing 1<<0..1<<2
unchanged):
```c
#define KL_DGRAM_CAP_MULTICAST (1u << 3)   /* runtime multicast join/leave (kl_datagram_multicast_*) */
#define KL_DGRAM_CAP_BROADCAST (1u << 4)   /* SO_BROADCAST; IPv4 fds only (reported per-fd); config via KlUdpConfig */
```
`MULTICAST` gates the §3 runtime API. `BROADCAST` is report-only (discoverability), configured at
`kl_datagram_open` time, no runtime toggle, and, being IPv4-only, is reported (§1 D-M2-1) only for an
`AF_INET` fd, never for `AF_INET6`.

## 3. Multicast runtime API: the extended-UDP layer (Decision D-CAP-3 / D-M2-7)

Multicast join/leave are dynamic (mDNS/SSDP add/drop groups at runtime), so they are runtime calls, not
config. The **entire** extended-UDP runtime surface for M2 is two functions (public, `datagram.h`):
```c
int kl_datagram_multicast_join (KlDatagram *dg, const char *group, unsigned iface_index);
int kl_datagram_multicast_leave(KlDatagram *dg, const char *group, unsigned iface_index);
```
Symmetric with `kl_udp_multicast_join/leave` (`src/udp.c:928`). Each returns `0` on success or `-1`,
setting `kl_datagram_last_error()` to a **deterministic** code per failure class (Decision D-M2-7a,
blocker P2). Evaluated in this order, the first failing check returns, and no later step runs:
- **capability missing**: `!(provider_caps & KL_DGRAM_CAP_MULTICAST)` or `mcast_membership == NULL` →
  `-1`, `KL_ERR_UNSUPPORTED`. Checked first (before parsing); no provider call.
- **malformed group literal**, the group fails to parse to a numeric multicast address (§ D-M2-8) →
  `-1`, `KL_ERR_INVALID_ARG`; no provider call.
- **provider / syscall failure**: `mcast_membership` returns `-1` (e.g. `setsockopt` `EADDRNOTAVAIL`,
  a group/socket family mismatch, no such interface) → `-1`, `KL_ERR_IO`. This is the only outcome that
  reaches the provider.
- **success**: `mcast_membership` returns `0` → `0`, `last_error` untouched.

On success the call routes to `dg_ops(dg)->mcast_membership(sp_ctx, dg->fd, family, group, iface_index,
join)`, the same provider op `KlUdp` uses.
- **Family (Decision D-M2-8):** `mcast_membership` takes an explicit `family` parameter (the `caps` op
  does not, §1, but that op is still fd-family-*aware*, inspecting the fd itself). `KlDatagramConfig`
  carries no family and is **frozen** (M1, no ABI change), so the join/leave **derives the family from
  the group literal** (`kl_sockaddr_parse`/family-detect: dotted-quad → `AF_INET`, colon → `AF_INET6`;
  a literal that does not parse to a numeric multicast address → `KL_ERR_INVALID_ARG`, above). A group
  whose family mismatches the socket is rejected by the provider/kernel (→ `KL_ERR_IO`, above), so no
  stored socket family is needed. (Alternative, record family via `getsockname` at init, rejected:
  extra syscall, not portable to EFI/lwIP; the group literal is unambiguous.)

**Placement (Decision D-M2-9):** the two functions live in the facade TU `src/datagram.c` (they route
through the existing `dg_ops()` accessor, exactly like `kl_datagram_send` → provider `send`). No new
`datagram_ext.c`/`.h`: two provider-routing calls do not warrant a TU. If batching/GSO are ever
justified (§6), a dedicated `datagram_ext.*` can be introduced then. This keeps M2's "extended layer"
honest: a capability gate + two runtime calls, not a new subsystem.

## 4. Per-provider capability table (Decision D-M2-10)

Each in-tree provider implements `caps` reporting its **true** support; unsupported features report
**absent**, never emulate (the lwIP source-pin/TOS no-op must surface as a *missing* cap, D-CAP-1):

Reported **for the fd's family** (§1 D-M2-1), so `BROADCAST` is `AF_INET`-only:

| Provider | SOURCE_PIN | TOS | CONNECTED | MULTICAST | BROADCAST |
|---|---|---|---|---|---|
| `socket_posix` (default) | ✓ | ✓ | ✓ | ✓ | ✓ **iff `AF_INET` fd** |
| `socket_winsock` | ✓ | ✓ | ✓ | ✓ | ✓ **iff `AF_INET` fd** |
| overlapped (iouring / iocp / pollcomp) | inherit posix/winsock dgram ops | | | | |
| lwIP (BSD + raw) | ✗ (ignored today) | ✗ | ✓ | ✓ (IGMP) | dep. |
| EFI_UDP4 | ✗ | ✗ | ✓ | dep. (EFI_UDP4 mcast) | ✗ |

Exact lwIP/EFI rows are finalized against each provider's real behavior at implementation; the
**principle is frozen**: report what is real **on this fd** (so `BROADCAST` is never reported on an
IPv6 fd), turning the D-CAP-1 init gate from a silent no-op into a loud `want_caps` failure. The POSIX
provider determines the fd's family via `getsockname` inside `caps`. (The overlapped providers delegate
to the underlying posix/winsock dgram ops, so they report that provider's per-fd set.)

## 5. Recv-TOS delivery: O-CAP-1 resolved (Decision D-M2-11)

Recv TOS is captured to the inbound slot (`KlDgramRxMeta.tos`) but not delivered by `KlDatagramRecvFn`.
**No consumer reads it** (`KlUdpServer` sets `recv_tos` but never surfaces it). **Resolution: (a) leave
it captured-not-delivered in M2**: do **not** grow the Tier-1 recv callback ABI for an unread knob.
If a future consumer needs it, add a post-delivery accessor `kl_datagram_recv_tos()` (option (b)) as a
trivial additive follow-up: explicitly **not** M2. (Growing `KlDatagramRecvFn`, option (c), is
rejected: a recv-callback ABI change for a knob nobody reads.)

## 6. Batching / GSO deferral: O-M2-batching (Decision D-M2-12)

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
  break (the M1 lesson). `kl_datagram_caps()` in particular keeps its exact meaning (granted caps).
- **`KlDatagramOps` vtable: append `caps`, an INTENTIONAL PRE-CONSUMER ABI REVISION.** This is *not*
  described as ABI-safe on the grounds that the member is trailing or nullable. Appending to a public
  vtable changes its size/layout: a provider compiled against the old `KlDatagramOps` is binary-
  incompatible and MUST be recompiled. NULL-tolerance (a recompiled provider that leaves `caps` NULL →
  no optional caps, §1a) is a *source*-level convenience *after* recompilation, not binary
  compatibility. It is acceptable **only because there are no external providers yet** (the same
  project-stage ruling that permits it must own it explicitly). Every in-tree provider adds the member
  in M2 (§4).
- **Additive-only public API:** new symbols `kl_datagram_multicast_join`/`_leave` and
  `kl_datagram_provider_caps`; new capability bits `KL_DGRAM_CAP_MULTICAST`/`_BROADCAST`; new
  `KlError` value `KL_ERR_UNSUPPORTED` (enum append, existing values fixed). None revises an existing
  symbol or type layout, so P2 (STABLE `kl_datagram_caps()`) does not arise: provider support is a new
  reporter, not a re-specification.

## 8. Consumer neutrality

- **DNS (M3):** `want_caps = 0` → the init gate is a no-op; unaffected. It never joins multicast.
- **`KlUdpServer` (M4):** will request `SOURCE_PIN` (source-pinned replies); now validated at init
  against provider support, and call `kl_datagram_multicast_join/leave` for
  `kl_udp_server_multicast_join/leave`. M2 builds and tests the mechanism; it wires no consumer.
- The default POSIX build reports the full cap set, so nothing regresses on the common path.

## 9. Test matrix

Provider/caps + multicast over the scripted mock provider (`test_datagram_public.c`) and a
capability-reporting mock, plus a live loopback multicast case where the platform allows:

1. **caps derivation**, a mock provider reporting a subset; `kl_datagram_provider_caps()` returns
   exactly the provider set, while `kl_datagram_caps()` returns the **granted** set (`want_caps`
   post-gate), the two reporters are distinct.
2. **want_caps init gate (fail-loud)**: `want_caps` requiring a cap the provider lacks → `init_ex`
   returns `-1` with `KL_ERR_UNSUPPORTED` and the fd is **not** adopted (retained by the caller); a
   `want_caps ⊆ provider_caps` request succeeds.
3. **NULL `caps` ⇒ no optional caps**, a provider with `caps == NULL`: `kl_datagram_provider_caps()`
   is 0; `want_caps == 0` init succeeds; **any** non-zero `want_caps` fails init with
   `KL_ERR_UNSUPPORTED` (no signature-derived grant, blocker P1).
4. **family-limited report rejects an unavailable requested cap (blocker P1)**, a mock provider whose
   `caps(ctx, fd)` returns a **family-limited** set (e.g. omits `BROADCAST` for the test fd, modelling
   an IPv6 socket): `want_caps = BROADCAST` → `init_ex` returns `-1` / `KL_ERR_UNSUPPORTED` (fd not
   adopted); the same provider reporting `BROADCAST` for an `AF_INET` fd grants it. Proves the gate is
   truthful **at init**, not deferred to a later syscall.
5. **multicast gated**: `kl_datagram_multicast_join/leave` on a provider **without** `MULTICAST` (or
   NULL `mcast_membership`) returns `-1`/`KL_ERR_UNSUPPORTED`, and does **not** call the provider.
6. **multicast error outcomes (blocker P2)**, deterministic `kl_datagram_last_error()`: a malformed
   group literal → `-1`/`KL_ERR_INVALID_ARG` (no provider call); a `mcast_membership` that returns `-1`
   → `-1`/`KL_ERR_IO`; the missing-capability case (test 5) → `KL_ERR_UNSUPPORTED`.
7. **multicast routes**: on a `MULTICAST`-capable mock, join/leave call `mcast_membership` with the
   family **derived from the group literal** (IPv4 group → `AF_INET`, IPv6 group → `AF_INET6`), the
   right `iface_index`, and `join` 1/0.
8. **recv-TOS unchanged**, the recv callback signature and delivery are byte-identical (O-CAP-1 (a)).
9. **live loopback multicast** (where supported): real POSIX provider join/leave on a loopback group
   returns 0 (skipped on platforms/CI without multicast).
10. **regression**: every existing datagram suite (send/close/public/live, DNS) passes verbatim.

## 10. Validation plan

- macOS default (kqueue readiness) `make test`: all suites + new caps/multicast cases, ASan/UBSan.
- `BACKEND=pollcomp`, the overlapped provider reports the delegated posix caps; multicast gating.
- Linux container `BACKEND=iouring` under ASan/UBSan/**LSan**: no leak from the caps/multicast path
  (no allocation added).
- Gates: `check-tier1-boundary` (the multicast calls route through the existing `dg_ops()` seam, no
  new boundary crossing), `check-sockaddr-neutral`, `check-doc-refs`, `cppcheck`; EFI host-mock +
  freestanding datagram build (new caps op + multicast funcs compile; EFI/lwIP report reduced sets).

## 11. Decisions: all resolved at review (revision 3)

- **P1-fd-truthful, RESOLVED: caps are reported for the specific fd's family** (§1 D-M2-1). Not
  provider-global; the provider inspects the fd (`getsockname`) or reports the cross-family
  intersection, so the fail-loud gate cannot grant a family-inapplicable cap (`BROADCAST` is
  `AF_INET`-only). Family-limited-provider init-rejection test at §9.4.
- **P2-mcast-errors, RESOLVED: deterministic `kl_datagram_last_error()`** (§3 D-M2-7a). Missing cap →
  `KL_ERR_UNSUPPORTED`; malformed group → `KL_ERR_INVALID_ARG`; provider/syscall failure → `KL_ERR_IO`.
- **O-M2-err, RESOLVED: add `KL_ERR_UNSUPPORTED`** (§1b). A precise fail-loud diagnostic on the
  `want_caps` init gate and the multicast gate; the reuse-`KL_ERR_INVALID_ARG` alternative dropped.
- **O-M2-caps-report, RESOLVED: preserve `kl_datagram_caps()` (granted), add
  `kl_datagram_provider_caps()`** (§1 D-M2-3). Additive: no re-specification of a STABLE function
  (P2 withdrawn without invoking the pre-consumer ruling for this symbol).
- **O-M2-fallback, RESOLVED: NULL/unknown ⇒ no optional caps** (§1a). Signatures never prove
  behavior; unknown providers report nothing, so any non-zero `want_caps` fails init loudly (the
  stricter, correct reading: accepted because there are no external providers to regress).
- **O-M2-batching, RESOLVED: defer** (§6). Single-flight core, no consumer justification; recorded
  M4 perf note.

No open decisions remain; the freeze is ready for review.
