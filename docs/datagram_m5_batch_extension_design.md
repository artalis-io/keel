# Datagram M5 — Batch / GSO / GRO high-throughput extension (design freeze, rev 3)

**Status:** DESIGN FREEZE (docs-only), **revision 3**. No production code, no ABI change, no consumer
migration here. Rev 3 freezes the concrete state-machine, lifetime, and ownership contracts that rev 2
left open, so the increment chain (M5.1–M5.4) is implementable.

**Supersedes** the M5 framing in
[`datagram_consolidation_design.md`](datagram_consolidation_design.md) §6.5. `KlUdp` is UNTOUCHED by
M5; its reduction is a deferred post-M5 decision.

Rev-1/2 background (layering, ground truth, KlUdpServer shape, byte-queue disposition) is retained in
§1–§2 and §8–§9; rev 3's substance is the frozen contracts in **§3–§6** and the findings map in §13.

### Rev 3 changelog — the six rev-2 blockers, frozen (map in §13)

- **B1 (send admission defeats batching):** §4.1 freezes an **enqueue-only batch transaction** — an
  accepted prefix is copied into slots with NO per-datagram submit and NO readiness fast path, then one
  batch flush runs under the same busy bracket. Acceptance-stop + return semantics (ACCEPTED /
  WOULD_BLOCK / TOO_LARGE / hard error) are defined precisely.
- **B2 (can't drop an arbitrary failed slot):** §4.2 — retire only the provider's reported sent prefix;
  a short/zero return is transient (retain remainder + re-arm); a hard `-1` triggers **single-send
  isolation of the head slot** (never infer which slot failed).
- **B3 (don't repoint the canonical inbound slot):** §5.1 adopts a **borrowed-view delivery seam**
  ({data,len,meta} into ext storage) — the owned inbound slot is not mutated and GRO buffers are not
  clamped by `recv_cap`. Finalization/truncation operate on the view.
- **B4 (extension teardown ownership):** §5.4 — the **core adopts/owns the attached recv extension**;
  reclamation is at the core's **destructive tail** (not `recv_inflight`, which readiness disarm clears
  in-callback); detach/free is permitted only while unattached or CLOSED (else UAF).
- **B5 (GSO queue semantics):** §4.3 — **atomic preflight admission of all `nseg` logical segments**
  against slot count AND byte budget (no partial), copy-once, then GSO-or-normal-sequence, with atomic
  (GSO) / per-segment (fallback) retirement and `total`-byte accounting.
- **B6 (fallback vs creation):** §6.4 — `kl_datagram_batch_create` **never fails on capability
  absence**; the provider block is optional (NULL ⇒ the portable fallback path), so the fallback is
  reachable. Create fails only for a completion-mode datagram (O-C) or OOM.
- **Semantic cleanup:** §6.1/§6.2 — `provider_caps` stays **provider support**; the M0 **accepted RX
  mask is separate per-socket state**; **GRO activates only when BOTH** provider GRO support AND
  `accepted_rx_caps & KL_DGRAM_RX_GRO` hold. CAP_GRO is not synthesized into provider support.

### Rulings applied

- **O-E (settled):** append `unsigned accepted_rx_caps` to `KlDatagramConfig` (ABI break accepted — no
  consumers), **masked to known `KL_DGRAM_RX_*` bits**.
- **O-F (settled):** **core-owned** attached-extension state, with ownership transfer on attach and
  **destructive-tail reclamation** (§5.4).
- **O-A:** GRO splits into logical datagrams by default; whole-buffer delivery is explicit
  (`on_recv_segments`). **O-B:** reply batching deferred. **O-C:** readiness-only for M5; completion
  stays single-flight. **O-D:** byte-only queue out of the core.

## 1. Mandate & layering (unchanged)

```
KlUdpServer  →  KlDatagram facade  →  optional batch/GSO/GRO extension  →  KlDatagramOps provider
                (single-datagram        (NEW; readiness-only; caller-        (already has send_gso /
                 CONTRACT unchanged)     preallocated; capability-gated)      recv_batch / send_batch)
```

## 2. Ground truth (verified `src/`; the facts the contracts stand on)

- **Send machine `KlDgramSend`** (`datagram_send.c`): fixed slots + LIFO free-list + FIFO ring
  (`ring`/`head`/`count`); `inflight_n ∈ {0,1}`; `send_pump` (`:137`) submits only the head; readiness
  submit is **synchronous** (submit→DONE, nothing lingers). The readiness empty-queue fast path
  (`kl_datagram_send` `:168`) direct-submits without consuming a slot. M1 byte gate (`bytes_used`) is
  admission-only; `count==0` is the drain predicate. Callbacks fire through `dispatch_enter/leave`
  (`:89`): `on_writable` while depth>1, `on_drain` at depth 0 (destructive tail).
- **Recv machine `KlDgramRecv`** (`datagram_recv.c`): one owned inbound slot (`inbound`, capacity
  `recv_cap`); readiness `kl_dgram_recv_on_readable` (`:180`) LOOPS `pull → recv_finalize →
  recv_present`, re-checking `!paused && !stopped && recv_inflight` after every delivery; `recv_finalize`
  (`:30`) enforces peer-mandatory + truncation (driven by `recv_cap`) + local flags. `recv_inflight`
  marks armed interest — **readiness disarm (pause/stop) clears it synchronously inside a callback**.
- **Lifetime:** the life-token + close coordinator own the from-callback deferral via the `busy`
  handshake (`on_activity(+1/-1)`, `kl_dgram_core_dispatch_begin/end`); the destructive tail
  (`close_detach`/`core_rx_final`) runs only when `busy==0` (outermost frame).
- **`init_ex`** (`datagram.c:274`) stores **no** capture mask; `KlDatagramConfig` has no `rx_caps`;
  completion recv hard-codes `capture = KL_DGRAM_RX_PKTINFO`. `KlUdpServer` embeds `KlDatagram dg` (M4)
  with `mmsg_batch`/`recv_gro` currently inert.

## 3. Decisions (rev 3)

**D-M5-1 — single-datagram CONTRACT unchanged; the core gains DEFINED internal seams.** Public
send/recv/close semantics and the allocation-free hot path are preserved. The batch/GSO/GRO paths are
internal core/facade extensions: an **enqueue-only + batch-flush** send path (§4), a **borrowed-view
recv delivery seam** with a batch-backed yield adapter (§5), and **one core-owned `ext` pointer**
(`KlDgramBatchExt *`, NULL when unused, O-F).

**D-M5-2 — caller-preallocated; no hot allocation.** All batch/GSO storage (provider blocks, staging
arrays, the recv buffer, the GSO staging buffer) is allocated once at create/attach; nothing allocates
on a send/recv edge.

**D-M5-3 — readiness-only (O-C).** The extension activates only when `core->completion == 0`. On a
completion datagram, `kl_datagram_recv_attach_batch` is refused; `kl_datagram_send_batch` /
`send_gso` admit+drain via the single-flight completion pump (correct, unbatched).

## 4. SEND — enqueue-only transaction, prefix-only retirement, atomic GSO (B1, B2, B5)

### 4.1 The enqueue-only batch transaction (B1)

The core send FIFO stays the sole admission authority. Batching adds an **enqueue-only** primitive that
does NOT trigger the per-datagram fast path or `send_pump`:

- New send-machine primitive `kl_dgram_send_enqueue(s, msg) → KlDatagramSendStatus`: run the slot
  acquire + byte gate + copy-into-slot + FIFO enqueue (`bytes_used += len`), **without** the readiness
  fast path and **without** `send_pump`. Returns `ACCEPTED` / `WOULD_BLOCK` (no free slot or byte gate)
  / `TOO_LARGE` (permanent: one datagram > `send_slot_cap`, or > the whole byte budget) / `CLOSED` /
  `ERROR`.

`kl_datagram_send_batch(dg, b, descs, n, KlDatagramSendStatus *stop) → int accepted`:
1. Open one busy bracket (`dispatch_enter`), so all edge callbacks defer to the end.
2. For `i` in `[0, n)`: `st = kl_dgram_send_enqueue(descs[i])`. If `st != ACCEPTED`: `*stop = st`;
   `accepted = i`; **stop** (acceptance is a prefix — no reordering, no skipping).
3. If all accepted: `accepted = n`, `*stop = ACCEPTED`.
4. Run **one** `kl_dgram_send_flush_batch` (readiness, §4.2) / `send_pump` (completion) inside the
   bracket; `dispatch_leave` fires the deferred `on_writable`/`on_drain` edges.
5. Return `accepted`.

**Return semantics (frozen):** `accepted` = datagrams the core TOOK (copied + queued; now core-owned,
draining on writable edges — invariant 4). `[accepted, n)` were NOT taken (caller retains them).
`*stop` classifies `descs[accepted]`: `WOULD_BLOCK` = transient (retry the remainder later after a
writable edge); `TOO_LARGE` = permanent (that datagram can never be admitted — the caller must drop or
resize it, not retry the same batch); `ERROR`/`CLOSED` = sticky/closed (stop). There is **no** hidden
partial admission and **no** direct-send of individual entries.

### 4.2 Readiness batch-drain: reserve head-run, one sendmmsg, prefix-only retire (B2)

`kl_dgram_send_flush_batch(s, int max)` — the readiness writable-flush when a TX batch is present
(replacing `send_pump`):
1. **Reserve** the head-run `k = min(max, count)` (`max` = the batch's `n_slots`).
2. **Stage** `KlDgramTxDesc[k]` from `ring[head .. head+k)` (data/len/peer/local/tos) into the batch's
   `tx_batch` block.
3. **Submit once**: `submit_batch(ctx, fd, tx_batch, descs, k) → sent` (provider `send_batch` = one
   `sendmmsg`, or the portable single-`send` loop under `!TX_BATCH`, §6.4). `sent ∈ [0, k]` **or** `-1`.
4. **Retire exactly `sent`** from the head (release slots, `bytes_used -= len`, advance `head`,
   `count -= sent`) via the existing retire path (byte accounting + full→non-full + drain edges).
5. Interpret the remainder:
   - `sent == k`: fully drained this batch — loop for the next head-run if `count > 0` (bounded).
   - `0 ≤ sent < k` (incl. 0, i.e. EAGAIN): **transient** — retain the remainder in FIFO order,
     re-arm WRITE interest, return. Never drop.
   - `sent == -1` (hard error, no prefix info — `sendmmsg` cannot name the offending descriptor):
     **isolate via single-send** — submit ONLY the head slot through the existing single submit
     adapter (`send_submit_slot(s,0)`): `DONE` → retire the head, continue; `WOULD_BLOCK` → retain +
     re-arm; **hard error → drop exactly that (now-identified) head slot** (`dropped++`, sticky `err`),
     advance. This isolates a permanently-bad descriptor to its own syscall; the batch path **never
     infers** which of the `k` slots failed.

Readiness send is synchronous, so there is no async in-flight batch (`inflight_n` stays 0); FIFO,
byte budget, edges, single-flight, and close (`discard_queued` drops the queued remainder; readiness
retire is always `RETIRED` → DETACHED) are all the existing machinery over a prefix. Completion mode
keeps `send_pump` (single-flight); `flush_batch` is never wired there.

### 4.3 GSO — atomic nseg admission, copy-once, GSO-or-normal-sequence (B5)

`kl_datagram_send_gso(dg, buf, total_len, seg, peer)`:
1. **Preflight**: `nseg = ceil(total_len / seg)` logical segments (last may be short). Bounds:
   `total_len ≤ GSO staging capacity` and `seg ≤ send_slot_cap`.
2. **Atomic admission** of all `nseg` segments against **both** limits — `nseg` free slots AND
   (BOTH policy) `bytes_used + total_len ≤ byte_budget`. **All-or-nothing:** if `nseg > send_slots`
   or `total_len > byte_budget` ⇒ `TOO_LARGE` (permanent); if they don't fit right now ⇒ `WOULD_BLOCK`
   (nothing admitted). **No partial hidden admission.**
3. **Copy once** (copy-at-submit invariant): the whole `buf` into the core-owned GSO staging buffer;
   reserve the `nseg` slots + `total_len` bytes as one GSO reservation.
4. **Submit**: GSO available AND not latched (§6.3) ⇒ one `send_gso(staging, total_len, seg, peer)`
   syscall ⇒ on `DONE` retire the whole `nseg`/`total_len` reservation atomically. Otherwise ⇒ the
   **same admitted sequence** sent normally: `nseg` single `send`s from the staging buffer at segment
   offsets, retiring one segment's reservation each. `WOULD_BLOCK` on submit re-arms WRITE and retains
   the reservation (drains on the writable edge). Byte accounting = `total_len` reserved at admission,
   released as segments retire (all at once on GSO success, per-segment on fallback). GSO occupies
   `nseg` logical slots (not one); fallback sends the identical admitted sequence.

## 5. RECV — borrowed-view delivery seam, held-buffer pause, core-owned teardown (B3, B4)

### 5.1 Borrowed-view delivery (B3) — the owned inbound slot is NOT repointed

The canonical `inbound` slot (owned, capacity `recv_cap`, drives single-datagram finalization) is
**left untouched** in batch mode. Instead the recv machine gains a borrowed-view delivery path used
when a batch is attached:

```c
typedef struct {                     /* borrowed; valid ONLY for the delivery call */
    const void *data; size_t len;    /* into ext rx-batch storage (capture cap = the batch slot bufsz) */
    KlSockAddr  peer;  int has_local; KlSockAddr local;
    int         tos;   unsigned flags;   /* KL_DGRAM_TRUNCATED / KL_DGRAM_HAS_LOCAL */
} KlDgramRxView;
```
- **Finalization on the view:** peer-mandatory + local-flags as today, but **truncation is the
  provider's `meta.truncated`** (recvmmsg `MSG_TRUNC`), NOT the `recv_cap` clamp. A GRO-coalesced
  buffer (up to the batch slot bufsz, which is sized for GRO — e.g. 64 KiB) is **not** clamped by
  `recv_cap`. This is the reviewer's "borrowed-view seam" (better throughput; no copy into the owned
  slot).
- **Lifetime:** the view points into the ext's `rx_batch` payload storage, valid only for the call;
  the storage is core-owned (§5.4) and reclaimed at the destructive tail.

### 5.2 The batch-backed yield adapter + GRO split

The facade wires a readiness **yield adapter** (in place of `dg_rdy_pull`) that fills the machine's
delivery via the view. The machine's `on_readable` loop drives it verbatim (one delivery per
iteration, `!paused && !stopped` re-checked each time). Ext cursor state (in `KlDgramBatchExt`):
`KlDgramRxSlot slots[n_slots]`, `int n_filled`, `int i`, `size_t seg_off`.
- **Refill** when exhausted: one `recv_batch` (RX_BATCH) or one single `recv` (fallback, `n_slots`
  effectively 1) → `n_filled`; `0` = EAGAIN (drained, ends the edge); `-1` = `recv_fail`.
- **Yield one logical datagram** as a `KlDgramRxView`: a GRO-coalesced slot
  (`meta.gro_seg > 0 && len - seg_off > gro_seg`) with **no** `on_recv_segments` registered yields the
  next `gro_seg`-sized segment (`seg_off += gro_seg`; `i` stays); the final short segment advances `i`.
  Otherwise the whole slot is yielded (plain datagram, last segment, or — with `on_recv_segments` set —
  the whole coalesced buffer with `gro_seg` carried), `i++`, `seg_off = 0`.
- **Deliver:** `on_recv(view)` for a logical datagram; `on_recv_segments(view + segment_size)` for a
  whole coalesced buffer (O-A explicit).

### 5.3 Pause / resume / stop lifetime (held buffer)

`recv_batch` has already consumed datagrams from the kernel, so a pause mid-buffer must not lose them:
- **Pause** (from a callback mid-buffer): `on_readable` breaks; the ext cursor (`slots`, `n_filled`,
  `i`, `seg_off`) is **retained** (a held buffer of ≥1 undelivered logical datagrams).
- **Resume**: drain the retained buffer first (deliver held logical datagrams one at a time,
  re-checking pause/stop — analogous to the completion held-datagram), THEN re-arm READ interest. So
  buffered datagrams are not stranded behind readiness.
- **Stop / close**: discard the retained ext buffer (no delivery).

### 5.4 Ownership + teardown safety (B4, O-F) — corrected

Rev 2's "`recv_inflight` prevents reclamation during delivery" is **wrong** — readiness `disarm`
clears `recv_inflight` inside the callback. The frozen model:
- **Core adopts the extension on attach.** `kl_datagram_recv_attach_batch(dg, b)` **transfers
  ownership** of `b`'s ext state to the core (`core->ext = ...`); after attach the caller MUST NOT free
  `b`. Attach is permitted only before `recv_start`, on a readiness datagram.
- **Reclamation at the destructive tail.** The ext is freed at the core's terminal
  (`close_detach`/`core_rx_final`), which the `busy` handshake defers past any in-progress delivery
  frame (the `on_readable` loop brackets `on_activity`). So the borrowed view + ext buffer stay valid
  for the whole delivery loop; slot N+1 storage is never freed mid-loop.
- **`!stopped` breaks before slot N+1.** A stop/close from delivery N sets `stopped=1`; the loop's
  top-of-iteration `!stopped` check returns before the next yield — slots/segments N+1.. are never
  delivered. (This, plus deferred ext free — NOT `recv_inflight` — is the safety.)
- **Standalone `kl_datagram_batch_free(b)`** is permitted only while `b` is **unattached** or the
  datagram is **CLOSED** (provably idle); calling it on an attached, non-closed datagram is refused
  (`-1`) — the core owns it, freed via close. (A **SEND** batch — §7 — is transient/caller-owned: its
  `tx` staging is copied into core slots at enqueue and referenced only during the synchronous flush,
  so nothing points into it after `send_batch` returns; it needs no adoption.)

## 6. Capability model (semantic cleanup)

### 6.1 provider_caps = provider SUPPORT (directional)

`kl_datagram_provider_caps()` reports what the provider/fd SUPPORTS — new bits added to
`keel/datagram.h`, reported by the provider `caps()` op:
```
KL_DGRAM_CAP_RX_BATCH (1u<<5)  /* recv_batch + rx_batch_new/free present + caps() reports it */
KL_DGRAM_CAP_TX_BATCH (1u<<6)  /* send_batch + tx_batch_new/free present + caps() reports it */
KL_DGRAM_CAP_GSO      (1u<<7)  /* send_gso present — advisory (§6.3) */
KL_DGRAM_CAP_GRO      (1u<<8)  /* the provider CAN capture UDP_GRO — advisory support, NOT per-socket */
```
Directional: a `RECV` batch checks `RX_BATCH`; `SEND`/GSO check `TX_BATCH`/`GSO` — one never gates the
other.

### 6.2 Accepted RX mask = separate per-socket state; GRO needs BOTH (semantic cleanup, P1-c)

`provider_caps` must NOT be synthesized from the M0 accepted mask (that is enabled-per-socket state,
not provider support). Frozen:
- Add `unsigned accepted_rx_caps;` to `KlDatagramConfig` (O-E) — the `KL_DGRAM_RX_*` mask the socket
  actually has enabled (from `KlDatagramPrep.rx_caps`); `kl_datagram_init_ex` **masks it to known
  `KL_DGRAM_RX_*` bits** and stores it separately on the core (not folded into `caps`). A caller who
  passes 0 gets no RX-derived features (graceful).
- **GRO ACTIVATES iff BOTH** `provider_caps & KL_DGRAM_CAP_GRO` (the provider supports GRO) **AND**
  `accepted_rx_caps & KL_DGRAM_RX_GRO` (this socket enabled it). Otherwise `meta.gro_seg` stays 0 and
  delivery is per-datagram (graceful). `kl_datagram_provider_caps` keeps reporting support; a distinct
  accessor (or the granted-caps `kl_datagram_caps`) reflects the AND for GRO activation.

### 6.3 GSO — advisory cap + core-owned per-fd latch (P1-e, unchanged from rev 2)

`KL_DGRAM_CAP_GSO` is advisory (op present); a runtime `int gso_unsupported` on the **core** (per-fd)
latches on first-use `EOPNOTSUPP` and switches to the per-segment fallback (§4.3); `provider_caps` does
not mutate. `kl_datagram_gso_active(dg)` exposes the latch.

### 6.4 Create never fails on capability absence (B6)

`kl_datagram_batch_create(dg, dir, n_slots, slot_bufsz)` allocates the provider `rx_batch`/`tx_batch`
block for a direction **only when that direction's cap is present**; when absent it leaves the block
NULL and the flush/refill takes the **portable fallback** (single `send`/`recv` loop, §4.2/§5.2). So
create **succeeds regardless of caps** — otherwise the fallback would be unreachable. Create fails only
for a **completion-mode** datagram (O-C) or **OOM**. A `RECV` batch is still useful without `RX_BATCH`
(GRO-split via a single-`recv` refill).

## 7. Public surface (`keel/datagram_batch.h`, rev 3)

```c
typedef struct KlDatagramBatch KlDatagramBatch;
typedef enum { KL_DGRAM_BATCH_RECV=1, KL_DGRAM_BATCH_SEND=2, KL_DGRAM_BATCH_BOTH=3 } KlDgramBatchDir;

/* Create once (allocates ext + present-cap provider blocks + staging). NULL only on completion-mode
 * datagram (O-C) or OOM — never on capability absence (§6.4). */
KlDatagramBatch *kl_datagram_batch_create(KlDatagram *dg, KlDgramBatchDir dir, int n_slots, size_t slot_bufsz);
/* Free a batch. Permitted only while UNATTACHED or the datagram is CLOSED (§5.4); else -1. */
int              kl_datagram_batch_free(KlDatagramBatch *b);

/* SEND (transient/caller-owned): enqueue-only accepted prefix + one batch flush (§4.1). Returns the
 * accepted count; *stop classifies descs[accepted] (ACCEPTED if all n taken). */
int kl_datagram_send_batch(KlDatagram *dg, KlDatagramBatch *b, const KlDgramTxDesc *descs, int n,
                           KlDatagramSendStatus *stop);
/* GSO: atomic nseg admission then one send_gso or the normal sequence (§4.3). */
KlDatagramSendStatus kl_datagram_send_gso(KlDatagram *dg, const void *buf, size_t total_len,
                                          uint16_t segment_size, const KlSockAddr *peer);
int  kl_datagram_gso_active(const KlDatagram *dg);

/* RECV (core-adopted on attach, §5.4): attach BEFORE recv_start; the machine delivers via the
 * borrowed view (§5.1), GRO split by default. Whole-buffer delivery via on_recv_segments. */
int  kl_datagram_recv_attach_batch(KlDatagram *dg, KlDatagramBatch *b);
typedef void (*KlDatagramRecvSegmentsFn)(void *ud, const void *data, size_t len, size_t segment_size,
                                         const KlSockAddr *peer, const KlSockAddr *local, unsigned flags);
void kl_datagram_recv_segments(KlDatagram *dg, KlDatagramRecvSegmentsFn cb, void *ud);
```
`keel/datagram.h`: the four `provider_caps` bits (§6.1) + `KlDatagramConfig.accepted_rx_caps` (§6.2) +
`kl_datagram_gso_active`. **No** explicit `recv_batch` (mode B deferred). `keel/socket_dgram.h`
unchanged. Internal: `KlDgramCore` gains `KlDgramBatchExt *ext` + `int gso_unsupported` +
`unsigned accepted_rx_caps`; the send machine gains `kl_dgram_send_enqueue` +
`kl_dgram_send_flush_batch` + the `submit_batch` adapter; the recv machine gains the borrowed-view
yield/deliver path.

## 8. `KlUdpServer` migration (unchanged from rev 2)

No API/ABI change. `kl_udp_server_init` threads `KlDatagramPrep.rx_caps` into `cfg.accepted_rx_caps`;
if `mmsg_batch>1` / `recv_gro` and the provider reports `RX_BATCH`/`GRO`, it `batch_create(RECV)` +
`recv_attach_batch` before `recv_start`. Handler unchanged (one logical datagram at a time, GRO
split). Reply stays single-send (O-B). Batch is core-adopted → reclaimed at the server datagram's
teardown. Absent caps ⇒ M4 single-flight (no regression).

## 9. Byte-only queue OUT (O-D); post-M5 options (unchanged)

Not moved into the core. Post-M5 `KlUdp` options: (1) keep it in a deprecated `KlUdp`; (2) replace with
explicit `send_slots` + `send_byte_budget` (M1 BOTH) in a breaking release; (3) an optional
allocator-backed queue adapter above `KlDatagram`, outside Tier-1.

## 10. Non-goals

No production code/ABI/migration here. Don't change the single-datagram delivery/close/quarantine
CONTRACT or the allocation-free hot path. Deferred: native completion batching (O-1); explicit mode-B
`recv_batch`; reply batching (O-B); byte queue (O-D); `KlUdp` reduction.

## 11. Test matrix

- **Send transaction (§4.1):** `send_batch` admits an accepted prefix, `*stop` classifies the first
  refusal (WOULD_BLOCK vs TOO_LARGE vs ERROR); the remainder is untaken; enqueue-only never
  direct-sends an individual entry (assert one `sendmmsg`, not N).
- **Prefix-only retire + isolation (§4.2):** short `sendmmsg` (sent<k) retains + re-arms; a provider
  `send_batch` returning `-1` triggers single-send of the head (a permanently-bad head is dropped and
  counted; a good head is retired) — no middle slot inferred. Fast path AND `NULL send_batch` fallback
  deliver identical bytes/order.
- **GSO (§4.3):** `nseg` atomic admission (no partial), TOO_LARGE when `nseg>send_slots` /
  `total>budget`, WOULD_BLOCK when it can't fit now; GSO path one syscall vs fallback `nseg` sends
  deliver the same segments; `total`-byte accounting released correctly; GSO latch → fallback.
- **Borrowed view + GRO (§5.1/§5.2):** one recvmmsg → N serial `on_recv` (view data into ext storage,
  not the owned slot; GRO buffer > `recv_cap` delivered un-clamped); GRO split per-segment by default,
  whole to `on_recv_segments`; GRO active only with provider GRO support AND `accepted_rx_caps` GRO.
- **Pause/resume held buffer (§5.3):** pause mid-buffer retains undelivered datagrams; resume drains
  them before re-arm; stop discards them — none lost, none double-delivered.
- **Teardown ownership (§5.4):** stop/close from delivery N → N+1 never delivered; ext freed once at
  the destructive tail (deferred past the callback); `batch_free` on an attached non-closed datagram
  refused; UAF/leak-free under ASan/UBSan/LSan.
- **Caps (§6):** `provider_caps` reports support (directional), not the accepted mask; `batch_create`
  succeeds with a cap absent and the fallback runs; RX-only / TX-only creation.
- **`KlUdpServer` (§8) + regression:** burst → handler one-at-a-time; caps absent → still delivered;
  DNS single-flight + existing suites unchanged; ext pointer NULL / zero alloc when no batch attached.

## 12. Increment breakdown (each its own freeze-then-code review)

1. **M5.1** — the four `provider_caps` bits + `KlDatagramConfig.accepted_rx_caps` (masked) stored
   separately + GRO = provider-support AND accepted; `KlDgramBatchExt` + `KlDatagramBatch` create/free
   (create-never-fails-on-caps); the core `ext`/`gso_unsupported`/`accepted_rx_caps` fields. No wiring.
2. **M5.2 — send.** `kl_dgram_send_enqueue` + `kl_dgram_send_flush_batch` (prefix retire + single-send
   isolation) + `submit_batch` + portable fallback; `kl_datagram_send_batch` (transaction + `*stop`);
   `kl_datagram_send_gso` (atomic nseg admission + latch + fallback).
3. **M5.3 — recv.** the borrowed-view seam + yield adapter + GRO split + held-buffer pause/resume +
   `recv_attach_batch` (core adoption) + `recv_segments`; teardown-from-delivery-N + destructive-tail
   reclamation tests.
4. **M5.4 — `KlUdpServer` opt-in.** thread `accepted_rx_caps`; wire `mmsg_batch`/`recv_gro`; parity +
   every-backend conformance + teardown ordering.
5. **(post-M5)** `KlUdp` decision (§9) + mode B: separate freezes.

**Order:** M5.1 → {M5.2, M5.3} → M5.4. Independent of any `KlUdp` change.

## 13. Findings resolution map

| # | rev-2 blocker | rev-3 resolution |
|---|---------------|------------------|
| B1 | normal admission defeats batching | §4.1 enqueue-only transaction (no fast path, no per-datagram submit) + one flush; prefix acceptance + `*stop` semantics for WOULD_BLOCK/TOO_LARGE/hard error |
| B2 | can't drop an arbitrary failed slot | §4.2 retire only the reported sent prefix; short/zero = transient (retain+re-arm); `-1` = single-send isolation of the head (never infer) |
| B3 | must not repoint the canonical inbound slot | §5.1 borrowed-view seam ({data,len,meta} into ext storage); owned slot untouched; truncation from provider meta, not `recv_cap` clamp |
| B4 | teardown ownership unsafe | §5.4 core adopts the attached ext; destructive-tail reclamation (not `recv_inflight`); `batch_free` only unattached/CLOSED; `!stopped` breaks before N+1 |
| B5 | GSO queue semantics undefined | §4.3 atomic `nseg` admission vs slot+byte (no partial), copy-once, GSO-or-normal-sequence, atomic/per-segment retire, `total` byte accounting |
| B6 | fallback contradicts creation | §6.4 create never fails on cap absence; provider block optional (NULL ⇒ fallback) |
| — | provider_caps vs accepted RX mask | §6.1/§6.2 provider_caps = support (directional); `accepted_rx_caps` separate per-socket; GRO needs BOTH |

## 14. Open decisions for the reviewer (rev 3)

None outstanding — O-A..O-F are ruled. Rev 3 requests acceptance-for-implementation of the frozen
contracts in §4–§6, after which M5.1 proceeds.
