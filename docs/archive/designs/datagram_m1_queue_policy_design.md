# Datagram M1, the `BOTH` (byte-gate) send-queue policy, design freeze

Status: **ACCEPTED (docs-only, revision 3); ready for implementation.** Per the consolidation
workflow. Sibling of the accepted M0 (`datagram_open.c`) and synchronous-teardown
(`docs/datagram_sync_teardown_design.md`) freezes. Authority for the policy fork is
`docs/datagram_consolidation_design.md` §4 (Decisions D-Q-1..D-Q-4); this freeze turns §4 into an
implementable increment and pins the edges §4 leaves open.

**Revision 2** resolves the three review blockers on revision 1: (P1a) the zero-length accounting
equivalence was false, dropped; (P1b) appending to `KlDatagramConfig` is an ABI break, replaced with
a new additive `kl_datagram_init_ex`, `KlDatagramConfig` frozen unchanged; (P1c) `budget ≥ slot_cap`
could not preserve arbitrary `KlUdp.max_send_queue` and `budget < slot_cap` could strand an empty
queue: replaced with a KlUdp-exact rule (direct-send-first, then permanent refusal of a datagram that
alone exceeds the budget), so any `budget > 0` is valid and nothing strands. Reviewer rulings applied:
single budget value (no enum); threaded through `kl_dgram_send_init` (no setter); hard byte limit (no
"always admit when empty"); the config change treated as a real STABLE-contract decision.

**Revision 3** applies the final review correction (P2): the abortive-discard accounting test (§10.9)
must not assume `bytes_used == 0` when a send is in flight; `kl_dgram_send_discard_queued` retains the
in-flight head, so it leaves `bytes_used == inflight_len` until the terminal completion retires it
(`abandon` still resets directly to 0, as a dead machine takes no late completion). The status-value
call is resolved: **reuse `KL_DATAGRAM_TOO_LARGE`** ("exceeds a hard configured send-path capacity,
`slot_cap` or the whole `byte_budget`"), no new enumerator. The freeze is accepted and ready for
implementation.

## 0. Scope

**M1 is additive and changes no consumer.** It adds a second admission policy to the existing
allocation-free (I8) send core; the default is unchanged (`SLOT`), DNS (M3) stays on `SLOT`, and **no
existing public function or type changes**, the only new surface is one new function
(`kl_datagram_init_ex`) and one new internal parameter. M1 is a prerequisite for M4 (`KlUdpServer` uses
`BOTH`); it does **not** depend on M2.

**In scope:** the `BOTH` policy over the fixed `send_slots × send_slot_cap` slot array, a scalar byte
**admission gate** layered on the existing count gate; the byte accounting rule and all its mutation
sites; the KlUdp-exact handling of a datagram that exceeds the budget; the backpressure edges under
`BOTH`; the new additive initializer; the doc/contract reconciliation.

**Explicitly out of scope (retained elsewhere by design):** a pure byte-only, count-unbounded queue
(a **proven impossibility** for an I8 core, §4; zero-length datagrams make a byte budget unable to
bound the datagram count). Exact `KlUdp` byte-only/count-unbounded parity stays in the **M5 compat
wrapper**'s existing malloc-per-node queue, untouched. M1 adds **no** hot-path allocation.

## 1. The two policies (recap of §4, for self-containment)

Storage is unchanged: one datagram per contiguous `slot_cap`-byte slot, all preallocated at init; **no
ring, no wrap, no fragmentation**; any admitted datagram (`len ≤ slot_cap`) is storable by
construction.

- **`SLOT`** (default, today): admit iff a free slot exists. Count-bounded only.
- **`BOTH`**: admit iff a free slot exists **AND** the byte gate passes (§4). Bounded in **both**
  dimensions. `bytes_used` is a scalar admission counter; it **never** governs storage layout.

There is deliberately **no pure byte-only core policy** (§4 impossibility). `BOTH` keeps the slot count
as the count bound precisely so that unboundedly-many zero-length datagrams cannot evade the bound.

## 2. API surface: a new additive initializer (Decision D-M1-1, blocker P1b)

`KlDatagramConfig` is part of the frozen public type contract (STABLE, 7B-10). Appending a field is an
**ABI break**, a previously-compiled caller passes the old, smaller object while the new library reads
past it. So `KlDatagramConfig` is **left byte-for-byte unchanged**, and the budget arrives through a
new function:

```c
/* Existing: UNCHANGED signature and semantics; now permanently the SLOT policy. */
int kl_datagram_init   (KlDatagram *dg, const KlDatagramConfig *cfg);

/* NEW additive symbol: SLOT when send_byte_budget == 0, BOTH when > 0. */
int kl_datagram_init_ex(KlDatagram *dg, const KlDatagramConfig *cfg, size_t send_byte_budget);
```

- `kl_datagram_init(dg, cfg)` is defined as `kl_datagram_init_ex(dg, cfg, 0)`: **permanently `SLOT`**.
  Every existing caller and the STABLE `KlDatagramConfig` layout are untouched; no recompile, no ABI
  change.
- `kl_datagram_init_ex(dg, cfg, budget)` selects `BOTH` with `budget` (bytes of accumulated
  queued+in-flight payload) when `budget > 0`, `SLOT` when `budget == 0`.

Single budget value, not an enum+value (reviewer ruling): a budget of 0 is a self-describing "no byte
gate," and a lone `size_t` parameter cannot express the invalid `(SLOT, budget>0)` /`(BOTH, budget=0)`
combinations an enum would. The value threads to the internal seam with **no** further public change:
`kl_datagram_init_ex(budget)` → `KlDgramCoreConfig.send_byte_budget` → `kl_dgram_send_init(..., size_t
byte_budget)` (a new trailing parameter, reviewer ruling: init param, no setter) → `KlDgramSend`.

The public status enum `KlDatagramSendStatus` is **unchanged** (§4 reuses `KL_DATAGRAM_TOO_LARGE`; no
new enumerator).

## 3. Accounting model (Decision D-M1-2, blocker P1a)

Add two scalars to `KlDgramSend`: `size_t byte_budget;` (0 = gate off) and `size_t bytes_used;`.

**Rule:** `bytes_used == Σ slot->len over every OCCUPIED slot (queued + the single in-flight)`. It rises
exactly when a slot is enqueued and falls exactly when a slot is released. It is a pure function of
slot occupancy. The complete set of mutation sites (all in `datagram_send.c`):

| Site | Δ `bytes_used` | Notes |
|---|---|---|
| enqueue in `kl_dgram_send` (slot acquired + filled) | `+= m->len` | zero-length adds 0 |
| retire in `send_pump` DONE (readiness sync send of a queued slot) | `-= slot->len` | before the slot returns to the pool |
| retire in `kl_dgram_send_on_complete` (ok **or** err) | `-= slot->len` | both retire the in-flight slot |
| `kl_dgram_send_discard_queued` (abortive close) | `-= slot->len` each | per discarded queued slot |
| `kl_dgram_send_abandon` / `kl_dgram_send_free` | `bytes_used = 0` | ring zeroed / object reset |

**`bytes_used` is NOT an emptiness predicate (blocker P1a, the revision-1 claim was false).** A
zero-length datagram occupies a slot but contributes 0 bytes, so `bytes_used == 0` can coexist with
`count > 0`. **`count` remains the sole drain/emptiness predicate**; `on_drain` fires only on
`count`: N→0, never on `bytes_used`→0. `bytes_used` is used **only** by the admission gate (§4) and is
never consulted for the drain edge or for "is the queue empty."

## 4. Admission algorithm (Decision D-M1-3, blocker P1c)

KlUdp is the compatibility target and defines the exact semantics (verified `src/udp.c:84-166`):

- **Readiness:** empty queue → **direct send first** (`udp_send_common:158`); a datagram larger than
  `max_send_queue` is *sent* if the socket is ready. On `WOULD_BLOCK` it enqueues, and `udp_enqueue`
  (`:87`) **drops** (`KL_ERR_QUEUE_FULL`) any datagram with `len > max_send_queue`.
- **Completion:** no direct-send path; `len > max_send_queue` is refused **upfront**
  (`udp_send_common:136`, `KL_ERR_IO`).

The core already mirrors that model/asymmetry (readiness has a direct fast path; completion always
posts a slot). So `BOTH` maps onto it exactly, in `kl_dgram_send`:

```
... existing UNSUPPORTED checks ...
if (m->len > slots->out_cap) return TOO_LARGE;      /* len > slot_cap: hard storage limit, unchanged */
send_enter(s);
/* readiness fast path (count==0): direct synchronous send, NO slot, NO bytes; UNCHANGED.
   A datagram the socket accepts now is ACCEPTED regardless of byte_budget (KlUdp parity). */
if (!s->completion && s->count == 0) { ... DONE->ACCEPTED / ERROR->ERROR / WOULD_BLOCK->fall through ... }

/* byte gate (BOTH only): reached on the readiness WOULD_BLOCK fall-through, or directly in
   completion mode (no fast path). Two outcomes, split on whether the datagram alone exceeds budget: */
if (s->byte_budget) {
    if (m->len > s->byte_budget) {                  /* (a) exceeds the WHOLE budget → can NEVER queue */
        status = KL_DATAGRAM_TOO_LARGE;             /*     PERMANENT refusal (do NOT set full)         */
        goto leave;                                 /*     not retryable → cannot strand               */
    }
    if (m->len > s->byte_budget - s->bytes_used) {  /* (b) transient: fits budget but not right now    */
        s->full = 1;                                /*     arm full->non-full; a retirement frees bytes */
        status  = KL_DATAGRAM_WOULD_BLOCK;
        goto leave;
    }
}
slot = kl_dgram_slots_acquire(s->slots);            /* count gate (unchanged) */
if (!slot) { s->full = 1; status = KL_DATAGRAM_WOULD_BLOCK; goto leave; }
... fill slot ...; s->bytes_used += m->len; ...
```

- **(a) `len > byte_budget` → permanent `TOO_LARGE`.** A datagram that alone exceeds the entire budget
  can never be queued (no amount of draining makes room), so a retryable `WOULD_BLOCK` here would
  either strand (empty queue: `count==0`, no WRITE armed, `on_writable` never fires) or livelock
  (retry always re-refuses under the same budget). It is therefore **terminal**. Reached only *after*
  the readiness fast path, so a socket-ready send still delivers it (KlUdp parity: sent-if-ready,
  refused-if-blocked-and-oversize). On completion mode there is no fast path, so it is refused upfront
  , exactly as KlUdp does on a completion loop. **`TOO_LARGE` is reused** (redefined: "exceeds a hard
  send-path capacity, the per-datagram `slot_cap`, or, under `BOTH`, the whole `byte_budget`"),
  keeping the public status enum unchanged.
- **(b) `bytes_used + len > byte_budget` with `len ≤ byte_budget` → transient `WOULD_BLOCK`, and it
  provably cannot strand.** `len ≤ byte_budget` ∧ `bytes_used + len > byte_budget` ⟹ `bytes_used >
  byte_budget − len ≥ 0` ⟹ `bytes_used > 0` ⟹ at least one non-empty occupied slot ⟹ `count ≥ 1`. With
  `count ≥ 1`, readiness has WRITE armed (`dg_reconcile_write`) and completion has an in-flight/queued
  op whose retirement fires `on_writable`, so the retry signal always arrives. Overflow-safe form
  (`len > byte_budget − bytes_used`) avoids any `bytes_used + len` wrap.

**Consequence: `budget ≥ slot_cap` is dropped (blocker P1c).** Any `budget > 0` is valid, including
`budget < slot_cap`: a `slot_cap`-sized datagram simply falls into case (a) (sent if the socket is
ready, else permanently refused), exactly as KlUdp treats a datagram larger than `max_send_queue`.
Arbitrary `KlUdp.max_send_queue` values migrate to M4 **unchanged**: no headroom inflation, no
minimum-budget rejection, no migration break.

## 5. Backpressure edges under `BOTH` (Decision D-M1-4)

- **`full` latch / `on_writable` (full→non-full).** `full` latches on a transient refusal; no free
  slot **or** case-(b) byte-gate, and the edge fires on **any** retirement while latched (a
  retirement frees both a slot and `slot->len` bytes). The consumer's `on_writable` retries and may
  re-latch (level-triggered "capacity released, retry," identical to `SLOT`). No size-exact "will the
  next datagram fit" prediction. The permanent case (a) does **not** set `full` (a writable edge
  cannot help).
- **`on_drain` (non-empty→empty).** Unchanged and driven **solely by `count`** (§3): fires only when
  `count` reaches 0. Never keyed on `bytes_used` (which may already be 0 with zero-length datagrams
  still occupying slots).

## 6. No forward-progress special case (Decision D-M1-5)

There is **no** `budget ≥ slot_cap` requirement and **no** "always admit when empty" rule (reviewer
ruling: preserve the hard byte limit). Forward progress is guaranteed structurally instead: the only
un-retryable condition (`len > byte_budget`, case (a)) is made **terminal** rather than a stranding
`WOULD_BLOCK`, and every `WOULD_BLOCK` the core returns (case (b) or slot-full) provably has `count ≥
1` (§4) and therefore an armed writable/retirement signal. `kl_datagram_init_ex` accepts any `size_t`
budget; `budget == 0` is `SLOT`.

## 7. Consumer neutrality + the M4 mapping (Decision D-M1-6)

- Default is `SLOT` (`kl_datagram_init`); **no existing consumer changes**. DNS (M3) keeps `SLOT` (its
  transient send-burst is count-shaped; the unbounded in-flight-awaiting-response list is a
  recv/logical concern, not the send queue: §4 D-Q-4).
- `BOTH` gains its first user in **M4**, where `KlUdpServer` builds on the core with
  `send_slot_cap = 65507` (max UDP payload) and `send_byte_budget = max_send_queue` (**verbatim**, no
  inflation). Because every real UDP datagram has `len ≤ 65507 = slot_cap`, the `slot_cap` `TOO_LARGE`
  is inert and the byte budget governs. The `KlUdpServer.reply` wrapper (M4) maps the core status to
  the preserved `int` return: `ACCEPTED → 0`; `WOULD_BLOCK`/`TOO_LARGE` (budget) → `-1`
  (`KL_ERR_QUEUE_FULL`, the drop KlUdp already reports); `ERROR → -1`. Observable KlUdpServer behavior
  (reply sent if capacity, else dropped with `-1`) is **unchanged** from today; the internal core
  contract is lossless-refuse while the wrapper presents KlUdp's lossy-drop surface. M1 only builds and
  tests the mechanism; it wires no consumer to it.

## 8. Internal threading

`kl_datagram_init_ex(dg, cfg, budget)` (`datagram.c`) copies `budget` into
`KlDgramCoreConfig.send_byte_budget`; `kl_datagram_init` calls it with 0. `datagram_core.c
kl_dgram_core_init` forwards it to `kl_dgram_send_init(..., byte_budget)`. The slots pool, recv
machine, close coordinator, life token, and both I/O-model adapters are **untouched**.

## 9. Documentation / contract reconciliation (ships with the code, not before)

- `include/keel/datagram.h:133`: "Backpressure is a datagram COUNT (send_slots), NOT a byte budget." →
  reword to note the two policies (COUNT under `SLOT` via `kl_datagram_init`; count-**and**-byte via
  `kl_datagram_init_ex` with `send_byte_budget > 0`, `BOTH`), and redefine `KL_DATAGRAM_TOO_LARGE` as
  "exceeds a hard send-path capacity: `slot_cap`, or the whole `byte_budget` under `BOTH`."
- `include/keel/datagram.h`: document `kl_datagram_init_ex` (0 = `SLOT`, >0 = `BOTH`; any budget
  valid).
- `docs/datagram_contract.md` §1 send-side backpressure: two policies; the core is count-bounded under
  `SLOT`, count-and-byte-bounded under `BOTH`; no pure byte-only core; `TOO_LARGE` covers the budget
  limit.
- `docs/datagram_consolidation_design.md` §4/§6: mark M1 landed; the M4 mapping is `max_send_queue`
  verbatim (correct the earlier "+ headroom" wording, no inflation).
- **STABLE contract (7B-10), the real decision (reviewer ruling, not a footnote).** `KlDatagramConfig`
  and `kl_datagram_init` keep byte-for-byte / signature-for-signature stability. `kl_datagram_init_ex`
  is a **new additive symbol**, purely a contract *extension*, never a modification: no existing
  caller, object layout, or signature is touched, so the STABLE guarantee is preserved by construction
  rather than waived. The banner gains one line documenting `kl_datagram_init_ex` as the additive
  `BOTH` entry point.

## 10. Test matrix (`tests/test_datagram_send.c` unit level + a public-facade case)

Over the existing scripted send/submit mock, both I/O models where the edge differs:

1. **`SLOT` unchanged**, via `kl_datagram_init` / budget 0: every existing send test passes verbatim
   (regression guard: default policy byte-identical).
2. **`BOTH` byte-gate refusal (case b)**: budget sized for K datagrams; the (K+1)-th refuses
   `WOULD_BLOCK` **while a slot is still free** (byte gate, not slot gate) and `count ≥ 1` so
   `on_writable` later fires.
3. **`BOTH` slot-gate still active**: many small datagrams under budget exhaust **slots** first →
   `WOULD_BLOCK` (count bound survives under `BOTH`).
4. **Zero-length under `BOTH`**: `send_slots` zero-length datagrams admit (0 bytes each, `bytes_used`
   stays 0), the next refuses on the **slot** bound (proves slots bound zero-length), and **`on_drain`
   fires on `count`→0 with `bytes_used` already 0 throughout** (blocker-P1a regression: drain keyed on
   count, not bytes).
5. **`len > byte_budget` permanent refusal (case a), readiness**: on an **empty** queue with the mock
   submit returning `WOULD_BLOCK`, an oversize (`len > budget`) send returns `TOO_LARGE`, sets **no**
   `full`, arms **no** WRITE, and never strands; with the mock returning `DONE` the same send is
   `ACCEPTED` (sent-if-ready parity).
6. **`len > byte_budget` permanent refusal (case a), completion**: refused `TOO_LARGE` upfront (no
   fast path), `count` unchanged.
7. **`budget < slot_cap` is valid**: `kl_datagram_init_ex` with `0 < budget < slot_cap` succeeds; a
   `budget`-fitting datagram admits, a `> budget` (but `≤ slot_cap`) datagram is case (a): proves the
   dropped `budget ≥ slot_cap` requirement.
8. **Byte accounting reopens admission**: fill the budget, retire one queued datagram, confirm
   `bytes_used` dropped by exactly its `len` and a same-size send now admits.
9. **`discard_queued` / `abandon` accounting (blocker P2, in-flight is retained).**
   `kl_dgram_send_discard_queued` deliberately keeps the in-flight head (`count > inflight_n`), so it
   reduces `bytes_used` by exactly the discarded **queued** slots and **leaves the in-flight slot's
   length accounted** until its terminal completion retires it. Three sub-cases:
   - **queued-only / readiness discard** (nothing in flight) → `bytes_used == 0` immediately after
     discard;
   - **in-flight + queued, completion** → discard leaves `bytes_used == inflight_len` (the retained
     head), then the terminal `kl_dgram_send_on_complete` reduces it to `0`;
   - **`abandon`** → `bytes_used == 0` directly (the machine is dead; a late completion cannot re-enter
     it, so the in-flight length need not be tracked to a completion that will never dispatch).
   Each sub-case run with a zero-length-mixed queue as well (a retained zero-length in-flight head
   leaves `bytes_used == 0` after discard yet `count == 1` until completion, the P1a/P2 interaction).
10. **Public facade**: one `test_datagram_public` case constructing `BOTH` via `kl_datagram_init_ex`
    end-to-end (budget admission + case-(a)/(b) split through the real facade, scripted completion
    mock), plus one asserting `kl_datagram_init` is `SLOT` (budget 0).

## 11. Validation plan

- macOS default (kqueue readiness) `make test`: all suites + the new send cases, ASan/UBSan.
- `BACKEND=pollcomp`, the completion-model edges (case (a) upfront refusal; in-flight slot counts
  toward `bytes_used`).
- Linux container `BACKEND=iouring` under ASan/UBSan/**LSan**, the byte-gate path holds no memory and
  leaks nothing (no hot-path allocation added; regression against the I8 guarantee).
- Gates: `check-tier1-boundary`, `check-sockaddr-neutral`, `check-doc-refs`, `cppcheck` (the overflow
  guard in §4 is exactly cppcheck's remit), EFI host-mock unaffected (no EFI change), freestanding
  datagram build (`kl_datagram_init_ex` present, budget inert there).

## 12. Rulings applied + the one remaining semantics call

Reviewer rulings from revision 1, now frozen into the design:
- **Single budget value** (no enum): §2.
- **Threaded through `kl_dgram_send_init`** (no setter): §2/§8.
- **Hard byte limit; no "always admit when empty"**: §4/§6 (case (a) is terminal, not an admit).
- **Public-config change treated as a real STABLE decision**, §2/§9: `KlDatagramConfig` frozen; the
  budget arrives via the new additive `kl_datagram_init_ex`, not a struct field.

Status-value decision, **resolved (confirmed at review):**
- **D-M1-status: reuse `KL_DATAGRAM_TOO_LARGE` for case (a) (`len > byte_budget`).** The public
  status enum (itself STABLE) is unchanged; `TOO_LARGE` means "exceeds a hard configured send-path
  capacity: `slot_cap`, or the whole `byte_budget` under `BOTH`." A new `KL_DATAGRAM_BUDGET_EXCEEDED`
  enumerator was rejected: it grows the public enum without improving M4 behavior (the
  `KlUdpServer` surface is `-1` / `KL_ERR_QUEUE_FULL` either way).

No open decisions remain; the freeze is ready for implementation.
