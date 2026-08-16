# Step 7B-9 — EFI KlDatagram close: cancel→terminal + quarantine retirement (design note)

Status: **IMPLEMENTED** (host-mock validated). The `retain_life` ownership delta below is unchanged from
the reviewed design; **§0a records one trace-based correction found during implementation** — the empty-
armed-recv terminal is surfaced by el_drain's **STALE_RETIRED** branch (not a `request_recv_cancel` +
`DELIVERED(EFI_ABORTED)` reap), because `backend_close` drains + cleanly closes the child (bumping the
generation) BEFORE el_drain polls. The reviewed ref-ownership semantics (transfer→release for a normal
terminal; borrow→retain for a quarantine terminal; `retain_life` honoured at all three release sites) are
unaffected — only the substrate-result LABEL and the change surface changed. §2a/§12 are corrected inline.

## 0a. Correction (post-trace, implementation) — the terminal source is STALE_RETIRED, not DELIVERED
The reviewed §2a assumed `el_cancel_dgram(RECV)` would leave the RxToken posted (a new
`kl_uefi_udp_request_recv_cancel`) so a later `el_drain` reaps it as `DELIVERED(out_ok=0)` via `EFI_ABORTED`.
Tracing the real close path (`src/datagram_close.c` `close_run_terminal`: `cancel_recv` **then**
`backend_close`, same synchronous frame) shows that is not what happens:
- `backend_close` = `dg_close_transport` → `kl_sock_close` → **`kl_uefi_udp_close`**, which does its OWN
  `Cancel(NULL)` + **bounded drain** of the posted RxToken (recycling the firmware RxData) and then bumps
  the child **generation** (clean close) or `udp_quarantine()`s it (unconfirmed) — ALL before any later
  `el_drain`.
- So when `el_drain` next polls the op by its captured generation, the op is **STALE**:
  `kl_uefi_udp_poll_recv` → `udp_stale_class` → **STALE_RETIRED** (clean close) or **QUARANTINED**
  (unconfirmed). It is never observed as a live `DELIVERED(EFI_ABORTED)` token.
- The OLD el_drain silently released the ref on STALE_RETIRED and emitted **no** event → the recv MACHINE
  never retired (`recv_inflight` stuck at 1) → close hangs. That is the true form of gap #1.

**Corrected mechanism (implemented):** el_drain emits the terminal for BOTH terminal results — a normal
`KL_COMP_DGRAM_RECV(ok=0)` for STALE_RETIRED (transfer→release, exactly like DELIVERED(ok=0)) and a
BORROWED `KL_COMP_DGRAM_RECV(ok=0, retain_life=1)` for QUARANTINED. `el_cancel_dgram(RECV)` keeps the
existing synchronous `kl_uefi_udp_cancel_recv`; **`kl_uefi_udp_request_recv_cancel` is NOT added** (it is
unnecessary — the terminal is a substrate-state observation after backend_close, not a reaped posted
token). Everything in §2a/§2c about ref ownership (transfer vs borrow), `recv_inflight`, and the join
outcome (DETACHED vs QUARANTINED) is exactly as reviewed; only the KlUefiUdpOpResult label differs
(STALE_RETIRED, not DELIVERED). Validated by the host-mock §11 cases (DETACHED reclaims ALL storage;
QUARANTINE retains it; the router no-handler site honours retain_life).

## 0. Problem

The public `KlDatagram` close is driven by the confirmed-detachment coordinator (`src/datagram_close.c`).
Its machine-level gate `close_fully_retired()` requires `recv_inflight == 0` (datagram_close.c:37) BEFORE
it classifies the outcome (`close_join_result()`), and the §4.3 join then reads `retire_dgram`. So the
recv **machine** must retire for *every* terminal outcome — DETACHED, QUARANTINED, CLOSE_ERROR.

Over the EFI_UDP4 completion backend an armed recv retires ONLY via a drained completion
(`kl_datagram_comp_dispatch` → `kl_dgram_core_recv_on_complete`). Two gaps (traced, not inferred):

1. **Empty armed recv (confirmed close).** `el_cancel_dgram(RECV)` (7B-2c) calls `kl_uefi_udp_cancel_recv`,
   which does a synchronous **Cancel + drain** and clears `rx_posted`. `el_drain`'s `kl_uefi_udp_poll_recv`
   then returns `PENDING` forever (`if (!u->rx_posted) return PENDING`) → no terminal completion →
   `recv_inflight` never reaches 0 → close hangs. (KlUdp is unaffected: it uses `kl_udp_free`'s own
   teardown, never the coordinator, never calls `cancel_dgram`.)
2. **Quarantine.** For an unconfirmed cancel the op is QUARANTINED — its life ref MUST be retained (the
   abandoned firmware RxToken may still write the inbound buffer; releasing the ref could free that
   storage → the exact UAF quarantine exists to prevent). But `kl_datagram_comp_dispatch` **always**
   releases `ev->life` after `recv_on_complete`. Today the EFI drain for a QUARANTINED recv abandons the
   ref and emits **no** completion (event_efi.c:665-666) → `recv_inflight` stays 1 → close hangs, and even
   if it didn't, a normal terminal would wrongly release the quarantined ref.

This note pins the mechanism for review before any code.

## 1. Frozen EFI recv-op states (as they exist)

`KlUefiUdpOpResult` (integrations/uefi/socket_efi_udp4.h): `PENDING · DELIVERED · RETIRED · STALE_RETIRED
· QUARANTINED · INVALID`. Relevant primitives:
- `kl_uefi_udp_poll_recv(fd, gen, buf, cap, …, *out_ok)` — Poll + CheckEvent. Signalled with an ERROR
  status (e.g. `EFI_ABORTED` from a Cancel) ⇒ returns **DELIVERED with `out_ok = 0`**; a signalled
  success ⇒ DELIVERED `out_ok = 1`; not signalled ⇒ PENDING; slot closed/reused ⇒ STALE_RETIRED /
  QUARANTINED (via `udp_stale_class`); not a dgram handle ⇒ INVALID.
- `kl_uefi_udp_cancel_recv(fd, gen)` — Cancel + **bounded drain** (consumes the completion; sets
  `rx_posted = 0`); returns RETIRED (confirmed) or QUARANTINED (unconfirmed). **This synchronous drain is
  what breaks the coordinator flow** — the completion is consumed internally, never surfaced.
- `kl_uefi_udp_op_state(fd, gen)` — pure query (drives `el_retire_dgram`).
- `el_drain` recv arm (event_efi.c:646-668): DELIVERED → emit `KL_COMP_DGRAM_RECV(ok=out_ok)` + TRANSFER
  ref; STALE_RETIRED → release ref, no event; QUARANTINED/INVALID → abandon ref (`op->life = NULL`), no
  event; then `op->in_use = 0` (retire the record).

**~~New primitive (proposed): `kl_uefi_udp_request_recv_cancel`~~ — DROPPED (see §0a).** It is
unnecessary: `backend_close` (`kl_uefi_udp_close`) drains + recycles the RxToken and bumps the generation
BEFORE any `el_drain`, so the op is observed as STALE_RETIRED/QUARANTINED, never as a live
DELIVERED(EFI_ABORTED) token. `el_cancel_dgram(RECV)` keeps the existing synchronous
`kl_uefi_udp_cancel_recv`; el_drain's STALE_RETIRED branch surfaces the terminal.

## 2. State sequences (the three close paths)

Notation: `refs(L)` = the life token's refcount. At recv arm: `refs = owner(1) + op(1) = 2`.

### 2a. Ordinary cancellation → confirmed retirement → DETACHED   *(corrected per §0a)*
1. `recv_start` → `el_post_dgram_recv` posts an RxToken; op `in_use=1`, holds 1 ref. `recv_inflight=1`.
2. `close_begin` → coordinator sets recv `stopped`; at `close_send_drained` calls `cancel_recv`
   (`el_cancel_dgram(RECV)` → **`kl_uefi_udp_cancel_recv`**: Cancel + bounded drain, `rx_posted → 0`)
   **then** `backend_close` (`dg_close_transport` → `kl_uefi_udp_close`: clean close, generation bumped).
   `retire_dgram` at this instant = PENDING (op still `in_use`). Close stays CLOSING.
3. pump → `el_drain` → `poll_recv` resolves the op by its captured generation ⇒ **STALE_RETIRED** (the
   child was cancel-drained + cleanly closed in step 2). Drain emits `KL_COMP_DGRAM_RECV(ok=0,
   retain_life=0)`, **TRANSFERS** the ref (`ev.life = op->life; op->life = NULL`), `op->in_use = 0`
   (record retired). `refs` unchanged (transfer, not release). *(Same ref semantics as the reviewed
   DELIVERED(ok=0) path — only the substrate label differs.)*
4. dispatch → `kl_dgram_core_recv_on_complete(core, 0, 0)`: recv machine is `stopped` ⇒ retires with **no
   delivery**, `recv_inflight = 0`. `quarantined=0` ⇒ **release** `ev.life` → `refs = owner(1)`.
5. coordinator re-runs (recv activity) → `close_fully_retired` true → `close_join_result` → `retire_dgram`
   now finds no `in_use` op ⇒ **RETIRED** for both kinds ⇒ **DETACHED**. `on_close(DETACHED)` fires.
6. `kl_datagram_free` drops owner ref → `refs = 0` → `on_final` frees the rx holder. Clean, LSan-clean.

### 2b. A datagram arrives, then close
Same as 2a except step 3's first drain may deliver the real datagram (ok=1) and re-arm; the close then
cancels the fresh arm exactly as 2a. No new mechanism.

### 2c. Unconfirmed cancellation → QUARANTINE
1. As 2a step 1.
2. `close_begin` → `cancel_recv` (request Cancel) → `backend_close` = provider close → `kl_uefi_udp_close`
   does Cancel + bounded drain + **quarantine classification** (the child could not be confirmed retired,
   e.g. after ExitBootServices): the fd's slot is QUARANTINED (generation bumped once, storage pinned).
3. pump → `el_drain` → `poll_recv` (slot closed) ⇒ **QUARANTINED**. **NEW behaviour:** emit
   `KL_COMP_DGRAM_RECV(ok=0, quarantined=1)`, with `ev.life = op->life` **BORROWED (not transferred:
   `op->life` stays set)**, set `op->terminal_emitted = 1`, and **keep `op->in_use = 1`** (do NOT retire
   the record). `refs` unchanged; the op still owns its ref (abandoned).
4. dispatch → `recv_on_complete(core, 0, 0)`: `recv_inflight = 0` (machine retired, no delivery).
   `quarantined=1` ⇒ **do NOT release** `ev.life` (borrow) → `refs` unchanged (`owner + op`).
5. coordinator re-runs → `close_fully_retired` true → `close_join_result` → `retire_dgram(RECV)`: the op
   is still `in_use` with `op_state == QUARANTINED` ⇒ **QUARANTINED** ⇒ object result **QUARANTINED**.
   `on_close(QUARANTINED)` fires. `close_state = CLOSED`.
6. `kl_datagram_free` drops the owner ref → `refs = op(1)` (the abandoned op ref remains) → `on_final`
   does NOT run → the rx holder + inbound storage stay pinned (bounded fail-closed leak to process/
   firmware teardown). The object + fd wrapper are detached and reusable. `el_drain` never re-emits (the
   `terminal_emitted` gate); ctx teardown keeps the QUARANTINED op's ref (existing el_close policy).

**The load-bearing distinction:** DETACHED **transfers** the ref + retires the record + **releases**;
QUARANTINE **borrows** the ref + keeps the record `in_use` + **abandons** (never releases). Borrow-not-
transfer is what lets `retire_dgram` keep reporting QUARANTINED at join time (step 5) — a transfer +
record-retire would make `retire_dgram` return RETIRED and lose the classification.

## 3. Ownership of each life ref at every transition
| Transition | op ref | owner ref | recv_inflight |
|---|---|---|---|
| arm | held by op (`op->life`) | held | 1 |
| cancel requested | held by op | held | 1 |
| DETACHED terminal drained | transferred → event → **released** by dispatch | held | 0 |
| QUARANTINE terminal drained | **borrowed** by event; **stays** on `op->life` (abandoned) | held | 0 |
| free (DETACHED) | (already released) | released → `on_final` runs | — |
| free (QUARANTINE) | still on `op->life` (never released) | released; `on_final` deferred forever | — |

## 4. Why emitting the quarantine event is safe despite a live firmware op
The abandoned RxToken may still write the inbound buffer after we detach. Safety rests on three points:
- **Storage is pinned:** the op's ref is retained (never released), so `on_final` never runs and the
  inbound slot is never freed — a late firmware write lands in still-valid storage (no UAF).
- **No exposure:** the terminal carries `ok=0` and the recv machine is `stopped`, so `recv_on_complete`
  retires **without delivering** — the possibly-in-flux buffer is never handed to `on_recv`.
- **No double action:** `op->terminal_emitted` gates re-emission; the op stays `in_use` but is never
  re-drained, re-delivered, or re-released.

## 5. recv_inflight → 0 without releasing the quarantined ref
`kl_dgram_recv_on_complete(r, 0, 0)` sets `r->recv_inflight = 0` (machine accounting) and, for a
`stopped`/`!ok` op, returns without delivery. It does **not** touch the life ref — the life ref is the
dispatch's concern. So the machine retires purely from the event; the `quarantined` flag then tells the
dispatch to skip the release. Machine-retirement and ref-lifetime are already decoupled; this note only
adds "skip the release for a quarantined terminal."

## 6. Generic flag vs a narrower datagram-terminal disposition
Two shapes considered:
- **(A) `int quarantined` on `KlCompletionEvent`** (recommended). One field, meaningful only on a
  `KL_COMP_DGRAM_RECV` terminal, default 0 (release — the invariant everywhere else). Minimal; the
  dispatch reads it alongside `ev->ok`/`ev->life` it already reads.
- **(B) a datagram-terminal *disposition* enum** `ev->dgram_disp ∈ { DELIVER, RETIRE_RELEASE,
  RETIRE_ABANDON }`. More self-documenting and closes the "ok=0 could mean deliver-failure vs
  retire" ambiguity, but adds a datagram concept to the shared event and touches more call sites.

Recommendation: **(A)**, named to state intent — `int retain_life` (1 ⇒ dispatch must NOT release
`ev->life`; 0 ⇒ release as today). It is strictly narrower than a disposition enum and preserves the
existing `ok`/`bytes`/`buf` semantics. The invariant it amends is stated explicitly in completion.h.

## 7. `retire_dgram` results before and after the synthetic event
- DETACHED path: before drain PENDING (op `in_use`, PENDING/DELIVERED); after drain RETIRED (op gone).
- QUARANTINE path: before drain PENDING; after drain **QUARANTINED and stays QUARANTINED** (the op record
  is intentionally kept `in_use` with `op_state == QUARANTINED`). This is why `close_join_result`, which
  runs only once `recv_inflight == 0`, still sees QUARANTINED.

## 8. Event-reference ownership at EVERY release site (all owners/targets/routers)

**`retain_life` governs the completion event's ref ownership BEFORE routing, at every site that could
release `ev->life` — not just the two owner handlers.** Rule (single-sourced): a site releases `ev->life`
IFF `!ev->retain_life`; when `retain_life == 1` the ref is borrowed and the site must leave it alone
(the backend op retains it, fail-closed). The three release sites (audited §9a) all obey this:

- **Router — `completion_core.c` `kl_comp_run` (KL_COMP_DGRAM_*).** THE MISSED SITE (review). It routes
  by the token's dispatch handler and, for a token with **no handler or a NULL life**, releases the ref
  itself (`else if (life) kl_dgram_life_release(life)`). This decision is made BEFORE (and instead of)
  routing, so it MUST honour `retain_life` first:
  `if (d) d(target, &ev); else if (life && !ev.retain_life) kl_dgram_life_release(life);`
  — a quarantined borrowed terminal whose token has `dispatch == NULL` is therefore NOT released here.
- **KlDatagram** (`kl_datagram_comp_dispatch`): RECV: fill slot (skip for a terminal — `ok=0`, no data);
  `recv_on_complete(core, bytes, ok)`; then `if (!ev->retain_life) kl_dgram_life_release(ev->life)`.
- **Legacy KlUdp** (`kl_udp_comp_dispatch`): a `retain_life` terminal is only emitted for a
  `cancel_dgram`-cancelled op, which KlUdp never triggers, so it never receives one. It MUST still honour
  the flag uniformly (`if (!ev->retain_life) release`) so the invariant is single-sourced, not
  per-owner. KlUdp's existing ok=0 recv handling (retire the posted machine only while live) is unchanged.
- **Dead target** (owner freed; `kl_dgram_life_target()` NULL): the token still has a handler, so routing
  reaches the owner dispatch (which retires nothing for a NULL target) and honours `retain_life` there. A
  quarantined terminal to a dead target keeps the ref (already the fail-closed intent).
- **Unknown owner kind:** cannot occur (token kind is UDP/_DATAGRAM, set at create); the release decision
  is owner-independent (reads `ev->retain_life`), so routing correctness is unaffected.

The note's earlier claim "release behaviour is owner-independent" is now made TRUE by enforcement: it is
independent of the owner AND of the router-vs-handler path, because every site reads the same
`ev->retain_life`.

## 9. Event initialisation across every completion backend
`retain_life` MUST default 0 on every emitted event, or a stray 1 would leak refs. Audit: every backend
zero-inits each `KlCompletionEvent` before filling it — pollcomp/io_uring/IOCP use `memset`, EFI uses an
explicit byte loop, lwIP-raw uses `memset`. Requirement: keep zero-init mandatory; only the EFI drain's
QUARANTINED branch sets `retain_life = 1`. A one-time audit line per backend is part of the 7B-9 diff.

### 9a. Release-site audit (every consumer of `KlCompletionEvent.life`)
The 7B-9 diff must touch every site that releases a completion event's `ev->life`, not only the two owner
handlers. Audited (grep `kl_dgram_life_release` for `ev`/event-borne life):
- `src/completion_core.c` `kl_comp_run` KL_COMP_DGRAM_* no-handler fallback — **must gate on
  `!ev.retain_life`** (the missed site).
- `src/datagram.c` `kl_datagram_comp_dispatch` — gate on `!ev->retain_life`.
- `src/udp.c` `kl_udp_comp_dispatch` — gate on `!ev->retain_life`.
Explicitly OUT of scope (not event-borne — these release a `op.life` retained by the CALLER before a
`post_dgram_*`, released on POST FAILURE, never a completion event): `src/udp.c` submit/arm failure
releases, `src/datagram.c` submit/arm failure releases, and KlUdp's internal owner-ref releases
(`kl_udp_free` teardown). These do not read `retain_life`; the review must confirm none is a disguised
`ev->life` release.

## 10. Context teardown + late firmware completion
- **Teardown (`el_close`):** the existing loop already RETAINS (abandons) a QUARANTINED op's ref and
  RELEASES a STALE_RETIRED/RETIRED op's — unchanged. A quarantined op left `in_use` at teardown is handled
  by that policy (retain). No new leak beyond the intended fail-closed one.
- **Late firmware completion:** a post-detach RxToken signal lands in the pinned storage (§4); the op is
  never re-drained (`terminal_emitted` + the fd/slot already quarantined), so no second event/ref action.

## 11. Test matrix (host-mock `mock_efi_test.c`, ASan/UBSan; + QEMU/OVMF e2e)
1. **Empty cancellation → DETACHED:** recv armed, no datagram, `close_begin`, `g_cancel_signals=1` →
   pump → `close_state==CLOSED`, `close_result==DETACHED`, `on_recv` never fired, `on_final` ran once.
2. **Quarantine → terminal QUARANTINED:** recv armed, unconfirmed cancel (`g_cancel_signals=0`) → pump →
   `close_result==QUARANTINED`, `on_recv` never fired.
3. **Retained storage/ref:** under (2), a counting allocator proves `on_final` does NOT run after
   `on_close(QUARANTINED)` + `free` (the op ref is retained); reclaimed via the arena (LSan-clean).
4. **No callback delivery:** (1)+(2) assert the terminal never delivers a datagram to `on_recv`.
5. **No double terminal / double release:** pump repeatedly after (1)/(2) → exactly one DGRAM_RECV
   terminal per op; `retire_dgram` stable (RETIRED after (1); QUARANTINED after (2)); refcount exact.
5a. **No-dispatch-handler quarantine (the router site):** drive a `KL_COMP_DGRAM_RECV(retain_life=1)`
   event whose token has `dispatch == NULL` straight through `kl_comp_run` (a token created without a
   handler, as KlDgramCore's neutral-adapter tokens are); assert `completion_core.c` does NOT release the
   borrowed ref (refcount unchanged) — proving the router site honours `retain_life`, not only the owner
   handlers. A `retain_life=0` no-handler event still releases (unchanged).
6. **Roundtrip + clean close** (a datagram delivered, then DETACHED) unchanged.
7. **KlUdp-over-EFI unaffected:** the existing e2e KlUdp cases stay green (never take the `retain_life`
   path).
8. **QEMU/OVMF e2e:** a KlDatagram roundtrip + clean DETACHED close on bare OVMF (extends the existing
   dgram-DNS harness) once the host-mock matrix is green.

## 12. Change surface (once approved)
- `src/completion.h`: `int retain_life;` on `KlCompletionEvent` + the amended invariant note (the
  transferred ref is released after dispatch UNLESS `retain_life`, at every release site).
- `src/completion_core.c` (`kl_comp_run` KL_COMP_DGRAM_* no-handler fallback): gate the release on
  `!ev.retain_life` — the router release site (§8, the missed one).
- `src/datagram.c` (`kl_datagram_comp_dispatch`) + `src/udp.c` (`kl_udp_comp_dispatch`): honour
  `retain_life`.
- `integrations/uefi/socket_efi_udp4.{h,c}`: **no new primitive** (the §0a correction dropped
  `kl_uefi_udp_request_recv_cancel` — `el_cancel_dgram` keeps the existing `kl_uefi_udp_cancel_recv`).
- `integrations/uefi/event_efi.c`: `el_cancel_dgram(RECV)` UNCHANGED (synchronous `cancel_recv`); `el_drain`
  recv branch emits a terminal for **DELIVERED or STALE_RETIRED** [ok=0 on non-delivery; transfer+release]
  and **QUARANTINED** [borrow+retain, keep `in_use`, `terminal_emitted` gate]; INVALID stays fail-safe
  (retain+retire); `EfiDgramOp` gains `terminal_emitted`.
- backends: confirm zero-init (audit, §9) — all memset/byte-loop before fill; only EFI's QUARANTINED
  branch sets `retain_life=1`. No change needed in pollcomp/io_uring/IOCP/lwIP-raw.
- `integrations/uefi/mock_efi_test.c`: the §11 cases (public DETACHED/QUARANTINED e2e + the no-handler
  router site); the pre-existing STALE_RETIRED/cancel-idempotent unit cases updated to the new
  "STALE_RETIRED emits a transferred terminal" contract.
- `docs/datagram_contract.md` §10: flip EFI cells only after host-mock + QEMU pass.
