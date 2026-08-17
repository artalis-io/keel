# KlUdp → KlDatagram consolidation — design freeze (inventory + decisions)

**Status:** design freeze. **Inventory and decisions only — no production code.** Each migration below
is a separately reviewable increment, gated by its own review before code. Positioning context:
[datagram_vs_udp.md](datagram_vs_udp.md). Reframed direction (R2): `KlUdp` is required today;
consolidation is a recognized future objective, and this document is its design.

## 0. Goal + the de-risking finding

Reduce the two public datagram APIs to one canonical Tier-1 transport (`KlDatagram`) plus an explicit
**extended-UDP layer**, migrate the two consumers (DNS, `KlUdpServer`), and reduce `KlUdp` to a
compatibility wrapper (optionally deprecated later) — **without** rebuilding a monolithic `KlUdp`
inside the `KlDatagram` facade.

**Key finding that de-risks this:** the internal provider seam **already does most of the work.** The
`KlDatagramOps` vtable (`include/keel/socket_dgram.h`) already declares `send`/`recv`/`send_gso`/
`configure`/`set_tos`/`mcast_membership`/`rx_batch_new`/`recv_batch`/`send_batch`, and the POSIX
provider (`socket_dgram_posix.c`) **implements all of them** (Linux offload gated). Source-pin and
per-packet TOS are already carried end-to-end (`KlDatagramMessage.local` / `.tos`, `KlDgramTxDesc`,
caps `KL_DGRAM_CAP_SOURCE_PIN` / `KL_DGRAM_CAP_TOS`). So the work is mostly **facade/core-level** —
wiring existing provider capability up, adding a **byte-gate `BOTH` queue policy** (§4; the core stays
allocation-free and count-bounded, and exact `KlUdp` byte-only queuing stays in the compat wrapper), a
**socket-preparation helper** (§2), and provider→caps derivation — **not new backend/provider work.**

The target layering (consumers never see backend internals — invariant I10):

```
DNS  /  KlUdpServer  /  future protocols (mDNS, CoAP, QUIC base)
                         │
        extended-UDP policy + optional capabilities
        (byte/both queue policy, multicast join/leave, batching/GSO/GRO opt-in)
                         │
        KlDatagram  — canonical Tier-1 message + lifetime transport
                         │
        KlDatagramOps providers  (POSIX / lwIP / EFI)
```

## 1. Feature inventory — every KlUdp feature × consumer need × provider support × KlDatagram status

Legend — **KlDatagram status:** `core` = already in the facade/message; `provider` = the
`KlDatagramOps` provider implements it but the facade does not expose it; `queue` = a queue-policy
concern; `—` = absent. **Consumers:** D = DNS resolver, S = `KlUdpServer`.

| KlUdp feature | DNS needs | UdpServer needs | Provider support (POSIX / lwIP / EFI) | KlDatagram status |
|---|---|---|---|---|
| unconnected `send_to(dest)` | **yes** | **yes** (reply) | ✓ / ✓ / — | **core** (`msg.peer`) |
| connected send | no | no | ✓ / ✓ / — | **core** (`KL_DGRAM_CAP_CONNECTED`) |
| per-datagram source (peer) on recv | **yes** (anti-spoof) | **yes** (reply dest) | ✓ / ✓ / — | **core** (recv `peer`) |
| local/dest addr capture (`recv_pktinfo`) | no | **yes** (multi-homed reply) | ✓ / ✗ / — | **core** (recv `local` + `KL_DGRAM_HAS_LOCAL`) |
| source-pinned send (`send_to_from`) | no | **yes** (reply pin) | ✓ / ✗ / — | **core** (`msg.local` + `KL_DGRAM_CAP_SOURCE_PIN`) |
| per-packet TOS on send | no | no | ✓ / ✗ / — | **core** (`msg.tos` + `KL_DGRAM_CAP_TOS`) |
| socket-default TOS (`tos`) | no | passthrough | ✓ / ✗ / — | **provider** (`set_tos`; fd-prep) |
| recv TOS delivery (`recv_tos`) | no | passthrough (unread) | ✓ / ✗ / — | **provider** (captured to slot, **not delivered** by `KlDatagramRecvFn`) |
| multicast join/leave + `IP_MULTICAST_*` | no | **yes** (public API) | ✓ / ✓(cond) / — | **provider** (`mcast_membership`; no facade API) |
| broadcast (`SO_BROADCAST`) | no | passthrough | ✓ / ? / — | **provider** (fd-prep) |
| `recvmmsg`/`sendmmsg` batching | no | passthrough (perf only) | ✓(Linux) / ✗ / — | **provider** (`recv_batch`/`send_batch`; facade single-flight) |
| GSO/GRO segmentation offload | no | passthrough (perf only) | ✓(Linux) / ✗ / — | **provider** (`send_gso`; GRO captured, not exposed) |
| byte-budget send queue (`max_send_queue`) | not needed (SLOT) | byte-bounded | n/a (queue policy) | core = `SLOT`/`BOTH` (bounded, §4); exact byte-only stays in the **KlUdp wrapper** |
| `reuse_addr/port`, `so_rcvbuf/sndbuf`, bind | yes (basic) | yes | fd-prep (`configure`) | **fd-prep** (caller/helper preps the fd before `kl_datagram_init`) |

**Reading it:** every feature DNS or `KlUdpServer` *load-bears* is already `core` or `provider`. The
genuinely-missing pieces are: (a) a **`BOTH` (byte-gate) queue policy** — note there is **no** pure
byte-only core policy (§4 proves it impossible under I8); exact `KlUdp` byte-only backpressure is kept
in the compat wrapper — (b) a **facade multicast join/leave API**, and (c) **provider→caps derivation**
so a consumer can discover what a provider actually supports. Batching/GSO/GRO/broadcast/recv-TOS are
passthrough perf/config knobs no consumer reads through its public API, so they can be provider-level
opt-ins without new facade surface.

## 2. DNS behavior-parity + failure-path requirements (the clean first migration)

DNS uses only `kl_udp_init` / `kl_udp_recv_start` / `kl_udp_send_to` / `kl_udp_free`, config
`{ctx, family, alloc}`, **zero extensions**. It requests no caps and can use the default recv model,
so it does **not** depend on the capability/extended layer (M2). Its only hard dependency is a
socket-preparation design (P2 below); the queue question resolves to the existing `SLOT` policy.
Parity requirements:

- **Datagram boundaries** preserved (one `send` = one query; one recv = one response). Core guarantees.
- **The unbounded list is the RECV side, not the send queue.** `dns_resolver.c`'s unbounded
  singly-linked `r->inflight` list (`src/dns_resolver.c:389`), two legs each, tracks queries
  **awaiting a response** — a recv/logical-state concern, matched by txn-id, **unchanged** by this
  migration. It is **not** the send queue: a leg's datagram occupies a send slot only *transiently*
  (readiness: only while the kernel briefly refuses the write; completion: only until its send op
  retires), then frees the slot. So "size `send_slots` to the leg count" was the wrong framing (the
  reviewer's P1). **Decision D-DNS-1 (corrected):** DNS uses the **existing `SLOT`** policy with
  `send_slots` sized for a realistic *transient send burst*; a pathological simultaneous-send burst
  overflows to `KL_DATAGRAM_WOULD_BLOCK`, which the resolver maps to its send-failure/retransmit path.
  This is a **documented, minor** difference from `KlUdp` (whose 256 KiB byte queue tolerates a larger
  burst before refusing) — acceptable, and arguably safer than an unbounded send queue. Because it
  uses the existing `SLOT` policy, **M3 depends on M0 only — not M1 or M2.**
- **Socket preparation + ownership (P2).** `kl_udp_init` today creates, configures, binds, (associates
  with the completion loop,) and **owns** the socket. `KlDatagramConfig` requires an **already
  prepared + bound** fd, and ownership transfers only on a successful `kl_datagram_init`. **Decision
  D-DNS-2:** M3 introduces a provider-neutral **preparation helper** (e.g. `kl_datagram_open(cfg)` or
  a documented sequence) that mirrors `kl_udp_init`'s prep exactly: `kl_sock_socket(sockets, family,
  SOCK_DGRAM)` → set cloexec + nonblocking → `provider->configure(...)` → `kl_sock_bind` (if a bind
  addr) → return the prepared fd; **on any step failure it closes the fd** (the caller reclaims
  nothing). `kl_datagram_init(fd)` then takes ownership; its own failure leaves the fd with the caller
  (per its contract), so the helper's caller closes it. This freezes the create/configure/bind/error-
  cleanup sequence the migration needs; the completion-loop association that `kl_udp_init` does is
  already handled inside `kl_datagram_init` (7B-7), so the helper stops at bind.
- **Source validation** — `dns_on_recv` rejects any datagram whose `peer` is not a configured
  nameserver. `KlDatagram` recv delivers `peer` — parity holds.
- **No local/TOS/UDP-truncation needs** — DNS uses the payload's TC bit, not the UDP `MSG_TRUNC`
  flag; it never reads `local` or recv-TOS. No caps required (`want_caps = 0`).
- **Per-query timeout/retransmit** are DNS-level timers, transport-agnostic — unchanged.
- **Transient send refusal (`WOULD_BLOCK`) needs NEW handling — it is NOT the existing failure path
  (P1).** Today `dns_transmit_leg` (`src/dns_resolver.c:460`) **consumes a try (`tries_left--`, :463)
  and builds the query into a local stack buffer *before* the send**, and any send failure returns
  `-1` → the caller marks the leg **done/failed** (:571) with no timer armed. So mapping `WOULD_BLOCK`
  to that path turns transient backpressure into a **resolution failure** — and since concurrent
  `resolve()` is unbounded, no fixed `send_slots` rules it out. (Under `KlUdp` today this is *masked*:
  its byte queue absorbs the transient block — `kl_udp_send_to` returns success and sends later — so
  DNS only sees a hard `-1` when the 256 KiB budget is exhausted, and then it fails the leg. The frozen
  behavior below **defers and retries** instead, so the parity target is **resolution success under
  send backpressure**, not byte-identical queue depth — a deliberate improvement.) **Decision D-DNS-3
  (transient backpressure, frozen):**
  - **Split `dns_transmit_leg` so a try is consumed and the response timer is armed ONLY on
    successful admission (`KL_DATAGRAM_ACCEPTED`).** It returns a 3-way result — `SENT` / `WOULD_BLOCK`
    / `FAILED`. Nothing is consumed on `WOULD_BLOCK`.
  - **On `WOULD_BLOCK`:** mark the leg `send_pending` (a new flag) and, on the **first** `WOULD_BLOCK`,
    arm the **send-admission guard timer** (frozen below). Do **not** decrement `tries_left`, do
    **not** arm the *response* timer, do **not** mark it done. The query is **rebuilt** on retry (it
    lives only in the stack buffer today — no retained payload storage needed), so a fresh txn-id /
    EDNS / cookie is re-derived at retry time.
  - **Register `kl_datagram_on_writable` once at resolver init.** On the full→non-full edge, scan the
    existing `r->inflight` list for `send_pending` legs and re-attempt each: on `SENT`, clear
    `send_pending` + **cancel the send-admission timer** + consume the try + arm the RESPONSE timer; on
    `WOULD_BLOCK` again, stop (queue re-filled — leave the rest pending, admission timer still running,
    for the next edge). No new unbounded structure — `send_pending` is a leg flag; the writable handler
    iterates `r->inflight` (already bounded by in-flight requests). The callback is
    non-destructive/reentrant-safe (no free/close inside it).
  - **Permanent send failure** (`FAILED` = `ERROR`/`TOO_LARGE`/`CLOSED`) keeps today's behavior:
    settle/advance the leg. (`TOO_LARGE` cannot occur for DNS if `send_slot_cap ≥ DNS_QUERY_MAX`.)
  - **Send-admission guard timer (FROZEN — bounds a leg that never gets admitted).** The resolver has
    **no overall request deadline** (only per-leg timers, armed *after* send, plus the RFC 8305
    resolution-delay timer), so a `send_pending` leg that never sees a writable edge (permanently full
    or dead socket) would hang. Frozen bound:
    - **Slot + callback:** reuse `leg->timer_id` (free while `send_pending` — mutually exclusive with
      the response timer) and the existing `dns_on_leg_timer`, which **branches on `send_pending`** at
      the top.
    - **Arm:** on the leg's **first** `WOULD_BLOCK` (the transition into `send_pending`), arm it for
      **`r->timeout_ms`** (the per-query timeout — no new config knob). **Preserved across writable
      retries** (armed once on entry, not re-armed per attempt): it bounds *total* time in
      `send_pending`.
    - **On expiry (fires while `send_pending`):** the leg was never transmitted → **settle/advance**
      it (`dns_leg_settle` → mark done → `dns_advance_candidate` if both legs done). **Expiry does NOT
      consume a network attempt** (`tries_left` unchanged — no packet was sent); retrying the same
      congested socket is futile, so it settles rather than retransmits.
    - **Cancellation:** the existing `dns_cancel_timers(r, q)` (`src/dns_resolver.c:402`) already
      cancels `leg->timer_id` when the request finalizes/cancels; the **request** unlinks from
      `r->inflight` only at finalization (not per leg — the request stays while its other leg is
      active). Safety for the writable scan comes from **clearing `send_pending` + marking the leg
      done** so the scan skips it, and from cancelling `leg->timer_id` on teardown so it cannot fire on
      a freed leg.
    - **Reentrancy:** the single-threaded loop serializes the writable-retry and the timer; once a leg
      is settled its `send_pending` is cleared and it is marked done, so a later writable scan skips it
      (the request may remain in `r->inflight` for its other leg). The expiry
      callback may finalize/free the request — reentrancy-safe exactly like today's `dns_on_leg_timer`.
- **TCP fallback** is already a **separate** `(fd, KlTls)` path (not the UDP socket) — untouched.

Failure-path tests to add with the DNS migration: **`WOULD_BLOCK` on a full send queue → leg goes
`send_pending`, no try consumed, response timer not armed, send-admission timer armed → `on_writable`
re-sends → resolution still succeeds** (the core backpressure regression); **no writable edge ever
arrives → the send-admission timer fires at `timeout_ms` → the leg settles/advances, no attempt
consumed, resolution completes (the other family/candidate) or fails cleanly — no hang** (the frozen
guard); cancel/free while `send_pending` (no use-after-free, no leaked timer); permanent send error →
NS rotation/settle; each prep-step failure closes the fd (no leak, no double-close); spoofed-source
drop; truncation → TCP fallback still triggers; dual-family concurrency; close/reopen.

## 3. Optional-capability seam (decisions)

`KlDatagram` already has the *negotiation* half: `KlDatagramConfig.want_caps` (consumer requires),
`kl_datagram_caps()` (reports), `KL_DATAGRAM_UNSUPPORTED` (send refused when a message uses an
un-granted cap). What is missing is **provider→caps derivation** and coverage of the extended features.

- **Decision D-CAP-1 (provider reports, facade derives).** Add a capability-report to the provider
  seam (either a new `KlDatagramOps.caps(ctx, fd, family)` or fold it into `configure`'s return) so
  `kl_datagram_caps()` reflects what the provider **actually supports**, not just what the caller
  declared. `want_caps` then becomes a *request* that fails init (not just send) if unsupported —
  fail-loud, no silent emulation. **Unsupported providers report absence; they never emulate
  incorrectly** (lwIP already ignores source-pin/TOS — that must surface as a missing cap, not a
  silent no-op).
- **Decision D-CAP-2 (extended features live above the core, not in it).** Keep the Tier-1 core
  message minimal (peer/local/tos/flags — already there). Batching, GSO/GRO, multicast join/leave,
  and broadcast go in the **extended-UDP layer** (a thin wrapper over `KlDatagram` that talks to the
  provider's already-existing `send_gso`/`recv_batch`/`send_batch`/`mcast_membership`). This avoids
  the monolith trap — the core facade does not grow a knob per UDP feature.
- **Decision D-CAP-3 (multicast is a runtime API).** Join/leave are dynamic (mDNS/SSDP), so expose
  them as runtime calls in the extended layer (e.g. `kl_dgram_ext_multicast_join/leave`) routed to
  the provider's `mcast_membership`, gated by a `KL_DGRAM_CAP_MULTICAST` capability.
- **Open question O-CAP-1 (recv-TOS delivery).** Recv TOS is captured to the inbound slot but not
  delivered by `KlDatagramRecvFn`. No current consumer reads it through a public API (`KlUdpServer`
  sets `recv_tos` but never surfaces it). Options: (a) leave it captured-not-delivered; (b) add a
  post-delivery accessor (like `kl_udp_get_recv_tos`); (c) extend the recv callback (ABI). Recommend
  (a)/(b) — do **not** grow the Tier-1 recv callback for an unread knob. Decide at implementation.

## 4. Send-queue policy — a proven impossibility, then an explicit fork

Today `KlDatagram` is **count-only**: a fixed `send_slots × send_slot_cap` array, atomic admission
(`WOULD_BLOCK` when no free slot, `TOO_LARGE` when `len > slot_cap`), single-flight pump, **all
preallocated** at init (no hot-path malloc). `KlUdp` is **byte-only, count-unbounded**: a
per-datagram-**malloc'd** `q_head`/`q_tail` linked list bounded by `q_bytes ≤ max_send_queue`
(`src/udp.c:87,116`); it allocates a node per queued datagram (legacy Tier-2; **not** I8).

**Impossibility (retracting the earlier BYTE arena).** An allocation-free (I8) queue has a **fixed
maximum descriptor count**, so it **cannot be count-unbounded**. UDP allows **zero-length** datagrams,
so a byte budget cannot bound the datagram count *at all* (unboundedly many zero-byte datagrams fit in
any budget). Therefore a "preallocated, byte-only, count-unbounded" queue is **impossible** — the
prior §4 "preallocated `BYTE` arena" was internally inconsistent (its descriptor pool is a second
count bound that a byte budget cannot size), and it is **retracted**. It also had no
fragmentation/wrap design; that problem is dissolved below by keeping per-slot contiguous storage.

Exact `KlUdp` semantics (byte-only + count-unbounded + zero-length) inherently require **hot-path
allocation** — which is precisely what `KlUdp` already does.

- **Decision D-Q-1 (the fork, resolved by LAYER — no impossible middle policy).**
  - **Tier-1 `KlDatagram` core stays allocation-free (I8), hence count-bounded.** Storage is the
    existing fixed `send_slots × send_slot_cap` array — **one datagram per contiguous slot**, so there
    is **no ring, no wrap, no fragmentation** (P1b does not arise; any admitted datagram is storable by
    construction, up to `slot_cap`). Two policies over that array:
    - **`SLOT`** (default, today): admit iff a free slot. Count-bounded.
    - **`BOTH`**: admit iff a free slot **and** `bytes_used + len ≤ byte_budget` — a byte **admission
      gate** on the slot array (`bytes_used` is a scalar; it never governs storage layout). Bounded in
      **both** dimensions.
    There is deliberately **no pure byte-only core policy** (per the impossibility).
  - **Exact `KlUdp` byte-only / count-unbounded = the `KlUdp` compatibility wrapper (M5) KEEPS
    `KlUdp`'s existing `malloc`-per-node queue, unchanged.** That layer already does hot-path
    allocation and is explicitly *not* the I8 core, so out-of-tree `KlUdp` users get byte-exact
    backpressure with **zero** behavior change — precisely because the wrapper does **not** re-express
    the queue over the core.
- **Decision D-Q-2 (consumers choose a layer; the difference is documented — the reviewer's fork).**
  This is the explicit choice among the three options:
  - **New protocols + migrated DNS / `KlUdpServer` use the core (`SLOT`/`BOTH`) and accept a documented
    *count* bound** (reviewer option 2). A pathological simultaneous-send burst refuses with
    `WOULD_BLOCK` sooner than `KlUdp`'s byte budget would; the caller retries/backpressures. The
    contract is explicitly count-and-byte-bounded — **no silent reinterpretation** of a byte budget as
    a slot count.
  - **`KlUdp`'s own exact byte-only/count-unbounded parity is preserved via hot-path allocation in the
    wrapper** (reviewer option 1) — kept in the compat layer, not the core.
  - We do **not** change the public UDP contract to reject zero-length/small datagrams (reviewer
    option 3 rejected).
- **Decision D-Q-3 (overflow rules).** `TOO_LARGE` iff `len > slot_cap`; `WOULD_BLOCK` on transient
  fullness (no free slot, or the `BOTH` byte gate). Each slot is a fixed `slot_cap`-byte contiguous
  buffer preallocated at init; storage is per-slot, so byte accounting is purely an admission gate.
- **Decision D-Q-4 (consumer mapping).** DNS (M3): `SLOT` (or `BOTH`) sized for a realistic transient
  send-backpressure burst (§2) — the unbounded *in-flight-awaiting-response* list is a recv/logical
  concern, **not** the send queue. `KlUdpServer` (M4): `BOTH`, byte budget from `max_send_queue` plus a
  reply-burst slot count; if exact byte-only/unbounded were ever required it stays on `KlUdp` (the
  off-ramp), rather than migrating to the count-bounded core.

## 5. KlUdpServer migration + compatibility-wrapper requirements

`KlUdpServer` public surface to preserve **exactly** (source + ABI): 8 functions
(`kl_udp_server_{init,reply,multicast_join,multicast_leave,free,local_port,fd,last_error}`), the
17-field `KlUdpServerConfig`, and the handler `KlUdpHandlerFn(KlUdpServer*, data, len,
const KlSockAddr *src, void*)`.

- **Load-bearing behaviors:** source-pinned reply (`kl_udp_send_to_from` → `msg.local` +
  `KL_DGRAM_CAP_SOURCE_PIN`, already in core) with auto-`recv_pktinfo` on wildcard bind; multicast
  join/leave (→ extended-layer `mcast_membership`). Everything else (batching/GRO/TOS/broadcast/byte
  budget) is **config passthrough** — the wrapper forwards it to fd-prep + queue policy + extended
  layer; none is read in handler/reply logic.
- **Migration shape:** re-implement `KlUdpServer` on `{KlDatagram core + extended-UDP layer}`,
  preserving the handler/reply/multicast semantics. The `reply` path uses `kl_datagram_send` with
  `msg.peer = src`, `msg.local = captured local` (source-pin), and the **`BOTH`** queue policy (byte
  budget from `max_send_queue` + a reply-burst slot count) — with the documented count-bound caveat
  (§4 D-Q-2). If exact byte-only/count-unbounded backpressure is required, `KlUdpServer` stays on the
  `KlUdp` wrapper instead (off-ramp) — do not silently convert its byte budget to a slot count.
- **Compatibility wrapper for `KlUdp` itself:** after the consumers migrate, express the public `KlUdp`
  API over the shared `{core + extended layer}` for send/recv/lifetime plumbing, **but keep `KlUdp`'s
  own byte-only, count-unbounded send queue (its `malloc`-per-node `q_head`/`q_tail`) unchanged** — that
  is the only way to preserve its exact byte-budget backpressure (the core cannot, per §4). So the
  wrapper is *not* a pure re-basing onto the core queue; its backpressure layer stays as-is. Out-of-tree
  `KlUdp` users are unaffected. Deprecation (if ever) is a later, explicitly-versioned step.
- **Note (separate cleanup):** the traced `KlUdpServer` handler uses `KlSockAddr *src`, not
  `struct sockaddr` — so the I10 note that lists `udp_server.c` as a `struct sockaddr` *address
  exception* is **stale** (that description fits `dns_resolver.c`, not `udp_server.c`). Worth a
  one-line I10 correction as its own trivial docs fix, independent of this consolidation.

## 6. Separately reviewable implementation increments

Each is its own increment with its own design-freeze-then-code review; **none is authorized by this
document** — it only scopes them.

0. **M0 — datagram socket-preparation helper (additive, no consumer change).** A provider-neutral
   create/configure/bind helper (§2 D-DNS-2) that mirrors `kl_udp_init`'s prep with per-step error
   cleanup + ownership handoff to `kl_datagram_init`. Tests: each step's failure closes the fd (no
   leak/double-close); success prepares a bound fd. Prereq for M3 and M4.
   **Status: implemented** — `kl_datagram_open()` in `src/datagram_open.c` (internal
   `src/datagram_open.h`); TIER1_INFRA-allowlisted infra (it creates/binds a platform socket, like
   `listener.c`/`connect_op.c`/`udp.c`), so the Tier-1 facade `datagram.c` stays platform-neutral. No
   public/ABI surface added; no consumer migrated. It fills a `KlDatagramPrep {fd, rx_caps, err}`: the
   prepared fd **plus** the `KL_DGRAM_RX_*` capture mask `configure()` actually enabled — that mask
   cannot be reconstructed after the fact, and M2 capability derivation + M4 source-pinned replies
   (pktinfo) need it. It rejects a provider with **no OR a partial** datagram vtable (missing the
   required `configure` op) **before** creating an fd. Tests in `tests/test_datagram_open.c` (13 cases):
   the per-step failure matrix over a fully-virtual mock provider (bad-arg / no-datagram-provider /
   partial-vtable / bad-bind / `socket()` / `set_nonblocking` / `bind`, each asserting exactly-one-or-zero
   close + the right `KlError`) + ownership handoff (success returns the unclosed fd; the caller closes) +
   capture-mask passthrough + a real default-provider bound/unbound loopback socket.
1. **M1 — `BOTH` queue policy (additive, no consumer change).** Add the `BOTH` policy per §4 — a byte
   **admission gate** on the existing fixed slot array (no ring, no byte arena; the impossible byte-only
   policy is not built). Default stays `SLOT`; preallocated; overflow-safe. Tests: `BOTH` admission by
   whichever dimension binds, retirement, no hot-path alloc (ASan/LSan), no change to `SLOT` consumers.
2. **M2 — capability derivation + extended-UDP layer (additive).** Provider→caps report (§3 D-CAP-1);
   a thin extended layer exposing multicast join/leave (D-CAP-3) and, if justified, batching/GSO/GRO
   opt-in over the existing provider ops. No consumer change yet.
3. **M3 — migrate DNS → KlDatagram (first production consumer).** Uses the M0 prep helper and the
   **existing `SLOT`** policy (its send queue is transient — §2 D-DNS-1); requests no caps.
   **Independent of M1 and M2.** Must implement the **transient-backpressure state machine (§2
   D-DNS-3)**: split `dns_transmit_leg` so a try/timer is consumed only on `KL_DATAGRAM_ACCEPTED`; a
   `send_pending` leg on `WOULD_BLOCK`; `kl_datagram_on_writable` re-sends; the **frozen send-admission
   guard timer** (`r->timeout_ms`, reusing `leg->timer_id`, settle-on-expiry, no attempt consumed) +
   cancellation/reentrancy rules. Behavior-parity + failure-path tests per §2 (the two load-bearing
   ones: `WOULD_BLOCK` → `on_writable` → success, and no-writable-edge → guard fires → no hang); TCP
   fallback untouched. Removes DNS's `KlUdp` dependency.
4. **M4 — migrate KlUdpServer → {core + extended layer}.** Uses M0 (prep) + M1 (`BOTH`) + M2 (multicast
   + passthrough caps). Preserve the public API/ABI + handler + multicast + source-pinned reply, with
   the documented count-bound caveat per §4/§5; or keep `KlUdpServer` on `KlUdp` if exact byte-only
   backpressure is required (off-ramp). Full parity + every backend gate.
5. **M5 — reduce KlUdp to a compatibility wrapper.** Express send/recv/lifetime over the shared
   substrate **but keep `KlUdp`'s own byte-only/count-unbounded queue** (§5) — the core cannot provide
   it; conformance across every backend. Optional deprecation is a later, versioned decision.

**Order/dependencies:** M0 is the prerequisite for M3/M4; M1 and M2 are additive. **M3 (DNS) needs M0
only (not M1 or M2)** — the low-risk proof. **M4 (server) needs M0 + M1 + M2.** M5 needs M3 + M4 complete
and green on every backend. Whether *full* consolidation is worthwhile is re-decided after M3 + M4 land —
if the extended layer or wrapper proves ugly, stopping after DNS (with `KlUdp` retained for the server)
remains a valid end state.

## 7. Non-goals of this freeze

- No production code, no ABI change, no consumer migration here.
- Do **not** grow the Tier-1 `KlDatagram` core into a second monolithic `KlUdp`; extended UDP lives
  in a distinct layer above it.
- No silent budget reinterpretation (byte↔slot) and no silent capability emulation (unsupported
  providers report absence).
- Consolidation is a *goal*, not a commitment to remove `KlUdp` — M5/deprecation stay optional and
  gated on the earlier increments proving clean.
