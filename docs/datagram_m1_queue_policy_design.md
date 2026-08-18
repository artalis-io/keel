# Datagram M1 — the `BOTH` (byte-gate) send-queue policy — design freeze

Status: **PROPOSED (docs-only)** — no code until reviewed and accepted, per the consolidation
workflow. Sibling of the accepted M0 (`datagram_open.c`) and synchronous-teardown
(`docs/datagram_sync_teardown_design.md`) freezes. Authority for the policy fork is
`docs/datagram_consolidation_design.md` §4 (Decisions D-Q-1..D-Q-4); this freeze turns §4 into an
implementable increment and pins the edges §4 leaves open (accounting sites, forward-progress,
backpressure edges, the one ABI touch).

## 0. Scope

**M1 is additive and changes no consumer.** It adds a second admission policy to the existing
allocation-free (I8) send core; the default is unchanged (`SLOT`), DNS (M3) stays on `SLOT`, and no
public function signature changes. The only new surface is one appended config field (§2). M1 is a
prerequisite for M4 (`KlUdpServer` uses `BOTH`); it does **not** depend on M2.

**In scope:** the `BOTH` policy over the fixed `send_slots × send_slot_cap` slot array — a scalar byte
**admission gate** layered on the existing count gate; the byte accounting invariant and all its
mutation sites; the backpressure edges under `BOTH`; init validation; the doc/contract reconciliation.

**Explicitly out of scope (retained elsewhere by design):** a pure byte-only, count-unbounded queue
(a **proven impossibility** for an I8 core — §4; zero-length datagrams make a byte budget unable to
bound the datagram count). Exact `KlUdp` byte-only/count-unbounded parity stays in the **M5 compat
wrapper**'s existing malloc-per-node queue, untouched. M1 adds **no** hot-path allocation.

## 1. The two policies (recap of §4, for self-containment)

Storage is unchanged: one datagram per contiguous `slot_cap`-byte slot, all preallocated at init — **no
ring, no wrap, no fragmentation**; any admitted datagram (`len ≤ slot_cap`) is storable by
construction.

- **`SLOT`** (default, today): admit iff a free slot exists. Count-bounded only.
- **`BOTH`**: admit iff a free slot exists **AND** `bytes_used + len ≤ byte_budget`. Bounded in **both**
  dimensions. `bytes_used` is a scalar admission counter; it **never** governs storage layout.

There is deliberately **no pure byte-only core policy** (§4 impossibility). `BOTH` keeps the slot count
as the count bound precisely so that unboundedly-many zero-length datagrams cannot evade the bound.

## 2. API surface — the single additive field (Decision D-M1-1)

Append one field to the public `KlDatagramConfig` (`include/keel/datagram.h`):

```c
typedef struct {
    struct KlEventCtx       *ctx;
    KlAllocator             *alloc;
    const struct KlSocketProvider *sockets;
    KlSocketHandle           fd;
    size_t                   send_slots;
    size_t                   send_slot_cap;
    size_t                   recv_cap;
    unsigned                 want_caps;
    size_t                   send_byte_budget;  /* NEW: 0 = SLOT (default); >0 = BOTH with this budget */
} KlDatagramConfig;
```

- **`send_byte_budget == 0` ⇒ `SLOT`** — byte-identical to today. Every existing consumer (DNS/M3, the
  live/public tests) is zero-initialized via designated initializers, so they select `SLOT`
  unchanged with no edit.
- **`send_byte_budget > 0` ⇒ `BOTH`** with that budget (in bytes of accumulated queued+in-flight
  payload).

No named-enum policy selector is added: a byte budget of 0 is a self-describing "no byte gate," and an
enum would admit invalid `(policy=SLOT, budget>0)` / `(policy=BOTH, budget=0)` combinations that this
single field makes unrepresentable. (Alternative considered in §12 D-M1-1 for the reviewer.)

The same field threads down through the internal seams (no public change beyond the above):
`KlDatagramConfig.send_byte_budget` → `KlDgramCoreConfig.send_byte_budget` → `kl_dgram_send_init(...)` →
`KlDgramSend.byte_budget`.

## 3. Accounting model (Decision D-M1-2)

Add two scalars to `KlDgramSend`: `size_t byte_budget;` (0 = gate off) and `size_t bytes_used;`.

**Invariant:** `bytes_used == Σ slot->len over every OCCUPIED slot (queued + the single in-flight)`.
Equivalently, `bytes_used` rises exactly when a slot is enqueued and falls exactly when a slot is
released back to the pool. It is a pure function of slot occupancy — it can never drift from the slots
if every release site is covered. The complete set of mutation sites (all in `datagram_send.c`):

| Site | Δ `bytes_used` | Notes |
|---|---|---|
| enqueue in `kl_dgram_send` (slot acquired + filled) | `+= m->len` | zero-length adds 0 (bounded by the slot count) |
| retire in `send_pump` DONE (readiness sync send of a queued slot) | `-= slot->len` | before the slot returns to the pool |
| retire in `kl_dgram_send_on_complete` (ok **or** err) | `-= slot->len` | both retire the in-flight slot |
| `kl_dgram_send_discard_queued` (abortive close) | `-= slot->len` each | per discarded queued slot |
| `kl_dgram_send_abandon` / `kl_dgram_send_free` | `bytes_used = 0` | ring zeroed / object reset |

Because `bytes_used` tracks occupancy exactly, `bytes_used == 0` **iff** `count == 0` — so it needs no
separate empty-detection and cannot desync the `on_drain` (non-empty→empty) edge.

## 4. Admission algorithm (Decision D-M1-3)

The byte gate is a **transient refusal** (`WOULD_BLOCK`), evaluated in `kl_dgram_send` **after** the
permanent `TOO_LARGE` check and the readiness fast path, **at** the slot-acquisition point:

```
... existing UNSUPPORTED checks ...
if (m->len > slots->out_cap) return TOO_LARGE;      /* permanent, unchanged (D-Q-3) */
send_enter(s);
/* readiness fast path (count==0): direct synchronous send, NO slot, NO bytes accounted — unchanged */
...
/* byte gate (BOTH only) — checked before acquiring a slot; overflow-safe form */
if (s->byte_budget && (s->bytes_used > s->byte_budget || m->len > s->byte_budget - s->bytes_used)) {
    s->full = 1;                     /* arm the full→non-full edge (a retirement frees budget) */
    status  = KL_DATAGRAM_WOULD_BLOCK;
    goto leave;
}
slot = kl_dgram_slots_acquire(s->slots);
if (!slot) { s->full = 1; status = KL_DATAGRAM_WOULD_BLOCK; goto leave; }
memcpy(slot->data, ...); slot->len = m->len; ...
s->bytes_used += m->len;             /* enqueue accounting (paired with every release in §3) */
...
```

- The **readiness fast path** (empty queue, direct submit) queues nothing, so it neither consults nor
  mutates `bytes_used`. A `WOULD_BLOCK` fall-through then reaches the byte gate with the real queue
  state — correct.
- Overflow-safe: `bytes_used ≤ budget` and `len ≤ slot_cap ≤ budget` (§6), but the gate is written as
  `len > budget - bytes_used` to avoid any theoretical `bytes_used + len` wrap.
- Ordering of the byte gate vs the slot gate is immaterial (both refuse `WOULD_BLOCK`, take no
  ownership, mutate no queue state); the byte gate is placed first so a budget-exhausted send never
  even acquires a slot.

## 5. Backpressure edges under `BOTH` (Decision D-M1-4)

- **`full` latch / `on_writable` (full→non-full).** Today `full` latches when a slot cannot be
  acquired and the edge fires on the next retirement (a slot frees). Under `BOTH`, `full` **also**
  latches on a byte-gate refusal, and the edge fires on **any** retirement while latched — because a
  retirement frees both a slot and `slot->len` bytes of budget. The consumer's `on_writable` handler
  retries and may re-latch (level-triggered "try again" semantics, identical to `SLOT`). The machine
  does **not** attempt a size-exact "will the next datagram fit" prediction (the next datagram's size
  is unknown) — it signals "capacity was released, retry," which is the existing contract.
- **`on_drain` (non-empty→empty).** Unchanged: fires only when `count` reaches 0, which coincides
  exactly with `bytes_used` reaching 0 (§3). No byte-specific drain edge.

## 6. Init validation — forward-progress guarantee (Decision D-M1-5)

`kl_dgram_send_init` / `kl_datagram_init` **require** `send_byte_budget == 0 || send_byte_budget ≥
send_slot_cap`; otherwise init fails (`-1`, `KL_ERR_INVALID_ARG`, object left zeroed/reusable per the
existing contract).

Rationale — **no livelock by construction:** with `budget ≥ slot_cap`, an empty-queue admit
(`bytes_used == 0`) always satisfies `len ≤ slot_cap ≤ budget`, so `BOTH` can never wedge on the sole
datagram; the byte gate refuses **only** when bytes are already queued. This is cleaner than a
special-case "always admit the first datagram" rule and makes the sole datagram's storability
(`len ≤ slot_cap`) and admissibility (`len ≤ budget`) the same condition. M4's mapping satisfies it
naturally (`slot_cap` = max UDP payload; `byte_budget` = `max_send_queue` + headroom ≥ `slot_cap`).

## 7. Consumer neutrality (Decision D-M1-6)

- Default is `SLOT` (`send_byte_budget = 0`); **no existing consumer changes**. DNS (M3) keeps `SLOT`
  (its transient send-burst is count-shaped; the unbounded in-flight-awaiting-response list is a
  recv/logical concern, not the send queue — §4 D-Q-4).
- `BOTH` gains its first user in **M4** (`KlUdpServer`: byte budget from `max_send_queue` + a
  reply-burst slot count), with the documented count-and-byte bound (§4 D-Q-2, reviewer option 2). M1
  only builds and tests the mechanism; it wires no consumer to it.

## 8. Internal threading

`KlDatagramConfig.send_byte_budget` → `datagram.c kl_datagram_init` copies it into
`KlDgramCoreConfig.send_byte_budget` → `datagram_core.c kl_dgram_core_init` forwards it to
`kl_dgram_send_init` (new trailing `size_t byte_budget` parameter, or a `kl_dgram_send_set_budget`
setter called before first use — implementation detail, §12 D-M1-2). The slots pool, recv machine,
close coordinator, life token, and both I/O-model adapters are **untouched**.

## 9. Documentation / contract reconciliation (ships with the code, not before)

- `include/keel/datagram.h:133`: "Backpressure is a datagram COUNT (send_slots), NOT a byte budget." →
  reword to "Backpressure is a datagram COUNT (`send_slots`) by default (`SLOT`), or count-**and**-byte
  bounded when `send_byte_budget > 0` (`BOTH`)."
- `include/keel/datagram.h` `KlDatagramConfig`: document the new field inline (0 = `SLOT`, >0 = `BOTH`,
  requires `≥ send_slot_cap`).
- `docs/datagram_contract.md` §1 send-side backpressure: note the two policies (the core is
  count-bounded under `SLOT`, count-and-byte-bounded under `BOTH`; no pure byte-only core).
- `docs/datagram_consolidation_design.md` §4/§6: mark M1 landed; leave the M4 mapping as-is.
- **STABLE-banner note (7B-10):** appending `send_byte_budget` grows `sizeof(KlDatagramConfig)`. The
  STABLE guarantee on the **existing** fields and every public function signature is preserved; the new
  field is purely additive and its zero value reproduces prior behavior exactly. Whether the appended
  field warrants a banner footnote is a reviewer call (§12 D-M1-3). In-tree there is no external ABI
  consumer yet (pre-1.0); all call sites are zero-initialized designated initializers.

## 10. Test matrix (`tests/test_datagram_send.c` unit level + a public-facade case)

Over the existing scripted send/submit mock (both I/O models where the edge differs):

1. **`SLOT` unchanged** — `send_byte_budget = 0`: every existing send test passes verbatim (regression
   guard: default policy is byte-identical).
2. **`BOTH` byte-gate refusal** — budget sized for K datagrams; the (K+1)-th refuses `WOULD_BLOCK`
   **while a slot is still free** (proves the byte gate, not the slot gate, fired).
3. **`BOTH` slot-gate still active** — many small datagrams under budget exhaust **slots** first →
   `WOULD_BLOCK` (proves the count bound survives under `BOTH`).
4. **Zero-length under `BOTH`** — `send_slots` zero-length datagrams admit (0 bytes each), the next
   refuses on the **slot** bound (proves the impossibility mitigation: slots bound zero-length).
5. **Byte accounting reopens admission** — fill the budget, retire one queued datagram, confirm
   `bytes_used` dropped by exactly its `len` and a same-size send now admits.
6. **`on_writable` under the byte gate** — a byte-gate `WOULD_BLOCK` latches `full`; a retirement fires
   `on_writable` exactly once (edge, not level storm).
7. **`TOO_LARGE` independent of budget** — `len > slot_cap` is permanent regardless of remaining
   budget (and regardless of `SLOT`/`BOTH`).
8. **Init validation** — `send_byte_budget` in `(0, slot_cap)` → `kl_datagram_init` returns `-1`
   (`KL_ERR_INVALID_ARG`); `== slot_cap` and `> slot_cap` succeed; `== 0` succeeds (`SLOT`).
9. **`discard_queued` / `abandon` reset** — after abortive discard and after abandon, `bytes_used == 0`
   (accounting symmetric with teardown; no drift).
10. **Public facade** — one `test_datagram_public` case constructing `BOTH` via `KlDatagramConfig`
    end-to-end (budget admission through the real facade, scripted completion mock).

## 11. Validation plan

- macOS default (kqueue readiness) `make test` — all suites + the new send cases, ASan/UBSan.
- `BACKEND=pollcomp` — the completion-model edge (in-flight slot counts toward `bytes_used`).
- Linux container `BACKEND=iouring` under ASan/UBSan/**LSan** — the byte-gate path holds no memory and
  leaks nothing (no hot-path allocation added; regression against the I8 guarantee).
- Gates: `check-tier1-boundary`, `check-sockaddr-neutral`, `check-doc-refs`, `cppcheck` (the overflow
  guard in §4 is exactly cppcheck's remit), EFI host-mock unaffected (no EFI change), freestanding
  datagram build (the field is inert there).

## 12. Open decisions for the reviewer (before implementation)

- **D-M1-1 — policy selector shape.** *Recommended:* the single `size_t send_byte_budget` field
  (0 = `SLOT`), as frozen in §2 — minimal ABI, unrepresentable invalid combinations. *Alternative:* an
  explicit `KlDatagramQueuePolicy { SLOT, BOTH }` enum + a separate `byte_budget` field (more explicit
  in call sites, but two fields that can disagree). Pick one.
- **D-M1-2 — budget plumbing into the send machine.** *Recommended:* a new trailing `size_t
  byte_budget` parameter on `kl_dgram_send_init` (internal, no ABI). *Alternative:* a
  `kl_dgram_send_set_budget()` setter (keeps the init arity, mirrors the existing
  `set_writable_cb`/`set_drain_cb` style). Pick one.
- **D-M1-3 — STABLE-banner footnote.** Does appending `send_byte_budget` to the STABLE
  `KlDatagramConfig` (§9) warrant an explicit banner footnote in `include/keel/datagram.h`, or is
  "additive, zero = prior behavior" sufficient given no external ABI consumer exists pre-1.0?
- **D-M1-4 — forward-progress rule.** Confirm the `budget ≥ slot_cap` init requirement (§6) over the
  alternative "always admit when the queue is empty" special case. The init check is stricter but makes
  livelock structurally impossible and keeps admission uniform.
