# Datagram M5: Batch / GSO / GRO high-throughput extension (design freeze, rev 6)

**Status:** DESIGN FREEZE (docs-only), **revision 6**. **This docs commit changes no ABI**; the design
it freezes intentionally introduces ABI additions in M5.1; appending
`KlDatagramConfig.accepted_rx_caps` (O-E) and a `KL_IO_UNSUPPORTED` `KlIoStatus` enumerator (§6.3),
both accepted because there are no consumers. No production code or consumer migration here. Rev 6
freezes the two remaining lifecycle/availability contracts (the queued-GSO SEND-batch lifetime, and
the completion-datagram creation rules) plus the M5.1 allocation contract, so the increment chain
(M5.1–M5.4) is implementable.

### Rev 6 changelog: two lifecycle/availability blockers + allocation contract (map in §13)

- **E1 (queued-GSO SEND-batch lifetime):** §4.5/§7, a queued GSO group keeps reading `b`'s buffer
  **asynchronously** across later writable/completion events, so a SEND batch is **not** transient once
  GSO is in flight. Frozen: `b` records its owning datagram + direction + `gso_busy`; the send APIs
  reject a `b` from another datagram or without SEND direction; `kl_datagram_batch_free(b)` returns
  `-1` while `gso_busy`; close/discard releases the group and clears `gso_busy` but does **not** free a
  caller-owned `b`; the caller must keep **both** `b` and its datagram alive until the group retires or
  close/discard completes.
- **E2 (completion creation rules, make the fallback reachable):** §3/§6.4/§7, SEND-batch creation is
  allowed on **both** readiness and completion datagrams (so the completion single-flight send fallback
  is reachable: D-M5-3 was self-contradictory); RECV attachment stays readiness-only; **BOTH** is
  refused on a completion datagram (create SEND explicitly). Completion `send_batch` = enqueue + the
  single-flight pump; completion `send_gso` = the admitted per-segment fallback (no native completion
  GSO seam in M5).
- **Allocation contract (M5.1):** §6.4; `batch_create` validates `n_slots > 0`, `slot_bufsz > 0`, and
  guards `n_slots * slot_bufsz` against `SIZE_MAX` before allocating.

### Rev 5 changelog: three rev-4 contradictions + editorial (map in §13)

- **D1 (batch hard-error vs sticky `err`):** §4.2/§4.4, a per-datagram hard error in the batch drain
  uses the **recoverable send-error policy** (drop the identified datagram, `dropped++`, set the
  **facade `last_error`**; do NOT set the send machine's sticky `s->err`; continue draining), because
  `KlDgramSend.err` halts all future pumping and turns graceful close abortive. Sticky `s->err` stays
  reserved for genuinely fatal transport failure.
- **D2 (GSO error classification):** §4.3/§6.3; classify `send_gso`'s `-1` immediately via
  `kl_sock_io_status`: `KL_IO_WOULD_BLOCK` → retain + re-arm; `KL_IO_UNSUPPORTED` → latch
  `gso_unsupported` + switch **this group** to fallback; any other hard error → the recoverable
  send-error policy (drop the group), **never** a fallback retransmit. Only UNSUPPORTED latches.
- **D3 (GSO storage, no hot alloc, explicit capacity):** §4.3/§7, `kl_datagram_send_gso` now takes a
  **caller-preallocated SEND/BOTH batch**; the group staging buffer is that batch's storage (capacity
  `n_slots × slot_bufsz`), allocated at `batch_create`. **No first-send allocation** (D-M5-2/I8).
- **Editorial:** §13/§14 headings corrected off "rev 3".

### Rev 4 changelog: the three rev-3 blockers + two corrections (map in §13)

- **C1 (send `-1` must be classified before isolation):** §4.2; after `send_batch` returns `-1`,
  inspect `kl_sock_io_status(provider)` **immediately** (before any other provider call overwrites it):
  `KL_IO_WOULD_BLOCK` retains the entire unsent prefix and re-arms; only a hard error enters single-send
  isolation.
- **C2 (GSO is a queued FIFO group):** §4.3, a GSO reservation is appended at the FIFO **tail** and
  submitted only when its first segment reaches the head; the group records `nseg` / remaining /
  offsets / GSO-or-fallback state; its staging buffer is pinned until the group retires, so a second
  outstanding GSO is `WOULD_BLOCK` (one preallocated group buffer). Close/discard releases the whole
  remaining group reservation.
- **C3 (adoption vs the public handle, full transfer):** §5.4/§7, a **successful**
  `kl_datagram_recv_attach_batch` **consumes `b`**; the caller must never touch it again (no
  `batch_free`). A failed attach leaves ownership with the caller. This removes the "how does `b` learn
  it was reclaimed" ambiguity: there is nothing left in `b` for the caller.
- **Correction, GSO validation/overflow:** §4.3, `segment_size != 0`; non-NULL `buf` for nonzero
  length; overflow-safe `nseg`; guarded `size_t`↔slot-count and byte-budget arithmetic.
- **Correction, ABI wording:** the intro now states the docs commit changes no ABI while M5.1
  intentionally will.

**Supersedes** the M5 framing in
[`datagram_consolidation_design.md`](datagram_consolidation_design.md) §6.5. `KlUdp` is UNTOUCHED by
M5; its reduction is a deferred post-M5 decision.

Rev-1/2 background (layering, ground truth, KlUdpServer shape, byte-queue disposition) is retained in
§1–§2 and §8–§9; the frozen contracts are in **§3–§6** and the findings map in §13.

### Rev 3 changelog: the six rev-2 blockers, frozen (map in §13)

- **B1 (send admission defeats batching):** §4.1 freezes an **enqueue-only batch transaction**, an
  accepted prefix is copied into slots with NO per-datagram submit and NO readiness fast path, then one
  batch flush runs under the same busy bracket. Acceptance-stop + return semantics (ACCEPTED /
  WOULD_BLOCK / TOO_LARGE / hard error) are defined precisely.
- **B2 (can't drop an arbitrary failed slot):** §4.2; retire only the provider's reported sent prefix;
  a short/zero return is transient (retain remainder + re-arm); a hard `-1` triggers **single-send
  isolation of the head slot** (never infer which slot failed).
- **B3 (don't repoint the canonical inbound slot):** §5.1 adopts a **borrowed-view delivery seam**
  ({data,len,meta} into ext storage), the owned inbound slot is not mutated and GRO buffers are not
  clamped by `recv_cap`. Finalization/truncation operate on the view.
- **B4 (extension teardown ownership):** §5.4, the **core adopts/owns the attached recv extension**;
  reclamation is at the core's **destructive tail** (not `recv_inflight`, which readiness disarm clears
  in-callback); detach/free is permitted only while unattached or CLOSED (else UAF).
- **B5 (GSO queue semantics):** §4.3; **atomic preflight admission of all `nseg` logical segments**
  against slot count AND byte budget (no partial), copy-once, then GSO-or-normal-sequence, with atomic
  (GSO) / per-segment (fallback) retirement and `total`-byte accounting.
- **B6 (fallback vs creation):** §6.4; `kl_datagram_batch_create` **never fails on capability
  absence**; the provider block is optional (NULL ⇒ the portable fallback path), so the fallback is
  reachable. Create fails only for a completion-mode datagram (O-C) or OOM.
- **Semantic cleanup:** §6.1/§6.2; `provider_caps` stays **provider support**; the M0 **accepted RX
  mask is separate per-socket state**; **GRO activates only when BOTH** provider GRO support AND
  `accepted_rx_caps & KL_DGRAM_RX_GRO` hold. CAP_GRO is not synthesized into provider support.

### Rulings applied

- **O-E (settled):** append `unsigned accepted_rx_caps` to `KlDatagramConfig` (ABI break accepted, no
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
  marks armed interest: **readiness disarm (pause/stop) clears it synchronously inside a callback**.
- **Lifetime:** the life-token + close coordinator own the from-callback deferral via the `busy`
  handshake (`on_activity(+1/-1)`, `kl_dgram_core_dispatch_begin/end`); the destructive tail
  (`close_detach`/`core_rx_final`) runs only when `busy==0` (outermost frame).
- **`init_ex`** (`datagram.c:274`) stores **no** capture mask; `KlDatagramConfig` has no `rx_caps`;
  completion recv hard-codes `capture = KL_DGRAM_RX_PKTINFO`. `KlUdpServer` embeds `KlDatagram dg` (M4)
  with `mmsg_batch`/`recv_gro` currently inert.

## 3. Decisions

**D-M5-1: single-datagram CONTRACT unchanged; the core gains DEFINED internal seams.** Public
send/recv/close semantics and the allocation-free hot path are preserved. The batch/GSO/GRO paths are
internal core/facade extensions: an **enqueue-only + batch-flush** send path (§4), a **borrowed-view
recv delivery seam** with a batch-backed yield adapter (§5), and **one core-owned `ext` pointer**
(`KlDgramBatchExt *`, NULL when unused, O-F).

**D-M5-2: caller-preallocated; no hot allocation.** All batch/GSO storage (provider blocks, staging
arrays, the recv buffer, the GSO staging buffer) is allocated once at create/attach; nothing allocates
on a send/recv edge.

**D-M5-3: batching fast paths are readiness-only; SEND is creatable on both backends (O-C, E2).** The
recvmmsg / sendmmsg fast paths and GRO activate only when `core->completion == 0`. Creation/activation
rules (E2), so the completion send fallback is reachable:
- `batch_create(RECV)` requires a readiness datagram (completion ⇒ NULL); `recv_attach_batch` stays
  readiness-only.
- `batch_create(SEND)` is allowed on **both** readiness and completion datagrams.
- `batch_create(BOTH)` is refused on a completion datagram (create `SEND` explicitly).
- On a **completion** datagram: `kl_datagram_send_batch` = `enqueue`-prefix + the existing single-flight
  completion pump (`send_pump`, posts one op at a time, correct, unbatched); `kl_datagram_send_gso` =
  the admitted per-segment fallback (the group's `mode` is always `FALLBACK`; each segment is a normal
  single-flight completion send). A native completion GSO seam (one `send_gso` op posted to the
  backend) is deferred (§10, O-1). The queued-GSO buffer lifetime (§4.5) applies on both backends.

## 4. SEND: enqueue-only transaction, prefix-only retirement, atomic GSO (B1, B2, B5)

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
   `accepted = i`; **stop** (acceptance is a prefix, no reordering, no skipping).
3. If all accepted: `accepted = n`, `*stop = ACCEPTED`.
4. Run **one** `kl_dgram_send_flush_batch` (readiness, §4.2) / `send_pump` (completion) inside the
   bracket; `dispatch_leave` fires the deferred `on_writable`/`on_drain` edges.
5. Return `accepted`.

**Return semantics (frozen):** `accepted` = datagrams the core TOOK (copied + queued; now core-owned,
draining on writable edges: invariant 4). `[accepted, n)` were NOT taken (caller retains them).
`*stop` classifies `descs[accepted]`: `WOULD_BLOCK` = transient (retry the remainder later after a
writable edge); `TOO_LARGE` = permanent (that datagram can never be admitted, the caller must drop or
resize it, not retry the same batch); `ERROR`/`CLOSED` = sticky/closed (stop). There is **no** hidden
partial admission and **no** direct-send of individual entries.

### 4.2 Readiness batch-drain: reserve head-run, one sendmmsg, prefix-only retire (B2)

`kl_dgram_send_flush_batch(s, int max)`, the readiness writable-flush when a TX batch is present
(replacing `send_pump`):
1. **Reserve** the head-run `k = min(max, count)` (`max` = the batch's `n_slots`).
2. **Stage** `KlDgramTxDesc[k]` from `ring[head .. head+k)` (data/len/peer/local/tos) into the batch's
   `tx_batch` block.
3. **Submit once**: `submit_batch(ctx, fd, tx_batch, descs, k) → sent` (provider `send_batch` = one
   `sendmmsg`, or the portable single-`send` loop under `!TX_BATCH`, §6.4). `sent ∈ [0, k]` **or** `-1`
   (the provider contract returns `-1` for **both** EAGAIN and a hard error, C1).
4. **Retire exactly `sent`** from the head (release slots, `bytes_used -= len`, advance `head`,
   `count -= sent`) via the existing retire path (byte accounting + full→non-full + drain edges).
5. Interpret the result:
   - `sent == k`: fully drained this batch; loop for the next head-run if `count > 0` (bounded).
   - `0 ≤ sent < k` (incl. 0): **transient**; retain the remainder in FIFO order, re-arm WRITE
     interest, return. Never drop.
   - `sent == -1` (**C1, classify before isolating**): call `kl_sock_io_status(provider)`
     **immediately after** `send_batch`, before any other provider call can overwrite it, and cache the
     `KlIoStatus`:
     - `KL_IO_WOULD_BLOCK`: **transient**; retain the **entire** unsent prefix (nothing was sent), re-arm
       WRITE interest, return. (`sendmmsg` blocked on the head; identical handling to `sent == 0`.)
     - any other status (a **hard error**): **isolate via single-send**; submit ONLY the head slot
       through the existing single submit adapter (`send_submit_slot(s,0)`), re-classifying its own
       result via `kl_sock_io_status`: `DONE` → retire the head, continue; `KL_IO_WOULD_BLOCK` → retain +
       re-arm; **hard error → the recoverable send-error policy (§4.4)** on exactly that
       (now-identified) head slot, then continue. This isolates a permanently-bad descriptor to its own
       syscall; the batch path **never infers** which of the `k` slots failed and **never** treats an
       EAGAIN as a drop.

Readiness send is synchronous, so there is no async in-flight batch (`inflight_n` stays 0); FIFO,
byte budget, edges, single-flight, and close (`discard_queued` drops the queued remainder; readiness
retire is always `RETIRED` → DETACHED) are all the existing machinery over a prefix. Completion mode
keeps `send_pump` (single-flight); `flush_batch` is never wired there. `flush_batch` defensively
rejects (`-1`) a completion machine or an outstanding `inflight_n != 0`, so a stray batch submit can
never duplicate an in-flight head.

**One batch attempt per public call (M5.2a decision, revised P1-3).** `kl_datagram_send_batch` does
exactly ONE `flush_batch` (readiness): it does NOT persistently attach the transient SEND batch to the
datagram. A short/EAGAIN **remainder is retained and arms WRITE, then drains via the ORDINARY
writable-edge single-flight path** (`dg_on_ready` → `send_pump`), NOT a re-entered `flush_batch`. This
preserves the transient SEND-batch ownership contract (§4.5, the batch is caller-owned, referenced
only during the synchronous call except for a GSO group) at the cost of a **deliberate throughput
limitation**: a batch's retained remainder egresses one datagram per syscall on subsequent edges, not
batched. Correctness is unaffected, and the recoverable per-datagram policy is NOT lost on that path:
each batch-enqueued slot carries a **recoverable provenance flag** (`KlDgramSlot.recoverable`, set by
`kl_dgram_send_enqueue`) so a hard error on a retained slot draining through `send_pump` (or completion
`on_complete`) DROPS+reports that one datagram (§4.4) instead of setting the sticky error. A future
increment MAY add persistent batch-flush state to keep the writable-edge drain batched; M5.2a does not.

**Recoverable reporting seam (P1-2).** A recoverable drop from ANY path; `flush_batch` isolation, the
`send_pump` writable-edge drain, or completion `on_complete`: fires the send machine's `on_drop`
callback, which the facade wires to record `kl_datagram_last_error` + a dropped count. So a bad-middle
datagram is reported wherever it is finally submitted, not only during the synchronous batch call.

**Facade reentrancy (P1-1).** `kl_datagram_send_batch` brackets the WHOLE operation with
`kl_dgram_core_dispatch_begin/end`: the drain's final activity release can otherwise run a deferred
teardown that frees `dg`/`core`, after which the facade still reconciles WRITE. The bracket defers that
terminal to `dispatch_end`, the last access to `dg`/`core` (only the local `accepted` is used after).

### 4.3 GSO - a queued FIFO group: tail-append, head-submit, atomic release (B5, C2)

A GSO request is **not** submitted ahead of older queued datagrams; it is a queued FIFO citizen, a
**group** occupying `nseg` contiguous slots at the tail, submitted only when its head segment reaches
the FIFO head.

**Validation / overflow (correction):** reject with `KL_DATAGRAM_ERROR` if `segment_size == 0`, or
`buf == NULL` while `total_len > 0`. Compute `nseg` overflow-safe: reject (`ERROR`) if `total_len >
SIZE_MAX - (segment_size - 1)`; else `nseg = (total_len + segment_size - 1) / segment_size` (0-length ⇒
`nseg == 0` ⇒ treated as a single empty datagram, `nseg = 1`). Reject (`ERROR`) if `nseg` does not fit
the slot-count type. The byte-budget check uses guarded arithmetic (`bytes_used > budget - total_len`
form, never `bytes_used + total_len`).

`kl_datagram_send_gso(dg, b, buf, total_len, seg, peer)`: `b` is a caller-preallocated SEND/BOTH
`KlDatagramBatch` (D3): its storage IS the GSO group buffer, capacity `n_slots × slot_bufsz`, allocated
at `batch_create`. **No allocation on the call.**
1. **Preflight bounds:** `seg ≤ send_slot_cap`; `total_len ≤ n_slots × slot_bufsz` (the batch capacity)
   else `TOO_LARGE`.
2. **Atomic admission** of all `nseg` segments against **both** limits; `nseg` free slots AND (BOTH
   policy) the guarded byte budget. **All-or-nothing:** `nseg > send_slots` or `total_len > byte_budget`
   ⇒ `TOO_LARGE` (permanent); doesn't fit right now (`< nseg` free slots, or `b`'s group buffer is
   already occupied by an outstanding group: see 5) ⇒ `WOULD_BLOCK` (nothing admitted). **No partial
   hidden admission.**
3. **Copy once** (copy-at-submit) the whole `buf` into `b`'s group buffer; append the group at the FIFO
   **tail** occupying `nseg` slots, reserving `nseg` slots + `total_len` bytes.
4. **The group record** (referenced by its `nseg` slots): `total_len`, `seg`, `nseg`, `remaining`
   (segments not yet retired), the next-segment `offset`, and a `mode` latched at first submission
   (`GSO` vs `FALLBACK`). It is submitted **only when its first segment is at the FIFO head** (the
   flush of §4.2 detects a group-head slot and diverts it out of the `sendmmsg` batch). On submission,
   classify the `send_gso` result immediately via `kl_sock_io_status` (D2), the **three frozen
   outcomes**:
   - **`KL_IO_WOULD_BLOCK`**: re-arm WRITE and retain the group at the head (drains on the next
     writable edge). Nothing retired.
   - **`KL_IO_UNSUPPORTED`** (only this): latch `gso_unsupported` (§6.3) and switch **this group** to
     `FALLBACK` mode: retransmit is safe because nothing was sent.
   - **any other hard error**: the **recoverable send-error policy (§4.4)** on the whole group (drop
     the remaining group, `dropped += remaining`, facade `last_error`, continue): **never** a
     fallback retransmit (an arbitrary hard error may have partially transmitted; re-sending would
     violate the send contract).
   On **`DONE`** (GSO mode): retire the **whole** group (all `remaining` slots + bytes) atomically.
   - **`FALLBACK` mode** (GSO absent, or latched, or switched per above): send the **same admitted
     sequence**: one `send` per remaining segment from the group buffer at `offset`, retiring one slot
     + `seg` bytes each and advancing `offset`/`remaining`; per-segment results classify the same way
     (WOULD_BLOCK retains + re-arm; hard error → §4.4 recoverable policy on that segment). Segments
     interleave correctly because they are contiguous head slots.
5. **One outstanding GSO group per batch:** while `b`'s group is queued or draining, a second
   `send_gso` on `b` returns `WOULD_BLOCK` (the group buffer cannot be reused until the group retires).
   Distinct SEND batches carry distinct group buffers (a caller wanting concurrent GSO preallocates
   more batches). M5 freezes one group buffer per batch.
6. **Close / discard** (`kl_dgram_send_discard_queued`, abortive close) releases the **complete
   remaining group reservation** in one step: all `remaining` slots + `total_len`-consumed bytes +
   the group buffer's pinned state: never a half-released group.

Byte accounting = `total_len` reserved at admission, released as the group retires (all at once in GSO
mode, per-segment in fallback). GSO occupies `nseg` logical slots (not one).

### 4.4 The recoverable send-error policy (D1): distinct from the sticky machine error

The core send machine's `s->err` is **fatal-only**: setting it halts all future pumping and turns a
graceful close abortive (`datagram_send.c`, `err` gates `send_pump`; `datagram_close.c` escalates a
send error to abort). A per-datagram hard error on a batch/GSO drain (a bad `sendmmsg`/`send`/`send_gso`
descriptor the extension has already isolated to a single datagram or group) must NOT poison the whole
queue. Frozen, the **recoverable send-error policy**, restoring `KlUdp`'s per-datagram drop semantics:

> Drop exactly the identified datagram(s) (release their slots + bytes), `dropped++`, set the **facade
> `last_error`** (observable via `kl_datagram_last_error`, a diagnostic), **do NOT set `s->err`**, and
> **continue draining** the rest of the queue.

`s->err` remains reserved for a genuinely fatal transport failure (not an isolated per-datagram error).
Both §4.2's isolated-head hard error and §4.3's GSO other-hard-error use this policy.

### 4.5 Queued-GSO SEND-batch lifetime (E1): a SEND batch is NOT transient while a group is in flight

A plain `send_batch` IS transient: its `descs` are copied into core slots at `enqueue`, and `b`'s `tx`
staging is read only during the synchronous flush; nothing references `b` after the call. **GSO is
different:** the queued group keeps reading `b`'s group buffer **asynchronously** across later writable
edges (readiness) or completion sends until the group retires. So `b` is a live object bound to its
datagram for the group's lifetime. Frozen:
- **`b` records** its owning `KlDatagram *`, its `KlDgramBatchDir`, and a `gso_busy` flag.
- **The send APIs validate `b`:** `kl_datagram_send_batch` / `kl_datagram_send_gso` reject (`ERROR`) a
  `b` whose owner ≠ this datagram, or a `b` without SEND in its direction. (`send_gso` additionally
  needs the group buffer, i.e. a SEND/BOTH batch: §4.3.)
- **`gso_busy`** is set when a GSO group from `b` is admitted (queued) and cleared when that group
  fully **retires** OR when the datagram's **close/discard** releases it (§4.3 step 6).
- **`kl_datagram_batch_free(b)` returns `-1` while `gso_busy`** (the group is still reading the buffer);
  it succeeds once the group has retired or after close/discard cleared `gso_busy`.
- **Close / discard** releases the group and clears `gso_busy`, but does **NOT** free a caller-owned
  `b` (a SEND batch is caller-owned, only a *consumed* RECV batch is core-owned, §5.4). The caller
  frees `b` afterward via `kl_datagram_batch_free`.
- **Caller obligation:** keep **both** `b` and its datagram alive until the group retires or
  close/discard completes. Reusing `b` for a new `send_gso`/`send_batch` is permitted once `!gso_busy`.

## 5. RECV: borrowed-view delivery seam, held-buffer pause, core-owned teardown (B3, B4)

### 5.1 Borrowed-view delivery (B3): the owned inbound slot is NOT repointed

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
  buffer (up to the batch slot bufsz, which is sized for GRO, e.g. 64 KiB) is **not** clamped by
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
  Otherwise the whole slot is yielded (plain datagram, last segment, or, with `on_recv_segments` set,
  the whole coalesced buffer with `gro_seg` carried), `i++`, `seg_off = 0`.
- **Deliver:** `on_recv(view)` for a logical datagram; `on_recv_segments(view + segment_size)` for a
  whole coalesced buffer (O-A explicit).

### 5.3 Pause / resume / stop lifetime (held buffer)

`recv_batch` has already consumed datagrams from the kernel, so a pause mid-buffer must not lose them:
- **Pause** (from a callback mid-buffer): `on_readable` breaks; the ext cursor (`slots`, `n_filled`,
  `i`, `seg_off`) is **retained** (a held buffer of ≥1 undelivered logical datagrams).
- **Resume**: drain the retained buffer first (deliver held logical datagrams one at a time,
  re-checking pause/stop: analogous to the completion held-datagram), THEN re-arm READ interest. So
  buffered datagrams are not stranded behind readiness.
- **Stop / close**: discard the retained ext buffer (no delivery).

### 5.4 Ownership + teardown safety (B4, O-F): corrected

Rev 2's "`recv_inflight` prevents reclamation during delivery" is **wrong**: readiness `disarm`
clears `recv_inflight` inside the callback. The frozen model: **full ownership transfer** (C3, the
reviewer's simpler contract):
- **Successful attach CONSUMES `b`.** `kl_datagram_recv_attach_batch(dg, b)` transfers the ENTIRE
  `KlDatagramBatch` (the public wrapper AND its ext/provider blocks, one allocation the core now owns
  via `core->ext`) to the core and returns 0. **The caller must never touch `b` again**, not
  `batch_free`, not any accessor. There is nothing left in `b` for the caller, so there is no "how does
  `b` learn it was reclaimed" question and no dangling-pointer path. Attach is permitted only before
  `recv_start`, on a readiness datagram; it fails (`-1`) on a completion datagram or a re-attach.
- **Failed attach leaves ownership with the caller.** On `-1`, `b` is untouched and still caller-owned;
  the caller frees it with `kl_datagram_batch_free(b)`.
- **The core frees the consumed batch at its destructive tail.** `core_rx_final` /
  `close_detach` free `core->ext` (the whole adopted `KlDatagramBatch`) exactly once, after the `busy`
  handshake defers past any in-progress `on_readable` frame, so the borrowed view + ext buffer stay
  valid for the whole delivery loop; slot N+1 storage is never freed mid-loop.
- **`!stopped` breaks before slot N+1.** A stop/close from delivery N sets `stopped=1`; the loop's
  top-of-iteration `!stopped` check returns before the next yield. (This, plus the deferred tail free,
  NOT `recv_inflight`: is the safety.)
- **`kl_datagram_batch_free(b)` is ONLY for a never-successfully-attached batch**, a **SEND** batch
  (caller-owned; for a plain `send_batch` its `tx` staging is copied into core slots at enqueue and read
  only during the synchronous flush, but a **queued GSO group keeps reading it asynchronously** until it
  retires, so `batch_free` is refused while `gso_busy`: §4.5), or a RECV batch whose attach returned
  `-1`. It is never called on a consumed (successfully-attached RECV) batch.

## 6. Capability model (semantic cleanup)

### 6.1 provider_caps = provider SUPPORT (directional)

`kl_datagram_provider_caps()` reports what the provider/fd SUPPORTS: new bits added to
`keel/datagram.h`, reported by the provider `caps()` op:
```
KL_DGRAM_CAP_RX_BATCH (1u<<5)  /* recv_batch + rx_batch_new/free present + caps() reports it */
KL_DGRAM_CAP_TX_BATCH (1u<<6)  /* send_batch + tx_batch_new/free present + caps() reports it */
KL_DGRAM_CAP_GSO      (1u<<7)  /* send_gso present: advisory (§6.3) */
KL_DGRAM_CAP_GRO      (1u<<8)  /* the provider CAN capture UDP_GRO; advisory support, NOT per-socket */
```
Directional: a `RECV` batch checks `RX_BATCH`; `SEND`/GSO check `TX_BATCH`/`GSO`: one never gates the
other.

### 6.2 Accepted RX mask = separate per-socket state; GRO needs BOTH (semantic cleanup, P1-c)

`provider_caps` must NOT be synthesized from the M0 accepted mask (that is enabled-per-socket state,
not provider support). Frozen:
- Add `unsigned accepted_rx_caps;` to `KlDatagramConfig` (O-E), the `KL_DGRAM_RX_*` mask the socket
  actually has enabled (from `KlDatagramPrep.rx_caps`); `kl_datagram_init_ex` **masks it to known
  `KL_DGRAM_RX_*` bits** and stores it separately on the core (not folded into `caps`). A caller who
  passes 0 gets no RX-derived features (graceful).
- **GRO ACTIVATES iff BOTH** `provider_caps & KL_DGRAM_CAP_GRO` (the provider supports GRO) **AND**
  `accepted_rx_caps & KL_DGRAM_RX_GRO` (this socket enabled it). Otherwise `meta.gro_seg` stays 0 and
  delivery is per-datagram (graceful). `kl_datagram_provider_caps` keeps reporting support; a distinct
  accessor (or the granted-caps `kl_datagram_caps`) reflects the AND for GRO activation.

### 6.3 GSO: advisory cap + core-owned per-fd latch, UNSUPPORTED-only (P1-e; D2)

`KL_DGRAM_CAP_GSO` is advisory (op present); a runtime `int gso_unsupported` on the **core** (per-fd)
latches **only** on a `send_gso` result classified `KL_IO_UNSUPPORTED` (D2): never on an arbitrary
hard error, and thereafter forces the per-segment fallback (§4.3); `provider_caps` does not mutate.
`kl_datagram_gso_active(dg)` exposes the latch.

To make "unsupported" provider-neutral (the freestanding providers have no hosted `errno`), M5.1
appends **`KL_IO_UNSUPPORTED`** to `KlIoStatus` (`keel/socket.h`); the hosted mapping
(`kl_sockdef_io_status`) maps `EOPNOTSUPP`/`ENOTSUP` → it (all other errors stay `KL_IO_FATAL`). This
is an additive enum change (values unchanged, appended last): M5.1 must audit exhaustive `KlIoStatus`
switches to add a `default`/case (the `-Wswitch` surface is small: the completion drain + connect
paths).

### 6.4 Create rules, allocation contract, capability-independence (B6, E2)

`kl_datagram_batch_create(dg, dir, n_slots, slot_bufsz)`:
- **Allocation contract (M5.1):** validate `n_slots > 0` and `slot_bufsz > 0`, and guard
  `n_slots * slot_bufsz` against overflow (`n_slots > SIZE_MAX / slot_bufsz` ⇒ NULL) **before** any
  allocation. The SEND/BOTH group buffer is `n_slots * slot_bufsz` (§4.3); the RECV payload storage is
  the provider `rx_batch` block.
- **Backend rules (E2):** `RECV` requires a readiness datagram (completion ⇒ NULL); `SEND` is allowed
  on **both** readiness and completion; `BOTH` is refused on a completion datagram (create `SEND`).
- **Capability-independence (B6):** the provider `rx_batch`/`tx_batch` block is allocated for a
  direction **only when that direction's cap is present**; when absent the block is NULL and the
  flush/refill takes the **portable fallback** (single `send`/`recv` loop, §4.2/§5.2). Create
  **succeeds regardless of caps**: otherwise the fallback would be unreachable. A `RECV` batch is still
  useful without `RX_BATCH` (GRO-split via a single-`recv` refill).
- **Create fails** only for: `n_slots`/`slot_bufsz` invalid or overflowing; a completion datagram with
  `dir` ∈ {`RECV`, `BOTH`}; or OOM.

## 7. Public surface (`keel/datagram_batch.h`)

```c
typedef struct KlDatagramBatch KlDatagramBatch;
typedef enum { KL_DGRAM_BATCH_RECV=1, KL_DGRAM_BATCH_SEND=2, KL_DGRAM_BATCH_BOTH=3 } KlDgramBatchDir;

/* Create once (bound to `dg`; validates n_slots>0 / slot_bufsz>0 / overflow, §6.4; allocates ext +
 * present-cap provider blocks + the SEND/BOTH group buffer). NULL on invalid/overflowing sizes, OOM,
 * a completion datagram with dir RECV/BOTH (E2), else success regardless of caps (§6.4). */
KlDatagramBatch *kl_datagram_batch_create(KlDatagram *dg, KlDgramBatchDir dir, int n_slots, size_t slot_bufsz);
/* Free a SEND batch (caller-owned), or a RECV batch whose attach returned -1. Returns -1 while a GSO
 * group is in flight (gso_busy, §4.5). NEVER call on a consumed (successfully-attached RECV) batch,
 * the core owns it (§5.4). */
int              kl_datagram_batch_free(KlDatagramBatch *b);

/* SEND: enqueue-only accepted prefix + one batch flush (§4.1). `b` must be a SEND/BOTH batch owned by
 * `dg` (else ERROR). Returns the accepted count; *stop classifies descs[accepted] (ACCEPTED if all n
 * taken). Plain send_batch is transient (nothing references b after return). */
int kl_datagram_send_batch(KlDatagram *dg, KlDatagramBatch *b, const KlDgramTxDesc *descs, int n,
                           KlDatagramSendStatus *stop);
/* GSO: `b` = a caller-preallocated SEND/BOTH batch owned by `dg` (else ERROR); its storage is the group
 * buffer (capacity n_slots*slot_bufsz, D3, no call-time alloc). Validated (seg!=0; buf non-NULL for
 * len>0; overflow-safe nseg) atomic nseg admission as a queued FIFO group, then one send_gso or the
 * same normal sequence (§4.3). ERROR on bad args/ownership/direction; TOO_LARGE past capacity/budget;
 * WOULD_BLOCK if b's group is already outstanding. The group reads b ASYNCHRONOUSLY until it retires:
 * keep b AND dg alive until then; b is gso_busy (batch_free refused) meanwhile (§4.5). */
KlDatagramSendStatus kl_datagram_send_gso(KlDatagram *dg, KlDatagramBatch *b, const void *buf,
                                          size_t total_len, uint16_t segment_size, const KlSockAddr *peer);
int  kl_datagram_gso_active(const KlDatagram *dg);

/* RECV: attach BEFORE recv_start on a readiness datagram (`b` a RECV/BOTH batch owned by `dg`). On
 * success (0) the core CONSUMES b, the caller must never touch b again (§5.4, C3). On -1 (completion
 * datagram, wrong owner/direction, re-attach), b is untouched and caller-owned. The machine then
 * delivers via the borrowed view (§5.1), GRO split by default (whole via on_recv_segments). */
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

## 8. `KlUdpServer` migration

No API/ABI change. `kl_udp_server_init` threads `KlDatagramPrep.rx_caps` into `cfg.accepted_rx_caps`;
on a **readiness** server datagram, if `mmsg_batch>1` / `recv_gro` and the provider reports
`RX_BATCH`/`GRO`, it `batch_create(RECV)` + `recv_attach_batch` before `recv_start`. Handler unchanged
(one logical datagram at a time, GRO split). Reply stays single-send (O-B). The consumed RECV batch is
core-owned → reclaimed at the server datagram's teardown. **On a completion-backend server**
(`batch_create(RECV)` ⇒ NULL, readiness-only per E2) OR absent caps ⇒ M4 single-flight, no regression.

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
- **Prefix-only retire + isolation (§4.2, C1):** short `sendmmsg` (sent<k) retains + re-arms; a
  `send_batch` `-1` with `KL_IO_WOULD_BLOCK` retains the **whole** prefix + re-arms (asserted via a
  gating provider that returns `-1`/EAGAIN: no drop); a `-1` hard error single-sends the head (a
  permanently-bad head is dropped + counted; a good head is retired): no middle slot inferred. Fast
  path AND `NULL send_batch` fallback deliver identical bytes/order.
- **GSO group (§4.3, C2):** `nseg` atomic admission (no partial), TOO_LARGE when `nseg>send_slots` /
  `total>budget`, WOULD_BLOCK when it can't fit now; a GSO group behind older queued datagrams submits
  only when its head segment reaches the FIFO head (assert ordering); a 2nd concurrent `send_gso` ⇒
  WOULD_BLOCK; GSO path one syscall vs fallback `nseg` sends deliver the same segments; close/discard
  releases the whole remaining group; `total`-byte accounting released correctly; GSO latch → fallback.
- **GSO validation:** `seg==0` / `buf==NULL && len>0` ⇒ ERROR; overflow-safe `nseg` (huge `total_len`
  ⇒ ERROR, not a wrapped small `nseg`); byte-budget arithmetic guarded.
- **Queued-GSO batch lifetime (§4.5, E1):** `batch_free` while a GSO group is in flight ⇒ `-1`
  (gso_busy); succeeds after the group retires or after close/discard; a `send_gso`/`send_batch` with a
  `b` owned by another datagram ⇒ ERROR; a `send_gso` on a RECV-only batch ⇒ ERROR; a second `send_gso`
  reusing `b` after the first group retired succeeds; close/discard with a group in flight clears
  gso_busy + releases the group but leaves `b` freeable, no leak/UAF (ASan/UBSan/LSan).
- **Completion creation/fallback (§6.4, E2):** `batch_create(RECV)`/`(BOTH)` on a completion datagram ⇒
  NULL; `batch_create(SEND)` on a completion datagram succeeds; completion `send_batch` delivers via the
  single-flight pump (one op at a time); completion `send_gso` delivers the per-segment fallback
  sequence; the group buffer lifetime (E1) holds across the async completion sends.
- **Allocation contract (§6.4):** `n_slots==0` / `slot_bufsz==0` ⇒ NULL; `n_slots * slot_bufsz`
  overflow ⇒ NULL (no allocation).
- **Borrowed view + GRO (§5.1/§5.2):** one recvmmsg → N serial `on_recv` (view data into ext storage,
  not the owned slot; GRO buffer > `recv_cap` delivered un-clamped); GRO split per-segment by default,
  whole to `on_recv_segments`; GRO active only with provider GRO support AND `accepted_rx_caps` GRO.
- **Pause/resume held buffer (§5.3):** pause mid-buffer retains undelivered datagrams; resume drains
  them before re-arm; stop discards them: none lost, none double-delivered.
- **Teardown ownership (§5.4, C3):** a successful attach consumes `b` (a subsequent `batch_free(b)` is
  a caller bug, not exercised); a **failed** attach leaves `b` caller-owned and `batch_free(b)` frees it
  once; stop/close from delivery N → N+1 never delivered; the consumed batch is freed once at the
  destructive tail (deferred past the callback); UAF/leak-free under ASan/UBSan/LSan.
- **Caps (§6):** `provider_caps` reports support (directional), not the accepted mask; `batch_create`
  succeeds with a cap absent and the fallback runs; RX-only / TX-only creation.
- **`KlUdpServer` (§8) + regression:** burst → handler one-at-a-time; caps absent → still delivered;
  DNS single-flight + existing suites unchanged; ext pointer NULL / zero alloc when no batch attached.

## 12. Increment breakdown (each its own freeze-then-code review)

1. **M5.1**, the four `provider_caps` bits + `KlDatagramConfig.accepted_rx_caps` (masked) stored
   separately + GRO = provider-support AND accepted; **`KL_IO_UNSUPPORTED`** appended to `KlIoStatus` +
   the hosted `EOPNOTSUPP`→it mapping + the `-Wswitch` audit (§6.3); `KlDgramBatchExt` +
   `KlDatagramBatch` create/free (validate `n_slots>0`/`slot_bufsz>0`/overflow; create-never-fails-on-
   caps; SEND allowed on completion, RECV/BOTH readiness-only, E2; `b` records owner/dir/`gso_busy`, E1;
   the SEND/BOTH batch preallocates the GSO group buffer, D3); the core `ext`/`gso_unsupported`/
   `accepted_rx_caps` fields. No wiring.
2. **M5.2: send.** `kl_dgram_send_enqueue` + `kl_dgram_send_flush_batch` (prefix retire + `-1`
   `io_status` classification + single-send isolation, C1) + the **recoverable send-error policy**
   (§4.4, D1) + `submit_batch` + portable fallback; `kl_datagram_send_batch` (transaction + `*stop`;
   readiness batch-drain, completion single-flight pump: E2); `kl_datagram_send_gso(dg, b, …)`
   (validated atomic nseg admission + the queued FIFO group + `gso_busy` lifetime + owner/direction
   checks, E1 + three-way `io_status` classification + UNSUPPORTED-only latch + fallback, C2/D2/D3).
3. **M5.3: recv.** the borrowed-view seam + yield adapter + GRO split + held-buffer pause/resume +
   `recv_attach_batch` (core adoption) + `recv_segments`; teardown-from-delivery-N + destructive-tail
   reclamation tests.
4. **M5.4: `KlUdpServer` opt-in. IMPLEMENTED.** `kl_udp_server_init` threads `prep.rx_caps` into
   `KlDatagramConfig.accepted_rx_caps`, enables GRO capture on a readiness loop (`uc.recv_gro`), and,
   when `mmsg_batch>1`/`recv_gro` and the provider reports `RX_BATCH`/`GRO`: creates a RECV
   `KlDatagramBatch` (`n_slots` = the normalized `mmsg_batch`, `slot_bufsz = max(recv_cap, 65535)` under
   GRO) and attaches it before `recv_start`. `mmsg_batch` is normalized exactly as `KlUdp`: `0` → default
   16, capped to `[1, 64]`, `1` disabling batching.
   - **Batching alone is a throughput optimization**: `batch_create` NULL on a completion datagram (E2)
     OR a create/attach failure falls back to the M4 single-flight recv with no behavioral change.
   - **Accepted GRO makes a successfully-SPLITTING attachment MANDATORY (correctness, not fallback).**
     Keyed to the ACCEPTED RX bit (`prep.rx_caps & KL_DGRAM_RX_GRO`), not merely provider support: once
     GRO capture is on the socket, the single-flight receiver cannot split a coalesced buffer, so multiple
     logical datagrams would reach the handler as one. So if GRO is enabled but active splitting is not
     achieved, the batch failed to create/attach, OR it attached but the §6.2 two-part gate is not
     satisfied (`provider_caps & KL_DGRAM_CAP_GRO` missing → the attached batch does NOT split): init
     TEARS DOWN (closes the fd) and FAILS with `KL_ERR_UNSUPPORTED`. It does NOT silently fall back.
   The handler still sees one logical datagram per call (the batch splits GRO buffers before dispatch).
   Reply stays single-send (O-B). The consumed RECV batch is core-owned → reclaimed at the server
   datagram's teardown. Tests: `m54_mmsg_batch_parity`; `m54_gro_splits_to_handler` (a GRO-fabricating
   mock → one coalesced datagram split into 3 per-segment handler calls); `m54_gro_batch_failure_fails_
   init` (rx_batch_new NULL → init -1); `m54_gro_accepted_but_no_provider_cap_fails` (RX_GRO accepted but
   CAP_GRO absent → attached-but-inactive split → init -1); `m54_mmsg_normalization` (0→16 / 1→no batch /
   100→64 / 32→32). Validated macOS + pollcomp + container epoll (attached batch, real recvmmsg + the two
   GRO-mandatory-failure regressions) + io_uring (single-flight) under ASan/UBSan/LSan; MinGW clean.
5. **(post-M5)** `KlUdp` decision (§9) + mode B: separate freezes.

**Order:** M5.1 → {M5.2, M5.3} → M5.4. Independent of any `KlUdp` change. **M5 (M5.1–M5.4) COMPLETE.**

## 13. Findings resolution map

| # | blocker | resolution |
|---|---------------|------------------|
| B1 | normal admission defeats batching | §4.1 enqueue-only transaction (no fast path, no per-datagram submit) + one flush; prefix acceptance + `*stop` semantics for WOULD_BLOCK/TOO_LARGE/hard error |
| B2 | can't drop an arbitrary failed slot | §4.2 retire only the reported sent prefix; short/zero = transient (retain+re-arm); `-1` = single-send isolation of the head (never infer) |
| B3 | must not repoint the canonical inbound slot | §5.1 borrowed-view seam ({data,len,meta} into ext storage); owned slot untouched; truncation from provider meta, not `recv_cap` clamp |
| B4 | teardown ownership unsafe | §5.4 core adopts the attached ext; destructive-tail reclamation (not `recv_inflight`); `batch_free` only unattached/CLOSED; `!stopped` breaks before N+1 |
| B5 | GSO queue semantics undefined | §4.3 atomic `nseg` admission vs slot+byte (no partial), copy-once, GSO-or-normal-sequence, atomic/per-segment retire, `total` byte accounting |
| B6 | fallback contradicts creation | §6.4 create never fails on cap absence; provider block optional (NULL ⇒ fallback) |
| - | provider_caps vs accepted RX mask | §6.1/§6.2 provider_caps = support (directional); `accepted_rx_caps` separate per-socket; GRO needs BOTH |
| C1 | `send_batch` `-1` = EAGAIN or hard | §4.2 classify via `kl_sock_io_status` immediately after; WOULD_BLOCK retains the whole prefix + re-arms; only a hard error isolates the head |
| C2 | GSO not integrated into FIFO | §4.3 queued group: tail-append, head-submit, `{nseg,remaining,offset,mode}` record, one pinned group buffer (2nd GSO ⇒ WOULD_BLOCK), whole-group release on close/discard |
| C3 | core adoption vs public handle ambiguous | §5.4/§7 full transfer: successful attach CONSUMES `b` (caller never touches it again); failed attach leaves it caller-owned; `batch_free` only for a never-attached batch |
| - | GSO validation/overflow + ABI wording | §4.3 seg≠0 / non-NULL buf / overflow-safe `nseg` / guarded budget; intro: docs commit = no ABI, M5.1 = intentional ABI break |
| D1 | batch hard-error vs sticky `s->err` | §4.4 recoverable send-error policy (drop + `dropped++` + facade `last_error`, NOT `s->err`, continue); `s->err` stays fatal-only |
| D2 | GSO error class contradicts §6.3 | §4.3/§6.3 three `io_status` outcomes: WOULD_BLOCK retain; UNSUPPORTED-only latch+fallback; other hard error → §4.4 (no fallback retransmit); `KL_IO_UNSUPPORTED` added |
| D3 | GSO hot-alloc + no capacity source | §4.3/§7 `send_gso(dg, b, …)` uses the caller-preallocated SEND/BOTH batch buffer (cap `n_slots×slot_bufsz`); no call-time alloc |
| E1 | queued GSO batch referenced after return | §4.5 `b` records owner/dir/`gso_busy`; send APIs reject wrong owner/direction; `batch_free` `-1` while busy; close/discard clears busy without freeing `b`; caller keeps `b`+dg alive |
| E2 | completion creation/fallback unreachable | §3/§6.4/§7 `SEND` create allowed on completion; `RECV`/`BOTH` refused; completion `send_batch` = single-flight pump, `send_gso` = per-segment fallback |
| - | M5.1 allocation contract | §6.4 validate `n_slots>0` / `slot_bufsz>0` / guard `n_slots*slot_bufsz` overflow before allocating |

## 14. Open decisions for the reviewer

None outstanding: O-A..O-F are ruled. Rev 5 requests acceptance-for-implementation of the frozen
contracts in §4–§6, after which M5.1 proceeds.
