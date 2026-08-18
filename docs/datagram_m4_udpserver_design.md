# Datagram M4 — migrate KlUdpServer → {KlDatagram core + extended layer} — design freeze

Status: **PROPOSED (docs-only, revision 2)** — no code until reviewed and accepted, per the
consolidation workflow. Sibling of the accepted M0 / synchronous-teardown / M1 / M2 freezes. Authority
is `docs/datagram_consolidation_design.md` §5 (KlUdpServer migration + compat-wrapper requirements) and
roadmap item 4; this freeze turns them into an implementable increment. **Depends on M0 (fd prep), M1
(`BOTH` policy), M2 (source-pin cap gate + multicast).**

**Revision 2** applies the review rulings and fixes three findings. (P1) Wildcard source-pin now
requires BOTH the send cap (`KL_DGRAM_CAP_SOURCE_PIN`) AND the accepted RX capture
(`prep.rx_caps & KL_DGRAM_RX_PKTINFO`) — §2/§4, with a frozen error + fd-cleanup path. (P1) Teardown is
frozen to ONE exact lifecycle with explicit reentrant-free behavior — §9, plus free-from-handler /
free-twice regressions. (P2) Queue sizing uses normalized, overflow-safe arithmetic off an
`effective_budget` default, with corrected example math and small-budget qualification — §3. Rulings
accepted: the `KlUdpServer` layout ABI revision; GRO-off + mmsg-inert; fail-loud wildcard source-pin
*with both caps verified*; full-payload slot sizing with accurately-stated defaults + tradeoff.

## 0. Scope

Re-implement `KlUdpServer` on top of the Tier-1 `KlDatagram` core (M0 prep + M1 `BOTH` policy) and the
M2 extended layer (caps + multicast), replacing its embedded `KlUdp`. The public **surface is preserved
exactly (source)**: the 8 functions (`kl_udp_server_{init,reply,multicast_join,multicast_leave,free,
local_port,fd,last_error}`), the 17-field `KlUdpServerConfig`, and the handler
`KlUdpHandlerFn(KlUdpServer*, data, len, const KlSockAddr *src, void*)`. `KlUdp` itself is **untouched**
by M4 (its reduction to a compat wrapper is M5).

**In scope:** the config→{KlUdpConfig + KlDatagramConfig} mapping; the send-queue sizing onto fixed
slots; source-pinned reply via `msg.local`; recv + local-address capture + handler dispatch; multicast
join-at-init + runtime join/leave via M2; deterministic error mapping; teardown; the documented
behavior caveats (count-bound queue, no mmsg/GRO coalescing).

**Not in scope:** M5 (`KlUdp` compat wrapper); any new public `KlUdpServer` API.

## 1. ABI: surface preserved; the KlUdpServer struct is a pre-consumer layout revision (D-M4-1)

`KlUdpServer` is a **caller-owned, embedded** struct (`KlUdpServer ns;` on the stack) whose current
first field is `KlUdp udp`. M4 replaces that with a `KlDatagram` plus the small amount of
extended/dispatch state, so `sizeof(KlUdpServer)` and its layout change. This is an **intentional
pre-consumer struct-layout ABI revision** — the same project-stage judgment accepted for the M2 vtable:
there are no external consumers requiring binary compatibility, and all in-tree callers (tests,
examples) recompile. The **8 function signatures, the 17-field `KlUdpServerConfig`, and the handler
typedef are byte-for-byte unchanged** (source + ABI). Proposed struct:

```c
struct KlUdpServer {
    KlDatagram      dg;              /* was: KlUdp udp — the migrated transport */
    KlUdpHandlerFn  handler;
    void           *user_data;
    KlError         last_error;
    KlSockAddr      local;           /* current datagram's local (dest) addr (source-pin reply) */
    int             have_local;      /* 0 = unknown → reply uses the socket default */
};
```

## 2. Config mapping (D-M4-2)

`kl_udp_server_init` maps the 17-field `KlUdpServerConfig` onto two existing config types — no new
public config. Step 1 prepares the fd provider-neutrally (M0 `kl_datagram_open` over a `KlUdpConfig`),
which returns `KlDatagramPrep{fd, rx_caps, err}`; step 1a verifies the RX capture on a wildcard bind
(§4); step 2 adopts the fd into a `KlDatagram` (`kl_datagram_init_ex`). Ordering matters: the RX-caps
check runs on `prep` **before** `kl_datagram_init_ex` adopts the fd, so a failure closes the caller-held
prep fd and adopts nothing.

**KlUdpConfig (→ `kl_datagram_open` → provider `configure`)** — the socket-option knobs:
`family` (from `bind_addr`), `bind_addr`, `bind_port = port`, `recv_buf_size`, `reuse_addr = 1`
(servers rebind), `reuse_port`, `recv_pktinfo = wildcard(bind_addr)` (§4), `so_rcvbuf`, `so_sndbuf`,
`tos` (default outgoing TOS via setsockopt), `recv_tos`, `broadcast`, `multicast_ttl`,
`multicast_disable_loop`, `multicast_iface`, `alloc`. **Forced OFF: `recv_gro` and `mmsg_batch`** (§7 —
the single-flight core cannot split GRO-coalesced datagrams nor drive recvmmsg). `multicast_group` is
NOT a configure knob here — it is joined post-init (§6).

**KlDatagramConfig (→ `kl_datagram_init_ex`)** — the transport/queue knobs: `fd` (from prep),
`send_slots` + `send_slot_cap` + `send_byte_budget` (§3), `recv_cap = recv_buf_size` (default 2048),
`want_caps = wildcard(bind_addr) ? KL_DGRAM_CAP_SOURCE_PIN : 0` (§4), `sockets = ctx->sockets`,
`ctx`, `alloc`.

## 3. Send-queue sizing — mapping a byte-unbounded queue onto fixed I8 slots (Decision D-M4-3, the crux)

`KlUdp`'s reply queue is byte-bounded (`max_send_queue`) and **count-unbounded** (malloc-per-reply). The
I8 core is `send_slots × send_slot_cap`, preallocated. M1's `BOTH` policy bounds **both** dimensions.
The mapping (per §4 D-Q-2 / D-Q-4, with the documented count-bound caveat):

Normalized, overflow-safe arithmetic (blocker P2), with `KL_UDP_DGRAM_MAX = 65507`:
- `effective_budget = cfg->max_send_queue ? cfg->max_send_queue : (256u * 1024u)` — normalize the
  `0 ⇒ 256 KiB` default FIRST, so a zero budget is never passed to `init_ex` (which would set
  `send_byte_budget = 0` and disable `BOTH`, silently reverting to `SLOT`).
- `send_byte_budget = effective_budget` — the M1 byte gate (no inflation).
- `send_slot_cap = KL_UDP_DGRAM_MAX (65507)` — a full UDP payload, so **any valid reply is queueable**
  (preserving `KlUdp`'s "can queue any reply" semantics; no per-reply size refusal).
- `send_slots = effective_budget / KL_UDP_DGRAM_MAX; if (send_slots == 0) send_slots = 1;` — `size_t`
  division (no overflow); clamped to ≥ 1. The default 256 KiB ⇒ `262144 / 65507 = 4` slots.

**Memory / count tradeoff, stated exactly.** For `effective_budget ≥ 65507`, preallocation
(`send_slots × 65507`) ≈ `effective_budget` (the memory bound the user already chose via
`max_send_queue`); e.g. 256 KiB ⇒ 4 × 65507 ≈ 256 KiB. For `effective_budget < 65507`, `send_slots`
clamps to 1, so **one 65507-byte slot is allocated even though the budget is smaller** — preallocation
is `65507`, above the byte budget; the byte gate still caps *queued bytes* at `effective_budget` (so a
sub-datagram budget admits exactly one queued reply up to its size). Preallocation is therefore
`max(65507, effective_budget)`, not `≈ budget`, for small budgets.

**Documented caveat (accepted §4 D-Q-2):** because slots are fixed and full-datagram-sized, a burst of
many *small* replies is count-bounded sooner than `KlUdp`'s byte-only bound — the (N+1)-th concurrent
queued reply gets `WOULD_BLOCK` even with byte budget to spare. `kl_udp_server_reply` maps that to `-1`
/ `KL_ERR_QUEUE_FULL` (§8) — the same lossy-drop surface `KlUdp` presents on overflow, so the
`KlUdpServer` contract (reply sent if capacity, else dropped with `-1`) is preserved. An application
that genuinely needs deep small-reply queuing stays on `KlUdp` (the off-ramp, §D-Q-4).

*(Alternative considered — O-M4-sizing, §12: a nominal `send_slot_cap` (e.g. `recv_buf_size`) with a
deeper `send_slots`, giving deeper small-reply queuing but refusing to QUEUE replies larger than the
nominal cap — case-(a) direct-send-or-refuse. Rejected as the default because it changes reply
queueability by size; kept as the reviewer's alternative.)*

## 4. Source-pinned reply + recv-pktinfo (Decision D-M4-4)

Multi-homed correctness: a reply must leave from the **local (destination) address the request arrived
on**. `KlUdp` enables `recv_pktinfo` only on a wildcard bind (a specific-address bind already fixes the
source), captures the local addr per datagram, and sends the reply source-pinned via `msg.local`.

Source-pin correctness needs **two** capabilities, verified independently (blocker P1): the send side
must accept `msg.local` (`KL_DGRAM_CAP_SOURCE_PIN`), AND the receive side must actually be capturing the
local (dest) address (`recv_pktinfo` accepted by the kernel). `want_caps` proves only the former;
`recv_pktinfo` is best-effort in `configure`, so M0 returns the accepted set in `KlDatagramPrep.rx_caps`
— which M4 must check. Otherwise the server initializes, receives no local address (`have_local` always
0), and silently replies via the default route — exactly the failure fail-loud is meant to prevent.

M4 makes source-pin **fail-loud where it is load-bearing** (M2), requiring both caps:
- **Wildcard bind:** request `recv_pktinfo = 1` in the `KlUdpConfig`, then after `kl_datagram_open`
  **require `prep.rx_caps & KL_DGRAM_RX_PKTINFO`** (the kernel accepted RX pktinfo). If absent →
  `kl_sock_close(sockets, prep.fd)` (M0 transferred the fd to the caller on open success) and return
  `-1` with `last_error = KL_ERR_UNSUPPORTED`, adopting nothing. Then `kl_datagram_init_ex` with
  `want_caps = KL_DGRAM_CAP_SOURCE_PIN`, whose M2 gate requires the send cap (init `-1` /
  `KL_ERR_UNSUPPORTED`, fd not adopted, on a provider lacking it). Both must hold; on POSIX/Winsock both
  are always available, so init succeeds and behavior matches `KlUdp`. Reply passes `msg.local =
  &s->local` when `have_local`.
- **Specific-address bind:** `recv_pktinfo = 0`, no RX-caps check, `want_caps = 0`, reply
  `msg.local = NULL` — the socket's bound address fixes the source; no source-pin needed.

*(O-M4-srcpin, §12: the alternative is opportunistic source-pin — `want_caps = 0` always, reply
consults `kl_datagram_provider_caps() & SOURCE_PIN` and passes `msg.local` only if supported — exactly
matching `KlUdp`'s graceful degrade. Recommended above is the fail-loud-on-wildcard variant, a no-op on
real platforms; the reviewer may prefer the opportunistic one to preserve `KlUdp` byte-for-byte on all
providers.)*

## 5. Recv path + local capture + dispatch (D-M4-5)

`kl_datagram_recv_start` with a `KlDatagramRecvFn` adapter (mirrors today's `udp_server_on_recv`):
```
static void us_on_recv(void *ud, const void *data, size_t len,
                       const KlSockAddr *peer, const KlSockAddr *local, unsigned flags) {
    KlUdpServer *s = ud;
    if (local && (flags & KL_DGRAM_HAS_LOCAL)) { s->local = *local; s->have_local = 1; }
    else                                       { s->have_local = 0; }
    s->handler(s, data, len, peer, s->user_data);   /* src = peer */
}
```
The handler calls `kl_udp_server_reply(s, data, len, dest)` → `kl_datagram_send(&s->dg, &(KlDatagramMessage){
.data, .len, .peer = dest, .local = s->have_local ? &s->local : NULL, .tos = -1 })`. `tos = -1`: the
reply uses the socket-default TOS (set at configure from `cfg->tos`), exactly as `KlUdp`'s reply does
(no per-packet TOS → no `KL_DGRAM_CAP_TOS` requirement).

## 6. Multicast (D-M4-6)

- **Join-at-init:** if `cfg->multicast_group != NULL`, after a successful `kl_datagram_init_ex` call
  `kl_datagram_multicast_join(&s->dg, cfg->multicast_group, cfg->multicast_iface)`; on failure, tear the
  datagram down and fail init (`last_error` from the join — §8), matching `KlUdp`'s init-time join.
- **Runtime:** `kl_udp_server_multicast_join/leave` → `kl_datagram_multicast_join/leave` (M2), which
  gate on `KL_DGRAM_CAP_MULTICAST` and route to the provider's `mcast_membership`. `last_error` is set
  from the M2 call's error (§8).

## 7. Batching / GRO / GSO / recv-TOS on the single-flight core (D-M4-7)

The Tier-1 core is single-flight (one recv posted, one send in flight), so:
- **`recv_gro` forced OFF.** UDP_GRO makes a plain recv return a *coalesced* super-buffer; the
  single-flight core would mis-deliver it as one datagram. Forcing it off keeps per-datagram delivery
  **correct** (not merely slower). A migrated server loses GRO receive-coalescing.
- **`mmsg_batch` inert.** The core never calls `recv_batch`/`send_batch`; the knob is accepted
  (config-compatible) but unused. No recvmmsg/sendmmsg coalescing.
- **GSO** unused (reply is one datagram; the core send posts one datagram).
- **`recv_tos`** capture may be enabled at configure, but per O-CAP-1 the recv callback does not deliver
  TOS — captured-not-delivered (no consumer reads it through `KlUdpServer`).

These are the M2 §6 no-coalescing caveat made concrete for M4: `KlUdpServer` on the core trades
`KlUdp`'s Linux `recvmmsg`/`sendmmsg`/GRO throughput for the uniform single-flight model. If a workload
needs that throughput, it stays on `KlUdp` (off-ramp). Recorded, not silently dropped.

## 8. Error mapping (D-M4-8)

`kl_udp_server_*` keep their `int 0/-1` + `KlError last_error` contract. Mapping from the core:
- **reply:** `KL_DATAGRAM_ACCEPTED → 0`; `WOULD_BLOCK` / `TOO_LARGE` (budget or slot_cap) → `-1`,
  `last_error = KL_ERR_QUEUE_FULL` (the drop `KlUdp` already reports); `CLOSED` → `-1`,
  `KL_ERR_INVALID_ARG`; `ERROR`/`UNSUPPORTED` → `-1`, `KL_ERR_IO` / `KL_ERR_UNSUPPORTED`.
- **init:** any prep/init/join failure → `-1` with `last_error` carried from the failing step
  (`kl_datagram_last_error` / the multicast error). fd never leaked (M0/M2 contracts).
- **multicast join/leave:** pass through the M2 error precedence (`KL_ERR_UNSUPPORTED` /
  `KL_ERR_INVALID_ARG` / `KL_ERR_IO`).

## 9. Teardown — one exact lifecycle (Decision D-M4-9, blocker P1)

`KlUdpServer` is caller-owned/embedded; its struct memory is never freed by the library. `kl_udp_server_free`
reclaims only the transport (the embedded `KlDatagram`: its heap core + the fd) via **`kl_datagram_teardown(&s->dg,
NULL, NULL)`** (Option A; `reclaim = NULL` because the owner memory is the caller's). No user callback is
invoked (it is a `void` free). Exactly-once fd close via the datagram close machine. `kl_datagram_teardown`
gives exactly two behaviors by whether a delivery frame is active:

- **Outside any handler dispatch (the normal case, `busy == 0`):** teardown is **fully SYNCHRONOUS** — the
  core is freed, the fd closed, and `s->dg` memset **before `kl_udp_server_free` returns**. The caller may
  immediately release or reuse the `KlUdpServer` memory. This preserves `KlUdp`'s synchronous caller-owned
  contract exactly.
- **From within a handler (reentrant, `busy > 0`):** the destructive reclamation **defers to the end of the
  current recv dispatch** (Option A's frame-leave rule — no internal UAF; the datagram coordinator reclaims at
  the outermost leave, after the handler and the recv adapter have returned and no longer touch `s`). The one
  explicit contract redefinition: **the caller MUST keep the `KlUdpServer` memory valid until the current
  `kl_event_ctx_run` tick returns** (it must not release it inside the handler), because the deferred reclamation
  memsets the embedded `s->dg` at frame-leave. `s->handler`/`user_data`/`local` are untouched by the memset.

- **Idempotent:** after teardown `s->dg.core == NULL`, so a second `kl_udp_server_free` is a no-op success
  (`kl_datagram_free`/`teardown` idempotency, frozen in the framework increment). A `free` following a
  deferred (from-handler) free is likewise a no-op once the deferral has run.

`us_on_recv` never touches `s` after calling `s->handler`, so a from-handler free that defers leaves no
use-after-teardown inside the wrapper; the datagram frame handles internal ordering.

## 10. Test matrix (`tests/test_udp_server.c` + the existing udp-server suite)

Preserve the existing suite (behavior parity) and add M4-specifics:
1. **ABI/surface** — the 8 functions, 17-field config, and handler signature compile + link unchanged;
   existing udp_server tests pass verbatim (loopback bind + echo reply).
2. **Source-pinned reply (wildcard)** — bind `0.0.0.0`, a datagram to a specific local addr; the reply
   is source-pinned to that local (assert via a second bound address / `recv` local on the peer where
   the platform allows); both `prep.rx_caps & KL_DGRAM_RX_PKTINFO` and `want_caps = SOURCE_PIN` accepted
   on POSIX.
3. **Missing RX pktinfo fails init (blocker P1)** — a mock provider whose `configure` returns
   `rx_caps` WITHOUT `KL_DGRAM_RX_PKTINFO` on a wildcard bind → `kl_udp_server_init` returns `-1` /
   `KL_ERR_UNSUPPORTED`, and the prep fd is closed (no adoption, no leak under ASan/LSan). A companion
   mock lacking the send `SOURCE_PIN` cap fails the `init_ex` gate likewise.
4. **Specific-address bind** — no `recv_pktinfo`, no RX-caps check, `want_caps = 0`, reply from the
   bound address.
5. **Reply backpressure** — fill the send queue (gating provider / no drain), the next reply returns
   `-1` / `KL_ERR_QUEUE_FULL`; drains and succeeds after progress (count-bound caveat exercised).
6. **Multicast** — join-at-init (`multicast_group` set) succeeds on a multicast-capable provider;
   runtime join/leave route through M2; a malformed group / unsupported provider yields the M2 errors.
7. **Teardown lifecycle (blocker P1)** — (a) `free` outside a handler is synchronous: fd closed exactly
   once, immediate reuse works, no leak (ASan/LSan); (b) **free-twice** — a second `free` is a no-op
   success; (c) **free-from-handler** — a handler calls `kl_udp_server_free`, the tick completes, the
   deferred teardown reclaims cleanly with the `KlUdpServer` kept valid through the tick — no UAF/leak.
   Run (b) and (c) on **both readiness (default) and completion (pollcomp/iouring)** backends.
8. **`recv_gro` forced off** — a server configured with `recv_gro = 1` still delivers one datagram per
   recv (no coalesced mis-delivery).
9. **Shared-loop coexistence** — a `KlUdpServer` and a TCP `KlServer` on one `KlEventCtx` (the headline
   use case) both serve.

## 11. Validation plan

- macOS default (kqueue readiness) `make test` — full suite incl. the udp_server suite, ASan/UBSan.
- `BACKEND=pollcomp` — the completion-model reply path (in-flight slot accounting).
- Linux container `BACKEND=iouring` under ASan/UBSan/**LSan** — reply/recv/close lifetime, no leak;
  and a `SO_REUSEPORT` fan-out smoke if feasible.
- Gates: `check-tier1-boundary` (KlUdpServer routes only through the datagram facade + M2 API — no new
  platform include), `check-sockaddr-neutral`, `check-doc-refs`, `cppcheck`; MinGW winsock compile;
  freestanding unaffected (KlUdpServer is not a freestanding component). No `KlUdp` change → `KlUdp`
  suites unaffected.

## 12. Decisions — resolved at review (revision 2)

- **O-M4-sizing (§3) — RESOLVED: full-payload slots with normalized defaults.** `send_slot_cap = 65507`,
  `effective_budget = max_send_queue ?: 256 KiB`, `send_byte_budget = effective_budget`, `send_slots =
  max(1, effective_budget / 65507)`. Memory/count tradeoff stated exactly (§3): preallocation ≈ budget
  for budgets ≥ 65507; one 65507-byte slot for smaller budgets. Accepted as defensible with accurate
  defaults/tradeoff (ruling).
- **O-M4-srcpin (§4) — RESOLVED: fail-loud wildcard source-pin, BOTH caps verified.** On a wildcard
  bind require `prep.rx_caps & KL_DGRAM_RX_PKTINFO` (receive capture accepted) AND `want_caps =
  KL_DGRAM_CAP_SOURCE_PIN` (send side); either missing → init `-1`/`KL_ERR_UNSUPPORTED`, prep fd closed
  (ruling + blocker P1).
- **O-M4-struct (§1) — RESOLVED: accepted pre-consumer layout ABI revision** (`KlUdp udp` →
  `KlDatagram dg`); no external consumers requiring binary compatibility (ruling).
- **O-M4-gro (§7) — RESOLVED: force `recv_gro` off, leave `mmsg` inert** for correctness on the
  single-flight core; documented M2 §6 caveat (ruling).
- **O-M4-free (§9) — RESOLVED: one lifecycle** — synchronous outside a handler; deferred-to-tick-end
  from within a handler (caller keeps `KlUdpServer` valid until the tick returns); idempotent. Regressions
  in §10.7 (blocker P1).

No open decisions remain; the freeze is ready for implementation review.
