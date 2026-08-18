# Datagram M5 — Batch / GSO / GRO high-throughput extension (design freeze)

**Status:** DESIGN FREEZE (docs-only). No production code, no ABI change, no consumer migration in
this document — it scopes a separately-reviewable increment (or short increment chain) that lands
afterward, each with its own freeze-then-code review.

**Supersedes** the original M5 framing in
[`datagram_consolidation_design.md`](datagram_consolidation_design.md) §6.5 ("reduce KlUdp to a
compatibility wrapper"). Per reviewer direction (2026-08-19), M5 is re-scoped: **first design the
batch/GSO/GRO extension for `KlDatagram` and migrate `KlUdpServer` onto it; only then decide whether
the remaining `KlUdp` compatibility value justifies keeping it.** The `KlUdp` reduction is deferred to
a later, separately-decided step (provisionally "M6"); `KlUdp` is UNTOUCHED by M5.

## 0. The mandate (reviewer, verbatim intent)

The four high-throughput datagram features materially improve packets-per-second and CPU efficiency on
Linux — they are **not** compatibility baggage:

- **recvmmsg receive batching** — read many datagrams in one syscall.
- **sendmmsg send batching** — transmit several queued datagrams in one syscall.
- **UDP GRO** — the kernel coalesces multiple received UDP segments into one buffer; software splits.
- **UDP GSO** — software submits a large buffer + segment size; the kernel/NIC emits many datagrams.

They belong in `KlDatagram` (as an optional layer), not pushed up into `KlUdpServer` directly. The
byte-only send queue is the *only* genuine legacy-compatibility item; it is mathematically incompatible
with the allocation-free fixed-preallocation core and stays out of the core (§7).

Target layering:

```
KlUdpServer
    ↓ (opts in when mmsg_batch / recv_gro configured)
KlDatagram facade                       ← single-datagram contract UNCHANGED
    ↓
optional batch / GSO / GRO extension    ← NEW (this freeze); caller-preallocated, capability-gated
    ↓
KlDatagramOps provider                  ← already has send_gso / recv_batch / send_batch / GRO
```

## 1. Ground truth (what already exists — verify before coding)

1. **The provider vtable already has the machinery.** `KlDatagramOps` (`include/keel/socket_dgram.h`)
   already declares, and `socket_posix.c` / `socket_winsock.c` already implement (POSIX = real
   recvmmsg/sendmmsg/GSO on Linux, per-datagram fallback elsewhere):
   - `send_gso(ctx, fd, data, len, seg, dest)` — one-syscall UDP GSO (NULL / `-1`+EOPNOTSUPP ⇒ caller
     falls back to per-segment `send`).
   - `rx_batch_new / tx_batch_new / rx_batch_free / tx_batch_free` — provider-allocated, caller-owned,
     opaque batch blocks (all NULL on a provider without mmsg).
   - `recv_batch(ctx, fd, rx_batch, KlDgramRxSlot *slots, int max)` — one recvmmsg → fills up to `max`
     caller-provided slots, each `.data` pointing into the batch's own payload storage.
   - `send_batch(ctx, fd, tx_batch, const KlDgramTxDesc *descs, int n)` — one sendmmsg from `n`
     caller-provided descriptors.
   - `configure(...)` returns the accepted `KL_DGRAM_RX_*` capture mask incl. `KL_DGRAM_RX_GRO`; the
     per-datagram `KlDgramRxMeta.gro_seg` (GRO coalesced segment size) and `.tos` are already carried.
   - `caps(ctx, fd)` (M2) — the per-fd capability report the extension gates on.
   The **public, caller-oriented data types already exist**: `KlDgramRxSlot`, `KlDgramTxDesc`,
   `KlDgramRxMeta` (all in `keel/socket_dgram.h`). No new provider ABI is required for the data plane.

2. **The public facade exposes none of it.** `kl_datagram_*` (`keel/datagram.h`) is single-message only
   (`kl_datagram_send` / `kl_datagram_recv_start` + one-datagram-per-callback) plus M2 multicast/caps.
   Batch/GSO/GRO are reachable today ONLY through `KlUdp`'s bespoke `KlUdpTransport` carve, which calls
   the provider ops directly (`udp.c`). **The gap is a facade/core model that surfaces the provider
   machinery without contaminating the minimal single-datagram Tier-1 contract.**

3. **`KlUdpServer` already rides the core but with the fast paths OFF.** M4 migrated `KlUdpServer` onto
   an embedded public `KlDatagram dg` (`udp_server.h:72`). Its `KlUdpServerConfig.mmsg_batch` /
   `.recv_gro` knobs are currently **inert** (forced off in the M4 mapping) — so relative to the old
   `KlUdp`-based server it has **regressed** on Linux pps. This extension is what restores them.

4. **`KlUdp` still carries the parallel implementation.** `KlUdp` keeps its `KlUdpTransport` carve
   (byte-budget send queue + its own batch/GSO/GRO calls) and is the ONLY consumer of the four fast
   paths today. M5 does not touch `KlUdp`. Its reduction (or retirement) is re-decided after M5 lands
   (§7, §9).

## 2. Design decisions

**D-M5-1 — the extension is a distinct layer, never folded into the Tier-1 core.** The single-datagram
contract (`kl_datagram_send` atomic accept/refuse; `kl_datagram_recv_start` one-datagram-per-callback;
allocation-free single-flight; the §4 close/quarantine model) is **unchanged**. Batch/GSO/GRO are
additive APIs in a new TU (`src/datagram_batch.c`) + public header (`keel/datagram_batch.h`), layered
ABOVE the core. The core's `KlDgramRecv`/`KlDgramSend` machines are not modified; the extension plugs
into the core's existing internal readiness-pull seam (the same seam `udp.c` uses) — it does not add a
second event driver. A build/consumer that never includes `keel/datagram_batch.h` is byte-for-byte
unaffected.

**D-M5-2 — caller-preallocated storage; the no-hot-allocation invariant holds.** Every batch/GSO API
requires caller-owned storage; nothing allocates on the data path:
- A `KlDatagramBatch` context (opaque, `keel/datagram_batch_detail.h` for embedding) is created ONCE
  (`kl_datagram_batch_create`), wrapping the provider `rx_batch`/`tx_batch` blocks (the only allocation,
  at create time). Freed once (`kl_datagram_batch_free`).
- The `KlDgramRxSlot[]` (recv) / `KlDgramTxDesc[]` (send) arrays are **caller-provided** (stack or
  caller-owned), passed by pointer+count to each call. The batch's payload storage (rx) is inside the
  provider `rx_batch` block; slots point into it, valid until the next `recv_batch` on that batch.
- The callback-machine batch-fill mode (D-M5-5) owns ONE internal `KlDatagramBatch` + slot scratch,
  allocated at attach time, reused for the datagram's lifetime — no per-edge allocation.

**D-M5-3 — capability-gated; absence is reported, never silently emulated.** Three new per-fd caps
(reported by the provider `caps()` op, M2 — family/platform aware), added to `keel/datagram.h`:
```
KL_DGRAM_CAP_BATCH  (1u << 5)   /* recvmmsg / sendmmsg batching (provider mmsg ops present) */
KL_DGRAM_CAP_GSO    (1u << 6)   /* one-syscall UDP GSO (send_gso present + kernel-accepted) */
KL_DGRAM_CAP_GRO    (1u << 7)   /* UDP_GRO receive coalescing (configure accepted KL_DGRAM_RX_GRO) */
```
`kl_datagram_provider_caps()` reports what the fd supports; `kl_datagram_caps()` reports what was
granted (⊆ `want_caps`). The extension never pretends a capability exists (master-plan §7 non-goal). See
D-M5-4 for the one nuance — a *portable correctness* fallback is not capability emulation.

**D-M5-4 — fast-path vs correctness fallback, made observable.** For the SEND primitives, a missing
provider fast path degrades to a portable loop that produces identical observable results (the
datagrams still go out, one syscall each) — this is a throughput fallback, not a fabricated capability:
- `kl_datagram_send_batch` without `KL_DGRAM_CAP_BATCH` → a loop of single sends (the caller's descs).
- `kl_datagram_send_gso` without `KL_DGRAM_CAP_GSO` → a per-segment send loop (matches `KlUdp` today).
The caller learns which path is active via `kl_datagram_provider_caps()` (throughput-sensitive callers
branch on it). For RECEIVE, `kl_datagram_recv_batch` REQUIRES `KL_DGRAM_CAP_BATCH` (its only "fallback"
is the single `kl_datagram_recv_start` the caller already has — no meaningful multi-fill emulation);
without the cap it returns `KL_ERR_UNSUPPORTED`. GRO is a `configure`-time capture: when
`KL_DGRAM_CAP_GRO` is off, `meta.gro_seg` is simply 0 and delivery is naturally per-datagram (graceful
degradation, no error). This mirrors `KlUdp` and keeps all callers correct on every platform.

**D-M5-5 — receive integration: single-datagram dispatch preserved; the adapter splits.** Two
mutually-exclusive receive modes per datagram, both above the unchanged single-flight core:
- **(A) Callback + batch-fill (the `KlUdpServer` mode — PRIMARY).** A `KlDatagramBatch` is attached to a
  callback-recv datagram before `kl_datagram_recv_start`. On a readiness edge the extension issues ONE
  `recv_batch` into the batch, then delivers each filled slot to `on_recv` **serially, one logical
  datagram per call** (re-checking liveness via the life-token between deliveries — the existing
  `udp.c` pattern). A GRO-coalesced slot (`meta.gro_seg > 0 && len > gro_seg`) is **split per-segment**
  into multiple single-datagram `on_recv` calls (the "adapter splits before dispatch"), OR delivered
  whole to an optional `KlDatagramRecvSegmentsFn` when the consumer registered one (advanced; matches
  `kl_udp_recv_segments`). The core machine's per-datagram delivery contract is untouched — only the
  underlying syscall is batched.
- **(B) Explicit data-oriented poll (advanced).** `kl_datagram_recv_batch(dg, batch, slots, max)` fills
  a caller array in one recvmmsg for a consumer driving its own readiness loop. Mutually exclusive with
  callback-recv on the same datagram (one owner of fd readiness). `KlUdpServer` does NOT use mode B.

  *Scope:* modes A and B are **readiness-path** (POSIX/Linux recvmmsg) — matching `KlUdp` today. On a
  COMPLETION backend (io_uring/IOCP) the datagram uses the single-flight core recv (correct, unbatched);
  native completion multishot/batched receive is explicitly a **later** step, not M5 (§7 open item O-1).

**D-M5-6 — send integration respects the core's send-queue + backpressure.** The explicit
`kl_datagram_send_batch` / `kl_datagram_send_gso` primitives sit on the core send path: each datagram is
subject to the same atomic accept/refuse + the configured backpressure (`send_slots` / BOTH byte
budget). A batch send admits datagrams up to available capacity and reports how many were accepted
(partial-accept is explicit — the caller retains the remainder), preserving the "no ownership on
refusal" invariant. GSO submits one logical buffer (bounded by `send_slot_cap`). No new backpressure
model is introduced; the byte-only queue is NOT resurrected here (§7).

**D-M5-7 — `KlUdpServer` opts in; the handler stays single-datagram.** When
`KlUdpServerConfig.mmsg_batch > 1` and/or `.recv_gro` is set and the provider reports the matching caps,
`kl_udp_server_init` creates a `KlDatagramBatch` and uses receive mode (A): the recvmmsg batch fills,
GRO buffers split, and each logical datagram is dispatched to the unchanged `KlUdpHandlerFn` one at a
time. If the caps are absent the knobs degrade to the single-flight core (today's behavior) — a
`kl_udp_server_*` query/last_error can surface that the fast path was not activated (D-M5-3). The reply
path (`kl_udp_server_reply`) stays on the core single send by default; batching replies (accumulate →
`send_batch`) is an OPTIONAL follow-on, low-value because replies are demand-driven one-per-recv (§9).

**D-M5-8 — DNS and portable consumers are untouched.** The `dns_resolver` (M3) and any portable
protocol consumer stay on the simple single-flight `kl_datagram_*` path. The extension is strictly
opt-in; not including `keel/datagram_batch.h` costs nothing.

## 3. Proposed public surface (`keel/datagram_batch.h` — for review)

```c
/* Opaque, caller-created batch context. Wraps the provider rx_batch/tx_batch blocks (allocated once).
 * Bound to one KlDatagram's provider + allocator. */
typedef struct KlDatagramBatch KlDatagramBatch;

/* Create a batch sized for `n_slots` datagrams of up to `slot_bufsz` payload each. Allocates the
 * provider rx/tx blocks ONCE (per `dir`). Returns NULL if the provider lacks the requested fast path
 * (check kl_datagram_provider_caps first) or on allocation failure. */
typedef enum { KL_DGRAM_BATCH_RECV = 1, KL_DGRAM_BATCH_SEND = 2, KL_DGRAM_BATCH_BOTH = 3 } KlDgramBatchDir;
KlDatagramBatch *kl_datagram_batch_create(KlDatagram *dg, KlDgramBatchDir dir,
                                          int n_slots, size_t slot_bufsz);
void             kl_datagram_batch_free(KlDatagramBatch *b);

/* (B) explicit data-oriented receive: one recvmmsg → fills up to `max` caller slots. Returns count
 * (>=0), 0 = drained (EAGAIN), -1 (kl_datagram_last_error; KL_ERR_UNSUPPORTED without CAP_BATCH). */
int kl_datagram_recv_batch(KlDatagram *dg, KlDatagramBatch *b, KlDgramRxSlot *slots, int max);

/* explicit send: one sendmmsg (or a portable loop without CAP_BATCH) from `n` caller descs, subject to
 * core backpressure. Returns datagrams accepted (>=0; may be < n on backpressure — caller keeps the
 * rest), -1 on error. */
int kl_datagram_send_batch(KlDatagram *dg, KlDatagramBatch *b, const KlDgramTxDesc *descs, int n);

/* one-syscall UDP GSO (or a portable per-segment loop without CAP_GSO), subject to core backpressure.
 * Returns KlDatagramSendStatus (ACCEPTED / WOULD_BLOCK / TOO_LARGE / UNSUPPORTED / CLOSED / ERROR). */
KlDatagramSendStatus kl_datagram_send_gso(KlDatagram *dg, const void *buf, size_t total_len,
                                          uint16_t segment_size, const KlSockAddr *peer);

/* (A) callback-machine batch-fill: attach a RECV batch to a callback-recv datagram BEFORE recv_start.
 * The machine's readiness pull draws from the batch; each slot is delivered to on_recv serially; GRO
 * buffers are split per-segment (or delivered whole to on_recv_segments if registered). */
int  kl_datagram_recv_attach_batch(KlDatagram *dg, KlDatagramBatch *b);
typedef void (*KlDatagramRecvSegmentsFn)(void *ud, const void *data, size_t len, size_t segment_size,
                                         const KlSockAddr *peer, const KlSockAddr *local, unsigned flags);
void kl_datagram_recv_segments(KlDatagram *dg, KlDatagramRecvSegmentsFn cb, void *ud);
```

New caps in `keel/datagram.h`: `KL_DGRAM_CAP_BATCH` / `_GSO` / `_GRO` (D-M5-3). No change to
`KlDatagramConfig` (frozen); the batch is attached post-init. `keel/socket_dgram.h` is unchanged (the
provider ops + data types already exist). `KlDgramRxSlot.data` lifetime (valid until the next
`recv_batch` on that batch) is already documented in `socket_dgram.h`.

## 4. Capability derivation (extension ⇄ provider)

The extension's caps are derived from the provider, not re-detected:
- `KL_DGRAM_CAP_BATCH` ⟸ the provider's mmsg ops (`recv_batch`/`send_batch` + the batch-block
  allocators) are non-NULL AND the provider's `caps()` reports it for the fd. (POSIX reports it on
  Linux where recvmmsg/sendmmsg exist; per-datagram fallback providers report absence.)
- `KL_DGRAM_CAP_GSO` ⟸ `send_gso` non-NULL AND `caps()` reports it (Linux UDP_SEGMENT; probed/kernel-
  accepted — the provider owns the once-only "GSO unsupported → disable" latch, as `udp.c` does today).
- `KL_DGRAM_CAP_GRO` ⟸ `configure()` returned `KL_DGRAM_RX_GRO` in its accepted mask (the datagram
  already stores the accepted RX mask via the M0 `KlDatagramPrep.rx_caps` handoff — reuse it, do not
  re-detect).

No silent budget/capability reinterpretation (master-plan §7). A provider that lacks a fast path
reports its absence; the send primitives take the portable fallback (D-M5-4), recv-batch refuses.

## 5. `KlUdpServer` migration shape

- No public API/ABI change: the 8 functions + 17-field config + `KlUdpHandlerFn` are preserved
  exactly. `mmsg_batch` / `recv_gro` stop being inert.
- `kl_udp_server_init`: after the M4 `KlDatagram` init, if `mmsg_batch > 1` and/or `recv_gro` and the
  provider reports `KL_DGRAM_CAP_BATCH` / `_GRO`, create a `KL_DGRAM_BATCH_RECV` `KlDatagramBatch`
  (sized `mmsg_batch` × `recv_buf_size`) and `kl_datagram_recv_attach_batch` before `recv_start`. The
  handler dispatch is unchanged: each logical datagram (batch slot, or a GRO segment after the split)
  arrives at `KlUdpHandlerFn` one at a time with its `src` (+ `local` for source-pinned reply).
- Source-pinned reply, multicast, TOS default, broadcast, `so_rcvbuf/sndbuf` remain the M4 passthrough.
  The batch context is torn down in `kl_udp_server_free` before the `KlDatagram` teardown (ownership:
  server-owned, freed exactly once).
- Off-ramp: if the platform/provider lacks the caps, the server runs on the single-flight core exactly
  as it does after M4 (correct, unbatched) — no behavioral regression, just no speedup.

## 6. The byte-only send queue — explicitly OUT of scope, options recorded

Per the mandate and master-plan §7, the byte-only / count-unbounded send queue is NOT moved into the
core (it cannot be preallocation-bounded — a zero-length datagram means a byte budget can't bound
message count). M5 does not touch it. Its long-term disposition is a SEPARATE later decision, options:
1. **Keep it only in a deprecated `KlUdp` compatibility implementation** (status quo; `KlUdp` retained).
2. **Replace it with explicit `send_slots` + `send_byte_budget` semantics** in a future breaking
   release (the BOTH policy already exists in the core — M1 — so `KlUdp` users would move to it).
3. **Offer an optional allocator-backed queue adapter above `KlDatagram`**, clearly outside the
   allocation-free Tier-1 contract, for consumers that truly need byte-only/unbounded backpressure.
This freeze commits to none; it records them so the post-M5 `KlUdp` decision (§9) is unblocked.

## 7. Non-goals of this freeze

- No production code, no consumer migration, no ABI change here.
- Do NOT modify the single-datagram Tier-1 core, its close/quarantine model, or its allocation-free
  guarantee. The extension is a strictly-additive layer.
- Do NOT move the byte-only queue into the core (§6).
- **O-1 (deferred):** native COMPLETION-backend batched receive (io_uring multishot / IOCP batched) is
  out of scope — M5 batch/GRO is readiness-path (matching `KlUdp`); completion backends use the
  single-flight core recv. Revisit after M5 if profiling justifies it.
- Do NOT reduce or retire `KlUdp` here (that is the post-M5 decision, §9).

## 8. Test matrix (for the implementation increments)

- **Provider parity:** `recv_batch` / `send_batch` / `send_gso` over the POSIX provider produce the same
  bytes as the single-datagram path (loopback), Linux fast path AND the per-datagram fallback (force via
  a provider whose mmsg/gso ops are NULL) — identical delivery, identical ordering.
- **Preallocation / no hot alloc:** ASan/LSan over a batch-recv + batch-send loop shows zero allocation
  after `kl_datagram_batch_create` (all storage from the create-time blocks + caller arrays).
- **Capability gating:** `provider_caps` reports BATCH/GSO/GRO honestly per fd/platform; `recv_batch`
  without CAP_BATCH → `KL_ERR_UNSUPPORTED`; `send_batch`/`send_gso` without the cap take the portable
  loop and still deliver (assert bytes + that `provider_caps` shows the cap absent).
- **GRO split (D-M5-5):** a coalesced buffer (`gro_seg>0`, `len>gro_seg`) is split into N single-datagram
  `on_recv` calls (assert N + per-segment bytes + boundary), OR delivered whole to `on_recv_segments`
  when registered. Liveness re-checked between segment deliveries (teardown-from-callback safe).
- **Send backpressure (D-M5-6):** `send_batch` partial-accept under a full `send_slots`/BOTH budget
  reports the accepted count; the caller-retained remainder is not sent (no ownership on refusal).
- **`KlUdpServer` opt-in (D-M5-7):** with `mmsg_batch`/`recv_gro` set on a capable provider, N datagrams
  sent in a burst reach the handler one at a time (assert count + `src`), and a GRO burst is split; with
  the caps absent the server still delivers every datagram (fallback), source-pinned reply unchanged.
- **Close/teardown:** batch context freed exactly once on `KlUdpServer`/datagram teardown; close with a
  recv batch attached reaches DETACHED (no leak, no UAF); ASan/UBSan/LSan clean.
- **Regression:** DNS (single-flight, no batch) + the existing datagram/udp_server suites unchanged.

## 9. Increment breakdown (each its own freeze-then-code review; none authorized here)

1. **M5.1 — caps + extension scaffolding.** Add `KL_DGRAM_CAP_BATCH/_GSO/_GRO` + provider `caps()`
   reporting; the `KlDatagramBatch` create/free over the provider blocks; capability derivation from the
   stored `rx_caps`. No consumer change.
2. **M5.2 — send extension.** `kl_datagram_send_batch` + `kl_datagram_send_gso` on the core send path
   (fast path + portable fallback; backpressure + partial-accept). Explicit primitives only.
3. **M5.3 — receive extension.** `kl_datagram_recv_batch` (mode B) + `kl_datagram_recv_attach_batch`
   (mode A, the callback-machine batch-fill) + `kl_datagram_recv_segments` + the GRO split adapter.
4. **M5.4 — `KlUdpServer` opt-in.** Wire `mmsg_batch`/`recv_gro` to mode A; handler unchanged; full
   parity + every-backend conformance; the batch teardown ordering.
5. **(post-M5) `KlUdp` decision.** With the fast paths available on `KlDatagram` and `KlUdpServer`
   migrated, re-decide whether `KlUdp` (and its byte-only queue, §6) is retained as a compat layer,
   reduced onto the core substrate, or scheduled for a versioned removal. Separate freeze.

**Order/dependencies:** M5.1 → {M5.2, M5.3} → M5.4. Independent of any `KlUdp` change. Whether the full
extension is worth landing is re-decided after M5.1 + one of M5.2/M5.3 prove the layering clean (the
master-plan §6 "stop if the layer proves ugly" escape hatch still applies).

## 10. Open decisions for the reviewer

- **O-A (GRO default delivery).** D-M5-5 makes per-segment split the default (single-datagram dispatch;
  `KlUdpServer` needs it) with an optional whole-buffer `on_recv_segments`. Confirm the split-by-default
  vs. segments-callback-by-default choice.
- **O-B (`KlUdpServer` reply batching).** D-M5-7 keeps the reply on the core single send. Confirm that
  reply-side `send_batch` is deferred (low value: replies are demand-driven one-per-recv), or in scope.
- **O-C (completion-path batching, O-1).** Confirm batch/GRO is readiness-path-only for M5 (completion
  backends use single-flight core recv), with native completion batching deferred.
- **O-D (byte-only queue, §6).** Confirm the byte-only queue stays out of scope for M5 and the three
  options are carried to the post-M5 `KlUdp` decision.
```
