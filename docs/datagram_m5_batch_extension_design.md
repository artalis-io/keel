# Datagram M5 — Batch / GSO / GRO high-throughput extension (design freeze, rev 2)

**Status:** DESIGN FREEZE (docs-only), **revision 2**. No production code, no ABI change, no consumer
migration here. Rev 2 replaces rev 1's "batching above an unchanged core" hand-wave with **concrete
core state-machine seams**, because (as the reviewer noted) the capability shape depends on the
execution model — the seams must be frozen before M5.1.

**Supersedes** the original M5 framing in
[`datagram_consolidation_design.md`](datagram_consolidation_design.md) §6.5. Per reviewer direction
(2026-08-19): design the batch/GSO/GRO extension for `KlDatagram` and migrate `KlUdpServer` onto it
first; the `KlUdp` reduction is deferred to a later step. `KlUdp` is UNTOUCHED by M5.

### Rev 2 changelog — how the six review findings are resolved (map in §13)

- **P1-a (send seam):** §4 freezes a **real readiness batch-drain over the head-run** of the core send
  FIFO (reservation → one `sendmmsg` → prefix retirement), integrated with byte accounting, the
  full→non-full / drain edges, and close. Not "a loop of `kl_datagram_send`"; not "call `send_batch`
  above the core."
- **P1-b (recv seam):** §5 freezes a **batch-backed multi-yield readiness pull adapter** on the
  existing recv machine — exact hook, cursor state, who owns readiness, busy-across-deliveries, and
  teardown-from-delivery-N safety. D-M5-1's absolute "core not modified" is **retracted**: the
  single-datagram *contract* is unchanged, but the readiness pull adapter gains a defined internal
  variant (an internal core/facade extension).
- **P1-c (GRO cap derivation):** §6.2 freezes an **explicit `rx_caps` handoff** (from the M0
  `KlDatagramPrep.rx_caps` into the datagram), because `kl_datagram_init_ex` stores no capture mask
  today. No inference of GRO from provider support.
- **P1-d (cap directionality):** §6.1 splits into **`KL_DGRAM_CAP_RX_BATCH`** and
  **`KL_DGRAM_CAP_TX_BATCH`** (recv/send mmsg are independently absent).
- **P1-e (GSO latch):** §6.3 defines **`KL_DGRAM_CAP_GSO` as advisory (op present)** plus an
  **extension-owned per-fd first-use-fail→latch→fallback**; `provider_caps` does not mutate (the
  provider stays stateless).
- **P2 (mode-B event ownership):** §5.4 **defers the explicit data-oriented `recv_batch` (mode B)** —
  its readiness-ownership contract needs its own design. M5 receive is **mode A only** (the callback
  machine), which the core watcher already drives, so there is no competing readiness owner.

### Rulings applied (O-A..O-D, settled)

- **O-A:** GRO splits into logical datagrams **by default**; whole-buffer delivery is **explicit**
  (register `on_recv_segments`).
- **O-B:** `KlUdpServer` reply batching is **deferred** (replies are demand-driven one-per-recv).
- **O-C:** batch/GRO is **readiness-only** for M5; completion backends (io_uring/IOCP) stay
  single-flight (correct, unbatched).
- **O-D:** the byte-only send queue stays **out** of M5 (§9).

## 1. Mandate & layering

The four Linux fast paths (recvmmsg / sendmmsg / UDP GSO / UDP GRO) are real pps/CPU wins and belong in
`KlDatagram` as an OPTIONAL layer, not pushed up into `KlUdpServer`. Target layering:

```
KlUdpServer  →  KlDatagram facade  →  optional batch/GSO/GRO extension  →  KlDatagramOps provider
                (single-datagram        (NEW: this freeze; readiness-only;    (already has send_gso /
                 contract unchanged)     caller-preallocated; cap-gated)       recv_batch / send_batch)
```

## 2. Ground truth (verified against `src/` — cite before coding)

1. **Provider vtable already has the ops.** `KlDatagramOps` (`keel/socket_dgram.h`): `send_gso`,
   `recv_batch`, `send_batch`, `rx_batch_new/tx_batch_new/rx_batch_free/tx_batch_free`, `configure`
   (returns the accepted `KL_DGRAM_RX_*` mask incl. `KL_DGRAM_RX_GRO`), `caps`. Public caller-oriented
   types `KlDgramRxSlot` / `KlDgramTxDesc` / `KlDgramRxMeta` exist. **No new provider data-plane ABI.**
2. **The core send machine is strictly single-flight** (`KlDgramSend`, `datagram_send.c`): a fixed slot
   array + LIFO free-list, a FIFO ring of occupied slots (`ring`/`head`/`count`), `inflight_n ∈ {0,1}`,
   and `send_pump` (`datagram_send.c:137`) submits **only the head** via `send_submit_slot(s, 0)`. In
   readiness mode submit is synchronous (submit→DONE, nothing lingers in-flight). Byte accounting
   (`bytes_used`, M1 BOTH policy) is admission-only; `count==0` is the drain predicate. FIFO order is
   the head-run of the ring.
3. **The core recv machine owns one inbound slot** (`KlDgramRecv`, `datagram_recv.c`): the readiness
   path `kl_dgram_recv_on_readable` (`datagram_recv.c:180`) **loops** `pull → recv_finalize →
   recv_present`, re-checking `!paused && !stopped && recv_inflight` **after every delivery**. The
   `pull` hook (`KlDgramRecvPullFn`) fills the **one** inbound slot and returns 1/0/-1. Completion mode
   posts one op; `on_complete` delivers + re-arms (single op at a time).
4. **`init_ex` stores no capture mask.** `kl_datagram_init_ex` (`datagram.c:274`) copies
   `send_slots/send_slot_cap/recv_cap/want_caps` + the byte budget into `KlDgramCoreConfig`. There is
   **no `rx_caps` field** on `KlDatagramConfig` or `KlDgramCoreConfig`; the completion recv seam
   hard-codes `capture = KL_DGRAM_RX_PKTINFO` (`datagram.c:140`). **The datagram does not know whether
   UDP_GRO was enabled.** (This is why P1-c is real.)
5. **`KlUdpServer` already embeds `KlDatagram dg`** (M4, `udp_server.h:72`); its `mmsg_batch` /
   `recv_gro` knobs are currently **inert** (a pps regression vs the old `KlUdp` path this extension
   restores).
6. **The life-token + close coordinator** own op lifetime and the from-callback deferral (the `busy`
   handshake via `on_activity(+1/-1)` and `kl_dgram_core_dispatch_begin/end`). Any new seam must run
   inside that bracket so a from-callback teardown defers to the outermost frame leave.

## 3. Design decisions (rev 2)

**D-M5-1 (revised) — the single-datagram CONTRACT is unchanged; the core gains DEFINED internal
seams.** The public single-datagram semantics (`kl_datagram_send` atomic accept/refuse;
`kl_datagram_recv_start` one-logical-datagram-per-callback; the §4 close/quarantine model; the
allocation-free hot path) are preserved exactly. But — correcting rev 1 — the batch/GSO/GRO paths ARE
an internal core/facade extension: a readiness batch-drain entry on the send machine (§4) and a
batch-backed multi-yield pull adapter on the recv machine (§5). The core gains **one optional `ext`
pointer** (`KlDgramBatchExt *`, NULL when unused) holding the tx/rx batch blocks, the GRO cursor, and
the GSO latch; a build/consumer that never attaches a batch is byte-for-byte unaffected (the ext
pointer stays NULL and the single-flight paths run).

**D-M5-2 — caller-preallocated; no hot allocation.** The `KlDgramBatchExt` and the provider
`rx_batch`/`tx_batch` blocks are allocated **once** at batch-create/attach; the `KlDgramRxSlot[]` /
`KlDgramTxDesc[]` staging arrays are ext-owned (sized at create) or caller-provided. No allocation on
any send/recv edge.

**D-M5-3 — readiness-only (O-C).** The extension activates ONLY on a readiness-mode datagram
(`core->completion == 0`). On a completion datagram, attaching a recv batch is refused
(`KL_ERR_UNSUPPORTED`) and the explicit send-batch admits+drains via the normal single-flight
completion pump (correct, unbatched). Native completion multishot is deferred (§10, O-1).

**D-M5-4 — capability-gated, directional, honest (P1-d/P1-e).** `KL_DGRAM_CAP_RX_BATCH` /
`_TX_BATCH` / `_GSO` / `_GRO` (§6). Absence is reported, never emulated; the only "fallback" is a
portable correctness loop for the SEND primitives (identical bytes, lower throughput, observable via
`provider_caps`) — §6.4.

**D-M5-5 — GRO splits by default (O-A); segments-callback is explicit.** The recv pull yields one
logical datagram per delivery; a GRO-coalesced buffer is split per-segment **unless** the consumer
registered `on_recv_segments`, in which case the whole coalesced buffer is delivered once (§5.3).

## 4. THE SEND SEAM — readiness batch-drain over the head-run (resolves P1-a)

The core send FIFO stays the single source of admission (slot count + M1 byte budget), ordering, and
close. Batching changes only **how the queue drains on a readiness writable edge**: instead of
`send_pump` submitting one head slot per iteration, a batch-drain reserves the head-run and issues one
`sendmmsg`.

**New send-machine entry (readiness only):** `kl_dgram_send_flush_batch(KlDgramSend *s, int max)`,
called by the facade's readiness writable-flush (replacing `send_pump` when a TX batch is attached):

1. **Reserve** the head-run: `k = min(max, s->count)` contiguous occupied slots `ring[head ..
   head+k)`. (`max` = the attached batch's `n_slots`, itself ≤ `mmsg_batch`.)
2. **Stage** `KlDgramTxDesc[k]` from those slots' metadata (`data`/`len`/`peer`→dest/`local`→src/`tos`)
   into the ext's `tx_batch` block. Zero-copy: `desc.data` points at the slot payload.
3. **Submit once** via a new batch-submit adapter `KlDgramSubmitBatchFn submit_batch(ctx, fd,
   tx_batch, descs, k) → sent` (facade wires it to provider `send_batch` = one `sendmmsg`, or the
   portable loop under `!TX_BATCH`, §6.4). `sent ∈ [0, k]`: `0` = WOULD_BLOCK, `<k` = partial, `k` =
   all. (A per-datagram hard error inside the provider is surfaced as in KlUdp today: drop that slot,
   count it in `dropped`, continue — the provider's `send_batch` contract already returns the sent
   prefix count.)
4. **Retire the sent prefix** from the head: for each of the `sent` slots, `send_retire_head`
   semantics — release the slot to the free-list, `bytes_used -= len`, `head = (head+1) % ring_cap`,
   `count--`. This reuses the existing retire path so the M1 byte accounting, the `full → non-full`
   edge (`pend_writable`), and the non-empty→empty `on_drain` edge (`pend_drain`) fire through the
   existing `dispatch_enter/leave` bracket.
5. **Re-arm** if `count > 0` after a partial/WOULD_BLOCK drain (leave the remainder queued in FIFO
   order); drop WRITE interest when `count == 0`.

**Invariants preserved.** Readiness send is synchronous, so there is **no async in-flight batch** —
`inflight_n` stays 0 (as it does for the current per-slot readiness pump); reservation/submission/
retirement all complete within the flush call. FIFO = the head-run; a partial drain keeps the tail in
order. Byte budget, `full`, drain/writable edges, and single-flight are all the existing machinery,
just retiring a prefix instead of one slot. **Close:** abortive close's `kl_dgram_send_discard_queued`
already releases the queued remainder; there is no in-flight batch to cancel; readiness retire is
always `RETIRED` → DETACHED. **Completion mode:** unaffected — it keeps `send_pump` (single-flight);
`flush_batch` is never wired there.

**Explicit primitive** `kl_datagram_send_batch(dg, batch, descs, n)`: admit `descs[0..n)` through the
normal core admit (copy-at-submit, slot + byte gate) up to the first `WOULD_BLOCK`/`TOO_LARGE`
(partial accept — the caller keeps the remainder, invariant 4), then run one `flush_batch` (readiness)
or the single-flight pump (completion). Returns the accepted count. Everything transits the core queue,
so admission/FIFO/byte/close/edges are identical to `kl_datagram_send`.

## 5. THE RECV SEAM — batch-backed multi-yield pull adapter (resolves P1-b, P2 mode-A)

The recv machine keeps its one inbound slot and its `pull → finalize → deliver` loop **unchanged**.
Batching/GRO change only the `pull` adapter: a batch-backed pull yields logical datagrams **one at a
time** from an ext-owned buffer, refilling via `recv_batch` (one recvmmsg) when the buffer drains.

### 5.1 The seam

The facade wires the readiness `pull` hook to `dg_rdy_pull_batch` (instead of `dg_rdy_pull`) when a RX
batch is attached. The machine's existing `kl_dgram_recv_on_readable` loop (`datagram_recv.c:180`)
drives it verbatim — one delivery per iteration, re-checking `!paused && !stopped && recv_inflight`
each time.

**Ext cursor state** (in `KlDgramBatchExt`, readiness-owned): `KlDgramRxSlot slots[n_slots]`, `int
n_filled`, `int i` (next slot), `size_t seg_off` (byte offset within a GRO-coalesced slot `i`).

**`dg_rdy_pull_batch(hook_ctx, inbound) → 1/0/-1`:**
1. If the buffer is exhausted (`i >= n_filled` and no pending GRO segment): call `recv_batch(ctx, fd,
   rx_batch, slots, n_slots)` → `n_filled`. `n_filled == 0` (EAGAIN) → return `0` (drained, ends the
   readable loop). `< 0` → return `-1` (the machine calls `recv_fail`). Reset `i = 0`, `seg_off = 0`.
2. **Yield the next logical datagram** into the inbound slot (point `inbound.data` at ext buffer
   storage — borrowed, valid until the next `recv_batch`; the delivery contract is already "valid only
   for the call"):
   - If `slots[i].meta.gro_seg > 0` and `on_recv_segments` is **not** registered and `slots[i].len -
     seg_off > gro_seg`: yield the segment `[seg_off, seg_off+gro_seg)`; `seg_off += gro_seg`; leave
     `i` (more segments remain). The final (short) segment yields the remainder and advances `i`.
   - Else: yield slot `i` whole (a plain datagram, the last GRO segment, or — when `on_recv_segments`
     is set — the whole coalesced buffer with `gro_seg` carried for the segments callback); `i++`,
     `seg_off = 0`.
   Copy `src`→peer, `meta`→local/tos/flags into the inbound slot exactly as `dg_rdy_pull` does. Return
   `1`.

The machine then `recv_finalize`s (peer mandatory, truncation, local flags — unchanged) and delivers
one logical datagram. **GRO-without-mmsg** (`recv_gro`, `mmsg_batch == 1`) is the same seam with
`n_slots == 1`: `recv_batch` returns one coalesced slot; the cursor splits it per-segment. (A provider
without `recv_batch` but with GRO uses a single-slot `recv` refill; §6.4.)

### 5.2 Event ownership (resolves P2 for mode A)

The **core's single readiness watcher** (READ interest, reconciled by `dg_reconcile`) drives
`on_readable`, which drives the batch pull. The extension owns **no** watcher and posts **no** op — it
is purely the pull source. There is exactly one readiness owner (the core), so mode A cannot compete
with a core watcher.

### 5.3 Whole-buffer segments delivery (O-A explicit)

`kl_datagram_recv_segments(dg, cb, ud)` registers a `KlDatagramRecvSegmentsFn`. When set, the pull
yields the coalesced buffer WHOLE (step 2, else-branch) and the deliver adapter routes a `gro_seg > 0`
datagram to the segments callback; when unset (default), the pull splits per-segment → `on_recv`.

### 5.4 Teardown-from-delivery-N safety (the reviewer's explicit requirement)

Three guarantees, all from existing machinery:
1. **The loop breaks before slot N+1.** A `stop`/`close` from delivery N sets `stopped = 1` (recv
   machine) inside the delivery; `kl_dgram_recv_on_readable` re-checks `!stopped` at the top of the
   next iteration and returns — `dg_rdy_pull_batch` is never called for slot N+1.
2. **The buffer outlives the loop.** `KlDgramBatchExt` (and its `rx_batch` storage) is freed only at
   the **terminal** (close-complete), which the `busy` handshake (`on_activity`/`dispatch_begin/end`)
   defers past any in-progress `on_readable` frame. So borrowed `inbound.data` (pointing into the ext
   buffer) is valid for the whole delivery, and no slot N+1 storage is freed mid-loop.
3. **"Busy" spans all deliveries.** `recv_inflight` stays 1 for the entire readable edge (readiness
   interest armed); the close coordinator cannot detach mid-drain because `close_fully_retired` gates
   on `recv_inflight == 0`, which only clears when the machine disarms (stop/pause) — i.e. after the
   loop yields control.

### 5.5 Mode B (explicit `kl_datagram_recv_batch`) — DEFERRED (resolves P2)

An explicit data-oriented poll (`kl_datagram_recv_batch(dg, batch, slots, max)`) would let a caller
drain a recvmmsg on its own loop — but once `KlDatagram` adopts the fd, the **core owns readiness**;
mode B needs its own event-ownership contract (who tells the caller the fd is readable; how it avoids
competing with the core watcher). That is out of scope for M5. **M5 receive is mode A only** (the
callback machine), which every M5 consumer (incl. `KlUdpServer`) uses. Mode B is revisited later with
a dedicated readiness-ownership design.

## 6. Capability model

### 6.1 Directional caps (resolves P1-d)

Added to `keel/datagram.h`:
```
KL_DGRAM_CAP_RX_BATCH (1u << 5)   /* recvmmsg: recv_batch + rx_batch_new/free present, caps() reports it */
KL_DGRAM_CAP_TX_BATCH (1u << 6)   /* sendmmsg: send_batch + tx_batch_new/free present, caps() reports it */
KL_DGRAM_CAP_GSO      (1u << 7)   /* send_gso present (advisory — see 6.3) */
KL_DGRAM_CAP_GRO      (1u << 8)   /* UDP_GRO capture was accepted on THIS fd (see 6.2) */
```
A `KL_DGRAM_BATCH_RECV` request checks `RX_BATCH` only; `KL_DGRAM_BATCH_SEND` checks `TX_BATCH` only —
neither is rejected for the other's absence.

### 6.2 GRO from an explicit `rx_caps` handoff (resolves P1-c)

`KL_DGRAM_CAP_GRO` is granted **iff the datagram was told UDP_GRO was accepted on its fd** — never
inferred from provider support. Since the M0 prep helper already computes the accepted mask
(`KlDatagramPrep.rx_caps`, `datagram_open.c`) but `init` drops it, freeze an explicit handoff:
- Add `unsigned rx_caps;` to `KlDatagramConfig` (the accepted `KL_DGRAM_RX_*` mask; **0 for a
  caller who does not know** → no GRO/pktinfo-derived caps). This is an additive, pre-1.0 config
  revision (the same class as M2's intentional `KlDatagramOps.caps` addition — noted for the ABI
  ledger). `kl_datagram_init_ex` stores it on the core; `KL_DGRAM_CAP_GRO` derives from `rx_caps &
  KL_DGRAM_RX_GRO`.
- `KlUdpServer` and other in-tree consumers thread `KlDatagramPrep.rx_caps` from `kl_datagram_open`
  into `cfg.rx_caps`. A consumer wrapping an arbitrary fd it configured itself sets `rx_caps` to what
  it enabled; if it sets 0, GRO is simply unavailable (graceful — `meta.gro_seg` stays 0).

### 6.3 GSO — advisory cap + extension-owned runtime latch (resolves P1-e)

Function presence and compile-time `UDP_SEGMENT` do **not** prove kernel acceptance, and the provider
vtable is stateless (the current `gso_disabled` latch lives in `KlUdp`, not the provider). Freeze:
- `KL_DGRAM_CAP_GSO` = **advisory**: the provider `send_gso` op is present (compile-time availability).
  `provider_caps` reports it and **does not mutate** at runtime.
- The **extension owns a per-fd runtime latch** (`int gso_unsupported` in `KlDgramBatchExt`, init 0).
  `kl_datagram_send_gso` tries the provider op; on `EOPNOTSUPP`/first-use failure it sets the latch and
  transparently falls back to the per-segment send loop (matching `KlUdp`); subsequent calls skip
  straight to the fallback. A query (`int kl_datagram_gso_active(const KlDatagram*)`) exposes the
  latch so a throughput-sensitive caller can observe it. No provider ABI or state is added.

### 6.4 Fast-path vs correctness fallback (send only)

`kl_datagram_send_batch` without `TX_BATCH` → the batch-submit adapter is the **portable loop** (one
`send` per desc); `kl_datagram_send_gso` without `GSO` (or after the latch) → the per-segment loop.
Both produce identical bytes/ordering at lower throughput; `provider_caps` reports the cap absent so a
caller can branch. `RX_BATCH` has **no** emulation: without it, `dg_rdy_pull_batch`'s refill uses a
single `recv` (`n_slots` effectively 1) — i.e. GRO-split still works, but there is no recvmmsg speedup;
the explicit mode-B API (deferred) would simply require the cap. No silent capability fabrication.

## 7. Proposed public surface (`keel/datagram_batch.h`, rev 2)

```c
typedef struct KlDatagramBatch KlDatagramBatch;              /* opaque; wraps the ext + provider blocks */
typedef enum { KL_DGRAM_BATCH_RECV = 1, KL_DGRAM_BATCH_SEND = 2, KL_DGRAM_BATCH_BOTH = 3 } KlDgramBatchDir;

/* Create once; allocates the ext + provider rx/tx blocks. NULL if the datagram is completion-mode
 * (readiness-only, D-M5-3), the direction's cap is absent, or on OOM. */
KlDatagramBatch *kl_datagram_batch_create(KlDatagram *dg, KlDgramBatchDir dir,
                                          int n_slots, size_t slot_bufsz);
void             kl_datagram_batch_free(KlDatagramBatch *b);   /* detaches + frees; safe post-close */

/* SEND: admit descs through the core queue (partial-accept per backpressure), then one sendmmsg
 * drain (or portable loop). Returns accepted count (<= n), or -1 (kl_datagram_last_error). */
int kl_datagram_send_batch(KlDatagram *dg, KlDatagramBatch *b, const KlDgramTxDesc *descs, int n);

/* one-syscall UDP GSO, or per-segment fallback (+ latch, 6.3). Core-queue backpressure applies. */
KlDatagramSendStatus kl_datagram_send_gso(KlDatagram *dg, const void *buf, size_t total_len,
                                          uint16_t segment_size, const KlSockAddr *peer);
int  kl_datagram_gso_active(const KlDatagram *dg);            /* 0 once the GSO latch tripped */

/* RECV mode A: attach a RX batch BEFORE recv_start; the machine's readiness pull becomes batch-backed
 * (§5). GRO splits per-segment by default; register on_recv_segments for whole-buffer delivery. */
int  kl_datagram_recv_attach_batch(KlDatagram *dg, KlDatagramBatch *b);
typedef void (*KlDatagramRecvSegmentsFn)(void *ud, const void *data, size_t len, size_t segment_size,
                                         const KlSockAddr *peer, const KlSockAddr *local, unsigned flags);
void kl_datagram_recv_segments(KlDatagram *dg, KlDatagramRecvSegmentsFn cb, void *ud);
```
New caps + `KlDatagramConfig.rx_caps` in `keel/datagram.h` (§6). **No** explicit `kl_datagram_recv_batch`
in M5 (mode B deferred, §5.5). `keel/socket_dgram.h` unchanged. Internally: `KlDgramCore` gains one
`KlDgramBatchExt *ext` pointer; the send machine gains `kl_dgram_send_flush_batch` + a
`KlDgramSubmitBatchFn` adapter; the facade gains `dg_rdy_pull_batch` / `dg_rdy_submit_batch`.

## 8. `KlUdpServer` migration

No public API/ABI change (8 funcs + 17-field config + `KlUdpHandlerFn` preserved; `mmsg_batch` /
`recv_gro` stop being inert). `kl_udp_server_init` threads `KlDatagramPrep.rx_caps` into `cfg.rx_caps`
(§6.2); if `mmsg_batch > 1` and/or `recv_gro` and the provider reports `RX_BATCH`/`GRO`, it
`kl_datagram_batch_create(RECV)` + `kl_datagram_recv_attach_batch` before `recv_start`. The handler is
unchanged — each logical datagram (a batch slot, or a GRO segment after the split) reaches
`KlUdpHandlerFn` one at a time with `src` (+ `local` for source-pinned reply). Reply stays on the core
single send (O-B). Batch torn down in `kl_udp_server_free` before the `KlDatagram` teardown
(server-owned, freed once). Off-ramp: absent caps → the M4 single-flight behavior, no regression.

## 9. Byte-only send queue — OUT of scope (O-D); options recorded

Not moved into the core (a zero-length datagram means a byte budget cannot bound message count). M5
does not touch it. Post-M5 `KlUdp` decision options (unchanged from rev 1): (1) keep it only in a
deprecated `KlUdp`; (2) replace with explicit `send_slots` + `send_byte_budget` (the M1 BOTH policy) in
a future breaking release; (3) an optional allocator-backed queue adapter above `KlDatagram`, outside
Tier-1.

## 10. Non-goals

- No production code / consumer migration / ABI break in this freeze (the `rx_caps` config field is a
  frozen-for-review proposal, landed in M5.1).
- Do NOT change the single-datagram delivery/close/quarantine CONTRACT or the allocation-free hot path.
- **O-1 (deferred):** native COMPLETION-backend batched receive (io_uring multishot / IOCP). M5 batch/
  GRO is readiness-only.
- **Mode B deferred** (§5.5). **Reply batching deferred** (O-B). **Byte queue out** (O-D). **`KlUdp`
  reduction deferred** to the post-M5 decision.

## 11. Test matrix

- **Send seam (§4):** batch-drain retires the sent prefix in FIFO order; partial `sendmmsg` (sent < k)
  leaves the tail queued + re-arms WRITE; byte budget released per retired slot; `on_drain` fires once
  at empty; abortive close discards the remainder (DETACHED). Fast path AND portable-loop fallback
  (provider with NULL `send_batch`) deliver identical bytes/order.
- **Recv seam (§5):** one recvmmsg → N serial `on_recv` (assert N, per-datagram bytes/src);
  GRO-coalesced slot splits into per-segment deliveries (assert segment count + boundaries) OR whole
  to `on_recv_segments`; GRO-without-mmsg (n_slots=1) splits the same; drained→EAGAIN ends the edge.
- **Teardown-from-delivery-N (§5.4):** `stop`/`close_begin`/`close_cancel` from delivery N (of a filled
  batch and of a multi-segment GRO buffer) → slots/segments N+1.. are never delivered, DETACHED, no
  UAF/leak (ASan/UBSan/LSan).
- **Caps (§6):** `provider_caps` reports RX_BATCH/TX_BATCH/GSO/GRO honestly per fd/platform;
  `batch_create(RECV)` succeeds with only RX_BATCH (TX absent) and vice-versa; `send_batch`/`send_gso`
  without the cap take the fallback and still deliver; GRO cap absent when `rx_caps` lacks
  `KL_DGRAM_RX_GRO` even on a GRO-capable provider.
- **GSO latch (§6.3):** a provider whose `send_gso` returns `EOPNOTSUPP` on first use → the datagram
  latches, `gso_active` becomes 0, subsequent sends use the per-segment loop and still deliver.
- **`rx_caps` handoff (§6.2):** a datagram built with `rx_caps` = 0 grants no GRO even on a capable fd;
  built from `KlDatagramPrep.rx_caps` grants GRO iff the kernel accepted `UDP_GRO`.
- **`KlUdpServer` opt-in (§8):** a burst reaches the handler one-at-a-time (assert count + `src`); a GRO
  burst is split; caps absent → every datagram still delivered (fallback); source-pinned reply
  unchanged; batch freed once at teardown.
- **Regression:** DNS single-flight + the existing datagram/udp_server suites unchanged; the ext
  pointer is NULL and no allocation occurs when no batch is attached.

## 12. Increment breakdown (each its own freeze-then-code review; none authorized here)

1. **M5.1 — caps + `rx_caps` handoff + ext scaffolding.** Add `KL_DGRAM_CAP_RX_BATCH/_TX_BATCH/_GSO/
   _GRO` + directional provider `caps()` reporting; add `KlDatagramConfig.rx_caps` + store it on the
   core + derive `CAP_GRO`; the `KlDgramBatchExt` + `KlDatagramBatch` create/free (no wiring yet). No
   behavior change to existing consumers.
2. **M5.2 — send seam.** `kl_dgram_send_flush_batch` + the `KlDgramSubmitBatchFn` adapter + the
   portable fallback; `kl_datagram_send_batch` + `kl_datagram_send_gso` + the GSO latch. Readiness
   batch-drain reservation/submit/retire per §4.
3. **M5.3 — recv seam.** `dg_rdy_pull_batch` + the GRO-split cursor + `kl_datagram_recv_attach_batch` +
   `kl_datagram_recv_segments`. Machine loop unchanged; teardown-from-delivery-N tests per §5.4.
4. **M5.4 — `KlUdpServer` opt-in.** Thread `rx_caps`; wire `mmsg_batch`/`recv_gro` to mode A; full
   parity + every-backend conformance; batch teardown ordering.
5. **(post-M5) `KlUdp` decision** (§9) and **mode B** (§5.5): separate freezes.

**Order/dependencies:** M5.1 → {M5.2, M5.3} → M5.4. Independent of any `KlUdp` change. Re-decided after
M5.1 + one of M5.2/M5.3 prove the seam clean (the master-plan §6 "stop if ugly" escape hatch applies).

## 13. Findings resolution map

| # | Finding | Resolution |
|---|---------|-----------|
| P1-a | send fast path can't sit above an unchanged core | §4 — readiness batch-drain over the head-run: real reserve/submit(one sendmmsg)/retire-prefix in the core send FIFO, integrated with byte accounting + edges + close; completion stays single-flight (O-C) |
| P1-b | callback recv batching needs a concrete seam | §5 — `dg_rdy_pull_batch` multi-yield adapter on the existing machine loop; cursor state; core owns readiness (§5.2); teardown-from-delivery-N safety from `stopped` re-check + deferred ext free (§5.4). D-M5-1 "core not modified" retracted → "contract unchanged, defined internal seam" |
| P1-c | GRO cap derived from state the datagram lacks | §6.2 — explicit `KlDatagramConfig.rx_caps` handoff from `KlDatagramPrep.rx_caps`; `CAP_GRO` from the stored mask, never inferred |
| P1-d | one CAP_BATCH bit not directional | §6.1 — split `KL_DGRAM_CAP_RX_BATCH` / `_TX_BATCH` |
| P1-e | GSO latch not grounded in the stateless provider | §6.3 — `CAP_GSO` advisory (op present); extension-owned per-fd first-use-fail→latch→fallback; `provider_caps` does not mutate |
| P2 | mode-B recv lacks event-ownership contract | §5.5 — mode B deferred; M5 recv is mode A only, driven by the core's single watcher (no competing owner) |

## 14. Open decisions for the reviewer (rev 2)

- **O-E — `rx_caps` handoff mechanism.** §6.2 adds a `KlDatagramConfig.rx_caps` field (additive pre-1.0
  revision). Confirm that vs. an alternative (an `init_ex2` parameter, or a post-init setter). The
  field is the least-friction and mirrors how `KlDatagramPrep` already carries the mask.
- **O-F — where `KlDgramBatchExt` lives.** §D-M5-1 puts one optional `ext` pointer on `KlDgramCore`
  (NULL when unused) rather than a parallel facade-side object. Confirm the core-pointer placement
  (it keeps the send/recv adapter wiring local to the core) vs. a facade-owned ext.
